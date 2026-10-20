/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * exec.h
 *	  vexec's executor nodes, and how they meet each other and row nodes
 *	  (pg_vector_executor.md §3.6, §3.8, §3.9).
 *
 * Every vector node is a CustomScan, and every one has two interfaces:
 *
 *	rows	ExecCustomScan returns a row a call, through the node's result
 *			slot, for a row parent;
 *	batches	vexec_next_batch() returns the node's next batch of output
 *			columns, for a vector parent, which recognises a vector child by
 *			its methods and calls it as ExecProcNode would: rescanning it on
 *			chgParam, keeping its EXPLAIN ANALYZE figures, checking for
 *			interrupts.
 *
 * Each node holds an input batch -- from its source, or its vector child --
 * evaluates its quals and target list over it (expr/), and then gives its
 * rows out in row order.  What it evaluates ahead of the rows being asked
 * for is pure; what may raise or has side effects -- lazy quals and
 * targets, and the rows a kernel marked -- PostgreSQL's evaluator runs a
 * row at a time, in row order, when the row is asked for (expr/expr.h).
 * So a LIMIT above a node stops it as it stops a row node: no error and no
 * side effect of a row the LIMIT never reads.
 *
 * The batch interface needs every qual and target of the node eager.  A
 * batch it hands up carries its redo rows, which the parent resolves through
 * vexec_resolve_row() when it reaches them, in row order.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_EXEC_H
#define VEXEC_EXEC_H

#include "access/tableam.h"
#include "port/atomics.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "utils/sortsupport.h"

#include "vexec_source.h"

#include "vexec.h"
#include "batch/batch.h"
#include "expr/expr.h"

#define VEXEC_SCAN_NAME		"VecScan"
#define VEXEC_RESULT_NAME	"VecResult"
#define VEXEC_AGG_NAME		"VecAgg"
#define VEXEC_HASHJOIN_NAME	"VecHashJoin"
#define VEXEC_SORT_NAME		"VecSort"
#define VEXEC_BITMAPSCAN_NAME	"VecBitmapHeapScan"
#define VEXEC_REPART_NAME	"VecRepartition"
#define VEXEC_WINDOW_NAME	"VecWindowHashAgg"
#define VEXEC_INSERT_NAME	"VecInsert"
#define VEXEC_INGEST_NAME	"VecIngest"
#define VEXEC_MOTION_SEND_NAME	"VecMotionSend"
#define VEXEC_MOTION_RECV_NAME	"VecMotionRecv"
#define VEXEC_AGGJOIN_NAME	"VecAggJoin"

typedef enum VexecNodeKind
{
	VEXEC_NODE_SCAN,
	VEXEC_NODE_RESULT,
	VEXEC_NODE_AGG,
	VEXEC_NODE_HASHJOIN,
	VEXEC_NODE_SORT,
	VEXEC_NODE_REPART,
	VEXEC_NODE_WINDOW,
	VEXEC_NODE_INSERT,
	VEXEC_NODE_INGEST,
	VEXEC_NODE_MOTION_SEND,
	VEXEC_NODE_MOTION_RECV
} VexecNodeKind;

/*
 * VecAgg's plan, as its custom_private holds it (vecagg.c encodes and
 * decodes it).  The scan tuple, which custom_scan_tlist describes, is a
 * group's keys, the columns it keeps from its first row, and its
 * aggregates' results; the node's HAVING qual and target list read it as
 * INDEX_VAR.  The aggregates are in executor form: their arguments and
 * FILTERs read the child's output as OUTER_VAR.
 */
#define VEXEC_AGGCOL_KEY	0	/* outsrc: the key's index */
#define VEXEC_AGGCOL_EXTRA	1	/* outsrc: the child's column, from 1 */
#define VEXEC_AGGCOL_AGG	2	/* outsrc: the aggregate's aggno */

typedef struct VexecAggPlan
{
	AggStrategy strategy;		/* AGG_PLAIN or AGG_HASHED */
	AggSplit	split;
	double		numgroups;
	List	   *keycols;		/* the child's columns, from 1 */
	List	   *eqops;			/* their grouping operators */
	List	   *collations;
	List	   *outkind;		/* per scan tuple column, VEXEC_AGGCOL_* */
	List	   *outsrc;
	List	   *aggrefs;		/* each aggno once, in aggno order */
	int			ntrans;			/* transition states: aggtransno's range */
	int64		topk;			/* C5: the groups a LIMIT without ORDER BY
								 * takes, kept by their hashes; 0: all */
	bool		sorted_input;	/* VC4: hashed, but the input may come in the
								 * keys' order, its scan's source's: taken so
								 * where the scan says so as it begins */
} VexecAggPlan;

