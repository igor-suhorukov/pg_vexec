/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * k_compare.c
 *	  Comparison kernels over fixed-width values (pg_vector_executor.md
 *	  §3.7, "The first kernel set"): integers of every width with each
 *	  other, floats with each other, oid, "char", bool, uuid, date,
 *	  timestamp and timestamptz, and date with timestamp.
 *
 * Each is written from the PostgreSQL function it stands for:
 *
 *	int2/4/8, cross-type	the C comparison of the two values widened
 *							(PG19:src/backend/utils/adt/int.c, int8.c)
 *	float4/8, cross-type	float.h's float8_eq ... float8_ge: every NaN
 *							equal to every other and above every number
 *							(PG19:src/include/utils/float.h:250-276); a
 *							float4 widened to float8 compares the same
 *	oid, "char"				unsigned (oid.c; char.c compares uint8)
 *	bool					false < true (bool.c)
 *	uuid					memcmp of the 16 bytes (uuid.c)
 *	date, timestamp[tz]		the integers, in one epoch: a column kept in
 *							Arrow's is shifted back first, the infinities
 *							staying at the integers' extremes
 *	date with timestamp		date_cmp_timestamp_internal(), PostgreSQL's
 *							own, a row at a time (date.c)
 *
 * None of them can raise.  A comparison of fixed-width values computes
 * every row of the batch, which costs less than testing which are active:
 * values at rows not computed are never read.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type.h"
#include "datatype/timestamp.h"
#include "utils/date.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/timestamp.h"

#include "vexec.h"
#include "expr/kernel.h"

typedef enum CmpOp
{
	OP_EQ,
	OP_NE,
	OP_LT,
	OP_LE,
	OP_GT,
	OP_GE
} CmpOp;

/* What a value is, for comparing: the C type it is read as. */
typedef enum CmpKind
{
	CK_I2,
	CK_I4,
	CK_I8,
	CK_F4,
	CK_F8,
	CK_OID,
	CK_CHAR,
	CK_BOOL,
	CK_UUID,
	CK_DATE,
	CK_TS,
	CK_DATE_TS,					/* date, timestamp */
	CK_TS_DATE					/* timestamp, date */
} CmpKind;

typedef struct CmpInfo
{
	uint8		op;
	uint8		kind;
} CmpInfo;

/* ---- the loops ---- */

#define INT_CMP(a, b, op) \
	((op) == OP_EQ ? (a) == (b) : (op) == OP_NE ? (a) != (b) : \
	 (op) == OP_LT ? (a) < (b) : (op) == OP_LE ? (a) <= (b) : \
	 (op) == OP_GT ? (a) > (b) : (a) >= (b))

#define FLT_CMP(a, b, op) \
	((op) == OP_EQ ? float8_eq(a, b) : (op) == OP_NE ? float8_ne(a, b) : \
	 (op) == OP_LT ? float8_lt(a, b) : (op) == OP_LE ? float8_le(a, b) : \
	 (op) == OP_GT ? float8_gt(a, b) : float8_ge(a, b))

/*
 * One loop per pair of C types and operator, computing every row of the
 * batch 64 at a time into the result's bits; a constant side read once.
 */
