/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * k_arith.c
 *	  Checked arithmetic over integers and floats, and the casts between
 *	  them (pg_vector_executor.md §3.7, "The first kernel set").
 *
 * Integers.  For every pair of int2, int4 and int8, PostgreSQL's +, -, *,
 * / and % give the exact result -- C's truncating division and its
 * remainder -- and raise where the divisor is zero or the result does not
 * fit the function's result type, INT_MIN / -1 among them
 * (PG19:src/backend/utils/adt/int.c, int8.c).  The kernels compute the
 * exact result in 128 bits and mark the rows PostgreSQL would raise on;
 * unary minus and abs likewise.  The casts that narrow mark the values that
 * do not fit, as int84() and its kind raise "out of range".
 *
 * Floats.  float.h's float4_pl ... float8_div decide PostgreSQL's errors
 * (PG19:src/include/utils/float.h): an infinite result from finite inputs
 * overflows, a zero result from nonzero inputs underflows (for * and /),
 * and a zero divisor raises unless the dividend is NaN.  The kernels test
 * the same conditions, in the same precision -- float4's in float4, the
 * cross-type ones in float8 -- and mark those rows.  float8 to float4 is
 * dtof()'s, the conversions to integers dtoi4()'s and its kind's: rint(),
 * then the range of the integer (float.c, int8.c).
 *
 * Every row of the batch is computed, a divisor tested before it divides;
 * the evaluator keeps only the failures of the active rows.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "common/int.h"
#include "utils/float.h"
#include "utils/fmgroids.h"

#include "vexec.h"
#include "expr/kernel.h"

typedef enum ArithOp
{
	AO_ADD,
	AO_SUB,
	AO_MUL,
	AO_DIV,
	AO_MOD,
	AO_NEG,
	AO_ABS,
	AO_CAST						/* to the result's type */
} ArithOp;

typedef enum ArithKind
{
	AK_INT,						/* integer operands, integer result */
	AK_FLOAT4,					/* float4 operands, in float4 */
	AK_FLOAT8,					/* float operands, in float8 */
	AK_INT_TO_FLOAT,			/* int2/4/8 to float4/8 */
	AK_FLOAT_TO_INT,			/* float4/8 to int2/4/8 */
	AK_FLOAT_TO_FLOAT			/* float4 <-> float8 */
} ArithKind;

typedef struct ArithInfo
{
	uint8		op;
	uint8		kind;
	uint8		outwidth;		/* the result type's bytes */
} ArithInfo;

/* An integer argument's values as int64, a constant repeated. */
static int64 *
load_int(VexecKernelCall *kc, const VexecVec *v)
{
	int			n = kc->nrows;
	int64	   *out = vexec_batch_alloc(kc->work, sizeof(int64) * Max(n, 1));
	int			i;

	if (v->encoding == VEXEC_CONST)
	{
		int64		c = v->shape.width == 2 ? ((const int16 *) v->values)[0] :
			v->shape.width == 4 ? ((const int32 *) v->values)[0] :
			((const int64 *) v->values)[0];

		for (i = 0; i < n; i++)
			out[i] = c;
		return out;
	}
	switch (v->shape.width)
	{
		case 2:
			for (i = 0; i < n; i++)
				out[i] = ((const int16 *) v->values)[i];
			break;
		case 4:
			for (i = 0; i < n; i++)
				out[i] = ((const int32 *) v->values)[i];
			break;
		default:
			memcpy(out, v->values, sizeof(int64) * n);
			break;
	}
	return out;
}

/* A float argument's values as float8, a constant repeated. */
static float8 *
load_float(VexecKernelCall *kc, const VexecVec *v)
{
	int			n = kc->nrows;
	float8	   *out = vexec_batch_alloc(kc->work, sizeof(float8) * Max(n, 1));
	int			i;

	if (v->encoding == VEXEC_CONST)
	{
		float8		c = v->shape.width == 4 ? ((const float4 *) v->values)[0]
			: ((const float8 *) v->values)[0];

		for (i = 0; i < n; i++)
			out[i] = c;
		return out;
	}
	if (v->shape.width == 4)
		for (i = 0; i < n; i++)
			out[i] = ((const float4 *) v->values)[i];
	else
		memcpy(out, v->values, sizeof(float8) * n);
	return out;
}

