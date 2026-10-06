/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * expr.h
 *	  Vector programs: a vector node's expressions, compiled when the node
 *	  begins (pg_vector_executor.md §3.7).
 *
 * A vector node's plan holds only PostgreSQL's Expr trees, its qual and its
 * target list; BeginCustomScan compiles them, as ExecInitExpr compiles row
 * programs, so plans copy, travel to workers and segments, and print in
 * EXPLAIN as any plan does.
 *
 * Each top-level qual and each target entry is one of two things:
 *
 *	eager	a tree of vector steps, evaluated a batch at a time: kernels,
 *			boolean logic, NULL tests, constants and parameters, and the
 *			fallback -- PostgreSQL's evaluator, row by row -- for the pure
 *			subtrees that have no kernel;
 *	lazy	PostgreSQL's evaluator, row by row, in row order, only for the
 *			rows the node's consumer asks for.
 *
 * An expression is eager when evaluating it ahead of PostgreSQL's order can
 * neither change an answer nor raise an error PostgreSQL would not: every
 * call in it has a kernel, or is of a leakproof function, which reveals
 * nothing of its arguments through errors, or is bound to a kernel pack's
 * declaration (vexec_kernels.h: of a function that never raises, called
 * only where its pack's check passes, or answered by its pack's prefilter),
 * and nothing in it is volatile or a SubPlan.  PostgreSQL evaluates a row's
 * quals in order, a row at a time, stops at the first that fails, and
 * evaluates nothing for rows its consumer never asks for; an expression
 * that may raise, or has side effects, is therefore lazy, so that its
 * errors, its call counts and its order stay PostgreSQL's (§3.7,
 * "SubPlans and volatile functions").
 *
 * Kernels never raise.  A kernel that meets a row on which the PostgreSQL
 * function would raise -- an overflow, a division by zero -- marks the row
 * failed and goes on; the node then evaluates that row with PostgreSQL's own
 * evaluator, in row order, when its consumer asks for it, which raises
 * exactly the error the row executor would have raised, SQLSTATE, message
 * and all, or none where PostgreSQL would not have evaluated the failing
 * call for that row (§3.7, "Errors are PostgreSQL's own").  Such rows are
 * the batch's redo rows, and so are a declared call's rows its pack's check
 * fails or its prefilter leaves undecided.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_EXPR_H
#define VEXEC_EXPR_H

#include "executor/execdesc.h"
#include "fmgr.h"
#include "nodes/execnodes.h"
#include "nodes/primnodes.h"

#include "vexec.h"
#include "batch/batch.h"

struct VexecKernelDef;
struct VexecNode;

typedef enum VexecExprKind
{
	VE_VAR,						/* a column of the node's input */
	VE_CONST,					/* a constant */
	VE_PARAM,					/* a parameter, read once a batch */
	VE_CALL,					/* a function with a kernel */
	VE_AND,
	VE_OR,
	VE_NOT,
	VE_NULLTEST,				/* IS [NOT] NULL */
	VE_BOOLTEST,				/* IS [NOT] TRUE / FALSE / UNKNOWN */
	VE_RELABEL,					/* the same values as another type */
	VE_SAOP,					/* x op ANY / ALL (an array constant) */
	VE_DISTINCT,				/* IS [NOT] DISTINCT FROM, through an
								 * equality kernel */
	VE_FALLBACK					/* PostgreSQL's evaluator, a row at a time:
								 * a pure subtree without a kernel */
} VexecExprKind;

/* VE_SAOP's array: its elements as constant registers. */
typedef struct VexecSaop
{
	bool		array_null;		/* the array itself is NULL */
	int			nelems;
	VexecVec  **elems;			/* constant registers; NULL for a NULL element */
} VexecSaop;

