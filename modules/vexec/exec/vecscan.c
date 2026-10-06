/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecscan.c
 *	  VecScan: a table's sequential scan, its quals and its projection, a
 *	  batch at a time (pg_vector_executor.md §3.8, §3.5).
 *
 * Its batches come from a source (§3.5):
 *
 *	a registered one	the batch reader the table's access method
 *						registered (vexec_source.h): PAX's, gp_ao's.  It
 *						hands each column in its storage's layout, and the
 *						rows it has deleted out of the batch's selection;
 *	heap's pages		heap's page reader (source/heap.c, V4): each page
 *						prepared by heap, its visible tuples deformed
 *						straight into the batch -- with vexec.heap_page_reader
 *						on, its default;
 *	the slot path		any other access method, and heap with the page
 *						reader off or under a snapshot heap reads no page at
 *						a time (§3.5.2): table_scan_getnextslot(), the needed
 *						attributes deformed, and their values copied into
 *						the batch.
 *
 * Any way vexec begins the scan itself, through table_beginscan() with
 * the flags a SeqScan gives it (PG19:src/backend/executor/nodeSeqscan.c),
 * so the access method keeps the snapshot's registration, predicate locks,
 * rescan and end; and it begins it at the first row asked for, as a SeqScan
 * does, so that an EXPLAIN without ANALYZE begins none.  The columns are
 * read in the format in effect when the node began (§3.4.4).
 *
 * Parallel-aware (V4).  A VecScan planned as a partial path, under a
 * Gather or Gather Merge, or under one of M8's Gathers in a segment, shares
 * its table among the Gather's participants as a SeqScan does
 * (nodeSeqscan.c, ExecSeqScanEstimate() and on): the leader makes the
 * access method's parallel descriptor in the CustomScan's chunk of the
 * DSM, keyed by its plan node id (PG19:src/backend/executor/nodeCustom.c:
 * 160-218), and each participant begins its scan on it with
 * table_beginscan_parallel(), so the access method hands each its blocks,
 * files or segment files -- heap's page reader, PAX's and gp_ao's readers
 * and the slot path alike.  A scan planned parallel and run with no DSM --
 * serially, as a Gather run outside parallel mode runs its child -- is an
 * ordinary scan.
 *
 * A VecAgg above a scan with no qual may have the source answer its
 * aggregates from the statistics it keeps of a unit of rows -- a file, a
 * group -- in place of the unit's batches (vexec_scan_aggregate(), H2): the
 * unit's rows are counted as the scan's, as if they had been handed up.
 *
 * VecBitmapHeapScan (H9, §3.14) is the same node over the pages a bitmap
 * selects: its child, a BitmapIndexScan or a tree of BitmapAnd and BitmapOr
 * over them, stays a row node and hands it a TID bitmap
 * (MultiExecProcNode()), as it hands a BitmapHeapScan one
 * (PG19:src/backend/executor/nodeBitmapHeapscan.c); the table's scan is
 * begun with table_beginscan_bm() and iterates the bitmap, a heap table's
 * pages read by heap's page reader (source/heap.c), any other's rows
 * through table_scan_bitmap_next_tuple().  Its quals are the bitmap's
 * conditions, custom_exprs -- a BitmapHeapScan's bitmapqualorig -- before
 * its own: PostgreSQL rechecks them only on a page the bitmap holds lossily
 * or the index asks it to, but a row of any other page satisfies them, so
 * checking them on every row gives the same rows, in the same order of
 * evaluation, a vector's work.  The rows they remove are counted apart, as
 * a BitmapHeapScan counts the rows its recheck removes, and a row
 * PostgreSQL's evaluator takes is rechecked first (node.c).  Not
 * parallel-aware: a parallel bitmap heap scan's shared iterator stays
 * PostgreSQL's.
 *
 * A VecSort's running bound (H6, §3.14; vecsort.c).  A bounded VecSort
 * above the scan lends it the first key of the sort's N-th row, and the
 * scan drops a row whose key is strictly past it: heap's page reader before
 * the row is deformed, where the bound is checked before the quals, else
 * among a batch's rows (node.c), before or after the quals as the planner
 * said; and a source that takes keys is given the bound as one, to pass
 * over the units none of whose rows is within it (vexec_source.h,
 * set_keys).
 *
 * The scan's tuple is the table's row: the plan's quals and target list
 * read Vars of scanrelid, as a SeqScan's do, so custom_scan_tlist is empty.
 * The node reads the attributes they name, the whole row for a whole-row
 * Var; ctid comes from each row's TID, tableoid from the relation.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "access/relscan.h"