/*
 * VecHashJoin's plan (vechashjoin.c encodes and decodes custom_private).
 * Its scan tuple, which custom_scan_tlist describes, is a join row: the
 * outer child's output columns, 1 to nouter, then the inner child's, from
 * nouter + 1.  The node's target list and qual -- the join's other quals,
 * a HashJoin's plan.qual -- read it as INDEX_VAR, and so does custom_exprs:
 * the hash clauses, the join quals, and the outer and inner hash keys, a
 * list each, at the positions below.
 */
#define VEXEC_JOIN_HASHCLAUSES	0
#define VEXEC_JOIN_JOINQUAL		1
#define VEXEC_JOIN_OUTERKEYS	2
#define VEXEC_JOIN_INNERKEYS	3

typedef struct VexecJoinPlan
{
	JoinType	jointype;		/* inner, left, semi, anti, right */
	bool		inner_unique;	/* an outer row meets one inner row at most */
	int			nouter;			/* the scan tuple's columns from the outer
								 * child */
	int			ninner;			/* and from the inner child */
	List	   *hashoperators;	/* per hash key, its equality operator */
	List	   *hashcollations;
	double		inner_rows;		/* the planner's estimate of the build side */
} VexecJoinPlan;

/*
 * VecSort's plan (vecsort.c encodes and decodes custom_private): its keys,
 * columns of its child's rows from 1, with their operators, collations and
 * NULLS FIRST flags, as a Sort's; and the bound the planner gave it, 0 for
 * none.  Its scan tuple, which custom_scan_tlist describes, is the child's
 * row, which its target list reads as INDEX_VAR.
 */
typedef struct VexecSortPlan
{
	List	   *keycols;
	List	   *operators;
	List	   *collations;
	List	   *nullsfirst;
	int64		bound;

	/*
	 * Late columns (H6): the child, a VecScan of the relation late_relid,
	 * gives the keys and each row's TID, in column late_tidcol, and the
	 * scan tuple -- custom_scan_tlist, its columns over the relation's Vars
	 * -- is computed over each kept row, fetched by its TID.  0: the child
	 * gives the scan tuple.
	 */
	int			late_tidcol;
	Index		late_relid;

	/*
	 * The running bound (H6): the child, a vector scan, checks its rows
	 * against the first key of the sort's N-th row, column bound_attno of
	 * the scan's relation, before the scan's quals where none of them may
	 * raise an error, else after them.  0: none.
	 */
	AttrNumber	bound_attno;
	bool		bound_before_quals;
} VexecSortPlan;

/*
 * A bounded VecSort's running bound (H6, §3.14): the first sort key of the
 * N-th of the rows it holds, once tuplesort keeps them in its bounded heap.
 * A row whose first key is strictly past it is one tuplesort would discard
 * as it came, and the sort's vector scan drops it -- in heap's page reader
 * before its other columns are deformed, or among a batch's rows.  The
 * sort's, lent to the scan below it in the same process.
 */
typedef struct VexecSortBound
{
	AttrNumber	attno;			/* the scan's relation's column */
	bool		before_quals;	/* checked before the scan's quals */
	Oid			sortop;			/* the first key's ordering operator */
	SortSupportData ssup;		/* the first key's order */
	bool		active;			/* a bound is set */
	Datum		value;
	bool		isnull;
	uint64		version;		/* bumped as the bound moves */
	int64		removed;		/* rows the scan dropped, EXPLAIN ANALYZE's */
	MemoryContext tmpcxt;		/* the comparisons' detoasted values */
} VexecSortBound;

/*
 * X6 (§3.18): a VecHashJoin's inner keys, lent to the vector scan on its
 * outer side once the table is built: the range of one integer key's
 * values, and the values themselves -- as a bitmap of the range where it
 * is small enough, else as a Bloom filter.  The scan drops a row whose key
 * no inner row has, which the join would meet with none: only on the outer
 * side of an inner, semi or right join.  Before the scan's quals where
 * none may raise, else after them, as H6's bound.  Off for
 * VEXEC_JOIN_FILTER_OFF batches where it kept more than 70% of a batch's
 * rows, as ClickHouse turns its join's runtime filter off.
 *
 * VC3: a scan whose source takes a join's keys (vexec_source.h,
 * set_join_keys) gives it the range, and the distinct values where there
 * are at most VEXEC_JOIN_KEYS_VALUES of them, for the source to pass over
 * its units by; the version says which build they are of.
 */
