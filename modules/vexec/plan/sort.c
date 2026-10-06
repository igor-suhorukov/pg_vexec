/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * sort.c
 *	  VecSort in the planners (pg_vector_executor.md §3.3.3, §3.3.4, §3.8,
 *	  V4): its path at UPPERREL_ORDERED in PostgreSQL's planner, its plan,
 *	  and ORCA's Sort, with the bound of a Limit above it.
 *
 * PostgreSQL's planner.  create_upper_paths_hook at UPPERREL_ORDERED is
 * called once core has made the ordered relation's paths
 * (create_ordered_paths()).  The hook adds a VecSort over the input
 * relation's cheapest path, with the query's sort keys, where that path is
 * not in their order already, to the query's final target, which core's
 * ordered paths carry.  Its one custom path is the row Sort
 * create_sort_path() makes of the same input, so that core plans the sort's
 * keys as it plans a Sort's -- prepare_sort_from_pathkeys() adds a key its
 * input lacks to the input's target list, which is static in createplan.c
 * -- and the builder takes the Sort's keys and its input from the plan core
 * made, and sets the Sort aside.  The VecSort projects: its target is the
 * ordered relation's, the query's final one, which it evaluates over the
 * sorted rows as a projection over a Sort would, through its target list.
 * Set-returning functions are added above it after the hook
 * (adjust_paths_for_srfs()), and a Limit above them.
 *
 * Its bound (§3.6, "Bounds").  create_ordered_paths() is given the LIMIT's
 * count and offset as the sort's limit (grouping_planner()), and passes
 * none where set-returning functions follow the sort; the hook is not
 * given it.  So the hook takes it from the query as preprocess_limit()
 * does (planner.c): a LIMIT whose count -- and offset, if any -- are
 * constants once the planner has folded them, the count and offset added;
 * none for FETCH ... WITH TIES, which reads past the bound's rows, and none
 * where the query has set-returning functions in its target list, which
 * may make any number of rows of each sorted one.  The Limit above then
 * reads at most the bound's rows of the sorted ones, which a bounded sort
 * keeps, as PostgreSQL's Sort keeps them when ExecSetTupleBound() passes it
 * the same bound at run time.
 *
 * In auto mode the VecSort competes with core's sorted paths by cost
 * (cost.c); in force mode it replaces them, where the oracle accepts it.
 *
 * Late columns (H6, §3.14).  A bounded VecSort -- of at most
 * VEXEC_LATE_ROWS rows -- straight over a VecScan sorts the keys and each
 * row's TID alone, where the child's other columns are the relation's
 * own: the scan then reads only the sort keys and its quals' columns, and
 * the other columns of the rows the sort keeps are fetched at the end, by
 * their TIDs, under the scan's snapshot (table_tuple_fetch_row_version()).
 * A Var raises no error, so reading one only for the kept rows changes no
 * answer; a column the child computes keeps it as it is.  The rows that
 * tie at the bound's key are those tuplesort keeps, as without it.
 *
 * In parallel, the input's cheapest partial path is sorted so too, in each
 * participant, under a Gather Merge, the bound each participant's.
 *
 * ORCA.  A Sort ORCA's translator built becomes a VecSort of the same keys
 * over the same child, its target list read over the scan tuple, the
 * child's row.  A Limit of a constant count over it -- or over a projection
 * of it, which passes rows as they come -- gives it its bound, as
 * ExecSetTupleBound() would give a Sort, when the Limit is offered to the
 * front end after its children.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "utils/lsyscache.h"

#include "vexec.h"
#include "exec/exec.h"
#include "plan/plan.h"

static Plan *plan_vecsort(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
						  List *tlist, List *clauses, List *custom_plans);

/* A bounded sort keeps its columns late up to this many rows (H6). */
#define VEXEC_LATE_ROWS		VEXEC_BATCH_ROWS

/* A running bound keeps the first key of up to this many rows (H6). */
#define VEXEC_BOUND_ROWS	1000000

static const CustomPathMethods vecsort_path_methods = {
	.CustomName = VEXEC_SORT_NAME,
	.PlanCustomPath = plan_vecsort,
};

/* "1 sort key", "2 sort keys" */
static const char *
count_of(int n, const char *one, const char *many)
{
	return psprintf("%d %s", n, n == 1 ? one : many);
}

/*
 * The LIMIT's rows the sort must keep: its count and offset, where both are
 * constants once folded, and nothing after the sort makes or drops rows.
 * 0: no bound.
 */
