/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * join.c
 *	  VecHashJoin in the planners (pg_vector_executor.md §3.3.3, §3.3.4,
 *	  §3.8): what the oracle accepts of a hash join, VecHashJoin's path in
 *	  PostgreSQL's planner, and its plan, built from a path or from the
 *	  HashJoin ORCA's translator built.
 *
 * PostgreSQL's planner calls set_join_pathlist_hook for every pair of
 * relations it joins, once per orientation, after its own paths of the
 * pair (joinpath.c:368-381).  For an inner, left, semi, anti or right join
 * with hash-joinable clauses, where the join's strategy mask allows a hash
 * join, the hook adds a VecHashJoin path over the cheapest paths of the two
 * sides, vector or row, priced by the vector cost model (cost.c).  In auto
 * mode it competes in add_path; in force mode the join relation keeps only
 * vector paths once it has one.  Explain mode records it.
 *
 * The plan's canonical form (plan/check.c): no scanrelid, the outer side in
 * lefttree and the inner side in righttree -- no Hash node -- and
 * custom_scan_tlist a join row: the outer plan's output columns, then the
 * inner plan's.  Its target list and qual, the join's other quals, read it
 * as INDEX_VAR, and so do custom_exprs' hash clauses, join quals and keys.
 * For PostgreSQL's planner set_customscan_references() puts them so
 * (setrefs.c), matching each Var to custom_scan_tlist by its relation,
 * column and nullingrels.  Above an outer join a column of its nullable
 * side carries the join in its nullingrels, below it not; the join's own
 * clauses read it below, its output above.  So the Vars of the scan tuple
 * and of the clauses are given the nullingrels the output reads them with,
 * as PG-Strom's GpuJoin gives its own (pg-strom/src/gpu_join.c:276-330):
 * the executor reads none of it.  For ORCA the builder rewrites the
 * HashJoin's OUTER_VAR and INNER_VAR references, and takes the Hash node's
 * input for its inner side.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/extendplan.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"

#include "vexec.h"
#include "exec/exec.h"
#include "plan/plan.h"

static Plan *plan_vechashjoin(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
							  List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods vechashjoin_path_methods = {
	.CustomName = VEXEC_HASHJOIN_NAME,
	.PlanCustomPath = plan_vechashjoin,
};

/* "1 hash clause", "2 hash clauses" */
static const char *
count_of(int n, const char *one, const char *many)
{
	return psprintf("%d %s", n, n == 1 ? one : many);
}

/* Whether a path is one of vexec's vector paths. */
bool
vexec_is_vector_path(Path *path)
{
	return path != NULL && IsA(path, CustomPath) &&
		strncmp(((CustomPath *) path)->methods->CustomName, "Vec", 3) == 0;
}

/* ---------------------------------------------------------------------
 * The oracle
 * ---------------------------------------------------------------------
 */

static const char *
join_type_name(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER:
			return "inner";
		case JOIN_LEFT:
			return "left";
		case JOIN_FULL:
			return "full";
		case JOIN_RIGHT:
			return "right";
		case JOIN_SEMI:
			return "semi";
		case JOIN_ANTI:
			return "anti";
		case JOIN_RIGHT_SEMI:
			return "right semi";
		case JOIN_RIGHT_ANTI:
			return "right anti";
		default:
			return "unique";
	}
}

/* The join types VecHashJoin takes (§3.8): a full join comes later. */
static bool
join_type_supported(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_SEMI:
		case JOIN_ANTI:
		case JOIN_RIGHT:
			return true;
		default:
			return false;
	}
}

/*
 * The hash clauses of a join, chosen from its restrictlist by the test
 * core's static hash_inner_and_outer() applies
 * (PG19:src/backend/optimizer/path/joinpath.c:2185-2245).
 */
static List *
hash_clauses(RelOptInfo *joinrel, RelOptInfo *outerrel, RelOptInfo *innerrel,
			 JoinType jointype, JoinPathExtraData *extra)
{
	bool		isouterjoin = IS_OUTER_JOIN(jointype);
	List	   *clauses = NIL;
	ListCell   *l;

	foreach(l, extra->restrictlist)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		/* an outer join hashes on its own join clauses only */
		if (isouterjoin && RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
			continue;
		if (!rinfo->can_join || rinfo->hashjoinoperator == InvalidOid)
			continue;
		if (!clause_sides_match_join(rinfo, outerrel->relids, innerrel->relids))
			continue;
		/* "inner op outer" is commuted when the plan is made */
		if (!rinfo->outer_is_left &&
			!OidIsValid(get_commutator(castNode(OpExpr, rinfo->clause)->opno)))
			continue;
		clauses = lappend(clauses, rinfo);
	}
	return clauses;
}

/* A clause's sides, as get_switched_clauses() puts them: the outer first. */
static OpExpr *
switched_clause(RestrictInfo *rinfo, Relids outerrelids)
{
	OpExpr	   *clause = castNode(OpExpr, rinfo->clause);

	if (bms_is_subset(rinfo->right_relids, outerrelids))
	{
		OpExpr	   *temp = makeNode(OpExpr);

		temp->opno = clause->opno;
		temp->opfuncid = InvalidOid;
		temp->opresulttype = clause->opresulttype;
		temp->opretset = clause->opretset;
		temp->opcollid = clause->opcollid;
		temp->inputcollid = clause->inputcollid;
		temp->args = list_copy(clause->args);
		temp->location = clause->location;
		CommuteOpExpr(temp);
		return temp;
	}
	return clause;
}

