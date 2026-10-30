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
 *						rows it has deleted out of the batch's selection.
 *						Its text stays in that layout -- offsets, views or
 *						Datums -- which kernels and parents read, and is
 *						made the format's only where PostgreSQL's
 *						evaluator reads it, at the rows past the eager
 *						quals (VC14, vexec.enable_source_layouts);
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
 * A VecHashJoin's inner keys (X6; exec.h).  The join above lends the scan
 * on its outer side its keys' range and values once its table is built,
 * and the scan drops a row whose key no inner row has: heap's page reader
 * before the row is deformed, else among a batch's rows.  A source that
 * takes a join's keys is given them as its keys -- the range, and the
 * distinct values where they are few -- to pass over the units none of
 * whose rows any inner row meets (VC3; vexec_source.h, set_join_keys):
 * only where nothing of the scan's may raise, and again for each table the
 * join builds; the join builds it before the scan reads a row.
 *
 * A source's columns in steps (VC2; vexec_source.h, VEXEC_SRC_STEPS): where
 * the source takes them, the scan asks for the columns of its first vector
 * qual first -- with the running bound's and the join keys' where those
 * are checked before the quals, and the TIDs -- then each later qual's of
 * the eager prefix, then the rest, and node.c reads a step only for a
 * batch where the quals before it left a row (next_rest()), so that the
 * source decodes a later step's columns only in its units where a row is
 * left.
 *
 * The scan's tuple is the table's row: the plan's quals and target list
 * read Vars of scanrelid, as a SeqScan's do, so custom_scan_tlist is empty.
 * The node reads the attributes they name, the whole row for a whole-row
 * Var; ctid comes from each row's TID, tableoid from the relation.
 *
 * The device's read of the scan's own units (W13, §3.27; below), where the
 * planner gave the scan the device (plan/paths.c, plan/sort.c): its source's
 * units read and filtered by the device, a bounded VecSort's rank above cut
 * there, and the rows kept handed up once the device has read them all.
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
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "access/stratnum.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/typcache.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/device.h"
#include "exec/exec.h"
#include "plan/plan.h"
#include "expr/expr.h"
#include "expr/kernel.h"
#include "source/source.h"

struct ScanDevice;

/*
 * W13 in a parallel plan: the device read's figures, the owner's, for the
 * leader's EXPLAIN where it was not the owner (scan_shutdown()); `set` once
 * written, as its read is done, before the sort above has its rows
 */
typedef struct ScanDevShared
{
	pg_atomic_uint32 set;
	bool		lost;
	int64		rows_kept;
	int64		rows_out;
	int64		units_redone;
	int64		split;			/* W39 */
	int64		requests;
	double		device_ms;
	double		gpu_ms;			/* OT10 */
	double		wait_ms;
	char		device[64];
} ScanDevShared;

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
	pg_atomic_uint64 *shared_bound; /* the participants' sort bound, in the
									 * DSM before pscan; NULL: none */
	int			late_slots;		/* M4: the rows each participant's sort above
								 * publishes, 0 for none */
	VexecLateShare *late_share; /* and their exchange, in the DSM after pscan */
	int			nattrs;			/* input columns that are attributes */
	AttrNumber *attrs;			/* their attnos */
	bool		need_tid;		/* the last input column is ctid */
	bool		count_quals;	/* F36: begin the source with VEXEC_SRC_COUNT_QUALS */

	/*
	 * F35 and F37: per output column, a VecAgg above reads it only for its
	 * length() (vexec_scan_parent_lengths()), or NULL; per input column, what
	 * is asked of the source for it (plan_lengths()); how many; the batches
	 * that had it so
	 */
	const bool *parent_lengths;
	int			nparent_lengths;
	bool		lengths_planned;
	uint8	   *length_cols;
	int			nlength_cols;
	int64		length_batches;
	int			maxattr;
	bool		done;

	/* H2: the units, and their rows, a source answered from statistics */
	int64		stats_units;
	int64		stats_rows;

	/* VC14: the most columns a batch kept in the layout the source gave them */
	int			kept_cols;

	/*
	 * VC5: per column, the identity the source gave its last dictionary
	 * (dictionary_id()) and vexec's for it, and the batches whose columns
	 * came as dictionaries
	 */
	uint64	   *dict_src_ids;
	uint64	   *dict_ids;
	int64		dict_batches;

	/*
	 * X3: the leading quals heap's page reader checks on a tuple before
	 * deforming it (source/prefilter.c): the checks, how many of the quals
	 * they stand for, and the rows they dropped that EXPLAIN ANALYZE counted
	 */
	VexecHeapCheck *checks;
	int			nchecks;
	int			nchecked_quals;
	int64		prefiltered_counted;

	/*
	 * A VecSort's running bound as a source's key (H6; vexec_source.h,
	 * set_keys): its strategy, 0 where the source is given none, and the
	 * bound's version last given.
	 */
	ScanKeyData bound_key;
	StrategyNumber bound_strategy;
	uint64		bound_given;

	/*
	 * VC3: a VecHashJoin's inner keys given to the source as its join keys
	 * (vexec_source.h, set_join_keys): whether they are, the version of the
	 * join's filter last given -- 0 none, as after a rescan -- what the
	 * last were, in their memory, and how many times they were given
	 */
	bool		jkeys_on;
	uint64		jkeys_given;
	MemoryContext jkeys_cxt;
	const char *jkeys_form;
	int64		jkeys_calls;

	/*
	 * VC4: the scan begun asking its source for its rows in ordering()'s
	 * order (VEXEC_SRC_ORDERED), as a VecAgg above takes them
	 */
	bool		want_order;

	/*
	 * A registered source's columns: spec_cols, per column of the spec, the
	 * input column it is -- the input's order, but in steps where the scan
	 * asks for them (VC2): nsteps, each step's end in the spec (step_ends),
	 * each input column's step (step_of, with the TIDs in step 0); the batch
	 * next() handed, which next_rest() fills; and the batches each step was
	 * read for.
	 */
	int		   *spec_cols;
	AttrNumber *spec_attnums;
	int			nsteps;
	int		   *step_ends;
	bool	  **step_of;
	VexecSourceBatch cur;
	bool		cur_dict;		/* a column of the batch came as a dictionary */
	int64		step_batches[VEXEC_SRC_MAX_STEPS];

	/*
	 * V8e: units handed to a device strategy above (next_unit(), minor 7):
	 * whether the batches now come from one unit alone, whose end ends them
	 * but not the scan; and the units and rows the device read
	 */
	bool		one_unit;
	bool		one_step;		/* W4: a device strategy above filters the
								 * units by the quals: the columns in one
								 * step, as next_unit() asks */
	int64		device_units;
	int64		device_rows;
	int64		cpu_units;

	/*
	 * W13: the device's read of the scan's own units (below): whether the
	 * plan gave it, a bounded VecSort's rank lent in the plan -- its bound,
	 * 0 for none, and its keys, attributes each ascending or descending, its
	 * NULLs first or last -- the device's state, and why it is not, for
	 * EXPLAIN
	 */
	bool		dev_planned;
	int64		dev_k;
	int			dev_nrank;
	AttrNumber *dev_rank_attnos;
	bool	   *dev_rank_desc;
	bool	   *dev_rank_nulls_first;
	struct ScanDevice *dev;
	const char *dev_why;

	/*
	 * W13 in a parallel plan: the participant that reads every unit on the
	 * device, plus one, in the DSM -- 0 until the first to begin claims it;
	 * NULL where there is none -- and this one's claim for the run: 0 not
	 * yet asked, 1 its own, 2 another's
	 */
	pg_atomic_uint32 *dev_owner;
	int			dev_claim;
	bool		dev_alone;		/* W32: its source told it claims alone */
	ScanDevShared *dev_shared;	/* the owner's figures, in the DSM */

	/*
	 * F7 (§3.28): the condition cache (condcache.c): whether the scan reads
	 * and fills it, its quals' key; the bits the source asked for last; the
	 * batch's granule as the source named it (minor 10), and the granule
	 * whose batches come now -- its unit, number, and whether every row of
	 * it so far was seen and none was true -- and, for EXPLAIN ANALYZE, the
	 * granules passed over and those found none true
	 */
	bool		cc_on;
	int64		cc_waits_granule;	/* F19: the granule cc_waits answers, or -1 */
	uint64		cc_waits_unit[2];
	bool		cc_waits;
	int64		cc_bound_waited;
	uint64		cc_key[2];
	uint64		cc_tkey[2];		/* F21: its order-free key, for the relation's figures */
	bool		cc_tally;
	int64		cc_granule_rows;	/* F21: the current granule's rows so far */
	uint64	   *cc_bits;
	uint32		cc_nwords;
	uint64		cc_batch_unit[2];
	int64		cc_batch_granule;
	bool		cc_batch_last;
	uint64		cc_unit[2];
	int64		cc_granule;
	bool		cc_none;
	bool		cc_seen;		/* W34: every row of the granule so far evaluated */
	int64		cc_passed;
	int64		cc_kept;
	int64		cc_unsearched;	/* W34: device units whose granules the cache saw before */
	bool		cc_named;		/* a batch named its unit's granule */
	ScanDevShared dev_copy;		/* and copied, where another was the owner */
	bool		dev_copied;

	/*
	 * PQ21 (pg_vector_executor.md §3.40): per spec column, the nested layout
	 * asked of the source (0 none) -- where the node's kernels read the
	 * column so and nothing else reads its layout -- and its children's
	 * types, two a column
	 */
	uint8	   *nested_asked;
	VexecType **nested_types;
} VexecScanState;

static bool scan_fetch(VexecNode *node);
static bool dev_scan_fetch(VexecScanState *s);
static void dev_scan_setup(VexecScanState *s);
static bool dev_scan_owner(VexecScanState *s);
static void dev_scan_publish(VexecScanState *s);
static void dev_scan_reset(VexecScanState *s);
static void dev_scan_explain(VexecScanState *s, ExplainState *es);
static void scan_begin(CustomScanState *css, EState *estate, int eflags);
static void cc_batch_done(VexecNode *node);
static bool cc_bound_waits(VexecNode *node);
static void plan_lengths(VexecScanState *s);
static const uint64 *cc_granules_passed(void *arg, const uint64 *unit_id, uint32 ngranules, uint32 begin,
										uint32 end);
static TupleTableSlot *scan_exec(CustomScanState *css);
static void scan_end(CustomScanState *css);
static void scan_rescan(CustomScanState *css);
static void scan_shutdown(CustomScanState *css);
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

/* Its methods, for pg_otel_probe's reporter of vector nodes (node.c). */
const CustomExecMethods *
vexec_bitmapscan_exec_methods(void)
{
	return &bitmapscan_exec_methods;
}

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
	.ShutdownCustomScan = scan_shutdown,
	.ExplainCustomScan = scan_explain,
};

/* Its methods, for pg_otel_probe's reporter of vector nodes (node.c). */
const CustomExecMethods *
vexec_scan_exec_methods(void)
{
	return &scan_exec_methods;
}

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

	/*
	 * F7: the condition cache, where a source of minor version 10 reads the
	 * scan's units and its quals are the same for every statement.  The CPU's
	 * batches name their granules, recorded as each ends (cc_batch_done());
	 * the device's read of the units (W13) records by its units' answers
	 * alone (W21, vexec_scan_units_none()): its rows handed up name no
	 * granule
	 */
	s->cc_granule = -1;
	if (s->src != NULL && s->src->minor >= 10 && vexec_condcache_enabled() &&
		vexec_condcache_key(cscan->scan.plan.qual, cscan->scan.scanrelid, s->cc_key))
	{
		s->cc_on = true;
		/* F21: the relation's figures under the quals, for the next planning's workers */
		s->cc_tally = vexec_condcache_planning &&
			vexec_condcache_tally_key(cscan->scan.plan.qual, cscan->scan.scanrelid, s->cc_tkey);
		if (!vexec_scan_plan_is_device(cscan))
		{
			node->after_eval = cc_batch_done;
			node->bound_waits = cc_bound_waits;
			s->cc_waits_granule = -1;
		}
	}

	/* W13: the device's read of the scan's units, and a bounded sort's rank, as the plan says */
	if (!s->bitmap && vexec_scan_plan_is_device(cscan))
	{
		List	   *p = cscan->custom_private;

		s->dev_planned = true;
		if (list_length(p) >= 3)
		{
			s->dev_k = intVal(lsecond(p));
			s->dev_nrank = intVal(lthird(p));
			s->dev_rank_attnos = palloc(sizeof(AttrNumber) * Max(s->dev_nrank, 1));
			s->dev_rank_desc = palloc(sizeof(bool) * Max(s->dev_nrank, 1));
			s->dev_rank_nulls_first = palloc(sizeof(bool) * Max(s->dev_nrank, 1));
			for (i = 0; i < s->dev_nrank; i++)
			{
				s->dev_rank_attnos[i] = (AttrNumber) intVal(list_nth(p, 3 + 3 * i));
				s->dev_rank_desc[i] = intVal(list_nth(p, 4 + 3 * i)) != 0;
				s->dev_rank_nulls_first[i] = intVal(list_nth(p, 5 + 3 * i)) != 0;
			}
		}
	}

	/* X3: the quals heap's page reader checks before deforming a tuple */
	if (s->heap_pages && !s->bitmap && vexec_enable_prefilter && cscan->scan.plan.qual != NIL)
		s->checks = vexec_heap_checks(s->rel, cscan->scan.scanrelid, cscan->scan.plan.qual,
									  &s->nchecks, &s->nchecked_quals);
}

/*
 * F7: the source's call as it selects a unit's granules (vexec_source.h,
 * granules_passed): the condition cache's bits of the unit for the scan's
 * quals, in the scan's memory until the next call -- those of [begin, end)
 * counted -- and NULL where none is set
 */
static const uint64 *
cc_granules_passed(void *arg, const uint64 *unit_id, uint32 ngranules, uint32 begin, uint32 end)
{
	VexecScanState *s = arg;
	uint32		nwords = (ngranules + 63) / 64;
	int64		n;

	if (nwords > s->cc_nwords)
	{
		s->cc_bits = s->cc_bits ? repalloc(s->cc_bits, sizeof(uint64) * nwords) :
			MemoryContextAlloc(s->node.mcxt, sizeof(uint64) * nwords);
		s->cc_nwords = nwords;
	}
	n = vexec_condcache_get(s->cc_key, unit_id, ngranules, begin, end, s->cc_bits);
	if (n == 0)
		return NULL;
	s->cc_passed += n;
	return s->cc_bits;
}

/*
 * F19 (§3.38): a running bound planned before the quals waits for them in a
 * batch of a granule the condition cache has not seen whole for them: a bound
 * first would leave the cache unlearning, the query's every later try reading
 * the granule again.  At most a granule's first try's rows evaluated more.
 */
static bool
cc_bound_waits(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;

	if (!vexec_bound_fills_cache || s->cc_batch_granule < 0 || s->cc_batch_granule > PG_INT32_MAX ||
		node->in->selection != NULL)
		return false;
	if (s->cc_waits_granule != s->cc_batch_granule || s->cc_waits_unit[0] != s->cc_batch_unit[0] ||
		s->cc_waits_unit[1] != s->cc_batch_unit[1])
	{
		int32		g = (int32) s->cc_batch_granule;

		s->cc_waits_granule = s->cc_batch_granule;
		s->cc_waits_unit[0] = s->cc_batch_unit[0];
		s->cc_waits_unit[1] = s->cc_batch_unit[1];
		s->cc_waits = !vexec_condcache_all_seen(s->cc_key, s->cc_batch_unit, &g, 1);
	}
	if (s->cc_waits)
		s->cc_bound_waited++;
	return s->cc_waits;
}

/*
 * F7: a batch's eager parts done (node.c, after_eval): whether the open
 * granule's rows so far were all seen and none was true -- every row of the
 * batch visible and evaluated, no row left to PostgreSQL's evaluator or a
 * child's, no running bound or join filter among what removed them, no
 * candidate left -- and at the granule's last batch, the granule kept so
 * where it is.  A source hands a granule's batches in turn, its first rows
 * first.
 */