static int64
query_bound(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Const	   *count = (Const *) parse->limitCount;
	int64		offset = 0;
	int64		n;

	if (count == NULL || !IsA(count, Const) || count->constisnull ||
		parse->limitOption != LIMIT_OPTION_COUNT || parse->hasTargetSRFs)
		return 0;
	n = DatumGetInt64(count->constvalue);
	if (n <= 0)
		return 0;
	if (parse->limitOffset != NULL)
	{
		Const	   *off = (Const *) parse->limitOffset;

		if (!IsA(off, Const))
			return 0;
		if (!off->constisnull)
			offset = Max(DatumGetInt64(off->constvalue), 0);
	}
	if (n > PG_INT32_MAX - offset)
		return 0;
	return n + offset;
}

/*
 * The final target as the VecSort evaluates it: what the input's target
 * holds whole -- an aggregate, a window function's result, a grouping
 * expression -- is a column of the scan tuple, as set_plan_references()
 * matches it (setrefs.c, fix_upper_expr()), and only what is computed over
 * those columns is the node's to evaluate.  For the oracle.
 */
static Node *
over_columns_mutator(Node *node, List *input_exprs)
{
	if (node == NULL)
		return NULL;
	if (list_member(input_exprs, node))
		return (Node *) makeVar(INDEX_VAR, 1, exprType(node), exprTypmod(node),
								exprCollation(node), 0);
	return expression_tree_mutator(node, over_columns_mutator, input_exprs);
}

/*
 * Whether a path of the ordered relation is an index scan's order: one whose
 * index orders it by a distance (indexorderbys) -- a nearest-neighbour
 * search's HNSW, IVFFlat or GiST index -- under projections or not.
 */
static bool
index_ordered_path(Path *path)
{
	while (IsA(path, ProjectionPath))
		path = ((ProjectionPath *) path)->subpath;
	return IsA(path, IndexPath) && ((IndexPath *) path)->indexorderbys != NIL;
}

/* A VecSort path: the row Sort of the same input its one custom path. */
static Path *
vecsort_path(RelOptInfo *rel, PathTarget *target, SortPath *rowpath, const VexecCost *cost,
			 int64 bound)
{
	CustomPath *cp = makeNode(CustomPath);
	Path	   *input = rowpath->subpath;

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = target;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = rel->consider_parallel && input->parallel_safe;
	cp->path.parallel_workers = input->parallel_workers;
	cp->path.rows = rowpath->path.rows;
	cp->path.disabled_nodes = rowpath->path.disabled_nodes;
	cp->path.startup_cost = cost->startup;
	cp->path.total_cost = cost->total;
	cp->path.pathkeys = rowpath->path.pathkeys;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = list_make1(rowpath);
	cp->custom_private = list_make1(makeInteger((int) bound));
	cp->methods = &vecsort_path_methods;
	return &cp->path;
}

/*
 * VecSort's alternative for the query's ORDER BY (§3.3.3, UPPERREL_ORDERED):
 * recorded for EXPLAIN, and in auto and force mode added.  In parallel, the
 * input's cheapest partial path is sorted in each participant too, under a
 * Gather Merge, as create_ordered_paths() sorts it: each participant's
 * sort bounded as the whole one is, the LIMIT's rows being among each
 * one's first.
 */