#define VEXEC_JOIN_FILTER_OFF	30
#define VEXEC_JOIN_KEYS_VALUES	1024

typedef struct VexecJoinFilter
{
	AttrNumber	attno;			/* the scan's relation's column */
	bool		before_quals;	/* checked before the scan's quals */
	bool		active;			/* the table is built */
	int64		min;			/* the inner keys' range */
	int64		max;
	uint64	   *bits;			/* a bit a value of the range, or NULL */
	uint64	   *bloom;			/* else a Bloom filter's bits */
	uint32		bloom_mask;		/* its bits less one */
	uint64		version;		/* bumped at each build lent: 1 the first */
	int			nvalues;		/* VC3: the distinct values, ascending, or 0 */
	int64	   *values;
	int			off;			/* batches left with the filter off */
	int64		removed;		/* rows dropped, EXPLAIN ANALYZE's */
	int64		off_batches;	/* batches the filter was off for */
} VexecJoinFilter;

/*
 * Whether an inner row may have key v: within the range, and its bit set,
 * or both of its Bloom bits, two of its hash's.
 */
static inline bool
vexec_join_filter_has(const VexecJoinFilter *f, int64 v)
{
	if (v < f->min || v > f->max)
		return false;
	if (f->bits != NULL)
	{
		uint64		i = (uint64) (v - f->min);

		return (f->bits[i / 64] >> (i % 64)) & 1;
	}
	if (f->bloom != NULL)
	{
		uint64		h = (uint64) v * UINT64CONST(0x9E3779B97F4A7C15);
		uint32		a = (uint32) (h >> 32) & f->bloom_mask;
		uint32		b = (uint32) h & f->bloom_mask;

		return ((f->bloom[a / 64] >> (a % 64)) & 1) && ((f->bloom[b / 64] >> (b % 64)) & 1);
	}
	return true;
}

/*
 * VecRepartition's plan (vecrepart.c): the keys its child's rows are dealt
 * out by among a Gather's participants, columns of the child's rows from 1,
 * with the grouping's equality operators, whose hash functions deal them,
 * and the keys' collations (H5).  Its scan tuple is the child's row, as
 * VecSort's.
 */
typedef struct VexecRepartPlan
{
	List	   *keycols;
	List	   *eqops;
	List	   *collations;
} VexecRepartPlan;

/*
 * VecWindowHashAgg's plan (vecwindow.c encodes and decodes custom_private):
 * a hashed window's partition keys, columns of its child's rows from 1, with
 * the window's equality operators and collations; the window's order within
 * a partition, columns with their ordering operators, collations and NULLS
 * FIRST flags; and the keys of the Sort it stands in place of -- the
 * partition's columns, then the window's order -- by which it sorts the
 * rows of a file past its last level.  Its scan tuple is its child's row,
 * as VecSort's.
 */
typedef struct VexecWindowPlan
{
	List	   *partcols;
	List	   *parteqops;
	List	   *partcollations;
	List	   *ordcols;
	List	   *ordops;
	List	   *ordcollations;
	List	   *ordnullsfirst;
	List	   *sortcols;
	List	   *sortops;
	List	   *sortcollations;
	List	   *sortnullsfirst;
} VexecWindowPlan;

/*
 * VecMotionSend's plan (vecmotion.c encodes and decodes custom_private;
 * motion/motion.h): the kind of the Motion above it as ORCA built it --
 * whose frames it sends to the segments its keys hash to, the next segment
 * in turn, every segment or the one gathering -- and, for a Redistribute,
 * its hash functions, one for each key of custom_exprs, which read the
 * child's rows as OUTER_VAR, and whether one is a legacy function.  Its scan
 * tuple, which custom_scan_tlist describes, is the child's row, then the
 * frame's segment and the frame, which its target list gives out.
 */
