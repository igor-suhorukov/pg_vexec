/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * orca.c
 *	  ORCA's front end: vector nodes in ORCA's plans, through gp_orca's API
 *	  (pg_vector_executor.md §3.3.4; the port's pg19/include/gp_orca_vec.h,
 *	  of which pgxs/include holds a copy).
 *
 * ORCA's plans reach none of PostgreSQL's planner hooks (§2.1).  vexec
 * registers with gp_orca instead, which calls it around each statement ORCA
 * plans:
 *
 *	begin	the statement's gates, as planner_setup_hook applies them to
 *			PostgreSQL's planner (paths.c), with vexec.orca and vexec.mode;
 *	build	each node of the translated plan, children first, before its
 *			Motions are checked and its slice table made: a SeqScan becomes
 *			a VecScan, a Result over a vector node a VecResult, a plain or
 *			hashed Agg of any split a VecAgg (agg.c), a HashJoin with its
 *			Hash a VecHashJoin (join.c), and a Sort a VecSort (sort.c),
 *			where the oracle accepts them and the mode chooses them; a Limit
 *			of constants over a VecSort bounds it, and a merge join keeps
 *			its inner side's Sort, which it marks and restores;
 *	end		the plan check, the reasons for EXPLAIN (VEXEC), and
 *			vexec.debug_require_vector.
 *
 * In V1 ORCA's own search does not see vector prices: that is V5's
 * CCostModelVec.  Force mode builds a vector node wherever the oracle
 * accepts one; auto mode where vexec's cost model, in PostgreSQL's units,
 * prices the vector scan below the row scan (cost.c); explain mode records
 * them and builds none.
 *
 * A Result with a constant qual stays a row node: it is a gating Result,
 * one of gp_core's squelch points (pg19/modules/gp_core/gp_motion.c:
 * 2062-2079).  A Result over a row node stays one too: a vector node reads
 * no row child a batch ahead (exec/vecresult.c).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "gp_orca_vec.h"

#include "vexec.h"
#include "exec/aggtrans.h"
#include "exec/exec.h"
#include "plan/plan.h"
#include "source/source.h"

static void *orca_begin(Query *parse, int cursorOptions, struct ExplainState *es);
static Plan *orca_build(void *state, Plan *plan, List *rtable);
static void orca_end(void *state, PlannedStmt *stmt);
static bool orca_describe(Plan *plan, GpOrcaVecNode *vn);

static const GpOrcaVecRoutine orca_routine = {
	.size = sizeof(GpOrcaVecRoutine),
	.minor = GP_ORCA_VEC_MINOR,
	.name = "vexec",
	.begin_statement = orca_begin,
	.build_node = orca_build,
	.end_statement = orca_end,
	.describe_node = orca_describe,
};

void
vexec_orca_install(void)
{
	gp_orca_vec_register(&orca_routine);
}

static void *
orca_begin(Query *parse, int cursorOptions, struct ExplainState *es)
{
	VexecPlanState *ps;

	if (vexec_mode == VEXEC_MODE_OFF || !vexec_orca)
		return NULL;
	ps = palloc0(sizeof(VexecPlanState));
	ps->mcxt = CurrentMemoryContext;
	ps->mode = vexec_mode;
	ps->gate_reason = vexec_statement_gate(parse, cursorOptions);
	ps->gate_open = ps->gate_reason == NULL;
	ps->record = vexec_mode == VEXEC_MODE_EXPLAIN || vexec_explain_requested(es);
	ps->layout = vexec_layout_config();
	return ps;
}

/* Whether ORCA's node may be built: the mode, after the oracle accepted it. */
static bool
chosen(VexecPlanState *ps, const VexecCost *cost)
{
	switch (ps->mode)
	{
		case VEXEC_MODE_FORCE:
			return true;
		case VEXEC_MODE_AUTO:
			return cost == NULL || cost->total < cost->row_total;
		default:
			return false;
	}
}

/* A system column VecScan cannot give: other than ctid and tableoid. */
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

