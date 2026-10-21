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
 * Vec Append (pg_vector_executor.md §3.22's T6) is ORCA's Append of vector
 * children made one: a VecResult with its first child in lefttree and the
 * others in custom_plans, read in turn, each to its end, its target list
 * the children's columns as OUTER_VAR, as an Append's are the first
 * child's -- so that EXPLAIN finds its columns through lefttree as it finds
 * an Append's.  Its batches are its children's, which a vector parent reads
 * with no row between; where a child hands up no batches, it runs a row at
 * a time over each child's rows.  It is not an Append's parallel-aware
 * form: under a Gather each participant reads every child, each child's
 * scan shared among them (gp_orca_vec.h's GP_ORCA_VEC_APPEND).
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
	PlanState  *child;			/* the child read now */
	bool		batches;		/* the children's batches; else their rows */
	/* Vec Append: its children, the outer plan the first, and the one read */
	int			nchildren;
	PlanState **children;
	int			current;
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

/* Its methods, for pg_otel_probe's reporter of vector nodes (node.c). */
const CustomExecMethods *
vexec_result_exec_methods(void)
{
	return &result_exec_methods;
}

Node *
vexec_create_result_state(CustomScan *cscan)
{
	VexecResultState *s = palloc0(sizeof(VexecResultState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &result_exec_methods;
	s->node.kind = VEXEC_NODE_RESULT;
	return (Node *) s;
}

/* Whether a child hands up batches a vector parent reads. */
static bool
batch_child(PlanState *child)
{
	return vexec_is_vector_state(child) && vexec_node_batchable((VexecNode *) child);
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
	s->batches = batch_child(s->child);

	/* Vec Append's other children, which EXPLAIN shows after the first */
	if (cscan->custom_plans != NIL)
	{
		s->nchildren = 1 + list_length(cscan->custom_plans);
		s->children = palloc(sizeof(PlanState *) * s->nchildren);
		s->children[0] = s->child;
		i = 1;
		foreach_ptr(Plan, plan, cscan->custom_plans)
		{
			s->children[i] = ExecInitNode(plan, estate, eflags);
			css->custom_ps = lappend(css->custom_ps, s->children[i]);
			s->batches = s->batches && batch_child(s->children[i]);
			i++;
		}
		node->label = "Vec Append";
	}

	node->input_varno = OUTER_VAR;
	node->ninput = desc->natts;
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(desc->natts, 1));
	for (i = 0; i < desc->natts; i++)
		node->input_attnos[i] = i + 1;
	node->input_slot = ExecInitExtraTupleSlot(estate, desc, &TTSOpsVirtual);

	vexec_node_compile(node, cscan->scan.plan.qual, cscan->scan.plan.targetlist);

	if (s->batches)
		node->vec_child = (VexecNode *) s->child;
	node->row_input = !s->batches;
}

/*
 * The child's next batch: its output columns, its rows to be resolved; for
 * Vec Append, the next child's once one is done, which its rows to be
 * resolved go back to (vec_child).
 */
static bool
result_fetch(VexecNode *node)
{
	VexecResultState *s = (VexecResultState *) node;
	VexecNode  *child;
	VexecBatch *b = vexec_next_batch(s->child);

	while (b == NULL && s->current + 1 < s->nchildren)
	{
		s->child = s->children[++s->current];
		node->vec_child = (VexecNode *) s->child;
		b = vexec_next_batch(s->child);
	}
	if (b == NULL)
		return false;
	child = node->vec_child;
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

/*
 * Row mode: the child's rows through PostgreSQL's evaluator, as a Result;
 * Vec Append's children's in turn, as an Append's.
 */
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
		if (TupIsNull(slot) && s->current + 1 < s->nchildren)
		{
			s->child = s->children[++s->current];
			continue;
		}
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

	/*
	 * a child rescans on its first call when its parameters changed; Vec
	 * Append's others are told of them here, as ExecReScanAppend() tells an
	 * Append's, where ExecReScan() tells the outer plan
	 */
	if (s->nchildren > 0)
	{
		for (int i = 0; i < s->nchildren; i++)
		{
			if (i > 0 && css->ss.ps.chgParam != NULL)
				UpdateChangedParamSet(s->children[i], css->ss.ps.chgParam);
			if (s->children[i]->chgParam == NULL)
				ExecReScan(s->children[i]);
		}
		s->current = 0;
		s->child = s->children[0];
		if (s->batches)
			s->node.vec_child = (VexecNode *) s->child;
	}
	else if (s->child->chgParam == NULL)
		ExecReScan(s->child);
	vexec_node_rescan(&s->node);
}

static void
result_end(CustomScanState *css)
{
	VexecResultState *s = (VexecResultState *) css;

	if (s->nchildren > 0)
		for (int i = 0; i < s->nchildren; i++)
			ExecEndNode(s->children[i]);
	else
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