#define DEFINE_CMP(NAME, LT, RT, CT, CMP) \
static void \
NAME(VexecKernelCall *kc) \
{ \
	const VexecVec *l = kc->args[0]; \
	const VexecVec *r = kc->args[1]; \
	const LT   *a = (const LT *) l->values; \
	const RT   *b = (const RT *) r->values; \
	uint64	   *out = (uint64 *) kc->result->values; \
	int			op = ((const CmpInfo *) kc->call->extra)->op; \
	int			n = kc->nrows; \
	int			w; \
	\
	if (l->encoding != VEXEC_CONST && r->encoding != VEXEC_CONST) \
	{ \
		for (w = 0; w < VEXEC_WORDS(n); w++) \
		{ \
			uint64		word = 0; \
			int			base = w * 64; \
			int			m = Min(64, n - base); \
			\
			for (int j = 0; j < m; j++) \
			{ \
				CT			x = (CT) a[base + j]; \
				CT			y = (CT) b[base + j]; \
				\
				word |= (uint64) CMP(x, y, op) << j; \
			} \
			out[w] = word; \
		} \
	} \
	else if (r->encoding == VEXEC_CONST) \
	{ \
		CT			y = (CT) b[0]; \
		\
		for (w = 0; w < VEXEC_WORDS(n); w++) \
		{ \
			uint64		word = 0; \
			int			base = w * 64; \
			int			m = Min(64, n - base); \
			\
			for (int j = 0; j < m; j++) \
			{ \
				CT			x = (CT) a[base + j]; \
				\
				word |= (uint64) CMP(x, y, op) << j; \
			} \
			out[w] = word; \
		} \
	} \
	else \
	{ \
		CT			x = (CT) a[0]; \
		\
		for (w = 0; w < VEXEC_WORDS(n); w++) \
		{ \
			uint64		word = 0; \
			int			base = w * 64; \
			int			m = Min(64, n - base); \
			\
			for (int j = 0; j < m; j++) \
			{ \
				CT			y = (CT) b[base + j]; \
				\
				word |= (uint64) CMP(x, y, op) << j; \
			} \
			out[w] = word; \
		} \
	} \
}

DEFINE_CMP(cmp_i2_i2, int16, int16, int64, INT_CMP)
DEFINE_CMP(cmp_i2_i4, int16, int32, int64, INT_CMP)
DEFINE_CMP(cmp_i2_i8, int16, int64, int64, INT_CMP)
DEFINE_CMP(cmp_i4_i2, int32, int16, int64, INT_CMP)
DEFINE_CMP(cmp_i4_i4, int32, int32, int64, INT_CMP)
DEFINE_CMP(cmp_i4_i8, int32, int64, int64, INT_CMP)
DEFINE_CMP(cmp_i8_i2, int64, int16, int64, INT_CMP)
DEFINE_CMP(cmp_i8_i4, int64, int32, int64, INT_CMP)
DEFINE_CMP(cmp_i8_i8, int64, int64, int64, INT_CMP)
DEFINE_CMP(cmp_f4_f4, float4, float4, float8, FLT_CMP)
DEFINE_CMP(cmp_f4_f8, float4, float8, float8, FLT_CMP)
DEFINE_CMP(cmp_f8_f4, float8, float4, float8, FLT_CMP)
DEFINE_CMP(cmp_f8_f8, float8, float8, float8, FLT_CMP)
DEFINE_CMP(cmp_oid, uint32, uint32, uint32, INT_CMP)
DEFINE_CMP(cmp_char, uint8, uint8, uint8, INT_CMP)

/* bool: false < true, a word at a time over the bits */
static void
cmp_bool(VexecKernelCall *kc)
{
	const VexecVec *l = kc->args[0];
	const VexecVec *r = kc->args[1];
	const uint64 *a = (const uint64 *) l->values;
	const uint64 *b = (const uint64 *) r->values;
	uint64	   *out = (uint64 *) kc->result->values;
	int			op = ((const CmpInfo *) kc->call->extra)->op;
	int			w;

	for (w = 0; w < VEXEC_WORDS(kc->nrows); w++)
	{
		uint64		x = l->encoding == VEXEC_CONST ? ((a[0] & 1) ? ~UINT64CONST(0) : 0) : a[w];
		uint64		y = r->encoding == VEXEC_CONST ? ((b[0] & 1) ? ~UINT64CONST(0) : 0) : b[w];
		uint64		res;

		switch (op)
		{
			case OP_EQ:
				res = ~(x ^ y);
				break;
			case OP_NE:
				res = x ^ y;
				break;
			case OP_LT:
				res = ~x & y;
				break;
			case OP_LE:
				res = ~x | y;
				break;
			case OP_GT:
				res = x & ~y;
				break;
			default:
				res = x | ~y;
				break;
		}
		out[w] = res;
	}
}