/* A SeqScan ORCA's translator built, and its VecScan. */
static Plan *
orca_scan(VexecPlanState *ps, SeqScan *seq, List *rtable)
{
	Scan	   *scan = &seq->scan;
	RangeTblEntry *rte;
	VexecAlt   *alt = NULL;
	Bitmapset  *attrs = NULL;
	VexecSteps	quals;
	VexecSteps	target;
	VexecCost	cost;
	Relation	rel;
	const char *how;
	const char *refusal = NULL;
	char		relkind = '\0';

	if (scan->scanrelid == 0 || scan->scanrelid > list_length(rtable))
		return NULL;
	rte = rt_fetch(scan->scanrelid, rtable);
	if (ps->record)
		alt = vexec_alt_record(ps, "VecScan", rte->eref ? rte->eref->aliasname : "?", NULL, NULL);

	pull_varattnos((Node *) scan->plan.targetlist, scan->scanrelid, &attrs);
	pull_varattnos((Node *) scan->plan.qual, scan->scanrelid, &attrs);
	/* ORCA's range table entries leave relkind unset: the catalog says */
	if (rte->rtekind == RTE_RELATION)
		relkind = get_rel_relkind(rte->relid);
	if (rte->rtekind != RTE_RELATION ||
		(relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW))
		refusal = "not a table";
	else if (rte->tablesample != NULL)
		refusal = "TABLESAMPLE";
	else if (!vexec_enable_scan)
		refusal = "vexec.enable_scan is off";
	else if (reads_other_system_column(attrs))
		refusal = "a system column other than ctid and tableoid";
	if (refusal != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return NULL;
	}

	memset(&quals, 0, sizeof(quals));
	memset(&target, 0, sizeof(target));
	vexec_oracle_exprs(NULL, scan->plan.qual, &quals);
	vexec_oracle_exprs(NULL, scan->plan.targetlist, &target);
	if (quals.refusal || target.refusal)
	{
		vexec_alt_refuse(ps, alt, quals.refusal ? quals.refusal : target.refusal);
		return NULL;
	}

	rel = table_open(rte->relid, NoLock);
	if (vexec_source_for(rel, &how) == NULL && vexec_heap_page_reader &&
		vexec_heap_reader_possible(rel))
		how = "heap's pages";
	if (ps->mode != VEXEC_MODE_FORCE && rel->rd_rel->reltuples < vexec_min_rows)
	{
		vexec_alt_refuse(ps, alt, psprintf("%.0f rows, fewer than vexec.min_rows",
										   rel->rd_rel->reltuples));
		table_close(rel, NoLock);
		return NULL;
	}
	vexec_cost_plan_scan(rel, &quals, &target, bms_num_members(attrs),
						 list_length(scan->plan.targetlist), scan->plan.plan_rows, &cost);
	table_close(rel, NoLock);
	vexec_alt_costed(ps, alt, &cost,
					 psprintf("ORCA's scan; source: %s; quals: %d kernel, %d fallback steps",
							  how, quals.kernel, quals.fallback));
	ps->npossible++;
	if (!chosen(ps, &cost))
		return NULL;
	return vexec_build_scan_from_seqscan(seq);
}

/*
 * A BitmapHeapScan ORCA's translator built, and its VecBitmapHeapScan
 * (H9): a heap table's pages through heap's page reader, any other
 * access method's rows through its bitmap callback.
 */