#include "nodes/makefuncs.h"
#include "nodes/tidbitmap.h"
#include "utils/ruleutils.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/pg_type.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "access/stratnum.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/typcache.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "expr/expr.h"
#include "source/source.h"

typedef struct VexecScanState
{
	VexecNode	node;			/* first */
	bool		bitmap;			/* VecBitmapHeapScan */
	PlanState  *bitmap_child;	/* bitmap: the index scans' tree */
	TIDBitmap  *tbm;			/* bitmap: its bitmap, once made */
	bool		bitmap_ready;	/* bitmap: the scan iterates it */
	TupleTableSlot *bmslot;		/* bitmap: the access method's rows */
	uint64		exact_pages;	/* bitmap, through the slot path */
	uint64		lossy_pages;
	Relation	rel;
	const VexecSourceRoutine *src;	/* NULL: heap's pages or the slot path */
	TableScanDesc scan;
	void	   *srcstate;
	bool		heap_pages;		/* heap's page reader, where it can */
	VexecHeapReader *heap;		/* the page reader, once begun */
	TupleTableSlot *amslot;		/* the slot path's */
	ParallelTableScanDesc pscan;	/* a parallel-aware scan's, in the DSM */
	Size		pscan_len;
	int			nattrs;			/* input columns that are attributes */
	AttrNumber *attrs;			/* their attnos */
	bool		need_tid;		/* the last input column is ctid */
	int			maxattr;
	bool		done;

	/* H2: the units, and their rows, a source answered from statistics */
	int64		stats_units;
	int64		stats_rows;

	/*
	 * A VecSort's running bound as a source's key (H6; vexec_source.h,
	 * set_keys): its strategy, 0 where the source is given none, and the
	 * bound's version last given.
	 */
	ScanKeyData bound_key;
	StrategyNumber bound_strategy;
	uint64		bound_given;
} VexecScanState;

static bool scan_fetch(VexecNode *node);
static void scan_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *scan_exec(CustomScanState *css);
static void scan_end(CustomScanState *css);
static void scan_rescan(CustomScanState *css);
static void scan_explain(CustomScanState *css, List *ancestors, ExplainState *es);
static Size scan_estimate_dsm(CustomScanState *css, ParallelContext *pcxt);
static void scan_initialize_dsm(CustomScanState *css, ParallelContext *pcxt, void *coordinate);
static void scan_reinitialize_dsm(CustomScanState *css, ParallelContext *pcxt, void *coordinate);
static void scan_initialize_worker(CustomScanState *css, shm_toc *toc, void *coordinate);

static const CustomExecMethods bitmapscan_exec_methods = {
	.CustomName = VEXEC_BITMAPSCAN_NAME,
	.BeginCustomScan = scan_begin,
	.ExecCustomScan = scan_exec,
	.EndCustomScan = scan_end,
	.ReScanCustomScan = scan_rescan,
	.ExplainCustomScan = scan_explain,
};

static const CustomExecMethods scan_exec_methods = {
	.CustomName = VEXEC_SCAN_NAME,
	.BeginCustomScan = scan_begin,
	.ExecCustomScan = scan_exec,
	.EndCustomScan = scan_end,
	.ReScanCustomScan = scan_rescan,
	.EstimateDSMCustomScan = scan_estimate_dsm,
	.InitializeDSMCustomScan = scan_initialize_dsm,
	.ReInitializeDSMCustomScan = scan_reinitialize_dsm,
	.InitializeWorkerCustomScan = scan_initialize_worker,
	.ExplainCustomScan = scan_explain,
};

