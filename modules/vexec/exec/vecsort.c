/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecsort.c
 *	  VecSort: a sort, with a top-N bound, inside an island of vector
 *	  nodes (pg_vector_executor.md §3.8, V4).
 *
 * It is PostgreSQL's Sort (PG19:src/backend/executor/nodeSort.c) as a
 * vector node.  It sorts through tuplesort, as Sort does, which spills to
 * disk past work_mem, uses SortSupport and abbreviated keys, and radix-sorts
 * a leading key that compares as an integer; with a bound it keeps a top-N
 * heap.  What it adds is the island it keeps whole (§3.8): a vector child's
 * batches go into the sort without the child handing them up as rows, and
 * its sorted rows go out a batch at a time to a vector parent, as rows to a
 * row parent.
 *
 *	the input	the child's rows, read to their end the first time a row is
 *				asked for, as Sort reads them: a vector child's batches, its
 *				rows still to be decided resolved in row order through the
 *				child (vexec_resolve_row()); a row child's rows through
 *				ExecProcNode();
 *	the output	the sorted rows, a batch of them at a time: the node's input
 *				batches (exec.h), over which its target list -- a projection
 *				where the planner gave the node one -- is evaluated as any
 *				vector node's is.
 *
 * Its scan tuple, which custom_scan_tlist describes, is its child's row:
 * column k is the child's column k, and the node's target list reads it as
 * INDEX_VAR.  Its keys are the child's columns (custom_private,
 * vexec_sort_plan_encode()), with Sort's operators, collations and NULLS
 * FIRST flags.
 *
 * The bound.  ExecSetTupleBound() passes a LIMIT's bound to a Sort below
 * it, and to no CustomScan (PG19:src/backend/executor/execProcnode.c:
 * 829-952).  So the planner gives VecSort its bound when it plans it (§3.6,
 * "Bounds"): PostgreSQL's planner from the query's LIMIT, root->limit_tuples,
 * where the LIMIT is a count, with no tie or set-returning function after
 * the sort (plan/sort.c); ORCA's front end from a Limit of constants above
 * it.  With a bound, tuplesort keeps the bound's rows, as Sort's bounded
 * sort keeps them.
 *
 * Late columns (H6).  Where the planner made it so (plan/sort.c), the
 * child, a VecScan, gives only the keys and each row's TID: the sort keeps
 * them, and each row it gives out is fetched by its TID under the query's
 * snapshot, the scan's (table_tuple_fetch_row_version()), and the scan
 * tuple -- custom_scan_tlist, over the relation's Vars -- computed over it.
 * The row was visible to the scan under that snapshot, so it is still.
 *
 * The running bound (H6, §3.14).  Where the planner made it so
 * (plan/sort.c), the sort lends its child, a vector scan, the first key of
 * the N-th of the rows it holds (VexecSortBound, exec.h), and the scan
 * drops a row whose first key is strictly past it.  tuplesort keeps no such
 * key where one can read it, so the sort keeps its own: a heap of the
 * first keys of the N smallest rows put so far, which are the first keys
 * of the N rows tuplesort holds -- the N smallest rows' first keys are the
 * N smallest first keys -- and its top is that of tuplesort's N-th row.
 * It is set once the sort has put 2N+1 rows, the row at which tuplesort has
 * switched to its bounded heap (tuplesort.c, puttuple_common(), earlier
 * where memory runs out): from then on tuplesort discards, as it comes,
 * every row that compares at or past its N-th row (TSS_BOUNDED), which a
 * row whose first key is strictly past that row's first key does.  So the
 * rows the scan drops are rows tuplesort would have discarded unread, and
 * the sort keeps the rows it keeps without the bound, the order of their
 * arrival and the rows that tie at the bound's key among them; below 2N+1
 * rows nothing is dropped, and tuplesort's quicksort sees what it sees
 * without.  A row whose first key equals the bound's is never dropped: its
 * later keys decide it.
 *
 * Rescans, as ExecReScanSort(): the sorted rows are read again where the
 * plan may rewind and the child has no changed parameter, and sorted again
 * otherwise.  It runs neither backward nor marks: the planners give none of
 * vexec's nodes a plan that asks it to (§3.3.7).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ruleutils.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"

#include "cb_explain.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "expr/expr.h"