static Plan *
orca_bitmapscan(VexecPlanState *ps, BitmapHeapScan *bhs, List *rtable)
{
	Scan	   *scan = &bhs->scan;
	RangeTblEntry *rte;
	VexecAlt   *alt = NULL;
	Bitmapset  *attrs = NULL;
	VexecSteps	steps;
	VexecCost	cost;
	const char *refusal = NULL;

	if (scan->scanrelid == 0 || scan->scanrelid > list_length(rtable))
		return NULL;
	rte = rt_fetch(scan->scanrelid, rtable);
	if (ps->record)
		alt = vexec_alt_record(ps, "VecBitmapHeapScan", rte->eref ? rte->eref->aliasname : "?",
							   NULL, NULL);
	pull_varattnos((Node *) scan->plan.targetlist, scan->scanrelid, &attrs);
	pull_varattnos((Node *) scan->plan.qual, scan->scanrelid, &attrs);
	pull_varattnos((Node *) bhs->bitmapqualorig, scan->scanrelid, &attrs);
	if (!vexec_enable_bitmapscan)
		refusal = "vexec.enable_bitmapscan is off";
	else if (rte->rtekind != RTE_RELATION || get_rel_relkind(rte->relid) != RELKIND_RELATION)
		refusal = "not a table";
	else if (scan->plan.parallel_aware)
		refusal = "a parallel bitmap heap scan";
	else if (reads_other_system_column(attrs))
		refusal = "a system column other than ctid and tableoid";
	if (refusal != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return NULL;
	}
	memset(&steps, 0, sizeof(steps));
	vexec_oracle_exprs(NULL, scan->plan.qual, &steps);
	vexec_oracle_exprs(NULL, scan->plan.targetlist, &steps);
	vexec_oracle_exprs(NULL, bhs->bitmapqualorig, &steps);
	if (steps.refusal)
	{
		vexec_alt_refuse(ps, alt, steps.refusal);
		return NULL;
	}
	memset(&cost, 0, sizeof(cost));
	cost.rows = scan->plan.plan_rows;
	vexec_alt_costed(ps, alt, &cost, "ORCA's bitmap heap scan");
	ps->npossible++;
	if (!chosen(ps, NULL))
		return NULL;
	return vexec_build_bitmapscan_from_bitmapscan(bhs);
}

/* A Result over a vector node, and its VecResult. */
static Plan *
orca_result(VexecPlanState *ps, Result *result)
{
	VexecAlt   *alt = NULL;
	VexecSteps	steps;

	if (result->plan.lefttree == NULL || result->resconstantqual != NULL)
		return NULL;			/* a gating Result, or one with no input */
	if (!vexec_is_vector_node(result->plan.lefttree) ||
		((CustomScan *) result->plan.lefttree)->methods == vexec_agg_methods())
		return NULL;			/* a child handing up rows is not read a batch
								 * ahead */
	if (ps->record)
		alt = vexec_alt_record(ps, "VecResult", "ORCA's Result", NULL, NULL);
	memset(&steps, 0, sizeof(steps));
	vexec_oracle_exprs(NULL, result->plan.qual, &steps);
	vexec_oracle_exprs(NULL, result->plan.targetlist, &steps);
	if (steps.refusal)
	{
		vexec_alt_refuse(ps, alt, steps.refusal);
		return NULL;
	}
	if (alt != NULL)
	{
		VexecCost	cost;

		memset(&cost, 0, sizeof(cost));
		cost.rows = result->plan.plan_rows;
		vexec_alt_costed(ps, alt, &cost, "over a vector node");
	}
	ps->npossible++;
	if (!chosen(ps, NULL))
		return NULL;
	return vexec_build_result_from_result(result);
}

