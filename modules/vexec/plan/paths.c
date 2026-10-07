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
 *	set_rel_pathlist_hook	VecScan, for a table's sequential scan; VecIngest,
 *							for vexec.ingest_stream()'s function scan
 *							(ingest.c)
 *	set_join_pathlist_hook	VecHashJoin, for a hash-joinable join (join.c)
 *	create_upper_paths_hook	VecAgg (GROUP_AGG, agg.c), VecSort (ORDERED,
 *							sort.c), VecInsert (FINAL, insert.c)
 *	planner_shutdown_hook	the plan check, and the reasons into the plan
 *
 * With vexec.mode = off, every hook calls the one it took the place of and
 * adds nothing, so plans are PostgreSQL's (§1.2).  In explain mode the
 * alternatives are costed and recorded but never added, so plans are
 * PostgreSQL's still.  In auto mode a VecScan path -- and its partial path,
 * for a parallel plan (V4) -- VecHashJoin's paths of a join relation
 * (join.c), VecAgg's paths of a grouped relation (agg.c) and VecSort's
 * path of the ordered relation (sort.c) compete with the relation's other
 * paths in add_path, by cost; in force mode they replace them, wherever
 * the oracle accepts them.
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

/* ", 1 declared call": kernel packs' declarations (§3.17), where there are */
static const char *
declared_of(int n)
{
	return n == 0 ? "" : psprintf(", %s", count_of(n, "declared call", "declared calls"));
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

/* vexec's planner extension id, for state kept beside the planner's. */
int
vexec_planner_extension_id(void)
{
	return planner_id;
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

	/*
	 * VecHashJoin's and VecSort's scan tuples, in the form ORCA's plans have
	 * them (join.c)
	 */
	if (ps != NULL && (ps->joins_built || ps->sorts_built))
		vexec_join_finish_plan(pstmt);
	if (ps != NULL)
		vexec_insert_finish_plan(pstmt);

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
	List	   *bitmap_paths = NIL;
	ListCell   *lc;

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
	if (src == NULL && vexec_heap_page_reader && vexec_heap_reader_possible(relation))
		how = "heap's pages";
	table_close(relation, NoLock);

	/*
	 * H9: beside each of the relation's bitmap heap paths, a
	 * VecBitmapHeapScan over the same bitmap, parameterized as it is -- not
	 * a parallel one, whose shared iterator stays PostgreSQL's.  Made before
	 * force mode empties the relation's paths.
	 */
	if (add && vexec_enable_bitmapscan)
	{
		foreach(lc, rel->pathlist)
		{
			Path	   *bp = lfirst(lc);

			if (IsA(bp, BitmapHeapPath) && !bp->parallel_aware)
				bitmap_paths = lappend(bitmap_paths,
									   vexec_bitmapscan_path(root, rel, (BitmapHeapPath *) bp,
															 &quals, &target));
		}
	}

	/*
	 * The sequential scan's own gates.  A bitmap heap scan's are its own,
	 * which core's bitmap heap paths passed: they stand beside it as their
	 * twins stand, in force mode in their place.
	 */
	if (!vexec_enable_scan)
		refusal = "vexec.enable_scan is off";
	else if ((rel->pgs_mask & PGS_SEQSCAN) == 0)
		refusal = "sequential scans are disabled";
	if (refusal != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		if (bitmap_paths != NIL)
		{
			if (ps->mode == VEXEC_MODE_FORCE)
			{
				List	   *keep = NIL;

				foreach(lc, rel->pathlist)
					if (!IsA(lfirst(lc), BitmapHeapPath) || ((Path *) lfirst(lc))->parallel_aware)
						keep = lappend(keep, lfirst(lc));
				rel->pathlist = keep;
			}
			foreach(lc, bitmap_paths)
				add_path(rel, lfirst(lc));
			ps->npossible++;
		}
		return;
	}

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
					 psprintf("source: %s; quals: %s, %s%s; target: %s, %s%s",
							  how,
							  count_of(quals.kernel, "kernel step", "kernel steps"),
							  count_of(quals.fallback, "fallback step", "fallback steps"),
							  declared_of(quals.declared),
							  count_of(target.kernel, "kernel step", "kernel steps"),
							  count_of(target.fallback, "fallback step", "fallback steps"),
							  declared_of(target.declared)));
	if (add)
	{
		Path	   *path = vexec_scan_path(root, rel, rowpath, &cost);
		bool		index_kept = false;

		/*
		 * Force mode: wherever the oracle accepts it, the vector scan is the
		 * relation's scan -- but for the paths an index orders by a distance
		 * (indexorderbys), a nearest-neighbour search's, which stay for cost
		 * to choose between them and a VecSort over the vector scan
		 * (plan/sort.c): a segment planning the search gp_core sends it keeps
		 * its HNSW, IVFFlat or GiST index.  Where one stays the plan may hold
		 * no vector node, and the vector scan is not counted possible
		 * (vexec.debug_require_vector).
		 */
		if (ps->mode == VEXEC_MODE_FORCE)
		{
			List	   *keep = NIL;

			foreach(lc, rel->pathlist)
				if (IsA(lfirst(lc), IndexPath) &&
					((IndexPath *) lfirst(lc))->indexorderbys != NIL)
					keep = lappend(keep, lfirst(lc));
			rel->pathlist = keep;
			index_kept = keep != NIL;
		}
		add_path(rel, path);
		foreach(lc, bitmap_paths)
			add_path(rel, lfirst(lc));
		if (!index_kept)
			ps->npossible++;

		/*
		 * And a partial path, where the relation may be scanned in parallel
		 * (V4): a parallel-aware VecScan, of the workers
		 * create_plain_partial_paths() gives the parallel sequential scan
		 * (PG19:src/backend/optimizer/path/allpaths.c), for a Gather, a
		 * Gather Merge or a partial VecAgg above it.  In force mode it is the
		 * relation's only partial path, so that a plan the planner makes
		 * parallel -- as tests that force one ask -- stays parallel.
		 */
		if (rel->consider_parallel && required_outer == NULL)
		{
			int			workers = compute_parallel_worker(rel, rel->pages, -1,
														  max_parallel_workers_per_gather);

			if (workers > 0)
			{
				Path	   *prowpath = create_seqscan_path(root, rel, NULL, workers);
				VexecCost	pcost;

				vexec_cost_scan(root, rel, prowpath, &quals, &target,
								bms_num_members(attrs), list_length(rel->reltarget->exprs),
								source_bytes, &pcost);
				if (ps->mode == VEXEC_MODE_FORCE)
					rel->partial_pathlist = NIL;
				add_partial_path(rel, vexec_scan_path(root, rel, prowpath, &pcost));
			}
		}
	}
}