static void
cc_batch_done(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;
	int			n = node->in->nrows;

	if (s->cc_batch_granule < 0)
	{
		s->cc_granule = -1;
		return;
	}
	s->cc_named = true;
	if (s->cc_granule != s->cc_batch_granule || s->cc_unit[0] != s->cc_batch_unit[0] ||
		s->cc_unit[1] != s->cc_batch_unit[1])
	{
		s->cc_granule = s->cc_batch_granule;
		s->cc_unit[0] = s->cc_batch_unit[0];
		s->cc_unit[1] = s->cc_batch_unit[1];
		s->cc_none = true;
		s->cc_seen = true;
		s->cc_granule_rows = 0;
	}
	s->cc_granule_rows += n;
	/*
	 * W34: a granule seen whole, every row of it evaluated, whether some is
	 * true or none.  F19: a running bound checked after the quals keeps that
	 * so, the rows true the quals' before it.
	 */
	if (node->in->selection != NULL || node->child_redo != NULL ||
		(node->bound != NULL && !(vexec_bound_fills_cache && (!node->bound->before_quals || node->bound_waited) &&
								  node->first_lazy == node->nquals)) ||
		node->jfilter != NULL || vexec_bits_any(node->redo, n))
		s->cc_seen = false;
	if (!s->cc_seen || (node->bound != NULL ? node->quals_left : vexec_bits_any(node->candidates, n)))
		s->cc_none = false;
	if (s->cc_batch_last)
	{
		if (s->cc_none)
		{
			int			first = vexec_condcache_set(s->cc_key, s->cc_unit, s->cc_granule);

			s->cc_kept++;
			if (s->cc_tally && first != 0)
				vexec_condcache_tally_add(RelationGetRelid(s->rel), s->cc_tkey, (first & 2) ? 1 : 0,
										  (first & 1) ? 1 : 0, (first & 2) ? s->cc_granule_rows : 0);
		}
		else if (s->cc_seen)
		{
			int32		g = (int32) s->cc_granule;

			if (s->cc_granule >= 0 && s->cc_granule <= PG_INT32_MAX &&
				vexec_condcache_seen(s->cc_key, s->cc_unit, &g, 1) > 0 && s->cc_tally)
				vexec_condcache_tally_add(RelationGetRelid(s->rel), s->cc_tkey, 1, 0, s->cc_granule_rows);
		}
		s->cc_granule = -1;
	}
}

/*
 * W21 (§3.28): of a units request the device answered, the granules of a unit
 * none of whose rows its filter -- the scan's quals, whole -- kept, a bit each
 * of the unit's granules in their order, kept in the condition cache as the
 * granules the CPU's scan found none true are (cc_batch_done()): a MERGETREE
 * unit's, none of whose rows is deleted, by its part's identity.
 */
void
vexec_scan_units_none(VexecNode *node, const VexecDevUnitDesc *x, const uint64 *none)
{
	VexecScanState *s = (VexecScanState *) node;

	Assert(node->kind == VEXEC_NODE_SCAN);
	if (!s->cc_on || x->granule_rows == NULL || x->deleted != NULL || (x->unit_id[0] == 0 && x->unit_id[1] == 0))
		return;
	{
		int64		seen = 0;
		int64		nones = 0;

		for (int g = 0; g < x->nparts; g++)
			if (none[g / 64] & (UINT64CONST(1) << (g % 64)))
			{
				int			first = vexec_condcache_set(s->cc_key, x->unit_id, (int64) x->parts[g]);

				s->cc_kept++;
				seen += (first & 2) ? 1 : 0;
				nones += (first & 1) ? 1 : 0;
			}
		/* W34: every granule of the unit searched, those of a row true too */
		seen += vexec_condcache_seen(s->cc_key, x->unit_id, x->parts, x->nparts);
		if (s->cc_tally)
			vexec_condcache_tally_add(RelationGetRelid(s->rel), s->cc_tkey, seen, nones, 0);
	}
	s->cc_named = true;
}

/*
 * W34 (§3.30): a MERGETREE unit's granules' rows kept in its request -- for
 * the executor to search its granules none of whose rows the filter keeps
 * (W21) -- only where the scan keeps the condition cache and the cache has
 * not seen every one of the unit's granules for the scan's quals: at a hot
 * try the device searched them already, or a CPU scan saw them, and the
 * granules it can find are the cache's
 */
void
vexec_scan_unit_granules(VexecNode *node, VexecDevUnitDesc *x)
{
	VexecScanState *s = (VexecScanState *) node;

	Assert(node->kind == VEXEC_NODE_SCAN);
	if (x->granule_rows == NULL)
		return;
	if (s->cc_on && x->deleted == NULL && (x->unit_id[0] != 0 || x->unit_id[1] != 0))
	{
		if (!vexec_condcache_all_seen(s->cc_key, x->unit_id, x->parts, x->nparts))
			return;
		s->cc_unsearched++;
	}
	pfree(x->granule_rows);
	x->granule_rows = NULL;
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

/*
 * X6: the join's inner keys checked by heap's page reader, on a tuple first,
 * where they come before the quals; else among a batch's rows (node.c).
 */
static void
join_filter_to_reader(VexecScanState *s)
{
	VexecJoinFilter *f = s->node.jfilter;

	if (f != NULL && f->before_quals && s->heap != NULL && !s->bitmap)
	{
		vexec_heap_reader_set_join_filter(s->heap, f, f->attno);
		s->node.jfilter_in_source = true;
	}
}

/*
 * VC3: whether the scan's source takes a join's inner keys on attribute
 * attno, a column it reads (vexec_source.h, set_join_keys): one that has
 * the member, an integer column, as X6's keys are, and nothing of the
 * scan's that may raise -- a unit the source passes over leaves its rows'
 * quals and targets unevaluated.
 */
static bool
source_takes_join_keys(VexecScanState *s, AttrNumber attno)
{
	Plan	   *plan = s->node.css.ss.ps.plan;
	Oid			typid;

	if (!vexec_enable_source_join_keys || s->bitmap || s->src == NULL ||
		!VEXEC_SOURCE_HAS(s->src, set_join_keys) ||
		attno < 1 || attno > RelationGetNumberOfAttributes(s->rel))
		return false;
	typid = getBaseType(TupleDescAttr(RelationGetDescr(s->rel), attno - 1)->atttypid);
	if (typid != INT2OID && typid != INT4OID && typid != INT8OID)
		return false;
	return !vexec_expr_may_raise((Node *) plan->targetlist) &&
		!vexec_expr_may_raise((Node *) plan->qual) &&
		!vexec_expr_may_raise((Node *) ((CustomScan *) plan)->custom_exprs);
}

/*
 * VC3: whether a VecHashJoin's inner keys on attno, lent to ps, would go to
 * its source: the join then builds its table before it reads an outer row.
 */
bool
vexec_scan_takes_join_keys(PlanState *ps, AttrNumber attno)
{
	VexecScanState *s;

	if (!IsA(ps, CustomScanState) || ((CustomScanState *) ps)->methods != &scan_exec_methods)
		return false;
	s = (VexecScanState *) ps;
	for (int col = 0; col < s->nattrs; col++)
		if (s->attrs[col] == attno)
			return source_takes_join_keys(s, attno);
	return false;
}

/*
 * VC3: the join's inner keys as the source's join keys, in the column's
 * type -- at least the least, at most the greatest, and one of the values
 * where the join counted them -- clamped to the type's range: how many,
 * none where no value of the type is within it.
 */
static int
make_join_keys(VexecScanState *s, VexecJoinFilter *f, ScanKeyData *keys)
{
	Oid			typid = getBaseType(TupleDescAttr(RelationGetDescr(s->rel), f->attno - 1)->atttypid);
	TypeCacheEntry *tc = lookup_type_cache(typid, TYPECACHE_BTREE_OPFAMILY);
	int64		tmin = typid == INT2OID ? PG_INT16_MIN : typid == INT4OID ? PG_INT32_MIN : PG_INT64_MIN;
	int64		tmax = typid == INT2OID ? PG_INT16_MAX : typid == INT4OID ? PG_INT32_MAX : PG_INT64_MAX;
	int64		lo = Max(f->min, tmin);
	int64		hi = Min(f->max, tmax);
	StrategyNumber strategies[3] = {BTGreaterEqualStrategyNumber, BTLessEqualStrategyNumber,
	BTEqualStrategyNumber};
	Oid			procs[3];
	int			n = 2;

	if (lo > hi || !OidIsValid(tc->btree_opf))
		return 0;
	for (int k = 0; k < 3; k++)
	{
		Oid			opno = get_opfamily_member(tc->btree_opf, typid, typid, strategies[k]);

		if (!OidIsValid(opno))
			return 0;
		procs[k] = get_opcode(opno);
	}
#define INT_DATUM(v) (typid == INT2OID ? Int16GetDatum((int16) (v)) : \
					  typid == INT4OID ? Int32GetDatum((int32) (v)) : Int64GetDatum(v))
	ScanKeyEntryInitialize(&keys[0], 0, f->attno, BTGreaterEqualStrategyNumber, typid, InvalidOid,
						   procs[0], INT_DATUM(lo));
	ScanKeyEntryInitialize(&keys[1], 0, f->attno, BTLessEqualStrategyNumber, typid, InvalidOid,
						   procs[1], INT_DATUM(hi));
	s->jkeys_form = "range";
	if (f->nvalues > 0)
	{
		Datum	   *elems = palloc(sizeof(Datum) * f->nvalues);
		int			m = 0;

		for (int i = 0; i < f->nvalues; i++)
			if (f->values[i] >= lo && f->values[i] <= hi)
				elems[m++] = INT_DATUM(f->values[i]);
		if (m > 0)
		{
			int16		typlen;
			bool		typbyval;
			char		typalign;

			get_typlenbyvalalign(typid, &typlen, &typbyval, &typalign);
			ScanKeyEntryInitialize(&keys[n++], SK_SEARCHARRAY, f->attno, BTEqualStrategyNumber, typid,
								   InvalidOid, procs[2],
								   PointerGetDatum(construct_array(elems, m, typid, typlen, typbyval, typalign)));
			s->jkeys_form = psprintf("range and %d values", m);
		}
	}
#undef INT_DATUM
	return n;
}

/*
 * VC3: the join's keys, of a table built since the source was last given
 * them, given to it; taken back where the join's table holds none to lend
 * -- it spilled, or is to be built again.
 */
static void
join_keys_to_source(VexecScanState *s)
{
	VexecJoinFilter *f = s->node.jfilter;
	MemoryContext old;
	ScanKeyData keys[3];
	int			n;

	if (!s->jkeys_on || s->srcstate == NULL)
		return;
	if (!f->active)
	{
		if (s->jkeys_given != 0)
			s->src->set_join_keys(s->srcstate, 0, NULL);
		s->jkeys_given = 0;
		return;
	}
	if (f->version == s->jkeys_given)
		return;
	MemoryContextReset(s->jkeys_cxt);
	s->jkeys_form = NULL;
	old = MemoryContextSwitchTo(s->jkeys_cxt);
	n = make_join_keys(s, f, keys);
	MemoryContextSwitchTo(old);
	s->src->set_join_keys(s->srcstate, n, keys);
	s->jkeys_given = f->version;
	if (n > 0)
		s->jkeys_calls++;
}

/*
 * A VecHashJoin's inner keys lent to this scan, the vector scan on its outer
 * side (X6, exec.h): the scan drops a row whose key no inner row has.
 * Before the quals where none of them may raise, else after them, where
 * every one is eager; never where the target list may raise, which
 * PostgreSQL computes for every row the quals pass.  False where the node
 * is no vector scan, reads no column of the key's, or cannot take it.
 */
bool
vexec_scan_set_join_filter(PlanState *ps, VexecJoinFilter *filter)
{
	VexecScanState *s;
	Plan	   *plan;
	int			col;

	if (!IsA(ps, CustomScanState) ||
		(((CustomScanState *) ps)->methods != &scan_exec_methods &&
		 ((CustomScanState *) ps)->methods != &bitmapscan_exec_methods))
		return false;
	s = (VexecScanState *) ps;
	plan = ps->plan;
	for (col = 0; col < s->nattrs; col++)
		if (s->attrs[col] == filter->attno)
			break;
	if (col >= s->nattrs || vexec_expr_may_raise((Node *) plan->targetlist))
		return false;
	filter->before_quals = !vexec_expr_may_raise((Node *) plan->qual) &&
		!vexec_expr_may_raise((Node *) ((CustomScan *) plan)->custom_exprs);
	if (!filter->before_quals && s->node.first_lazy < s->node.nquals)
		return false;
	s->node.jfilter = filter;
	s->node.jfilter_col = col;
	s->node.jfilter_in_source = false;
	/* VC3: and to the source, where it takes them */
	s->jkeys_on = filter->before_quals && source_takes_join_keys(s, filter->attno);
	if (s->jkeys_on && s->jkeys_cxt == NULL)
		s->jkeys_cxt = AllocSetContextCreate(s->node.mcxt, "vexec join keys", ALLOCSET_SMALL_SIZES);
	if (s->scan != NULL)
		join_filter_to_reader(s);
	return true;
}

static void scan_load_step(VexecNode *node, int step, const uint64 *rows);

/*
 * VC2: the order the scan asks its source for its columns in, and the
 * steps they come in where the source takes steps (next_rest()) and
 * vexec.enable_source_steps is on: the first vector qual's columns first --
 * with the running bound's and the join keys' where node.c checks those
 * before the quals -- then, for each later qual of the eager prefix that
 * reads a column not read by then, its new columns, up to
 * VEXEC_SRC_MAX_STEPS - 1 such steps, and every other column last; the
 * TIDs with the first.  Each step's columns in the input's order.  One
 * step -- the input's order -- where the prefix reads every column, or
 * there is no eager qual.
 */
static void
plan_steps(VexecScanState *s)
{
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) node->css.ss.ps.plan;
	Index		relid = cscan->scan.scanrelid;
	int8	   *step = palloc(sizeof(int8) * Max(s->nattrs, 1));
	int			nsteps = 0;
	int			k = 0;
	int			j = 0;
	int			col;
	ListCell   *lc;

	s->spec_cols = palloc(sizeof(int) * Max(s->nattrs, 1));
	s->spec_attnums = palloc(sizeof(AttrNumber) * Max(s->nattrs, 1));
	s->nsteps = 0;
	node->nsteps = 0;
	for (col = 0; col < s->nattrs; col++)
	{
		s->spec_cols[col] = col;
		s->spec_attnums[col] = s->attrs[col];
	}
	if (s->src == NULL || !VEXEC_SOURCE_HAS(s->src, next_rest) || !vexec_enable_source_steps ||
		node->first_lazy == 0 || s->nattrs < 2 || s->one_step)
		return;

	memset(step, -1, sizeof(int8) * s->nattrs);
	if (node->bound != NULL && node->bound->before_quals && !node->bound_in_source)
		step[node->bound_col] = 0;
	if (node->jfilter != NULL && node->jfilter->before_quals && !node->jfilter_in_source &&
		node->jfilter_col < s->nattrs)
		step[node->jfilter_col] = 0;
	for (col = 0; col < s->nattrs; col++)
		if (step[col] == 0)
			nsteps = 1;
	node->qual_step = palloc0(sizeof(int8) * Max(node->nquals, 1));
	foreach(lc, cscan->scan.plan.qual)
	{
		Bitmapset  *attrs = NULL;
		bool		whole;
		bool		fresh = false;
		int			last = 0;

		if (k >= node->first_lazy)
			break;
		pull_varattnos(lfirst(lc), relid, &attrs);
		whole = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs);
		for (col = 0; col < s->nattrs; col++)
			if (step[col] < 0 &&
				(whole || bms_is_member(s->attrs[col] - FirstLowInvalidHeapAttributeNumber, attrs)))
				fresh = true;
		if (fresh)
		{
			/* the steps left for the quals taken: the qual and those after it last */
			if (nsteps >= VEXEC_SRC_MAX_STEPS - 1)
				break;
			for (col = 0; col < s->nattrs; col++)
				if (step[col] < 0 &&
					(whole || bms_is_member(s->attrs[col] - FirstLowInvalidHeapAttributeNumber, attrs)))
					step[col] = nsteps;
			nsteps++;
		}
		for (col = 0; col < s->nattrs; col++)
			if (whole || bms_is_member(s->attrs[col] - FirstLowInvalidHeapAttributeNumber, attrs))
				last = Max(last, step[col]);
		node->qual_step[k++] = last;
		bms_free(attrs);
	}
	for (col = 0; col < s->nattrs; col++)
		if (step[col] < 0)
		{
			for (int c = col; c < s->nattrs; c++)
				if (step[c] < 0)
					step[c] = nsteps;
			nsteps++;
			break;
		}
	for (; k < node->nquals; k++)
		node->qual_step[k] = nsteps - 1;
	if (nsteps < 2)
	{
		pfree(step);
		node->qual_step = NULL;
		return;
	}

	/* the spec's columns step by step, each step's in the input's order */
	s->step_ends = palloc(sizeof(int) * nsteps);
	s->step_of = palloc(sizeof(bool *) * nsteps);
	node->in_step = palloc0(sizeof(int8) * Max(node->ninput, 1));
	for (int st = 0; st < nsteps; st++)
	{
		s->step_of[st] = palloc0(sizeof(bool) * Max(node->ninput, 1));
		for (col = 0; col < s->nattrs; col++)
			if (step[col] == st)
			{
				s->spec_cols[j] = col;
				s->spec_attnums[j++] = s->attrs[col];
				s->step_of[st][col] = true;
				node->in_step[col] = st;
			}
		s->step_ends[st] = j;
	}
	/* the TIDs come with next() */
	if (s->need_tid)
		s->step_of[0][s->nattrs] = true;
	pfree(step);
	s->nsteps = nsteps;
	node->nsteps = nsteps;
	node->load_step = scan_load_step;
}