typedef struct VexecMotionSendPlan
{
	int			motion;			/* VEXEC_GP_MOTION_*: HASH, RANDOM,
								 * BROADCAST or GATHER */
	bool		legacy;			/* a key hashed by a legacy function */
	List	   *hashfuncs;
	int			ncols;			/* the child's columns */
} VexecMotionSendPlan;

typedef struct VexecNode VexecNode;

/* What the node's EXPLAIN ANALYZE counts. */
typedef struct VexecNodeStats
{
	int64		batches;
	int64		rows_in;
	int64		kernel_steps;	/* kernel calls, a batch each */
	int64		fallback_rows;	/* rows the fallback evaluated */
	int64		lazy_rows;		/* rows PostgreSQL's evaluator ran lazily */
	int64		memo_rows;		/* row-by-row targets' results reused */
	int64		redo_rows;		/* rows a kernel sent to it */
	int64		declared_rows;	/* rows declared calls answered */
	int64		batches_out;	/* handed to a vector parent */
	int64		drained_motions;	/* Motions read to their end, unread */
	int64		drained_rows;
	int64		squelched;		/* subtrees gp_core squelched (V7) */
	int64		text_converted; /* VC14: rows of text kept as a source handed
								 * it, made the format's for PostgreSQL's
								 * evaluator */
	int64		dict_values;	/* VC5: dictionary values subtrees were
								 * evaluated over */
	int64		dict_rows;		/* VC5: rows those results answered */
} VexecNodeStats;

struct VexecNode
{
	CustomScanState css;		/* first */
	VexecNodeKind kind;
	VexecLayoutConfig layout;	/* read when the node began (§3.4.4) */
	MemoryContext mcxt;			/* the node's */

	/* the input: its batch, and where a row of it is loaded */
	bool		(*fetch) (VexecNode *node);	/* the next input batch into in;
											 * false at the end */

	/*
	 * X9: a node whose output is not its input's rows -- VecAgg's groups --
	 * makes its batches for a vector parent itself: the next one, every row
	 * of it selected and none to redo, or NULL at the end; NULL where the
	 * node hands up rows alone, or batches of its input's rows
	 */
	VexecBatch *(*next_out) (VexecNode *node);
	bool		row_input;		/* reads its child's rows, a VecResult over a
								 * child that hands up no batches: it has
								 * none to hand up either */
	VexecBatch *in;
	Index		input_varno;	/* scanrelid, or OUTER_VAR */
	int			ninput;			/* the input's columns */
	AttrNumber *input_attnos;	/* for each input column, the attno it is */
	TupleTableSlot *input_slot; /* the scan slot, or an outer slot */
	int			loaded_row;		/* the input row in input_slot, or -1 */

	/*
	 * VC14: the input batch's columns a scan left in the layout its source
	 * handed them, not the format's (vexec_batch_apply_config_keep_bytes());
	 * and whether PostgreSQL's evaluator reads the input's rows -- a lazy
	 * qual or target, a fallback step -- so that those are made the
	 * format's for it, after the eager quals (node.c)
	 */
	int			in_kept;
	bool		needs_input_datums;

	/* the programs */
	int			nquals;
	VexecTop   *quals;			/* in the plan's order */
	int			first_lazy;		/* the first lazy qual, or nquals */
	int			ntargets;
	VexecTop   *targets;		/* the target list's, in its order */
	bool		any_lazy_target;
	struct VexecRowMemo **row_memo;	/* per target: a lazy one's results by
									 * its one input column's value, or NULL
									 * (node.c) */
	bool	   *target_inexact; /* per target, or NULL: evaluated for rows
								 * PostgreSQL may not evaluate it for, as
								 * an aggregate's argument under a FILTER */
	int			nkernels;		/* compiled */
	int			nfallbacks;
	int			nshared;		/* X10: subtrees the targets share */
	List	   *declared;		/* compiled calls bound to kernel packs'
								 * declarations, VexecExpr *, for EXPLAIN */
	ExprContext *eager_econtext;	/* the fallback's and parameters' */

	/*
	 * A bitmap's conditions to recheck, the first nrecheck quals: the rows
	 * they remove are counted apart (EXPLAIN's "Rows Removed by Index
	 * Recheck"), and PostgreSQL's evaluator runs them, recheck_qual, before
	 * the plan's own quals, which core gives ss.ps.qual alone
	 * (ExecInitCustomScan()).
	 */
	int			nrecheck;
	ExprState  *recheck_qual;