/*
 * The clauses in the order a qual list runs them (createplan.c's static
 * order_qual_clauses()): by security level, a leakproof and cheap clause
 * free of its own, then by cost, stably.
 */
static List *
order_quals(PlannerInfo *root, List *clauses)
{
	int			n = list_length(clauses);
	Node	  **items;
	Cost	   *costs;
	Index	   *levels;
	List	   *result = NIL;
	int			i;

	if (n <= 1)
		return clauses;
	items = palloc(sizeof(Node *) * n);
	costs = palloc(sizeof(Cost) * n);
	levels = palloc(sizeof(Index) * n);
	for (i = 0; i < n; i++)
	{
		Node	   *clause = list_nth(clauses, i);
		QualCost	qc;

		cost_qual_eval_node(&qc, clause, root);
		items[i] = clause;
		costs[i] = qc.per_tuple;
		levels[i] = 0;
		if (IsA(clause, RestrictInfo))
		{
			RestrictInfo *rinfo = (RestrictInfo *) clause;

			if (!(rinfo->leakproof && costs[i] < 10 * cpu_operator_cost))
				levels[i] = rinfo->security_level;
		}
	}
	for (i = 1; i < n; i++)
	{
		Node	   *item = items[i];
		Cost		cost = costs[i];
		Index		level = levels[i];
		int			j;

		for (j = i; j > 0; j--)
		{
			if (level > levels[j - 1] || (level == levels[j - 1] && cost >= costs[j - 1]))
				break;
			items[j] = items[j - 1];
			costs[j] = costs[j - 1];
			levels[j] = levels[j - 1];
		}
		items[j] = item;
		costs[j] = cost;
		levels[j] = level;
	}
	for (i = 0; i < n; i++)
		result = lappend(result, items[i]);
	return result;
}

/* ---------------------------------------------------------------------
 * PostgreSQL's planner: the path
 * ---------------------------------------------------------------------
 */

/*
 * Force mode's VecHashJoin for a join relation, kept on the relation: the
 * cheapest one any pair of its inputs gave, so that it can be added again
 * where a later pair's row path made add_path() drop it.  Its input paths
 * are their relations' cheapest, which no later join changes.  It names its
 * relation: eager aggregation's grouped relation is a flat copy of the
 * relation it groups, extension state and all (relnode.c,
 * build_grouped_rel()), and reads the same state.
 */
typedef struct ForceJoinPath
{
	bool		valid;
	Path	   *outer;
	Path	   *inner;
	JoinType	jointype;
	bool		inner_unique;
	List	   *hashrinfos;
	List	   *restrictlist;
	double		rows;
	VexecCost	cost;
} ForceJoinPath;

typedef struct ForceJoin
{
	RelOptInfo *rel;
	ForceJoinPath full;			/* the cheapest VecHashJoin */
	ForceJoinPath partial;		/* the cheapest partial one (V4) */
} ForceJoin;

static ForceJoin *
force_join_of(RelOptInfo *joinrel)
{
	ForceJoin  *fj = GetRelOptInfoExtensionState(joinrel, vexec_planner_extension_id());

	return fj != NULL && fj->rel == joinrel ? fj : NULL;
}

static Path *
hashjoin_path(RelOptInfo *joinrel, JoinType jointype, bool inner_unique, List *restrictlist,
			  Path *outer, Path *inner, List *hashrinfos, double rows, const VexecCost *cost)
{
	CustomPath *cp = makeNode(CustomPath);

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = joinrel;
	cp->path.pathtarget = joinrel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = joinrel->consider_parallel &&
		outer->parallel_safe && inner->parallel_safe;
	cp->path.parallel_workers = 0;
	cp->path.rows = rows;
	cp->path.disabled_nodes = outer->disabled_nodes + inner->disabled_nodes;
	cp->path.startup_cost = cost->startup;
	cp->path.total_cost = cost->total;
	cp->path.pathkeys = NIL;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = list_make2(outer, inner);
	cp->custom_private = list_make4(makeInteger(jointype), makeBoolean(inner_unique),
									list_copy(hashrinfos), list_copy(restrictlist));
	cp->methods = &vechashjoin_path_methods;
	return &cp->path;
}

/*
 * PostgreSQL's own hash join for these inputs, a partial one where the
 * outer side is.  final_cost_hashjoin() caches each hash clause's bucket
 * size and MCV frequency in its RestrictInfo, computed for the pair it
 * costs (costsize.c:4510-4541), and core reads them back for every later
 * pair.  Core may never have costed this pair -- add_path_precheck()
 * turned it down -- so the cache is put back as it was, and pricing an
 * alternative changes no later cost and no plan.
 */