Node *
vexec_create_scan_state(CustomScan *cscan)
{
	VexecScanState *s = palloc0(sizeof(VexecScanState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &scan_exec_methods;
	s->node.kind = VEXEC_NODE_SCAN;
	return (Node *) s;
}

Node *
vexec_create_bitmapscan_state(CustomScan *cscan)
{
	VexecScanState *s = palloc0(sizeof(VexecScanState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &bitmapscan_exec_methods;
	s->node.kind = VEXEC_NODE_SCAN;
	s->node.label = "Vec Bitmap Heap Scan";
	s->bitmap = true;
	return (Node *) s;
}

/*
 * The attributes the plan's quals and target list read: every attribute
 * for a whole-row Var; and whether ctid is read.
 */
static Bitmapset *
needed_attrs(CustomScan *cscan, bool *need_tid)
{
	Index		relid = cscan->scan.scanrelid;
	Bitmapset  *attrs = NULL;

	pull_varattnos((Node *) cscan->scan.plan.targetlist, relid, &attrs);
	pull_varattnos((Node *) cscan->scan.plan.qual, relid, &attrs);
	pull_varattnos((Node *) cscan->custom_exprs, relid, &attrs);	/* a bitmap's */
	*need_tid = bms_is_member(SelfItemPointerAttributeNumber - FirstLowInvalidHeapAttributeNumber,
							  attrs);
	return attrs;
}

static void
scan_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecScanState *s = (VexecScanState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	TupleDesc	desc;
	Bitmapset  *attrs;
	VexecType **types;
	bool		whole_row;
	int			col;
	int			i;
	const char *how;

	vexec_node_begin(node, estate);
	node->fetch = scan_fetch;
	s->rel = css->ss.ss_currentRelation;
	desc = RelationGetDescr(s->rel);

	attrs = needed_attrs(cscan, &s->need_tid);
	whole_row = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs);

	/* the input columns: the attributes read, in order, then ctid */
	s->attrs = palloc(sizeof(AttrNumber) * (desc->natts + 1));
	for (i = 1; i <= desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i - 1);

		if (att->attisdropped)
			continue;
		if (whole_row || bms_is_member(i - FirstLowInvalidHeapAttributeNumber, attrs))
			s->attrs[s->nattrs++] = i;
	}
	s->maxattr = s->nattrs > 0 ? s->attrs[s->nattrs - 1] : 0;

	node->ninput = s->nattrs + (s->need_tid ? 1 : 0);
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(node->ninput, 1));
	types = palloc(sizeof(VexecType *) * Max(node->ninput, 1));
	for (col = 0; col < s->nattrs; col++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, s->attrs[col] - 1);

		node->input_attnos[col] = s->attrs[col];
		types[col] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
	}
	if (s->need_tid)
	{
		node->input_attnos[col] = SelfItemPointerAttributeNumber;
		types[col] = vexec_type_make(TIDOID, -1, InvalidOid);
	}
	node->in = vexec_batch_create(node->mcxt, node->ninput, types);
	node->input_varno = cscan->scan.scanrelid;
	node->input_slot = css->ss.ss_ScanTupleSlot;
	node->input_slot->tts_tableOid = RelationGetRelid(s->rel);

	/* a registered source serves only the columns it supports */
	s->src = s->bitmap ? NULL : vexec_source_for(s->rel, &how);
	if (s->src != NULL && s->src->supports != NULL)
	{
		for (i = 0; i < s->nattrs; i++)
			if (!s->src->supports(s->rel, s->attrs[i]))
			{
				s->src = NULL;
				break;
			}
	}
	s->heap_pages = s->src == NULL && vexec_heap_page_reader &&
		vexec_heap_reader_possible(s->rel);

	if (s->bitmap)
	{
		/* the index scans' tree, a row node; the bitmap's conditions first */
		s->bitmap_child = ExecInitNode(outerPlan(cscan), estate, eflags);
		outerPlanState(css) = s->bitmap_child;
		vexec_node_compile(node, list_concat_copy(cscan->custom_exprs, cscan->scan.plan.qual),
						   cscan->scan.plan.targetlist);
		node->nrecheck = list_length(cscan->custom_exprs);
		node->recheck_qual = ExecInitQual(cscan->custom_exprs, &css->ss.ps);
	}
	else
		vexec_node_compile(node, cscan->scan.plan.qual, cscan->scan.plan.targetlist);
}

/* The quals a source may prune by: no parameter, no SubPlan, nothing volatile. */
static bool
prunable_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param) || IsA(node, SubPlan) || IsA(node, AlternativeSubPlan))
		return true;
	return expression_tree_walker(node, prunable_walker, context);
}

static List *
pruning_quals(CustomScan *cscan)
{
	List	   *quals = NIL;
	ListCell   *lc;

	foreach(lc, cscan->scan.plan.qual)
	{
		Node	   *q = lfirst(lc);

		if (!prunable_walker(q, NULL) && !contain_volatile_functions(q))
			quals = lappend(quals, q);
	}
	return quals;
}

/* The flags a SeqScan begins its scan with (nodeSeqscan.c). */
static uint32
scan_flags(VexecScanState *s)
{
	EState	   *estate = s->node.css.ss.ps.state;
	uint32		flags = SO_NONE;

	if (ScanRelIsReadOnly(&s->node.css.ss))
		flags |= SO_HINT_REL_READ_ONLY;
	if (estate->es_instrument & INSTRUMENT_IO)
		flags |= SO_SCAN_INSTRUMENT;
	return flags;
}