/* An Agg ORCA's translator built, and its VecAgg (§3.8). */
static Plan *
orca_agg(VexecPlanState *ps, Agg *agg)
{
	VexecAlt   *alt = NULL;
	VexecSteps	steps;
	VexecCost	cost;
	const char *refusal;
	int			naggs = 0;
	int			nvec = 0;
	ListCell   *lc;
	List	   *aggrefs = NIL;

	if (ps->record)
		alt = vexec_alt_record(ps, "VecAgg", agg->numCols > 0 ? "ORCA's GROUP BY" : "ORCA's aggregates",
							   NULL, NULL);
	if (!vexec_enable_agg)
	{
		vexec_alt_refuse(ps, alt, "vexec.enable_agg is off");
		return NULL;
	}
	if ((refusal = vexec_orca_agg_refusal(agg)) != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return NULL;
	}
	if (ps->mode != VEXEC_MODE_FORCE && agg->plan.lefttree->plan_rows < vexec_min_rows)
	{
		vexec_alt_refuse(ps, alt, psprintf("%.0f input rows, fewer than vexec.min_rows",
										   agg->plan.lefttree->plan_rows));
		return NULL;
	}

	/* the aggregates' arguments and FILTERs, as the oracle takes them */
	memset(&steps, 0, sizeof(steps));
	aggrefs = pull_var_clause((Node *) list_concat_copy(agg->plan.targetlist, agg->plan.qual),
							  PVC_INCLUDE_AGGREGATES | PVC_RECURSE_PLACEHOLDERS |
							  PVC_RECURSE_WINDOWFUNCS);
	foreach(lc, aggrefs)
	{
		Aggref	   *aggref = (Aggref *) lfirst(lc);
		Oid			argtype = InvalidOid;

		if (!IsA(aggref, Aggref))
			continue;
		naggs++;
		vexec_oracle_exprs(NULL, aggref->args, &steps);
		if (aggref->aggfilter)
			vexec_oracle_expr(NULL, (Node *) aggref->aggfilter, &steps);
		if (aggref->args != NIL)
			argtype = exprType((Node *) linitial_node(TargetEntry, aggref->args)->expr);
		if (vexec_agg_vectorized(aggref->aggfnoid, agg->aggsplit, argtype, NULL))
			nvec++;
	}
	if (steps.refusal)
	{
		vexec_alt_refuse(ps, alt, steps.refusal);
		return NULL;
	}
	vexec_cost_plan_agg(&agg->plan, agg->numCols, naggs, nvec,
						vexec_is_vector_node(agg->plan.lefttree), &cost);
	vexec_alt_costed(ps, alt, &cost,
					 psprintf("ORCA's %s aggregation, split %d; %d aggregates, %d with vector transitions",
							  agg->aggstrategy == AGG_HASHED ? "hashed" : "plain",
							  (int) agg->aggsplit, naggs, nvec));
	ps->npossible++;
	if (!chosen(ps, &cost))
		return NULL;
	return vexec_build_agg_from_agg(agg);
}

/*
 * A HashJoin ORCA's translator built, and its VecHashJoin (§3.8): the
 * HashJoin and its Hash in one node, offered after the Hash, which stays
 * ORCA's until the join takes it.
 */
static Plan *
orca_hashjoin(VexecPlanState *ps, HashJoin *hj)
{
	VexecAlt   *alt = NULL;
	VexecSteps	steps;
	VexecCost	cost;
	const char *refusal;
	Plan	   *outer = hj->join.plan.lefttree;
	Plan	   *inner;
	int			nkernel = 0;
	ListCell   *lc;

	if (ps->record)
		alt = vexec_alt_record(ps, "VecHashJoin", "ORCA's hash join", NULL, NULL);
	if (!vexec_enable_hashjoin)
	{
		vexec_alt_refuse(ps, alt, "vexec.enable_hashjoin is off");
		return NULL;
	}
	if ((refusal = vexec_orca_hashjoin_refusal(hj)) != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return NULL;
	}
	inner = hj->join.plan.righttree->lefttree;
	if (ps->mode != VEXEC_MODE_FORCE && Max(outer->plan_rows, inner->plan_rows) < vexec_min_rows)
	{
		vexec_alt_refuse(ps, alt, psprintf("%.0f rows at most a side, fewer than vexec.min_rows",
										   Max(outer->plan_rows, inner->plan_rows)));
		return NULL;
	}
	memset(&steps, 0, sizeof(steps));
	vexec_oracle_exprs(NULL, hj->join.plan.targetlist, &steps);
	vexec_oracle_exprs(NULL, hj->join.plan.qual, &steps);
	vexec_oracle_exprs(NULL, hj->join.joinqual, &steps);
	vexec_oracle_exprs(NULL, hj->hashclauses, &steps);
	vexec_oracle_exprs(NULL, hj->hashkeys, &steps);
	vexec_oracle_exprs(NULL, ((Hash *) hj->join.plan.righttree)->hashkeys, &steps);
	if (steps.refusal)
	{
		vexec_alt_refuse(ps, alt, steps.refusal);
		return NULL;
	}
	foreach(lc, hj->hashclauses)
	{
		OpExpr	   *op = (OpExpr *) lfirst(lc);

		if (!IsA(op, OpExpr) || list_length(op->args) != 2)
			continue;
		set_opfuncid(op);
		if (vexec_kernel_bound(op->opfuncid, exprType(linitial(op->args)), op->inputcollid))
			nkernel++;
	}
	vexec_cost_plan_hashjoin(&hj->join.plan, outer, inner, list_length(hj->hashclauses),
							 nkernel, &cost);
	vexec_alt_costed(ps, alt, &cost,
					 psprintf("ORCA's hash join, join type %d; %d hash clauses, %d with kernels",
							  (int) hj->join.jointype, list_length(hj->hashclauses), nkernel));
	ps->npossible++;
	if (!chosen(ps, &cost))
		return NULL;
	return vexec_build_hashjoin_from_hashjoin(hj);
}