static float4 *
load_float4(VexecKernelCall *kc, const VexecVec *v)
{
	int			n = kc->nrows;
	float4	   *out = vexec_batch_alloc(kc->work, sizeof(float4) * Max(n, 1));
	int			i;

	if (v->encoding == VEXEC_CONST)
	{
		for (i = 0; i < n; i++)
			out[i] = ((const float4 *) v->values)[0];
		return out;
	}
	memcpy(out, v->values, sizeof(float4) * n);
	return out;
}

/* Store an exact integer result in the result's width, or mark it. */
static inline void
store_int(VexecKernelCall *kc, int i, int128 r, int width)
{
	switch (width)
	{
		case 2:
			if (r < PG_INT16_MIN || r > PG_INT16_MAX)
				vexec_fail(kc, i);
			else
				((int16 *) kc->result->values)[i] = (int16) r;
			break;
		case 4:
			if (r < PG_INT32_MIN || r > PG_INT32_MAX)
				vexec_fail(kc, i);
			else
				((int32 *) kc->result->values)[i] = (int32) r;
			break;
		default:
			if (r < PG_INT64_MIN || r > PG_INT64_MAX)
				vexec_fail(kc, i);
			else
				((int64 *) kc->result->values)[i] = (int64) r;
			break;
	}
}

static void
int_arith(VexecKernelCall *kc)
{
	const ArithInfo *info = kc->call->extra;
	int			n = kc->nrows;
	int			width = info->outwidth;
	int64	   *a = load_int(kc, kc->args[0]);
	int64	   *b = kc->call->nargs > 1 ? load_int(kc, kc->args[1]) : NULL;
	int			i;

	switch (info->op)
	{
		case AO_ADD:
			for (i = 0; i < n; i++)
				store_int(kc, i, (int128) a[i] + b[i], width);
			break;
		case AO_SUB:
			for (i = 0; i < n; i++)
				store_int(kc, i, (int128) a[i] - b[i], width);
			break;
		case AO_MUL:
			for (i = 0; i < n; i++)
				store_int(kc, i, (int128) a[i] * b[i], width);
			break;
		case AO_DIV:
			for (i = 0; i < n; i++)
			{
				if (b[i] == 0)
					vexec_fail(kc, i);
				else
					store_int(kc, i, (int128) a[i] / b[i], width);
			}
			break;
		case AO_MOD:
			for (i = 0; i < n; i++)
			{
				if (b[i] == 0)
					vexec_fail(kc, i);
				else
					store_int(kc, i, (int128) a[i] % b[i], width);
			}
			break;
		case AO_NEG:
			for (i = 0; i < n; i++)
				store_int(kc, i, -(int128) a[i], width);
			break;
		case AO_ABS:
			for (i = 0; i < n; i++)
				store_int(kc, i, a[i] < 0 ? -(int128) a[i] : (int128) a[i], width);
			break;
		default:				/* AO_CAST */
			for (i = 0; i < n; i++)
				store_int(kc, i, (int128) a[i], width);
			break;
	}
}

/* float8: float8_pl() and its kind's conditions (float.h) */
static void
float8_arith(VexecKernelCall *kc)
{
	const ArithInfo *info = kc->call->extra;
	int			n = kc->nrows;
	float8	   *a = load_float(kc, kc->args[0]);
	float8	   *b = kc->call->nargs > 1 ? load_float(kc, kc->args[1]) : NULL;
	float8	   *out = kc->result->values;
	int			i;

	for (i = 0; i < n; i++)
	{
		float8		x = a[i];
		float8		y = b ? b[i] : 0;
		float8		r;

		switch (info->op)
		{
			case AO_ADD:
				r = x + y;
				if (unlikely(isinf(r)) && !isinf(x) && !isinf(y))
					vexec_fail(kc, i);
				break;
			case AO_SUB:
				r = x - y;
				if (unlikely(isinf(r)) && !isinf(x) && !isinf(y))
					vexec_fail(kc, i);
				break;
			case AO_MUL:
				r = x * y;
				if ((unlikely(isinf(r)) && !isinf(x) && !isinf(y)) ||
					(unlikely(r == 0.0) && x != 0.0 && y != 0.0))
					vexec_fail(kc, i);
				break;
			case AO_DIV:
				if (unlikely(y == 0.0) && !isnan(x))
				{
					vexec_fail(kc, i);
					r = 0;
					break;
				}
				r = x / y;
				if ((unlikely(isinf(r)) && !isinf(x)) ||
					(unlikely(r == 0.0) && x != 0.0 && !isinf(y)))
					vexec_fail(kc, i);
				break;
			default:			/* AO_NEG */
				r = -x;
				break;
		}
		out[i] = r;
	}
}

