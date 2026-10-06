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

typedef enum VexecNodeKind
{
	VEXEC_NODE_SCAN,
	VEXEC_NODE_RESULT,
	VEXEC_NODE_AGG,
	VEXEC_NODE_HASHJOIN,
	VEXEC_NODE_SORT,
	VEXEC_NODE_REPART,
	VEXEC_NODE_WINDOW
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

typedef struct VexecNode VexecNode;

/* What the node's EXPLAIN ANALYZE counts. */
typedef struct VexecNodeStats
{
	int64		batches;
	int64		rows_in;
	int64		kernel_steps;	/* kernel calls, a batch each */
	int64		fallback_rows;	/* rows the fallback evaluated */
	int64		lazy_rows;		/* rows PostgreSQL's evaluator ran lazily */
	int64		redo_rows;		/* rows a kernel sent to it */
	int64		batches_out;	/* handed to a vector parent */
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
	bool		row_input;		/* reads its child's rows, a VecResult over a
								 * child that hands up no batches: it has
								 * none to hand up either */
	VexecBatch *in;
	Index		input_varno;	/* scanrelid, or OUTER_VAR */
	int			ninput;			/* the input's columns */
	AttrNumber *input_attnos;	/* for each input column, the attno it is */
	TupleTableSlot *input_slot; /* the scan slot, or an outer slot */
	int			loaded_row;		/* the input row in input_slot, or -1 */

	/* the programs */
	int			nquals;
	VexecTop   *quals;			/* in the plan's order */
	int			first_lazy;		/* the first lazy qual, or nquals */
	int			ntargets;
	VexecTop   *targets;		/* the target list's, in its order */
	bool		any_lazy_target;
	bool	   *target_inexact; /* per target, or NULL: evaluated for rows
								 * PostgreSQL may not evaluate it for, as
								 * an aggregate's argument under a FILTER */
	int			nkernels;		/* compiled */
	int			nfallbacks;
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
	 * The node read its input in this process.  On a cluster the node a
	 * segment ran is, on the coordinator, a node that never ran: gp_core
	 * brings its Instrumentation from the segments, and nothing of a custom
	 * node's own, so EXPLAIN ANALYZE prints the node's own figures only
	 * where it ran, as PostgreSQL's Hash prints its table's.
	 */
	bool		ran;

	VexecNodeStats stats;
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
extern Node *vexec_create_result_state(CustomScan *cscan);
extern Node *vexec_create_agg_state(CustomScan *cscan);
extern List *vexec_agg_plan_encode(const VexecAggPlan *plan);
extern void vexec_agg_plan_decode(CustomScan *cscan, VexecAggPlan *plan);

/* vecsort.c */
extern Node *vexec_create_sort_state(CustomScan *cscan);
extern List *vexec_sort_plan_encode(const VexecSortPlan *plan);
extern void vexec_sort_plan_decode(CustomScan *cscan, VexecSortPlan *plan);
extern void vexec_sort_set_bound(CustomScan *cscan, int64 bound);
extern bool vexec_sort_make_late(CustomScan *cscan);
extern bool vexec_sort_plan_running_bound(CustomScan *cscan);
extern bool vexec_scan_set_bound(PlanState *ps, VexecSortBound *bound);

/* vecrepart.c */
extern Node *vexec_create_repart_state(CustomScan *cscan);
extern List *vexec_repart_plan_encode(const VexecRepartPlan *plan);
extern void vexec_repart_plan_decode(CustomScan *cscan, VexecRepartPlan *plan);

/* vecwindow.c */
extern Node *vexec_create_window_state(CustomScan *cscan);
extern List *vexec_window_plan_encode(const VexecWindowPlan *plan);
extern void vexec_window_plan_decode(CustomScan *cscan, VexecWindowPlan *plan);

/* vechashjoin.c */
extern Node *vexec_create_hashjoin_state(CustomScan *cscan);
extern List *vexec_join_plan_encode(const VexecJoinPlan *plan);
extern void vexec_join_plan_decode(CustomScan *cscan, VexecJoinPlan *plan);
extern bool vexec_plan_read_ahead_safe(Plan *plan);

#endif							/* VEXEC_EXEC_H */
