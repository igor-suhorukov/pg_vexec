/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_testpack.c
 *	  A kernel pack of vexec's tests (pg_vector_executor.md §3.17, VK): an
 *	  extension's functions, and a pack's declarations of them, in one
 *	  library.
 *
 * The functions, each a member of the extension vexec_testpack:
 *
 *	tp_mix(int8)		its bits mixed, which raises nothing: declared never
 *						raising
 *	tp_div(int8, int8)	a / b, which raises on division by zero and on
 *						INT64_MIN / -1, as int8div does: declared with a check
 *	tp_le(int8, int8)	a <= b, which raises on a negative b: declared with a
 *						prefilter that answers for b >= 0 where a < 0 (true)
 *						and where a > b + 1000 (false), and leaves every other
 *						row undecided
 *	tp_bytes(bytea)		its length times 256 plus its first byte, which raises
 *						on an empty value: declared with a check, which also
 *						fails a value handed to it toasted -- vexec detoasts a
 *						check's arguments, so a row it would fail so shows as
 *						a row sent to PostgreSQL in EXPLAIN ANALYZE
 *	tp_lazy(int8)		raises on a negative value: declared nowhere
 *	tp_context(int8)	its argument, but raises where it is called in its
 *						own fn_mcxt, the query's memory, where PostgreSQL's
 *						evaluator never calls a function: declared nowhere
 *	tp_alias(int8)		tp_mix's arithmetic under a C symbol of its own, which
 *						the declaration of that name does not name
 *	tp_mix(int4)		an overload, whose signature no declaration has
 *
 * The pack names version 1.0 of the extension alone; 1.1 has the same
 * functions, under a version it does not name.  It registers while the
 * postmaster preloads libraries, and only then: loaded by a call of one of
 * its functions, it registers nothing.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/int.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "utils/memutils.h"
#include "varatt.h"

#include "vexec_kernels.h"

PG_MODULE_MAGIC_EXT(
					.name = "vexec_testpack",
					.version = "1.0"
);

PG_FUNCTION_INFO_V1(tp_mix);
PG_FUNCTION_INFO_V1(tp_mix4);
PG_FUNCTION_INFO_V1(tp_alias);
PG_FUNCTION_INFO_V1(tp_div);
PG_FUNCTION_INFO_V1(tp_le);
PG_FUNCTION_INFO_V1(tp_bytes);
PG_FUNCTION_INFO_V1(tp_lazy);
PG_FUNCTION_INFO_V1(tp_context);

static uint64
mix(uint64 x)
{
	x ^= x >> 31;
	x *= UINT64CONST(0x9E3779B97F4A7C15);
	x ^= x >> 29;
	return x;
}

Datum
tp_mix(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) (mix((uint64) PG_GETARG_INT64(0)) >> 1));
}

Datum
tp_mix4(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) (mix((uint64) (int64) PG_GETARG_INT32(0)) >> 1));
}

Datum
tp_alias(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64((int64) (mix((uint64) PG_GETARG_INT64(0)) >> 1));
}

Datum
tp_div(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int64		b = PG_GETARG_INT64(1);

	if (b == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));
	if (b == -1 && a == PG_INT64_MIN)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
	PG_RETURN_INT64(a / b);
}

Datum
tp_le(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int64		b = PG_GETARG_INT64(1);

	if (b < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("tp_le: a negative bound, " INT64_FORMAT, b)));
	PG_RETURN_BOOL(a <= b);
}

Datum
tp_bytes(PG_FUNCTION_ARGS)
{
	bytea	   *v = PG_GETARG_BYTEA_PP(0);
	int			len = VARSIZE_ANY_EXHDR(v);

	if (len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("tp_bytes: an empty value")));
	PG_RETURN_INT32(len * 256 + (unsigned char) VARDATA_ANY(v)[0]);
}

Datum
tp_lazy(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);

	if (a < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("tp_lazy: a negative value, " INT64_FORMAT, a)));
	PG_RETURN_INT64(a + 1);
}

Datum
tp_context(PG_FUNCTION_ARGS)
{
	if (CurrentMemoryContext == fcinfo->flinfo->fn_mcxt)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("tp_context: called in its own fn_mcxt")));
	PG_RETURN_INT64(PG_GETARG_INT64(0));
}

/* ---- the pack's own functions: never raise ---- */

/* tp_div's check: neither of the two divisions it raises on */
static Datum
check_div(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int64		b = PG_GETARG_INT64(1);

	PG_RETURN_BOOL(b != 0 && !(b == -1 && a == PG_INT64_MIN));
}

/* tp_le's prefilter: the answer where b >= 0 and a is far from it */
static Datum
prefilter_le(PG_FUNCTION_ARGS)
{
	int64		a = PG_GETARG_INT64(0);
	int64		b = PG_GETARG_INT64(1);
	int64		limit;

	if (b >= 0 && a < 0)
		PG_RETURN_BOOL(true);
	if (b >= 0 && !pg_add_s64_overflow(b, 1000, &limit) && a > limit)
		PG_RETURN_BOOL(false);
	ereturn(fcinfo->context, (Datum) 0,
			errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			errmsg("tp_le: undecided"));
}

/* tp_bytes's check: a value that is not empty, handed to it detoasted */
static Datum
check_bytes(PG_FUNCTION_ARGS)
{
	struct varlena *v = (struct varlena *) DatumGetPointer(PG_GETARG_DATUM(0));

	if (VARATT_IS_EXTENDED(v))
		PG_RETURN_BOOL(false);
	PG_RETURN_BOOL(VARSIZE(v) > VARHDRSZ);
}

static const char *const args_int8[] = {"int8"};
static const char *const args_int8_int8[] = {"int8", "int8"};
static const char *const args_bytea[] = {"bytea"};

static const VexecKernelDecl decls[] = {
	{"tp_mix", 1, args_int8, "tp_mix", "$libdir/vexec_testpack", VEXEC_DECL_NEVER_RAISES, NULL},
	{"tp_div", 2, args_int8_int8, "tp_div", "$libdir/vexec_testpack", VEXEC_DECL_CHECK, check_div},
	{"tp_le", 2, args_int8_int8, "tp_le", "$libdir/vexec_testpack", VEXEC_DECL_PREFILTER, prefilter_le},
	{"tp_bytes", 1, args_bytea, "tp_bytes", "$libdir/vexec_testpack", VEXEC_DECL_CHECK, check_bytes},
	/* the C symbol tp_alias(int8) is not created with */
	{"tp_alias", 1, args_int8, "tp_mix", "$libdir/vexec_testpack", VEXEC_DECL_NEVER_RAISES, NULL},
};

static const char *const versions[] = {"1.0", NULL};

static const VexecKernelPack pack = {
	.size = sizeof(VexecKernelPack),
	.minor = VEXEC_KERNELS_MINOR,
	.name = "vexec_testpack",
	.extension = "vexec_testpack",
	.versions = versions,
	.ndecls = lengthof(decls),
	.decls = decls,
};

void
_PG_init(void)
{
	if (process_shared_preload_libraries_in_progress)
		vexec_register_kernel_pack(&pack);
}
