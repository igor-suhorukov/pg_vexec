/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * k_numeric.c
 *	  Kernels over numeric in the scaled layout (pg_vector_executor.md
 *	  §3.4.1, §3.7): comparisons, +, -, *, unary minus, and the casts from
 *	  integers.
 *
 * A scaled numeric is an integer v at a scale s, standing for v / 10^s,
 * every value of the column at the same display scale (batch/numeric.c).
 * The kernels follow numeric.c's rules (PG19:src/backend/utils/adt/
 * numeric.c):
 *
 *	compare		by value, whatever the display scales: 1.0 = 1.00.  Two
 *				scales are aligned by multiplying the one with fewer digits
 *				after the point; where that product leaves 128 bits it is
 *				larger in magnitude than any value the layout holds, so its
 *				sign decides.
 *	+, -		exact, at the larger display scale (add_var, sub_var)
 *	*			exact, at the sum of the display scales (numeric_mul asks
 *				mul_var for it), where that is 38 at most
 *	-x			exact, at x's scale
 *	int -> numeric	the integer at scale 0 (int64_to_numeric)
 *
 * A result the layout cannot hold -- 38 digits or more -- is not an error
 * of PostgreSQL's: the row is marked, and PostgreSQL's evaluator computes
 * it, so these kernels are of those that can fail.  A batch whose column
 * holds NaN, an infinity or another scale is in the varlena layout (§3.4.1),
 * and has no variant here: comparisons call numeric's own functions a row
 * at a time, arithmetic sends its rows to PostgreSQL's evaluator.
 *
 * A constant argument is taken at its own display scale.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "utils/fmgroids.h"

#include "vexec.h"
#include "expr/kernel.h"

typedef enum NumOp
{
	NO_EQ,
	NO_NE,
	NO_LT,
	NO_LE,
	NO_GT,
	NO_GE,
	NO_ADD,
	NO_SUB,
	NO_MUL,
	NO_NEG,
	NO_FROM_INT
} NumOp;

typedef struct NumInfo
{
	uint8		op;
} NumInfo;

/* 10^n, 0 <= n <= 38 */
static int128
pow10_128(int n)
{
	int128		r = 1;

	while (n-- > 0)
		r *= 10;
	return r;
}

static int128 pow10_table[39];
static bool pow10_ready = false;

static inline int128
p10(int n)
{
	if (!pow10_ready)
	{
		for (int i = 0; i <= 38; i++)
			pow10_table[i] = pow10_128(i);
		pow10_ready = true;
	}
	return pow10_table[n];
}

/* The largest magnitude the scaled layout holds: 38 digits. */
#define SCALED_LIMIT	(p10(38))

static inline int128
scaled_at(const VexecVec *v, int row)
{
	row = vexec_arg_row(v, row);
	if (v->shape.width == 8)
		return ((const int64 *) v->values)[row];
	else
	{
		int128		x;

		memcpy(&x, (const char *) v->values + (Size) row * 16, sizeof(int128));
		return x;
	}
}

/*
 * a / 10^sa against b / 10^sb: the one with fewer digits after the point
 * multiplied up; a product past 128 bits outweighs every scaled value.
 */
static inline int
cmp_scaled(int128 a, int sa, int128 b, int sb)
{
	int128		x;

	if (sa < sb)
	{
		if (__builtin_mul_overflow(a, p10(sb - sa), &x))
			return a > 0 ? 1 : -1;
		a = x;
	}
	else if (sb < sa)
	{
		if (__builtin_mul_overflow(b, p10(sa - sb), &x))
			return b > 0 ? -1 : 1;
		b = x;
	}
	return a < b ? -1 : a > b ? 1 : 0;
}

static void
num_compare(VexecKernelCall *kc)
{
	const NumInfo *info = kc->call->extra;
	const VexecVec *l = kc->args[0];
	const VexecVec *r = kc->args[1];
	int			sa = l->shape.scale;
	int			sb = r->shape.scale;

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		int			c = cmp_scaled(scaled_at(l, i), sa, scaled_at(r, i), sb);
		bool		res;

		switch (info->op)
		{
			case NO_EQ:
				res = c == 0;
				break;
			case NO_NE:
				res = c != 0;
				break;
			case NO_LT:
				res = c < 0;
				break;
			case NO_LE:
				res = c <= 0;
				break;
			case NO_GT:
				res = c > 0;
				break;
			default:
				res = c >= 0;
				break;
		}
		vexec_result_bit(kc->result, i, res);
	}
}

static inline void
store16(VexecKernelCall *kc, int i, int128 x)
{
	if (x >= SCALED_LIMIT || x <= -SCALED_LIMIT)
		vexec_fail(kc, i);
	else
		memcpy((char *) kc->result->values + (Size) i * 16, &x, sizeof(int128));
}

static void
num_arith(VexecKernelCall *kc)
{
	const NumInfo *info = kc->call->extra;
	const VexecVec *l = kc->args[0];
	const VexecVec *r = kc->nargs > 1 ? kc->args[1] : NULL;
	int			sa = l->shape.scale;
	int			sb = r ? r->shape.scale : 0;
	int			s = Max(sa, sb);

	VEXEC_FOREACH_ROW(kc->active, kc->nrows, i)
	{
		int128		a = scaled_at(l, i);
		int128		b = r ? scaled_at(r, i) : 0;
		int128		x;
		bool		over = false;

		switch (info->op)
		{
			case NO_ADD:
			case NO_SUB:
				over = __builtin_mul_overflow(a, p10(s - sa), &a) ||
					__builtin_mul_overflow(b, p10(s - sb), &b);
				if (!over)
					over = info->op == NO_ADD ? __builtin_add_overflow(a, b, &x)
						: __builtin_sub_overflow(a, b, &x);
				break;
			case NO_MUL:
				over = __builtin_mul_overflow(a, b, &x);
				break;
			default:			/* NO_NEG */
				x = -a;
				break;
		}
		if (over)
			vexec_fail(kc, i);
		else
			store16(kc, i, x);
	}
}