/* A Sort ORCA's translator built, and its VecSort (§3.8). */
static Plan *
orca_sort(VexecPlanState *ps, Sort *sort)
{
	VexecAlt   *alt = NULL;
	VexecSteps	steps;
	VexecCost	cost;
	const char *refusal;
	Plan	   *child = sort->plan.lefttree;
	bool		child_batches;

	if (ps->record)
		alt = vexec_alt_record(ps, "VecSort", "ORCA's sort", NULL, NULL);
	if (!vexec_enable_sort)
	{
		vexec_alt_refuse(ps, alt, "vexec.enable_sort is off");
		return NULL;
	}
	if ((refusal = vexec_orca_sort_refusal(sort)) != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return NULL;
	}
	if (ps->mode != VEXEC_MODE_FORCE && child->plan_rows < vexec_min_rows)
	{
		vexec_alt_refuse(ps, alt, psprintf("%.0f input rows, fewer than vexec.min_rows",
										   child->plan_rows));
		return NULL;
	}
	memset(&steps, 0, sizeof(steps));
	vexec_oracle_exprs(NULL, sort->plan.targetlist, &steps);
	if (steps.refusal)
	{
		vexec_alt_refuse(ps, alt, steps.refusal);
		return NULL;
	}
	child_batches = vexec_is_vector_node(child) &&
		((CustomScan *) child)->methods != vexec_agg_methods();
	vexec_cost_plan_sort(&sort->plan, child_batches, &cost);
	vexec_alt_costed(ps, alt, &cost,
					 psprintf("ORCA's sort; %d sort keys, over %s", sort->numCols,
							  child_batches ? "a vector node's batches" : "rows"));
	ps->npossible++;
	if (!chosen(ps, &cost))
		return NULL;
	return vexec_build_sort_from_sort(sort);
}

/*
 * A merge join marks and restores its inner side (nodeMergejoin.c), which a
 * vector node does not do (§3.3.7): a VecSort the front end made of the
 * Sort there is made the Sort again.
 */
static void
orca_mergejoin(MergeJoin *mj)
{
	Plan	   *inner = mj->join.plan.righttree;

	if (inner != NULL && IsA(inner, CustomScan) &&
		((CustomScan *) inner)->methods == vexec_sort_methods())
		mj->join.plan.righttree = vexec_unbuild_sort((CustomScan *) inner);
}

static Plan *
orca_build(void *state, Plan *plan, List *rtable)
{
	VexecPlanState *ps = state;

	if (ps == NULL || !ps->gate_open)
		return NULL;
	switch (nodeTag(plan))
	{
		case T_SeqScan:
			return orca_scan(ps, (SeqScan *) plan, rtable);
		case T_Result:
			return orca_result(ps, (Result *) plan);
		case T_Agg:
			return orca_agg(ps, (Agg *) plan);
		case T_HashJoin:
			return orca_hashjoin(ps, (HashJoin *) plan);
		case T_Sort:
			return orca_sort(ps, (Sort *) plan);
		case T_BitmapHeapScan:
			return orca_bitmapscan(ps, (BitmapHeapScan *) plan, rtable);
		case T_Limit:
			vexec_orca_limit_bound((Limit *) plan);
			return NULL;
		case T_MergeJoin:
			orca_mergejoin((MergeJoin *) plan);
			return NULL;
		default:
			return NULL;
	}
}