	/* the current batch */
	VexecBatch *work;			/* registers */
	uint64	   *candidates;		/* rows past the eager quals */
	uint64	   *redo;			/* rows for PostgreSQL's evaluator */
	uint64	  **qual_pass;		/* per qual after the first lazy one: its
								 * eager result */
	VexecVec  **outputs;		/* per eager target */
	VexecBatch *out;			/* the output batch, for a vector parent */
	int			next_row;
	bool		have_batch;
	bool		finished;

	/* a vector child's redo rows, resolved as the rows are reached */
	VexecNode  *vec_child;
	uint64	   *child_redo;

	/*
	 * Who resolves a row of child_redo where the input is not a vector
	 * child's batch: VecHashJoin, whose join rows still to be decided are
	 * decided as they are reached.  The input row, in input_slot, or NULL
	 * where it gives none.  NULL: the vector child, vexec_resolve_row().
	 */
	TupleTableSlot *(*resolve_input) (VexecNode *node, int row);

	const char *label;			/* EXPLAIN's name in text: NULL, the kind's */

	/*
	 * A VecSort's running bound over this node's rows (H6): a scan's input
	 * column bound_col holds its key; NULL: none.  bound_in_source: heap's
	 * page reader checks it, before the quals.
	 */
	VexecSortBound *bound;
	int			bound_col;
	bool		bound_in_source;

	/*
	 * X6: a VecHashJoin's inner keys over this scan's rows: the input column
	 * jfilter_col holds the key; NULL: none
	 */
	VexecJoinFilter *jfilter;
	int			jfilter_col;
	bool		jfilter_in_source;	/* heap's page reader checks it */

	/*
	 * VC2: a scan's input in steps, its source's (vexec_source.h,
	 * VEXEC_SRC_STEPS): nsteps 0 where every column comes at once.  in_step:
	 * per input column, the step it comes in; qual_step: per qual of the
	 * eager prefix, the last step its columns are in; steps_loaded: the
	 * steps of the batch now in `in` read so far, step 0 with it; load_step:
	 * the scan's read of a step, for the rows of `rows` (node.c)
	 */
	int			nsteps;
	int8	   *in_step;
	int8	   *qual_step;
	int			steps_loaded;
	void		(*load_step) (VexecNode *node, int step, const uint64 *rows);

	/*
	 * The node read its input in this process.  On a cluster the node a
	 * segment ran is, on the coordinator, a node that never ran: gp_core
	 * brings its Instrumentation from the segments, and nothing of a custom
	 * node's own, so EXPLAIN ANALYZE prints the node's own figures only
	 * where it ran, as PostgreSQL's Hash prints its table's.
	 */
	bool		ran;

	VexecNodeStats stats;

	/*
	 * The segments' figures of the node, on the coordinator, where it did
	 * not run: their sums, and how many reported them (motion/gpcore.c,
	 * GpCoreApi.explain_register).
	 */
	int			segments_seen;
	VexecNodeStats segment_stats;
};

/* methods.c */
extern void vexec_exec_install(void);
extern bool vexec_is_vector_node(Plan *plan);
extern bool vexec_is_vector_state(PlanState *ps);
extern const CustomScanMethods *vexec_scan_methods(void);
extern const CustomScanMethods *vexec_result_methods(void);
extern const CustomScanMethods *vexec_agg_methods(void);
extern const CustomScanMethods *vexec_hashjoin_methods(void);
extern const CustomScanMethods *vexec_sort_methods(void);
extern const CustomScanMethods *vexec_bitmapscan_methods(void);
extern const CustomScanMethods *vexec_repart_methods(void);
extern const CustomScanMethods *vexec_window_methods(void);
extern const CustomScanMethods *vexec_insert_methods(void);
extern const CustomScanMethods *vexec_ingest_methods(void);
extern const CustomScanMethods *vexec_motion_send_methods(void);
extern const CustomScanMethods *vexec_motion_recv_methods(void);
extern const CustomScanMethods *vexec_aggjoin_methods(void);

/*
 * Each kind's exec methods, which pg_otel_probe's reporter of vector nodes
 * is registered for (vexec_report_install(), node.c).
 */
