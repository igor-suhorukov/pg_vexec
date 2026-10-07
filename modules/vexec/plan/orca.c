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
 *			its inner side's Sort, which it marks and restores; and from V7
 *			a Motion over a vector node carries its batches as frames,
 *			VecMotionSend below it and VecMotionRecv above it, the Motion
 *			rebuilt where it stands (motion.c);
 *	end		the plan check, the reasons for EXPLAIN (VEXEC), and
 *			vexec.debug_require_vector.
 *
 * From VI, build makes two more (§3.16): a FunctionScan of
 * vexec.ingest_stream() a VecIngest (ingest.c), and an INSERT's ModifyTable
 * a VecInsert (insert.c), in the segments' fragment that writes on a
 * cluster; end adds a partitioned target's partitions to the relations the
 * plan depends on.
 *
 * From V5, through the API's minor version 2, at four more:
 *
 *	set_options	before ORCA is asked: create_vectorization_plan, which
 *			offers ORCA's hashed window, where vexec builds windows and
 *			prices ORCA's search; and vexec.orca_settings, ORCA's settings
 *			for the statement alone;
 *	cost_factors and the cost oracle, during ORCA's search: the factors
 *			of vexec's cost model in ORCA's terms, and whether a call, an
 *			aggregate, a relation's scan and a key are vexec's, which
 *			CCostModelVec (the port's pg19/orca/cost/) prices ORCA's
 *			operators with (vexec.orca_cost_model);
 *	build_window	ORCA's hashed window, which the translator lowered to a
 *			WindowAgg over a Sort: VecWindowHashAgg in the Sort's place
 *			(window.c).
 *
 * Force mode builds a vector node wherever the oracle accepts one.  Auto
 * mode, where ORCA's search priced vexec's nodes, builds one wherever it
 * priced one: where the oracle accepts it, of vexec.min_rows rows at least.
 * Without those prices -- vexec.orca_cost_model off -- auto mode builds one
 * where vexec's own cost model, in PostgreSQL's units, prices it below the
 * row node (cost.c).  Explain mode records them and builds none, and leaves
 * ORCA's search its own prices, so that its plans stay the row plans.
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
#include "catalog/pg_aggregate.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "commands/defrem.h"
#include "optimizer/cost.h"
#include "utils/builtins.h"
#include "utils/varlena.h"
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
#include "motion/motion.h"
#include "plan/plan.h"
#include "source/source.h"

static void *orca_begin(Query *parse, int cursorOptions, struct ExplainState *es);
static Plan *orca_build(void *state, Plan *plan, List *rtable);
static void orca_end(void *state, PlannedStmt *stmt);
static bool orca_describe(Plan *plan, GpOrcaVecNode *vn);
static void orca_set_options(void *state, GpOrcaVecOptions *options);
static bool orca_cost_factors(void *state, GpOrcaVecCosts *costs);
static int	orca_cost_call(void *state, Oid funcid, Oid opno, int nargs,
						   const Oid *argtypes, Oid collation);
static int	orca_cost_aggregate(void *state, Oid aggfnoid, int nargs,
								const Oid *argtypes, bool distinct, bool ordered);
static int	orca_cost_relation(void *state, Oid relid);
static bool orca_cost_hash_key(void *state, Oid eqop, Oid collation);
static Plan *orca_build_window(void *state, WindowAgg *window, List *rtable);

static const GpOrcaVecRoutine orca_routine = {
	.size = sizeof(GpOrcaVecRoutine),
	.minor = GP_ORCA_VEC_MINOR,
	.name = "vexec",
	.begin_statement = orca_begin,
	.build_node = orca_build,
	.end_statement = orca_end,
	.describe_node = orca_describe,
	.set_options = orca_set_options,
	.cost_factors = orca_cost_factors,
	.cost_call = orca_cost_call,
	.cost_aggregate = orca_cost_aggregate,
	.cost_relation = orca_cost_relation,
	.cost_hash_key = orca_cost_hash_key,
	.build_window = orca_build_window,
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
	ps->parse = parse;
	ps->cursor_options = cursorOptions;
	return ps;
}

/*
 * Whether ORCA's node may be built: the mode, after the oracle accepted it.
 * In auto mode, where ORCA's search priced vexec's nodes, it chose its plan
 * with this node priced as vexec's: it is built, as priced.
 */
static bool
chosen(VexecPlanState *ps, const VexecCost *cost)
{
	switch (ps->mode)
	{
		case VEXEC_MODE_FORCE:
			return true;
		case VEXEC_MODE_AUTO:
			return ps->orca_costed || cost == NULL || cost->total < cost->row_total;
		default:
			return false;
	}
}

/* Whether vexec runs the statement's nodes: auto or force mode, its gates open. */
static bool
takes(VexecPlanState *ps)
{
	return ps != NULL && ps->gate_open &&
		(ps->mode == VEXEC_MODE_AUTO || ps->mode == VEXEC_MODE_FORCE);
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
					 psprintf("ORCA's scan; source: %s; quals: %d kernel, %d fallback steps%s",
							  how, quals.kernel, quals.fallback,
							  quals.declared > 0 ? psprintf(", %d declared calls", quals.declared) : ""));
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

/* A FunctionScan of vexec.ingest_stream() ORCA's translator built, and its VecIngest. */
static Plan *
orca_ingest(VexecPlanState *ps, FunctionScan *fs, List *rtable)
{
	VexecAlt   *alt = NULL;
	const char *refusal;
	Plan	   *built;

	built = vexec_build_ingest_from_functionscan(fs, rtable, &refusal);
	if (built == NULL && refusal == NULL)
		return NULL;			/* another function */
	if (ps->record)
		alt = vexec_alt_record(ps, "VecIngest", "vexec.ingest_stream()", NULL, NULL);
	if (built == NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return NULL;
	}
	if (alt != NULL)
	{
		VexecCost	cost;

		memset(&cost, 0, sizeof(cost));
		cost.rows = fs->scan.plan.plan_rows;
		vexec_alt_costed(ps, alt, &cost, "ORCA's function scan; source: the client's stream");
	}
	ps->npossible++;
	if (!chosen(ps, NULL))
		return NULL;
	return built;
}

/*
 * The rows an INSERT of ORCA's writes, as ORCA estimated them: the
 * translator gives the Result it puts under the ModifyTable none of its
 * own (CTranslatorDXLToPlStmt::TranslateDXLDml()), so the first node down
 * that has an estimate.  A client's stream read by VecIngest, through a
 * Motion on a cluster, is as long as the client makes it, which ORCA
 * prices as any function's rows, 1,000: it counts as vexec.min_rows.
 */
static double
insert_rows(ModifyTable *mt)
{
	Plan	   *p;
	double		rows = mt->plan.plan_rows;

	for (p = mt->plan.lefttree; p != NULL; p = p->lefttree)
	{
		if (rows <= 0 && p->plan_rows > 0)
			rows = p->plan_rows;
		if (IsA(p, CustomScan) && ((CustomScan *) p)->methods == vexec_ingest_methods())
			return Max(rows, vexec_min_rows);
	}
	return rows;
}

/* An INSERT's ModifyTable ORCA's translator built, and its VecInsert. */
static Plan *
orca_insert(VexecPlanState *ps, ModifyTable *mt, List *rtable)
{
	VexecAlt   *alt = NULL;
	const char *refusal;
	Plan	   *built;
	List	   *oids = NIL;

	if (mt->operation != CMD_INSERT)
		return NULL;
	if (ps->record)
		alt = vexec_alt_record(ps, "VecInsert", "ORCA's INSERT", NULL, NULL);
	built = vexec_build_insert_from_modifytable(mt, rtable, &refusal, &oids);
	if (built == NULL)
	{
		vexec_alt_refuse(ps, alt, refusal ? refusal : "not an INSERT");
		return NULL;
	}
	if (ps->mode != VEXEC_MODE_FORCE && insert_rows(mt) < vexec_min_rows)
	{
		vexec_alt_refuse(ps, alt, psprintf("%.0f rows, fewer than vexec.min_rows",
										   insert_rows(mt)));
		return NULL;
	}
	if (alt != NULL)
	{
		VexecCost	cost;

		memset(&cost, 0, sizeof(cost));
		cost.rows = insert_rows(mt);
		vexec_alt_costed(ps, alt, &cost,
						 psprintf("ORCA's INSERT; input: %s",
								  vexec_is_vector_node(mt->plan.lefttree) ? "batches" : "rows"));
	}
	ps->npossible++;
	if (!chosen(ps, NULL))
		return NULL;
	ps->relation_oids = list_concat(ps->relation_oids, oids);

	/*
	 * On a cluster's coordinator the ModifyTable stays, the dispatcher's
	 * mark of a write, and VecInsert below it writes the rows (insert.c).
	 */
	if (vexec_on_coordinator())
	{
		mt->plan.lefttree = built;
		ps->insert_inputs = true;
		return NULL;
	}
	return built;
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
		case T_FunctionScan:
			return orca_ingest(ps, (FunctionScan *) plan, rtable);
		case T_ModifyTable:
			return orca_insert(ps, (ModifyTable *) plan, rtable);
		case T_Limit:
			vexec_orca_limit_bound((Limit *) plan);
			return NULL;
		case T_MergeJoin:
			orca_mergejoin((MergeJoin *) plan);
			return NULL;
		case T_CustomScan:
			/* a Motion over a vector node: its frames (motion.c, V7) */
			if (vexec_is_gp_motion(plan))
				return vexec_orca_motion(ps, (CustomScan *) plan, ps->cursor_options);
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
	else if (cscan->methods == vexec_motion_send_methods())
	{
		/*
		 * the top of a Motion's fragment, which passes its child's rows on
		 * in frames, and computes a Redistribute's keys; M8's Gather may go
		 * above it, each participant sending its share's frames
		 */
		vn->kind = GP_ORCA_VEC_RESULT;
		vn->exprs = cscan->custom_exprs;
	}
	else if (cscan->methods == vexec_motion_recv_methods())
		vn->kind = GP_ORCA_VEC_RESULT;	/* what the Motion's rows were */
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

/* ---------------------------------------------------------------------
 * ORCA's options for the statement (V5)
 * ---------------------------------------------------------------------
 */

/* vexec.orca_settings, parsed: a list of DefElem, a setting's name and value. */
static List *
parse_orca_settings(const char *value, char **error)
{
	char	   *raw = pstrdup(value);
	List	   *items = NIL;
	List	   *settings = NIL;
	ListCell   *lc;

	*error = NULL;
	if (!SplitGUCList(raw, ',', &items))
	{
		*error = "a list of name=value, separated by commas, was expected";
		return NIL;
	}
	foreach(lc, items)
	{
		char	   *item = lfirst(lc);
		char	   *eq = strchr(item, '=');
		char	   *name;
		char	   *val;

		if (eq == NULL || eq == item || eq[1] == '\0')
		{
			*error = psprintf("\"%s\" is not name=value", item);
			return NIL;
		}
		*eq = '\0';
		name = pstrdup(item);
		val = pstrdup(eq + 1);
		/* the setting's spaces around "=" are not its own */
		while (*name && name[strlen(name) - 1] == ' ')
			name[strlen(name) - 1] = '\0';
		while (*val == ' ')
			val++;
		if (strncmp(name, "gp.optimizer_", strlen("gp.optimizer_")) != 0)
		{
			*error = psprintf("\"%s\" is not one of ORCA's settings, gp.optimizer_...", name);
			return NIL;
		}
		settings = lappend(settings, makeDefElem(name, (Node *) makeString(val), -1));
	}
	return settings;
}

bool
vexec_orca_settings_check(char **newval, void **extra, GucSource source)
{
	char	   *error;

	(void) extra;
	(void) source;
	if (*newval == NULL || **newval == '\0')
		return true;
	(void) parse_orca_settings(*newval, &error);
	if (error != NULL)
	{
		GUC_check_errdetail("%s", error);
		return false;
	}
	return true;
}

typedef struct OrderedAggContext
{
	bool		found;
} OrderedAggContext;

/* An aggregate a hashed aggregation cannot run: DISTINCT, ORDER BY, ordered-set. */
static bool
find_unhashable_agg(Node *node, OrderedAggContext *ctx)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;

		if (aggref->aggdistinct != NIL || aggref->aggorder != NIL ||
			aggref->aggkind != AGGKIND_NORMAL)
		{
			ctx->found = true;
			return true;
		}
	}
	if (IsA(node, Query))
		return query_tree_walker((Query *) node, find_unhashable_agg, ctx, 0);
	return expression_tree_walker(node, find_unhashable_agg, ctx);
}

