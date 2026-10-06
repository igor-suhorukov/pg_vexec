/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * node.c
 *	  What every vector node shares (pg_vector_executor.md §3.6, §3.7,
 *	  §3.9; exec.h).
 *
 * A batch, in three parts:
 *
 *	1. its input: the node's fetch() fills `in` -- from a source, or from
 *	   a vector child's output batch;
 *	2. the eager parts, a batch at a time: the quals up to the first lazy
 *	   one, in order, each over the rows the ones before it passed; the
 *	   eager quals after it, over the same rows, kept per qual; the eager
 *	   targets, over the rows past the eager quals.  A row a kernel could
 *	   not compute joins the batch's redo rows (expr/expr.h);
 *	3. the rows, one a call, in row order, as the consumer asks: a redo
 *	   row is evaluated whole by PostgreSQL's evaluator -- every qual, then
 *	   the projection, from the node's own ExprStates -- which raises where
 *	   PostgreSQL raises; any other row runs the quals from the first lazy
 *	   one on, in order, the eager ones' results read and the lazy ones
 *	   evaluated, and then the target list, the eager entries read from
 *	   their columns and the lazy ones evaluated.
 *
 * So everything PostgreSQL would evaluate for a row, in its order, is
 * either a pure step evaluated ahead or evaluated by PostgreSQL itself when
 * the row is reached; and nothing that can raise or has a side effect runs
 * for a row the consumer never asks for.
 *
 * A row is valid until the next call, the usual rule (§3.6): the result
 * slot's values point into the node's batches, which the next input batch
 * resets.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/sysattr.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "utils/memutils.h"
#include "utils/regproc.h"

#include "cb_explain.h"
#include "vexec_kernels.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "expr/expr.h"
#include "expr/kernel.h"

void
vexec_node_begin(VexecNode *node, EState *estate)
{
	/*
	 * The planner offers no vector node to EvalPlanQual's plans (§3.3.7);
	 * one that met it anyway raises rather than return a wrong row (§3.9).
	 */
	if (estate->es_epq_active != NULL)
		elog(ERROR, "vexec: a vector node in EvalPlanQual's plan");

	node->mcxt = CurrentMemoryContext;
	node->layout = vexec_layout_config();
	node->eager_econtext = CreateExprContext(estate);
	node->work = vexec_batch_create(node->mcxt, 0, NULL);
	node->loaded_row = -1;
}

/*
 * Compile the node's quals and target list against its input: a Var of
 * input_varno reads the input column whose attno it is.
 */
void
vexec_node_compile(VexecNode *node, List *quals, List *tlist)
{
	VexecCompileContext cc;
	int			base = -FirstLowInvalidHeapAttributeNumber;
	int			maxattno = 0;
	ListCell   *lc;
	int			i;

	for (i = 0; i < node->ninput; i++)
		maxattno = Max(maxattno, node->input_attnos[i]);

	memset(&cc, 0, sizeof(cc));
	cc.parent = &node->css.ss.ps;
	cc.input_varno = node->input_varno;
	cc.attno_base = base;
	cc.attno_max = maxattno;
	cc.mcxt = node->mcxt;
	cc.attno_col = palloc(sizeof(int) * (base + maxattno + 1));
	for (i = 0; i < base + maxattno + 1; i++)
		cc.attno_col[i] = -1;
	for (i = 0; i < node->ninput; i++)
		cc.attno_col[node->input_attnos[i] + base] = i;

	node->nquals = list_length(quals);
	node->quals = palloc0(sizeof(VexecTop) * Max(node->nquals, 1));
	node->first_lazy = node->nquals;
	i = 0;
	foreach(lc, quals)
	{
		vexec_compile_top(&cc, lfirst(lc), true, &node->quals[i]);
		if (node->quals[i].eager == NULL && node->first_lazy == node->nquals)
			node->first_lazy = i;
		i++;
	}
	node->qual_pass = palloc0(sizeof(uint64 *) * Max(node->nquals, 1));

	node->ntargets = list_length(tlist);
	node->targets = palloc0(sizeof(VexecTop) * Max(node->ntargets, 1));
	node->outputs = palloc0(sizeof(VexecVec *) * Max(node->ntargets, 1));
	i = 0;
	foreach(lc, tlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		vexec_compile_top(&cc, tle->expr, false, &node->targets[i]);
		if (node->targets[i].eager == NULL)
			node->any_lazy_target = true;
		i++;
	}
	node->nkernels = cc.nkernels;
	node->nfallbacks = cc.nfallbacks;
	node->declared = list_concat(node->declared, cc.declared);

	/* the output batch, its columns the targets' */
	{
		VexecType **types = palloc(sizeof(VexecType *) * Max(node->ntargets, 1));

		for (i = 0; i < node->ntargets; i++)
			types[i] = node->targets[i].type;
		node->out = vexec_batch_create(node->mcxt, node->ntargets, types);
	}
}