/* uuid: memcmp of the 16 bytes, at each side's stride */
static void
cmp_uuid(VexecKernelCall *kc)
{
	const VexecVec *l = kc->args[0];
	const VexecVec *r = kc->args[1];
	const char *a = l->values;
	const char *b = r->values;
	int			op = ((const CmpInfo *) kc->call->extra)->op;
	int			i;

	for (i = 0; i < kc->nrows; i++)
	{
		int			c = memcmp(a + (Size) vexec_arg_row(l, i) * l->shape.stride,
							   b + (Size) vexec_arg_row(r, i) * r->shape.stride, 16);

		vexec_result_bit(kc->result, i, INT_CMP(c, 0, op));
	}
}

/*
 * date with timestamp, and timestamp with date: PostgreSQL's own
 * comparison, a row at a time.  A date past timestamp's range is above
 * every finite timestamp and below infinity; the function says so without
 * raising.
 */
static void
cmp_date_ts(VexecKernelCall *kc)
{
	const CmpInfo *info = kc->call->extra;
	bool		swapped = info->kind == CK_TS_DATE;
	const VexecVec *dv = kc->args[swapped ? 1 : 0];
	const VexecVec *tv = kc->args[swapped ? 0 : 1];
	const DateADT *d = dv->values;
	const Timestamp *t = tv->values;
	int			i;

	for (i = 0; i < kc->nrows; i++)
	{
		int32		c = date_cmp_timestamp_internal(d[vexec_arg_row(dv, i)],
												 t[vexec_arg_row(tv, i)]);

		if (swapped)
			c = -c;
		vexec_result_bit(kc->result, i, INT_CMP(c, 0, info->op));
	}
}

/* ---- the variants ---- */

/*
 * A temporal argument in PostgreSQL's epoch: a column kept in Arrow's is
 * shifted back into the work batch (§3.4.2).
 */
static void
pg_epoch(VexecKernelCall *kc, VexecVec **arg)
{
	VexecShape	to;
	VexecVec   *c;

	if (!(*arg)->shape.arrow_values)
		return;
	to = (*arg)->shape;
	to.arrow_values = false;
	c = vexec_batch_alloc(kc->work, sizeof(VexecVec));
	*c = **arg;
	if (!vexec_vec_convert(kc->work, c, &to))
		elog(ERROR, "vexec: a temporal column could not return to PostgreSQL's epoch");
	*arg = c;
}

/* A bool argument as a bit a row. */
static void
bool_as_bits(VexecKernelCall *kc, VexecVec **arg)
{
	VexecVec   *c;

	if ((*arg)->shape.layout == VEXEC_BIT_BOOL)
		return;
	c = vexec_batch_alloc(kc->work, sizeof(VexecVec));
	*c = **arg;
	if (!vexec_vec_convert(kc->work, c, &vexec_bool_bits))
		elog(ERROR, "vexec: a bool column could not become bits");
	*arg = c;
}

static VexecKernelFn
int_loop(int lw, int rw)
{
	static const VexecKernelFn loops[3][3] = {
		{cmp_i2_i2, cmp_i2_i4, cmp_i2_i8},
		{cmp_i4_i2, cmp_i4_i4, cmp_i4_i8},
		{cmp_i8_i2, cmp_i8_i4, cmp_i8_i8},
	};
	int			li = lw == 2 ? 0 : lw == 4 ? 1 : 2;
	int			ri = rw == 2 ? 0 : rw == 4 ? 1 : 2;

	return loops[li][ri];
}