void
vexec_consider_sort(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
					VexecPlanState *ps, const char *target_name)
{
	VexecAlt   *alt = NULL;
	Path	   *input = input_rel->cheapest_total_path;
	bool		add = ps->mode == VEXEC_MODE_AUTO || ps->mode == VEXEC_MODE_FORCE;
	SortPath   *rowpath;
	List	   *newpaths;
	VexecSteps	steps;
	VexecCost	cost;
	PathTarget *target;
	int64		bound;
	bool		index_kept = false;
	ListCell   *lc;

	if (root->sort_pathkeys == NIL || output_rel->pathlist == NIL)
		return;

	/*
	 * The query's final target, which the ordered relation's paths make:
	 * create_ordered_paths() gives it to them and leaves the relation's own
	 * reltarget empty (planner.c).
	 */
	target = ((Path *) linitial(output_rel->pathlist))->pathtarget;
	if (ps->record)
		alt = vexec_alt_record(ps, "VecSort", target_name, root, NULL);
	if (alt == NULL && !add)
		return;

	if (!vexec_enable_sort)
	{
		vexec_alt_refuse(ps, alt, "vexec.enable_sort is off");
		return;
	}
	if (input == NULL || PATH_REQ_OUTER(input) != NULL)
	{
		vexec_alt_refuse(ps, alt, "a parameterized input");
		return;
	}
	if (pathkeys_contained_in(root->sort_pathkeys, input->pathkeys))
	{
		vexec_alt_refuse(ps, alt, "its input is in the order already");
		return;
	}

	if (ps->mode != VEXEC_MODE_FORCE && input->rows < vexec_min_rows)
	{
		vexec_alt_refuse(ps, alt, psprintf("%.0f input rows, fewer than vexec.min_rows", input->rows));
		return;
	}
	memset(&steps, 0, sizeof(steps));
	vexec_oracle_exprs(root, (List *) over_columns_mutator((Node *) target->exprs,
														   input->pathtarget->exprs),
					   &steps);
	if (steps.refusal)
	{
		vexec_alt_refuse(ps, alt, steps.refusal);
		return;
	}

	bound = query_bound(root);
	rowpath = create_sort_path(root, output_rel, input, root->sort_pathkeys,
							   bound > 0 ? (double) bound : -1.0);
	vexec_cost_sort(root, &rowpath->path, input, vexec_is_vector_path(input), &cost);
	vexec_alt_costed(ps, alt, &cost,
					 psprintf("%s%s", count_of(list_length(root->sort_pathkeys), "sort key", "sort keys"),
							  bound > 0 ? psprintf(", bounded at " INT64_FORMAT, bound) : ""));
	if (!add)
		return;
	newpaths = list_make1(vecsort_path(output_rel, target, rowpath, &cost, bound));

	if (output_rel->consider_parallel && input_rel->partial_pathlist != NIL)
	{
		Path	   *pinput = linitial(input_rel->partial_pathlist);

		if (PATH_REQ_OUTER(pinput) == NULL &&
			equal(pinput->pathtarget->exprs, input->pathtarget->exprs) &&
			!pathkeys_contained_in(root->sort_pathkeys, pinput->pathkeys))
		{
			SortPath   *prow = create_sort_path(root, output_rel, pinput, root->sort_pathkeys,
												bound > 0 ? (double) bound : -1.0);
			VexecCost	pcost;
			Path	   *psort;
			double		total;

			vexec_cost_sort(root, &prow->path, pinput, vexec_is_vector_path(pinput), &pcost);
			psort = vecsort_path(output_rel, target, prow, &pcost, bound);
			total = compute_gather_rows(psort);
			newpaths = lappend(newpaths,
							   create_gather_merge_path(root, output_rel, psort, target,
														root->sort_pathkeys, NULL, &total));
		}
	}

	/*
	 * Force mode replaces core's sorts, and keeps the paths an index orders,
	 * for cost to choose between them and VecSort: a nearest-neighbour
	 * search keeps its index where it has one -- on a cluster's segment too,
	 * which plans the search gp_core sends it with its LIMIT
	 * (pg19/modules/gp_core/gp_scan.c, bound_nearest()) -- and gets VecSort
	 * where it has none.  Where such a path is kept, the plan may hold no
	 * vector node, and VecSort is not counted possible
	 * (vexec.debug_require_vector).
	 */
	if (ps->mode == VEXEC_MODE_FORCE)
	{
		List	   *kept = NIL;

		foreach(lc, output_rel->pathlist)
			if (index_ordered_path((Path *) lfirst(lc)))
				kept = lappend(kept, lfirst(lc));
		output_rel->pathlist = kept;
		index_kept = kept != NIL;
	}
	foreach(lc, newpaths)
		add_path(output_rel, lfirst(lc));
	if (!index_kept)
		ps->npossible++;
	ps->sorts_built = true;
}

/*
 * VecSort's plan: the Sort core made of its custom path gives its keys and
 * its child, and is set aside.  Its scan tuple is the child's row, in
 * planner form for set_plan_references() to match its target list to,
 * which planner_shutdown_hook then makes references to the child's
 * columns (join.c, vexec_join_finish_plan()).
 */