void
vexec_node_count_fallback(VexecNode *node, int rows)
{
	node->stats.fallback_rows += rows;
}

void
vexec_node_count_kernel(VexecNode *node)
{
	node->stats.kernel_steps++;
}

void
vexec_node_count_declared(VexecNode *node, int rows)
{
	node->stats.declared_rows += rows;
}

/* Point both expression contexts at the input slot. */
static void
set_input_slot(VexecNode *node, ExprContext *econtext, TupleTableSlot *slot)
{
	if (node->input_varno == OUTER_VAR)
		econtext->ecxt_outertuple = slot;
	else
		econtext->ecxt_scantuple = slot;
}

/*
 * Load input row `row` into the input slot, for PostgreSQL's evaluator:
 * every column the node reads, the others NULL.
 */
void
vexec_node_load_input(VexecNode *node, int row)
{
	TupleTableSlot *slot = node->input_slot;
	int			natts = slot->tts_tupleDescriptor->natts;
	int			i;

	if (node->loaded_row == row)
		return;
	ExecClearTuple(slot);
	for (i = 0; i < natts; i++)
	{
		slot->tts_values[i] = (Datum) 0;
		slot->tts_isnull[i] = true;
	}
	for (i = 0; i < node->ninput; i++)
	{
		AttrNumber	attno = node->input_attnos[i];
		bool		isnull;
		Datum		d = vexec_vec_datum(node->work, &node->in->cols[i], row, &isnull);

		if (attno > 0 && attno <= natts)
		{
			slot->tts_values[attno - 1] = d;
			slot->tts_isnull[attno - 1] = isnull;
		}
		else if (attno == SelfItemPointerAttributeNumber && !isnull)
			slot->tts_tid = *(ItemPointer) DatumGetPointer(d);
	}
	ExecStoreVirtualTuple(slot);
	node->loaded_row = row;
}

/*
 * A VecSort's running bound over the rows of `active` (H6, vecsort.c): a
 * row whose bound key is strictly past the bound is one the sort would
 * discard, and leaves `active`; a row PostgreSQL's evaluator is to decide,
 * in `redo`, stays.  How many left.
 */