extern const CustomExecMethods *vexec_scan_exec_methods(void);
extern const CustomExecMethods *vexec_bitmapscan_exec_methods(void);
extern const CustomExecMethods *vexec_result_exec_methods(void);
extern const CustomExecMethods *vexec_agg_exec_methods(void);
extern const CustomExecMethods *vexec_hashjoin_exec_methods(void);
extern const CustomExecMethods *vexec_sort_exec_methods(void);
extern const CustomExecMethods *vexec_repart_exec_methods(void);
extern const CustomExecMethods *vexec_window_exec_methods(void);
extern const CustomExecMethods *vexec_insert_exec_methods(void);
extern const CustomExecMethods *vexec_ingest_exec_methods(void);
extern const CustomExecMethods *vexec_motion_send_exec_methods(void);
extern const CustomExecMethods *vexec_motion_recv_exec_methods(void);
extern void vexec_report_install(void);

/* node.c: what every node shares */
extern void vexec_node_begin(VexecNode *node, EState *estate);
extern void vexec_node_compile(VexecNode *node, List *quals, List *tlist);
extern TupleTableSlot *vexec_node_exec(VexecNode *node);
extern void vexec_node_rescan(VexecNode *node);
extern void vexec_node_end(VexecNode *node);
extern void vexec_node_explain(VexecNode *node, List *ancestors, ExplainState *es);
extern void vexec_node_relabel(VexecNode *node, ExplainState *es);
extern void vexec_node_explain_properties(VexecNode *node, List *ancestors, ExplainState *es);
extern void vexec_node_load_input(VexecNode *node, int row);
extern void vexec_node_count_fallback(VexecNode *node, int rows);
extern void vexec_node_count_kernel(VexecNode *node);
extern void vexec_node_count_declared(VexecNode *node, int rows);
extern void vexec_node_explain_declared(VexecNode *node, ExplainState *es);
extern VexecBatch *vexec_next_batch(PlanState *ps);
extern bool vexec_node_batchable(VexecNode *node);
extern TupleTableSlot *vexec_resolve_row(VexecNode *node, int row);
extern TupleTableSlot *vexec_node_unprojected(VexecNode *node, TupleTableSlot *input);
extern bool vexec_node_next_input(VexecNode *node);

/* vecscan.c, vecresult.c, vecagg.c */
extern Node *vexec_create_scan_state(CustomScan *cscan);
extern Node *vexec_create_bitmapscan_state(CustomScan *cscan);
extern bool vexec_scan_can_aggregate(VexecNode *node);
extern bool vexec_scan_aggregate(VexecNode *node, int nreqs, const VexecSourceAgg *reqs,
								 VexecSourceAggAnswer *answers, int64 *nrows);
extern const char *vexec_scan_source_name(VexecNode *node);
extern int	vexec_scan_ordering(VexecNode *node, int max, AttrNumber *attnums);
extern Node *vexec_create_result_state(CustomScan *cscan);
extern Node *vexec_create_agg_state(CustomScan *cscan);
extern List *vexec_agg_plan_encode(const VexecAggPlan *plan);
extern void vexec_agg_plan_decode(CustomScan *cscan, VexecAggPlan *plan);

/*
 * vecagg.c: VecAgg's hash of a row's keys (C4, §3.19), which VecRepartition
 * deals rows out by and hands up, and a partial VecAgg's groups' hashes,
 * which VecRepartition takes from it.
 */
typedef struct VexecKeyHash VexecKeyHash;
extern VexecKeyHash *vexec_key_hash_create(TupleDesc desc, List *keycols, List *eqops,
										   List *collations);
extern uint32 vexec_key_hash_slot(VexecKeyHash *kh, TupleTableSlot *slot);
extern bool vexec_agg_hashes_keys(VexecNode *node, List *keycols, List *eqops,
								  List *collations);
extern uint32 vexec_agg_last_hash(VexecNode *node);
extern const uint32 *vexec_agg_batch_hashes(VexecNode *node);
extern void vexec_agg_set_topk_shared(VexecNode *node, pg_atomic_uint32 *shared);
extern uint32 vexec_agg_topk_bound(VexecNode *node);
extern uint32 vexec_repart_topk_bound(VexecNode *node);
extern void vexec_agg_deal(VexecNode *node, int npartitions,
						   int (*deal) (void *arg, uint32 hash), void *arg);
extern int	vexec_agg_dealt_partition(VexecNode *node);
extern void vexec_agg_allow_freeze(VexecNode *node);