static Plan *
plan_vecsort(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Sort	   *sort = linitial(custom_plans);
	VexecSortPlan plan;
	List	   *scan_tlist = NIL;
	ListCell   *lc;
	int			i;

	(void) root;
	(void) rel;
	(void) clauses;
	if (list_length(custom_plans) != 1 || !IsA(sort, Sort) || sort->plan.lefttree == NULL)
		elog(ERROR, "vexec: a VecSort's path planned without its Sort");

	memset(&plan, 0, sizeof(plan));
	for (i = 0; i < sort->numCols; i++)
	{
		plan.keycols = lappend_int(plan.keycols, sort->sortColIdx[i]);
		plan.operators = lappend_oid(plan.operators, sort->sortOperators[i]);
		plan.collations = lappend_oid(plan.collations, sort->collations[i]);
		plan.nullsfirst = lappend_int(plan.nullsfirst, sort->nullsFirst[i] ? 1 : 0);
	}
	plan.bound = intVal(linitial(best_path->custom_private));

	/* the scan tuple: the child's row, column by column */
	foreach(lc, sort->plan.lefttree->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry(copyObject(tle->expr), list_length(scan_tlist) + 1,
											 NULL, false));
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.plan.lefttree = sort->plan.lefttree;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_private = vexec_sort_plan_encode(&plan);
	cscan->custom_scan_tlist = scan_tlist;
	cscan->methods = vexec_sort_methods();
	(void) vexec_sort_make_late(cscan);
	(void) vexec_sort_plan_running_bound(cscan);
	return &cscan->scan.plan;
}

/*
 * A bounded VecSort over a VecScan made to sort its keys and TIDs alone,
 * the rest of each kept row fetched by its TID (H6, above), where every
 * column of the child's row that is no key is a Var of the scanned
 * relation, and one at least a column of its own; whether it was.  The
 * child's target list becomes the keys, then ctid; the scan tuple the
 * child's row as it was, over the relation's Vars.  Its Vars are those of
 * the relation the child scans, whichever form the plan is in --
 * PostgreSQL's planner's, before set_plan_references() moves them by the
 * same offset as the child's, or ORCA's -- and the executor finds the
 * relation from the child.
 */
bool
vexec_sort_make_late(CustomScan *cscan)
{
	VexecSortPlan plan;
	Plan	   *child = cscan->scan.plan.lefttree;
	Index		relid;
	List	   *scan_tlist = NIL;
	List	   *child_tlist = NIL;
	List	   *keycols = NIL;
	bool		any_late = false;
	ListCell   *lc;
	int			nkeys;
	int			i;

	vexec_sort_plan_decode(cscan, &plan);
	if (plan.bound <= 0 || plan.bound > VEXEC_LATE_ROWS || plan.late_tidcol > 0)
		return false;
	if (child == NULL || !IsA(child, CustomScan) ||
		((CustomScan *) child)->methods != vexec_scan_methods() ||
		child->parallel_aware || ((Scan *) child)->scanrelid == 0 ||
		contain_subplans((Node *) child->targetlist))
		return false;
	relid = ((Scan *) child)->scanrelid;

	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Var		   *v = (Var *) tle->expr;

		if (list_member_int(plan.keycols, foreach_current_index(lc) + 1))
			continue;
		if (!IsA(v, Var) || v->varno != relid || v->varlevelsup != 0 || v->varattno == 0)
			return false;
		if (v->varattno > 0)
			any_late = true;
	}
	if (!any_late)
		return false;

	/* the scan tuple: the child's row as it was */
	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry(copyObject(tle->expr), list_length(scan_tlist) + 1,
											 NULL, false));
	}

	/* the child's: the keys, then each row's TID */
	nkeys = list_length(plan.keycols);
	for (i = 0; i < nkeys; i++)
	{
		TargetEntry *tle = list_nth_node(TargetEntry, child->targetlist,
										 list_nth_int(plan.keycols, i) - 1);

		child_tlist = lappend(child_tlist, makeTargetEntry(copyObject(tle->expr), i + 1,
														   NULL, false));
		keycols = lappend_int(keycols, i + 1);
	}
	child_tlist = lappend(child_tlist,
						  makeTargetEntry((Expr *) makeVar(relid, SelfItemPointerAttributeNumber,
														   TIDOID, -1, InvalidOid, 0),
										  nkeys + 1, NULL, false));

	child->targetlist = child_tlist;
	cscan->custom_scan_tlist = scan_tlist;
	plan.keycols = keycols;
	plan.late_tidcol = nkeys + 1;
	plan.late_relid = 0;		/* the child's, wherever setrefs puts it */
	cscan->custom_private = vexec_sort_plan_encode(&plan);
	return true;
}