static void ask_nested(VexecScanState *s, uint8 *layouts);

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

		/* VC2: the columns' order, in steps where the source takes them */
		if (s->spec_cols == NULL)
			plan_steps(s);
		for (i = 0; i < s->nattrs; i++)
		{
			VexecShape	shape;

			vexec_type_shape(s->node.in->types[s->spec_cols[i]], &s->node.layout, &shape);
			layouts[i] = shape.layout;
		}
		/* PQ21 (§3.40): the nested layouts the node's kernels read, of a source that hands them */
		if (s->src->minor >= 15 && vexec_enable_source_layouts)
			ask_nested(s, layouts);
		spec->size = sizeof(VexecSourceSpec);
		spec->ncolumns = s->nattrs;
		spec->attnums = s->spec_attnums;
		spec->layouts = layouts;
		spec->quals = pruning_quals((CustomScan *) s->node.css.ss.ps.plan);
		spec->max_rows = vexec_batch_rows;
		spec->flags = s->need_tid ? VEXEC_SRC_TIDS : 0;
		if (s->want_order)
			spec->flags |= VEXEC_SRC_ORDERED;
		if (s->count_quals)
			spec->flags |= VEXEC_SRC_COUNT_QUALS;
		if (s->nsteps > 0)
		{
			spec->flags |= VEXEC_SRC_STEPS;
			spec->nsteps = s->nsteps;
			spec->step_ends = s->step_ends;
		}
		/* F7: the condition cache's granules passed over, and each batch's granule named */
		if (s->cc_on)
		{
			spec->granules_passed = cc_granules_passed;
			spec->granules_arg = s;
		}
		/* W26: a device's units of several ranges of granules taken (exec/device.c) */
		spec->unit_ranges = true;
		/* F25: the running bound's keys may pass over a unit's granules (set_keys, minor 17) */
		spec->granule_keys = vexec_enable_granule_bound;
		/* F35 and F37: the columns read for their lengths alone, in the spec's order */
		plan_lengths(s);
		if (s->length_cols != NULL)
		{
			uint8	   *lens = palloc(Max(s->nattrs, 1));

			for (i = 0; i < s->nattrs; i++)
				lens[i] = s->length_cols[s->spec_cols[i]];
			spec->length_columns = lens;
		}
		/* VC5: a source that keeps a column as a dictionary hands it so */
		if (vexec_batch_dictionary)
		{
			spec->flags |= VEXEC_SRC_DICT;
			if (s->dict_ids == NULL)
			{
				s->dict_src_ids = palloc0(sizeof(uint64) * Max(s->nattrs, 1));
				s->dict_ids = palloc0(sizeof(uint64) * Max(s->nattrs, 1));
			}
		}
		s->srcstate = s->src->begin(s->scan, spec);
		return;
	}
	if (s->heap_pages)
		s->heap = vexec_heap_reader_begin(s->scan, s->node.in, s->nattrs, s->attrs,
										  s->need_tid);
	if (s->heap == NULL)
		s->amslot = table_slot_create(s->rel, NULL);
	else if (s->nchecks > 0)
		vexec_heap_reader_set_checks(s->heap, s->checks, s->nchecks);
	bound_to_reader(s);
	join_filter_to_reader(s);
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
	while (in->nrows < in->room)
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
	bool		ok = vexec_heap_reader_next(s->heap, in);

	/* the rows the reader's checks dropped, which the quals remove (X3) */
	if (s->nchecks > 0)
	{
		int64		prefiltered = vexec_heap_reader_prefiltered(s->heap);

		InstrCountFiltered1(&s->node.css, prefiltered - s->prefiltered_counted);
		s->prefiltered_counted = prefiltered;
	}
	if (!ok)
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
	while (in->nrows < in->room)
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

/*
 * A source's dictionary column as the contract has it (VC5): its codes
 * within the dictionary, and no NULL among its values -- a NULL row is one
 * whose validity bit is clear.
 */
static void
check_dictionary(VexecScanState *s, const VexecVec *v, int col)
{
	const VexecVec *d = v->dictionary;
	int			i;

	if (d->validity != NULL && vexec_vec_null_count(NULL, d) > 0)
		elog(ERROR, "vexec: the batch source \"%s\" gave a dictionary holding a NULL in column %d",
			 s->src->name, col + 1);
	for (i = 0; i < v->nvalues; i++)
		if ((uint32) v->codes[i] >= (uint32) d->nvalues)
			elog(ERROR, "vexec: the batch source \"%s\" gave code %d past its dictionary's %d values in column %d",
				 s->src->name, v->codes[i], d->nvalues, col + 1);
}

/*
 * vexec's identity for a source column's dictionary -- column `col` of the
 * spec: one for each identity the source gives (dictionary_id()), new each
 * time it changes; 0 where the source gives none
 */
static uint64
dictionary_identity(VexecScanState *s, int col)
{
	uint64		sid;

	if (!VEXEC_SOURCE_HAS(s->src, dictionary_id))
		return 0;
	sid = s->src->dictionary_id(s->srcstate, col);
	if (sid == 0)
		return 0;
	if (sid != s->dict_src_ids[col])
	{
		s->dict_src_ids[col] = sid;
		s->dict_ids[col] = vexec_dict_new_id();
	}
	return s->dict_ids[col];
}

/* minor version 10's flags took VexecColumn's padding byte: its size and its other members stay */
StaticAssertDecl(offsetof(VexecColumn, flags) == 3 && offsetof(VexecColumn, scale) == 4,
				 "VexecColumn's members moved");

/*
 * PQ21 (§3.40): a nested column of the source's batch, which the scan asked
 * for (ask_nested()): its children into input columns of the elements',
 * keys' and values' types, and its source's Datums of its rows
 */
static void
nested_column(VexecScanState *s, const VexecSourceBatch *sb, int j, VexecVec *v)
{
	VexecBatch *in = s->node.in;
	int			nkids = v->shape.layout == VEXEC_MAP ? 2 : 1;
	const int32 *off = (const int32 *) v->values;
	VexecNestedSource *ns;

	if (s->src->minor < 15 || s->nested_asked == NULL || s->nested_asked[j] != v->shape.layout ||
		sb->children == NULL || sb->nested_datum == NULL || v->encoding != VEXEC_FLAT || off == NULL)
		elog(ERROR, "vexec: the batch source \"%s\" gave a nested column it was not asked for, or one without its children",
			 s->src->name);
	for (int k = 0; k < nkids; k++)
	{
		const VexecColumn *cc = sb->children[2 * j + k];
		VexecVec   *cv = vexec_batch_alloc0(in, sizeof(VexecVec));

		if (cc == NULL || cc->encoding != VEXEC_FLAT || VEXEC_NESTED_LAYOUT(cc->layout))
			elog(ERROR, "vexec: the batch source \"%s\" gave a nested column whose child is not flat",
				 s->src->name);
		cv->type = s->nested_types[2 * j + k];
		cv->shape.layout = cc->layout;
		cv->shape.arrow_values = cc->arrow_values;
		cv->shape.scale = cc->scale;
		cv->shape.width = cc->width;
		cv->shape.stride = cc->stride;
		vexec_shape_normalize(&cv->shape);
		cv->encoding = VEXEC_FLAT;
		cv->flags = vexec_trust_source_text && (cc->flags & VEXEC_COL_TEXT_WHOLE) ? VEXEC_VEC_TEXT_WHOLE : 0;
		cv->nvalues = sb->nrows > 0 ? off[sb->nrows] : 0;
		cv->validity = (uint64 *) cc->validity;
		cv->values = (void *) cc->values;
		cv->buffers = (char **) cc->buffers;
		cv->buffer_sizes = (int64 *) cc->buffer_sizes;
		cv->nbuffers = cc->nbuffers;
		cv->datums = (Datum *) cc->datums;
		v->children[k] = cv;
	}
	ns = vexec_batch_alloc(in, sizeof(VexecNestedSource));
	ns->datum = sb->nested_datum;
	ns->owner = sb->owner;
	ns->column = j;
	v->nested = ns;
}

/*
 * PQ21: the nested layouts asked of a source of minor version 15 or later,
 * of the columns the node's kernels read so alone -- a list's elements, a
 * map's keys and values its children, typed here -- into layouts[]
 */
static void
ask_nested(VexecScanState *s, uint8 *layouts)
{
	VexecNode  *node = &s->node;

	s->nested_asked = palloc0(Max(s->nattrs, 1));
	s->nested_types = palloc0(sizeof(VexecType *) * 2 * Max(s->nattrs, 1));
	for (int j = 0; j < s->nattrs; j++)
	{
		int			col = s->spec_cols[j];
		const VexecType *t = node->in->types[col];
		int			use;

		if (t->tclass != VEXEC_TC_VARLENA || (node->bound != NULL && col == node->bound_col) ||
			(node->jfilter != NULL && col == node->jfilter_col))
			continue;
		use = vexec_node_nested_use(node, col);
		/* an output a vector parent's kernels alone read nested, as it is (vexec_node_parent_nested()) */
		for (int q = 0; q < node->ntargets && use >= 0 && node->out_nested != NULL; q++)
		{
			const VexecExpr *e = node->targets[q].eager;

			if (node->out_nested[q] != 0 && e != NULL && e->kind == VE_VAR && e->col == col)
				use = use > 0 && use != node->out_nested[q] ? -1 : node->out_nested[q];
		}
		if (use <= 0)
			continue;
		if (use == VEXEC_LIST)
		{
			Oid			elem = get_element_type(t->basetype);

			if (!OidIsValid(elem))
				continue;
			s->nested_types[2 * j] = vexec_type_make(elem, t->basetypmod, t->collation);
		}
		else
		{
			s->nested_types[2 * j] = vexec_type_make(TEXTOID, -1, DEFAULT_COLLATION_OID);
			s->nested_types[2 * j + 1] = vexec_type_make(TEXTOID, -1, DEFAULT_COLLATION_OID);
		}
		s->nested_asked[j] = (uint8) use;
		layouts[j] = (uint8) use;
	}
}

/*
 * Column j of the source's batch -- of its spec -- into the input column it
 * is, as the source holds it (fetch_source(), scan_load_step()).
 */
static void
source_column(VexecScanState *s, const VexecSourceBatch *sb, int j)
{
	VexecBatch *in = s->node.in;
	int			i = s->spec_cols[j];
	const VexecColumn *c = &sb->columns[j];
	VexecVec   *v = &in->cols[i];

	v->shape.layout = c->layout;
	v->shape.arrow_values = c->arrow_values;
	v->shape.scale = c->scale;
	v->shape.width = c->width;
	v->shape.stride = c->stride;
	vexec_shape_normalize(&v->shape);
	v->encoding = c->encoding;
	/* F5: the source's word that its text is whole, from minor version 10, where it is taken */
	v->flags = vexec_trust_source_text && s->src->minor >= 10 && (c->flags & VEXEC_COL_TEXT_WHOLE) ?
		VEXEC_VEC_TEXT_WHOLE : 0;
	/* F35 and F37: character counts, no bytes: only where the scan asked them */
	if ((c->flags & VEXEC_COL_LENGTHS) && s->length_cols != NULL)
	{
		v->flags = VEXEC_VEC_LENGTHS;
		s->length_batches++;
	}
	v->nvalues = c->encoding == VEXEC_CONST ? 1 : sb->nrows;
	v->validity = (uint64 *) c->validity;
	v->values = (void *) c->values;
	v->buffers = (char **) c->buffers;
	v->buffer_sizes = (int64 *) c->buffer_sizes;
	v->nbuffers = c->nbuffers;
	v->datums = (Datum *) c->datums;
	v->codes = NULL;
	v->dictionary = NULL;
	v->dict_id = 0;
	if (c->encoding == VEXEC_DICT)
	{
		VexecVec   *d = vexec_batch_alloc0(in, sizeof(VexecVec));
		const VexecColumn *dc = c->dictionary;

		if (s->dict_ids == NULL || dc == NULL || c->codes == NULL ||
			c->ndictionary <= 0 || dc->encoding != VEXEC_FLAT)
			elog(ERROR, "vexec: the batch source \"%s\" gave a dictionary column it was not asked for, or one without its dictionary",
				 s->src->name);
		v->codes = (int32 *) c->codes;
		d->type = v->type;
		d->shape.layout = dc->layout;
		d->shape.arrow_values = dc->arrow_values;
		d->shape.scale = dc->scale;
		d->shape.width = dc->width;
		d->shape.stride = dc->stride;
		vexec_shape_normalize(&d->shape);
		d->encoding = VEXEC_FLAT;
		d->flags = v->flags;
		d->nvalues = c->ndictionary;
		d->validity = (uint64 *) dc->validity;
		d->values = (void *) dc->values;
		d->buffers = (char **) dc->buffers;
		d->buffer_sizes = (int64 *) dc->buffer_sizes;
		d->nbuffers = dc->nbuffers;
		d->datums = (Datum *) dc->datums;
		v->dictionary = d;
		/* the column is read by its dictionary's shape, whatever it said */
		v->shape = d->shape;
		check_dictionary(s, v, i);
		v->dict_id = dictionary_identity(s, j);
		s->cur_dict = true;
	}
	v->children[0] = v->children[1] = NULL;
	v->nested = NULL;
	if (VEXEC_NESTED_LAYOUT(c->layout))
		nested_column(s, sb, j, v);
}

/*
 * VC2: a later step's column until it is read, every row NULL, so that a
 * read of it before -- none is made -- would find no value of another row.
 */
static void
step_placeholder(VexecBatch *in, VexecVec *v)
{
	VexecShape	shape = v->shape;

	vexec_vec_init(in, v, &shape, 1);
	v->encoding = VEXEC_CONST;
	v->validity = vexec_bitmap_alloc(in, 1, false);
}