static bool
cmp_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	const CmpInfo *info = kc->call->extra;
	VexecVec   *l = args[0];
	VexecVec   *r = args[1];

	v->result = vexec_bool_bits;
	switch (info->kind)
	{
		case CK_I2:
		case CK_I4:
		case CK_I8:
			if (l->shape.layout != VEXEC_FIXED || r->shape.layout != VEXEC_FIXED)
				return false;
			v->fn = int_loop(l->shape.width, r->shape.width);
			return true;
		case CK_F4:
		case CK_F8:
			if (l->shape.layout != VEXEC_FIXED || r->shape.layout != VEXEC_FIXED)
				return false;
			v->fn = l->shape.width == 4 ? (r->shape.width == 4 ? cmp_f4_f4 : cmp_f4_f8)
				: (r->shape.width == 4 ? cmp_f8_f4 : cmp_f8_f8);
			return true;
		case CK_OID:
			v->fn = cmp_oid;
			return l->shape.layout == VEXEC_FIXED && r->shape.layout == VEXEC_FIXED;
		case CK_CHAR:
			v->fn = cmp_char;
			return l->shape.layout == VEXEC_FIXED && r->shape.layout == VEXEC_FIXED;
		case CK_BOOL:
			bool_as_bits(kc, &args[0]);
			bool_as_bits(kc, &args[1]);
			v->fn = cmp_bool;
			return true;
		case CK_UUID:
			v->fn = cmp_uuid;
			return l->shape.layout == VEXEC_FIXED && r->shape.layout == VEXEC_FIXED;
		case CK_DATE:
		case CK_TS:
			/* one epoch: either side kept in Arrow's goes back to PostgreSQL's */
			if (l->shape.arrow_values != r->shape.arrow_values)
			{
				pg_epoch(kc, &args[0]);
				pg_epoch(kc, &args[1]);
			}
			v->fn = info->kind == CK_DATE ? cmp_i4_i4 : cmp_i8_i8;
			return true;
		case CK_DATE_TS:
		case CK_TS_DATE:
			pg_epoch(kc, &args[0]);
			pg_epoch(kc, &args[1]);
			v->fn = cmp_date_ts;
			return true;
	}
	return false;
}

static const VexecKernelDef cmp_def = {"compare", false, NULL, cmp_variant};

/* ---- the table ---- */

#define CMP(fid, op, kind) {fid, {op, kind}}