/*
 * Begin the table's scan, and the source's, at the first row asked for: a
 * share of a parallel scan where the DSM holds one, the whole table else.
 */
static void begin_bitmap_scan(VexecScanState *s);

/*
 * A VecSort's running bound over the scan's rows (H6, vecsort.c): checked
 * before the quals by heap's page reader, before a row's columns are
 * deformed, where the reader reads the pages; else among a batch's rows
 * (node.c).
 */
static void
bound_to_reader(VexecScanState *s)
{
	VexecSortBound *b = s->node.bound;

	if (b != NULL && b->before_quals && s->heap != NULL)
	{
		vexec_heap_reader_set_bound(s->heap, b);
		s->node.bound_in_source = true;
	}
}

/*
 * The running bound as a source's key, "the first key at most the bound"
 * (vexec_source.h, set_keys): where a source can take one, the bound is
 * checked before the quals -- passing a unit over leaves its rows'
 * quals unevaluated -- and the first key's order is its type's own,
 * ascending or descending, under the column's collation, with its NULLs
 * past every bound: last, or none.  A key's NULLs never satisfy it.
 */
static void
bound_source_key(VexecScanState *s)
{
	VexecSortBound *b = s->node.bound;
	Form_pg_attribute att = TupleDescAttr(RelationGetDescr(s->rel), b->attno - 1);
	TypeCacheEntry *tc;
	StrategyNumber strategy;
	Oid			opno;

	s->bound_strategy = 0;
	if (s->src == NULL || !VEXEC_SOURCE_HAS(s->src, set_keys) || !b->before_quals ||
		(b->ssup.ssup_nulls_first && !att->attnotnull) ||
		(OidIsValid(att->attcollation) && b->ssup.ssup_collation != att->attcollation))
		return;
	tc = lookup_type_cache(att->atttypid, TYPECACHE_LT_OPR | TYPECACHE_GT_OPR |
						   TYPECACHE_BTREE_OPFAMILY);
	if (b->sortop == tc->lt_opr)
		strategy = BTLessEqualStrategyNumber;
	else if (b->sortop == tc->gt_opr)
		strategy = BTGreaterEqualStrategyNumber;
	else
		return;
	opno = get_opfamily_member(tc->btree_opf, att->atttypid, att->atttypid, strategy);
	if (!OidIsValid(opno))
		return;
	ScanKeyEntryInitialize(&s->bound_key, 0, b->attno, strategy, att->atttypid,
						   b->ssup.ssup_collation, get_opcode(opno), (Datum) 0);
	s->bound_strategy = strategy;
	s->bound_given = 0;
}

/* The bound, moved since the source was last given it, given again. */
static void
bound_to_source(VexecScanState *s)
{
	VexecSortBound *b = s->node.bound;

	if (b == NULL || s->bound_strategy == 0 || !b->active || b->isnull ||
		b->version == s->bound_given)
		return;
	s->bound_key.sk_argument = b->value;
	s->src->set_keys(s->srcstate, 1, &s->bound_key);
	s->bound_given = b->version;
}

/*
 * A VecSort's running bound lent to this scan, the node below it (H6): the
 * scan checks its rows against it.  False where the node is no vector scan,
 * or reads no column of the bound's.
 */
bool
vexec_scan_set_bound(PlanState *ps, VexecSortBound *bound)
{
	VexecScanState *s;
	int			col;

	if (!IsA(ps, CustomScanState) ||
		(((CustomScanState *) ps)->methods != &scan_exec_methods &&
		 ((CustomScanState *) ps)->methods != &bitmapscan_exec_methods))
		return false;
	s = (VexecScanState *) ps;
	for (col = 0; col < s->nattrs; col++)
		if (s->attrs[col] == bound->attno)
			break;
	if (col >= s->nattrs)
		return false;
	s->node.bound = bound;
	s->node.bound_col = col;
	s->node.bound_in_source = false;
	bound_source_key(s);
	if (s->scan != NULL)
		bound_to_reader(s);
	return true;
}