/*
 * Whether an operator's function may raise an error for some row's values:
 * not a comparison by a btree operator family's operator of two built-in
 * scalar types, nor LIKE or ILIKE of a constant pattern -- whose errors,
 * a pattern's or a collation's, come from constants and are raised at the
 * first row.
 */
static bool
operator_may_raise(Oid opno, List *args)
{
	Oid			left;
	Oid			right;
	char	   *name;

	if (opno >= FirstNormalObjectId)
		return true;
	op_input_types(opno, &left, &right);
	if (!OidIsValid(left) || !OidIsValid(right) ||
		get_typtype(getBaseType(left)) != TYPTYPE_BASE || type_is_array(getBaseType(left)) ||
		get_typtype(getBaseType(right)) != TYPTYPE_BASE || type_is_array(getBaseType(right)))
		return true;
	if (get_op_index_interpretation(opno) != NIL)
		return false;
	name = get_opname(opno);
	if (name != NULL &&
		(strcmp(name, "~~") == 0 || strcmp(name, "!~~") == 0 ||
		 strcmp(name, "~~*") == 0 || strcmp(name, "!~~*") == 0))
		return list_length(args) != 2 || !IsA(lsecond(args), Const);
	return true;
}

/*
 * Whether an expression may raise an error for some row's values (H6): the
 * running bound is checked before a scan's quals only where none may, so
 * that a row PostgreSQL's quals would raise on is never dropped unread.
 * Only what is known not to: Vars, constants, parameters from outside, AND,
 * OR and NOT, NULL tests, relabelings, and the comparisons above.  A
 * PARAM_EXEC may run an InitPlan, which may raise.
 */
static bool
may_raise_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
			return false;
		case T_Param:
			return ((Param *) node)->paramkind != PARAM_EXTERN;
		case T_BoolExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_RelabelType:
		case T_List:
		case T_TargetEntry:
			break;
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			if (operator_may_raise(((OpExpr *) node)->opno, ((OpExpr *) node)->args))
				return true;
			break;
		case T_ScalarArrayOpExpr:
			if (operator_may_raise(((ScalarArrayOpExpr *) node)->opno, NIL))
				return true;
			break;
		default:
			return true;
	}
	return expression_tree_walker(node, may_raise_walker, context);
}

/*
 * The running bound (H6): a bounded VecSort straight over a vector scan --
 * a VecScan or a VecBitmapHeapScan, parallel-aware or not -- whose first
 * sort key is a column of the scan's relation lends the scan the first key
 * of its N-th row once tuplesort keeps its rows in a bounded heap, and the
 * scan drops a row whose first key is strictly past it: before its quals,
 * in heap's page reader before the row's other columns are deformed, where
 * none of the quals may raise an error; after them otherwise, so that no
 * error PostgreSQL would raise is skipped.  None where the scan's target
 * list may raise: PostgreSQL computes it for every row the quals pass, a
 * row the sort then discards among them.  Whether it was planned.
 */
bool
vexec_sort_plan_running_bound(CustomScan *cscan)
{
	VexecSortPlan plan;
	Plan	   *child = cscan->scan.plan.lefttree;
	CustomScan *scan;
	TargetEntry *tle;
	Var		   *v;

	vexec_sort_plan_decode(cscan, &plan);
	plan.bound_attno = 0;
	plan.bound_before_quals = false;
	if (vexec_enable_running_bound && plan.bound > 0 && plan.bound <= VEXEC_BOUND_ROWS &&
		child != NULL && IsA(child, CustomScan) &&
		(((CustomScan *) child)->methods == vexec_scan_methods() ||
		 ((CustomScan *) child)->methods == vexec_bitmapscan_methods()) &&
		((Scan *) child)->scanrelid != 0 &&
		linitial_int(plan.keycols) <= list_length(child->targetlist))
	{
		scan = (CustomScan *) child;
		tle = list_nth_node(TargetEntry, child->targetlist, linitial_int(plan.keycols) - 1);
		v = (Var *) tle->expr;
		if (IsA(v, Var) && v->varno == scan->scan.scanrelid && v->varattno > 0 &&
			v->varlevelsup == 0 && !may_raise_walker((Node *) child->targetlist, NULL))
		{
			plan.bound_attno = v->varattno;
			plan.bound_before_quals = !may_raise_walker((Node *) child->qual, NULL) &&
				!may_raise_walker((Node *) scan->custom_exprs, NULL);
		}
	}
	cscan->custom_private = vexec_sort_plan_encode(&plan);
	return plan.bound_attno > 0;
}