static void
vexec_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	VexecPlanState *ps = vexec_plan_state(root);

	if (ps != NULL && rte->rtekind == RTE_FUNCTION)
		vexec_consider_ingest(root, rel, rte, ps);
	else if (ps != NULL)
		consider_scan(root, rel, rti, rte, ps);

	if (prev_set_rel_pathlist)
		prev_set_rel_pathlist(root, rel, rti, rte);
}

static void
vexec_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outerrel,
						RelOptInfo *innerrel, JoinType jointype, JoinPathExtraData *extra)
{
	VexecPlanState *ps = vexec_plan_state(root);

	if (ps != NULL)
		vexec_consider_hashjoin(root, joinrel, outerrel, innerrel, jointype, extra, ps);

	if (prev_set_join_pathlist)
		prev_set_join_pathlist(root, joinrel, outerrel, innerrel, jointype, extra);
}

/* An upper stage, named as a subquery's when it is one. */
static const char *
upper_target(PlannerInfo *root, const char *stage)
{
	return root->plan_name ? psprintf("%s (%s)", stage, root->plan_name) : stage;
}

static void
vexec_create_upper_paths(PlannerInfo *root, UpperRelationKind stage,
						 RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	VexecPlanState *ps = vexec_plan_state(root);

	if (ps != NULL && stage == UPPERREL_GROUP_AGG)
		vexec_consider_agg(root, input_rel, output_rel, (GroupPathExtraData *) extra, ps,
						   upper_target(root, root->processed_groupClause ? "GROUP BY" : "aggregates"));
	else if (ps != NULL && stage == UPPERREL_ORDERED)
		vexec_consider_sort(root, input_rel, output_rel, ps, upper_target(root, "ORDER BY"));
	else if (ps != NULL && stage == UPPERREL_FINAL)
		vexec_consider_insert(root, output_rel, ps);

	if (prev_create_upper_paths)
		prev_create_upper_paths(root, stage, input_rel, output_rel, extra);
}