static int
apply_bound(VexecNode *node, uint64 *active, const uint64 *redo, int n)
{
	VexecSortBound *b = node->bound;
	const VexecVec *v = &node->in->cols[node->bound_col];
	MemoryContext old;
	int			removed = 0;
	int			row;

	if (!b->active)
		return 0;
	old = MemoryContextSwitchTo(b->tmpcxt);
	for (row = 0; row < n; row++)
	{
		bool		isnull;
		Datum		d;

		if (!vexec_bit(active, row) || (redo != NULL && vexec_bit(redo, row)))
			continue;
		d = vexec_vec_datum(node->in, v, row, &isnull);
		if (ApplySortComparator(d, isnull, b->value, b->isnull, &b->ssup) > 0)
		{
			vexec_bit_clear(active, row);
			removed++;
		}
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(b->tmpcxt);
	b->removed += removed;
	return removed;
}

/* Whether one row is past the running bound, after the lazy quals (H6). */
static bool
row_past_bound(VexecNode *node, int row)
{
	VexecSortBound *b = node->bound;
	MemoryContext old;
	bool		isnull;
	Datum		d;
	bool		past;

	if (!b->active)
		return false;
	old = MemoryContextSwitchTo(b->tmpcxt);
	d = vexec_vec_datum(node->in, &node->in->cols[node->bound_col], row, &isnull);
	past = ApplySortComparator(d, isnull, b->value, b->isnull, &b->ssup) > 0;
	MemoryContextSwitchTo(old);
	MemoryContextReset(b->tmpcxt);
	if (past)
		b->removed++;
	return past;
}

/* The rows past the quals so far: those they passed, and those to redo. */
static int
rows_kept(VexecNode *node, uint64 *active, uint64 *redo, int n)
{
	return vexec_bits_count(vexec_bits_andnot(node->work, active, redo, n), n) +
		vexec_bits_count(redo, n);
}

/* The eager parts of the batch now in `in` (above, part 2). */
static void
eval_batch(VexecNode *node)
{
	VexecEval	ev;
	int			n = node->in->nrows;
	uint64	   *active;
	int			i;
	int			selected;
	int			rechecked;
	int			nrecheck = Min(node->nrecheck, node->first_lazy);
	int			bounded = 0;

	memset(&ev, 0, sizeof(ev));
	ev.node = node;
	ev.in = node->in;
	ev.work = node->work;
	ev.nrows = n;
	ev.redo = vexec_bitmap_alloc(node->work, n, false);
	ev.exact = true;
	ev.econtext = node->eager_econtext;
	set_input_slot(node, node->eager_econtext, node->input_slot);

	/* the input's rows, less a vector child's rows still to be resolved */
	active = vexec_bits_copy(node->work, node->in->selection, n);
	if (node->child_redo != NULL)
		active = vexec_bits_andnot(node->work, active, node->child_redo, n);
	/* a VecSort's running bound before the quals, where no source checked it */
	if (node->bound != NULL && node->bound->before_quals && !node->bound_in_source)
		(void) apply_bound(node, active, NULL, n);
	selected = vexec_bits_count(active, n);

	/*
	 * the eager prefix of the quals: each over the rows the ones before
	 * passed; the rows a bitmap's conditions keep, counted
	 */
	rechecked = selected;
	for (i = 0; i < node->first_lazy; i++)
	{
		active = vexec_eval_qual(&ev, node->quals[i].eager, active);
		if (i + 1 == nrecheck && node->css.ss.ps.instrument)
			rechecked = rows_kept(node, active, ev.redo, n);
	}

	/*
	 * or after them, where every qual is eager; with a lazy one, a row is
	 * checked once the lazy quals have passed it (vexec_node_exec())
	 */
	if (node->bound != NULL && !node->bound->before_quals && node->first_lazy == node->nquals)
		bounded = apply_bound(node, active, ev.redo, n);

	/* the eager quals after the first lazy one, over the same rows */
	ev.exact = false;
	for (i = node->first_lazy; i < node->nquals; i++)
		node->qual_pass[i] = node->quals[i].eager ?
			vexec_eval_qual(&ev, node->quals[i].eager, active) : NULL;

	/*
	 * The eager targets, over the rows past the eager quals; none where no
	 * row is, so that nothing of them is evaluated for none.
	 */
	for (i = 0; i < node->ntargets; i++)
	{
		ev.exact = node->first_lazy == node->nquals &&
			!(node->target_inexact != NULL && node->target_inexact[i]);
		node->outputs[i] = node->targets[i].eager && vexec_bits_any(active, n) ?
			vexec_eval(&ev, node->targets[i].eager, active) : NULL;
	}

	node->redo = ev.redo;
	node->candidates = vexec_bits_andnot(node->work, active, ev.redo, n);

	/* what the eager quals removed, for EXPLAIN ANALYZE */
	if (node->css.ss.ps.instrument)
	{
		int			kept = vexec_bits_count(node->candidates, n) + vexec_bits_count(ev.redo, n);

		InstrCountFiltered2(node, Max(selected - rechecked, 0));
		InstrCountFiltered1(node, Max(rechecked - kept - bounded, 0));
	}
	node->stats.redo_rows += vexec_bits_count(ev.redo, n);
}

/* The next input batch with rows in it, and its eager parts; false at the end. */
static bool
next_batch(VexecNode *node)
{
	for (;;)
	{
		vexec_batch_reset(node->work);
		node->candidates = NULL;
		node->redo = NULL;
		node->child_redo = NULL;
		node->loaded_row = -1;
		node->ran = true;
		if (!node->fetch(node))
			return false;
		if (node->in->nrows == 0)
			continue;
		node->stats.batches++;
		node->stats.rows_in += node->in->nrows;
		eval_batch(node);
		node->have_batch = true;
		node->next_row = 0;
		return true;
	}
}

/*
 * The next input batch and its eager parts, for a node that consumes its
 * input itself, as VecAgg does: false at the end.
 */
bool
vexec_node_next_input(VexecNode *node)
{
	return next_batch(node);
}

/*
 * A row without a projection: the node's target list is its input row as
 * it is -- or empty, as a Result's under ORCA's count(*) is, whose input
 * still has the columns the quals read.
 */
TupleTableSlot *
vexec_node_unprojected(VexecNode *node, TupleTableSlot *input)
{
	TupleTableSlot *result = node->css.ss.ps.ps_ResultTupleSlot;

	if (result->tts_tupleDescriptor->natts == 0)
	{
		ExecClearTuple(result);
		return ExecStoreVirtualTuple(result);
	}
	return ExecCopySlot(result, input);
}

/* Why row_by_postgres() gave no row. */
typedef enum RowGone
{
	ROW_BY_QUAL,				/* the plan's quals rejected it */
	ROW_BY_RECHECK,				/* a bitmap's conditions did */
	ROW_BY_INPUT				/* resolving its input gave none */
} RowGone;

/*
 * A row through PostgreSQL's evaluator alone: every qual, then the
 * projection, from the node's own ExprStates.  NULL when a qual rejects
 * it.  A row of child_redo is resolved first -- by the vector child, or by
 * the node's resolve_input.  *gone says why it gave no row.
 */
static TupleTableSlot *
row_by_postgres(VexecNode *node, int row, RowGone *gone)
{
	ExprContext *econtext = node->css.ss.ps.ps_ExprContext;
	ProjectionInfo *proj = node->css.ss.ps.ps_ProjInfo;
	TupleTableSlot *input;

	*gone = ROW_BY_INPUT;
	if (node->child_redo != NULL && vexec_bit(node->child_redo, row))
	{
		if (node->resolve_input != NULL)
		{
			/* the node's own: its input row, loaded, or none */
			input = node->resolve_input(node, row);
			if (input == NULL)
				return NULL;
		}
		else
		{
			input = vexec_resolve_row(node->vec_child, row);
			if (input == NULL)
				return NULL;
			node->loaded_row = -1;
		}
	}
	else
	{
		vexec_node_load_input(node, row);
		input = node->input_slot;
	}
	set_input_slot(node, econtext, input);
	*gone = ROW_BY_RECHECK;
	if (node->recheck_qual != NULL && !ExecQual(node->recheck_qual, econtext))
		return NULL;
	*gone = ROW_BY_QUAL;
	if (node->css.ss.ps.qual != NULL && !ExecQual(node->css.ss.ps.qual, econtext))
		return NULL;
	if (proj != NULL)
		return ExecProject(proj);
	return vexec_node_unprojected(node, input);
}

/*
 * The quals from the first lazy one on, in order, for one row: the eager
 * ones' results read, the lazy ones evaluated by PostgreSQL.  The first
 * that rejects the row, or -1.
 */
static int
lazy_qual_rejecting(VexecNode *node, int row)
{
	ExprContext *econtext = node->css.ss.ps.ps_ExprContext;
	int			i;

	for (i = node->first_lazy; i < node->nquals; i++)
	{
		if (node->quals[i].eager != NULL)
		{
			if (!vexec_bit(node->qual_pass[i], row))
				return i;
			continue;
		}
		vexec_node_load_input(node, row);
		set_input_slot(node, econtext, node->input_slot);
		node->stats.lazy_rows++;
		if (!ExecQual(node->quals[i].state, econtext))
			return i;
	}
	return -1;
}

/*
 * The target list for one row: the eager entries read, the lazy evaluated
 * in the row's memory, as ExecProject evaluates a target.  Never in the
 * query's: a function's fn_mcxt is the query's memory, and a function that
 * keeps state in fn_extra may take what is allocated there for its own --
 * PostGIS's caches keep a geometry allocated there by reference, and free
 * it later.
 */
static TupleTableSlot *
project_row(VexecNode *node, int row)
{
	TupleTableSlot *slot = node->css.ss.ps.ps_ResultTupleSlot;
	ExprContext *econtext = node->css.ss.ps.ps_ExprContext;
	int			i;

	ExecClearTuple(slot);
	for (i = 0; i < node->ntargets; i++)
	{
		if (node->outputs[i] != NULL)
			slot->tts_values[i] = vexec_vec_datum(node->work, node->outputs[i], row,
												  &slot->tts_isnull[i]);
		else
		{
			vexec_node_load_input(node, row);
			set_input_slot(node, econtext, node->input_slot);
			slot->tts_values[i] = ExecEvalExprSwitchContext(node->targets[i].state, econtext,
															&slot->tts_isnull[i]);
		}
	}
	return ExecStoreVirtualTuple(slot);
}

/* The rows of the batch to give out: candidates, redo rows, a child's. */
static int
next_row(VexecNode *node)
{
	int			n = node->in->nrows;
	int			a = vexec_bits_next(node->candidates, n, node->next_row);
	int			b = vexec_bits_next(node->redo, n, node->next_row);
	int			r = a < 0 ? b : (b < 0 ? a : Min(a, b));

	if (node->child_redo != NULL)
	{
		int			c = vexec_bits_next(node->child_redo, n, node->next_row);

		if (c >= 0 && (r < 0 || c < r))
			r = c;
	}
	return r;
}

/* The row interface: the node's next row, or an empty slot at the end. */
TupleTableSlot *
vexec_node_exec(VexecNode *node)
{
	ExprContext *econtext = node->css.ss.ps.ps_ExprContext;
	TupleTableSlot *slot;

	for (;;)
	{
		int			row;

		ResetExprContext(econtext);
		if (!node->have_batch)
		{
			if (node->finished || !next_batch(node))
			{
				node->finished = true;
				return ExecClearTuple(node->css.ss.ps.ps_ResultTupleSlot);
			}
		}
		row = next_row(node);
		if (row < 0)
		{
			node->have_batch = false;
			continue;
		}
		node->next_row = row + 1;

		if ((node->redo != NULL && vexec_bit(node->redo, row)) ||
			(node->child_redo != NULL && vexec_bit(node->child_redo, row)))
		{
			RowGone		gone;

			slot = row_by_postgres(node, row, &gone);
			if (slot == NULL)
			{
				/*
				 * A row its input gave none for is no filter's here: the
				 * vector child counted it (vexec_resolve_row()), and a join
				 * row its join quals decided against is no filter's.
				 */
				if (gone == ROW_BY_RECHECK)
					InstrCountFiltered2(node, 1);
				else if (gone == ROW_BY_QUAL)
					InstrCountFiltered1(node, 1);
				continue;
			}
			return slot;
		}
		if (node->first_lazy < node->nquals)
		{
			int			rejecting = lazy_qual_rejecting(node, row);

			if (rejecting >= 0)
			{
				if (rejecting < node->nrecheck)
					InstrCountFiltered2(node, 1);
				else
					InstrCountFiltered1(node, 1);
				continue;
			}
			if (node->bound != NULL && !node->bound->before_quals && row_past_bound(node, row))
				continue;
		}
		return project_row(node, row);
	}
}

/*
 * Whether the node can hand batches to a vector parent: every qual and
 * target of it eager, and its input batches -- a VecResult's child hands
 * them up too.  A VecAgg's targets are its aggregates' inputs, not its
 * output: it hands up rows.
 */
bool
vexec_node_batchable(VexecNode *node)
{
	return node->kind != VEXEC_NODE_AGG && node->kind != VEXEC_NODE_WINDOW &&
		node->first_lazy == node->nquals && !node->any_lazy_target &&
		!node->row_input;
}

/*
 * The batch interface: the node's next batch of output columns, its
 * selection the rows past its quals, and its redo rows -- with a vector
 * child's still to be resolved -- in node->redo and node->child_redo, for
 * the parent to resolve with vexec_resolve_row() in row order.  NULL at
 * the end.  It does by hand what ExecProcNode does (§3.6).
 */
VexecBatch *
vexec_next_batch(PlanState *ps)
{
	VexecNode  *node = (VexecNode *) ps;
	VexecBatch *out = node->out;
	bool		ok;
	int			i;

	Assert(vexec_node_batchable(node));
	if (ps->chgParam != NULL)
		ExecReScan(ps);
	CHECK_FOR_INTERRUPTS();
	if (ps->instrument)
		InstrStartNode(ps->instrument);

	ok = !node->finished && next_batch(node);
	node->have_batch = false;
	if (!ok)
	{
		node->finished = true;
		if (ps->instrument)
			InstrStopNode(ps->instrument, 0);
		return NULL;
	}

	vexec_batch_reset(out);
	out->nrows = node->in->nrows;
	for (i = 0; i < node->ntargets; i++)
	{
		if (node->outputs[i] != NULL)
			out->cols[i] = *node->outputs[i];
		else
		{
			/* no row past the quals: an empty column, every row NULL */
			VexecShape	shape;

			vexec_type_build_shape(out->cols[i].type, &shape);
			vexec_vec_init(out, &out->cols[i], &shape, out->nrows);
			out->cols[i].validity = vexec_bitmap_alloc(out, out->nrows, false);
		}
	}
	out->selection = node->candidates;
	node->stats.batches_out++;

	if (ps->instrument)
		InstrStopNode(ps->instrument, vexec_bits_count(node->candidates, out->nrows));
	return out;
}

/*
 * A row of the batch the node last handed up, through PostgreSQL's
 * evaluator: its output row, or NULL when its quals reject it, which the
 * node counts as its own.  For a parent that reached one of the node's
 * redo rows.
 */
TupleTableSlot *
vexec_resolve_row(VexecNode *node, int row)
{
	TupleTableSlot *slot;
	RowGone		gone;

	ResetExprContext(node->css.ss.ps.ps_ExprContext);
	slot = row_by_postgres(node, row, &gone);
	if (slot != NULL)
	{
		if (node->css.ss.ps.instrument)
			InstrUpdateTupleCount(node->css.ss.ps.instrument, 1);
	}
	else if (gone == ROW_BY_RECHECK)
		InstrCountFiltered2(node, 1);
	else if (gone == ROW_BY_QUAL)
		InstrCountFiltered1(node, 1);
	return slot;
}

void
vexec_node_rescan(VexecNode *node)
{
	node->have_batch = false;
	node->finished = false;
	node->next_row = 0;
	node->loaded_row = -1;
	node->candidates = NULL;
	node->redo = NULL;
	node->child_redo = NULL;
	vexec_batch_reset(node->work);
}

void
vexec_node_end(VexecNode *node)
{
	if (node->eager_econtext)
		FreeExprContext(node->eager_econtext, true);
	node->eager_econtext = NULL;
}

/* EXPLAIN's name for a vector node, in text (§3.6). */
void
vexec_node_relabel(VexecNode *node, ExplainState *es)
{
	CbExplainRelabel(&node->css, es,
					 node->label != NULL ? node->label :
					 node->kind == VEXEC_NODE_SCAN ? "Vec Seq Scan" : "Vec Result", NULL);
}

/* EXPLAIN's lines for a vector node (§3.6, "Properties"). */
void
vexec_node_explain(VexecNode *node, List *ancestors, ExplainState *es)
{
	vexec_node_relabel(node, es);
	vexec_node_explain_properties(node, ancestors, es);
}

/* The lines without the name, for a node that puts its own first. */
void
vexec_node_explain_properties(VexecNode *node, List *ancestors, ExplainState *es)
{
	int			eager = 0;
	int			lazy_targets = 0;
	int			i;

	(void) ancestors;

	for (i = 0; i < node->nquals; i++)
		if (node->quals[i].eager)
			eager++;
	for (i = 0; i < node->ntargets; i++)
		if (!node->targets[i].eager)
			lazy_targets++;

	if (es->verbose)
	{
		ExplainPropertyText("Batch Format",
							node->layout.format == VEXEC_FORMAT_ARROW ? "arrow" : "postgres", es);
		if (node->nquals > 0)
			ExplainPropertyText("Vector Quals",
								psprintf("%d of %d", eager, node->nquals), es);
		ExplainPropertyInteger("Kernel Steps", NULL, node->nkernels, es);
		ExplainPropertyInteger("Fallback Steps", NULL, node->nfallbacks, es);
		vexec_node_explain_declared(node, es);
		if (lazy_targets > 0)
			ExplainPropertyInteger("Row-by-Row Targets", NULL, lazy_targets, es);
		for (i = 0; i < node->nquals; i++)
			if (!node->quals[i].eager)
			{
				ExplainPropertyText("Row-by-Row Qual", node->quals[i].why_lazy, es);
				break;
			}
	}
	if (es->analyze && es->verbose && node->ran)
	{
		ExplainPropertyInteger("Batches", NULL, node->stats.batches, es);
		if (node->stats.lazy_rows > 0)
			ExplainPropertyInteger("Rows Evaluated Row by Row", NULL, node->stats.lazy_rows, es);
		if (node->stats.redo_rows > 0)
			ExplainPropertyInteger("Rows Sent to PostgreSQL", NULL, node->stats.redo_rows, es);
		if (node->stats.declared_rows > 0)
			ExplainPropertyInteger("Rows Through Declared Calls", NULL,
								   node->stats.declared_rows, es);
	}
}

/*
 * EXPLAIN VERBOSE's names of the calls bound to kernel packs' declarations
 * (§3.17): each function once, with its pack and its declaration, in the
 * order the node's programs call them.
 */
void
vexec_node_explain_declared(VexecNode *node, ExplainState *es)
{
	List	   *names = NIL;
	List	   *seen = NIL;
	ListCell   *lc;

	if (!es->verbose || node->declared == NIL)
		return;
	foreach(lc, node->declared)
	{
		VexecExpr  *e = lfirst(lc);

		if (list_member_oid(seen, e->funcid))
			continue;
		seen = lappend_oid(seen, e->funcid);
		names = lappend(names, psprintf("%s [%s: %s]", format_procedure(e->funcid),
										e->pack->name, vexec_decl_kind_name(e->decl->kind)));
	}
	ExplainPropertyList("Declared Calls", names, es);
}