/* ---------------------------------------------------------------------
 * ORCA's Sort, and the Limit above it
 * ---------------------------------------------------------------------
 */

/* Whether ORCA's Sort can be a VecSort, and why not. */
const char *
vexec_orca_sort_refusal(Sort *sort)
{
	if (sort->plan.lefttree == NULL)
		return "no input";
	if (sort->numCols <= 0)
		return "no sort key";
	if (sort->plan.qual != NIL)
		return "a Sort with conditions";
	return NULL;
}

/* An expression over the Sort's child, as OUTER_VAR, over the scan tuple. */
static Node *
outer_to_index_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == OUTER_VAR)
	{
		Var		   *v = copyObject((Var *) node);

		v->varno = INDEX_VAR;
		return (Node *) v;
	}
	return expression_tree_mutator(node, outer_to_index_mutator, context);
}

/*
 * VecSort in place of a Sort ORCA's translator built: the same keys over the
 * same child, its plan node id, costs and parameters kept; its target list,
 * which reads the child's columns as OUTER_VAR, read over the scan tuple,
 * which is the child's row and whose columns are references to the child's.
 */
Plan *
vexec_build_sort_from_sort(Sort *sort)
{
	CustomScan *cscan = makeNode(CustomScan);
	VexecSortPlan plan;
	List	   *scan_tlist = NIL;
	ListCell   *lc;
	int			i;

	memset(&plan, 0, sizeof(plan));
	for (i = 0; i < sort->numCols; i++)
	{
		plan.keycols = lappend_int(plan.keycols, sort->sortColIdx[i]);
		plan.operators = lappend_oid(plan.operators, sort->sortOperators[i]);
		plan.collations = lappend_oid(plan.collations, sort->collations[i]);
		plan.nullsfirst = lappend_int(plan.nullsfirst, sort->nullsFirst[i] ? 1 : 0);
	}
	foreach(lc, sort->plan.lefttree->targetlist)
	{
		Var		   *v = makeVarFromTargetEntry(OUTER_VAR, lfirst_node(TargetEntry, lc));

		v->varnosyn = 0;
		v->varattnosyn = 0;
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) v, list_length(scan_tlist) + 1,
														 NULL, false));
	}

	cscan->scan.plan = sort->plan;
	cscan->scan.plan.type = T_CustomScan;
	cscan->scan.plan.targetlist = (List *)
		outer_to_index_mutator((Node *) sort->plan.targetlist, NULL);
	cscan->scan.scanrelid = 0;
	cscan->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = vexec_sort_plan_encode(&plan);
	cscan->methods = vexec_sort_methods();
	return &cscan->scan.plan;
}

/* An expression over the scan tuple, as OUTER_VAR over the child, again. */
static Node *
index_to_outer_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == INDEX_VAR)
	{
		Var		   *v = copyObject((Var *) node);

		v->varno = OUTER_VAR;
		return (Node *) v;
	}
	return expression_tree_mutator(node, index_to_outer_mutator, context);
}

/*
 * The Sort a VecSort was made of from ORCA's: for a parent that marks and
 * restores it, which a vector node does not do (orca.c).
 */
Plan *
vexec_unbuild_sort(CustomScan *cscan)
{
	Sort	   *sort = makeNode(Sort);
	VexecSortPlan plan;
	int			n;
	int			i;

	vexec_sort_plan_decode(cscan, &plan);
	n = list_length(plan.keycols);
	sort->plan = cscan->scan.plan;
	sort->plan.type = T_Sort;
	sort->plan.targetlist = (List *) index_to_outer_mutator((Node *) cscan->scan.plan.targetlist,
															NULL);
	sort->numCols = n;
	sort->sortColIdx = palloc(sizeof(AttrNumber) * n);
	sort->sortOperators = palloc(sizeof(Oid) * n);
	sort->collations = palloc(sizeof(Oid) * n);
	sort->nullsFirst = palloc(sizeof(bool) * n);
	for (i = 0; i < n; i++)
	{
		sort->sortColIdx[i] = (AttrNumber) list_nth_int(plan.keycols, i);
		sort->sortOperators[i] = list_nth_oid(plan.operators, i);
		sort->collations[i] = list_nth_oid(plan.collations, i);
		sort->nullsFirst[i] = list_nth_int(plan.nullsfirst, i) != 0;
	}
	return &sort->plan;
}