static HashPath *
row_hashjoin(PlannerInfo *root, RelOptInfo *joinrel, JoinType jointype, List *clauses,
			 Path *outer, Path *inner, JoinPathExtraData *extra)
{
	JoinCostWorkspace workspace;
	HashPath   *rowpath;
	int			n = list_length(clauses);
	Selectivity *saved = palloc(sizeof(Selectivity) * 4 * Max(n, 1));
	int			i = 0;
	ListCell   *lc;

	initial_cost_hashjoin(root, &workspace, jointype, clauses, outer, inner, extra, false);
	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = lfirst(lc);

		saved[i++] = rinfo->left_bucketsize;
		saved[i++] = rinfo->right_bucketsize;
		saved[i++] = rinfo->left_mcvfreq;
		saved[i++] = rinfo->right_mcvfreq;
	}
	rowpath = create_hashjoin_path(root, joinrel, jointype, &workspace, extra,
								   outer, inner, false, extra->restrictlist, NULL, clauses);
	i = 0;
	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = lfirst(lc);

		rinfo->left_bucketsize = saved[i++];
		rinfo->right_bucketsize = saved[i++];
		rinfo->left_mcvfreq = saved[i++];
		rinfo->right_mcvfreq = saved[i++];
	}
	pfree(saved);
	return rowpath;
}

/*
 * Force mode, after each pair of a join relation's inputs, for its partial
 * paths: a relation for which some pair gave a partial VecHashJoin has the
 * cheapest of them as its one partial path, whatever add_partial_path()
 * made of later pairs' partial row paths.
 */
static void
force_vector_partial_paths(RelOptInfo *joinrel)
{
	ForceJoin  *fj = force_join_of(joinrel);
	ForceJoinPath *fp;
	Path	   *path;

	if (fj == NULL || !fj->partial.valid)
		return;
	fp = &fj->partial;
	path = hashjoin_path(joinrel, fp->jointype, fp->inner_unique, fp->restrictlist,
						 fp->outer, fp->inner, fp->hashrinfos, fp->rows, &fp->cost);
	path->parallel_safe = true;
	path->parallel_workers = fp->outer->parallel_workers;
	joinrel->partial_pathlist = NIL;
	add_partial_path(joinrel, path);
}

/*
 * Force mode, after each pair of a join relation's inputs: a relation for
 * which some pair gave a VecHashJoin keeps only vector paths -- the cheapest
 * one again where a later pair's row path made add_path() drop it -- its
 * partial paths aside (force_vector_partial_paths()).
 */
static void
force_vector_paths(RelOptInfo *joinrel, Path *added)
{
	ForceJoin  *fj = force_join_of(joinrel);
	List	   *keep = NIL;
	bool		any = false;
	ListCell   *lc;

	foreach(lc, joinrel->pathlist)
	{
		if (vexec_is_vector_path(lfirst(lc)))
		{
			keep = lappend(keep, lfirst(lc));
			any = true;
		}
	}
	if ((fj == NULL || !fj->full.valid) && added == NULL && !any)
		return;
	joinrel->pathlist = keep;
	if (added != NULL)
		add_path(joinrel, added);
	else if (!any && fj != NULL && fj->full.valid)
		add_path(joinrel, hashjoin_path(joinrel, fj->full.jointype, fj->full.inner_unique,
										fj->full.restrictlist, fj->full.outer, fj->full.inner,
										fj->full.hashrinfos, fj->full.rows, &fj->full.cost));
}

/*
 * Force mode: the relation's cheapest VecHashJoin so far, or partial
 * VecHashJoin, kept on it.
 */
static void
force_keep(RelOptInfo *joinrel, bool partial, JoinType jointype, JoinPathExtraData *extra,
		   Path *outer, Path *inner, List *hashrinfos, double rows, const VexecCost *cost)
{
	ForceJoin  *fj = force_join_of(joinrel);
	ForceJoinPath *fp;

	if (fj == NULL)
	{
		fj = palloc0(sizeof(ForceJoin));
		fj->rel = joinrel;
		SetRelOptInfoExtensionState(joinrel, vexec_planner_extension_id(), fj);
	}
	fp = partial ? &fj->partial : &fj->full;
	if (fp->valid && fp->cost.total <= cost->total)
		return;
	fp->valid = true;
	fp->outer = outer;
	fp->inner = inner;
	fp->jointype = jointype;
	fp->inner_unique = extra->inner_unique;
	fp->hashrinfos = list_copy(hashrinfos);
	fp->restrictlist = list_copy(extra->restrictlist);
	fp->rows = rows;
	fp->cost = *cost;
}

/*
 * gp_core's runtime filter, the port's gp.enable_runtime_filter: after
 * planning, a node above the outer side of each of the planner's hash joins
 * of these types that Cloudberry's rule takes, filled as the join's Hash
 * reads its input (pg19/modules/gp_core/gp_rtfilter.c, rtf_add_filters());
 * a VecHashJoin has no Hash and carries none.  While the setting is on, a
 * join it may take stays PostgreSQL's (§3.8).  On a vanilla server the
 * setting does not exist.
 */
static bool
runtime_filter_may_take(JoinType jointype)
{
	const char *on = GetConfigOption("gp.enable_runtime_filter", true, false);

	return on != NULL && strcmp(on, "on") == 0 &&
		(jointype == JOIN_INNER || jointype == JOIN_RIGHT || jointype == JOIN_SEMI);
}