/* A registered source's next batch, its columns as the source holds them. */
static bool
fetch_source(VexecScanState *s)
{
	VexecBatch *in = s->node.in;
	VexecSourceBatch *sb = &s->cur;
	int			nfirst = s->nsteps > 0 ? s->step_ends[0] : s->nattrs;
	int			j;

	memset(sb, 0, sizeof(VexecSourceBatch));
	vexec_batch_reset(in);
	bound_to_source(s);
	join_keys_to_source(s);
	if (!s->src->next(s->srcstate, sb))
	{
		/* V8e: a unit's end alone, where its batches were asked for */
		if (s->one_unit)
			s->one_unit = false;
		else
			s->done = true;
		return false;
	}
	in->nrows = sb->nrows;
	in->selection = (uint64 *) sb->visible;
	s->cur_dict = false;
	/* F7: the batch's granule, as a source of minor version 10 names it */
	if (s->cc_on)
	{
		s->cc_batch_unit[0] = sb->unit_id[0];
		s->cc_batch_unit[1] = sb->unit_id[1];
		s->cc_batch_granule = sb->unit_id[0] == 0 && sb->unit_id[1] == 0 ? -1 : sb->granule;
		s->cc_batch_last = sb->granule_last;
	}
	for (j = 0; j < nfirst; j++)
		source_column(s, sb, j);
	/* VC2: the later steps' columns come from next_rest() (scan_load_step()) */
	for (; j < s->nattrs; j++)
		step_placeholder(in, &in->cols[s->spec_cols[j]]);
	if (s->cur_dict)
		s->dict_batches++;
	if (s->need_tid)
	{
		VexecVec   *v = &in->cols[s->nattrs];

		if (sb->tids == NULL)
			elog(ERROR, "vexec: the batch source \"%s\" gave no TIDs", s->src->name);

		memset(&v->shape, 0, sizeof(VexecShape));
		v->shape.layout = VEXEC_FIXED;
		v->shape.width = sizeof(ItemPointerData);
		v->shape.stride = sizeof(ItemPointerData);
		v->encoding = VEXEC_FLAT;
		v->nvalues = sb->nrows;
		v->validity = NULL;
		v->values = (void *) sb->tids;
	}

	/*
	 * VC14: text stays as the source handed it -- kernels and parents read
	 * its layout -- and is made the format's only for PostgreSQL's
	 * evaluator, after the eager quals (node.c, convert_kept())
	 */
	if (vexec_enable_source_layouts)
	{
		s->node.in_kept = vexec_batch_apply_config_cols(in, &s->node.layout,
														s->nsteps > 0 ? s->step_of[0] : NULL, true);
		s->kept_cols = Max(s->kept_cols, s->node.in_kept);
	}
	else
		(void) vexec_batch_apply_config_cols(in, &s->node.layout,
											 s->nsteps > 0 ? s->step_of[0] : NULL, false);
	if (s->nsteps > 0)
		s->step_batches[0]++;
	return true;
}

/*
 * VC2: step `step` of the batch now in the input, from the source's
 * next_rest(), for at least the rows of `rows` (node.c, load_steps()): its
 * columns into the input, given the format's layouts as fetch_source()
 * gives the first step's.
 */
static void
scan_load_step(VexecNode *node, int step, const uint64 *rows)
{
	VexecScanState *s = (VexecScanState *) node;
	bool		had_dict = s->cur_dict;
	int			j;

	Assert(step > 0 && step < s->nsteps);
	s->src->next_rest(s->srcstate, &s->cur, step, rows);
	for (j = s->step_ends[step - 1]; j < s->step_ends[step]; j++)
		source_column(s, &s->cur, j);
	if (s->cur_dict && !had_dict)
		s->dict_batches++;
	if (vexec_enable_source_layouts)
	{
		node->in_kept += vexec_batch_apply_config_cols(node->in, &node->layout, s->step_of[step], true);
		s->kept_cols = Max(s->kept_cols, node->in_kept);
	}
	else
		(void) vexec_batch_apply_config_cols(node->in, &node->layout, s->step_of[step], false);
	s->step_batches[step]++;
}

static bool
scan_fetch(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;

	if (s->scan == NULL && s->dev_planned && s->dev == NULL && s->dev_why == NULL)
		dev_scan_setup(s);
	if (s->dev != NULL && !dev_scan_owner(s))
		return false;
	if (s->scan == NULL || (s->bitmap && !s->bitmap_ready))
		begin_scan(s);
	/*
	 * W32 (§3.29): the owner of a parallel plan's device read, which claims
	 * every unit, its source told so once its scan has begun -- a part's
	 * adjacent units then handed at once, where each of the parallel scan's,
	 * twenty granules of ClickBench's, was a device unit of its own
	 */
	if (s->dev != NULL && s->dev_claim == 1 && !s->dev_alone)
	{
		s->dev_alone = true;
		if (s->node.css.ss.ps.plan->parallel_aware && s->dev_owner != NULL &&
			VEXEC_SOURCE_HAS(s->src, claim_alone) && s->srcstate != NULL)
			s->src->claim_alone(s->srcstate);
	}
	if (s->done)
		return false;
	if (s->dev != NULL)
		return dev_scan_fetch(s);
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
	return s->src != NULL && VEXEC_SOURCE_HAS(s->src, aggregate) && !s->need_tid &&
		(plan->qual == NIL ||
		 (vexec_enable_count_under_quals && list_length(plan->qual) == 1 &&
		  VEXEC_SOURCE_HAS(s->src, count_under_quals)));
}

/*
 * F35 and F37 (§3.38): a text Var's uses in an expression, as VexecLengthUses
 * says: length(col), or col = '' or col <> '' under a deterministic collation,
 * read no value's bytes where a source hands the column as character counts.
 */
static void
note_length_use(VexecLengthUses *w, const Var *v, uint8 use)
{
	if (v->varno == w->varno && v->varattno >= 1 && v->varattno <= w->n && v->varlevelsup == 0)
		w->uses[v->varattno - 1] = Max(w->uses[v->varattno - 1], use);
}

/* A column, under a relabeling that changes no byte (varchar's to text), or NULL */
static Var *
length_arg(Node *node)
{
	while (node != NULL && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node != NULL && IsA(node, Var) ? (Var *) node : NULL;
}

/* length(col), by any of its names that has a kernel (expr/k_text.c) */
bool
vexec_is_length_call(Node *node)
{
	FuncExpr   *f = (FuncExpr *) node;

	return node != NULL && IsA(node, FuncExpr) && list_length(f->args) == 1 &&
		(f->funcid == F_LENGTH_TEXT || f->funcid == F_CHAR_LENGTH_TEXT ||
		 f->funcid == F_CHARACTER_LENGTH_TEXT) &&
		length_arg(linitial(f->args)) != NULL;
}

bool
vexec_length_uses(Node *node, VexecLengthUses *w)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		note_length_use(w, (Var *) node, 2);
		return false;
	}
	if (vexec_is_length_call(node))
	{
		note_length_use(w, length_arg(linitial(((FuncExpr *) node)->args)), 1);
		return false;
	}
	if (IsA(node, OpExpr) && list_length(((OpExpr *) node)->args) == 2)
	{
		OpExpr	   *op = (OpExpr *) node;
		Node	   *a = linitial(op->args);
		Node	   *b = lsecond(op->args);
		Var		   *v = length_arg(a) != NULL ? length_arg(a) : length_arg(b);
		Const	   *c = IsA(a, Const) ? (Const *) a : IsA(b, Const) ? (Const *) b : NULL;

		if (v != NULL && c != NULL && (op->opfuncid == F_TEXTEQ || op->opfuncid == F_TEXTNE) &&
			!c->constisnull && VARSIZE_ANY_EXHDR(DatumGetPointer(c->constvalue)) == 0 &&
			OidIsValid(op->inputcollid) && get_collation_isdeterministic(op->inputcollid))
		{
			note_length_use(w, v, 1);
			return false;
		}
	}
	return expression_tree_walker(node, vexec_length_uses, w);
}

/*
 * F37: a VecAgg over this scan says which of the scan's output columns it reads
 * only by length(), before the scan's first batch.  The scan keeps the array.
 */
void
vexec_scan_parent_lengths(VexecNode *node, const bool *outputs, int n)
{
	VexecScanState *s = (VexecScanState *) node;

	Assert(node->kind == VEXEC_NODE_SCAN);
	s->parent_lengths = outputs;
	s->nparent_lengths = n;
}

/*
 * F35 and F37: the input columns of a scan read only by length() and by
 * comparison with '' -- in its quals, in its target list's expressions, by a
 * VecAgg above -- asked of the source as such.  None where a row may yet come to
 * PostgreSQL's evaluator, which would read the value's bytes: a lazy qual or
 * target, a fallback step, an expression that may raise; nor for a column a
 * running bound or a join's keys read, or a scan's TIDs or device.
 */
static void
plan_lengths(VexecScanState *s)
{
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) node->css.ss.ps.plan;
	Plan	   *plan = &cscan->scan.plan;
	VexecLengthUses w;
	ListCell   *lc;

	if (s->lengths_planned)
		return;
	s->lengths_planned = true;
	if (!vexec_enable_length_columns || !vexec_enable_source_layouts || s->need_tid || s->bitmap ||
		s->dev_planned || node->needs_input_datums || vexec_expr_may_raise((Node *) plan->qual))
		return;
	w.varno = cscan->scan.scanrelid;
	w.n = RelationGetNumberOfAttributes(s->rel);
	w.uses = palloc0(Max(w.n, 1));
	(void) vexec_length_uses((Node *) plan->qual, &w);
	foreach(lc, plan->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (IsA(tle->expr, Var))
			note_length_use(&w, (Var *) tle->expr,
							s->parent_lengths != NULL && tle->resno <= s->nparent_lengths &&
							s->parent_lengths[tle->resno - 1] ? 1 : 2);
		else if (vexec_expr_may_raise((Node *) tle->expr))
		{
			/* a target's raising is PostgreSQL's evaluator's to report: length() alone stays */
			if (!vexec_is_length_call((Node *) tle->expr))
				return;
			(void) vexec_length_uses((Node *) tle->expr, &w);
		}
		else
			(void) vexec_length_uses((Node *) tle->expr, &w);
	}
	if (node->bound != NULL)
		w.uses[s->attrs[node->bound_col] - 1] = 2;
	if (node->jfilter != NULL && node->jfilter_col < s->nattrs)
		w.uses[s->attrs[node->jfilter_col] - 1] = 2;
	for (int col = 0; col < s->nattrs; col++)
		if (s->attrs[col] >= 1 && s->attrs[col] <= w.n && w.uses[s->attrs[col] - 1] == 1)
		{
			if (s->length_cols == NULL)
				s->length_cols = MemoryContextAllocZero(node->mcxt, s->nattrs);
			s->length_cols[col] = 1;
			s->nlength_cols++;
		}
	pfree(w.uses);
}

/*
 * F36 (§3.38): a VecAgg above has only count(*) to ask the source under the
 * scan's qual: the scan is begun saying so, before its first unit
 */