/* float4: float4_pl() and its kind's, in float4 */
static void
float4_arith(VexecKernelCall *kc)
{
	const ArithInfo *info = kc->call->extra;
	int			n = kc->nrows;
	float4	   *a = load_float4(kc, kc->args[0]);
	float4	   *b = kc->call->nargs > 1 ? load_float4(kc, kc->args[1]) : NULL;
	float4	   *out = kc->result->values;
	int			i;

	for (i = 0; i < n; i++)
	{
		float4		x = a[i];
		float4		y = b ? b[i] : 0;
		float4		r;

		switch (info->op)
		{
			case AO_ADD:
				r = x + y;
				if (unlikely(isinf(r)) && !isinf(x) && !isinf(y))
					vexec_fail(kc, i);
				break;
			case AO_SUB:
				r = x - y;
				if (unlikely(isinf(r)) && !isinf(x) && !isinf(y))
					vexec_fail(kc, i);
				break;
			case AO_MUL:
				r = x * y;
				if ((unlikely(isinf(r)) && !isinf(x) && !isinf(y)) ||
					(unlikely(r == 0.0f) && x != 0.0f && y != 0.0f))
					vexec_fail(kc, i);
				break;
			case AO_DIV:
				if (unlikely(y == 0.0f) && !isnan(x))
				{
					vexec_fail(kc, i);
					r = 0;
					break;
				}
				r = x / y;
				if ((unlikely(isinf(r)) && !isinf(x)) ||
					(unlikely(r == 0.0f) && x != 0.0f && !isinf(y)))
					vexec_fail(kc, i);
				break;
			default:			/* AO_NEG */
				r = -x;
				break;
		}
		out[i] = r;
	}
}

/* int2/4/8 to float4/8: the C conversion, as i4tod() and its kind's */
static void
int_to_float(VexecKernelCall *kc)
{
	const ArithInfo *info = kc->call->extra;
	int64	   *a = load_int(kc, kc->args[0]);
	int			i;

	if (info->outwidth == 8)
		for (i = 0; i < kc->nrows; i++)
			((float8 *) kc->result->values)[i] = (float8) a[i];
	else
		for (i = 0; i < kc->nrows; i++)
			((float4 *) kc->result->values)[i] = (float4) a[i];
}

/* float4/8 to int2/4/8: rint(), then the integer's range (dtoi4, ftoi4) */
static void
float_to_int(VexecKernelCall *kc)
{
	const ArithInfo *info = kc->call->extra;
	const VexecVec *v = kc->args[0];
	int			i;

	for (i = 0; i < kc->nrows; i++)
	{
		int			row = vexec_arg_row(v, i);
		bool		fits;

		if (v->shape.width == 4)
		{
			float4		num = rint(((const float4 *) v->values)[row]);

			fits = !isnan(num) &&
				(info->outwidth == 2 ? FLOAT4_FITS_IN_INT16(num) :
				 info->outwidth == 4 ? FLOAT4_FITS_IN_INT32(num) : FLOAT4_FITS_IN_INT64(num));
			if (!fits)
				vexec_fail(kc, i);
			else
				store_int(kc, i, (int128) (int64) num, info->outwidth);
		}
		else
		{
			float8		num = rint(((const float8 *) v->values)[row]);

			fits = !isnan(num) &&
				(info->outwidth == 2 ? FLOAT8_FITS_IN_INT16(num) :
				 info->outwidth == 4 ? FLOAT8_FITS_IN_INT32(num) : FLOAT8_FITS_IN_INT64(num));
			if (!fits)
				vexec_fail(kc, i);
			else
				store_int(kc, i, (int128) (int64) num, info->outwidth);
		}
	}
}

/* float4 to float8 widens; float8 to float4 is dtof()'s, with its checks */
static void
float_to_float(VexecKernelCall *kc)
{
	const ArithInfo *info = kc->call->extra;
	const VexecVec *v = kc->args[0];
	int			i;

	for (i = 0; i < kc->nrows; i++)
	{
		int			row = vexec_arg_row(v, i);

		if (info->outwidth == 8)
			((float8 *) kc->result->values)[i] = ((const float4 *) v->values)[row];
		else
		{
			float8		num = ((const float8 *) v->values)[row];
			float4		r = (float4) num;

			if ((unlikely(isinf(r)) && !isinf(num)) ||
				(unlikely(r == 0.0f) && num != 0.0))
				vexec_fail(kc, i);
			((float4 *) kc->result->values)[i] = r;
		}
	}
}