typedef struct VexecSortState
{
	VexecNode	node;			/* first */
	VexecSortPlan plan;
	PlanState  *child;
	TupleDesc	desc;			/* the child's rows */
	bool		child_batches;	/* the child hands batches */
	bool		random_access;	/* the plan may rewind it */
	TupleTableSlot *put_slot;	/* a vector child's row, into the sort */
	TupleTableSlot *get_slot;	/* a sorted row */
	Tuplesortstate *sort;
	bool		sorted;
	int64		rows_sorted;
	int64		rows_out;		/* read back from the sort */

	/* late columns (H6) */
	Relation	late_rel;		/* the child's relation */
	TupleTableSlot *late_slot;	/* a row fetched by its TID */
	ExprState **late_exprs;		/* the scan tuple's columns, over it */
	int			nlate;
	Datum	   *late_values;
	bool	   *late_nulls;

	/* the running bound (H6): a max-heap of the first keys, by their order */
	VexecSortBound *bound;		/* lent to the child; NULL: none */
	AttrNumber	bound_keycol;	/* the first key, a column of the child's rows */
	int16		key_typlen;
	bool		key_typbyval;
	Datum	   *kh_values;
	bool	   *kh_nulls;
	int			kh_count;
	int64		rows_put;		/* rows put since the sort began */
	MemoryContext kh_cxt;		/* the by-reference keys' copies */
} VexecSortState;

static bool sort_fetch(VexecNode *node);
static void sort_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *sort_exec(CustomScanState *css);
static void sort_end(CustomScanState *css);
static void sort_rescan(CustomScanState *css);
static void sort_explain(CustomScanState *css, List *ancestors, ExplainState *es);
static void bound_begin(VexecSortState *s);

static const CustomExecMethods sort_exec_methods = {
	.CustomName = VEXEC_SORT_NAME,
	.BeginCustomScan = sort_begin,
	.ExecCustomScan = sort_exec,
	.EndCustomScan = sort_end,
	.ReScanCustomScan = sort_rescan,
	.ExplainCustomScan = sort_explain,
};