void
vexec_scan_count_under_quals(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;

	Assert(node->kind == VEXEC_NODE_SCAN);
	s->count_quals = true;
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
	/* F36: under the scan's qual, where the source took it so as it began */
	if (ps->plan->qual != NIL && !s->src->count_under_quals(s->srcstate))
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

/*
 * V8e: whether a device strategy above may have the scan's source hand its
 * units, for the GPU's executor to read itself (vexec_source.h's
 * next_unit() and read_unit(), minor 7): the scan reads no TID and is given
 * no key, and checks no qual -- or, where the strategy filters the units'
 * rows by the scan's quals itself (W4, `filters`), its quals are those
 * vexec_scan_quals() gives -- so that the device reads every row of a unit
 * as the scan would hand it up.  Else why not.
 */
bool
vexec_scan_can_units(VexecNode *node, bool filters, const char **why)
{
	VexecScanState *s = (VexecScanState *) node;
	Plan	   *plan = node->css.ss.ps.plan;

	Assert(node->kind == VEXEC_NODE_SCAN);
	*why = NULL;
	if (s->bitmap || s->src == NULL || !VEXEC_SOURCE_HAS(s->src, next_unit) ||
		!VEXEC_SOURCE_HAS(s->src, read_unit))
		*why = "its scan's source hands no units";
	else if (plan->qual != NIL && !filters)
		*why = "its scan's quals";
	else if (s->need_tid)
		*why = "its scan's TIDs";
	else if (s->dev_planned)
		*why = "its scan reads its units on the device itself (W13)";
	return *why == NULL;
}

/*
 * W4: a device strategy above filters the units by the scan's quals: the
 * scan's columns in one step from its start, as next_unit() asks (VC2's
 * steps are for the quals the scan itself checks).  Before the scan begins.
 */
void
vexec_scan_units_filtered(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;

	Assert(node->kind == VEXEC_NODE_SCAN && s->scan == NULL);
	s->one_step = true;
}

/* W4: the scan's quals, an implicit AND, of Vars of *varno: its relation's */
List *
vexec_scan_quals(VexecNode *node, Index *varno)
{
	CustomScan *cscan = (CustomScan *) node->css.ss.ps.plan;

	Assert(node->kind == VEXEC_NODE_SCAN);
	*varno = cscan->scan.scanrelid;
	return cscan->scan.plan.qual;
}

/* W4: the units' column of the relation's attribute attnum -- the spec's -- or -1. */
int
vexec_scan_attr_unit_column(VexecNode *node, AttrNumber attnum)
{
	VexecScanState *s = (VexecScanState *) node;

	Assert(node->kind == VEXEC_NODE_SCAN);
	for (int j = 0; j < s->nattrs; j++)
		if (s->attrs[j] == attnum)
			return j;
	return -1;
}

/*
 * V8e: the units' column of the scan's output column `col`, from 0 -- the
 * spec's, whose names a unit gives -- where the column is the relation's
 * attribute as it is; else -1.  The scan of units has no steps, so the
 * spec's columns are the input's, in order.
 */
int
vexec_scan_unit_column(VexecNode *node, int col)
{
	VexecScanState *s = (VexecScanState *) node;
	CustomScan *cscan = (CustomScan *) node->css.ss.ps.plan;
	TargetEntry *tle;
	Var		   *v;

	Assert(node->kind == VEXEC_NODE_SCAN);
	if (col < 0 || col >= list_length(cscan->scan.plan.targetlist))
		return -1;
	tle = list_nth_node(TargetEntry, cscan->scan.plan.targetlist, col);
	if (!IsA(tle->expr, Var))
		return -1;
	v = (Var *) tle->expr;
	if (v->varno != cscan->scan.scanrelid || v->varattno <= 0 || v->varlevelsup != 0)
		return -1;
	for (int j = 0; j < s->nattrs; j++)
		if (s->attrs[j] == v->varattno)
			return j;
	return -1;
}

/*
 * V8e: the source's next unit for the device, between batches: true with
 * *unit filled -- a device's format, the scan having passed it over and
 * counted its rows as handed up, or VEXEC_UNIT_CPU, the scan's next batches
 * then the unit's, which end at its end -- false at the scan's end.
 */
bool
vexec_scan_next_unit(VexecNode *node, VexecSourceUnit *unit)
{
	VexecScanState *s = (VexecScanState *) node;
	PlanState  *ps = &node->css.ss.ps;
	bool		ok;

	Assert(node->kind == VEXEC_NODE_SCAN);
	if (ps->chgParam != NULL)
		ExecReScan(ps);
	if (s->scan == NULL)
		begin_scan(s);
	if (s->done)
		return false;
	CHECK_FOR_INTERRUPTS();
	if (ps->instrument)
		InstrStartNode(ps->instrument);
	memset(unit, 0, sizeof(VexecSourceUnit));
	unit->size = sizeof(VexecSourceUnit);
	ok = s->src->next_unit(s->srcstate, unit);
	if (!ok)
		s->done = true;
	else if (unit->format == VEXEC_UNIT_CPU)
	{
		/* its batches next, through the node as ever */
		s->one_unit = true;
		node->finished = false;
		s->cpu_units++;
	}
	else
	{
		s->device_units++;
		s->device_rows += (int64) unit->rows;
	}
	if (ps->instrument)
		InstrStopNode(ps->instrument,
					  ok && unit->format != VEXEC_UNIT_CPU ? (double) unit->rows : 0);
	return ok;
}

/*
 * V8e: a unit the device was given, read again by the scan's next batches,
 * which end at its end: for an executor that lost it, or refused it.
 */
void
vexec_scan_read_unit(VexecNode *node, const VexecSourceUnit *unit)
{
	VexecScanState *s = (VexecScanState *) node;

	Assert(node->kind == VEXEC_NODE_SCAN && s->srcstate != NULL);
	s->src->read_unit(s->srcstate, unit);
	s->one_unit = true;
	s->done = false;
	node->finished = false;
	s->cpu_units++;
}

/*
 * W6: bytes of a stream of a MERGETREE unit the scan's source handed (minor
 * 8, read_unit_bytes()), into the device's memory, between batches.
 */
void
vexec_scan_read_unit_bytes(VexecNode *node, const VexecSourceUnit *unit, int stream, uint64 offset,
						   uint64 length, void *dest)
{
	VexecScanState *s = (VexecScanState *) node;

	Assert(node->kind == VEXEC_NODE_SCAN && s->srcstate != NULL);
	if (!VEXEC_SOURCE_HAS(s->src, read_unit_bytes))
		elog(ERROR, "vexec: a MergeTree unit of a source that reads none of its bytes");
	s->src->read_unit_bytes(s->srcstate, unit, stream, offset, length, dest);
}

/*
 * V8e: the scan's batches again as its source hands them, its units no
 * longer claimed one at a time: for a device strategy given up.
 */
void
vexec_scan_resume(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;

	Assert(node->kind == VEXEC_NODE_SCAN);
	s->one_unit = false;
	if (!s->done)
		node->finished = false;
}

/*
 * VC4: the order the scan's rows come in, its source's (vexec_source.h's
 * ordering(), minor 4): the scan begun now, asking for it, where it has not
 * begun; the attributes, up to max, into attnums, and how many -- 0 where
 * the source has no order to give, the scan is parallel, or it began
 * without asking.
 */
int
vexec_scan_ordering(VexecNode *node, int max, AttrNumber *attnums)
{
	VexecScanState *s = (VexecScanState *) node;
	PlanState  *ps = &node->css.ss.ps;

	Assert(node->kind == VEXEC_NODE_SCAN);
	if (s->bitmap || s->src == NULL || !VEXEC_SOURCE_HAS(s->src, ordering) || s->pscan != NULL ||
		ps->plan->parallel_aware)
		return 0;
	if (s->scan == NULL)
	{
		if (ps->chgParam != NULL)
			ExecReScan(ps);
		s->want_order = true;
		begin_scan(s);
	}
	if (!s->want_order)
		return 0;
	return s->src->ordering(s->rel, s->srcstate, max, attnums);
}

/*
 * W10: the scan's output column col, from 0, as its target list computes it
 * over the relation's columns, or NULL; and the spec's columns, the units'.
 */
Expr *
vexec_scan_output_expr(VexecNode *node, int col)
{
	CustomScan *cscan = (CustomScan *) node->css.ss.ps.plan;

	Assert(node->kind == VEXEC_NODE_SCAN);
	if (col < 0 || col >= list_length(cscan->scan.plan.targetlist))
		return NULL;
	return list_nth_node(TargetEntry, cscan->scan.plan.targetlist, col)->expr;
}

int
vexec_scan_unit_ncols(VexecNode *node)
{
	Assert(node->kind == VEXEC_NODE_SCAN);
	return ((VexecScanState *) node)->nattrs;
}

/* W7: the units' relation, and the attribute a units' column is (from 0, the spec's). */
Oid
vexec_scan_relid(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;

	Assert(node->kind == VEXEC_NODE_SCAN);
	return s->rel != NULL ? RelationGetRelid(s->rel) : InvalidOid;
}

AttrNumber
vexec_scan_unit_attnum(VexecNode *node, int uc)
{
	VexecScanState *s = (VexecScanState *) node;

	Assert(node->kind == VEXEC_NODE_SCAN);
	return uc >= 0 && uc < s->nattrs ? s->attrs[uc] : InvalidAttrNumber;
}

/* The scan's source's name, for EXPLAIN. */
const char *
vexec_scan_source_name(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;

	return s->src != NULL && s->src->name != NULL ? s->src->name : "a registered source";
}

/* ---------------------------------------------------------------------
 * W13 (§3.27): the device's read of the scan's own units
 * ---------------------------------------------------------------------
 *
 * The scan's source hands its units (next_unit(), minor 7), and the device
 * reads them (VEXEC_DEV_OP_SCAN, ops_scan.cpp): each needed attribute a
 * column of the program, the quals the units' filter (W4) -- every one of
 * them, or the device is not taken -- and, for a scan of TIDs, each row's
 * ordinal among the task's units' rows, made by the executor
 * (VEXEC_DEV_UCOL_ORDINAL) -- or, where the units name a column of their
 * rows' ids in their files (a table of its own files', minor 9), that
 * column, read as any other (VEXEC_DEV_UCOL_ROWID): ClickBench's Q24 on
 * Parquet spent 50 ms of a fresh backend's in the CPU's reading the ids
 * at the ordinals' places.  A bounded VecSort's rank lent in the plan
 * (plan/sort.c) cuts the rows there to the bound's first and those tying
 * with the last of them.  A unit the source hands the CPU, and one the
 * device refuses or has no room for, is read here, its batches handed up
 * as ever.  The device's rows come up once every unit is claimed: its
 * answer read whole -- so that a device lost before its end leaves every
 * unit it took to be read here, none of its rows handed up yet -- then a
 * batch at a time, each column made the scan's from the device's value,
 * and, for a scan of TIDs, each row's TID from its ordinal by the source's
 * unit_tids() (minor 9), a unit's rows at once, or from its id by
 * row_tids().  The node's quals check them
 * again, as every row it hands up: the filter kept them as the quals would.
 * In a parallel plan, a bounded VecSort's device read under a Gather Merge,
 * the first participant to begin reads every unit on the device and the
 * others none (dev_scan_owner()), so that the sort's participants fetch its
 * rows' late columns together (M4): ClickBench's Q23, ten rows of 103 late
 * columns, fetched them in 104 ms in the one process of a serial plan.
 */

#define SD_PAGES		4
#define SD_MAX_ROWS		((uint64) 1 << 20)	/* an answer's rows unranked, at most */

/*
 * W49 (pg_vector_executor.md §3.36): a request's units' rows, about -- 32
 * chunks' rows, as VecAgg's (device_add_unit()), W38's share of the
 * device's memory bounding their bytes.  At 2^21 rows ClickBench's Q23 at
 * 100M rows read URL and EventTime in 46 requests, 2.29 s of decoding for
 * 3 GB, where 7 requests decoded them in 0.88.
 */
static inline uint64
sd_request_rows(void)
{
	return (uint64) Max(vexec_gpu_chunk_rows / VEXEC_BATCH_ROWS, 1) * VEXEC_BATCH_ROWS * 32;
}

typedef enum ScanDevPhase
{
	SD_CLAIM,					/* the source's next unit claimed */
	SD_CPU,						/* a unit's batches here */
	SD_ANSWER,					/* the device's rows up */
	SD_DONE
} ScanDevPhase;

/* A unit the device was given, and its first row's ordinal among the task's */
typedef struct ScanDevUnit
{
	VexecDevUnitDesc *x;
	uint64		first;
} ScanDevUnit;

/* A page of the device's answer, copied: its columns' buffers, and its rows' TIDs */
typedef struct ScanDevPage
{
	uint32		nrows;
	const char **values;
	const uint64 **validity;
	const int32 **offsets;
	ItemPointerData *tids;
} ScanDevPage;

typedef struct ScanDevice
{
	VexecDevTask *t;
	const char *device;
	ScanDevPhase phase;
	bool		lost;
	const char *why;			/* the device given up: why */
	bool		claimed;		/* the source's units all claimed */
	bool		answered;
	/* the program: the scan's attributes, then the ordinal */
	int			ncols;
	VexecDevColType *types;
	int		   *read_cols;		/* each column's spec column, -1 the ordinal's */
	uint8	   *flags;
	int16	   *attnums;
	int			ordinal;		/* the ordinal's column, -1 for none */
	int			id_mode;		/* its rows' TIDs, as the first unit has them:
								 * 0 not yet, 1 by their ordinals
								 * (unit_tids()), 2 by their ids, a column of
								 * the units' (row_tids()) */
	char	   *filter;
	Size		filter_len;
	char	   *program;
	Size		proglen;
	uint32		pages[SD_PAGES];
	int			npages;
	/* the units: gathered, in flight, taken by the device, to be read here */
	List	   *ufill;			/* ScanDevUnit */
	uint64		ufill_rows;
	uint64		ufill_bytes;	/* W38: their columns' bytes, about, */
	uint64		request_bytes;	/* and what a request's are gathered within */
	double		row_bytes;		/* a row's read columns' widths */
	List	   *uflight;
	List	   *utaken;			/* in their ordinals' order */
	List	   *uredo;			/* VexecDevUnitDesc */
	VexecDevUnitDesc *reading;	/* the one read here now, freed at its end */
	uint64		next_first;
	/* the answer, whole */
	MemoryContext stagecxt;
	List	   *staged;			/* ScanDevPage */
	ListCell   *spage;
	uint32		srow;
	char	  **textbuf;		/* per column: its text value of the row made a varlena */
	Size	   *textlen;
	/* figures */
	int64		rows_kept;
	int64		rows_out;
	int64		units_redone;
	int64		nomem;
	int64		split;			/* W39: requests halved, the device's memory
								 * short of them */
	VexecDevFigures fig;
} ScanDevice;

/* An attribute's column of the program -- its place among the scan's -- or -1 */
static int
dev_scan_filter_column(void *arg, AttrNumber attnum, Oid typid, Oid collid)
{
	VexecScanState *s = arg;

	(void) typid;
	(void) collid;
	for (int j = 0; j < s->nattrs; j++)
		if (s->attrs[j] == attnum)
			return j;
	return -1;
}

/*
 * The device's read set up, before the scan begins, where it may be had:
 * vexec.gpu and the provider, a source that hands units -- and their rows'
 * TIDs, for a scan of TIDs -- every attribute of a type the device holds,
 * the quals of a form its filter takes; else the scan is the CPU's, and
 * dev_why says why.
 */
static void
dev_scan_setup(VexecScanState *s)
{
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) node->css.ss.ps.plan;
	ScanDevice *d;
	const char *why = NULL;
	MemoryContext old;

	if (!vexec_device_wanted(true, vexec_enable_gpu_scan, &why))
	{
		s->dev_why = why;
		return;
	}
	if (s->src == NULL || !VEXEC_SOURCE_HAS(s->src, next_unit) || !VEXEC_SOURCE_HAS(s->src, read_unit))
		s->dev_why = "its source hands no units";
	else if (s->need_tid && !VEXEC_SOURCE_HAS(s->src, unit_tids))
		s->dev_why = "its source gives no TIDs of its units' rows";
	else if (s->nattrs == 0)
		s->dev_why = "no column to read";
	if (s->dev_why != NULL)
		return;

	old = MemoryContextSwitchTo(node->mcxt);
	d = palloc0(sizeof(ScanDevice));
	d->ncols = s->nattrs + (s->need_tid ? 1 : 0);
	d->types = palloc0(sizeof(VexecDevColType) * d->ncols);
	d->read_cols = palloc(sizeof(int) * d->ncols);
	d->flags = palloc0(sizeof(uint8) * d->ncols);
	d->attnums = palloc0(sizeof(int16) * d->ncols);
	d->ordinal = s->need_tid ? s->nattrs : -1;
	for (int c = 0; c < s->nattrs; c++)
	{
		int16		scale;
		int			type = vexec_dev_type_of(node->in->types[c], &scale);

		if (type == VEXEC_DEV_NONE)
		{
			s->dev_why = psprintf("a column of type %s, which the device does not hold",
								  format_type_be(node->in->types[c]->typid));
			MemoryContextSwitchTo(old);
			return;
		}
		d->types[c].type = (uint8) type;
		d->types[c].scale = scale;
		d->read_cols[c] = c;
		d->attnums[c] = s->attrs[c];
	}
	if (d->ordinal >= 0)
	{
		d->types[d->ordinal].type = VEXEC_DEV_INT64;
		d->read_cols[d->ordinal] = -1;
		d->flags[d->ordinal] = VEXEC_DEV_UCOL_ORDINAL;
	}
	/* W38: a row's bytes as the device decodes it, by its read columns' widths -- an ordinal's 8 */
	d->row_bytes = vexec_devunit_row_bytes(RelationGetRelid(s->rel), d->ncols, d->attnums) +
		(d->ordinal >= 0 ? sizeof(int64) : 0);
	if (cscan->scan.plan.qual != NIL)
	{
		d->filter = vexec_devfilter_build(cscan->scan.plan.qual, cscan->scan.scanrelid, dev_scan_filter_column,
										  s, &d->filter_len, &why);
		if (d->filter == NULL)
		{
			s->dev_why = psprintf("its quals: %s", why);
			MemoryContextSwitchTo(old);
			return;
		}
	}

	/* the program: the columns, and the rank, each key an attribute's column */
	d->proglen = sizeof(VexecDevProgram) + VEXEC_DEV_ALIGN(sizeof(VexecDevColType) * d->ncols) +
		vexec_dev_scan_size(s->dev_k > 0 ? s->dev_nrank : 0);
	d->program = palloc0(d->proglen);
	{
		VexecDevProgram *prog = (VexecDevProgram *) d->program;
		VexecDevScan *sc;

		prog->ncols = d->ncols;
		prog->memory = (uint64) (s->dev_k > 0 ? Min(s->dev_k * 4, (int64) SD_MAX_ROWS) : SD_MAX_ROWS) * 64 +
			(UINT64CONST(64) << 20);
		prog->admission_wait_ms = VEXEC_DEV_WAIT_DEFAULT;
		memcpy(vexec_dev_program_cols(prog), d->types, sizeof(VexecDevColType) * d->ncols);
		sc = (VexecDevScan *) vexec_dev_program_op(prog);
		if (s->dev_k > 0)
		{
			sc->k = (uint64) s->dev_k;
			sc->nrank = (uint16) s->dev_nrank;
			sc->rank_flags = VEXEC_DEV_RANK_TIES;
			for (int i = 0; i < s->dev_nrank; i++)
			{
				VexecDevRankKey *k = &vexec_dev_scan_keys(sc)[i];
				int			col = dev_scan_filter_column(s, s->dev_rank_attnos[i], InvalidOid, InvalidOid);

				if (col < 0)
				{
					s->dev_why = "a sort key the scan does not read";
					MemoryContextSwitchTo(old);
					return;
				}
				k->col = (int16) col;
				k->descending = s->dev_rank_desc[i] ? 1 : 0;
				k->nulls_first = s->dev_rank_nulls_first[i] ? 1 : 0;
			}
		}
	}
	d->stagecxt = AllocSetContextCreate(node->mcxt, "VecScan device answer", ALLOCSET_DEFAULT_SIZES);
	d->phase = SD_CLAIM;
	MemoryContextSwitchTo(old);
	s->dev = d;
	/* the columns in one step, as next_unit() asks */
	s->one_step = true;
}

/*
 * Whether this participant reads the units on the device in this run: a
 * serial scan's one always.  In a parallel plan -- a bounded VecSort's
 * device read under a Gather Merge, its late columns fetched by every
 * participant (M4; plan/sort.c) -- the first participant to begin claims
 * the read, and every unit then, as the source hands its units to whoever
 * asks first, the others none and so no row.  Where no DSM was made, a
 * Gather in no parallel mode, the one participant reads them.
 */
static bool
dev_scan_owner(VexecScanState *s)
{
	CustomScan *cscan = (CustomScan *) s->node.css.ss.ps.plan;

	if (s->dev_claim == 0)
	{
		uint32		none = 0;
		uint32		me = (IsParallelWorker() ? ParallelWorkerNumber + 1 : 0) + 1;

		s->dev_claim = !cscan->scan.plan.parallel_aware || s->dev_owner == NULL ||
			pg_atomic_compare_exchange_u32(s->dev_owner, &none, me) ? 1 : 2;
	}
	return s->dev_claim == 1;
}

