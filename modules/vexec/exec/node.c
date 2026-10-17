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
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "varatt.h"

#include "cb_explain.h"
#include "vexec_kernels.h"

#include "vexec.h"
#include "pg_otel_probe_api.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "exec/keyhash.h"
#include "expr/expr.h"
#include "expr/kernel.h"
#include "motion/motion.h"

/*
 * A lazy target's results, kept by the value of the one input column it
 * reads (vexec.enable_row_memo).  The target is computed at a value's first
 * row, in row order as ever, and a later row of the same value takes that
 * result: it is the same, since the target holds no volatile function, no
 * SubPlan and no parameter, and so is a function of that column alone for
 * the statement; and the first row raised whatever a later one would have.
 * Values match by their bytes, never by a collation's equality.  The
 * entries -- their inputs and results copied into the memo's memory -- are
 * dropped together once MEMO_ENTRIES are kept: repeats come near one
 * another in a table's order (in ClickBench's hits, by its sort key, 54% of
 * Referer's values are within 16,384 rows of their last, 40% within its
 * batch of 1,024).  A memo that found fewer than one value in ten again
 * over its first MEMO_PROBATION rows stops.
 */
#define MEMO_SLOTS		32768
#define MEMO_ENTRIES	16384
#define MEMO_PROBATION	8192

typedef struct VexecRowMemoSlot
{
	uint32		hash;
	bool		used;
	bool		in_null;
	bool		out_null;
	Datum		in;
	Datum		out;
} VexecRowMemoSlot;

typedef struct VexecRowMemo
{
	int			col;			/* the input column */
	int16		in_len;
	bool		in_byval;
	int16		out_len;
	bool		out_byval;
	bool		off;			/* stopped: too few repeats */
	int			nentries;
	int64		lookups;
	int64		hits;
	MemoryContext cxt;			/* the entries' inputs and results */
	VexecRowMemoSlot *slots;	/* MEMO_SLOTS of them */
} VexecRowMemo;

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

	/* on a cluster, the segments' figures of vector nodes for EXPLAIN ANALYZE */
	vexec_gp_core_explain_register();
}

/*
 * X10: whether a subtree is kernels' alone, over columns and constants,
 * which compute every row they are asked for and raise nothing.
 */
static bool
kernels_only(const VexecExpr *e)
{
	if (e->kind == VE_VAR || e->kind == VE_CONST)
		return true;
	if (e->kind != VE_CALL && e->kind != VE_RELABEL)
		return false;
	for (int i = 0; i < e->nargs; i++)
		if (!kernels_only(e->args[i]))
			return false;
	return true;
}

static VexecExpr *
share_walk(VexecNode *node, VexecExpr *e, List **seen)
{
	bool		candidate = e->kind == VE_CALL && kernels_only(e);

	if (candidate)
	{
		foreach_ptr(VexecExpr, s, *seen)
		{
			if (s->type->typid == e->type->typid && s->type->typmod == e->type->typmod &&
				s->type->collation == e->type->collation && equal(s->expr, e->expr))
			{
				if (!s->shared)
					node->nshared++;
				s->shared = true;
				return s;
			}
		}
	}
	for (int i = 0; i < e->nargs; i++)
		e->args[i] = share_walk(node, e->args[i], seen);
	if (candidate)
		*seen = lappend(*seen, e);
	return e;
}

/*
 * X10 (§3.18): equal subtrees of the node's eager targets, kernels' alone,
 * made one, computed once a batch for each that asks over the same rows,
 * as Velox computes an expression several of an operator's share once
 * (velox/expression/Expr.h) -- TPC-H Q1's l_extendedprice * (1 - l_discount)
 * in two of its sums.
 */
static void
share_subtrees(VexecNode *node)
{
	List	   *seen = NIL;

	for (int i = 0; i < node->ntargets; i++)
		if (node->targets[i].eager != NULL)
			node->targets[i].eager = share_walk(node, node->targets[i].eager, &seen);
	list_free(seen);
}

static bool
has_param_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
		return true;
	return expression_tree_walker(node, has_param_walker, context);
}

