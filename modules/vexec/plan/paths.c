/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * paths.c
 *	  PostgreSQL's planner: the statement's gates and the vector
 *	  alternatives, at the hooks it already has (pg_vector_executor.md
 *	  §3.3.3).
 *
 *	planner_setup_hook		the statement's gates, into the planner's
 *							global extension state
 *	set_rel_pathlist_hook	VecScan, for a table's sequential scan
 *	set_join_pathlist_hook	VecHashJoin, for a hash-joinable join
 *	create_upper_paths_hook	VecAgg (GROUP_AGG), VecSort (ORDERED)
 *	planner_shutdown_hook	the plan check, and the reasons into the plan
 *
 * With vexec.mode = off, every hook calls the one it took the place of and
 * adds nothing, so plans are PostgreSQL's (§1.2).  In explain mode the
 * alternatives are costed and recorded but never added, so plans are
 * PostgreSQL's still.  In auto mode a VecScan path, and VecAgg's paths of a
 * grouped relation (agg.c), compete with the relation's other paths in
 * add_path, by cost; in force mode they replace them, wherever the oracle
 * accepts them.  The join and sort alternatives are still only costed and
 * recorded: their nodes come in V3 and V4.
 *
 * The scan hook works before it calls the hook it took the place of, so
 * that on a cluster's coordinator gp_core's hook, which empties the path
 * list of a table whose rows are on the segments, removes what it adds
 * (pg19/modules/gp_core/gp_scan.c:1668-1708).
 *
 * Traps (from the Route B research): add_path frees the paths it rejects,
 * so nothing is keyed by a Path *; GEQO builds join relations in a
 * short-lived context, so records go into the planning context; the join
 * hook's extra and its sjinfo do not outlive the call.
 *
 * ORCA plans reach none of these hooks: gp_orca's planner_hook returns its
 * plan without standard_planner (§2.1).  Its front end is the gp_orca API,
 * from V1.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "commands/explain_state.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/extendplan.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "vexec.h"
#include "plan/plan.h"
#include "source/source.h"

static int	planner_id = -1;

/* "1 hash clause", "2 hash clauses" */
static const char *
count_of(int n, const char *one, const char *many)
{
	return psprintf("%d %s", n, n == 1 ? one : many);
}

static planner_setup_hook_type prev_planner_setup = NULL;
static planner_shutdown_hook_type prev_planner_shutdown = NULL;
static set_rel_pathlist_hook_type prev_set_rel_pathlist = NULL;
static set_join_pathlist_hook_type prev_set_join_pathlist = NULL;
static create_upper_paths_hook_type prev_create_upper_paths = NULL;

static void vexec_planner_setup(PlannerGlobal *glob, Query *parse,
								const char *query_string, int cursorOptions,
								double *tuple_fraction, ExplainState *es);
static void vexec_planner_shutdown(PlannerGlobal *glob, Query *parse,
								   const char *query_string, PlannedStmt *pstmt);
static void vexec_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
								   Index rti, RangeTblEntry *rte);
static void vexec_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
									RelOptInfo *outerrel, RelOptInfo *innerrel,
									JoinType jointype, JoinPathExtraData *extra);
static void vexec_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
									 RelOptInfo *input_rel, RelOptInfo *output_rel,
									 void *extra);

void
vexec_planner_install(void)
{
	planner_id = GetPlannerExtensionId("vexec");

	prev_planner_setup = planner_setup_hook;
	planner_setup_hook = vexec_planner_setup;
	prev_planner_shutdown = planner_shutdown_hook;
	planner_shutdown_hook = vexec_planner_shutdown;
	prev_set_rel_pathlist = set_rel_pathlist_hook;
	set_rel_pathlist_hook = vexec_set_rel_pathlist;
	prev_set_join_pathlist = set_join_pathlist_hook;
	set_join_pathlist_hook = vexec_set_join_pathlist;
	prev_create_upper_paths = create_upper_paths_hook;
	create_upper_paths_hook = vexec_create_upper_paths;
}