/*
 * A hash join's vector alternative (§3.3.3, Hash joins): an inner, left,
 * semi, anti or right join with hash-joinable clauses, where the join's
 * strategy mask allows a hash join, over the cheapest unparameterized paths
 * of the two sides.  Nothing of extra's own is kept: the restrictlist is
 * the join relation's, which core's paths keep too.
 */
void
vexec_consider_hashjoin(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel,
						RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra,
						VexecPlanState *ps)
{
	VexecAlt   *alt = NULL;
	bool		add = ps->mode == VEXEC_MODE_AUTO || ps->mode == VEXEC_MODE_FORCE;
	Path	   *added = NULL;
	List	   *clauses;
	Path	   *outer = outerrel->cheapest_total_path;
	Path	   *inner = innerrel->cheapest_total_path;
	JoinType	orig_jointype = jointype;
	HashPath   *rowpath;
	VexecCost	cost;
	VexecSteps	steps;
	const char *refusal = NULL;
	ListCell   *lc;
	int			nkernel = 0;

	if (ps->record)
		alt = vexec_alt_record(ps, "VecHashJoin", vexec_relids_names(root, joinrel->relids),
							   root, joinrel->relids);
	if (alt == NULL && !add)
		return;

	/*
	 * A semi join's inner side made unique, then joined: an inner join to
	 * core's paths (joinpath.c, add_paths_to_joinrel()), over the unique
	 * relation's paths.
	 */
	if (jointype == JOIN_UNIQUE_INNER || jointype == JOIN_UNIQUE_OUTER)
		jointype = JOIN_INNER;

	if (!join_type_supported(jointype))
		refusal = psprintf("a %s join", join_type_name(jointype));
	else if (!vexec_enable_hashjoin)
		refusal = "vexec.enable_hashjoin is off";
	else if ((extra->pgs_mask & PGS_HASHJOIN) == 0)
		refusal = "hash joins are disabled";
	else if (runtime_filter_may_take(jointype))
		refusal = "a join gp_core's runtime filter may take (gp.enable_runtime_filter)";
	else if (IS_GROUPED_REL(joinrel))
		refusal = "eager aggregation's grouped join";
	else if (joinrel->lateral_relids != NULL)
		refusal = "lateral references";
	else if (outer == NULL || inner == NULL ||
			 PATH_REQ_OUTER(outer) != NULL || PATH_REQ_OUTER(inner) != NULL)
		refusal = "a parameterized input";
	else if (ps->mode != VEXEC_MODE_FORCE && Max(outer->rows, inner->rows) < vexec_min_rows)
		refusal = psprintf("%.0f rows at most a side, fewer than vexec.min_rows",
						   Max(outer->rows, inner->rows));
	if (refusal != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		goto done;
	}
	clauses = hash_clauses(joinrel, outerrel, innerrel, jointype, extra);
	if (clauses == NIL)
	{
		vexec_alt_refuse(ps, alt, "no hash-joinable clause");
		goto done;
	}
	foreach(lc, extra->restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		/* a gating clause core's join plan puts in a Result; a scan's plan would drop it */
		if (rinfo->pseudoconstant)
		{
			vexec_alt_refuse(ps, alt, "a pseudoconstant join clause");
			goto done;
		}
	}
	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		OpExpr	   *op = castNode(OpExpr, rinfo->clause);

		if (!op_strict(op->opno))
		{
			vexec_alt_refuse(ps, alt, "a hash operator that is not strict");
			goto done;
		}
	}
	memset(&steps, 0, sizeof(steps));
	vexec_oracle_exprs(root, extra->restrictlist, &steps);
	if (steps.refusal)
	{
		vexec_alt_refuse(ps, alt, steps.refusal);
		goto done;
	}

	rowpath = row_hashjoin(root, joinrel, jointype, clauses, outer, inner, extra);
	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = lfirst(lc);
		OpExpr	   *op = castNode(OpExpr, rinfo->clause);

		set_opfuncid(op);
		if (vexec_kernel_bound(op->opfuncid, exprType(linitial(op->args)), op->inputcollid))
			nkernel++;
	}
	vexec_cost_hashjoin(root, &rowpath->jpath.path, outer, inner, list_length(clauses),
						nkernel, vexec_is_vector_path(outer), vexec_is_vector_path(inner), &cost);
	vexec_alt_costed(ps, alt, &cost,
					 psprintf("%s join, outer %s, inner %s; %s, %d with kernels",
							  join_type_name(jointype),
							  vexec_relids_names(root, outerrel->relids),
							  vexec_relids_names(root, innerrel->relids),
							  count_of(list_length(clauses), "hash clause", "hash clauses"),
							  nkernel));
	if (add)
	{
		added = hashjoin_path(joinrel, jointype, extra->inner_unique, extra->restrictlist,
							  outer, inner, clauses, rowpath->jpath.path.rows, &cost);
		if (ps->mode == VEXEC_MODE_FORCE)
			force_keep(joinrel, false, jointype, extra, outer, inner, clauses,
					   rowpath->jpath.path.rows, &cost);
		else
			add_path(joinrel, added);
		ps->npossible++;

		/*
		 * A partial path too (V4), as hash_inner_and_outer() makes a
		 * parallel-oblivious one (joinpath.c): the outer side's cheapest
		 * partial path, which the Gather's participants share, and the
		 * inner side whole in each, its cheapest parallel-safe path, each
		 * participant building its own table.  Not a right join, whose
		 * unmatched inner rows each participant would return; not a join
		 * made unique on one side.
		 */
		if (joinrel->consider_parallel && outerrel->partial_pathlist != NIL &&
			(orig_jointype == JOIN_INNER || orig_jointype == JOIN_LEFT ||
			 orig_jointype == JOIN_SEMI || orig_jointype == JOIN_ANTI))
		{
			Path	   *pouter = linitial(outerrel->partial_pathlist);
			Path	   *pinner = inner->parallel_safe ? inner :
				get_cheapest_parallel_safe_total_inner(innerrel->pathlist);

			if (pinner != NULL && PATH_REQ_OUTER(pinner) == NULL && pouter->parallel_workers > 0)
			{
				HashPath   *prowpath = row_hashjoin(root, joinrel, jointype, clauses,
													pouter, pinner, extra);
				VexecCost	pcost;
				Path	   *ppath;

				vexec_cost_hashjoin(root, &prowpath->jpath.path, pouter, pinner,
									list_length(clauses), nkernel,
									vexec_is_vector_path(pouter), vexec_is_vector_path(pinner),
									&pcost);
				ppath = hashjoin_path(joinrel, jointype, extra->inner_unique, extra->restrictlist,
									  pouter, pinner, clauses, prowpath->jpath.path.rows, &pcost);
				ppath->parallel_safe = true;
				ppath->parallel_workers = pouter->parallel_workers;
				if (ps->mode == VEXEC_MODE_FORCE)
					force_keep(joinrel, true, jointype, extra, pouter, pinner, clauses,
							   prowpath->jpath.path.rows, &pcost);
				else
					add_partial_path(joinrel, ppath);
			}
		}
	}