/*
 * What a vector node stands for (gp_orca_vec.h, from its minor version 1),
 * for the port's passes over the finished plan that look at nodes by their
 * kind: M8's parallel.c, which puts Gathers over the scans a segment's
 * fragment can split among its workers, and their aggregations in three
 * stages (§3.10, V4); and gp_core's bound_gathers(), on PostgreSQL's
 * planner's plans too, which sends a nearest-neighbour search's ORDER BY and
 * LIMIT to the segments through the sort below a Limit.  A VecScan is a
 * sequential scan, which shares its table among a Gather's participants
 * once parallel-aware (exec/vecscan.c); a VecResult a projection; a
 * VecHashJoin a hash join, its inner side read whole by each participant;
 * a VecAgg the Agg it was made of; a VecSort the Sort of its keys over its
 * child.
 */
static bool
orca_describe(Plan *plan, GpOrcaVecNode *vn)
{
	CustomScan *cscan;

	if (!vexec_is_vector_node(plan))
		return false;
	cscan = (CustomScan *) plan;
	memset(vn, 0, sizeof(GpOrcaVecNode));
	if (cscan->methods == vexec_scan_methods())
		vn->kind = GP_ORCA_VEC_SEQSCAN;
	else if (cscan->methods == vexec_bitmapscan_methods())
		vn->kind = GP_ORCA_VEC_OTHER;	/* a scan of its bitmap's pages */
	else if (cscan->methods == vexec_result_methods())
		vn->kind = GP_ORCA_VEC_RESULT;
	else if (cscan->methods == vexec_hashjoin_methods())
	{
		VexecJoinPlan jp;

		vexec_join_plan_decode(cscan, &jp);
		vn->kind = GP_ORCA_VEC_HASHJOIN;
		vn->jointype = jp.jointype;
		vn->nhashclauses = list_length(jp.hashoperators);
		vn->exprs = cscan->custom_exprs;
	}
	else if (cscan->methods == vexec_agg_methods())
	{
		vn->kind = GP_ORCA_VEC_AGG;
		vn->agg = vexec_agg_describe(cscan);
	}
	else if (cscan->methods == vexec_sort_methods())
	{
		ListCell   *lc;
		int			i;

		vn->kind = GP_ORCA_VEC_SORT;
		vn->sort = vexec_sort_describe(cscan);
		/* its keys, the expressions it evaluates beyond its own */
		for (i = 0; i < vn->sort->numCols; i++)
			foreach(lc, vn->sort->plan.targetlist)
				if (lfirst_node(TargetEntry, lc)->resno == vn->sort->sortColIdx[i])
					vn->exprs = lappend(vn->exprs, lfirst_node(TargetEntry, lc)->expr);
	}
	else
		vn->kind = GP_ORCA_VEC_OTHER;
	return true;
}

/* ORCA's plan is made: the plan check, the reasons, the debug requirement. */
static void
orca_end(void *state, PlannedStmt *stmt)
{
	VexecPlanState *ps = state;
	int			nodes = -1;

	if (vexec_debug_check_plans || vexec_debug_require_vector)
		nodes = vexec_check_plan(stmt, ps->mode);
	if (ps->record)
		stmt->extension_state = lappend(stmt->extension_state,
										makeDefElem("vexec", vexec_reasons_node(ps), -1));
	if (vexec_debug_require_vector && ps->gate_open &&
		(ps->mode == VEXEC_MODE_AUTO || ps->mode == VEXEC_MODE_FORCE) &&
		ps->npossible > 0 && nodes == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("the plan has no vector node"),
				 errdetail("vexec.debug_require_vector is on."),
				 errhint("EXPLAIN (VEXEC) shows each vector alternative, and why it was not taken.")));
}