/* The statement's state, or NULL: vexec is off, or the gates are closed. */
VexecPlanState *
vexec_plan_state(PlannerInfo *root)
{
	VexecPlanState *ps;

	if (planner_id < 0)
		return NULL;
	ps = GetPlannerGlobalExtensionState(root->glob, planner_id);
	if (ps == NULL || !ps->gate_open)
		return NULL;
	return ps;
}

/*
 * The statement's gates (§3.3.3, §3.3.7).  A statement gets no vector
 * alternative where EvalPlanQual, WHERE CURRENT OF or row locks need a row
 * node:
 *
 *	- row marks: EvalPlanQual re-fetches the row into the scan node's slot
 *	  through the access method (PG19:src/backend/executor/execMain.c:2914-2918);
 *	- UPDATE, DELETE, MERGE and INSERT ... ON CONFLICT DO UPDATE: their
 *	  plans are below ModifyTable, whose EvalPlanQual needs the same;
 *	- a cursor (CURSOR_OPT_FAST_PLAN, which DECLARE and PL/pgSQL's cursors
 *	  set): WHERE CURRENT OF reads a scan's slot through Result, Limit,
 *	  SubqueryScan and Append (PG19:src/backend/executor/execCurrent.c:
 *	  319-400), and which of a cursor's plans could be its target is not
 *	  known while it is planned.
 */
const char *
vexec_statement_gate(Query *parse, int cursorOptions)
{
	if (parse->rowMarks != NIL)
		return "row marks: FOR UPDATE or FOR SHARE";
	if (parse->hasModifyingCTE)
		return "a data-modifying WITH, below whose ModifyTable EvalPlanQual runs";
	switch (parse->commandType)
	{
		case CMD_UPDATE:
			return "UPDATE";
		case CMD_DELETE:
			return "DELETE";
		case CMD_MERGE:
			return "MERGE";
		case CMD_INSERT:
			if (parse->onConflict != NULL &&
				parse->onConflict->action == ONCONFLICT_UPDATE)
				return "INSERT ... ON CONFLICT DO UPDATE";
			break;
		default:
			break;
	}
	if (cursorOptions & CURSOR_OPT_FAST_PLAN)
		return "a cursor, whose scan WHERE CURRENT OF may read";
	return NULL;
}

static void
vexec_planner_setup(PlannerGlobal *glob, Query *parse, const char *query_string,
					int cursorOptions, double *tuple_fraction, ExplainState *es)
{
	if (vexec_mode != VEXEC_MODE_OFF)
	{
		VexecPlanState *ps = palloc0(sizeof(VexecPlanState));

		ps->mcxt = CurrentMemoryContext;
		ps->mode = vexec_mode;
		ps->gate_reason = vexec_statement_gate(parse, cursorOptions);
		ps->gate_open = ps->gate_reason == NULL;
		ps->record = vexec_mode == VEXEC_MODE_EXPLAIN || vexec_explain_requested(es);
		ps->layout = vexec_layout_config();
		SetPlannerGlobalExtensionState(glob, planner_id, ps);
	}

	if (prev_planner_setup)
		prev_planner_setup(glob, parse, query_string, cursorOptions, tuple_fraction, es);
}

static void
vexec_planner_shutdown(PlannerGlobal *glob, Query *parse, const char *query_string,
					   PlannedStmt *pstmt)
{
	VexecPlanState *ps = planner_id >= 0 ? GetPlannerGlobalExtensionState(glob, planner_id) : NULL;
	int			nodes = -1;

	if (vexec_debug_check_plans || vexec_debug_require_vector)
		nodes = vexec_check_plan(pstmt, ps ? ps->mode : VEXEC_MODE_OFF);

	if (ps != NULL && ps->record)
		pstmt->extension_state = lappend(pstmt->extension_state,
										 makeDefElem("vexec", vexec_reasons_node(ps), -1));

	if (vexec_debug_require_vector && ps != NULL && ps->gate_open &&
		(ps->mode == VEXEC_MODE_AUTO || ps->mode == VEXEC_MODE_FORCE) &&
		ps->npossible > 0 && nodes == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the plan has no vector node"),
				 errdetail("vexec.debug_require_vector is on."),
				 errhint("EXPLAIN (VEXEC) shows each vector alternative, and why it was not taken.")));

	if (prev_planner_shutdown)
		prev_planner_shutdown(glob, parse, query_string, pstmt);
}