static bool
arith_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	const ArithInfo *info = kc->call->extra;
	int			i;

	for (i = 0; i < kc->nargs; i++)
		if (args[i]->shape.layout != VEXEC_FIXED)
			return false;
	memset(&v->result, 0, sizeof(VexecShape));
	v->result.layout = VEXEC_FIXED;
	v->result.width = info->outwidth;
	v->result.stride = info->outwidth;
	switch (info->kind)
	{
		case AK_INT:
			v->fn = int_arith;
			break;
		case AK_FLOAT4:
			v->fn = float4_arith;
			break;
		case AK_FLOAT8:
			v->fn = float8_arith;
			break;
		case AK_INT_TO_FLOAT:
			v->fn = int_to_float;
			break;
		case AK_FLOAT_TO_INT:
			v->fn = float_to_int;
			break;
		default:
			v->fn = float_to_float;
			break;
	}
	return true;
}

static const VexecKernelDef arith_fail_def = {"arithmetic", true, NULL, arith_variant};
static const VexecKernelDef arith_def = {"arithmetic", false, NULL, arith_variant};

#define A(fid, op, kind, w) {fid, {op, kind, w}}

static const struct
{
	Oid			funcid;
	ArithInfo	info;
}			arith_funcs[] =
{
	/* int2 op int2 -> int2; int4 and the mixed int2/int4 -> int4; int8 and its mixes -> int8 */
	A(F_INT2PL, AO_ADD, AK_INT, 2), A(F_INT2MI, AO_SUB, AK_INT, 2), A(F_INT2MUL, AO_MUL, AK_INT, 2),
	A(F_INT2DIV, AO_DIV, AK_INT, 2), A(F_INT2MOD, AO_MOD, AK_INT, 2),
	A(F_INT4PL, AO_ADD, AK_INT, 4), A(F_INT4MI, AO_SUB, AK_INT, 4), A(F_INT4MUL, AO_MUL, AK_INT, 4),
	A(F_INT4DIV, AO_DIV, AK_INT, 4), A(F_INT4MOD, AO_MOD, AK_INT, 4),
	A(F_INT24PL, AO_ADD, AK_INT, 4), A(F_INT24MI, AO_SUB, AK_INT, 4), A(F_INT24MUL, AO_MUL, AK_INT, 4),
	A(F_INT24DIV, AO_DIV, AK_INT, 4),
	A(F_INT42PL, AO_ADD, AK_INT, 4), A(F_INT42MI, AO_SUB, AK_INT, 4), A(F_INT42MUL, AO_MUL, AK_INT, 4),
	A(F_INT42DIV, AO_DIV, AK_INT, 4),
	A(F_INT8PL, AO_ADD, AK_INT, 8), A(F_INT8MI, AO_SUB, AK_INT, 8), A(F_INT8MUL, AO_MUL, AK_INT, 8),
	A(F_INT8DIV, AO_DIV, AK_INT, 8), A(F_INT8MOD, AO_MOD, AK_INT, 8),
	A(F_INT48PL, AO_ADD, AK_INT, 8), A(F_INT48MI, AO_SUB, AK_INT, 8), A(F_INT48MUL, AO_MUL, AK_INT, 8),
	A(F_INT48DIV, AO_DIV, AK_INT, 8),
	A(F_INT84PL, AO_ADD, AK_INT, 8), A(F_INT84MI, AO_SUB, AK_INT, 8), A(F_INT84MUL, AO_MUL, AK_INT, 8),
	A(F_INT84DIV, AO_DIV, AK_INT, 8),
	A(F_INT28PL, AO_ADD, AK_INT, 8), A(F_INT28MI, AO_SUB, AK_INT, 8), A(F_INT28MUL, AO_MUL, AK_INT, 8),
	A(F_INT28DIV, AO_DIV, AK_INT, 8),
	A(F_INT82PL, AO_ADD, AK_INT, 8), A(F_INT82MI, AO_SUB, AK_INT, 8), A(F_INT82MUL, AO_MUL, AK_INT, 8),
	A(F_INT82DIV, AO_DIV, AK_INT, 8),
	A(F_INT2UM, AO_NEG, AK_INT, 2), A(F_INT4UM, AO_NEG, AK_INT, 4), A(F_INT8UM, AO_NEG, AK_INT, 8),
	A(F_INT2ABS, AO_ABS, AK_INT, 2), A(F_INT4ABS, AO_ABS, AK_INT, 4), A(F_INT8ABS, AO_ABS, AK_INT, 8),
	/* integer casts: the narrowing ones can fail */
	A(F_INT2_INT4, AO_CAST, AK_INT, 2), A(F_INT2_INT8, AO_CAST, AK_INT, 2),
	A(F_INT4_INT8, AO_CAST, AK_INT, 4),
	/* floats */
	A(F_FLOAT4PL, AO_ADD, AK_FLOAT4, 4), A(F_FLOAT4MI, AO_SUB, AK_FLOAT4, 4),
	A(F_FLOAT4MUL, AO_MUL, AK_FLOAT4, 4), A(F_FLOAT4DIV, AO_DIV, AK_FLOAT4, 4),
	A(F_FLOAT8PL, AO_ADD, AK_FLOAT8, 8), A(F_FLOAT8MI, AO_SUB, AK_FLOAT8, 8),
	A(F_FLOAT8MUL, AO_MUL, AK_FLOAT8, 8), A(F_FLOAT8DIV, AO_DIV, AK_FLOAT8, 8),
	A(F_FLOAT48PL, AO_ADD, AK_FLOAT8, 8), A(F_FLOAT48MI, AO_SUB, AK_FLOAT8, 8),
	A(F_FLOAT48MUL, AO_MUL, AK_FLOAT8, 8), A(F_FLOAT48DIV, AO_DIV, AK_FLOAT8, 8),
	A(F_FLOAT84PL, AO_ADD, AK_FLOAT8, 8), A(F_FLOAT84MI, AO_SUB, AK_FLOAT8, 8),
	A(F_FLOAT84MUL, AO_MUL, AK_FLOAT8, 8), A(F_FLOAT84DIV, AO_DIV, AK_FLOAT8, 8),
	A(F_FLOAT4_FLOAT8, AO_CAST, AK_FLOAT_TO_FLOAT, 4),
	A(F_INT2_FLOAT4, AO_CAST, AK_FLOAT_TO_INT, 2), A(F_INT2_FLOAT8, AO_CAST, AK_FLOAT_TO_INT, 2),
	A(F_INT4_FLOAT4, AO_CAST, AK_FLOAT_TO_INT, 4), A(F_INT4_FLOAT8, AO_CAST, AK_FLOAT_TO_INT, 4),
	A(F_INT8_FLOAT4, AO_CAST, AK_FLOAT_TO_INT, 8), A(F_INT8_FLOAT8, AO_CAST, AK_FLOAT_TO_INT, 8),
};

