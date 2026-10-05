/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecresult.c
 *	  VecResult: a projection or a filter over a vector child, a batch at
 *	  a time (pg_vector_executor.md §3.8).
 *
 * It is ORCA's Result made a vector node (§3.3.4): a Result over a vector
 * child, with no constant qual -- a gating Result stays a row node, one of
 * gp_core's squelch points (pg19/modules/gp_core/gp_motion.c:2062-2079).
 * Its child is in lefttree, its qual and target list read the child's
 * output as OUTER_VAR, as a Result's do.
 *
 * It reads its child's batches through the batch interface where the
 * child's quals and targets are all eager (exec.h).  Otherwise it runs a
 * row at a time, as a Result, PostgreSQL's evaluator over each row the
 * child gives: reading such a child a batch ahead would evaluate rows the
 * consumer may never ask for.  A vector node never reads a row child a
 * batch ahead for the same reason; the planners build no VecResult over
 * one.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "nodes/nodeFuncs.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "expr/expr.h"

typedef struct VexecResultState
{
	VexecNode	node;			/* first */
	PlanState  *child;
	bool		batches;		/* the child's batches; else its rows */
} VexecResultState;

static bool result_fetch(VexecNode *node);
static void result_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *result_exec(CustomScanState *css);
static void result_end(CustomScanState *css);
static void result_rescan(CustomScanState *css);
static void result_explain(CustomScanState *css, List *ancestors, ExplainState *es);

static const CustomExecMethods result_exec_methods = {
	.CustomName = VEXEC_RESULT_NAME,
	.BeginCustomScan = result_begin,
	.ExecCustomScan = result_exec,
	.EndCustomScan = result_end,
	.ReScanCustomScan = result_rescan,
	.ExplainCustomScan = result_explain,
};

Node *
vexec_create_result_state(CustomScan *cscan)
{
	VexecResultState *s = palloc0(sizeof(VexecResultState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &result_exec_methods;
	s->node.kind = VEXEC_NODE_RESULT;
	return (Node *) s;
}

static void
result_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecResultState *s = (VexecResultState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	TupleDesc	desc;
	int			i;

	vexec_node_begin(node, estate);
	node->fetch = result_fetch;

	s->child = ExecInitNode(outerPlan(cscan), estate, eflags);
	outerPlanState(css) = s->child;
	desc = ExecGetResultType(s->child);

	node->input_varno = OUTER_VAR;
	node->ninput = desc->natts;
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(desc->natts, 1));
	for (i = 0; i < desc->natts; i++)
		node->input_attnos[i] = i + 1;
	node->input_slot = ExecInitExtraTupleSlot(estate, desc, &TTSOpsVirtual);

	vexec_node_compile(node, cscan->scan.plan.qual, cscan->scan.plan.targetlist);

	s->batches = vexec_is_vector_state(s->child) &&
		vexec_node_batchable((VexecNode *) s->child);
	if (s->batches)
		node->vec_child = (VexecNode *) s->child;
	node->row_input = !s->batches;
}

/* The child's next batch: its output columns, its rows to be resolved. */
static bool
result_fetch(VexecNode *node)
{
	VexecResultState *s = (VexecResultState *) node;
	VexecNode  *child = node->vec_child;
	VexecBatch *b = vexec_next_batch(s->child);

	if (b == NULL)
		return false;
	node->in = b;
	if (vexec_bits_any(child->redo, b->nrows) ||
		(child->child_redo != NULL && vexec_bits_any(child->child_redo, b->nrows)))
	{
		node->child_redo = vexec_bits_copy(node->work, child->redo, b->nrows);
		if (child->child_redo != NULL)
			vexec_bits_or_into(node->child_redo, child->child_redo, b->nrows);
	}
	return true;
}

/* Row mode: the child's rows through PostgreSQL's evaluator, as a Result. */
static TupleTableSlot *
result_rows(VexecResultState *s)
{
	ExprContext *econtext = s->node.css.ss.ps.ps_ExprContext;
	ProjectionInfo *proj = s->node.css.ss.ps.ps_ProjInfo;

	for (;;)
	{
		TupleTableSlot *slot;

		ResetExprContext(econtext);
		slot = ExecProcNode(s->child);
		if (TupIsNull(slot))
			return ExecClearTuple(s->node.css.ss.ps.ps_ResultTupleSlot);
		econtext->ecxt_outertuple = slot;
		if (s->node.css.ss.ps.qual != NULL && !ExecQual(s->node.css.ss.ps.qual, econtext))
		{
			InstrCountFiltered1(&s->node, 1);
			continue;
		}
		s->node.stats.lazy_rows++;
		if (proj != NULL)
			return ExecProject(proj);
		return vexec_node_unprojected(&s->node, slot);
	}
}

static TupleTableSlot *
result_exec(CustomScanState *css)
{
	VexecResultState *s = (VexecResultState *) css;

	if (!s->batches)
		return result_rows(s);
	return vexec_node_exec(&s->node);
}

static void
result_rescan(CustomScanState *css)
{
	VexecResultState *s = (VexecResultState *) css;

	/* the child rescans on its first call when its parameters changed */
	if (s->child->chgParam == NULL)
		ExecReScan(s->child);
	vexec_node_rescan(&s->node);
}

static void
result_end(CustomScanState *css)
{
	VexecResultState *s = (VexecResultState *) css;

	ExecEndNode(s->child);
	vexec_node_end(&s->node);
}

static void
result_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecResultState *s = (VexecResultState *) css;

	vexec_node_explain(&s->node, ancestors, es);
	if (es->verbose)
		ExplainPropertyText("Input", s->batches ? "batches" : "rows", es);
}