/* The attribute numbers a relation's scan needs: its quals' and its target's. */
static Bitmapset *
needed_attrs(RelOptInfo *rel)
{
	Bitmapset  *attrs = NULL;
	ListCell   *lc;

	pull_varattnos((Node *) rel->reltarget->exprs, rel->relid, &attrs);
	foreach(lc, rel->baserestrictinfo)
		pull_varattnos((Node *) ((RestrictInfo *) lfirst(lc))->clause, rel->relid, &attrs);
	return attrs;
}

/*
 * A system column the scan cannot give: VecScan gives ctid, from each row's
 * TID, and tableoid; xmin and its kind are heap's tuple header.
 */
static bool
reads_other_system_column(Bitmapset *attrs)
{
	int			i = -1;

	while ((i = bms_next_member(attrs, i)) >= 0)
	{
		AttrNumber	attno = i + FirstLowInvalidHeapAttributeNumber;

		if (attno < 0 && attno != SelfItemPointerAttributeNumber &&
			attno != TableOidAttributeNumber)
			return true;
	}
	return false;
}

/*
 * A table's sequential scan, and its vector alternative (§3.3.3, Scans):
 * for a base relation or an appendrel child that is a table, whose access
 * method has a source or the slot path.  A partitioned parent gets none:
 * its paths are rebuilt from its children's, which get theirs.  In auto
 * and force mode the VecScan path is added; in explain mode, and for
 * EXPLAIN (VEXEC), it is recorded with its cost or its refusal.
 */