/*
 * D2 (§3.20): a partial VecAgg's groups handed on as they lie -- a group's
 * header, keys, extras and states as the table holds them, a
 * by-reference value's bytes after them, at the offset its Datum then
 * holds -- into a VecRepartition's partitions, which hold them in chunks
 * of the parallel query's shared memory, and a final VecAgg above that
 * merges them as they lie (vecagg.c, vecrepart.c)
 */
typedef struct VexecPackSink VexecPackSink;
extern char *vexec_pack_alloc(VexecPackSink *sink, int partition, Size len);
extern void vexec_pack_batch_end(VexecPackSink *sink);
extern bool vexec_agg_pack_ok(VexecNode *node);
extern void vexec_agg_pack(VexecNode *node, VexecPackSink *sink, int npartitions,
						   int (*deal) (void *arg, uint32 hash), void *arg);
extern int64 vexec_agg_pack_all(VexecNode *node);

/* aggsizes.c: the groups VecAgg's tables reached, in shared memory (C6) */
extern void vexec_aggsize_install(void);
extern uint64 vexec_aggsize_key(EState *estate, int plan_node_id);
extern int64 vexec_aggsize_get(uint64 key);
extern void vexec_aggsize_put(uint64 key, int64 groups);
extern bool vexec_agg_set_topn(VexecNode *node, int64 bound, List *keycols, List *sortops,
							   List *collations, List *nullsfirst);

/* vecsort.c */
extern Node *vexec_create_sort_state(CustomScan *cscan);
extern List *vexec_sort_plan_encode(const VexecSortPlan *plan);
extern void vexec_sort_plan_decode(CustomScan *cscan, VexecSortPlan *plan);
extern void vexec_sort_set_bound(CustomScan *cscan, int64 bound);
extern bool vexec_sort_make_late(CustomScan *cscan, bool serial_ok, bool parallel_ok);
extern bool vexec_sort_plan_running_bound(CustomScan *cscan);
extern bool vexec_scan_set_bound(PlanState *ps, VexecSortBound *bound);
extern pg_atomic_uint64 *vexec_scan_shared_bound(PlanState *ps);
extern bool vexec_scan_set_join_filter(PlanState *ps, VexecJoinFilter *filter);
extern bool vexec_scan_takes_join_keys(PlanState *ps, AttrNumber attno);

/* vecrepart.c */
extern Node *vexec_create_repart_state(CustomScan *cscan);
extern List *vexec_repart_plan_encode(const VexecRepartPlan *plan);
extern void vexec_repart_plan_decode(CustomScan *cscan, VexecRepartPlan *plan);
extern bool vexec_repart_hashes_keys(VexecNode *node, List *keycols, List *eqops,
									 List *collations);
extern const uint32 *vexec_repart_batch_hashes(VexecNode *node);
extern VexecNode *vexec_repart_child_agg(VexecNode *node);
extern bool vexec_repart_offer_unpack(VexecNode *node);
extern bool vexec_repart_packed(VexecNode *node);
extern bool vexec_repart_next_chunk(VexecNode *node, const char **groups, Size *len);

/* vecwindow.c */
extern Node *vexec_create_window_state(CustomScan *cscan);
extern List *vexec_window_plan_encode(const VexecWindowPlan *plan);
extern void vexec_window_plan_decode(CustomScan *cscan, VexecWindowPlan *plan);

/* vecinsert.c */
extern Node *vexec_create_insert_state(CustomScan *cscan);

/* vecaggjoin.c: a row node, two aggregations' groups joined by their keys */
extern Node *vexec_create_aggjoin_state(CustomScan *cscan);

/* vecingest.c */
extern Node *vexec_create_ingest_state(CustomScan *cscan);

/* vecmotion.c */
extern Node *vexec_create_motion_send_state(CustomScan *cscan);
extern Node *vexec_create_motion_recv_state(CustomScan *cscan);
extern List *vexec_motion_send_plan_encode(const VexecMotionSendPlan *plan);
extern void vexec_motion_send_plan_decode(CustomScan *cscan, VexecMotionSendPlan *plan);

/* vechashjoin.c */
extern Node *vexec_create_hashjoin_state(CustomScan *cscan);
extern List *vexec_join_plan_encode(const VexecJoinPlan *plan);
extern void vexec_join_plan_decode(CustomScan *cscan, VexecJoinPlan *plan);
extern bool vexec_plan_read_ahead_safe(Plan *plan);

#endif							/* VEXEC_EXEC_H */