/*
 * The input column a lazy target is a function of alone, for its memo, and
 * that column's type: one column of the input, no volatile function,
 * SubPlan or parameter; -1 for none.  A cstring has no length to compare
 * by.
 */
static int
memo_column(VexecNode *node, Expr *expr, Oid *type)
{
	List	   *vars;
	Var		   *var = NULL;
	ListCell   *lc;

	if (contain_volatile_functions((Node *) expr) || contain_subplans((Node *) expr) ||
		has_param_walker((Node *) expr, NULL))
		return -1;
	vars = pull_var_clause((Node *) expr, PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS |
						   PVC_RECURSE_PLACEHOLDERS);
	foreach(lc, vars)
	{
		Var		   *v = lfirst(lc);

		if (v->varno != node->input_varno || v->varlevelsup != 0 || v->varattno <= 0 ||
			(var != NULL && v->varattno != var->varattno))
			return -1;
		var = v;
	}
	if (var == NULL || get_typlen(var->vartype) == -2)
		return -1;
	*type = var->vartype;
	for (int i = 0; i < node->ninput; i++)
		if (node->input_attnos[i] == var->varattno)
			return i;
	return -1;
}

static VexecRowMemo *
memo_create(VexecNode *node, int col, Oid in_type, Oid out_type)
{
	VexecRowMemo *m = MemoryContextAllocZero(node->mcxt, sizeof(VexecRowMemo));

	m->col = col;
	get_typlenbyval(in_type, &m->in_len, &m->in_byval);
	get_typlenbyval(out_type, &m->out_len, &m->out_byval);
	m->cxt = AllocSetContextCreate(node->mcxt, "vexec row memo", ALLOCSET_DEFAULT_SIZES);
	m->slots = MemoryContextAllocZero(node->mcxt, sizeof(VexecRowMemoSlot) * MEMO_SLOTS);
	return m;
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

	/* a lazy target of one input column: its results kept by that column's value */
	node->row_memo = palloc0(sizeof(VexecRowMemo *) * Max(node->ntargets, 1));
	if (vexec_enable_row_memo && node->any_lazy_target)
	{
		i = 0;
		foreach(lc, tlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);
			Oid			type;
			int			col;

			if (node->targets[i].eager == NULL && (col = memo_column(node, tle->expr, &type)) >= 0)
				node->row_memo[i] = memo_create(node, col, type, exprType((Node *) tle->expr));
			i++;
		}
	}
	node->nkernels = cc.nkernels;
	node->nfallbacks = cc.nfallbacks;
	node->declared = list_concat(node->declared, cc.declared);
	if (vexec_enable_shared_exprs)
		share_subtrees(node);

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

/*
 * X6: a VecHashJoin's inner keys over the rows of `active` (exec.h): a row
 * whose key no inner row has, or NULL, leaves `active`; a row PostgreSQL's
 * evaluator is to decide, in `redo`, stays.  How many left.  Where it keeps
 * more than 70% of the rows it checks, it is off for the next
 * VEXEC_JOIN_FILTER_OFF batches, as ClickHouse's runtime filter is.
 */