/*
 * Whether the statement may plan without sorted aggregation
 * (gp.optimizer_enable_groupagg off): every aggregation of it can hash --
 * no aggregate with DISTINCT or ORDER BY, no ordered-set aggregate, no
 * grouping sets, and every grouping key hashable.  Otherwise ORCA would
 * find no plan, and the planner would plan the statement (§3.3.4).
 */
static bool
aggregations_hash(Query *parse)
{
	OrderedAggContext ctx = {false};
	ListCell   *lc;

	if (parse == NULL)
		return false;
	(void) query_tree_walker(parse, find_unhashable_agg, &ctx, 0);
	if (ctx.found || parse->groupingSets != NIL)
		return false;
	foreach(lc, parse->groupClause)
		if (!lfirst_node(SortGroupClause, lc)->hashable)
			return false;
	foreach(lc, parse->distinctClause)
		if (!lfirst_node(SortGroupClause, lc)->hashable)
			return false;
	return true;
}

/*
 * ORCA's options for a statement vexec takes: its hashed window, where
 * vexec builds windows and ORCA's search sees their prices -- without
 * them ORCA would choose a hashed window over input in an order, which the
 * translator refuses -- and the settings of vexec.orca_settings, all but
 * gp.optimizer_enable_groupagg = off for a statement one of whose
 * aggregations cannot hash.
 */
