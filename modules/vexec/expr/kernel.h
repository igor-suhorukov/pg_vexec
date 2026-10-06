/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * kernel.h
 *	  Kernels: C functions that apply one PostgreSQL function to the rows
 *	  of a batch (pg_vector_executor.md §3.7, §3.4.3).
 *
 * Binding.  A kernel is found by the function's OID, its input types, the
 * collation class and, per batch, the layouts of its inputs; never by name
 * or through search_path.  The tables are the built-in functions of
 * pg_catalog, whose OIDs are fixed below FirstGenbkiObjectId and named by
 * utils/fmgroids.h.  An extension's function has no kernel: it runs in the
 * fallback, or, where a kernel pack declares it (§3.17, packs.c), through
 * fmgr a batch's rows at a time (eval.c's call_rows()).  What ExecInitFunc
 * does for a call site, binding does too: the EXECUTE permission check and
 * InvokeFunctionExecuteHook (PG19:src/backend/executor/execExpr.c:2708-2711);
 * a security-definer function, one with proconfig, and one a plugin claims
 * through needs_fmgr_hook are left to fmgr.
 *
 * Calling.  A kernel computes the rows of `active`: rows selected, and, for
 * a strict function, whose arguments are all not NULL; the evaluator has
 * set the result's validity.  Arguments are flat or constant, in the
 * layouts the variant asked for.  A kernel never raises: a row on which the
 * PostgreSQL function would raise is set in `failed`, and its result is
 * left as it is.  The node evaluates such rows with PostgreSQL's evaluator,
 * in row order, when they are asked for (expr.h).
 *
 * Variants.  Each kernel is written once per operation against a few
 * accessors, and instantiated for the layouts its inputs can have (§3.4.3):
 * the layouts both formats share have one variant; temporal values have one
 * per epoch, taken as a constant; varlena values one per layout, through
 * their bytes.  A layout with no variant gets a conversion first, or the
 * row-by-row variant: the function called through fmgr, for a function
 * that cannot raise, or every active row sent to PostgreSQL's evaluator,
 * for one that can.  A missing variant costs time, never an answer.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_KERNEL_H
#define VEXEC_KERNEL_H

#include "port/pg_bitutils.h"

#include "expr/expr.h"

#define VEXEC_KERNEL_MAXARGS	3

typedef struct VexecKernelCall
{
	VexecBatch *work;			/* the work batch: its arena, allocations */
	int			nrows;
	const uint64 *active;		/* rows to compute; never NULL */
	int			nargs;
	const VexecVec *args[VEXEC_KERNEL_MAXARGS];
	VexecVec   *result;			/* in the variant's shape, nvalues = nrows */
	uint64	   *failed;			/* rows the function would raise on */
	const VexecExpr *call;		/* collation, extra */
} VexecKernelCall;

typedef void (*VexecKernelFn) (VexecKernelCall *kc);

/* A kernel for the shapes of one call's arguments, as one batch has them. */
typedef struct VexecVariant
{
	VexecKernelFn fn;
	VexecShape	result;
} VexecVariant;

typedef struct VexecKernelDef
{
	const char *name;
	bool		can_fail;		/* may set rows in `failed` */

	/*
	 * When the call is compiled: whether the kernel takes it -- its
	 * constant arguments, its collation -- with what it keeps of them in
	 * call->extra.  NULL: it takes every call it is bound to.
	 */
	bool		(*bind) (VexecExpr *call, const void *info);

	/*
	 * For a batch: the variant for the arguments' shapes, after any
	 * conversion it makes of them into the work batch.  False: none, and
	 * the row-by-row variant runs.
	 */
	bool		(*variant) (VexecKernelCall *kc, VexecVec **args, VexecVariant *v);
} VexecKernelDef;

/* An entry of the binding table. */
typedef struct VexecKernelEntry
{
	Oid			funcid;
	const VexecKernelDef *def;
	const void *info;			/* the family's static data for the function */
} VexecKernelEntry;

/* kernels.c */
extern const VexecKernelEntry *vexec_kernel_find(Oid funcid, int nargs, const Oid *argtypes,
												 Oid collation);
extern bool vexec_kernel_permitted(Oid funcid);
extern int	vexec_kernel_count(void);
extern Datum vexec_kernel_list(PG_FUNCTION_ARGS);

/*
 * packs.c: a call bound to a kernel pack's declaration (vexec_kernels.h):
 * the pack's and the declaration's static data, valid for the process.
 */
typedef struct VexecDeclared
{
	const struct VexecKernelPack *pack;
	const struct VexecKernelDecl *decl;
} VexecDeclared;

extern bool vexec_declared_find(Oid funcid, VexecDeclared *out);
extern bool vexec_declared_never_raises(Oid funcid);
extern const char *vexec_decl_kind_name(int kind);
extern Datum vexec_kernel_packs(PG_FUNCTION_ARGS);
extern Datum vexec_declared_calls(PG_FUNCTION_ARGS);

/* eval.c: a declared call's kernel, which has no variant (call_rows()) */
extern const VexecKernelDef vexec_declared_kernel;

/* The families, each its own file. */
extern void vexec_kernels_compare(void (*add) (Oid, const VexecKernelDef *, const void *));
extern void vexec_kernels_arith(void (*add) (Oid, const VexecKernelDef *, const void *));
extern void vexec_kernels_text(void (*add) (Oid, const VexecKernelDef *, const void *));
extern void vexec_kernels_numeric(void (*add) (Oid, const VexecKernelDef *, const void *));
extern void vexec_kernels_datetime(void (*add) (Oid, const VexecKernelDef *, const void *));

/* Iterating a bitmap's set bits, a word at a time. */
#define VEXEC_FOREACH_ROW(bits, nrows, i) \
	for (int _w = 0, _nw = VEXEC_WORDS(nrows); _w < _nw; _w++) \
		for (uint64 _word = (bits)[_w]; _word != 0; _word &= _word - 1) \
			for (int i = _w * 64 + pg_rightmost_one_pos64(_word), _once = 1; _once; _once = 0)

/* What kernels share: a value of an argument at a row, flat or constant. */
static inline int
vexec_arg_row(const VexecVec *v, int row)
{
	return v->encoding == VEXEC_CONST ? 0 : row;
}

/* The result of a boolean kernel: bit i of its values. */
static inline void
vexec_result_bit(VexecVec *r, int row, bool value)
{
	uint64	   *bits = (uint64 *) r->values;

	if (value)
		bits[row >> 6] |= UINT64CONST(1) << (row & 63);
	else
		bits[row >> 6] &= ~(UINT64CONST(1) << (row & 63));
}

static inline void
vexec_fail(VexecKernelCall *kc, int row)
{
	kc->failed[row >> 6] |= UINT64CONST(1) << (row & 63);
}

/* A boolean shape: the internal one is a bit a row. */
extern const VexecShape vexec_bool_bits;

#endif							/* VEXEC_KERNEL_H */