/* A compiled expression: a node of a vector program. */
typedef struct VexecExpr
{
	VexecExprKind kind;
	Expr	   *expr;			/* what it was compiled from */
	VexecType  *type;			/* its result's */
	int			nargs;
	struct VexecExpr **args;

	/* VE_VAR: the input column */
	int			col;
	/* VE_CONST: one value, in its type's build shape, in the node's memory */
	VexecVec   *constvec;
	/* VE_PARAM */
	ExprState  *param_state;
	int			paramid;		/* PARAM_EXEC's, or -1 */
	/* VE_CALL, VE_SAOP, VE_DISTINCT */
	const struct VexecKernelDef *kernel;
	Oid			funcid;
	Oid			collation;		/* the call's input collation */
	void	   *extra;			/* the kernel's own data for this call */
	bool		strict;
	FmgrInfo   *finfo;			/* for the row-by-row variant */
	/* VE_CALL bound to a kernel pack's declaration (packs.c), else NULL */
	const struct VexecKernelPack *pack;
	const struct VexecKernelDecl *decl;
	bool		useOr;			/* VE_SAOP: ANY */
	VexecSaop  *saop;			/* VE_SAOP: the array's elements */
	/* VE_NULLTEST, VE_BOOLTEST */
	int			testtype;
	/* VE_FALLBACK */
	ExprState  *state;
} VexecExpr;

/* A top-level qual or target entry: eager, or lazy. */
typedef struct VexecTop
{
	Expr	   *expr;
	VexecExpr  *eager;			/* NULL: lazy */
	ExprState  *state;			/* PostgreSQL's, for the lazy one and the
								 * row path; for a qual, ExecInitQual's */
	VexecType  *type;
	const char *why_lazy;		/* for EXPLAIN */
} VexecTop;

/* What a node's programs are compiled against. */
typedef struct VexecCompileContext
{
	PlanState  *parent;			/* the vector node */
	Index		input_varno;	/* scanrelid, or OUTER_VAR */
	int		   *attno_col;		/* attno -> input column, -1 none; from
								 * FirstLowInvalidHeapAttributeNumber */
	int			attno_base;		/* index of attno 0 in attno_col */
	int			attno_max;
	VexecType **col_types;		/* the input columns' */
	MemoryContext mcxt;			/* the node's */
	int			nkernels;		/* compiled: kernel steps */
	int			nfallbacks;		/* fallback steps */
	List	   *declared;		/* compiled: the declared calls' steps,
								 * VexecExpr *, for EXPLAIN */
	const char *why;			/* why the last top-level one is lazy */
} VexecCompileContext;

/* What a batch's evaluation shares. */
typedef struct VexecEval
{
	struct VexecNode *node;
	VexecBatch *in;				/* the input rows */
	VexecBatch *work;			/* where registers are allocated */
	int			nrows;
	uint64	   *redo;			/* rows for PostgreSQL's evaluator */
	bool		exact;			/* the rows evaluated are those PostgreSQL
								 * would reach: an InitPlan may run */
	ExprContext *econtext;		/* for fallback steps and parameters */
} VexecEval;

/* compile.c */
extern const char *vexec_expr_impurity(Node *expr, bool kernels);
extern VexecExpr *vexec_compile_expr(VexecCompileContext *cc, Expr *expr);
extern void vexec_compile_top(VexecCompileContext *cc, Expr *expr, bool is_qual,
							  VexecTop *top);
extern Oid	vexec_collation_class(Oid collation);

/* eval.c */
extern VexecVec *vexec_eval(VexecEval *ev, VexecExpr *e, const uint64 *active);
extern uint64 *vexec_eval_qual(VexecEval *ev, VexecExpr *e, const uint64 *active);
extern uint64 *vexec_bits_copy(VexecBatch *work, const uint64 *bits, int nrows);
extern uint64 *vexec_bits_and(VexecBatch *work, const uint64 *a, const uint64 *b, int nrows);
extern uint64 *vexec_bits_andnot(VexecBatch *work, const uint64 *a, const uint64 *b, int nrows);
extern void vexec_bits_or_into(uint64 *dst, const uint64 *src, int nrows);
extern bool vexec_bits_any(const uint64 *bits, int nrows);
extern int	vexec_bits_count(const uint64 *bits, int nrows);
extern int	vexec_bits_next(const uint64 *bits, int nrows, int from);

#endif							/* VEXEC_EXPR_H */