static void
orca_set_options(void *state, GpOrcaVecOptions *options)
{
	VexecPlanState *ps = state;
	List	   *settings;
	char	   *error;
	ListCell   *lc;

	if (!takes(ps))
		return;
	if (vexec_orca_cost_model && vexec_enable_window)
		options->create_vectorization_plan = true;
	if (vexec_orca_settings == NULL || *vexec_orca_settings == '\0')
		return;
	settings = parse_orca_settings(vexec_orca_settings, &error);
	foreach(lc, settings)
	{
		DefElem    *d = lfirst_node(DefElem, lc);

		if (strcmp(d->defname, "gp.optimizer_enable_groupagg") == 0)
		{
			bool		on;

			if (parse_bool(strVal(d->arg), &on) && !on && !aggregations_hash(ps->parse))
				continue;
		}
		options->settings = lappend(options->settings, d);
	}
}

/* ---------------------------------------------------------------------
 * The cost oracle, for ORCA's search (V5)
 * ---------------------------------------------------------------------
 */

/*
 * vexec's prices in ORCA's terms, for a statement vexec takes in auto or
 * force mode with vexec.orca_cost_model on: the two factors are shares,
 * whatever the units; a crossing between rows and batches and a node's
 * setup, in PostgreSQL's units, become shares of a tuple's processing, as
 * cpu_tuple_cost prices it there.
 */