static void
begin_scan(VexecScanState *s)
{
	EState	   *estate = s->node.css.ss.ps.state;

	if (s->bitmap)
	{
		begin_bitmap_scan(s);
		return;
	}
	if (s->pscan != NULL)
		s->scan = table_beginscan_parallel(s->rel, s->pscan, scan_flags(s));
	else
		s->scan = table_beginscan(s->rel, estate->es_snapshot, 0, NULL, scan_flags(s));
	s->node.css.ss.ss_currentScanDesc = s->scan;

	if (s->src != NULL)
	{
		VexecSourceSpec *spec = palloc0(sizeof(VexecSourceSpec));
		uint8	   *layouts = palloc(Max(s->nattrs, 1));
		int			i;

		for (i = 0; i < s->nattrs; i++)
		{
			VexecShape	shape;

			vexec_type_shape(s->node.in->types[i], &s->node.layout, &shape);
			layouts[i] = shape.layout;
		}
		spec->size = sizeof(VexecSourceSpec);
		spec->ncolumns = s->nattrs;
		spec->attnums = s->attrs;
		spec->layouts = layouts;
		spec->quals = pruning_quals((CustomScan *) s->node.css.ss.ps.plan);
		spec->max_rows = VEXEC_BATCH_ROWS;
		spec->flags = s->need_tid ? VEXEC_SRC_TIDS : 0;
		s->srcstate = s->src->begin(s->scan, spec);
		return;
	}
	if (s->heap_pages)
		s->heap = vexec_heap_reader_begin(s->scan, s->node.in, s->nattrs, s->attrs,
										  s->need_tid);
	if (s->heap == NULL)
		s->amslot = table_slot_create(s->rel, NULL);
	bound_to_reader(s);
}

/*
 * A bitmap heap scan's start, as BitmapTableScanSetup() makes it
 * (nodeBitmapHeapscan.c): the bitmap from the index scans' tree, the table's
 * bitmap scan begun -- once -- and its iterator set over the bitmap.
 */
static void
begin_bitmap_scan(VexecScanState *s)
{
	EState	   *estate = s->node.css.ss.ps.state;

	s->tbm = (TIDBitmap *) MultiExecProcNode(s->bitmap_child);
	if (s->tbm == NULL || !IsA(s->tbm, TIDBitmap))
		elog(ERROR, "unrecognized result from subplan");
	if (s->scan == NULL)
	{
		s->scan = table_beginscan_bm(s->rel, estate->es_snapshot, 0, NULL, scan_flags(s));
		s->node.css.ss.ss_currentScanDesc = s->scan;
		s->bmslot = table_slot_create(s->rel, NULL);
		if (s->heap_pages)
			s->heap = vexec_heap_reader_begin_bitmap(s->scan, s->node.in, s->nattrs, s->attrs,
													 s->need_tid, s->bmslot);
		bound_to_reader(s);
	}
	s->scan->st.rs_tbmiterator = tbm_begin_iterate(s->tbm, NULL, InvalidDsaPointer);
	s->bitmap_ready = true;
}

/* A bitmap heap scan's next batch, a row at a time through the slot path. */
static bool
fetch_bitmap_slots(VexecScanState *s)
{
	VexecBatch *in = s->node.in;
	Datum	   *values = palloc(sizeof(Datum) * Max(s->node.ninput, 1));
	bool	   *isnull = palloc(sizeof(bool) * Max(s->node.ninput, 1));
	bool		recheck;
	int			i;

	vexec_batch_reset(in);
	vexec_batch_begin_rows(in);
	while (in->nrows < VEXEC_BATCH_ROWS)
	{
		if (!table_scan_bitmap_next_tuple(s->scan, s->bmslot, &recheck,
										  &s->lossy_pages, &s->exact_pages))
		{
			s->done = true;
			break;
		}
		slot_getsomeattrs(s->bmslot, s->maxattr);
		for (i = 0; i < s->nattrs; i++)
		{
			values[i] = s->bmslot->tts_values[s->attrs[i] - 1];
			isnull[i] = s->bmslot->tts_isnull[s->attrs[i] - 1];
		}
		if (s->need_tid)
		{
			values[i] = PointerGetDatum(&s->bmslot->tts_tid);
			isnull[i] = false;
		}
		vexec_batch_add_values(in, values, isnull);
	}
	ExecClearTuple(s->bmslot);
	pfree(values);
	pfree(isnull);
	if (in->nrows > 0)
		vexec_batch_apply_config(in, &s->node.layout);
	return in->nrows > 0;
}

/* Heap's page reader: the next batch, deformed from the scan's pages. */
static bool
fetch_heap(VexecScanState *s)
{
	VexecBatch *in = s->node.in;

	if (!vexec_heap_reader_next(s->heap, in))
	{
		s->done = true;
		return false;
	}
	vexec_batch_apply_config(in, &s->node.layout);
	return true;
}

