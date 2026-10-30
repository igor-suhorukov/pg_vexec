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
 * The row was visible to the scan under that snapshot, so it is still.  A
 * source that decodes a column store's granules for a fetch is told the
 * columns the scan tuple reads (vexec_source.h's fetch_columns(), minor 4,
 * VC16), which it then decodes alone, the slot's other attributes NULL.
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
 * The device's order (V8d, §3.11).  An unbounded sort whose keys the
 * device orders as their operators do keeps its vector child's batches,
 * copies of them, sends their rows' keys to the device in chunks, and
 * gathers its sorted rows from the kept batches by the order the device
 * answers -- unless two rows tie on every key, or the device cannot go on,
 * where tuplesort sorts the kept rows as it would have sorted them
 * (below).
 *
 * Rescans, as ExecReScanSort(): the sorted rows are read again where the
 * plan may rewind and the child has no changed parameter, and sorted again
 * otherwise.  It runs neither backward nor marks: the planners give none of
 * vexec's nodes a plan that asks it to (§3.3.7).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/ruleutils.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"
#include "utils/wait_event.h"

#include "cb_explain.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/device.h"
#include "exec/exec.h"
#include "expr/expr.h"
#include "source/source.h"

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

	/*
	 * The participants' bound (below): a first key of an integer, a date or
	 * a timestamp, in its type's own order, shared through the parallel
	 * VecScan's DSM
	 */
	int			share_len;		/* the key's bytes, 2, 4 or 8; 0: none */
	bool		share_desc;		/* its order descending */
	pg_atomic_uint64 *shared;	/* the scan's, once its DSM is made */
	bool		out_done;		/* a kept row past it: the rest are too */
	int64		rows_unfetched; /* kept rows not fetched for it */

	/*
	 * M4 (§3.21, below): the late columns of a parallel sort fetched for the
	 * rows the whole sort can return alone, by whichever participant is free
	 */
	VexecLateShare *coop;		/* the scan's exchange; NULL: as H6 */
	bool		coop_late;		/* came once the others had published */
	bool		coop_ready;		/* the candidates fetched */
	int			coop_me;		/* this participant's slot: the leader 0 */
	int			coop_n;			/* its rows published */
	int			coop_out;		/* the next of them to give out */
	uint64		coop_boundary;	/* the least first key past which no row is
								 * the sort's */
	dsa_area   *coop_dsa;		/* the query's, where fetched rows go */
	TupleTableSlot *coop_slot;	/* a fetched row, read back */
	int64		coop_fetched;	/* rows this participant fetched, any one's */

	/* V8d: the device's order (below); NULL: tuplesort's alone */
	struct SortDevice *dev;
	const char *dev_why;		/* why not, for EXPLAIN */
} VexecSortState;

/* the wait at the late columns' barrier, as pg_stat_activity names it */
static uint32 sort_wait_event = 0;

static bool sort_fetch(VexecNode *node);
static void sort_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *sort_exec(CustomScanState *css);
static void sort_end(CustomScanState *css);
static void sort_rescan(CustomScanState *css);
static void sort_explain(CustomScanState *css, List *ancestors, ExplainState *es);
static void bound_begin(VexecSortState *s);
static void dev_setup(VexecSortState *s);
static void dev_reset(VexecSortState *s);

static const CustomExecMethods sort_exec_methods = {
	.CustomName = VEXEC_SORT_NAME,
	.BeginCustomScan = sort_begin,
	.ExecCustomScan = sort_exec,
	.EndCustomScan = sort_end,
	.ReScanCustomScan = sort_rescan,
	.ExplainCustomScan = sort_explain,
};

/* Its methods, for pg_otel_probe's reporter of vector nodes (node.c). */
const CustomExecMethods *
vexec_sort_exec_methods(void)
{
	return &sort_exec_methods;
}

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
 * with the late columns and the running bound (H6), and whether the
 * device's order was planned (V8d).
 */
