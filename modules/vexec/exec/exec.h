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

#include "vexec_source.h"

#include "vexec.h"
#include "batch/batch.h"
#include "expr/expr.h"

#define VEXEC_SCAN_NAME		"VecScan"
#define VEXEC_RESULT_NAME	"VecResult"

typedef enum VexecNodeKind
{
	VEXEC_NODE_SCAN,
	VEXEC_NODE_RESULT
} VexecNodeKind;

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
	int			nkernels;		/* compiled */
	int			nfallbacks;
	ExprContext *eager_econtext;	/* the fallback's and parameters' */

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

	VexecNodeStats stats;
};

/* methods.c */
extern void vexec_exec_install(void);
extern bool vexec_is_vector_node(Plan *plan);
extern bool vexec_is_vector_state(PlanState *ps);
extern const CustomScanMethods *vexec_scan_methods(void);
extern const CustomScanMethods *vexec_result_methods(void);

/* node.c: what every node shares */
extern void vexec_node_begin(VexecNode *node, EState *estate);
extern void vexec_node_compile(VexecNode *node, List *quals, List *tlist);
extern TupleTableSlot *vexec_node_exec(VexecNode *node);
extern void vexec_node_rescan(VexecNode *node);
extern void vexec_node_end(VexecNode *node);
extern void vexec_node_explain(VexecNode *node, List *ancestors, ExplainState *es);
extern void vexec_node_load_input(VexecNode *node, int row);
extern void vexec_node_count_fallback(VexecNode *node, int rows);
extern void vexec_node_count_kernel(VexecNode *node);
extern VexecBatch *vexec_next_batch(PlanState *ps);
extern bool vexec_node_batchable(VexecNode *node);
extern TupleTableSlot *vexec_resolve_row(VexecNode *node, int row);
extern TupleTableSlot *vexec_node_unprojected(VexecNode *node, TupleTableSlot *input);

/* vecscan.c, vecresult.c */
extern Node *vexec_create_scan_state(CustomScan *cscan);
extern Node *vexec_create_result_state(CustomScan *cscan);

#endif							/* VEXEC_EXEC_H */