/* The owner's figures for the leader, once its read is done: a parallel plan's alone */
static void
dev_scan_publish(VexecScanState *s)
{
	ScanDevice *d = s->dev;
	ScanDevShared *sh = s->dev_shared;

	if (sh == NULL || !s->node.css.ss.ps.plan->parallel_aware || s->dev_claim != 1)
		return;
	sh->lost = d->lost || d->why != NULL;
	sh->rows_kept = d->rows_kept;
	sh->rows_out = d->rows_out;
	sh->units_redone = d->units_redone;
	sh->split = d->split;
	sh->requests = d->fig.requests;
	sh->device_ms = d->fig.device_ms;
	sh->gpu_ms = d->fig.gpu_ms;
	sh->wait_ms = d->fig.wait_ms;
	strlcpy(sh->device, d->device ? d->device : "none", sizeof(sh->device));
	pg_write_barrier();
	pg_atomic_write_u32(&sh->set, 1);
}

/* The task's figures kept, and the task ended. */
static void
dev_scan_end_task(ScanDevice *d)
{
	if (d->t == NULL)
		return;
	d->fig.requests += d->t->fig.requests;
	d->fig.answers += d->t->fig.answers;
	d->fig.answer_rows += d->t->fig.answer_rows;
	d->fig.answer_bytes += d->t->fig.answer_bytes;
	d->fig.wait_ms += d->t->fig.wait_ms;
	d->fig.device_ms += d->t->fig.device_ms;
	d->fig.gpu_ms += d->t->fig.gpu_ms;
	vexec_devtask_end(d->t);
	d->t = NULL;
}

/* The task begun, with its answer pages; false where it is not. */
static bool
dev_scan_start(VexecScanState *s)
{
	ScanDevice *d = s->dev;
	const char *why;
	int			got;

	d->t = vexec_devtask_begin(VEXEC_DEV_OP_SCAN, d->program, d->proglen, s->node.mcxt, &why);
	if (d->t == NULL)
	{
		d->lost = true;
		d->why = why;
		return false;
	}
	if (d->device == NULL)
	{
		const char *name = vexec_device_ready() ? vexec_device_ready()->device_name() : NULL;

		d->device = MemoryContextStrdup(s->node.mcxt, name ? name : "the device");
	}
	got = vexec_devtask_claim_answers(d->t, SD_PAGES, d->pages);
	d->t->held += got;
	d->npages = got;
	if (got == 0)
	{
		dev_scan_end_task(d);
		d->lost = true;
		d->why = "no block of the arena for its answers";
		return false;
	}
	return true;
}

/* The device given up: every unit it had, to be read here; its answer, if any, dropped. */
static void
dev_scan_lose(VexecScanState *s, const char *why)
{
	ScanDevice *d = s->dev;
	List	   *lists[3] = {d->utaken, d->uflight, d->ufill};
	MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);
	ListCell   *lc;

	if (!d->lost)
	{
		d->lost = true;
		d->why = why;
	}
	for (int i = 0; i < 3; i++)
	{
		foreach(lc, lists[i])
		{
			ScanDevUnit *u = lfirst(lc);

			d->uredo = lappend(d->uredo, u->x);
			pfree(u);
		}
		list_free(lists[i]);
	}
	MemoryContextSwitchTo(old);
	d->utaken = d->uflight = d->ufill = NIL;
	d->ufill_rows = 0;
	d->ufill_bytes = 0;
	MemoryContextReset(d->stagecxt);
	d->staged = NIL;
	d->spage = NULL;
	if (d->t != NULL)
	{
		vexec_devtask_units_done(d->t);
		dev_scan_end_task(d);
	}
}

static void dev_scan_send(VexecScanState *s, List *units);

/* The request in flight answered: its units the device's, or, refused, to be read here. */
static void
dev_scan_take(VexecScanState *s)
{
	ScanDevice *d = s->dev;
	int			status;

	if (d->uflight == NIL)
		return;
	status = vexec_devtask_wait(d->t);
	vexec_devtask_units_done(d->t);
	if (status == VEXEC_DEV_OK)
	{
		const VexecDevAnswer *a = vexec_devtask_answer(d->t);

		d->rows_kept = (int64) a->value;
		/* W21: the granules none of whose rows the filter kept, into the condition cache */
		if ((a->flags & VEXEC_DEV_ANSF_GRANULES) && d->filter != NULL)
		{
			const uint64 *words = vexec_dev_answer_granules(unconstify(VexecDevAnswer *, a));
			ListCell   *lc;

			foreach(lc, d->uflight)
			{
				VexecDevUnitDesc *x = ((ScanDevUnit *) lfirst(lc))->x;

				if (x->granule_rows == NULL)
					continue;
				vexec_scan_units_none(&s->node, x, words);
				words += (x->nparts + 63) / 64;
			}
		}
		d->utaken = list_concat(d->utaken, d->uflight);
		list_free(d->uflight);
		d->uflight = NIL;
		/* an answer unranked of more rows than the CPU's scan is to stage: the scan's again */
		if (s->dev_k <= 0 && (uint64) d->rows_kept > SD_MAX_ROWS)
			dev_scan_lose(s, "more rows kept than the device hands back");
		return;
	}
	if (status == VEXEC_DEV_NOMEM && list_length(d->uflight) > 1)
	{
		/*
		 * W39 (pg_vector_executor.md §3.35): the device had no memory for the
		 * request, its rows as before it: its halves asked for, one after the
		 * other, down to a unit -- only a unit it cannot hold alone read here
		 */
		List	   *flight = d->uflight;
		int			n = list_length(flight);
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);
		List	   *first = list_copy_head(flight, n / 2);
		List	   *rest = list_copy_tail(flight, n / 2);

		MemoryContextSwitchTo(old);
		list_free(flight);
		d->uflight = NIL;
		d->split++;
		dev_scan_send(s, first);
		dev_scan_send(s, rest);
		dev_scan_take(s);
		return;
	}
	if (status == VEXEC_DEV_NOMEM || status == VEXEC_DEV_REFUSED)
	{
		/* the request not taken, the device's rows as before it: its units here */
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);
		ListCell   *lc;

		d->nomem++;
		foreach(lc, d->uflight)
		{
			d->uredo = lappend(d->uredo, ((ScanDevUnit *) lfirst(lc))->x);
			pfree(lfirst(lc));
		}
		list_free(d->uflight);
		d->uflight = NIL;
		if (d->why == NULL)
			d->why = pstrdup(d->t->message);
		MemoryContextSwitchTo(old);
		return;
	}
	dev_scan_lose(s, psprintf("the device's answer was \"%s\"%s%s", vexec_dev_status_name(status),
							  d->t->message[0] ? ": " : "", d->t->message));
}

/* W6: a MERGETREE unit's stream's bytes, from the scan's source, into the device's memory */
static void
dev_scan_unit_bytes(void *arg, const VexecDevUnitDesc *x, int stream, uint64 offset, uint64 length, void *dest)
{
	VexecScanState *s = arg;

	s->src->read_unit_bytes(s->srcstate, &x->unit, x->streams[stream].source, offset, length, dest);
}

/*
 * Units (ScanDevUnit) as one request -- in halves where it does not fit, a
 * unit that does not read here -- once the one before is answered.  The list
 * is the request's, or the CPU's.
 */
static void
dev_scan_send(VexecScanState *s, List *units)
{
	ScanDevice *d = s->dev;
	VexecDevUnitDesc **arr;
	int			n = list_length(units);
	ListCell   *lc;
	int			i = 0;

	if (n == 0)
		return;
	dev_scan_take(s);
	if (d->lost || d->t == NULL)
	{
		/* read here with the device's others, before those gathered since */
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		d->ufill = list_concat(units, d->ufill);
		MemoryContextSwitchTo(old);
		dev_scan_lose(s, d->why);
		return;
	}
	arr = palloc(sizeof(VexecDevUnitDesc *) * n);
	foreach(lc, units)
		arr[i++] = ((ScanDevUnit *) lfirst(lc))->x;
	vexec_devunits_probe(d->t, d->ncols, 0, d->flags, NULL, RelationGetRelid(s->rel), d->attnums, arr, n);
	if (!vexec_devtask_units(d->t, d->ncols, 0, d->flags, NULL, RelationGetRelid(s->rel), d->attnums, d->filter,
							 d->filter_len, NULL, 0, n, arr, ((ScanDevUnit *) linitial(units))->first,
							 dev_scan_unit_bytes, s, 0))
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		pfree(arr);
		if (n > 1)
		{
			List	   *rest = list_copy_tail(units, n / 2);

			units = list_truncate(units, n / 2);
			MemoryContextSwitchTo(old);
			dev_scan_send(s, units);
			dev_scan_send(s, rest);
		}
		else
		{
			d->uredo = lappend(d->uredo, ((ScanDevUnit *) linitial(units))->x);
			pfree(linitial(units));
			list_free(units);
			MemoryContextSwitchTo(old);
		}
		return;
	}
	pfree(arr);
	d->uflight = units;
}

/* The units gathered, as one request (dev_scan_send()). */
static void
dev_scan_submit(VexecScanState *s)
{
	ScanDevice *d = s->dev;
	List	   *units = d->ufill;

	d->ufill = NIL;
	d->ufill_rows = 0;
	d->ufill_bytes = 0;
	dev_scan_send(s, units);
}

/* A unit for the device, gathered into the next request. */
static void
dev_scan_add_unit(VexecScanState *s, const VexecSourceUnit *u)
{
	ScanDevice *d = s->dev;
	uint64		bytes = vexec_devunit_bytes(u, d->row_bytes);
	MemoryContext old;
	ScanDevUnit *x;

	/*
	 * W38 (pg_vector_executor.md §3.35): a request's units' columns within a
	 * share of the device's free memory as the request begins -- those
	 * gathered sent before a unit that would take them past it; a unit past
	 * it alone
	 */
	if (d->ufill != NIL && d->ufill_bytes + bytes > d->request_bytes)
		dev_scan_submit(s);
	if (d->ufill == NIL)
		d->request_bytes = vexec_device_request_bytes();
	old = MemoryContextSwitchTo(s->node.mcxt);
	x = palloc(sizeof(ScanDevUnit));

	x->x = vexec_devunit_copy(s->node.mcxt, u, d->ncols, d->read_cols, d->flags);
	vexec_scan_unit_granules(&s->node, x->x);	/* W34 */
	/* the ids' column of the unit's file, by its name there */
	if (d->id_mode == 2)
	{
		pfree(x->x->names[d->ordinal]);
		x->x->names[d->ordinal] = pstrdup(u->rowid_name);
	}
	x->first = d->next_first;
	d->next_first += u->rows;
	d->ufill = lappend(d->ufill, x);
	MemoryContextSwitchTo(old);
	d->ufill_rows += u->rows;
	d->ufill_bytes += bytes;
	if (d->ufill_rows >= sd_request_rows() || list_length(d->ufill) >= 256)
		dev_scan_submit(s);
}

/*
 * A scan of TIDs: the staged rows' TIDs from their ordinals -- the unit
 * each is of among those the device took, in their ordinals' order, and its
 * place there -- by the source, a unit's rows at once.
 */
typedef struct OrdinalRef
{
	uint64		ordinal;
	ItemPointerData *tid;
} OrdinalRef;

static int
cmp_ordinal(const void *a, const void *b)
{
	uint64		x = ((const OrdinalRef *) a)->ordinal;
	uint64		y = ((const OrdinalRef *) b)->ordinal;

	return x < y ? -1 : x > y ? 1 : 0;
}

static void
dev_scan_tids(VexecScanState *s)
{
	ScanDevice *d = s->dev;
	int64		n = 0;
	int64		k = 0;
	OrdinalRef *refs;
	uint64	   *rows;
	ItemPointerData *tids;
	ListCell   *lc;
	ListCell   *uc;

	foreach(lc, d->staged)
		n += ((ScanDevPage *) lfirst(lc))->nrows;
	if (n == 0)
		return;
	refs = palloc(sizeof(OrdinalRef) * n);
	foreach(lc, d->staged)
	{
		ScanDevPage *pg = lfirst(lc);
		const int64 *ord = (const int64 *) pg->values[d->ordinal];

		for (uint32 r = 0; r < pg->nrows; r++)
		{
			refs[k].ordinal = (uint64) ord[r];
			refs[k++].tid = &pg->tids[r];
		}
	}
	qsort(refs, n, sizeof(OrdinalRef), cmp_ordinal);
	rows = palloc(sizeof(uint64) * n);
	tids = palloc(sizeof(ItemPointerData) * n);
	k = 0;
	uc = list_head(d->utaken);
	while (k < n)
	{
		ScanDevUnit *u;
		int64		from = k;

		/* the unit of the next ordinal: the units are in their ordinals' order */
		while (uc != NULL && refs[k].ordinal >= ((ScanDevUnit *) lfirst(uc))->first +
			   ((ScanDevUnit *) lfirst(uc))->x->rows)
			uc = lnext(d->utaken, uc);
		if (uc == NULL || refs[k].ordinal < ((ScanDevUnit *) lfirst(uc))->first)
			elog(ERROR, "vexec: a row of the device's answer of no unit it took");
		u = lfirst(uc);
		while (k < n && refs[k].ordinal < u->first + u->x->rows)
		{
			rows[k - from] = refs[k].ordinal - u->first;
			k++;
		}
		s->src->unit_tids(s->srcstate, &u->x->unit, (int) (k - from), rows, tids);
		for (int64 i = from; i < k; i++)
			*refs[i].tid = tids[i - from];
	}
	pfree(refs);
	pfree(rows);
	pfree(tids);
}

/* The device's answer read whole, its pages copied, and its rows' TIDs made: false, the device lost. */
static bool
dev_scan_answer(VexecScanState *s)
{
	ScanDevice *d = s->dev;
	MemoryContext old;

	MemoryContextReset(d->stagecxt);
	d->staged = NIL;
	for (;;)
	{
		int			status = vexec_devtask_answer_page(d->t, d->npages, d->pages, 0);
		const VexecDevAnswer *a;
		ScanDevPage *pg;

		if (status != VEXEC_DEV_OK && status != VEXEC_DEV_MORE)
		{
			dev_scan_lose(s, psprintf("the device's answer was \"%s\"%s%s", vexec_dev_status_name(status),
									  d->t->message[0] ? ": " : "", d->t->message));
			return false;
		}
		a = vexec_devtask_answer(d->t);
		if (a->ncols != d->ncols)
			elog(ERROR, "vexec: a device's scan answer of %d columns, of %d", a->ncols, d->ncols);
		old = MemoryContextSwitchTo(d->stagecxt);
		pg = palloc0(sizeof(ScanDevPage));
		pg->nrows = a->nrows;
		pg->values = palloc0(sizeof(char *) * d->ncols);
		pg->validity = palloc0(sizeof(uint64 *) * d->ncols);
		pg->offsets = palloc0(sizeof(int32 *) * d->ncols);
		for (int i = 0; i < d->ncols && a->nrows > 0; i++)
		{
			const VexecDevAnsCol *c = &vexec_dev_answer_cols(unconstify(VexecDevAnswer *, a))[i];
			const char *v = vexec_devans_values(d->t, c);
			const uint64 *nv = vexec_devans_validity(d->t, c);
			const int32 *ov = vexec_devans_offsets(d->t, c);

			if (v != NULL)
				pg->values[i] = memcpy(palloc(c->values.length), v, c->values.length);
			else
				pg->values[i] = palloc0(1);
			if (nv != NULL)
				pg->validity[i] = memcpy(palloc(c->validity.length), nv, c->validity.length);
			if (ov != NULL)
				pg->offsets[i] = memcpy(palloc(c->offsets.length), ov, c->offsets.length);
		}
		if (d->ordinal >= 0)
			pg->tids = palloc0(sizeof(ItemPointerData) * Max(a->nrows, 1));
		if (a->nrows > 0)
			d->staged = lappend(d->staged, pg);
		MemoryContextSwitchTo(old);
		d->rows_out += a->nrows;
		if (status == VEXEC_DEV_OK)
			break;
	}
	if (d->ordinal >= 0 && d->id_mode == 2)
	{
		ListCell   *lc;

		foreach(lc, d->staged)
		{
			ScanDevPage *pg = lfirst(lc);

			s->src->row_tids(s->rel, (int) pg->nrows, (const uint64 *) pg->values[d->ordinal], pg->tids);
		}
	}
	else if (d->ordinal >= 0)
	{
		old = MemoryContextSwitchTo(d->stagecxt);
		dev_scan_tids(s);
		MemoryContextSwitchTo(old);
	}
	d->spage = list_head(d->staged);
	d->srow = 0;
	/* the units the answer covers are done with */
	{
		ListCell   *lc;

		foreach(lc, d->utaken)
		{
			vexec_devunit_free(((ScanDevUnit *) lfirst(lc))->x);
			pfree(lfirst(lc));
		}
		list_free(d->utaken);
		d->utaken = NIL;
	}
	dev_scan_end_task(d);
	return true;
}