static bool
orca_cost_factors(void *state, GpOrcaVecCosts *costs)
{
	VexecPlanState *ps = state;
	double		tuple = cpu_tuple_cost > 0 ? cpu_tuple_cost : DEFAULT_CPU_TUPLE_COST;

	if (!takes(ps) || !vexec_orca_cost_model)
		return false;
	costs->tuple_factor = vexec_cpu_tuple_factor;
	costs->operator_factor = vexec_cpu_operator_factor;
	costs->convert_factor = vexec_convert_cost / tuple;
	costs->setup_rows = vexec_batch_setup_cost / tuple;
	costs->min_rows = ps->mode == VEXEC_MODE_FORCE ? 0 : vexec_min_rows;
	costs->kinds = 1U << GP_ORCA_VEC_RESULT;
	if (vexec_enable_scan)
		costs->kinds |= 1U << GP_ORCA_VEC_SEQSCAN;
	if (vexec_enable_hashjoin)
		costs->kinds |= 1U << GP_ORCA_VEC_HASHJOIN;
	if (vexec_enable_agg)
		costs->kinds |= 1U << GP_ORCA_VEC_AGG;
	if (vexec_enable_sort)
		costs->kinds |= 1U << GP_ORCA_VEC_SORT;
	if (vexec_enable_window)
		costs->kinds |= 1U << GP_ORCA_VEC_WINDOW;
	ps->orca_costed = true;
	return true;
}

/*
 * A call: a kernel where one is bound to the function for its first
 * argument's type, as the oracle binds it (oracle.c), else the fallback; a
 * set-returning function is in no vector node.  ORCA's metadata carries no
 * collation: a collatable argument takes the database's, as the column's
 * would.
 */