/*
 * What a VecSort stands for, to a pass over the finished plan that looks at
 * nodes by their kind (gp_orca_vec.h, describe_node; gp_core's
 * bound_gathers(), which sends a nearest-neighbour search's ORDER BY and
 * LIMIT to the segments through the sort below a Limit): a Sort of its keys
 * whose target list is its child's row, a column an OUTER_VAR Var, which the
 * keys index, and whose lefttree is the node's own child, through which the
 * pass may change it.  The node's own target list is its projection, the
 * query's final target on PostgreSQL's planner's route.  Read, never put in
 * the plan.
 */
Sort *
vexec_sort_describe(CustomScan *cscan)
{
	Sort	   *sort = makeNode(Sort);
	Plan	   *child = cscan->scan.plan.lefttree;
	VexecSortPlan plan;
	ListCell   *lc;
	int			n;
	int			i;

	vexec_sort_plan_decode(cscan, &plan);
	n = list_length(plan.keycols);
	sort->plan = cscan->scan.plan;
	sort->plan.type = T_Sort;
	sort->plan.qual = NIL;
	sort->plan.targetlist = NIL;
	sort->plan.lefttree = child;
	sort->plan.righttree = NULL;
	if (child != NULL)
	{
		foreach(lc, child->targetlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			sort->plan.targetlist =
				lappend(sort->plan.targetlist,
						makeTargetEntry((Expr *) makeVar(OUTER_VAR, tle->resno,
														 exprType((Node *) tle->expr),
														 exprTypmod((Node *) tle->expr),
														 exprCollation((Node *) tle->expr), 0),
										tle->resno, NULL, false));
		}
	}
	sort->numCols = n;
	sort->sortColIdx = palloc(sizeof(AttrNumber) * n);
	sort->sortOperators = palloc(sizeof(Oid) * n);
	sort->collations = palloc(sizeof(Oid) * n);
	sort->nullsFirst = palloc(sizeof(bool) * n);
	for (i = 0; i < n; i++)
	{
		sort->sortColIdx[i] = (AttrNumber) list_nth_int(plan.keycols, i);
		sort->sortOperators[i] = list_nth_oid(plan.operators, i);
		sort->collations[i] = list_nth_oid(plan.collations, i);
		sort->nullsFirst[i] = list_nth_int(plan.nullsfirst, i) != 0;
	}
	return sort;
}

/*
 * A Limit ORCA's translator built: where its count and offset are
 * constants, and a VecSort is below it, directly or under projections that
 * pass every row on (a Result of no qual and no set-returning function, or
 * a VecResult of the same), the VecSort is bounded at their sum, as
 * ExecSetTupleBound() bounds a Sort (execProcnode.c).  The Limit stays.
 */
void
vexec_orca_limit_bound(Limit *limit)
{
	Plan	   *below = limit->plan.lefttree;
	Const	   *count = (Const *) limit->limitCount;
	int64		offset = 0;
	int64		n;

	if (count == NULL || !IsA(count, Const) || count->constisnull ||
		limit->limitOption != LIMIT_OPTION_COUNT)
		return;
	n = DatumGetInt64(count->constvalue);
	if (n <= 0)
		return;
	if (limit->limitOffset != NULL)
	{
		Const	   *off = (Const *) limit->limitOffset;

		if (!IsA(off, Const))
			return;
		if (!off->constisnull)
			offset = Max(DatumGetInt64(off->constvalue), 0);
	}
	if (n > PG_INT32_MAX - offset)
		return;

	while (below != NULL)
	{
		if (IsA(below, Result) && below->qual == NIL &&
			((Result *) below)->resconstantqual == NULL &&
			!expression_returns_set((Node *) below->targetlist))
			below = below->lefttree;
		else if (IsA(below, CustomScan) &&
				 ((CustomScan *) below)->methods == vexec_result_methods() &&
				 below->qual == NIL &&
				 !expression_returns_set((Node *) below->targetlist))
			below = below->lefttree;
		else
			break;
	}
	if (below != NULL && IsA(below, CustomScan) &&
		((CustomScan *) below)->methods == vexec_sort_methods())
	{
		vexec_sort_set_bound((CustomScan *) below, n + offset);
		(void) vexec_sort_make_late((CustomScan *) below);
		(void) vexec_sort_plan_running_bound((CustomScan *) below);
	}
}