/* The slot path: up to a batch of rows, their values copied in. */
static bool
fetch_slots(VexecScanState *s)
{
	VexecBatch *in = s->node.in;
	Datum	   *values = palloc(sizeof(Datum) * Max(s->node.ninput, 1));
	bool	   *isnull = palloc(sizeof(bool) * Max(s->node.ninput, 1));
	int			i;

	vexec_batch_reset(in);
	vexec_batch_begin_rows(in);
	while (in->nrows < VEXEC_BATCH_ROWS)
	{
		if (!table_scan_getnextslot(s->scan, ForwardScanDirection, s->amslot))
		{
			s->done = true;
			break;
		}
		slot_getsomeattrs(s->amslot, s->maxattr);
		for (i = 0; i < s->nattrs; i++)
		{
			values[i] = s->amslot->tts_values[s->attrs[i] - 1];
			isnull[i] = s->amslot->tts_isnull[s->attrs[i] - 1];
		}
		if (s->need_tid)
		{
			values[i] = PointerGetDatum(&s->amslot->tts_tid);
			isnull[i] = false;
		}
		vexec_batch_add_values(in, values, isnull);
	}
	pfree(values);
	pfree(isnull);
	if (in->nrows > 0)
		vexec_batch_apply_config(in, &s->node.layout);
	return in->nrows > 0;
}

/* A registered source's next batch, its columns as the source holds them. */
static bool
fetch_source(VexecScanState *s)
{
	VexecBatch *in = s->node.in;
	VexecSourceBatch sb;
	int			i;

	memset(&sb, 0, sizeof(sb));
	vexec_batch_reset(in);
	bound_to_source(s);
	if (!s->src->next(s->srcstate, &sb))
	{
		s->done = true;
		return false;
	}
	in->nrows = sb.nrows;
	in->selection = (uint64 *) sb.visible;
	for (i = 0; i < s->nattrs; i++)
	{
		const VexecColumn *c = &sb.columns[i];
		VexecVec   *v = &in->cols[i];

		v->shape.layout = c->layout;
		v->shape.arrow_values = c->arrow_values;
		v->shape.scale = c->scale;
		v->shape.width = c->width;
		v->shape.stride = c->stride;
		vexec_shape_normalize(&v->shape);
		v->encoding = c->encoding;
		v->nvalues = c->encoding == VEXEC_CONST ? 1 : sb.nrows;
		v->validity = (uint64 *) c->validity;
		v->values = (void *) c->values;
		v->buffers = (char **) c->buffers;
		v->buffer_sizes = (int64 *) c->buffer_sizes;
		v->nbuffers = c->nbuffers;
		v->datums = (Datum *) c->datums;
		if (c->encoding == VEXEC_DICT)
		{
			VexecVec   *d = vexec_batch_alloc0(in, sizeof(VexecVec));
			const VexecColumn *dc = c->dictionary;

			v->codes = (int32 *) c->codes;
			d->type = v->type;
			d->shape.layout = dc->layout;
			d->shape.arrow_values = dc->arrow_values;
			d->shape.scale = dc->scale;
			d->shape.width = dc->width;
			d->shape.stride = dc->stride;
			vexec_shape_normalize(&d->shape);
			d->encoding = VEXEC_FLAT;
			d->nvalues = c->ndictionary;
			d->validity = (uint64 *) dc->validity;
			d->values = (void *) dc->values;
			d->buffers = (char **) dc->buffers;
			d->buffer_sizes = (int64 *) dc->buffer_sizes;
			d->nbuffers = dc->nbuffers;
			d->datums = (Datum *) dc->datums;
			v->dictionary = d;
		}
	}
	if (s->need_tid)
	{
		VexecVec   *v = &in->cols[i];

		if (sb.tids == NULL)
			elog(ERROR, "vexec: the batch source \"%s\" gave no TIDs", s->src->name);

		memset(&v->shape, 0, sizeof(VexecShape));
		v->shape.layout = VEXEC_FIXED;
		v->shape.width = sizeof(ItemPointerData);
		v->shape.stride = sizeof(ItemPointerData);
		v->encoding = VEXEC_FLAT;
		v->nvalues = sb.nrows;
		v->validity = NULL;
		v->values = (void *) sb.tids;
	}
	vexec_batch_apply_config(in, &s->node.layout);
	return true;
}