done:
	if (ps->mode == VEXEC_MODE_FORCE)
	{
		force_vector_paths(joinrel, added);
		force_vector_partial_paths(joinrel);
	}
}

/* ---------------------------------------------------------------------
 * PostgreSQL's planner: the plan
 * ---------------------------------------------------------------------
 */

/*
 * The nullingrels a join's expressions match its scan tuple by.
 * set_customscan_references() matches each Var and PlaceHolderVar of the
 * join's target list, quals and custom_exprs to custom_scan_tlist by its
 * relation and column, or its phid, and asks for the same nullingrels
 * (setrefs.c, fix_upper_expr(), NRM_EQUAL).  Above an outer join a column
 * of its nullable side carries the join, below it not; the join's own
 * clauses read it below, its output above.  So each scan tuple column takes
 * the version the output reads it in, and every reference to it in the
 * clauses that version too.  The walk is fix_upper_expr()'s: an expression
 * equal to a scan tuple column that is not a Var matches whole and is left
 * as it is; a PlaceHolderVar a child supplies matches by its phid; one the
 * join evaluates is looked into, and the Vars in it are read above the
 * join, as its output's are.
 */
typedef struct NullingContext
{
	List	   *entries;		/* custom_scan_tlist */
	List	   *above;			/* Vars and PlaceHolderVars as the output
								 * reads them */
} NullingContext;

static TargetEntry *
entry_of_var(List *entries, Var *var)
{
	ListCell   *lc;

	foreach(lc, entries)
	{
		Var		   *e = (Var *) lfirst_node(TargetEntry, lc)->expr;

		if (IsA(e, Var) && e->varno == var->varno && e->varattno == var->varattno &&
			e->varlevelsup == var->varlevelsup)
			return lfirst(lc);
	}
	return NULL;
}

static TargetEntry *
entry_of_phv(List *entries, PlaceHolderVar *phv)
{
	ListCell   *lc;

	foreach(lc, entries)
	{
		PlaceHolderVar *e = (PlaceHolderVar *) lfirst_node(TargetEntry, lc)->expr;

		if (IsA(e, PlaceHolderVar) && e->phid == phv->phid && e->phlevelsup == phv->phlevelsup)
			return lfirst(lc);
	}
	return NULL;
}

/* An expression the scan tuple has as a column of its own, not a Var. */
static bool
matches_whole(List *entries, Node *node)
{
	ListCell   *lc;

	if (IsA(node, Const) || IsA(node, Param))
		return false;
	foreach(lc, entries)
	{
		Node	   *e = (Node *) lfirst_node(TargetEntry, lc)->expr;

		if (!IsA(e, Var) && !IsA(e, PlaceHolderVar) && equal(e, node))
			return true;
	}
	return false;
}

static bool
collect_above_walker(Node *node, NullingContext *ctx)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup == 0)
			ctx->above = lappend(ctx->above, node);
		return false;
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;

		if (entry_of_phv(ctx->entries, phv) != NULL)
		{
			ctx->above = lappend(ctx->above, node);
			return false;
		}
		return collect_above_walker((Node *) phv->phexpr, ctx);
	}
	if (matches_whole(ctx->entries, node))
		return false;
	return expression_tree_walker(node, collect_above_walker, ctx);
}