/*
 * A device's value of the program's column c of a staged page's row r, as the
 * scan's column's Datum -- a text's in the column's own buffer, valid until
 * the column's value of the next row: a row's every column is made before
 * vexec_batch_add_values() copies them, and with one buffer a row's texts
 * were all its last text column's
 */
static Datum
dev_scan_datum(VexecScanState *s, const ScanDevPage *pg, int c, uint32 r)
{
	ScanDevice *d = s->dev;
	int			type = d->types[c].type;
	const char *p;

	if (type == VEXEC_DEV_STRING)
	{
		int32		off = pg->offsets[c][r];
		int32		len = pg->offsets[c][r + 1] - off;
		Size		need = VARHDRSZ + (Size) len;

		if (d->textbuf == NULL)
		{
			d->textbuf = MemoryContextAllocZero(s->node.mcxt, sizeof(char *) * d->ncols);
			d->textlen = MemoryContextAllocZero(s->node.mcxt, sizeof(Size) * d->ncols);
		}
		if (d->textlen[c] < need)
		{
			if (d->textbuf[c] != NULL)
				pfree(d->textbuf[c]);
			d->textlen[c] = Max(need, 1024);
			d->textbuf[c] = MemoryContextAlloc(s->node.mcxt, d->textlen[c]);
		}
		SET_VARSIZE(d->textbuf[c], need);
		memcpy(VARDATA(d->textbuf[c]), pg->values[c] + off, len);
		return PointerGetDatum(d->textbuf[c]);
	}
	p = pg->values[c] + (Size) r * vexec_dev_type_width(type);
	switch (type)
	{
		case VEXEC_DEV_BOOL8:
			return BoolGetDatum(*p != 0);
		case VEXEC_DEV_INT8:
			return CharGetDatum(*p);
		case VEXEC_DEV_INT16:
			{
				int16		v;

				memcpy(&v, p, sizeof(v));
				return Int16GetDatum(v);
			}
		case VEXEC_DEV_INT32:
			{
				int32		v;

				memcpy(&v, p, sizeof(v));
				return Int32GetDatum(v);
			}
		case VEXEC_DEV_UINT32:
			{
				uint32		v;

				memcpy(&v, p, sizeof(v));
				return ObjectIdGetDatum(v);
			}
		case VEXEC_DEV_INT64:
			{
				int64		v;

				memcpy(&v, p, sizeof(v));
				return Int64GetDatum(v);
			}
		case VEXEC_DEV_FLOAT32:
			{
				float4		v;

				memcpy(&v, p, sizeof(v));
				return Float4GetDatum(v);
			}
		case VEXEC_DEV_FLOAT64:
			{
				float8		v;

				memcpy(&v, p, sizeof(v));
				return Float8GetDatum(v);
			}
		case VEXEC_DEV_DECIMAL64:
			{
				int64		v;

				memcpy(&v, p, sizeof(v));
				return vexec_scaled_to_numeric(s->node.in, (int128) v, d->types[c].scale);
			}
		case VEXEC_DEV_DECIMAL128:
			{
				int128		v;

				memcpy(&v, p, sizeof(v));
				return vexec_scaled_to_numeric(s->node.in, v, d->types[c].scale);
			}
	}
	elog(ERROR, "vexec: a device's column of type %d in a scan's answer", type);
	return (Datum) 0;
}

/* The next batch of the device's staged rows: false where they are out. */
static bool
dev_scan_emit(VexecScanState *s)
{
	ScanDevice *d = s->dev;
	VexecBatch *in = s->node.in;
	Datum	   *values;
	bool	   *isnull;

	while (d->spage != NULL && d->srow >= ((ScanDevPage *) lfirst(d->spage))->nrows)
	{
		d->spage = lnext(d->staged, d->spage);
		d->srow = 0;
	}
	if (d->spage == NULL)
		return false;
	values = palloc(sizeof(Datum) * Max(s->node.ninput, 1));
	isnull = palloc(sizeof(bool) * Max(s->node.ninput, 1));
	vexec_batch_reset(in);
	vexec_batch_begin_rows(in);
	while (d->spage != NULL && in->nrows < in->room)
	{
		ScanDevPage *pg = lfirst(d->spage);
		uint32		r = d->srow;

		if (r >= pg->nrows)
		{
			d->spage = lnext(d->staged, d->spage);
			d->srow = 0;
			continue;
		}
		for (int c = 0; c < s->nattrs; c++)
		{
			isnull[c] = pg->validity[c] != NULL && !vexec_bit(pg->validity[c], r);
			values[c] = isnull[c] ? (Datum) 0 : dev_scan_datum(s, pg, c, r);
		}
		if (s->need_tid)
		{
			values[s->nattrs] = PointerGetDatum(&pg->tids[r]);
			isnull[s->nattrs] = false;
		}
		vexec_batch_add_values(in, values, isnull);
		d->srow++;
	}
	pfree(values);
	pfree(isnull);
	if (in->nrows == 0)
		return false;
	vexec_batch_apply_config(in, &s->node.layout);
	return true;
}

/* The source's next unit claimed, as vexec_scan_next_unit() claims it for a node above */
static bool
dev_scan_next_unit(VexecScanState *s, VexecSourceUnit *unit)
{
	memset(unit, 0, sizeof(VexecSourceUnit));
	unit->size = sizeof(VexecSourceUnit);
	CHECK_FOR_INTERRUPTS();
	if (!s->src->next_unit(s->srcstate, unit))
		return false;
	if (unit->format == VEXEC_UNIT_CPU)
	{
		s->one_unit = true;
		s->cpu_units++;
	}
	else
	{
		s->device_units++;
		s->device_rows += (int64) unit->rows;
	}
	return true;
}

static bool
dev_scan_fetch(VexecScanState *s)
{
	ScanDevice *d = s->dev;
	VexecSourceUnit unit;

	for (;;)
	{
		switch (d->phase)
		{
			case SD_CPU:
				/* a unit's batches here: one the source handed the CPU, or one read again */
				if (fetch_source(s))
					return true;
				if (d->reading != NULL)
					vexec_devunit_free(d->reading);
				d->reading = NULL;
				d->phase = SD_CLAIM;
				break;
			case SD_CLAIM:
				if (d->uredo != NIL)
				{
					d->reading = linitial(d->uredo);
					d->uredo = list_delete_first(d->uredo);
					s->src->read_unit(s->srcstate, &d->reading->unit);
					s->one_unit = true;
					s->cpu_units++;
					d->units_redone++;
					d->phase = SD_CPU;
					break;
				}
				if (d->claimed)
				{
					d->phase = SD_ANSWER;
					break;
				}
				if (!dev_scan_next_unit(s, &unit))
				{
					d->claimed = true;
					if (!d->lost && d->t != NULL)
					{
						dev_scan_submit(s);
						dev_scan_take(s);
					}
					break;
				}
				if (unit.format == VEXEC_UNIT_CPU)
				{
					d->phase = SD_CPU;
					break;
				}
				/*
				 * a scan of TIDs: its rows' ids as its first unit names them, a
				 * column of its file's, or their ordinals; a unit of the other
				 * kind is read here
				 */
				if (d->ordinal >= 0)
				{
					int			mode = unit.rowid_name != NULL && VEXEC_SOURCE_HAS(s->src, row_tids) ? 2 : 1;

					if (d->id_mode == 0)
					{
						d->id_mode = mode;
						d->flags[d->ordinal] = mode == 2 ? VEXEC_DEV_UCOL_ROWID : VEXEC_DEV_UCOL_ORDINAL;
					}
					else if (d->id_mode != mode)
					{
						s->device_units--;
						s->device_rows -= (int64) unit.rows;
						s->src->read_unit(s->srcstate, &unit);
						s->one_unit = true;
						s->cpu_units++;
						d->units_redone++;
						d->phase = SD_CPU;
						break;
					}
				}
				if (!d->lost && d->t == NULL)
					(void) dev_scan_start(s);
				if (d->lost)
				{
					/* a unit the device does not take now: read here */
					s->device_units--;
					s->device_rows -= (int64) unit.rows;
					s->src->read_unit(s->srcstate, &unit);
					s->one_unit = true;
					s->cpu_units++;
					d->units_redone++;
					d->phase = SD_CPU;
					break;
				}
				dev_scan_add_unit(s, &unit);
				break;
			case SD_ANSWER:
				if (!d->answered && !d->lost && d->t != NULL)
				{
					d->answered = true;
					if (!dev_scan_answer(s))
					{
						/* the device lost: its units here, then nothing of its own */
						d->phase = SD_CLAIM;
						break;
					}
				}
				d->answered = true;
				if (dev_scan_emit(s))
					return true;
				d->phase = SD_DONE;
				break;
			case SD_DONE:
				if (!s->done)
					dev_scan_publish(s);
				s->done = true;
				return false;
		}
	}
}

/* The device's state let go -- a rescan reads the units again from the start -- and its figures kept */
static void
dev_scan_reset(VexecScanState *s)
{
	ScanDevice *d = s->dev;
	List	   *lists[3];
	ListCell   *lc;

	if (d == NULL)
		return;
	if (d->t != NULL)
		vexec_devtask_units_done(d->t);
	dev_scan_end_task(d);
	lists[0] = d->utaken;
	lists[1] = d->uflight;
	lists[2] = d->ufill;
	for (int i = 0; i < 3; i++)
	{
		foreach(lc, lists[i])
		{
			vexec_devunit_free(((ScanDevUnit *) lfirst(lc))->x);
			pfree(lfirst(lc));
		}
		list_free(lists[i]);
	}
	foreach(lc, d->uredo)
		vexec_devunit_free(lfirst(lc));
	list_free(d->uredo);
	if (d->reading != NULL)
		vexec_devunit_free(d->reading);
	d->reading = NULL;
	d->utaken = d->uflight = d->ufill = d->uredo = NIL;
	d->ufill_rows = 0;
	d->ufill_bytes = 0;
	d->next_first = 0;
	MemoryContextReset(d->stagecxt);
	d->staged = NIL;
	d->spage = NULL;
	d->claimed = false;
	d->answered = false;
	d->lost = false;
	d->phase = SD_CLAIM;
}