Node *
vexec_create_sort_state(CustomScan *cscan)
{
	VexecSortState *s = palloc0(sizeof(VexecSortState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &sort_exec_methods;
	s->node.kind = VEXEC_NODE_SORT;
	return (Node *) s;
}

/*
 * The plan's custom_private: the keys, the child's columns from 1, their
 * operators, collations and NULLS FIRST flags; and the bound, 0 for none,
 * with the late columns and the running bound (H6).
 */
List *
vexec_sort_plan_encode(const VexecSortPlan *plan)
{
	List	   *nullsfirst = NIL;
	ListCell   *lc;

	foreach(lc, plan->nullsfirst)
		nullsfirst = lappend_int(nullsfirst, lfirst_int(lc) ? 1 : 0);
	return list_make5(list_copy(plan->keycols), list_copy(plan->operators),
					  list_copy(plan->collations), nullsfirst,
					  list_make5(makeInteger((int) Min(plan->bound, PG_INT32_MAX)),
								 makeInteger(plan->late_tidcol),
								 makeInteger((int) plan->late_relid),
								 makeInteger((int) plan->bound_attno),
								 makeInteger(plan->bound_before_quals ? 1 : 0)));
}

void
vexec_sort_plan_decode(CustomScan *cscan, VexecSortPlan *plan)
{
	List	   *p = cscan->custom_private;

	if (list_length(p) != 5)
		elog(ERROR, "vexec: a VecSort's plan of %d parts", list_length(p));
	plan->keycols = list_nth(p, 0);
	plan->operators = list_nth(p, 1);
	plan->collations = list_nth(p, 2);
	plan->nullsfirst = list_nth(p, 3);
	if (list_length((List *) list_nth(p, 4)) != 5)
		elog(ERROR, "vexec: a VecSort's plan of %d figures", list_length((List *) list_nth(p, 4)));
	plan->bound = intVal(linitial(list_nth(p, 4)));
	plan->late_tidcol = intVal(lsecond(list_nth(p, 4)));
	plan->late_relid = (Index) intVal(lthird(list_nth(p, 4)));
	plan->bound_attno = (AttrNumber) intVal(lfourth(list_nth(p, 4)));
	plan->bound_before_quals = intVal(list_nth((List *) list_nth(p, 4), 4)) != 0;
	if (list_length(plan->keycols) == 0 ||
		list_length(plan->operators) != list_length(plan->keycols) ||
		list_length(plan->collations) != list_length(plan->keycols) ||
		list_length(plan->nullsfirst) != list_length(plan->keycols))
		elog(ERROR, "vexec: a VecSort of %d keys and %d operators",
			 list_length(plan->keycols), list_length(plan->operators));
}

/* A VecSort's plan, its bound given: ORCA's front end's, from a Limit above. */
void
vexec_sort_set_bound(CustomScan *cscan, int64 bound)
{
	List	   *p = cscan->custom_private;

	Assert(cscan->methods == vexec_sort_methods());
	if (list_length(p) != 5)
		elog(ERROR, "vexec: a VecSort's plan of %d parts", list_length(p));
	linitial((List *) list_nth(p, 4)) = makeInteger((int) Min(Max(bound, 0), PG_INT32_MAX));
}

static void
sort_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecSortState *s = (VexecSortState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	TupleDesc	scandesc = css->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	VexecType **types;
	int			i;

	vexec_node_begin(node, estate);
	node->fetch = sort_fetch;
	node->label = "Vec Sort";
	vexec_sort_plan_decode(cscan, &s->plan);

	/*
	 * As ExecInitSort(): the sort is kept where the plan may rewind it, and
	 * the child, read once to its end, needs neither to rewind nor to mark.
	 */
	s->random_access = (eflags & (EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) != 0;
	eflags &= ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);
	s->child = ExecInitNode(outerPlan(cscan), estate, eflags);
	outerPlanState(css) = s->child;
	s->desc = ExecGetResultType(s->child);
	if (s->plan.late_tidcol > 0)
	{
		/* the child's relation, and each kept row fetched from it by TID */
		Scan	   *childscan = (Scan *) outerPlan(cscan);
		ListCell   *lc;

		if (!IsA(childscan, CustomScan) || childscan->scanrelid == 0 ||
			s->plan.late_tidcol > s->desc->natts)
			elog(ERROR, "vexec: a VecSort of late columns over no scan");
		s->late_rel = ExecOpenScanRelation(estate, childscan->scanrelid, eflags);
		s->late_slot = ExecInitExtraTupleSlot(estate, RelationGetDescr(s->late_rel),
											  table_slot_callbacks(s->late_rel));
		s->nlate = list_length(cscan->custom_scan_tlist);
		s->late_exprs = palloc(sizeof(ExprState *) * Max(s->nlate, 1));
		s->late_values = palloc(sizeof(Datum) * Max(s->nlate, 1));
		s->late_nulls = palloc(sizeof(bool) * Max(s->nlate, 1));
		/*
		 * Compiled with no parent: the node's scan slot is virtual, and the
		 * fetched row is the access method's own slot, which a program the
		 * parent's slot types shaped would misread (ExecComputeSlotInfo()).
		 * The planner makes no late columns over a SubPlan, which needs one.
		 */
		foreach(lc, cscan->custom_scan_tlist)
			s->late_exprs[foreach_current_index(lc)] =
				ExecInitExpr(lfirst_node(TargetEntry, lc)->expr, NULL);
	}
	else if (scandesc->natts != s->desc->natts)
		elog(ERROR, "vexec: a VecSort's scan tuple of %d columns, its child's %d",
			 scandesc->natts, s->desc->natts);

	/* the scan tuple: the child's row */
	types = palloc(sizeof(VexecType *) * Max(scandesc->natts, 1));
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(scandesc->natts, 1));
	for (i = 0; i < scandesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(scandesc, i);

		types[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		node->input_attnos[i] = i + 1;
	}
	node->input_varno = INDEX_VAR;
	node->ninput = scandesc->natts;
	node->input_slot = css->ss.ss_ScanTupleSlot;
	node->in = vexec_batch_create(node->mcxt, scandesc->natts, types);
	vexec_node_compile(node, cscan->scan.plan.qual, cscan->scan.plan.targetlist);

	s->child_batches = vexec_is_vector_state(s->child) &&
		vexec_node_batchable((VexecNode *) s->child);
	s->put_slot = ExecInitExtraTupleSlot(estate, s->desc, &TTSOpsVirtual);
	s->get_slot = ExecInitExtraTupleSlot(estate, s->desc, &TTSOpsMinimalTuple);
	if (s->plan.bound_attno > 0 && s->plan.bound > 0)
		bound_begin(s);
}

/* ---------------------------------------------------------------------
 * The running bound (H6, above)
 * ---------------------------------------------------------------------
 */

/* The bound, lent to the child where it is a vector scan of the column. */
static void
bound_begin(VexecSortState *s)
{
	VexecSortBound *b;
	Form_pg_attribute att;

	s->bound_keycol = (AttrNumber) linitial_int(s->plan.keycols);
	att = TupleDescAttr(s->desc, s->bound_keycol - 1);
	b = palloc0(sizeof(VexecSortBound));
	b->attno = s->plan.bound_attno;
	b->before_quals = s->plan.bound_before_quals;
	b->sortop = linitial_oid(s->plan.operators);
	b->ssup.ssup_cxt = CurrentMemoryContext;
	b->ssup.ssup_collation = linitial_oid(s->plan.collations);
	b->ssup.ssup_nulls_first = linitial_int(s->plan.nullsfirst) != 0;
	b->ssup.ssup_attno = s->bound_keycol;
	b->ssup.abbreviate = false;
	PrepareSortSupportFromOrderingOp(linitial_oid(s->plan.operators), &b->ssup);
	b->tmpcxt = AllocSetContextCreate(CurrentMemoryContext, "vexec running bound",
									  ALLOCSET_SMALL_SIZES);
	if (!vexec_scan_set_bound(s->child, b))
		return;					/* the child reads no such column: no bound */
	s->bound = b;
	s->key_typlen = att->attlen;
	s->key_typbyval = att->attbyval;
	s->kh_count = 0;
	s->rows_put = 0;
	/* the heap's arrays once a row is put: EXPLAIN alone puts none */
	s->kh_cxt = AllocSetContextCreate(CurrentMemoryContext, "vexec running bound keys",
									  ALLOCSET_DEFAULT_SIZES);
}

/* The heap's order: its top is the greatest of the keys, by the first key's. */
static inline int
kh_compare(VexecSortState *s, int i, int j)
{
	return ApplySortComparator(s->kh_values[i], s->kh_nulls[i],
							   s->kh_values[j], s->kh_nulls[j], &s->bound->ssup);
}

static inline void
kh_swap(VexecSortState *s, int i, int j)
{
	Datum		v = s->kh_values[i];
	bool		n = s->kh_nulls[i];

	s->kh_values[i] = s->kh_values[j];
	s->kh_nulls[i] = s->kh_nulls[j];
	s->kh_values[j] = v;
	s->kh_nulls[j] = n;
}

static void
kh_sift_down(VexecSortState *s, int i)
{
	for (;;)
	{
		int			l = 2 * i + 1;
		int			r = l + 1;
		int			top = i;

		if (l < s->kh_count && kh_compare(s, l, top) > 0)
			top = l;
		if (r < s->kh_count && kh_compare(s, r, top) > 0)
			top = r;
		if (top == i)
			return;
		kh_swap(s, i, top);
		i = top;
	}
}

static void
kh_sift_up(VexecSortState *s, int i)
{
	while (i > 0)
	{
		int			parent = (i - 1) / 2;

		if (kh_compare(s, i, parent) <= 0)
			return;
		kh_swap(s, i, parent);
		i = parent;
	}
}

/*
 * A row put into the sort: its first key into the heap, and, past 2N rows,
 * the heap's top lent to the scan as the bound.
 */
static void
bound_note(VexecSortState *s, TupleTableSlot *slot)
{
	VexecSortBound *b = s->bound;
	int			n = (int) s->plan.bound;
	bool		isnull;
	Datum		d = slot_getattr(slot, s->bound_keycol, &isnull);
	MemoryContext old;
	bool		moved = false;

	if (s->kh_values == NULL)
	{
		s->kh_values = MemoryContextAlloc(s->node.mcxt, sizeof(Datum) * n);
		s->kh_nulls = MemoryContextAlloc(s->node.mcxt, sizeof(bool) * n);
	}

	/* the comparisons' detoasted values in the bound's short-lived context */
	old = MemoryContextSwitchTo(b->tmpcxt);
	if (s->kh_count < n)
	{
		MemoryContextSwitchTo(s->kh_cxt);
		s->kh_values[s->kh_count] = isnull ? (Datum) 0 :
			datumCopy(d, s->key_typbyval, s->key_typlen);
		MemoryContextSwitchTo(b->tmpcxt);
		s->kh_nulls[s->kh_count] = isnull;
		s->kh_count++;
		kh_sift_up(s, s->kh_count - 1);
	}
	else if (ApplySortComparator(d, isnull, s->kh_values[0], s->kh_nulls[0], &b->ssup) < 0)
	{
		if (!s->key_typbyval && !s->kh_nulls[0])
			pfree(DatumGetPointer(s->kh_values[0]));
		MemoryContextSwitchTo(s->kh_cxt);
		s->kh_values[0] = isnull ? (Datum) 0 : datumCopy(d, s->key_typbyval, s->key_typlen);
		MemoryContextSwitchTo(b->tmpcxt);
		s->kh_nulls[0] = isnull;
		kh_sift_down(s, 0);
		moved = true;
	}
	MemoryContextSwitchTo(old);
	s->rows_put++;
	if (s->rows_put % VEXEC_BATCH_ROWS == 0)
		MemoryContextReset(b->tmpcxt);
	if (s->rows_put > 2 * (int64) n)
	{
		if (!b->active || moved)
			b->version++;
		b->value = s->kh_values[0];
		b->isnull = s->kh_nulls[0];
		b->active = true;
	}
}

/* The bound forgotten: the sort is made again. */
static void
bound_reset(VexecSortState *s)
{
	if (s->bound == NULL)
		return;
	s->bound->active = false;
	s->kh_count = 0;
	s->rows_put = 0;
	MemoryContextReset(s->kh_cxt);
}

/* A vector child's batch into the sort, its rows in order. */
static void
put_batch(VexecSortState *s, VexecNode *child, VexecBatch *b)
{
	int			row;

	for (row = 0; row < b->nrows; row++)
	{
		TupleTableSlot *slot;

		if ((child->redo != NULL && vexec_bit(child->redo, row)) ||
			(child->child_redo != NULL && vexec_bit(child->child_redo, row)))
		{
			/* a row the child's kernels left: its own evaluator decides it */
			slot = vexec_resolve_row(child, row);
			if (slot == NULL)
				continue;
		}
		else if (b->selection == NULL || vexec_bit(b->selection, row))
		{
			vexec_batch_store_row(b, row, s->put_slot);
			slot = s->put_slot;
		}
		else
			continue;
		tuplesort_puttupleslot(s->sort, slot);
		s->rows_sorted++;
		if (s->bound != NULL)
			bound_note(s, slot);
	}
}

/* The child read to its end into tuplesort, and sorted (ExecSort()). */
static void
do_sort(VexecSortState *s)
{
	EState	   *estate = s->node.css.ss.ps.state;
	ScanDirection dir = estate->es_direction;
	int			nkeys = list_length(s->plan.keycols);
	AttrNumber *cols = palloc(sizeof(AttrNumber) * nkeys);
	Oid		   *ops = palloc(sizeof(Oid) * nkeys);
	Oid		   *colls = palloc(sizeof(Oid) * nkeys);
	bool	   *nulls = palloc(sizeof(bool) * nkeys);
	int			opts = TUPLESORT_NONE;
	int			i;

	for (i = 0; i < nkeys; i++)
	{
		cols[i] = (AttrNumber) list_nth_int(s->plan.keycols, i);
		ops[i] = list_nth_oid(s->plan.operators, i);
		colls[i] = list_nth_oid(s->plan.collations, i);
		nulls[i] = list_nth_int(s->plan.nullsfirst, i) != 0;
	}
	if (s->random_access)
		opts |= TUPLESORT_RANDOMACCESS;
	if (s->plan.bound > 0)
		opts |= TUPLESORT_ALLOWBOUNDED;

	/* the child is read forward while the sort is made, as Sort reads it */
	estate->es_direction = ForwardScanDirection;
	s->sort = tuplesort_begin_heap(s->desc, nkeys, cols, ops, colls, nulls,
								   work_mem, NULL, opts);
	if (s->plan.bound > 0)
		tuplesort_set_bound(s->sort, s->plan.bound);
	s->rows_sorted = 0;
	s->rows_out = 0;
	bound_reset(s);

	if (s->child_batches)
	{
		VexecNode  *child = (VexecNode *) s->child;
		VexecBatch *b;

		while ((b = vexec_next_batch(s->child)) != NULL)
			put_batch(s, child, b);
	}
	else
	{
		for (;;)
		{
			TupleTableSlot *slot = ExecProcNode(s->child);

			if (TupIsNull(slot))
				break;
			tuplesort_puttupleslot(s->sort, slot);
			s->rows_sorted++;
			if (s->bound != NULL)
				bound_note(s, slot);
		}
	}
	tuplesort_performsort(s->sort);
	/* the scan is read to its end: no bound for a rescan's until it is made */
	if (s->bound != NULL)
		s->bound->active = false;
	estate->es_direction = dir;
	s->sorted = true;
}

/*
 * A kept row of late columns: fetched by its TID, and its scan tuple's
 * columns computed over it into the batch.
 */
static void
add_late_row(VexecSortState *s, VexecBatch *in)
{
	EState	   *estate = s->node.css.ss.ps.state;
	ExprContext *econtext = s->node.css.ss.ps.ps_ExprContext;
	bool		isnull;
	Datum		tid = slot_getattr(s->get_slot, s->plan.late_tidcol, &isnull);
	int			i;

	if (isnull ||
		!table_tuple_fetch_row_version(s->late_rel, (ItemPointer) DatumGetPointer(tid),
									   estate->es_snapshot, s->late_slot))
		elog(ERROR, "vexec: a row a sort kept is not visible by its TID");
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = s->late_slot;
	for (i = 0; i < s->nlate; i++)
		s->late_values[i] = ExecEvalExprSwitchContext(s->late_exprs[i], econtext,
													  &s->late_nulls[i]);
	vexec_batch_add_values(in, s->late_values, s->late_nulls);
	ExecClearTuple(s->late_slot);
}

/*
 * The next batch of sorted rows, as the node's input batch.  A bounded sort
 * gives no more than its bound's rows -- tuplesort refuses the next
 * (tuplesort_gettuple_common()) -- which are all the Limit above reads.
 */
static bool
sort_fetch(VexecNode *node)
{
	VexecSortState *s = (VexecSortState *) node;
	VexecBatch *in = node->in;

	if (!s->sorted)
		do_sort(s);

	vexec_batch_reset(in);
	vexec_batch_begin_rows(in);
	while (in->nrows < VEXEC_BATCH_ROWS &&
		   (s->plan.bound <= 0 || s->rows_out < s->plan.bound) &&
		   tuplesort_gettupleslot(s->sort, true, false, s->get_slot, NULL))
	{
		if (s->late_rel != NULL)
			add_late_row(s, in);
		else
			vexec_batch_add_slot(in, s->get_slot, NULL);
		s->rows_out++;
	}
	ExecClearTuple(s->get_slot);
	if (in->nrows == 0)
		return false;
	vexec_batch_apply_config(in, &node->layout);
	return true;
}

static TupleTableSlot *
sort_exec(CustomScanState *css)
{
	return vexec_node_exec(&((VexecSortState *) css)->node);
}

/* As ExecReScanSort(). */
static void
sort_rescan(CustomScanState *css)
{
	VexecSortState *s = (VexecSortState *) css;

	vexec_node_rescan(&s->node);
	if (!s->sorted)
		return;
	ExecClearTuple(s->get_slot);
	if (s->child->chgParam != NULL || !s->random_access)
	{
		s->sorted = false;
		tuplesort_end(s->sort);
		s->sort = NULL;
		if (s->child->chgParam == NULL)
			ExecReScan(s->child);
	}
	else
	{
		tuplesort_rescan(s->sort);
		s->rows_out = 0;
	}
}

static void
sort_end(CustomScanState *css)
{
	VexecSortState *s = (VexecSortState *) css;

	if (s->sort != NULL)
		tuplesort_end(s->sort);
	s->sort = NULL;
	vexec_node_end(&s->node);
	ExecEndNode(s->child);
}

/*
 * A sort key's options, as EXPLAIN gives a Sort's (explain.c,
 * show_sortorder_options(), which is static there): COLLATE where it is not
 * the type's, DESC or USING, and NULLS FIRST or LAST where it is not the
 * direction's default.
 */
static void
sort_key_options(StringInfo buf, Oid keytype, Oid sortop, Oid collation, bool nullsfirst)
{
	TypeCacheEntry *typentry = lookup_type_cache(keytype, TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
	bool		reverse = false;

	if (OidIsValid(collation) && collation != get_typcollation(keytype))
	{
		char	   *collname = get_collation_name(collation);

		if (collname == NULL)
			elog(ERROR, "cache lookup failed for collation %u", collation);
		appendStringInfo(buf, " COLLATE %s", quote_identifier(collname));
	}
	if (sortop == typentry->gt_opr)
	{
		appendStringInfoString(buf, " DESC");
		reverse = true;
	}
	else if (sortop != typentry->lt_opr)
	{
		char	   *opname = get_opname(sortop);

		if (opname == NULL)
			elog(ERROR, "cache lookup failed for operator %u", sortop);
		appendStringInfo(buf, " USING %s", opname);
		(void) get_equality_op_for_ordering_op(sortop, &reverse);
	}
	if (nullsfirst && !reverse)
		appendStringInfoString(buf, " NULLS FIRST");
	else if (!nullsfirst && reverse)
		appendStringInfoString(buf, " NULLS LAST");
}

static void
sort_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecSortState *s = (VexecSortState *) css;
	List	   *context = set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan, ancestors);
	List	   *keys = NIL;
	int			i;

	CbExplainRelabel(css, es, "Vec Sort", NULL);
	for (i = 0; i < list_length(s->plan.keycols); i++)
	{
		int			col = list_nth_int(s->plan.keycols, i);
		Form_pg_attribute att = TupleDescAttr(s->desc, col - 1);
		Var		   *v = makeVar(OUTER_VAR, col, att->atttypid, att->atttypmod,
								att->attcollation, 0);
		StringInfoData buf;

		initStringInfo(&buf);
		appendStringInfoString(&buf, deparse_expression((Node *) v, context, es->verbose, false));
		sort_key_options(&buf, att->atttypid, list_nth_oid(s->plan.operators, i),
						 list_nth_oid(s->plan.collations, i),
						 list_nth_int(s->plan.nullsfirst, i) != 0);
		keys = lappend(keys, buf.data);
	}
	ExplainPropertyList("Sort Key", keys, es);
	if (s->plan.bound > 0)
		ExplainPropertyInteger("Bound", NULL, s->plan.bound, es);
	if (s->late_rel != NULL)
		ExplainPropertyText("Columns", "the keys and TIDs sorted, the rest fetched by TID", es);
	if (es->verbose && s->plan.bound_attno > 0)
		ExplainPropertyText("Running Bound",
							s->plan.bound_before_quals ? "the first key, before the scan's quals" :
							"the first key, after the scan's quals", es);
	vexec_node_explain_properties(&s->node, ancestors, es);
	if (es->verbose)
		ExplainPropertyText("Input", s->child_batches ? "batches" : "rows", es);

	/* where it ran: on a cluster, not on the coordinator for a segment's */
	if (es->analyze && s->node.ran && s->sorted && s->sort != NULL)
	{
		TuplesortInstrumentation stats;

		tuplesort_get_stats(s->sort, &stats);
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Sort Method: %s  %s: " INT64_FORMAT "kB\n",
							 tuplesort_method_name(stats.sortMethod),
							 tuplesort_space_type_name(stats.spaceType),
							 stats.spaceUsed);
		}
		else
		{
			ExplainPropertyText("Sort Method", tuplesort_method_name(stats.sortMethod), es);
			ExplainPropertyInteger("Sort Space Used", "kB", stats.spaceUsed, es);
			ExplainPropertyText("Sort Space Type", tuplesort_space_type_name(stats.spaceType), es);
		}
		if (es->verbose)
			ExplainPropertyInteger("Rows Sorted", NULL, s->rows_sorted, es);
		if (es->verbose && s->late_rel != NULL)
			ExplainPropertyInteger("Rows Fetched by TID", NULL, s->rows_out, es);
	}
}