/* A scan tuple column, its Var or PlaceHolderVar in the version above. */
static Expr *
entry_as_above(Expr *expr, List *above)
{
	ListCell   *lc;

	foreach(lc, above)
	{
		Node	   *a = lfirst(lc);

		if (IsA(expr, Var) && IsA(a, Var))
		{
			Var		   *v = (Var *) expr;
			Var		   *av = (Var *) a;

			if (av->varno == v->varno && av->varattno == v->varattno &&
				av->varlevelsup == v->varlevelsup)
			{
				if (bms_equal(av->varnullingrels, v->varnullingrels))
					return expr;
				v = copyObject(v);
				v->varnullingrels = bms_copy(av->varnullingrels);
				return (Expr *) v;
			}
		}
		else if (IsA(expr, PlaceHolderVar) && IsA(a, PlaceHolderVar))
		{
			PlaceHolderVar *p = (PlaceHolderVar *) expr;
			PlaceHolderVar *ap = (PlaceHolderVar *) a;

			if (ap->phid == p->phid && ap->phlevelsup == p->phlevelsup)
			{
				if (bms_equal(ap->phnullingrels, p->phnullingrels))
					return expr;
				p = copyObject(p);
				p->phnullingrels = bms_copy(ap->phnullingrels);
				return (Expr *) p;
			}
		}
	}
	return expr;
}

/* An expression's references to the scan tuple, in its columns' versions. */
static Node *
as_entries_mutator(Node *node, NullingContext *ctx)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		TargetEntry *tle = entry_of_var(ctx->entries, (Var *) node);
		Var		   *e;

		if (tle == NULL)
			return node;
		e = (Var *) tle->expr;
		if (bms_equal(e->varnullingrels, ((Var *) node)->varnullingrels))
			return node;
		node = (Node *) copyObject(node);
		((Var *) node)->varnullingrels = bms_copy(e->varnullingrels);
		return node;
	}
	if (IsA(node, PlaceHolderVar))
	{
		PlaceHolderVar *phv = (PlaceHolderVar *) node;
		TargetEntry *tle = entry_of_phv(ctx->entries, phv);

		if (tle == NULL)
		{
			phv = copyObject(phv);
			phv->phexpr = (Expr *) as_entries_mutator((Node *) phv->phexpr, ctx);
			return (Node *) phv;
		}
		if (!bms_equal(((PlaceHolderVar *) tle->expr)->phnullingrels, phv->phnullingrels))
		{
			phv = copyObject(phv);
			phv->phnullingrels = bms_copy(((PlaceHolderVar *) tle->expr)->phnullingrels);
		}
		return (Node *) phv;
	}
	if (matches_whole(ctx->entries, node))
		return node;
	return expression_tree_mutator(node, as_entries_mutator, ctx);
}

/* The plan of a VecHashJoin path. */
static Plan *
plan_vechashjoin(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
				 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *outer_plan = linitial(custom_plans);
	Plan	   *inner_plan = lsecond(custom_plans);
	Path	   *outer_path = linitial(best_path->custom_paths);
	Path	   *inner_path = lsecond(best_path->custom_paths);
	List	   *priv = best_path->custom_private;
	JoinType	jointype = intVal(linitial(priv));
	List	   *hashrinfos = lthird(priv);
	List	   *restrictlist = lfourth(priv);
	List	   *joinclauses;
	List	   *otherclauses;
	List	   *hashclauses = NIL;
	List	   *okeys = NIL;
	List	   *ikeys = NIL;
	List	   *scan_tlist = NIL;
	List	   *above;
	VexecJoinPlan plan;
	ListCell   *lc;
	int			resno = 1;

	(void) clauses;
	memset(&plan, 0, sizeof(plan));
	plan.jointype = jointype;
	plan.inner_unique = boolVal(lsecond(priv));
	{
		VexecPlanState *ps = vexec_plan_state(root);

		if (ps != NULL)
			ps->joins_built = true;
	}

	/* the join's clauses, as create_hashjoin_plan() sorts them (createplan.c) */
	joinclauses = order_quals(root, restrictlist);
	if (IS_OUTER_JOIN(jointype))
		extract_actual_join_clauses(joinclauses, rel->relids, &joinclauses, &otherclauses);
	else
	{
		joinclauses = extract_actual_clauses(joinclauses, false);
		otherclauses = NIL;
	}
	joinclauses = list_difference(joinclauses, get_actual_clauses(hashrinfos));
	foreach(lc, hashrinfos)
	{
		OpExpr	   *op = switched_clause(lfirst_node(RestrictInfo, lc), outer_path->parent->relids);

		hashclauses = lappend(hashclauses, op);
		plan.hashoperators = lappend_oid(plan.hashoperators, op->opno);
		plan.hashcollations = lappend_oid(plan.hashcollations, op->inputcollid);
		okeys = lappend(okeys, linitial(op->args));
		ikeys = lappend(ikeys, lsecond(op->args));
	}

	/* the scan tuple: the outer plan's columns, then the inner plan's */
	foreach(lc, outer_plan->targetlist)
		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry(copyObject(lfirst_node(TargetEntry, lc)->expr),
											 resno++, NULL, false));
	foreach(lc, inner_plan->targetlist)
		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry(copyObject(lfirst_node(TargetEntry, lc)->expr),
											 resno++, NULL, false));
	plan.nouter = list_length(outer_plan->targetlist);
	plan.ninner = list_length(inner_plan->targetlist);
	plan.inner_rows = inner_path->rows;

	/*
	 * One version of each column: the one the join's output reads.  The
	 * join relation's target has them all: a projection core puts on the
	 * plan after it is made (createplan.c, create_projection_plan(), which
	 * asks for no target list here) reads only its columns.
	 */
	{
		NullingContext ctx = {scan_tlist, NIL};
		ListCell   *lc2;

		(void) collect_above_walker((Node *) rel->reltarget->exprs, &ctx);
		(void) collect_above_walker((Node *) tlist, &ctx);
		(void) collect_above_walker((Node *) otherclauses, &ctx);
		above = ctx.above;
		foreach(lc2, scan_tlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc2);

			tle->expr = entry_as_above(tle->expr, above);
		}
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = otherclauses;
	cscan->scan.plan.lefttree = outer_plan;
	cscan->scan.plan.righttree = inner_plan;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_scan_tlist = scan_tlist;
	{
		NullingContext ctx = {scan_tlist, NIL};

		cscan->custom_exprs = list_make4(as_entries_mutator((Node *) hashclauses, &ctx),
										 as_entries_mutator((Node *) joinclauses, &ctx),
										 as_entries_mutator((Node *) okeys, &ctx),
										 as_entries_mutator((Node *) ikeys, &ctx));
	}
	cscan->custom_private = vexec_join_plan_encode(&plan);
	cscan->methods = vexec_hashjoin_methods();
	return &cscan->scan.plan;
}