static void
dev_scan_explain(VexecScanState *s, ExplainState *es)
{
	ScanDevice *d = s->dev;
	const char *one = s->node.css.ss.ps.plan->parallel_aware ? " in one participant" : "";

	if (es->analyze && s->dev_copied)
	{
		/* a parallel plan's read, which a worker made */
		const ScanDevShared *c = &s->dev_copy;

		ExplainPropertyText("Device", c->device, es);
		ExplainPropertyText("Device Participant", "a worker", es);
		if (c->lost)
			ExplainPropertyText("Device Gave Up", "lost", es);
		if (s->dev_k > 0)
			ExplainPropertyText("Device Rank", psprintf("%d key%s, bound " INT64_FORMAT, s->dev_nrank,
														s->dev_nrank == 1 ? "" : "s", s->dev_k), es);
		ExplainPropertyInteger("Device Rows Kept", NULL, c->rows_kept, es);
		ExplainPropertyInteger("Device Rows Handed Up", NULL, c->rows_out, es);
		if (c->units_redone > 0 || es->format != EXPLAIN_FORMAT_TEXT)
			ExplainPropertyInteger("Device Units Read Here", NULL, c->units_redone, es);
		if (c->split > 0)
			ExplainPropertyInteger("Device Requests Split", NULL, c->split, es);
		if (es->verbose)
		{
			ExplainPropertyInteger("Device Requests", NULL, c->requests, es);
			ExplainPropertyFloat("Device Time", "ms", c->device_ms, 3, es);
			ExplainPropertyFloat("Device Wait", "ms", c->wait_ms, 3, es);
		}
		return;
	}
	if (!es->analyze || !s->node.ran)
	{
		ExplainPropertyText("Device", s->dev_k > 0 ?
							psprintf("the GPU's read of its units%s, as the scan begins, ranked by %d key%s for a bound of " INT64_FORMAT,
									 one, s->dev_nrank, s->dev_nrank == 1 ? "" : "s", s->dev_k) :
							psprintf("the GPU's read of its units%s, as the scan begins", one), es);
		return;
	}
	if (s->dev_claim == 2)
	{
		ExplainPropertyText("Device", "the GPU's read of its units, in another participant", es);
		return;
	}
	if (d == NULL)
	{
		ExplainPropertyText("Device", psprintf("none: %s", s->dev_why ? s->dev_why : "not begun"), es);
		return;
	}
	ExplainPropertyText("Device", d->device ? d->device : "none", es);
	if (s->node.css.ss.ps.plan->parallel_aware)
		ExplainPropertyText("Device Participant", "the leader", es);
	if (d->lost || d->why != NULL)
		ExplainPropertyText("Device Gave Up", d->why ? d->why : "lost", es);
	if (s->dev_k > 0)
		ExplainPropertyText("Device Rank", psprintf("%d key%s, bound " INT64_FORMAT, s->dev_nrank,
													s->dev_nrank == 1 ? "" : "s", s->dev_k), es);
	ExplainPropertyInteger("Device Rows Kept", NULL, d->rows_kept, es);
	ExplainPropertyInteger("Device Rows Handed Up", NULL, d->rows_out, es);
	if (d->units_redone > 0 || es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyInteger("Device Units Read Here", NULL, d->units_redone, es);
	/* W39: requests the device had no memory for, halved and asked for again */
	if (d->split > 0)
		ExplainPropertyInteger("Device Requests Split", NULL, d->split, es);
	if (es->verbose)
	{
		ExplainPropertyInteger("Device Requests", NULL, d->fig.requests, es);
		ExplainPropertyFloat("Device Time", "ms", d->fig.device_ms, 3, es);
		ExplainPropertyFloat("Device Wait", "ms", d->fig.wait_ms, 3, es);
	}
}

/*
 * pg_otel_probe's span of a VecScan that ran (node.c; pg_vector_executor.md
 * §3.41): W13's device read of its own units, as dev_scan_explain() prints
 * it -- a worker's copied where the read was a worker's, nothing where
 * another participant read the units (OT1); the rows heap's page reader
 * prefiltered, the granules the condition cache passed over, the device's
 * units it left unsearched, the units answered from their statistics (OT3);
 * and its source's own figures, where it gives them (OT5).  Its fields
 * alone, and the source's, which neither allocates nor raises.
 */
void
vexec_scan_figures(VexecNode *node, VexecNodeFigures *f, bool on_error)
{
	VexecScanState *s = (VexecScanState *) node;

	(void) on_error;
	if (s->dev_planned)
	{
		if (s->dev_copied)
		{
			const ScanDevShared *c = &s->dev_copy;

			f->device = c->device[0] != '\0' ? c->device : "none";
			if (c->lost)
				f->device_why = "lost";
			f->device_requests = c->requests;
			f->device_ms = c->device_ms;
			f->device_gpu_ms = c->gpu_ms;
			f->device_wait_ms = c->wait_ms;
		}
		else if (s->dev_claim == 2)
		{
			/* the units another participant's, whose span says what it read */
		}
		else if (s->dev == NULL)
		{
			f->device = "none";
			f->device_why = s->dev_why != NULL ? s->dev_why : "not begun";
		}
		else
		{
			ScanDevice *d = s->dev;

			f->device = d->device != NULL ? d->device : "none";
			if (d->lost || d->why != NULL)
				f->device_why = d->why != NULL ? d->why : "lost";
			f->device_requests = d->fig.requests;
			f->device_bytes_in = d->fig.bytes;
			f->device_bytes_out = d->fig.answer_bytes;
			f->device_ms = d->fig.device_ms;
			f->device_gpu_ms = d->fig.gpu_ms;
			f->device_wait_ms = d->fig.wait_ms;
		}
	}
	if (s->heap != NULL && s->nchecks > 0)
		f->rows_prefiltered = vexec_heap_reader_prefiltered(s->heap);
	if (s->cc_on && (s->cc_named || s->cc_passed > 0 || s->cc_unsearched > 0))
	{
		f->granules_passed_over = s->cc_passed;
		if (s->cc_unsearched > 0)
			f->units_not_searched = s->cc_unsearched;
	}
	if (s->stats_units > 0)
		f->units_from_statistics = s->stats_units;
	if (s->src != NULL && s->srcstate != NULL && VEXEC_SOURCE_HAS(s->src, scan_figures))
	{
		int			n = s->src->scan_figures(s->srcstate, f->source);

		f->nsource = Max(0, Min(n, VEXEC_SOURCE_FIGURES_MAX));
	}
}

/*
 * W13: a VecScan's plan as the device's read of its units (plan/paths.c),
 * and a bounded VecSort's rank lent to it (plan/sort.c): its bound and its
 * keys, the scan's attributes, each descending or not, its NULLs first or
 * not.  custom_private holds them, as integers.
 */
void
vexec_scan_plan_device(CustomScan *cscan)
{
	cscan->custom_private = list_make1(makeInteger(1));
}

bool
vexec_scan_plan_is_device(CustomScan *cscan)
{
	return cscan->methods == vexec_scan_methods() && cscan->custom_private != NIL &&
		IsA(linitial(cscan->custom_private), Integer) && intVal(linitial(cscan->custom_private)) == 1;
}

void
vexec_scan_plan_rank(CustomScan *cscan, int64 bound, int n, const AttrNumber *attnos, const bool *desc,
					 const bool *nulls_first)
{
	List	   *p = list_make3(makeInteger(1), makeInteger((int) bound), makeInteger(n));

	Assert(vexec_scan_plan_is_device(cscan) && bound > 0 && bound <= INT_MAX);
	for (int i = 0; i < n; i++)
		p = lappend(lappend(lappend(p, makeInteger(attnos[i])), makeInteger(desc[i] ? 1 : 0)),
					makeInteger(nulls_first[i] ? 1 : 0));
	cscan->custom_private = p;
}

static TupleTableSlot *
scan_exec(CustomScanState *css)
{
	vexec_pool_kick();			/* F3 */
	return vexec_node_exec(&((VexecScanState *) css)->node);
}

static void
scan_rescan(CustomScanState *css)
{
	VexecScanState *s = (VexecScanState *) css;

	s->bound_given = 0;			/* a source forgets the running bound's keys */
	s->jkeys_given = 0;			/* and the join's (VC3) */
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
	s->one_unit = false;
	s->cc_granule = -1;			/* F7: no granule open */
	s->dev_claim = 0;
	s->dev_alone = false;
	s->dev_copied = false;
	dev_scan_reset(s);
	vexec_node_rescan(&s->node);
}

/*
 * W13 in a parallel plan: the owner's device figures copied from the DSM
 * before the Gather lets it go, where another participant was the owner --
 * or this one, the leader, did not run the plan
 */
static void
scan_shutdown(CustomScanState *css)
{
	VexecScanState *s = (VexecScanState *) css;

	if (s->dev_claim != 1 && s->dev_shared != NULL && pg_atomic_read_u32(&s->dev_shared->set) != 0)
	{
		pg_read_barrier();
		memcpy(&s->dev_copy, s->dev_shared, sizeof(ScanDevShared));
		s->dev_copied = true;
	}
}

static void
scan_end(CustomScanState *css)
{
	VexecScanState *s = (VexecScanState *) css;

	dev_scan_reset(s);

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
		if (s->nchecks > 0)
			ExplainPropertyText("Prefilter",
								psprintf("%d of %d quals", s->nchecked_quals,
										 list_length(css->ss.ps.plan->qual)), es);
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
	{
		int			maxpins;
		int64		copied;

		ExplainPropertyInteger("Heap Pages", NULL, vexec_heap_reader_pages(s->heap), es);
		if (s->nchecks > 0)
			ExplainPropertyInteger("Rows Prefiltered", NULL,
								   vexec_heap_reader_prefiltered(s->heap), es);
		vexec_heap_reader_pin_stats(s->heap, &maxpins, &copied);
		ExplainPropertyInteger("Heap Pins per Batch", NULL, maxpins, es);
		if (copied > 0 || es->format != EXPLAIN_FORMAT_TEXT)
			ExplainPropertyInteger("Heap Pages Copied", NULL, copied, es);
	}
	/* a VecHashJoin's inner keys (X6): text gives no zero, as for a filter */
	if (es->analyze && s->node.ran && s->node.jfilter != NULL)
	{
		if (s->node.jfilter->removed > 0 || es->format != EXPLAIN_FORMAT_TEXT)
			ExplainPropertyInteger("Rows Removed by Join Keys", NULL,
								   s->node.jfilter->removed, es);
		if (s->node.jfilter->off_batches > 0 || es->format != EXPLAIN_FORMAT_TEXT)
			ExplainPropertyInteger("Batches Join Keys Were Off", NULL,
								   s->node.jfilter->off_batches, es);
	}
	/*
	 * VC3: the join's keys given to the source, how many times, and the
	 * last's form
	 */
	if (es->analyze && s->node.ran && s->jkeys_on &&
		(s->jkeys_calls > 0 || es->format != EXPLAIN_FORMAT_TEXT))
	{
		ExplainPropertyInteger("Join Keys Given to Source", NULL, s->jkeys_calls, es);
		if (es->verbose && s->jkeys_form != NULL)
			ExplainPropertyText("Join Keys Given", s->jkeys_form, es);
	}
	/* a VecSort's running bound (H6): text gives no zero, as for a filter */
	if (es->analyze && s->node.ran && s->node.bound != NULL &&
		(s->node.bound->removed > 0 || es->format != EXPLAIN_FORMAT_TEXT))
		ExplainPropertyInteger("Rows Removed by Bound", NULL, s->node.bound->removed, es);
	/* F18: the rows the bound compared as the integers they hold, VERBOSE's */
	if (es->analyze && es->verbose && s->node.ran && s->node.bound != NULL &&
		(s->node.bound->typed > 0 || es->format != EXPLAIN_FORMAT_TEXT))
		ExplainPropertyInteger("Rows Bound as Integers", NULL, s->node.bound->typed, es);
	if (es->analyze && s->stats_units > 0)
	{
		ExplainPropertyInteger("Units From Statistics", NULL, s->stats_units, es);
		ExplainPropertyInteger("Rows From Statistics", NULL, s->stats_rows, es);
	}
	/*
	 * F7: the condition cache, where the scan took it and its source named
	 * its batches' granules -- this participant's granules passed over and
	 * found none true; in text, with VERBOSE alone
	 */
	/* W21: or a device above read its units, the scan itself never run */
	if (s->cc_on && es->analyze && (s->cc_named || s->cc_passed > 0 || s->cc_unsearched > 0) &&
		(es->verbose || es->format != EXPLAIN_FORMAT_TEXT))
	{
		ExplainPropertyText("Condition Cache", "on", es);
		ExplainPropertyInteger("Granules Passed Over", NULL, s->cc_passed, es);
		ExplainPropertyInteger("Granules Found None True", NULL, s->cc_kept, es);
		/* F19 */
		if (s->cc_bound_waited > 0)
			ExplainPropertyInteger("Batches of Quals Before the Bound", NULL, s->cc_bound_waited, es);
		/* W34: the device's units it did not search, the cache having seen their granules */
		if (s->cc_unsearched > 0)
			ExplainPropertyInteger("Units Not Searched", NULL, s->cc_unsearched, es);
	}
	/* W13: the device's read of the scan's own units */
	if (s->dev_planned)
		dev_scan_explain(s, es);
	/* V8e: the units the device read itself, and those read here for it */
	if (es->analyze && (s->device_units > 0 || s->cpu_units > 0))
	{
		ExplainPropertyInteger("Units Read by the Device", NULL, s->device_units, es);
		ExplainPropertyInteger("Rows Read by the Device", NULL, s->device_rows, es);
		ExplainPropertyInteger("Units Read Here", NULL, s->cpu_units, es);
	}
	/* VC14: only where a batch kept one, in every format, as node.c's counts */
	if (es->analyze && es->verbose && s->kept_cols > 0)
		ExplainPropertyInteger("Text Columns Kept as Handed", NULL, s->kept_cols, es);
	/* F35 and F37 */
	if (es->analyze && es->verbose && s->length_batches > 0)
		ExplainPropertyInteger("Text Columns Read for Their Lengths", NULL, s->nlength_cols, es);
	/* VC5: likewise, only where the source handed a dictionary */
	if (es->analyze && es->verbose && s->dict_batches > 0)
		ExplainPropertyInteger("Batches With Dictionaries", NULL, s->dict_batches, es);
	/*
	 * VC2: the columns of each step, where the source hands them in steps;
	 * and the batches each step was read for
	 */
	if (es->verbose && s->nsteps > 0)
	{
		List	   *steps = NIL;
		List	   *batches = NIL;

		for (int st = 0; st < s->nsteps; st++)
		{
			steps = lappend(steps, psprintf("%d", s->step_ends[st] - (st > 0 ? s->step_ends[st - 1] : 0)));
			batches = lappend(batches, psprintf(INT64_FORMAT, s->step_batches[st]));
		}
		ExplainPropertyList("Columns by Step", steps, es);
		if (es->analyze)
			ExplainPropertyList("Batches by Step", batches, es);
	}
}

/*
 * A parallel-aware VecScan's chunk of the DSM: the participants' shared
 * sort bound, then the access method's parallel scan descriptor, as a
 * SeqScan's (ExecSeqScanEstimate() and on).  The bound is a bounded
 * VecSort's above the scan in each participant (vecsort.c): the best of the
 * first keys each one's sort has kept its N rows within, encoded so that
 * less is better, PG_UINT64_MAX for none.
 */
typedef struct ScanShared
{
	pg_atomic_uint64 bound;		/* the participants' sort bound */
	Size		late_off;		/* M4: the late columns' exchange, from the
								 * chunk's start; 0 for none */
	pg_atomic_uint32 device_owner;	/* W13: the participant reading the units
									 * on the device, plus one; 0 none yet */
	ScanDevShared device_figures;	/* and its figures */
} ScanShared;

#define SCAN_SHARED_SIZE	MAXALIGN(sizeof(ScanShared))

/* M4: the exchange of a sort above publishing n rows a participant */
static Size
late_share_size(int nslots, int n)
{
	return add_size(add_size(MAXALIGN(sizeof(VexecLateShare)), MAXALIGN(sizeof(int32) * nslots)),
					mul_size(mul_size(nslots, n), sizeof(VexecLateEntry)));
}

static void
late_share_reset(VexecLateShare *ls)
{
	BarrierInit(&ls->barrier, 0);
	pg_atomic_write_u32(&ls->next, 0);
	memset(VEXEC_LATE_COUNTS(ls), 0, sizeof(int32) * ls->nslots);
}

static Size
scan_estimate_dsm(CustomScanState *css, ParallelContext *pcxt)
{
	VexecScanState *s = (VexecScanState *) css;
	Size		size;

	s->pscan_len = table_parallelscan_estimate(s->rel, css->ss.ps.state->es_snapshot);
	size = SCAN_SHARED_SIZE + MAXALIGN(s->pscan_len);
	if (s->late_slots > 0)
		size = add_size(size, late_share_size(pcxt->nworkers + 1, s->late_slots));
	return size;
}

static void
scan_initialize_dsm(CustomScanState *css, ParallelContext *pcxt, void *coordinate)
{
	VexecScanState *s = (VexecScanState *) css;
	ScanShared *ss = (ScanShared *) coordinate;

	vexec_pool_reserve(pcxt, &css->ss.ps);	/* F3: the pool's workers for the Gather's, where it may */

	s->shared_bound = &ss->bound;
	pg_atomic_init_u64(s->shared_bound, PG_UINT64_MAX);
	s->dev_owner = &ss->device_owner;
	pg_atomic_init_u32(s->dev_owner, 0);
	s->dev_shared = &ss->device_figures;
	pg_atomic_init_u32(&s->dev_shared->set, 0);
	s->pscan = (ParallelTableScanDesc) ((char *) coordinate + SCAN_SHARED_SIZE);
	table_parallelscan_initialize(s->rel, s->pscan, css->ss.ps.state->es_snapshot);
	ss->late_off = 0;
	s->late_share = NULL;
	if (s->late_slots > 0)
	{
		ss->late_off = SCAN_SHARED_SIZE + MAXALIGN(s->pscan_len);
		s->late_share = (VexecLateShare *) ((char *) coordinate + ss->late_off);
		s->late_share->nslots = pcxt->nworkers + 1;
		s->late_share->n = s->late_slots;
		pg_atomic_init_u32(&s->late_share->next, 0);
		late_share_reset(s->late_share);
	}
}

static void
scan_reinitialize_dsm(CustomScanState *css, ParallelContext *pcxt, void *coordinate)
{
	VexecScanState *s = (VexecScanState *) css;

	vexec_pool_reserve(pcxt, &css->ss.ps);	/* F3: the pool's workers for the Gather's, where it may */

	(void) pcxt;
	(void) coordinate;
	if (s->shared_bound != NULL)
		pg_atomic_write_u64(s->shared_bound, PG_UINT64_MAX);
	if (s->dev_owner != NULL)
		pg_atomic_write_u32(s->dev_owner, 0);
	if (s->dev_shared != NULL)
		pg_atomic_write_u32(&s->dev_shared->set, 0);
	if (s->pscan != NULL)
		table_parallelscan_reinitialize(s->rel, s->pscan);
	/* M4: the participants are new; the sort above detached from the barrier */
	if (s->late_share != NULL)
		late_share_reset(s->late_share);
}

static void
scan_initialize_worker(CustomScanState *css, shm_toc *toc, void *coordinate)
{
	VexecScanState *s = (VexecScanState *) css;
	ScanShared *ss = (ScanShared *) coordinate;

	(void) toc;
	s->shared_bound = &ss->bound;
	s->dev_owner = &ss->device_owner;
	s->dev_shared = &ss->device_figures;
	s->pscan = (ParallelTableScanDesc) ((char *) coordinate + SCAN_SHARED_SIZE);
	s->late_share = ss->late_off > 0 ? (VexecLateShare *) ((char *) coordinate + ss->late_off) : NULL;
}

/*
 * M4: a bounded VecSort above, of late columns, publishing n rows a
 * participant: asked as the plan begins, before the Gather sizes the DSM
 */
void
vexec_scan_reserve_late(PlanState *ps, int n)
{
	if (!IsA(ps, CustomScanState) || ((CustomScanState *) ps)->methods != &scan_exec_methods)
		return;
	((VexecScanState *) ps)->late_slots = n;
}

/* M4: the late columns' exchange, once the DSM is made; NULL: none */
VexecLateShare *
vexec_scan_late_share(PlanState *ps)
{
	if (!IsA(ps, CustomScanState) || ((CustomScanState *) ps)->methods != &scan_exec_methods)
		return NULL;
	return ((VexecScanState *) ps)->late_share;
}

/*
 * The participants' shared sort bound of a parallel-aware VecScan, once its
 * DSM is made -- the Gather makes it as it first runs -- else NULL.
 */
pg_atomic_uint64 *
vexec_scan_shared_bound(PlanState *ps)
{
	if (!IsA(ps, CustomScanState) || ((CustomScanState *) ps)->methods != &scan_exec_methods)
		return NULL;
	return ((VexecScanState *) ps)->shared_bound;
}