static bool
scan_fetch(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;

	if (s->scan == NULL || (s->bitmap && !s->bitmap_ready))
		begin_scan(s);
	if (s->done)
		return false;
	if (s->src != NULL)
		return fetch_source(s);
	if (s->heap != NULL)
		return fetch_heap(s);
	return s->bitmap ? fetch_bitmap_slots(s) : fetch_slots(s);
}

/*
 * Whether the scan's source may answer aggregates from its statistics: it
 * has the contract's aggregate(), and the scan no qual and no TIDs to read.
 */
bool
vexec_scan_can_aggregate(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;
	Plan	   *plan = node->css.ss.ps.plan;

	Assert(node->kind == VEXEC_NODE_SCAN);
	return s->src != NULL && VEXEC_SOURCE_HAS(s->src, aggregate) &&
		plan->qual == NIL && !s->need_tid;
}

/*
 * The source's next unit answered from its statistics, between batches:
 * true, and its rows counted as handed up; false where the source leaves
 * the unit to its batches, or the scan is done.
 */
bool
vexec_scan_aggregate(VexecNode *node, int nreqs, const VexecSourceAgg *reqs,
					 VexecSourceAggAnswer *answers, int64 *nrows)
{
	VexecScanState *s = (VexecScanState *) node;
	PlanState  *ps = &node->css.ss.ps;
	bool		ok;

	Assert(vexec_scan_can_aggregate(node));
	if (ps->chgParam != NULL)
		ExecReScan(ps);
	if (s->scan == NULL)
		begin_scan(s);
	if (s->done || node->finished)
		return false;
	CHECK_FOR_INTERRUPTS();
	if (ps->instrument)
		InstrStartNode(ps->instrument);
	*nrows = 0;
	ok = s->src->aggregate(s->srcstate, nreqs, reqs, answers, nrows);
	if (ps->instrument)
		InstrStopNode(ps->instrument, ok ? (double) *nrows : 0);
	if (ok)
	{
		s->stats_units++;
		s->stats_rows += *nrows;
	}
	return ok;
}

/* The scan's source's name, for EXPLAIN. */
const char *
vexec_scan_source_name(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;

	return s->src != NULL && s->src->name != NULL ? s->src->name : "a registered source";
}

static TupleTableSlot *
scan_exec(CustomScanState *css)
{
	return vexec_node_exec(&((VexecScanState *) css)->node);
}

static void
scan_rescan(CustomScanState *css)
{
	VexecScanState *s = (VexecScanState *) css;

	s->bound_given = 0;			/* a source forgets the running bound's keys */
	if (s->bitmap)
	{
		/* as ExecReScanBitmapHeapScan(): the bitmap made again */
		if (s->scan != NULL)
		{
			if (!tbm_exhausted(&s->scan->st.rs_tbmiterator))
				tbm_end_iterate(&s->scan->st.rs_tbmiterator);
			if (s->heap != NULL)
				vexec_heap_reader_rescan(s->heap);
			table_rescan(s->scan, NULL);
		}
		if (s->tbm != NULL)
			tbm_free(s->tbm);
		s->tbm = NULL;
		s->bitmap_ready = false;
		s->done = false;
		vexec_node_rescan(&s->node);
		if (s->bitmap_child->chgParam == NULL)
			ExecReScan(s->bitmap_child);
		return;
	}

	if (s->scan != NULL)
	{
		if (s->heap != NULL)
			vexec_heap_reader_rescan(s->heap);
		table_rescan(s->scan, NULL);
		if (s->src != NULL)
			s->src->rescan(s->srcstate);
	}
	s->done = false;
	vexec_node_rescan(&s->node);
}

static void
scan_end(CustomScanState *css)
{
	VexecScanState *s = (VexecScanState *) css;

	if (s->bitmap)
	{
		ExecEndNode(s->bitmap_child);
		if (s->scan != NULL && !tbm_exhausted(&s->scan->st.rs_tbmiterator))
			tbm_end_iterate(&s->scan->st.rs_tbmiterator);
		if (s->bmslot != NULL)
			ExecDropSingleTupleTableSlot(s->bmslot);
		s->bmslot = NULL;
		if (s->tbm != NULL)
			tbm_free(s->tbm);
		s->tbm = NULL;
	}

	if (s->srcstate != NULL)
		s->src->end(s->srcstate);
	s->srcstate = NULL;
	if (s->heap != NULL)
		vexec_heap_reader_end(s->heap);
	s->heap = NULL;
	if (s->amslot != NULL)
		ExecDropSingleTupleTableSlot(s->amslot);
	s->amslot = NULL;
	if (s->scan != NULL)
		table_endscan(s->scan);
	s->scan = NULL;
	vexec_node_end(&s->node);
}