/* The scan tuple's entries of a side: its columns, as OUTER_VAR or INNER_VAR. */
static List *
side_entries(List *scan_tlist, List *child_tlist, int varno)
{
	ListCell   *lc;

	foreach(lc, child_tlist)
	{
		Var		   *v = makeVarFromTargetEntry(varno, lfirst_node(TargetEntry, lc));

		v->varnosyn = 0;
		v->varattnosyn = 0;
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) v, list_length(scan_tlist) + 1,
														 NULL, false));
	}
	return scan_tlist;
}

/*
 * A finished plan's VecHashJoins, their scan tuples made references to
 * their sides' columns -- OUTER_VAR and INNER_VAR Vars, as the builder from
 * ORCA's HashJoin makes them -- once set_plan_references() has matched
 * their expressions to them (planner_shutdown_hook).  The executor reads
 * the scan tuple's types, the same; EXPLAIN resolves a join row's column
 * through the side it comes from, as it resolves a HashJoin's: a Var of a
 * CTE's or a subquery's record type is deparsed in the plan that scans it,
 * where ruleutils.c looks for it (get_name_for_var_field()), not in the
 * join's.
 */
static void
finish_walker(Plan *plan)
{
	ListCell   *lc;

	if (plan == NULL)
		return;
	check_stack_depth();
	switch (nodeTag(plan))
	{
		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;

				if (cscan->methods == vexec_hashjoin_methods() &&
					plan->lefttree != NULL && plan->righttree != NULL)
				{
					List	   *scan_tlist = NIL;

					scan_tlist = side_entries(scan_tlist, plan->lefttree->targetlist, OUTER_VAR);
					scan_tlist = side_entries(scan_tlist, plan->righttree->targetlist, INNER_VAR);
					if (list_length(scan_tlist) == list_length(cscan->custom_scan_tlist))
						cscan->custom_scan_tlist = scan_tlist;
				}
				else if (cscan->methods == vexec_repart_methods() && plan->lefttree != NULL)
				{
					/* VecRepartition's: its child's row (agg.c, H5) */
					List	   *scan_tlist = side_entries(NIL, plan->lefttree->targetlist, OUTER_VAR);

					if (list_length(scan_tlist) == list_length(cscan->custom_scan_tlist))
						cscan->custom_scan_tlist = scan_tlist;
				}
				else if (cscan->methods == vexec_sort_methods() && plan->lefttree != NULL)
				{
					/*
					 * VecSort's: its child's row (sort.c) -- but where its
					 * columns are fetched late, by TID, the relation's own
					 * columns, which the executor computes them from
					 */
					List	   *scan_tlist = side_entries(NIL, plan->lefttree->targetlist, OUTER_VAR);
					VexecSortPlan sp;

					vexec_sort_plan_decode(cscan, &sp);
					if (sp.late_tidcol == 0 &&
						list_length(scan_tlist) == list_length(cscan->custom_scan_tlist))
						cscan->custom_scan_tlist = scan_tlist;
				}
				foreach(lc, cscan->custom_plans)
					finish_walker(lfirst(lc));
				break;
			}
		case T_Append:
			foreach(lc, ((Append *) plan)->appendplans)
				finish_walker(lfirst(lc));
			break;
		case T_MergeAppend:
			foreach(lc, ((MergeAppend *) plan)->mergeplans)
				finish_walker(lfirst(lc));
			break;
		case T_BitmapAnd:
			foreach(lc, ((BitmapAnd *) plan)->bitmapplans)
				finish_walker(lfirst(lc));
			break;
		case T_BitmapOr:
			foreach(lc, ((BitmapOr *) plan)->bitmapplans)
				finish_walker(lfirst(lc));
			break;
		case T_SubqueryScan:
			finish_walker(((SubqueryScan *) plan)->subplan);
			break;
		default:
			break;
	}
	finish_walker(plan->lefttree);
	finish_walker(plan->righttree);
}