static void
consider_scan(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte,
			  VexecPlanState *ps)
{
	VexecAlt   *alt = NULL;
	Relation	relation;
	const VexecSourceRoutine *src;
	const char *how;
	VexecSteps	quals;
	VexecSteps	target;
	Path	   *rowpath;
	VexecCost	cost;
	double		source_bytes = -1;
	Bitmapset  *attrs;
	Relids		required_outer;
	bool		add = ps->mode == VEXEC_MODE_AUTO || ps->mode == VEXEC_MODE_FORCE;
	const char *refusal = NULL;

	if (rte->rtekind != RTE_RELATION || rte->inh)
		return;					/* not a table's own scan */
	if (rel->reloptkind != RELOPT_BASEREL && rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		return;
	if (IS_DUMMY_REL(rel))
		return;					/* proved empty: nothing to scan */

	if (ps->record)
		alt = vexec_alt_record(ps, "VecScan", vexec_relids_names(root, rel->relids), root, NULL);
	if (alt == NULL && !add)
		return;

	attrs = needed_attrs(rel);
	if (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_MATVIEW)
		refusal = rte->relkind == RELKIND_FOREIGN_TABLE ? "a foreign table" : "not a table";
	else if (rte->tablesample != NULL)
		refusal = "TABLESAMPLE";
	else if (root->rowMarks != NIL)
		refusal = "row marks at its query level: FOR UPDATE or FOR SHARE";
	else if (!vexec_enable_scan)
		refusal = "vexec.enable_scan is off";
	else if ((rel->pgs_mask & PGS_SEQSCAN) == 0)
		refusal = "sequential scans are disabled";
	else if (ps->mode != VEXEC_MODE_FORCE && rel->tuples < vexec_min_rows)
		refusal = psprintf("%.0f rows, fewer than vexec.min_rows", rel->tuples);
	else if (reads_other_system_column(attrs))
		refusal = "a system column other than ctid and tableoid";
	if (refusal != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return;
	}

	memset(&quals, 0, sizeof(quals));
	memset(&target, 0, sizeof(target));
	vexec_oracle_exprs(root, rel->baserestrictinfo, &quals);
	vexec_oracle_exprs(root, rel->reltarget->exprs, &target);
	if (quals.refusal || target.refusal)
	{
		vexec_alt_refuse(ps, alt, quals.refusal ? quals.refusal : target.refusal);
		return;
	}

	relation = table_open(rte->relid, NoLock);
	src = vexec_source_for(relation, &how);
	if (src == NULL && relation->rd_tableam == NULL)
	{
		table_close(relation, NoLock);
		vexec_alt_refuse(ps, alt, how);
		return;
	}
	table_close(relation, NoLock);

	/*
	 * A relation with lateral references has only paths parameterized by
	 * them: a VecScan of it is rescanned for each outer row.
	 */
	required_outer = rel->lateral_relids;
	rowpath = create_seqscan_path(root, rel, required_outer, 0);
	vexec_cost_scan(root, rel, rowpath, &quals, &target,
					bms_num_members(attrs), list_length(rel->reltarget->exprs),
					source_bytes, &cost);
	vexec_alt_costed(ps, alt, &cost,
					 psprintf("source: %s; quals: %s, %s; target: %s, %s",
							  how,
							  count_of(quals.kernel, "kernel step", "kernel steps"),
							  count_of(quals.fallback, "fallback step", "fallback steps"),
							  count_of(target.kernel, "kernel step", "kernel steps"),
							  count_of(target.fallback, "fallback step", "fallback steps")));
	if (add)
	{
		Path	   *path = vexec_scan_path(root, rel, rowpath, &cost);

		/*
		 * Force mode: wherever the oracle accepts it, the vector scan is the
		 * relation's scan.  Its partial paths stay, since V1 has no
		 * parallel-aware vector scan (V4): where the planner makes a
		 * parallel plan, as the tests that force one ask, it keeps it.
		 */
		if (ps->mode == VEXEC_MODE_FORCE)
			rel->pathlist = NIL;
		add_path(rel, path);
		ps->npossible++;
	}
}

static void
vexec_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	VexecPlanState *ps = vexec_plan_state(root);

	if (ps != NULL)
		consider_scan(root, rel, rti, rte, ps);

	if (prev_set_rel_pathlist)
		prev_set_rel_pathlist(root, rel, rti, rte);
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

/*
 * A hash join's vector alternative (§3.3.3, Hash joins): inner, left,
 * semi, anti and right joins with hash-joinable clauses, where the join's
 * strategy mask allows a hash join, over the cheapest unparameterized paths
 * of the two sides.  Nothing from extra is kept.
 */
static void
consider_hashjoin(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel,
				  RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra,
				  VexecPlanState *ps)
{
	VexecAlt   *alt;
	List	   *clauses;
	Path	   *outer = outerrel->cheapest_total_path;
	Path	   *inner = innerrel->cheapest_total_path;
	JoinCostWorkspace workspace;
	HashPath   *rowpath;
	VexecCost	cost;
	VexecSteps	steps;
	ListCell   *lc;

	alt = vexec_alt_record(ps, "VecHashJoin", vexec_relids_names(root, joinrel->relids),
						   root, joinrel->relids);
	if (alt == NULL)
		return;

	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_SEMI:
		case JOIN_ANTI:
		case JOIN_RIGHT:
			break;
		default:
			vexec_alt_refuse(ps, alt, psprintf("a %s join", join_type_name(jointype)));
			return;
	}
	if (!vexec_enable_hashjoin)
	{
		vexec_alt_refuse(ps, alt, "vexec.enable_hashjoin is off");
		return;
	}
	if ((extra->pgs_mask & PGS_HASHJOIN) == 0)
	{
		vexec_alt_refuse(ps, alt, "hash joins are disabled");
		return;
	}
	if (outer == NULL || inner == NULL ||
		PATH_REQ_OUTER(outer) != NULL || PATH_REQ_OUTER(inner) != NULL)
	{
		vexec_alt_refuse(ps, alt, "a parameterized input");
		return;
	}
	if (ps->mode != VEXEC_MODE_FORCE &&
		Max(outer->rows, inner->rows) < vexec_min_rows)
	{
		vexec_alt_refuse(ps, alt, psprintf("%.0f rows at most a side, fewer than vexec.min_rows",
										   Max(outer->rows, inner->rows)));
		return;
	}
	clauses = hash_clauses(joinrel, outerrel, innerrel, jointype, extra);
	if (clauses == NIL)
	{
		vexec_alt_refuse(ps, alt, "no hash-joinable clause");
		return;
	}
	memset(&steps, 0, sizeof(steps));
	vexec_oracle_exprs(root, extra->restrictlist, &steps);
	if (steps.refusal)
	{
		vexec_alt_refuse(ps, alt, steps.refusal);
		return;
	}

	/*
	 * PostgreSQL's own hash join for these inputs.  final_cost_hashjoin()
	 * caches each hash clause's bucket size and MCV frequency in its
	 * RestrictInfo, computed for the pair it costs (costsize.c:4510-4541),
	 * and core reads them back for every later pair.  Core may never have
	 * costed this pair -- add_path_precheck() turned it down -- so the
	 * cache is put back as it was, and recording an alternative changes no
	 * later cost and no plan.
	 */
	initial_cost_hashjoin(root, &workspace, jointype, clauses, outer, inner, extra, false);
	{
		int			n = list_length(clauses);
		Selectivity *saved = palloc(sizeof(Selectivity) * 4 * Max(n, 1));
		int			i = 0;

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
	}
	{
		int			nkernel = 0;

		foreach(lc, clauses)
		{
			RestrictInfo *rinfo = lfirst(lc);
			OpExpr	   *op = castNode(OpExpr, rinfo->clause);

			set_opfuncid(op);
			if (vexec_kernel_bound(op->opfuncid, exprType(linitial(op->args)), op->inputcollid))
				nkernel++;
		}
		vexec_cost_hashjoin(root, &rowpath->jpath.path, outer, inner,
							list_length(clauses), nkernel, &cost);
		vexec_alt_costed(ps, alt, &cost,
						 psprintf("%s join, outer %s, inner %s; %s, %d with kernels",
								  join_type_name(jointype),
								  vexec_relids_names(root, outerrel->relids),
								  vexec_relids_names(root, innerrel->relids),
								  count_of(list_length(clauses), "hash clause", "hash clauses"),
								  nkernel));
	}
}