static void
scan_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecScanState *s = (VexecScanState *) css;

	vexec_node_relabel(&s->node, es);
	if (s->bitmap && ((CustomScan *) css->ss.ps.plan)->custom_exprs != NIL)
	{
		/* the bitmap's conditions, as a BitmapHeapScan shows them */
		List	   *context = set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan, ancestors);
		Node	   *cond = (Node *) make_ands_explicit(((CustomScan *) css->ss.ps.plan)->custom_exprs);

		ExplainPropertyText("Recheck Cond",
							deparse_expression(cond, context, es->verbose, false), es);
		/* as show_instrumentation_count() shows it (explain.c) */
		if (es->analyze && css->ss.ps.instrument != NULL)
		{
			NodeInstrumentation *instr = css->ss.ps.instrument;

			if (instr->nfiltered2 > 0 || es->format != EXPLAIN_FORMAT_TEXT)
				ExplainPropertyFloat("Rows Removed by Index Recheck", NULL,
									 instr->nloops > 0 ? instr->nfiltered2 / instr->nloops : 0.0,
									 0, es);
		}
	}
	vexec_node_explain_properties(&s->node, ancestors, es);
	if (es->verbose)
	{
		const char *how;

		(void) vexec_source_for(s->rel, &how);
		ExplainPropertyText("Source", s->src != NULL ? how :
							s->heap_pages ? "heap's pages" : "the slot path", es);
	}
	if (es->analyze && s->bitmap && s->scan != NULL)
	{
		uint64		exact = s->exact_pages;
		uint64		lossy = s->lossy_pages;

		if (s->heap != NULL)
			vexec_heap_reader_bitmap_pages(s->heap, &exact, &lossy);
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Heap Blocks: exact=" UINT64_FORMAT " lossy=" UINT64_FORMAT "\n",
							 exact, lossy);
		}
		else
		{
			ExplainPropertyUInteger("Exact Heap Blocks", NULL, exact, es);
			ExplainPropertyUInteger("Lossy Heap Blocks", NULL, lossy, es);
		}
	}
	if (es->analyze && es->verbose && s->heap != NULL)
		ExplainPropertyInteger("Heap Pages", NULL, vexec_heap_reader_pages(s->heap), es);
	/* a VecSort's running bound (H6): text gives no zero, as for a filter */
	if (es->analyze && s->node.ran && s->node.bound != NULL &&
		(s->node.bound->removed > 0 || es->format != EXPLAIN_FORMAT_TEXT))
		ExplainPropertyInteger("Rows Removed by Bound", NULL, s->node.bound->removed, es);
	if (es->analyze && s->stats_units > 0)
	{
		ExplainPropertyInteger("Units From Statistics", NULL, s->stats_units, es);
		ExplainPropertyInteger("Rows From Statistics", NULL, s->stats_rows, es);
	}
}

/*
 * A parallel-aware VecScan's chunk of the DSM: the access method's parallel
 * scan descriptor, as a SeqScan's (ExecSeqScanEstimate() and on).
 */
static Size
scan_estimate_dsm(CustomScanState *css, ParallelContext *pcxt)
{
	VexecScanState *s = (VexecScanState *) css;

	(void) pcxt;
	s->pscan_len = table_parallelscan_estimate(s->rel, css->ss.ps.state->es_snapshot);
	return s->pscan_len;
}

static void
scan_initialize_dsm(CustomScanState *css, ParallelContext *pcxt, void *coordinate)
{
	VexecScanState *s = (VexecScanState *) css;

	(void) pcxt;
	s->pscan = (ParallelTableScanDesc) coordinate;
	table_parallelscan_initialize(s->rel, s->pscan, css->ss.ps.state->es_snapshot);
}

static void
scan_reinitialize_dsm(CustomScanState *css, ParallelContext *pcxt, void *coordinate)
{
	VexecScanState *s = (VexecScanState *) css;

	(void) pcxt;
	(void) coordinate;
	if (s->pscan != NULL)
		table_parallelscan_reinitialize(s->rel, s->pscan);
}

static void
scan_initialize_worker(CustomScanState *css, shm_toc *toc, void *coordinate)
{
	VexecScanState *s = (VexecScanState *) css;

	(void) toc;
	s->pscan = (ParallelTableScanDesc) coordinate;
}