static int
orca_cost_call(void *state, Oid funcid, Oid opno, int nargs, const Oid *argtypes,
			   Oid collation)
{
	Oid			inputtype = nargs > 0 ? argtypes[0] : InvalidOid;

	(void) state;
	if (OidIsValid(opno))
		funcid = get_opcode(opno);
	if (!OidIsValid(funcid))
		return GP_ORCA_VEC_STEP_FALLBACK;
	if (get_func_retset(funcid))
		return GP_ORCA_VEC_STEP_REFUSED;
	if (!OidIsValid(inputtype))
	{
		Oid		   *declared;
		int			ndeclared;

		(void) get_func_signature(funcid, &declared, &ndeclared);
		if (ndeclared > 0)
			inputtype = declared[0];
	}
	if (!OidIsValid(collation) && OidIsValid(inputtype) && type_is_collatable(inputtype))
		collation = DEFAULT_COLLATION_OID;
	return vexec_kernel_bound(funcid, inputtype, collation) ?
		GP_ORCA_VEC_STEP_KERNEL : GP_ORCA_VEC_STEP_FALLBACK;
}

/*
 * An aggregate: VecAgg runs any but a DISTINCT, an ordered or an
 * ordered-set one -- by a vector transition where it has one, else
 * through its transition function, row by row (§3.8).
 */
static int
orca_cost_aggregate(void *state, Oid aggfnoid, int nargs, const Oid *argtypes,
					bool distinct, bool ordered)
{
	(void) state;
	if (distinct || ordered)
		return GP_ORCA_VEC_STEP_REFUSED;
	return vexec_agg_vectorized(aggfnoid, AGGSPLIT_SIMPLE, nargs > 0 ? argtypes[0] : InvalidOid,
								NULL) ?
		GP_ORCA_VEC_STEP_KERNEL : GP_ORCA_VEC_STEP_FALLBACK;
}

/*
 * What vexec's scan of a relation reads: none of a relation that is not a
 * table; only its columns where its storage keeps them apart -- ao_column's
 * and PAX's, whose batch sources read the columns asked for (§3.3.4) --
 * else its rows, each transposed into batches: heap's through its page
 * reader, any other through its source or the slot path.
 */
static int
orca_cost_relation(void *state, Oid relid)
{
	Relation	rel;
	char		relkind;
	int			kind = GP_ORCA_VEC_REL_ROWS;

	(void) state;
	relkind = get_rel_relkind(relid);
	if (relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW)
		return GP_ORCA_VEC_REL_NONE;
	/* the statement holds its relations' locks; one it does not is taken */
	rel = table_open(relid, AccessShareLock);
	if (rel->rd_tableam == NULL)
		kind = GP_ORCA_VEC_REL_NONE;
	else
	{
		const char *how;
		char	   *amname = get_am_name(rel->rd_rel->relam);

		if (vexec_source_for(rel, &how) != NULL && amname != NULL &&
			(strcmp(amname, "ao_column") == 0 || strcmp(amname, "pax") == 0))
			kind = GP_ORCA_VEC_REL_COLUMNS;
	}
	table_close(rel, NoLock);
	return kind;
}

/* Whether vexec hashes a key compared by this operator: strict, with a hash function. */
static bool
orca_cost_hash_key(void *state, Oid eqop, Oid collation)
{
	Oid			lhash;
	Oid			rhash;

	(void) state;
	(void) collation;
	return OidIsValid(eqop) && op_strict(eqop) &&
		get_op_hash_functions(eqop, &lhash, &rhash) && OidIsValid(lhash);
}

/* ORCA's hashed window, lowered: VecWindowHashAgg in its Sort's place (window.c). */
static Plan *
orca_build_window(void *state, WindowAgg *window, List *rtable)
{
	VexecPlanState *ps = state;

	if (ps == NULL || !ps->gate_open)
		return NULL;
	return vexec_build_window(ps, window, rtable);
}

/* ORCA's plan is made: the plan check, the reasons, the debug requirement. */
static void
orca_end(void *state, PlannedStmt *stmt)
{
	VexecPlanState *ps = state;
	int			nodes = -1;

	/* a VecInsert's partitions: the plan depends on each (insert.c) */
	if (ps->relation_oids != NIL)
		stmt->relationOids = list_concat(stmt->relationOids, ps->relation_oids);

	/*
	 * A node added beside ORCA's own -- a VecInsert under the ModifyTable
	 * the dispatcher writes by -- numbered past the plan's others, as the
	 * port's passes over ORCA's plans number theirs (pg19/orca/merge.c,
	 * parallel.c): a node's id says which node a segment's figures are
	 * for.  Found in the finished plan, which may hold a copy of it.
	 */
	if (ps->insert_inputs)
		vexec_number_insert_inputs(stmt);

	/* the nodes made around Motions, numbered past the plan's own */
	vexec_orca_motion_number(ps, stmt);

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