static void
vexec_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel,
						RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra)
{
	VexecPlanState *ps = vexec_plan_state(root);

	if (ps != NULL && ps->record)
		consider_hashjoin(root, joinrel, outerrel, innerrel, jointype, extra, ps);

	if (prev_set_join_pathlist)
		prev_set_join_pathlist(root, joinrel, outerrel, innerrel, jointype, extra);
}

/* An upper stage, named as a subquery's when it is one. */
static const char *
upper_target(PlannerInfo *root, const char *stage)
{
	return root->plan_name ? psprintf("%s (%s)", stage, root->plan_name) : stage;
}

/* A sort's vector alternative (§3.3.3, UPPERREL_ORDERED): V4's node. */
static void
consider_sort(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
			  VexecPlanState *ps)
{
	VexecAlt   *alt;
	Path	   *input = input_rel->cheapest_total_path;
	SortPath   *rowpath;
	VexecCost	cost;

	if (root->sort_pathkeys == NIL)
		return;
	alt = vexec_alt_record(ps, "VecSort", upper_target(root, "ORDER BY"), root, NULL);
	if (alt == NULL)
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
	if (ps->mode != VEXEC_MODE_FORCE && input->rows < vexec_min_rows)
	{
		vexec_alt_refuse(ps, alt, psprintf("%.0f input rows, fewer than vexec.min_rows", input->rows));
		return;
	}
	rowpath = create_sort_path(root, output_rel, input, root->sort_pathkeys, root->limit_tuples);
	vexec_cost_sort(root, &rowpath->path, input, &cost);
	vexec_alt_costed(ps, alt, &cost,
					 psprintf("%s%s",
							  count_of(list_length(root->sort_pathkeys), "sort key", "sort keys"),
							  root->limit_tuples > 0 ? ", bounded" : ""));
}

static void
vexec_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
						 RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	VexecPlanState *ps = vexec_plan_state(root);

	if (ps != NULL && stage == UPPERREL_GROUP_AGG)
		vexec_consider_agg(root, input_rel, output_rel, (GroupPathExtraData *) extra, ps,
						   upper_target(root, root->processed_groupClause ? "GROUP BY" : "aggregates"));
	else if (ps != NULL && ps->record && stage == UPPERREL_ORDERED)
		consider_sort(root, input_rel, output_rel, ps);

	if (prev_create_upper_paths)
		prev_create_upper_paths(root, stage, input_rel, output_rel, extra);
}