void
vexec_join_finish_plan(PlannedStmt *pstmt)
{
	ListCell   *lc;

	finish_walker(pstmt->planTree);
	foreach(lc, pstmt->subplans)
		finish_walker(lfirst(lc));
}

/* ---------------------------------------------------------------------
 * ORCA's HashJoin
 * ---------------------------------------------------------------------
 */

/* Whether ORCA's HashJoin can be a VecHashJoin, and why not. */
const char *
vexec_orca_hashjoin_refusal(HashJoin *hj)
{
	Plan	   *hash = hj->join.plan.righttree;
	ListCell   *lc;

	if (!join_type_supported(hj->join.jointype))
		return psprintf("a %s join", join_type_name(hj->join.jointype));
	if (hash == NULL || !IsA(hash, Hash) || hash->lefttree == NULL)
		return "no Hash node over its inner side";
	if (hash->qual != NIL || hash->initPlan != NIL)
		return "a Hash node with conditions or InitPlans of its own";
	if (list_length(hash->targetlist) != list_length(hash->lefttree->targetlist))
		return "a Hash node that projects";
	if (hj->hashclauses == NIL || hj->hashkeys == NIL ||
		list_length(hj->hashkeys) != list_length(((Hash *) hash)->hashkeys) ||
		list_length(hj->hashkeys) != list_length(hj->hashoperators))
		return "no hash keys";
	foreach(lc, hj->hashoperators)
		if (!op_strict(lfirst_oid(lc)))
			return "a hash operator that is not strict";
	if (hj->join.plan.lefttree == NULL)
		return "no outer side";
	return NULL;
}

typedef struct OrcaJoinContext
{
	int			nouter;
	bool		hash_keys;		/* the Hash node's: OUTER_VAR is its input,
								 * the inner side (setrefs.c,
								 * set_hash_references) */
} OrcaJoinContext;

/* The HashJoin's references to its sides, as references to the join row. */
static Node *
orca_join_mutator(Node *node, OrcaJoinContext *ctx)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == OUTER_VAR || var->varno == INNER_VAR)
		{
			Var		   *v = copyObject(var);
			bool		inner = var->varno == INNER_VAR || ctx->hash_keys;

			v->varno = INDEX_VAR;
			if (inner && var->varattno > 0)
				v->varattno = ctx->nouter + var->varattno;
			return (Node *) v;
		}
		return node;
	}
	return expression_tree_mutator(node, orca_join_mutator, ctx);
}

static Node *
orca_join_expr(Node *node, int nouter, bool hash_keys)
{
	OrcaJoinContext ctx = {nouter, hash_keys};

	return orca_join_mutator(node, &ctx);
}

/*
 * VecHashJoin in place of the HashJoin ORCA's translator built: its outer
 * side as it is, its Hash node's input as its inner side, the HashJoin's
 * plan node id, costs and parameters kept.  The Hash node goes: the
 * VecHashJoin is both.
 */
Plan *
vexec_build_hashjoin_from_hashjoin(HashJoin *hj)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *outer = hj->join.plan.lefttree;
	Hash	   *hash = (Hash *) hj->join.plan.righttree;
	Plan	   *inner = hash->plan.lefttree;
	VexecJoinPlan plan;
	List	   *scan_tlist = NIL;
	int			nouter = list_length(outer->targetlist);

	memset(&plan, 0, sizeof(plan));
	plan.jointype = hj->join.jointype;
	plan.inner_unique = hj->join.inner_unique;
	plan.nouter = nouter;
	plan.ninner = list_length(inner->targetlist);
	plan.hashoperators = hj->hashoperators;
	plan.hashcollations = hj->hashcollations;
	plan.inner_rows = inner->plan_rows;

	scan_tlist = side_entries(scan_tlist, outer->targetlist, OUTER_VAR);
	scan_tlist = side_entries(scan_tlist, inner->targetlist, INNER_VAR);

	cscan->scan.plan = hj->join.plan;
	cscan->scan.plan.type = T_CustomScan;
	cscan->scan.plan.targetlist = (List *) orca_join_expr((Node *) hj->join.plan.targetlist,
														  nouter, false);
	cscan->scan.plan.qual = (List *) orca_join_expr((Node *) hj->join.plan.qual, nouter, false);
	cscan->scan.plan.lefttree = outer;
	cscan->scan.plan.righttree = inner;
	cscan->scan.scanrelid = 0;
	cscan->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cscan->custom_plans = NIL;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_exprs = list_make4(orca_join_expr((Node *) hj->hashclauses, nouter, false),
									 orca_join_expr((Node *) hj->join.joinqual, nouter, false),
									 orca_join_expr((Node *) hj->hashkeys, nouter, false),
									 orca_join_expr((Node *) hash->hashkeys, nouter, true));
	cscan->custom_private = vexec_join_plan_encode(&plan);
	cscan->methods = vexec_hashjoin_methods();
	return &cscan->scan.plan;
}