static int
apply_join_filter(VexecNode *node, uint64 *active, const uint64 *redo, int n)
{
	VexecJoinFilter *f = node->jfilter;
	const VexecVec *v = &node->in->cols[node->jfilter_col];
	int			width = v->encoding == VEXEC_FLAT && v->shape.layout == VEXEC_FIXED &&
		!v->shape.arrow_values && v->shape.stride == v->shape.width ? v->shape.width : 0;
	int16		typlen = v->type->typlen;
	int			checked = 0;
	int			removed = 0;

	if (!f->active)
		return 0;
	if (f->off > 0)
	{
		f->off--;
		f->off_batches++;
		return 0;
	}
	VEXEC_FOREACH_ROW(active, n, row)
	{
		int64		key;
		bool		isnull;

		if (redo != NULL && vexec_bit(redo, row))
			continue;
		checked++;
		if (width == 8 || width == 4 || width == 2)
		{
			isnull = v->validity != NULL && !vexec_bit(v->validity, row);
			key = width == 8 ? ((const int64 *) v->values)[row] :
				width == 4 ? ((const int32 *) v->values)[row] :
				((const int16 *) v->values)[row];
		}
		else
		{
			Datum		d = vexec_vec_datum(node->in, v, row, &isnull);

			key = typlen == 8 ? DatumGetInt64(d) :
				typlen == 4 ? DatumGetInt32(d) : DatumGetInt16(d);
		}
		if (isnull || !vexec_join_filter_has(f, key))
		{
			vexec_bit_clear(active, row);
			removed++;
		}
	}
	f->removed += removed;
	if (checked > 0 && (int64) (checked - removed) * 10 > (int64) checked * 7)
		f->off = VEXEC_JOIN_FILTER_OFF;
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
	/* a VecHashJoin's inner keys before the quals, where none may raise (X6) */
	if (node->jfilter != NULL && node->jfilter->before_quals && !node->jfilter_in_source)
		(void) apply_join_filter(node, active, NULL, n);
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
	/* or after them (X6; vexec_scan_set_join_filter() asks every one eager) */
	if (node->jfilter != NULL && !node->jfilter->before_quals)
		bounded += apply_join_filter(node, active, ev.redo, n);

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
 * A memo's input value, as the row holds it.  A varlena is compared by its
 * bytes as stored -- its header, a compressed value's compressed bytes, a
 * TOAST pointer -- never detoasted: what a function returns may depend on
 * how a value is stored, as pg_column_compression(), pg_column_size() and
 * pg_column_toast_chunk_id() do, so only a value stored alike is the same.
 */
static Datum
memo_input(VexecNode *node, VexecRowMemo *m, int row, bool *isnull)
{
	return vexec_vec_datum(node->work, &node->in->cols[m->col], row, isnull);
}

static uint32
memo_hash(VexecRowMemo *m, Datum d, bool isnull)
{
	if (isnull)
		return 0x9e3779b9;
	if (m->in_len == -1)
		return vexec_hash_bytes(DatumGetPointer(d), VARSIZE_ANY(DatumGetPointer(d)));
	if (m->in_byval)
		return murmurhash32((uint32) DatumGetUInt64(d) ^ (uint32) (DatumGetUInt64(d) >> 32));
	return vexec_hash_bytes(DatumGetPointer(d), m->in_len);
}

static bool
memo_equal(VexecRowMemo *m, const VexecRowMemoSlot *s, Datum d, bool isnull)
{
	if (s->in_null || isnull)
		return s->in_null && isnull;
	if (m->in_len == -1)
	{
		const char *a = DatumGetPointer(s->in);
		const char *b = DatumGetPointer(d);

		return VARSIZE_ANY(a) == VARSIZE_ANY(b) && memcmp(a, b, VARSIZE_ANY(a)) == 0;
	}
	if (m->in_byval)
		return s->in == d;
	return memcmp(DatumGetPointer(s->in), DatumGetPointer(d), m->in_len) == 0;
}

/*
 * Lazy target i for one row through its memo: a value met before takes its
 * result; another is computed, at this row, by PostgreSQL's evaluator, and
 * kept.
 */
static Datum
memo_eval(VexecNode *node, int i, int row, bool *isnull)
{
	VexecRowMemo *m = node->row_memo[i];
	ExprContext *econtext = node->css.ss.ps.ps_ExprContext;
	bool		in_null;
	Datum		in = memo_input(node, m, row, &in_null);
	uint32		hash = memo_hash(m, in, in_null);
	uint32		k;
	VexecRowMemoSlot *s;
	Datum		out;
	MemoryContext old;

	m->lookups++;
	for (k = hash & (MEMO_SLOTS - 1);; k = (k + 1) & (MEMO_SLOTS - 1))
	{
		s = &m->slots[k];
		if (!s->used)
			break;
		if (s->hash == hash && memo_equal(m, s, in, in_null))
		{
			m->hits++;
			node->stats.memo_rows++;
			*isnull = s->out_null;
			return s->out;
		}
	}

	vexec_node_load_input(node, row);
	set_input_slot(node, econtext, node->input_slot);
	out = ExecEvalExprSwitchContext(node->targets[i].state, econtext, isnull);

	if (m->lookups == MEMO_PROBATION && m->hits * 10 < m->lookups)
	{
		/* too few repeats to pay for the lookups: the memo stops */
		m->off = true;
		MemoryContextReset(m->cxt);
		return out;
	}
	if (m->nentries >= MEMO_ENTRIES)
	{
		MemoryContextReset(m->cxt);
		memset(m->slots, 0, sizeof(VexecRowMemoSlot) * MEMO_SLOTS);
		m->nentries = 0;
		s = &m->slots[hash & (MEMO_SLOTS - 1)];
	}
	old = MemoryContextSwitchTo(m->cxt);
	s->used = true;
	s->hash = hash;
	s->in_null = in_null;
	s->in = in_null ? (Datum) 0 : datumCopy(in, m->in_byval, m->in_len);
	s->out_null = *isnull;
	s->out = *isnull ? (Datum) 0 : datumCopy(out, m->out_byval, m->out_len);
	MemoryContextSwitchTo(old);
	m->nentries++;
	return out;
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
		else if (node->row_memo != NULL && node->row_memo[i] != NULL && !node->row_memo[i]->off)
			slot->tts_values[i] = memo_eval(node, i, row, &slot->tts_isnull[i]);
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
 * output: it hands up batches it makes itself (next_out), where it can.
 */
bool
vexec_node_batchable(VexecNode *node)
{
	if (node->next_out != NULL)
		return true;
	return node->kind != VEXEC_NODE_AGG && node->kind != VEXEC_NODE_WINDOW &&
		node->kind != VEXEC_NODE_MOTION_SEND &&
		node->first_lazy == node->nquals && !node->any_lazy_target &&
		!node->row_input;
}

/* X9: the batch the node made itself, as vexec_next_batch() hands it up. */
static VexecBatch *
next_made_batch(PlanState *ps)
{
	VexecNode  *node = (VexecNode *) ps;
	VexecBatch *out;

	node->ran = true;
	out = node->finished ? NULL : node->next_out(node);
	node->candidates = NULL;
	node->child_redo = NULL;
	if (out == NULL)
	{
		node->finished = true;
		node->redo = NULL;
		if (ps->instrument)
			InstrStopNode(ps->instrument, 0);
		return NULL;
	}
	out->selection = vexec_bitmap_alloc(out, out->nrows, true);
	/* none to redo: a NULL bitmap would be every row */
	node->redo = vexec_bitmap_alloc(out, out->nrows, false);
	node->stats.batches_out++;
	if (ps->instrument)
		InstrStopNode(ps->instrument, out->nrows);
	return out;
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
	if (node->next_out != NULL)
		return next_made_batch(ps);

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
		if (node->nshared > 0)
			ExplainPropertyInteger("Shared Steps", NULL, node->nshared, es);
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
	if (es->analyze && es->verbose && (node->ran || node->segments_seen > 0))
	{
		/*
		 * Where the segments ran it and the coordinator did not, their sums,
		 * as they reported them (motion/gpcore.c)
		 */
		const VexecNodeStats *st = node->ran ? &node->stats : &node->segment_stats;

		ExplainPropertyInteger("Batches", NULL, st->batches, es);
		if (st->lazy_rows > 0)
			ExplainPropertyInteger("Rows Evaluated Row by Row", NULL, st->lazy_rows, es);
		if (st->memo_rows > 0)
			ExplainPropertyInteger("Row-by-Row Results Reused", NULL, st->memo_rows, es);
		if (st->redo_rows > 0)
			ExplainPropertyInteger("Rows Sent to PostgreSQL", NULL, st->redo_rows, es);
		if (st->declared_rows > 0)
			ExplainPropertyInteger("Rows Through Declared Calls", NULL,
								   st->declared_rows, es);
		if (!node->ran)
			ExplainPropertyInteger("Segments Reporting", NULL, node->segments_seen, es);
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

/* ---------------------------------------------------------------------
 * pg_otel_probe's spans of vector nodes
 * ---------------------------------------------------------------------
 */

/*
 * What a vector node's span carries in pg_otel_probe's traces (its PLAN.md
 * §4.7.4, pg_otel_probe_api.h): the batches it read and the rows in them,
 * its kernels' steps against the rows the fallback evaluated -- and those
 * PostgreSQL's evaluator ran lazily, a kernel sent it, a kernel pack's
 * declared calls answered -- its batch format, and the share of its input
 * rows its quals passed.  A VecMotion's also its frames, their bytes and
 * the schema frames among them.  On the coordinator, a node the segments
 * ran carries their sums, as EXPLAIN ANALYZE prints them.  It reads the
 * node's fields alone, and so is safe on a failed statement's error path.
 */
static void
vexec_node_report(PlanState *ps, PgOtelProbeAttrs *attrs, int when)
{
	VexecNode  *node = (VexecNode *) ps;
	const VexecNodeStats *st = node->ran ? &node->stats : &node->segment_stats;

	attrs->add_string(attrs, "vexec.batch_format",
					  node->layout.format == VEXEC_FORMAT_ARROW ? "arrow" : "postgres");
	attrs->add_int(attrs, "vexec.kernels", node->nkernels);
	attrs->add_int(attrs, "vexec.fallbacks", node->nfallbacks);
	if (!node->ran && node->segments_seen == 0)
		return;
	attrs->add_int(attrs, "vexec.batches", st->batches);
	attrs->add_int(attrs, "vexec.rows_in", st->rows_in);
	if (st->batches > 0)
		attrs->add_double(attrs, "vexec.rows_per_batch", (double) st->rows_in / (double) st->batches);
	attrs->add_int(attrs, "vexec.kernel_steps", st->kernel_steps);
	attrs->add_int(attrs, "vexec.fallback_rows", st->fallback_rows);
	if (st->lazy_rows > 0)
		attrs->add_int(attrs, "vexec.lazy_rows", st->lazy_rows);
	if (st->memo_rows > 0)
		attrs->add_int(attrs, "vexec.memo_rows", st->memo_rows);
	if (st->redo_rows > 0)
		attrs->add_int(attrs, "vexec.redo_rows", st->redo_rows);
	if (st->declared_rows > 0)
		attrs->add_int(attrs, "vexec.declared_rows", st->declared_rows);
	if (st->batches_out > 0)
		attrs->add_int(attrs, "vexec.batches_out", st->batches_out);
	if (node->ran && node->nquals > 0 && ps->instrument != NULL && st->rows_in > 0)
		attrs->add_double(attrs, "vexec.selection_density",
						  ps->instrument->ntuples / (double) st->rows_in);
	if (!node->ran)
		attrs->add_int(attrs, "vexec.segments_reporting", node->segments_seen);
	if (node->kind == VEXEC_NODE_MOTION_SEND || node->kind == VEXEC_NODE_MOTION_RECV)
	{
		const VexecMotionStats *m = vexec_motion_stats(node);

		attrs->add_int(attrs, "vexec.motion.frames", m->frames);
		attrs->add_int(attrs, "vexec.motion.schema_frames", m->schema_frames);
		attrs->add_int(attrs, "vexec.motion.bytes", m->bytes);
		attrs->add_int(attrs, "vexec.motion.rows", m->rows);
		if (m->rows_by_postgres > 0)
			attrs->add_int(attrs, "vexec.motion.rows_by_postgres", m->rows_by_postgres);
	}
}

/*
 * The reporter of every kind of vector node, from _PG_init: an entry of
 * pg_otel_probe's registry, unused where pg_otel_probe is not loaded.
 */
void
vexec_report_install(void)
{
	const CustomExecMethods *(*const kinds[]) (void) = {
		vexec_scan_exec_methods,
		vexec_bitmapscan_exec_methods,
		vexec_result_exec_methods,
		vexec_agg_exec_methods,
		vexec_hashjoin_exec_methods,
		vexec_sort_exec_methods,
		vexec_repart_exec_methods,
		vexec_window_exec_methods,
		vexec_insert_exec_methods,
		vexec_ingest_exec_methods,
		vexec_motion_send_exec_methods,
		vexec_motion_recv_exec_methods,
	};

	for (int i = 0; i < lengthof(kinds); i++)
		pg_otel_probe_register_node_reporter(kinds[i](), vexec_node_report,
											 PG_OTEL_PROBE_ABORT_SAFE);
}