static const struct
{
	Oid			funcid;
	CmpInfo		info;
}			cmp_funcs[] =
{
	/* int2, int4, int8, every pair */
	CMP(F_INT2EQ, OP_EQ, CK_I2), CMP(F_INT2NE, OP_NE, CK_I2), CMP(F_INT2LT, OP_LT, CK_I2),
	CMP(F_INT2LE, OP_LE, CK_I2), CMP(F_INT2GT, OP_GT, CK_I2), CMP(F_INT2GE, OP_GE, CK_I2),
	CMP(F_INT4EQ, OP_EQ, CK_I4), CMP(F_INT4NE, OP_NE, CK_I4), CMP(F_INT4LT, OP_LT, CK_I4),
	CMP(F_INT4LE, OP_LE, CK_I4), CMP(F_INT4GT, OP_GT, CK_I4), CMP(F_INT4GE, OP_GE, CK_I4),
	CMP(F_INT8EQ, OP_EQ, CK_I8), CMP(F_INT8NE, OP_NE, CK_I8), CMP(F_INT8LT, OP_LT, CK_I8),
	CMP(F_INT8LE, OP_LE, CK_I8), CMP(F_INT8GT, OP_GT, CK_I8), CMP(F_INT8GE, OP_GE, CK_I8),
	CMP(F_INT24EQ, OP_EQ, CK_I4), CMP(F_INT24NE, OP_NE, CK_I4), CMP(F_INT24LT, OP_LT, CK_I4),
	CMP(F_INT24LE, OP_LE, CK_I4), CMP(F_INT24GT, OP_GT, CK_I4), CMP(F_INT24GE, OP_GE, CK_I4),
	CMP(F_INT42EQ, OP_EQ, CK_I4), CMP(F_INT42NE, OP_NE, CK_I4), CMP(F_INT42LT, OP_LT, CK_I4),
	CMP(F_INT42LE, OP_LE, CK_I4), CMP(F_INT42GT, OP_GT, CK_I4), CMP(F_INT42GE, OP_GE, CK_I4),
	CMP(F_INT28EQ, OP_EQ, CK_I8), CMP(F_INT28NE, OP_NE, CK_I8), CMP(F_INT28LT, OP_LT, CK_I8),
	CMP(F_INT28LE, OP_LE, CK_I8), CMP(F_INT28GT, OP_GT, CK_I8), CMP(F_INT28GE, OP_GE, CK_I8),
	CMP(F_INT82EQ, OP_EQ, CK_I8), CMP(F_INT82NE, OP_NE, CK_I8), CMP(F_INT82LT, OP_LT, CK_I8),
	CMP(F_INT82LE, OP_LE, CK_I8), CMP(F_INT82GT, OP_GT, CK_I8), CMP(F_INT82GE, OP_GE, CK_I8),
	CMP(F_INT48EQ, OP_EQ, CK_I8), CMP(F_INT48NE, OP_NE, CK_I8), CMP(F_INT48LT, OP_LT, CK_I8),
	CMP(F_INT48LE, OP_LE, CK_I8), CMP(F_INT48GT, OP_GT, CK_I8), CMP(F_INT48GE, OP_GE, CK_I8),
	CMP(F_INT84EQ, OP_EQ, CK_I8), CMP(F_INT84NE, OP_NE, CK_I8), CMP(F_INT84LT, OP_LT, CK_I8),
	CMP(F_INT84LE, OP_LE, CK_I8), CMP(F_INT84GT, OP_GT, CK_I8), CMP(F_INT84GE, OP_GE, CK_I8),
	/* float4, float8, every pair */
	CMP(F_FLOAT4EQ, OP_EQ, CK_F4), CMP(F_FLOAT4NE, OP_NE, CK_F4), CMP(F_FLOAT4LT, OP_LT, CK_F4),
	CMP(F_FLOAT4LE, OP_LE, CK_F4), CMP(F_FLOAT4GT, OP_GT, CK_F4), CMP(F_FLOAT4GE, OP_GE, CK_F4),
	CMP(F_FLOAT8EQ, OP_EQ, CK_F8), CMP(F_FLOAT8NE, OP_NE, CK_F8), CMP(F_FLOAT8LT, OP_LT, CK_F8),
	CMP(F_FLOAT8LE, OP_LE, CK_F8), CMP(F_FLOAT8GT, OP_GT, CK_F8), CMP(F_FLOAT8GE, OP_GE, CK_F8),
	CMP(F_FLOAT48EQ, OP_EQ, CK_F8), CMP(F_FLOAT48NE, OP_NE, CK_F8), CMP(F_FLOAT48LT, OP_LT, CK_F8),
	CMP(F_FLOAT48LE, OP_LE, CK_F8), CMP(F_FLOAT48GT, OP_GT, CK_F8), CMP(F_FLOAT48GE, OP_GE, CK_F8),
	CMP(F_FLOAT84EQ, OP_EQ, CK_F8), CMP(F_FLOAT84NE, OP_NE, CK_F8), CMP(F_FLOAT84LT, OP_LT, CK_F8),
	CMP(F_FLOAT84LE, OP_LE, CK_F8), CMP(F_FLOAT84GT, OP_GT, CK_F8), CMP(F_FLOAT84GE, OP_GE, CK_F8),
	/* oid, "char", bool, uuid */
	CMP(F_OIDEQ, OP_EQ, CK_OID), CMP(F_OIDNE, OP_NE, CK_OID), CMP(F_OIDLT, OP_LT, CK_OID),
	CMP(F_OIDLE, OP_LE, CK_OID), CMP(F_OIDGT, OP_GT, CK_OID), CMP(F_OIDGE, OP_GE, CK_OID),
	CMP(F_CHAREQ, OP_EQ, CK_CHAR), CMP(F_CHARNE, OP_NE, CK_CHAR), CMP(F_CHARLT, OP_LT, CK_CHAR),
	CMP(F_CHARLE, OP_LE, CK_CHAR), CMP(F_CHARGT, OP_GT, CK_CHAR), CMP(F_CHARGE, OP_GE, CK_CHAR),
	CMP(F_BOOLEQ, OP_EQ, CK_BOOL), CMP(F_BOOLNE, OP_NE, CK_BOOL), CMP(F_BOOLLT, OP_LT, CK_BOOL),
	CMP(F_BOOLLE, OP_LE, CK_BOOL), CMP(F_BOOLGT, OP_GT, CK_BOOL), CMP(F_BOOLGE, OP_GE, CK_BOOL),
	CMP(F_UUID_EQ, OP_EQ, CK_UUID), CMP(F_UUID_NE, OP_NE, CK_UUID), CMP(F_UUID_LT, OP_LT, CK_UUID),
	CMP(F_UUID_LE, OP_LE, CK_UUID), CMP(F_UUID_GT, OP_GT, CK_UUID), CMP(F_UUID_GE, OP_GE, CK_UUID),
	/* date, timestamp, timestamptz, and date with timestamp */
	CMP(F_DATE_EQ, OP_EQ, CK_DATE), CMP(F_DATE_NE, OP_NE, CK_DATE), CMP(F_DATE_LT, OP_LT, CK_DATE),
	CMP(F_DATE_LE, OP_LE, CK_DATE), CMP(F_DATE_GT, OP_GT, CK_DATE), CMP(F_DATE_GE, OP_GE, CK_DATE),
	CMP(F_TIMESTAMP_EQ, OP_EQ, CK_TS), CMP(F_TIMESTAMP_NE, OP_NE, CK_TS),
	CMP(F_TIMESTAMP_LT, OP_LT, CK_TS), CMP(F_TIMESTAMP_LE, OP_LE, CK_TS),
	CMP(F_TIMESTAMP_GT, OP_GT, CK_TS), CMP(F_TIMESTAMP_GE, OP_GE, CK_TS),
	CMP(F_TIMESTAMPTZ_EQ, OP_EQ, CK_TS), CMP(F_TIMESTAMPTZ_NE, OP_NE, CK_TS),
	CMP(F_TIMESTAMPTZ_LT, OP_LT, CK_TS), CMP(F_TIMESTAMPTZ_LE, OP_LE, CK_TS),
	CMP(F_TIMESTAMPTZ_GT, OP_GT, CK_TS), CMP(F_TIMESTAMPTZ_GE, OP_GE, CK_TS),
	CMP(F_DATE_EQ_TIMESTAMP, OP_EQ, CK_DATE_TS), CMP(F_DATE_NE_TIMESTAMP, OP_NE, CK_DATE_TS),
	CMP(F_DATE_LT_TIMESTAMP, OP_LT, CK_DATE_TS), CMP(F_DATE_LE_TIMESTAMP, OP_LE, CK_DATE_TS),
	CMP(F_DATE_GT_TIMESTAMP, OP_GT, CK_DATE_TS), CMP(F_DATE_GE_TIMESTAMP, OP_GE, CK_DATE_TS),
	CMP(F_TIMESTAMP_EQ_DATE, OP_EQ, CK_TS_DATE), CMP(F_TIMESTAMP_NE_DATE, OP_NE, CK_TS_DATE),
	CMP(F_TIMESTAMP_LT_DATE, OP_LT, CK_TS_DATE), CMP(F_TIMESTAMP_LE_DATE, OP_LE, CK_TS_DATE),
	CMP(F_TIMESTAMP_GT_DATE, OP_GT, CK_TS_DATE), CMP(F_TIMESTAMP_GE_DATE, OP_GE, CK_TS_DATE),
};

void
vexec_kernels_compare(void (*add) (Oid, const VexecKernelDef *, const void *))
{
	int			i;

	for (i = 0; i < lengthof(cmp_funcs); i++)
		add(cmp_funcs[i].funcid, &cmp_def, &cmp_funcs[i].info);
}
