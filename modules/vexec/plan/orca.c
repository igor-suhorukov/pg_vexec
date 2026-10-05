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
 *			a VecScan, and a Result over a vector node a VecResult, where the
 *			oracle accepts them and the mode chooses them;
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
#include "exec/exec.h"
#include "plan/plan.h"
#include "source/source.h"

static void *orca_begin(Query *parse, int cursorOptions, struct ExplainState *es);
static Plan *orca_build(void *state, Plan *plan, List *rtable);
static void orca_end(void *state, PlannedStmt *stmt);

static const GpOrcaVecRoutine orca_routine = {
	.size = sizeof(GpOrcaVecRoutine),
	.minor = GP_ORCA_VEC_MINOR,
	.name = "vexec",
	.begin_statement = orca_begin,
	.build_node = orca_build,
	.end_statement = orca_end,
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
	(void) vexec_source_for(rel, &how);
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

/* A Result over a vector node, and its VecResult. */
static Plan *
orca_result(VexecPlanState *ps, Result *result)
{
	VexecAlt   *alt = NULL;
	VexecSteps	steps;

	if (result->plan.lefttree == NULL || result->resconstantqual != NULL)
		return NULL;			/* a gating Result, or one with no input */
	if (!vexec_is_vector_node(result->plan.lefttree))
		return NULL;			/* a row child is not read a batch ahead */
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
		default:
			return NULL;
	}
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