static void
num_from_int(VexecKernelCall *kc)
{
	const VexecVec *v = kc->args[0];
	int			i;

	for (i = 0; i < kc->nrows; i++)
	{
		int			row = vexec_arg_row(v, i);
		int64		x = v->shape.width == 2 ? ((const int16 *) v->values)[row] :
			v->shape.width == 4 ? ((const int32 *) v->values)[row] :
			((const int64 *) v->values)[row];

		if (kc->result->shape.width == 8)
			((int64 *) kc->result->values)[i] = x;
		else
		{
			int128		y = x;

			memcpy((char *) kc->result->values + (Size) i * 16, &y, sizeof(int128));
		}
	}
}

/*
 * A numeric argument in the scaled layout: a column already in it, or a
 * constant at its own display scale.  False for anything else.
 */
static bool
as_scaled(VexecKernelCall *kc, VexecVec **arg)
{
	VexecVec   *v = *arg;
	VexecVec   *c;
	int			scale;
	int128		x;
	bool		isnull;
	Datum		d;

	if (v->shape.layout == VEXEC_SCALED)
		return true;
	if (v->encoding != VEXEC_CONST || v->shape.layout != VEXEC_DATUM)
		return false;
	d = vexec_vec_datum(kc->work, v, 0, &isnull);
	if (isnull)
		return true;			/* a NULL constant: no row computes */
	scale = vexec_numeric_dscale(d);
	if (scale < 0 || scale > 38 || !vexec_numeric_to_scaled(d, scale, 38, 16, &x))
		return false;
	c = vexec_batch_alloc0(kc->work, sizeof(VexecVec));
	c->type = v->type;
	c->shape.layout = VEXEC_SCALED;
	c->shape.width = 16;
	c->shape.stride = 16;
	c->shape.scale = scale;
	c->encoding = VEXEC_CONST;
	c->nvalues = 1;
	c->values = vexec_batch_alloc(kc->work, 16);
	memcpy(c->values, &x, 16);
	*arg = c;
	return true;
}

static bool
num_variant(VexecKernelCall *kc, VexecVec **args, VexecVariant *v)
{
	const NumInfo *info = kc->call->extra;
	int			i;

	memset(&v->result, 0, sizeof(VexecShape));
	if (info->op == NO_FROM_INT)
	{
		if (args[0]->shape.layout != VEXEC_FIXED)
			return false;
		v->result.layout = VEXEC_SCALED;
		v->result.width = args[0]->shape.width == 8 ? 16 : 8;
		v->result.stride = v->result.width;
		v->result.scale = 0;
		v->fn = num_from_int;
		return true;
	}
	for (i = 0; i < kc->nargs; i++)
		if (!as_scaled(kc, &args[i]))
			return false;
	if (info->op <= NO_GE)
	{
		v->result = vexec_bool_bits;
		v->fn = num_compare;
		return true;
	}
	v->result.layout = VEXEC_SCALED;
	v->result.width = 16;
	v->result.stride = 16;
	switch (info->op)
	{
		case NO_ADD:
		case NO_SUB:
			v->result.scale = Max(args[0]->shape.scale, args[1]->shape.scale);
			break;
		case NO_MUL:
			if (args[0]->shape.scale + args[1]->shape.scale > 38)
				return false;
			v->result.scale = args[0]->shape.scale + args[1]->shape.scale;
			break;
		default:
			v->result.scale = args[0]->shape.scale;
			break;
	}
	v->fn = num_arith;
	return true;
}

static const VexecKernelDef num_cmp_def = {"numeric compare", false, NULL, num_variant};
static const VexecKernelDef num_arith_def = {"numeric arithmetic", true, NULL, num_variant};
static const VexecKernelDef num_cast_def = {"numeric cast", false, NULL, num_variant};

static const struct
{
	Oid			funcid;
	NumInfo		info;
	const VexecKernelDef *def;
}			num_funcs[] =
{
	{F_NUMERIC_EQ, {NO_EQ}, &num_cmp_def}, {F_NUMERIC_NE, {NO_NE}, &num_cmp_def},
	{F_NUMERIC_LT, {NO_LT}, &num_cmp_def}, {F_NUMERIC_LE, {NO_LE}, &num_cmp_def},
	{F_NUMERIC_GT, {NO_GT}, &num_cmp_def}, {F_NUMERIC_GE, {NO_GE}, &num_cmp_def},
	{F_NUMERIC_ADD, {NO_ADD}, &num_arith_def}, {F_NUMERIC_SUB, {NO_SUB}, &num_arith_def},
	{F_NUMERIC_MUL, {NO_MUL}, &num_arith_def}, {F_NUMERIC_UMINUS, {NO_NEG}, &num_arith_def},
	{F_NUMERIC_INT2, {NO_FROM_INT}, &num_cast_def}, {F_NUMERIC_INT4, {NO_FROM_INT}, &num_cast_def},
	{F_NUMERIC_INT8, {NO_FROM_INT}, &num_cast_def},
};

void
vexec_kernels_numeric(void (*add) (Oid, const VexecKernelDef *, const void *))
{
	int			i;

	for (i = 0; i < lengthof(num_funcs); i++)
		add(num_funcs[i].funcid, num_funcs[i].def, &num_funcs[i].info);
}