/* The ones that cannot fail. */
static const struct
{
	Oid			funcid;
	ArithInfo	info;
}			safe_funcs[] =
{
	A(F_INT4_INT2, AO_CAST, AK_INT, 4), A(F_INT8_INT2, AO_CAST, AK_INT, 8),
	A(F_INT8_INT4, AO_CAST, AK_INT, 8),
	A(F_FLOAT4UM, AO_NEG, AK_FLOAT4, 4), A(F_FLOAT8UM, AO_NEG, AK_FLOAT8, 8),
	A(F_FLOAT8_FLOAT4, AO_CAST, AK_FLOAT_TO_FLOAT, 8),
	A(F_FLOAT4_INT2, AO_CAST, AK_INT_TO_FLOAT, 4), A(F_FLOAT4_INT4, AO_CAST, AK_INT_TO_FLOAT, 4),
	A(F_FLOAT4_INT8, AO_CAST, AK_INT_TO_FLOAT, 4),
	A(F_FLOAT8_INT2, AO_CAST, AK_INT_TO_FLOAT, 8), A(F_FLOAT8_INT4, AO_CAST, AK_INT_TO_FLOAT, 8),
	A(F_FLOAT8_INT8, AO_CAST, AK_INT_TO_FLOAT, 8),
};

void
vexec_kernels_arith(void (*add) (Oid, const VexecKernelDef *, const void *))
{
	int			i;

	for (i = 0; i < lengthof(arith_funcs); i++)
		add(arith_funcs[i].funcid, &arith_fail_def, &arith_funcs[i].info);
	for (i = 0; i < lengthof(safe_funcs); i++)
		add(safe_funcs[i].funcid, &arith_def, &safe_funcs[i].info);
}