List *
vexec_sort_plan_encode(const VexecSortPlan *plan)
{
	List	   *nullsfirst = NIL;
	ListCell   *lc;
	List	   *figures;

	foreach(lc, plan->nullsfirst)
		nullsfirst = lappend_int(nullsfirst, lfirst_int(lc) ? 1 : 0);
	figures = list_make5(makeInteger((int) Min(plan->bound, PG_INT32_MAX)),
						 makeInteger(plan->late_tidcol),
						 makeInteger((int) plan->late_relid),
						 makeInteger((int) plan->bound_attno),
						 makeInteger(plan->bound_before_quals ? 1 : 0));
	figures = lappend(figures, makeInteger(plan->device ? 1 : 0));
	return list_make5(list_copy(plan->keycols), list_copy(plan->operators),
					  list_copy(plan->collations), nullsfirst, figures);
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
	if (list_length((List *) list_nth(p, 4)) != 6)
		elog(ERROR, "vexec: a VecSort's plan of %d figures", list_length((List *) list_nth(p, 4)));
	plan->bound = intVal(linitial(list_nth(p, 4)));
	plan->late_tidcol = intVal(lsecond(list_nth(p, 4)));
	plan->late_relid = (Index) intVal(lthird(list_nth(p, 4)));
	plan->bound_attno = (AttrNumber) intVal(lfourth(list_nth(p, 4)));
	plan->bound_before_quals = intVal(list_nth((List *) list_nth(p, 4), 4)) != 0;
	plan->device = intVal(list_nth((List *) list_nth(p, 4), 5)) != 0;
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

/*
 * VC16: the late columns a fetch into the late slot decodes, where the
 * relation's source takes them (fetch_columns(), minor 4): the attributes
 * the scan tuple reads; every one for a whole row.
 */
static void
late_columns(VexecSortState *s, CustomScan *cscan, Index relid)
{
	const char *how;
	const VexecSourceRoutine *src = vexec_source_for(s->late_rel, &how);
	Bitmapset  *attrs = NULL;
	AttrNumber *attnums;
	int			n = 0;
	int			x = -1;

	if (src == NULL || !VEXEC_SOURCE_HAS(src, fetch_columns))
		return;
	pull_varattnos((Node *) cscan->custom_scan_tlist, relid, &attrs);
	if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs))
		return;
	attnums = palloc(sizeof(AttrNumber) * Max(bms_num_members(attrs), 1));
	while ((x = bms_next_member(attrs, x)) >= 0)
	{
		AttrNumber	attno = x + FirstLowInvalidHeapAttributeNumber;

		if (attno > 0)
			attnums[n++] = attno;
	}
	src->fetch_columns(s->late_rel, s->late_slot, n, attnums);
	pfree(attnums);
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
		late_columns(s, cscan, childscan->scanrelid);
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

	/*
	 * M4: a bounded sort of late columns over a parallel VecScan, its first
	 * key shared: the scan's DSM gets an exchange for its participants' rows
	 */
	if (s->late_rel != NULL && s->share_len > 0 && s->plan.bound > 0 &&
		s->plan.bound <= VEXEC_BATCH_ROWS && !s->random_access && vexec_enable_late_exchange)
	{
		vexec_scan_reserve_late(s->child, (int) s->plan.bound);
		s->coop_slot = ExecInitExtraTupleSlot(estate, scandesc, &TTSOpsMinimalTuple);
	}

	/*
	 * C3: a bounded sort over a VecAgg lends it its keys and bound; the
	 * aggregation keeps its n best groups, which this sort orders
	 * (vecagg.c).  Its keys are columns of the child's rows, the scan tuple.
	 */
	if (s->plan.bound > 0 && s->plan.late_tidcol == 0 && vexec_is_vector_state(s->child))
		(void) vexec_agg_set_topn((VexecNode *) s->child, s->plan.bound, s->plan.keycols,
								  s->plan.operators, s->plan.collations, s->plan.nullsfirst);

	/* V8d: the device's order, where the node may take it */
	dev_setup(s);
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

	/* the participants' bound: by the type's own < or > */
	switch (att->atttypid)
	{
		case INT2OID:
			s->share_len = 2;
			break;
		case INT4OID:
		case DATEOID:
			s->share_len = 4;
			break;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			s->share_len = 8;
			break;
		default:
			s->share_len = 0;
			break;
	}
	if (s->share_len > 0)
	{
		TypeCacheEntry *tc = lookup_type_cache(att->atttypid, TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

		if (b->sortop == tc->lt_opr)
			s->share_desc = false;
		else if (b->sortop == tc->gt_opr)
			s->share_desc = true;
		else
			s->share_len = 0;
	}
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
 * The participants' bound.  Under a Gather Merge each participant sorts
 * its own rows, bounded at the LIMIT's N, over a parallel VecScan; a
 * participant whose heap holds N first keys keeps N rows within its top,
 * so no row whose first key is strictly past that top is among the N the
 * Gather Merge returns, in whatever participant it is -- and the least of
 * the participants' tops bounds them all.  Each one makes its top the
 * shared one where it is less (share_publish()), lends its scan the shared
 * one where that is less than its own, and gives out none of its kept rows
 * past it: the rows whose first key equals it stay, as the bound of one
 * sort keeps them.  Over late columns that saves their fetches, each a
 * granule read in a column store: of ClickBench's Q23 on 16 workers, 170
 * fetches for the LIMIT's 10.  A key is encoded so that less is better:
 * its bits as unsigned, the sign flipped, and complemented for a
 * descending order.
 */
static inline uint64
share_encode(VexecSortState *s, Datum d)
{
	int64		v = s->share_len == 2 ? (int64) DatumGetInt16(d) :
		s->share_len == 4 ? (int64) DatumGetInt32(d) : DatumGetInt64(d);
	uint64		u = (uint64) v ^ (UINT64CONST(1) << 63);

	return s->share_desc ? ~u : u;
}

static inline Datum
share_decode(VexecSortState *s, uint64 u)
{
	int64		v = (int64) ((s->share_desc ? ~u : u) ^ (UINT64CONST(1) << 63));

	return s->share_len == 2 ? Int16GetDatum((int16) v) :
		s->share_len == 4 ? Int32GetDatum((int32) v) : Int64GetDatum(v);
}

/* The shared bound, PG_UINT64_MAX for none: the scan's DSM, once made */
static uint64
share_read(VexecSortState *s)
{
	if (s->share_len == 0)
		return PG_UINT64_MAX;
	if (s->shared == NULL && (s->shared = vexec_scan_shared_bound(s->child)) == NULL)
		return PG_UINT64_MAX;
	return pg_atomic_read_u64(s->shared);
}

/* This participant's top, made the shared bound where it is less. */
static void
share_publish(VexecSortState *s, Datum top)
{
	uint64		u = share_encode(s, top);
	uint64		cur = share_read(s);

	if (s->shared == NULL)
		return;
	while (u < cur && !pg_atomic_compare_exchange_u64(s->shared, &cur, u))
		;
}

/* Whether a kept row, sorted, is past the shared bound: its first key NULL never is. */
static bool
share_past(VexecSortState *s, TupleTableSlot *slot)
{
	bool		isnull;
	Datum		d;
	uint64		u = share_read(s);

	if (u == PG_UINT64_MAX)
		return false;
	d = slot_getattr(slot, s->bound_keycol, &isnull);
	return !isnull && share_encode(s, d) > u;
}

/*
 * A row put into the sort: its first key into the heap, and, past 2N rows,
 * the heap's top lent to the scan as the bound -- or the participants'
 * bound, where it is less.
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
	if (s->rows_put % vexec_batch_rows == 0)
		MemoryContextReset(b->tmpcxt);
	if (s->share_len > 0 && s->kh_count == n && !s->kh_nulls[0] &&
		(moved || s->rows_put == n))
		share_publish(s, s->kh_values[0]);
	if (s->rows_put > 2 * (int64) n)
	{
		Datum		top = s->kh_values[0];
		bool		top_null = s->kh_nulls[0];

		if (s->share_len > 0 && !top_null)
		{
			uint64		u = share_read(s);

			if (u < share_encode(s, top))
				top = share_decode(s, u);
		}
		if (!b->active || b->isnull != top_null || (!top_null && b->value != top) || moved)
			b->version++;
		b->value = top;
		b->isnull = top_null;
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

/* ---------------------------------------------------------------------
 * The device's order (V8d, §3.11)
 *
 * An unbounded sort whose keys the device orders as their operators do
 * (vexec_sort_device_key()) keeps its vector child's batches as they come
 * -- copies of them, their selected rows alone where few are -- and sends
 * their rows' keys to the device in chunks, each row's id beside them: its
 * kept batch, and its row there.  At the input's end the device answers
 * the ids in the keys' order, and the sorted rows are gathered from the
 * kept batches by them, a batch at a time: no tuple is formed here, and no
 * key compared.  Two rows that tie on every key come out of tuplesort in an
 * order of its own, which the device cannot reproduce: the device says so
 * and gives no order, and tuplesort sorts the rows -- the kept batches put
 * into it in the order they came, as the CPU's strategy would have put
 * them, and the rest of the input after them.  So it is too where a batch
 * holds rows its child's kernels left, which the child decides before its
 * next batch; where the kept rows' memory passes work_mem, past which
 * tuplesort would spill them; and where the device fails.  Either way the
 * rows are the same, in the same order.  A bounded sort keeps tuplesort's
 * heap: which of the rows tying at its bound it keeps is the heap's.
 * ---------------------------------------------------------------------
 */

/* A row's id: its kept batch, and its row there. */
#define SORT_ID_SHIFT		12
#define SORT_ID_ROW_MASK	(VEXEC_BATCH_ROWS - 1)
#define SORT_ID_MAX_BATCHES	(1 << (31 - SORT_ID_SHIFT))

StaticAssertDecl(VEXEC_BATCH_ROWS == 1 << SORT_ID_SHIFT,
				 "a sort row's id holds its batch's row in its low bits");

typedef struct SortDevice
{
	VexecDevTask *t;
	int			nkeys;
	int		   *keycol;			/* the keys' columns of the child's rows,
								 * from 0 */
	VexecDevColType *types;		/* the keys', then the ids' */
	void	   *program;
	Size		proglen;
	uint32		cap;			/* a chunk's rows */
	uint64	   *strings;
	VexecType  *idtype;			/* int4 */
	MemoryContext keepcxt;		/* the kept batches */
	MemoryContext tmpcxt;		/* a batch's detoasted keys, a compaction */
	VexecBatch **kept;			/* in the order they came */
	int			nkept;
	int			maxkept;
	Size		kept_bytes;
	VexecDevFill *fill;			/* the chunk being filled, */
	VexecDevFill *flight;		/* and the one the device is taking */
	uint64		sent;			/* rows of the chunks submitted */
	uint32		page;			/* the answer's block */
	bool		started;
	bool		lost;			/* tuplesort's from here */
	const char *why;			/* what lost it */
	const char *device;
	int32	   *order;			/* the ids, in the keys' order */
	int64		norder;
	int64		pos;			/* the next given out */
	bool		ordered;		/* the rows are given from order[] */
	VexecBatch **src;			/* a gather's rows: their kept batches, */
	int		   *rows;			/* and their rows there */

	/* EXPLAIN ANALYZE's */
	int64		poured;			/* kept rows put into tuplesort */
	Size		peak_bytes;
	VexecDevFigures fig;
} SortDevice;

/*
 * V8d: whether the device orders a sort key as its operator does -- NULL
 * where it does, *descending set -- else why not.  Integers, OIDs, dates,
 * times, timestamps and money by their values, booleans false first,
 * scaled numerics by their scaled integers, floats with NaN after the
 * infinities and -0 equal to 0, as PostgreSQL orders them (measured); bytea
 * and, under C's collation, text by their bytes, unsigned, the shorter
 * first.  "char" PostgreSQL compares unsigned, and the device signed.  The
 * operator is the type's own < or >.
 */
const char *
vexec_sort_device_key(Oid typid, Oid sortop, Oid collation, bool *descending)
{
	TypeCacheEntry *tc;

	switch (getBaseType(typid))
	{
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case DATEOID:
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case CASHOID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case BYTEAOID:
			break;
		case TEXTOID:
		case VARCHAROID:
			if (!OidIsValid(collation) || !pg_newlocale_from_collation(collation)->collate_is_c)
				return "a text key under another collation than C's";
			break;
		default:
			return psprintf("a key of type %s", format_type_be(typid));
	}
	tc = lookup_type_cache(typid, TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
	if (sortop == tc->lt_opr)
		*descending = false;
	else if (sortop == tc->gt_opr)
		*descending = true;
	else
		return "a key ordered by another operator than its type's < or >";
	return NULL;
}

/*
 * The node's device side, where it may take it as it begins, and its
 * program: the keys, then the rows' ids.  Else s->dev_why says why not.
 */
static void
dev_setup(VexecSortState *s)
{
	VexecNode  *node = &s->node;
	const char *why = NULL;
	int			nkeys = list_length(s->plan.keycols);
	SortDevice *d;
	VexecDevSortKey *keys;
	VexecDevProgram *prog;
	VexecDevSort *so;
	double		rows;
	Size		len;

	if (!vexec_device_wanted(s->plan.device, vexec_enable_gpu_sort, &why))
	{
		s->dev_why = why;
		return;
	}
	if (s->plan.bound > 0)
		why = "a bounded sort";
	else if (s->late_rel != NULL)
		why = "late columns";
	else if (!s->child_batches)
		why = "a child that hands rows";
	d = palloc0(sizeof(SortDevice));
	d->nkeys = nkeys;
	d->keycol = palloc_array(int, nkeys);
	d->types = palloc0_array(VexecDevColType, nkeys + 1);
	keys = palloc0_array(VexecDevSortKey, nkeys);
	for (int i = 0; i < nkeys && why == NULL; i++)
	{
		int			col = list_nth_int(s->plan.keycols, i) - 1;
		const VexecType *type = node->in->types[col];
		bool		desc = false;
		int16		scale;
		int			t;

		why = vexec_sort_device_key(type->typid, list_nth_oid(s->plan.operators, i),
									list_nth_oid(s->plan.collations, i), &desc);
		if (why != NULL)
			break;
		t = vexec_dev_type_of(type, &scale);
		if (t == VEXEC_DEV_NONE)
		{
			why = psprintf("a key of type %s, which the device's columns do not hold",
						   format_type_be(type->typid));
			break;
		}
		d->keycol[i] = col;
		d->types[i].type = (uint8) t;
		d->types[i].scale = scale;
		keys[i].descending = desc ? 1 : 0;
		keys[i].nulls_first = list_nth_int(s->plan.nullsfirst, i) != 0 ? 1 : 0;
	}
	if (why != NULL)
	{
		s->dev_why = why;
		return;
	}
	d->types[nkeys].type = VEXEC_DEV_INT32;

	/* the program */
	len = sizeof(VexecDevProgram) + VEXEC_DEV_ALIGN(sizeof(VexecDevColType) * (nkeys + 1)) +
		vexec_dev_sort_size(nkeys);
	prog = palloc0(len);
	prog->ncols = nkeys + 1;
	rows = Max(node->css.ss.ps.plan->plan_rows, (double) VEXEC_BATCH_ROWS);
	prog->memory = (uint64) Min(rows * (nkeys * 16 + 4) * 3, (double) (UINT64CONST(1) << 40));
	prog->admission_wait_ms = VEXEC_DEV_WAIT_DEFAULT;
	memcpy(vexec_dev_program_cols(prog), d->types, sizeof(VexecDevColType) * (nkeys + 1));
	so = (VexecDevSort *) vexec_dev_program_op(prog);
	so->nkeys = (uint32) nkeys;
	memcpy(vexec_dev_sort_keys(so), keys, sizeof(VexecDevSortKey) * nkeys);
	d->program = prog;
	d->proglen = len;

	d->cap = (uint32) Max(vexec_gpu_chunk_rows / VEXEC_BATCH_ROWS, 1) * VEXEC_BATCH_ROWS;
	d->strings = palloc0_array(uint64, nkeys + 1);
	d->idtype = vexec_type_make(INT4OID, -1, InvalidOid);
	d->src = palloc_array(VexecBatch *, VEXEC_BATCH_ROWS);
	d->rows = palloc_array(int, VEXEC_BATCH_ROWS);
	d->keepcxt = AllocSetContextCreate(node->mcxt, "VecSort device rows", ALLOCSET_DEFAULT_SIZES);
	d->tmpcxt = AllocSetContextCreate(node->mcxt, "VecSort device chunk", ALLOCSET_DEFAULT_SIZES);
	s->dev = d;
}

/* The task's figures kept, and the task ended: its slot and blocks freed. */
static void
dev_end_task(SortDevice *d)
{
	if (d->t == NULL)
		return;
	d->fig.chunks += d->t->fig.chunks;
	d->fig.rows += d->t->fig.rows;
	d->fig.bytes += d->t->fig.bytes;
	d->fig.answers += d->t->fig.answers;
	d->fig.answer_rows += d->t->fig.answer_rows;
	d->fig.answer_bytes += d->t->fig.answer_bytes;
	d->fig.requests += d->t->fig.requests;
	d->fig.wait_ms += d->t->fig.wait_ms;
	d->fig.device_ms += d->t->fig.device_ms;
	d->fig.gpu_ms += d->t->fig.gpu_ms;
	vexec_devtask_end(d->t);
	d->t = NULL;
}

static const char *
dev_answer_why(SortDevice *d, int status)
{
	return psprintf("the device's answer was \"%s\"%s%s", vexec_dev_status_name(status),
					d->t && d->t->message[0] ? ": " : "", d->t ? d->t->message : "");
}

/* The task begun, its answers' block claimed: false, the sort then tuplesort's, where it is not. */
static bool
dev_start(VexecSortState *s)
{
	SortDevice *d = s->dev;
	const char *why;
	MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

	d->started = true;
	d->t = vexec_devtask_begin(VEXEC_DEV_OP_SORT, d->program, d->proglen, s->node.mcxt, &why);
	if (d->t == NULL)
	{
		d->lost = true;
		d->why = why;
		MemoryContextSwitchTo(old);
		return false;
	}
	d->device = vexec_device_ready() ? vexec_device_ready()->device_name() : NULL;
	d->device = d->device ? pstrdup(d->device) : "the device";
	MemoryContextSwitchTo(old);
	if (vexec_devtask_claim_answers(d->t, 1, &d->page) != 1)
	{
		dev_end_task(d);
		d->lost = true;
		d->why = "no block of the arena for its answers";
		return false;
	}
	d->t->held++;
	return true;
}

/* A request in flight and the chunk being filled let go of, and the task ended. */
static void
dev_drop_task(SortDevice *d)
{
	if (d->flight != NULL)
	{
		/* the task's end lets its blocks go once the executor has */
		d->flight->nblocks = 0;
		vexec_devfill_free(d->flight);
		d->flight = NULL;
	}
	vexec_devfill_free(d->fill);
	d->fill = NULL;
	dev_end_task(d);
}

/*
 * The device given up for the rest of the sort: the kept batches put into
 * tuplesort in the order they came, each row as the CPU's strategy puts it,
 * and the rest of the input after them.
 */
static void
dev_lose(VexecSortState *s, const char *why)
{
	SortDevice *d = s->dev;
	MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

	d->why = pstrdup(why);
	MemoryContextSwitchTo(old);
	d->lost = true;
	d->ordered = false;
	dev_drop_task(d);
	for (int i = 0; i < d->nkept; i++)
	{
		VexecBatch *k = d->kept[i];

		for (int row = 0; row < k->nrows; row++)
		{
			if (k->selection != NULL && !vexec_bit(k->selection, row))
				continue;
			vexec_batch_store_row(k, row, s->put_slot);
			tuplesort_puttupleslot(s->sort, s->put_slot);
			d->poured++;
		}
		vexec_batch_free(k);
		d->kept[i] = NULL;
		CHECK_FOR_INTERRUPTS();
	}
	ExecClearTuple(s->put_slot);
	MemoryContextReset(d->keepcxt);
	d->kept = NULL;
	d->nkept = d->maxkept = 0;
	d->kept_bytes = 0;
	d->order = NULL;
	d->norder = d->pos = 0;
}

/*
 * The chunk in flight answered, and let go of: false where the device is
 * lost -- or the chunk's rows tie among themselves, which the device says
 * as it takes them, the rest of the input then tuplesort's at once.
 */
static bool
dev_take(VexecSortState *s)
{
	SortDevice *d = s->dev;
	int			status;

	if (d->flight == NULL)
		return !d->lost;
	status = vexec_devtask_wait(d->t);
	vexec_devfill_free(d->flight);
	d->flight = NULL;
	if (status != VEXEC_DEV_OK)
	{
		dev_lose(s, dev_answer_why(d, status));
		return false;
	}
	if (vexec_devtask_answer(d->t)->flags & VEXEC_DEV_ANSF_TIES)
	{
		dev_lose(s, "two rows that tie on every key, whose order is tuplesort's");
		return false;
	}
	return true;
}

/*
 * The chunk being filled to the device, once the one before it is
 * answered.  The first, of a few batches' rows, is answered at once: keys
 * of few values tie among its rows, and the sort is then tuplesort's before
 * a chunk of a million is kept.
 */
static void
dev_submit(VexecSortState *s)
{
	SortDevice *d = s->dev;
	VexecDevFill *f = d->fill;
	bool		first = d->sent == 0;

	d->fill = NULL;
	if (!dev_take(s) || f == NULL || f->nrows == 0)
	{
		vexec_devfill_free(f);
		return;
	}
	f->first_row = d->sent;
	d->sent += f->nrows;
	vexec_devfill_submit(f);
	d->flight = f;
	if (first)
		(void) dev_take(s);
}

/* The first chunk's rows: a few batches' of 1,024, as before F24 (dev_submit()). */
#define SORT_FIRST_CHUNK_ROWS	(8 * 1024)

/* A new chunk to fill, the one in flight answered first where the arena's blocks are out. */
static bool
dev_new_fill(VexecSortState *s)
{
	SortDevice *d = s->dev;
	uint32		cap = d->sent == 0 && d->flight == NULL ? Min(d->cap, SORT_FIRST_CHUNK_ROWS) : d->cap;

	d->fill = vexec_devfill_begin(d->t, d->nkeys + 1, d->types, cap, 0);
	if (d->fill == NULL && d->flight != NULL)
	{
		if (!dev_take(s))
			return false;
		d->fill = vexec_devfill_begin(d->t, d->nkeys + 1, d->types, cap, 0);
	}
	if (d->fill == NULL)
	{
		dev_lose(s, "no room in the arena for a chunk of its keys");
		return false;
	}
	return true;
}

/*
 * A copy of a batch that outlives it: whole, its selection with it, where
 * most of its rows are selected; else its selected rows alone, compacted.
 */
static VexecBatch *
dev_copy(SortDevice *d, VexecBatch *b, int nsel)
{
	VexecBatch *tmp;
	VexecBatch *k;

	if (b->selection == NULL || nsel * 2 > b->nrows)
		return vexec_batch_clone(d->keepcxt, b, NULL);
	tmp = vexec_batch_clone(d->tmpcxt, b, NULL);
	vexec_batch_compact(tmp);
	k = vexec_batch_clone(d->keepcxt, tmp, NULL);
	vexec_batch_free(tmp);
	return k;
}

/*
 * Kept batch bi's selected rows -- their keys, and their ids -- into the
 * chunk being filled, the chunk submitted where it is full: false where the
 * device is lost.
 */
static bool
dev_fill(VexecSortState *s, VexecBatch *k, int bi, int nsel)
{
	SortDevice *d = s->dev;
	int			n = k->nrows;
	uint64		all[VEXEC_WORDS(VEXEC_BATCH_ROWS)];
	const uint64 *sel = k->selection;
	int32		ids[VEXEC_BATCH_ROWS];
	VexecVec	idv;
	MemoryContext old;

	if (sel == NULL)
	{
		memset(all, 0, sizeof(all));
		for (int w = 0; w < n / 64; w++)
			all[w] = ~UINT64CONST(0);
		if (n % 64)
			all[n / 64] = (UINT64CONST(1) << (n % 64)) - 1;
		sel = all;
	}
	if (d->fill == NULL && !dev_new_fill(s))
		return false;
	for (int c = 0; c < d->nkeys; c++)
	{
		const VexecVec *v = &k->cols[d->keycol[c]];

		if (!vexec_devfill_can_append(d->fill, c, v))
		{
			dev_lose(s, psprintf("a key's layout, %s, which the device's chunks take no copy of",
								 vexec_shape_name(v->type, &v->shape)));
			return false;
		}
		d->strings[c] = d->types[c].type == VEXEC_DEV_STRING ?
			vexec_devfill_string_bytes(v, sel, n) : 0;
	}
	if (!vexec_devfill_room(d->fill, nsel, d->strings))
	{
		dev_submit(s);
		if (d->lost || !dev_new_fill(s))
			return false;
		if (!vexec_devfill_room(d->fill, nsel, d->strings))
		{
			dev_lose(s, "a batch's keys larger than the arena takes");
			return false;
		}
	}

	/* the ids: the batch's, and each row's */
	for (int r = 0; r < n; r++)
		ids[r] = (int32) (((uint32) bi << SORT_ID_SHIFT) | (uint32) r);
	memset(&idv, 0, sizeof(idv));
	idv.type = d->idtype;
	vexec_type_build_shape(d->idtype, &idv.shape);
	idv.encoding = VEXEC_FLAT;
	idv.nvalues = n;
	idv.values = ids;

	old = MemoryContextSwitchTo(d->tmpcxt);
	for (int c = 0; c < d->nkeys; c++)
		if (!vexec_devfill_append(d->fill, c, NULL, &k->cols[d->keycol[c]], sel, n, nsel))
			elog(ERROR, "vexec: a device chunk's column %d refused a layout it was checked for", c);
	if (!vexec_devfill_append(d->fill, d->nkeys, NULL, &idv, sel, n, nsel))
		elog(ERROR, "vexec: a device chunk refused its rows' ids");
	vexec_devfill_rows(d->fill, nsel);
	MemoryContextSwitchTo(old);
	MemoryContextReset(d->tmpcxt);
	if (d->fill->nrows + VEXEC_BATCH_ROWS > d->fill->cap)
		dev_submit(s);
	return !d->lost;
}

/*
 * A child's batch kept, and its rows' keys and ids into the chunk being
 * filled: true where the sort has taken it so, or put it into tuplesort
 * with the batches kept before it; false where the device was lost before
 * it -- or its rows are its child's to decide -- and the batch is to be put
 * into tuplesort as the CPU's strategy puts it.
 */
static bool
dev_keep(VexecSortState *s, VexecNode *child, VexecBatch *b)
{
	SortDevice *d = s->dev;
	VexecBatch *k;
	int			nsel;

	if (!d->started && !dev_start(s))
		return false;
	if ((child->redo != NULL && vexec_bits_any(child->redo, b->nrows)) ||
		(child->child_redo != NULL && vexec_bits_any(child->child_redo, b->nrows)))
	{
		dev_lose(s, "rows its child's kernels left to PostgreSQL's evaluator");
		return false;
	}
	nsel = b->selection != NULL ? vexec_bits_count(b->selection, b->nrows) : b->nrows;
	if (nsel == 0)
		return true;
	if (d->nkept >= SORT_ID_MAX_BATCHES)
	{
		dev_lose(s, "more batches than its rows' ids number");
		return false;
	}
	if (d->nkept == d->maxkept)
	{
		d->maxkept = Max(d->maxkept * 2, 64);
		d->kept = d->kept == NULL ?
			MemoryContextAlloc(d->keepcxt, sizeof(VexecBatch *) * d->maxkept) :
			repalloc(d->kept, sizeof(VexecBatch *) * d->maxkept);
	}
	k = dev_copy(d, b, nsel);
	d->kept[d->nkept++] = k;
	d->kept_bytes += MemoryContextMemAllocated(k->mcxt, false) + sizeof(VexecBatch) +
		sizeof(VexecType *) * k->ncols;
	d->peak_bytes = Max(d->peak_bytes, d->kept_bytes);
	s->rows_sorted += nsel;
	if (d->kept_bytes > (Size) work_mem * 1024)
	{
		dev_lose(s, "its rows past work_mem");
		return true;
	}
	(void) dev_fill(s, k, d->nkept - 1, nsel);
	return true;
}

/*
 * The input's end: the last chunk submitted, and the order read -- every
 * row's id, in the keys' order -- and the task ended.  False where the
 * device gives none, two rows tying, or the device lost: tuplesort then
 * holds the rows.
 */
static bool
dev_order(VexecSortState *s)
{
	SortDevice *d = s->dev;
	int			status;

	if (d->lost)
		return false;
	if (!d->started || (d->sent == 0 && d->fill == NULL && d->nkept == 0))
	{
		/* no row came: tuplesort's sort of none */
		dev_drop_task(d);
		d->lost = true;
		d->why = "no rows";
		return false;
	}
	dev_submit(s);
	if (!dev_take(s))
		return false;
	d->order = MemoryContextAllocHuge(d->keepcxt, sizeof(int32) * Max(d->sent, 1));
	d->norder = 0;
	do
	{
		const VexecDevAnswer *a;

		status = vexec_devtask_answer_page(d->t, 1, &d->page, 0);
		if (status != VEXEC_DEV_OK && status != VEXEC_DEV_MORE)
		{
			dev_lose(s, dev_answer_why(d, status));
			return false;
		}
		a = vexec_devtask_answer(d->t);
		if (a->flags & VEXEC_DEV_ANSF_TIES)
		{
			dev_lose(s, "two rows that tie on every key, whose order is tuplesort's");
			return false;
		}
		if (a->nrows > 0)
		{
			const VexecDevAnsCol *col = vexec_dev_answer_cols((VexecDevAnswer *) a);

			if (d->norder + a->nrows > (int64) d->sent)
				elog(ERROR, "vexec: the device's order of more rows than the sort gave it");
			memcpy(d->order + d->norder, vexec_devans_values(d->t, &col[0]),
				   sizeof(int32) * a->nrows);
			d->norder += a->nrows;
		}
		CHECK_FOR_INTERRUPTS();
	} while (status == VEXEC_DEV_MORE);
	if (d->norder != (int64) d->sent)
	{
		dev_lose(s, psprintf("an order of " INT64_FORMAT " rows for " UINT64_FORMAT,
							 d->norder, d->sent));
		return false;
	}
	dev_end_task(d);
	d->ordered = true;
	d->pos = 0;
	return true;
}

/* How many rows ahead a gather fetches its source into the cache. */
#define SORT_PREFETCH	8

/*
 * Column c of the next n sorted rows, src[j]'s row rows[j] each, into the
 * batch's column dst, in its build shape: a value in that shape copied as
 * it is -- a varlena's Datum pointing into its kept batch, which outlives
 * the batch -- and any other read as its Datum, which vexec_vec_datum()
 * makes in the batch's memory where it makes one.  The order reads the
 * kept rows at random: the source a few rows ahead is fetched into the
 * cache meanwhile.
 */
static void
dev_gather_col(VexecBatch *in, VexecVec *dst, int c, VexecBatch *const *src, const int *rows, int n)
{
	const VexecType *type = dst->type;
	uint8		layout = dst->shape.layout;

	for (int j = 0; j < n; j++)
	{
		const VexecVec *v = &src[j]->cols[c];
		int			r = rows[j];

		if (j + SORT_PREFETCH < n)
		{
			const VexecVec *pv = &src[j + SORT_PREFETCH]->cols[c];
			int			pr = rows[j + SORT_PREFETCH];

			if (pv->encoding == VEXEC_FLAT && pv->shape.layout == VEXEC_FIXED)
				__builtin_prefetch((const char *) pv->values + (Size) pr * pv->shape.stride);
			else if (pv->encoding == VEXEC_FLAT && pv->shape.layout == VEXEC_DATUM)
				__builtin_prefetch((const Datum *) pv->values + pr);
			if (pv->validity != NULL)
				__builtin_prefetch(pv->validity + (pr >> 6));
		}
		if (vexec_vec_isnull(v, r))
		{
			vexec_vec_set_null(in, dst, j);
			if (layout == VEXEC_DATUM)
				((Datum *) dst->values)[j] = (Datum) 0;
			continue;
		}
		if (dst->validity != NULL)
			vexec_bit_set(dst->validity, j);
		if (v->encoding == VEXEC_FLAT && v->shape.layout == layout &&
			(layout != VEXEC_FIXED ||
			 (v->shape.width == dst->shape.width && v->shape.stride == dst->shape.stride &&
			  v->shape.arrow_values == dst->shape.arrow_values)))
		{
			if (layout == VEXEC_FIXED)
				memcpy((char *) dst->values + (Size) j * dst->shape.stride,
					   (const char *) v->values + (Size) r * v->shape.stride, dst->shape.width);
			else if (layout == VEXEC_BYTE_BOOL)
				((uint8 *) dst->values)[j] = ((const uint8 *) v->values)[r];
			else
				((Datum *) dst->values)[j] = ((const Datum *) v->values)[r];
			continue;
		}
		{
			bool		isnull;
			Datum		dv = vexec_vec_datum(in, v, r, &isnull);

			if (layout == VEXEC_BYTE_BOOL)
				((uint8 *) dst->values)[j] = DatumGetBool(dv) ? 1 : 0;
			else if (layout == VEXEC_FIXED)
			{
				char	   *p = (char *) dst->values + (Size) j * dst->shape.stride;

				if (type->typbyval)
					store_att_byval(p, dv, type->typlen);
				else
					memcpy(p, DatumGetPointer(dv), type->typlen);
			}
			else
				((Datum *) dst->values)[j] = dv;
		}
	}
	dst->nvalues = n;
}

/*
 * The next sorted rows, gathered from the kept batches by their ids into
 * the batch being filled, a column at a time.
 */
static void
dev_emit(VexecSortState *s, VexecBatch *in)
{
	SortDevice *d = s->dev;
	int			n = (int) Min((int64) in->room, d->norder - d->pos);

	if (n <= 0)
		return;
	for (int j = 0; j < n; j++)
	{
		uint32		id = (uint32) d->order[d->pos + j];
		uint32		bi = id >> SORT_ID_SHIFT;
		int			r = (int) (id & SORT_ID_ROW_MASK);

		if (bi >= (uint32) d->nkept || r >= d->kept[bi]->nrows)
			elog(ERROR, "vexec: the device's order names a row the sort did not keep");
		d->src[j] = d->kept[bi];
		d->rows[j] = r;
	}
	for (int c = 0; c < in->ncols; c++)
		dev_gather_col(in, &in->cols[c], c, d->src, d->rows, n);
	in->nrows = n;
	d->pos += n;
	s->rows_out += n;
}

/* The device side for a run again, or the node's end: the task over, the kept rows dropped. */
static void
dev_reset(VexecSortState *s)
{
	SortDevice *d = s->dev;

	if (d == NULL)
		return;
	dev_drop_task(d);
	MemoryContextReset(d->keepcxt);
	MemoryContextReset(d->tmpcxt);
	d->kept = NULL;
	d->nkept = d->maxkept = 0;
	d->kept_bytes = 0;
	d->order = NULL;
	d->norder = d->pos = 0;
	d->sent = 0;
	d->started = false;
	d->lost = false;
	d->ordered = false;
	d->why = NULL;
}

/* EXPLAIN's lines of the device's order. */
static void
dev_explain(VexecSortState *s, ExplainState *es)
{
	SortDevice *d = s->dev;

	if (d == NULL)
	{
		if (s->dev_why != NULL)
			ExplainPropertyText("Device", psprintf("none: %s", s->dev_why), es);
		return;
	}
	if (!es->analyze || !s->node.ran)
	{
		ExplainPropertyText("Device", "the GPU's order, as the node begins", es);
		return;
	}
	if (d->device != NULL && !d->lost)
		ExplainPropertyText("Device", d->device, es);
	else if (d->device != NULL)
		ExplainPropertyText("Device", psprintf("%s, then the CPU: %s", d->device, d->why), es);
	else
		ExplainPropertyText("Device", psprintf("none: %s", d->why ? d->why : "it did not begin"), es);
	ExplainPropertyInteger("Device Chunks", NULL, d->fig.chunks, es);
	ExplainPropertyInteger("Device Rows", NULL, d->fig.rows, es);
	if (es->verbose)
	{
		ExplainPropertyInteger("Device Bytes In", "kB", (d->fig.bytes + 1023) / 1024, es);
		ExplainPropertyInteger("Device Bytes Out", "kB", (d->fig.answer_bytes + 1023) / 1024, es);
		ExplainPropertyInteger("Device Answer Pages", NULL, d->fig.answers, es);
		if (es->timing)
		{
			ExplainPropertyFloat("Device Wait Time", "ms", d->fig.wait_ms, 3, es);
			ExplainPropertyFloat("Device Time", "ms", d->fig.device_ms, 3, es);
		}
	}
	if (d->poured > 0)
		ExplainPropertyInteger("Kept Rows Put into tuplesort", NULL, d->poured, es);
}

/*
 * pg_otel_probe's span of a VecSort that ran (node.c; pg_vector_executor.md
 * §3.41): the device's order, or why none, and what its tasks did, as
 * dev_explain() prints them (OT1); the sort's method and space, the
 * device's or tuplesort's -- whose statistics a failed statement's error
 * path leaves unread (OT2).
 */
void
vexec_sort_figures(VexecNode *node, VexecNodeFigures *f, bool on_error)
{
	VexecSortState *s = (VexecSortState *) node;
	SortDevice *d = s->dev;

	if (d == NULL && s->dev_why != NULL)
	{
		f->device = "none";
		f->device_why = s->dev_why;
	}
	else if (d != NULL)
	{
		f->device = d->device != NULL ? d->device : "none";
		if (d->device == NULL)
			f->device_why = d->why != NULL ? d->why : "it did not begin";
		else if (d->lost)
			f->device_why = d->why;
		f->device_requests = d->fig.requests;
		f->device_bytes_in = d->fig.bytes;
		f->device_bytes_out = d->fig.answer_bytes;
		f->device_ms = d->fig.device_ms;
		f->device_gpu_ms = d->fig.gpu_ms;
		f->device_wait_ms = d->fig.wait_ms;
	}
	if (s->sorted && s->sort == NULL && d != NULL && d->ordered)
	{
		f->sort_method = "device";
		f->sort_space_kb = (int64) ((d->peak_bytes + 1023) / 1024);
	}
	else if (s->sorted && s->sort != NULL && !on_error)
	{
		TuplesortInstrumentation stats;

		tuplesort_get_stats(s->sort, &stats);
		f->sort_method = tuplesort_method_name(stats.sortMethod);
		f->sort_space_kb = stats.spaceUsed;
	}
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
	s->out_done = false;
	s->rows_unfetched = 0;
	bound_reset(s);
	dev_reset(s);

	if (s->child_batches)
	{
		VexecNode  *child = (VexecNode *) s->child;
		VexecBatch *b;

		while ((b = vexec_next_batch(s->child)) != NULL)
		{
			/* V8d: kept for the device's order, or put as the CPU's strategy puts it */
			if (s->dev != NULL && !s->dev->lost && dev_keep(s, child, b))
				continue;
			put_batch(s, child, b);
		}
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
	if (s->dev != NULL && dev_order(s))
	{
		/* the device's order: tuplesort holds no row */
		tuplesort_end(s->sort);
		s->sort = NULL;
	}
	else
		tuplesort_performsort(s->sort);
	/* the scan is read to its end: no bound for a rescan's until it is made */
	if (s->bound != NULL)
		s->bound->active = false;
	estate->es_direction = dir;
	s->sorted = true;
}

/* ---------------------------------------------------------------------
 * The late columns' exchange (M4, §3.21)
 *
 * The participants' bound above lets a participant give out no row past
 * the least of the tops published so far, which the sorts still running
 * leave loose: ClickBench's Q23 over MergeTree fetched 38 rows' late columns
 * -- 105 of them, a granule of each decoded a row -- for the LIMIT's 10, ten
 * of them in one worker, one after another, the query's last 70 ms.  Where
 * the scan below has the exchange (vexec_scan_late_share()), each
 * participant, attached to its barrier before its scan, publishes its sorted
 * rows' first keys and TIDs once its sort is done, and waits for the others:
 * the N-th least first key of them all then bounds the rows the whole sort
 * can return, as one sort's N-th row bounds its own, and only the rows within
 * it are fetched -- each by the next participant free, through the scan's
 * snapshot, which they share, into the query's DSA -- and given out by the
 * participant whose sort holds them, in its order.  A row whose first key
 * equals the boundary is fetched: its later keys decide it, in the Gather
 * Merge.  A participant that attaches once the others have published came
 * after the scan's units were all handed out, and has no row.
 * ---------------------------------------------------------------------
 */

/* A row's first key as the exchange orders it: less is better, a NULL first or last. */
static uint64
coop_key(VexecSortState *s, TupleTableSlot *slot)
{
	bool		isnull;
	Datum		d = slot_getattr(slot, s->bound_keycol, &isnull);

	if (isnull)
		return linitial_int(s->plan.nullsfirst) != 0 ? 0 : PG_UINT64_MAX;
	return share_encode(s, d);
}

/* At the sort's first fetch, before its scan: the exchange's barrier attached. */
static void
coop_attach(VexecSortState *s)
{
	VexecLateShare *ls;

	s->coop = NULL;
	s->coop_ready = false;
	s->coop_late = false;
	if (s->coop_slot == NULL || (ls = vexec_scan_late_share(s->child)) == NULL ||
		ls->n < s->plan.bound || s->node.css.ss.ps.state->es_query_dsa == NULL)
		return;
	if (sort_wait_event == 0)
		sort_wait_event = WaitEventExtensionNew("VexecLateColumns");
	s->coop = ls;
	s->coop_dsa = s->node.css.ss.ps.state->es_query_dsa;
	s->coop_me = IsParallelWorker() ? ParallelWorkerNumber + 1 : 0;
	if (s->coop_me >= ls->nslots)
		elog(ERROR, "vexec: a participant of a parallel sort past its exchange's %d", ls->nslots);
	if (BarrierAttach(&ls->barrier) > 0)
	{
		/* the others published: the scan's units are all handed out */
		BarrierDetach(&ls->barrier);
		s->coop_late = true;
	}
}

/* An entry's row fetched by its TID, its scan tuple's columns into the DSA. */
static void
coop_fetch(VexecSortState *s, VexecLateEntry *e)
{
	EState	   *estate = s->node.css.ss.ps.state;
	ExprContext *econtext = s->node.css.ss.ps.ps_ExprContext;
	TupleDesc	desc = s->node.css.ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	MinimalTuple mt;
	dsa_pointer dp;

	if (!table_tuple_fetch_row_version(s->late_rel, &e->tid, estate->es_snapshot, s->late_slot))
		elog(ERROR, "vexec: a row a sort kept is not visible by its TID");
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = s->late_slot;
	for (int i = 0; i < s->nlate; i++)
		s->late_values[i] = ExecEvalExprSwitchContext(s->late_exprs[i], econtext,
													  &s->late_nulls[i]);
	mt = heap_form_minimal_tuple(desc, s->late_values, s->late_nulls, 0);
	dp = dsa_allocate(s->coop_dsa, mt->t_len);
	memcpy(dsa_get_address(s->coop_dsa, dp), mt, mt->t_len);
	e->row = dp;
	heap_free_minimal_tuple(mt);
	ExecClearTuple(s->late_slot);
	s->coop_fetched++;
}

static int
uint64_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a;
	uint64		y = *(const uint64 *) b;

	return x < y ? -1 : x > y ? 1 : 0;
}

/*
 * Once the sort is done: its rows published, the boundary of them all
 * found, the rows within it fetched by whoever is free, and the barrier let
 * go of once every one is.
 */
static void
coop_run(VexecSortState *s)
{
	VexecLateShare *ls = s->coop;
	int32	   *counts = VEXEC_LATE_COUNTS(ls);
	VexecLateEntry *all = VEXEC_LATE_ENTRIES(ls);
	VexecLateEntry *mine = all + (Size) s->coop_me * ls->n;
	uint64	   *keys;
	int		   *cands;
	int			nkeys = 0;
	int			ncands = 0;
	int			n = 0;

	while (n < s->plan.bound && tuplesort_gettupleslot(s->sort, true, false, s->get_slot, NULL))
	{
		bool		isnull;
		Datum		tid = slot_getattr(s->get_slot, s->plan.late_tidcol, &isnull);

		if (isnull)
			elog(ERROR, "vexec: a row a sort kept has no TID");
		mine[n].key = coop_key(s, s->get_slot);
		mine[n].tid = *(ItemPointer) DatumGetPointer(tid);
		mine[n].row = InvalidDsaPointer;
		n++;
	}
	ExecClearTuple(s->get_slot);
	s->coop_n = n;
	s->coop_out = 0;
	if (s->coop_late)
	{
		if (n > 0)
			elog(ERROR, "vexec: a participant of a parallel sort that came last has rows");
		s->coop_ready = true;
		return;
	}
	counts[s->coop_me] = n;
	BarrierArriveAndWait(&ls->barrier, sort_wait_event);

	/* the N-th least first key of every participant's rows, or none */
	keys = palloc(sizeof(uint64) * Max((Size) ls->nslots * ls->n, 1));
	for (int p = 0; p < ls->nslots; p++)
		for (int i = 0; i < counts[p]; i++)
			keys[nkeys++] = all[(Size) p * ls->n + i].key;
	s->coop_boundary = PG_UINT64_MAX;
	if (nkeys > s->plan.bound)
	{
		qsort(keys, nkeys, sizeof(uint64), uint64_cmp);
		s->coop_boundary = keys[s->plan.bound - 1];
	}
	pfree(keys);

	/* the rows within it, in an order every participant makes alike, each fetched once */
	cands = palloc(sizeof(int) * Max((Size) ls->nslots * ls->n, 1));
	for (int p = 0; p < ls->nslots; p++)
		for (int i = 0; i < counts[p]; i++)
			if (all[(Size) p * ls->n + i].key <= s->coop_boundary)
				cands[ncands++] = p * ls->n + i;
	for (;;)
	{
		uint32		j = pg_atomic_fetch_add_u32(&ls->next, 1);

		if (j >= (uint32) ncands)
			break;
		CHECK_FOR_INTERRUPTS();
		coop_fetch(s, &all[cands[j]]);
	}
	pfree(cands);
	BarrierArriveAndWait(&ls->barrier, sort_wait_event);
	BarrierDetach(&ls->barrier);
	s->coop_ready = true;
}

/* The next of this participant's rows within the boundary, fetched, into the batch. */
static bool
coop_emit(VexecSortState *s, VexecBatch *in)
{
	VexecLateEntry *e;
	MinimalTuple mt;

	if (s->coop_out >= s->coop_n)
		return false;
	e = VEXEC_LATE_ENTRIES(s->coop) + (Size) s->coop_me * s->coop->n + s->coop_out;
	if (e->key > s->coop_boundary)
	{
		/* this row and those after it, sorted, are none of the sort's */
		s->rows_unfetched = s->coop_n - s->coop_out;
		s->coop_out = s->coop_n;
		return false;
	}
	if (!DsaPointerIsValid(e->row))
		elog(ERROR, "vexec: a row within a parallel sort's boundary was not fetched");
	mt = (MinimalTuple) dsa_get_address(s->coop_dsa, e->row);
	ExecStoreMinimalTuple(mt, s->coop_slot, false);
	slot_getallattrs(s->coop_slot);
	vexec_batch_add_values(in, s->coop_slot->tts_values, s->coop_slot->tts_isnull);
	ExecClearTuple(s->coop_slot);
	dsa_free(s->coop_dsa, e->row);
	e->row = InvalidDsaPointer;
	s->coop_out++;
	return true;
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
	{
		/* M4: the exchange's barrier attached before the scan is read */
		if (s->coop_slot != NULL)
			coop_attach(s);
		do_sort(s);
	}

	vexec_batch_reset(in);
	vexec_batch_begin_rows(in);
	if (s->dev != NULL && s->dev->ordered)
	{
		/* V8d: the kept rows, by the device's order */
		dev_emit(s, in);
		if (in->nrows == 0)
			return false;
		vexec_batch_apply_config(in, &node->layout);
		return true;
	}
	if (s->coop != NULL)
	{
		/* M4: the rows within the whole sort's boundary, fetched by any participant */
		if (!s->coop_ready)
			coop_run(s);
		while (in->nrows < in->room && coop_emit(s, in))
			s->rows_out++;
		if (in->nrows == 0)
			return false;
		vexec_batch_apply_config(in, &node->layout);
		return true;
	}
	while (in->nrows < in->room && !s->out_done &&
		   (s->plan.bound <= 0 || s->rows_out < s->plan.bound) &&
		   tuplesort_gettupleslot(s->sort, true, false, s->get_slot, NULL))
	{
		if (s->late_rel != NULL)
		{
			/*
			 * Past the participants' bound, this row and those after it,
			 * sorted, are none of the LIMIT's: none is fetched
			 */
			if (s->share_len > 0 && share_past(s, s->get_slot))
			{
				s->out_done = true;
				s->rows_unfetched = Min(s->rows_sorted, s->plan.bound) - s->rows_out;
				break;
			}
			add_late_row(s, in);
		}
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
		if (s->sort != NULL)
			tuplesort_end(s->sort);
		s->sort = NULL;
		dev_reset(s);
		if (s->child->chgParam == NULL)
			ExecReScan(s->child);
	}
	else
	{
		/* the sorted rows again: tuplesort's, or the device's order's */
		if (s->sort != NULL)
			tuplesort_rescan(s->sort);
		else
			s->dev->pos = 0;
		s->rows_out = 0;
		s->out_done = false;
		s->rows_unfetched = 0;
	}
}

static void
sort_end(CustomScanState *css)
{
	VexecSortState *s = (VexecSortState *) css;

	if (s->sort != NULL)
		tuplesort_end(s->sort);
	s->sort = NULL;
	dev_reset(s);
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
	dev_explain(s, es);

	/* V8d: sorted by the device's order, the kept rows' memory */
	if (es->analyze && s->node.ran && s->sorted && s->sort == NULL && s->dev != NULL &&
		s->dev->ordered)
	{
		int64		kb = (int64) ((s->dev->peak_bytes + 1023) / 1024);

		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Sort Method: device  Memory: " INT64_FORMAT "kB\n", kb);
		}
		else
		{
			ExplainPropertyText("Sort Method", "device", es);
			ExplainPropertyInteger("Sort Space Used", "kB", kb, es);
			ExplainPropertyText("Sort Space Type", "Memory", es);
		}
		if (es->verbose)
			ExplainPropertyInteger("Rows Sorted", NULL, s->rows_sorted, es);
	}

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
			ExplainPropertyInteger("Rows Fetched by TID", NULL,
								   s->coop != NULL ? s->coop_fetched : s->rows_out, es);
		if (es->verbose && s->coop != NULL)
			ExplainPropertyText("Late Columns", "fetched for the whole sort's rows alone, by any participant", es);
		if (es->verbose && s->rows_unfetched > 0)
			ExplainPropertyInteger("Rows Left Past the Workers' Bound", NULL, s->rows_unfetched, es);
	}
}
