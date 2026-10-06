/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * numeric.c
 *	  numeric as a scaled integer, and back (pg_vector_executor.md §3.4.1).
 *
 * A numeric whose typmod bounds its digits to 18 or 38 is held as an int64
 * or int128 at the typmod's scale: the idea comes from reading openGauss,
 * whose scaled-integer numeric is where much of its TPC-H Q1 gain comes
 * from; none of its code is used (§3.1).
 *
 * PostgreSQL exports no reader of a numeric's digits and no int128 builder
 * (PG19:src/backend/utils/adt/numeric.c:529; numeric.h:55-57), so both are
 * written here from the format numeric.c documents, which is also the
 * format on disk (numeric.c:107-261):
 *
 *	a varlena whose payload starts with a uint16 header word.  Its top two
 *	bits are 0x8000 for the short form, 0x0000 / 0x4000 for the long form's
 *	positive / negative, 0xC000 for NaN and the infinities.  The short form
 *	keeps sign (0x2000), display scale (0x1F80 >> 7) and weight (a 7-bit
 *	two's-complement number, 0x0040 its sign) in that word; the long form
 *	keeps sign and display scale (0x3FFF) in it and the weight in the next
 *	int16.  Then the digits, int16s in base 10000, the first of weight
 *	`weight`, leading and trailing zero digits stripped; zero has none, a
 *	weight of 0 and a positive sign.  The short form is used when the
 *	display scale is at most 63 and the weight between -64 and 63
 *	(NUMERIC_CAN_BE_SHORT, numeric.c:492-495).
 *
 * The scaled layout keeps a value only when it is finite and its display
 * scale is the typmod's: a value of another scale -- possible where an
 * access method stores Datums without coercion -- would print otherwise
 * after a round trip, so the column keeps its varlena layout instead.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "utils/numeric.h"
#include "varatt.h"

#include "vexec.h"
#include "batch/batch.h"

#define NBASE			10000
#define DEC_DIGITS		4

#define N_SIGN_MASK		0xC000
#define N_POS			0x0000
#define N_NEG			0x4000
#define N_SHORT			0x8000
#define N_SPECIAL		0xC000
#define N_SHORT_SIGN	0x2000
#define N_SHORT_DSCALE_MASK	0x1F80
#define N_SHORT_DSCALE_SHIFT 7
#define N_SHORT_DSCALE_MAX	(N_SHORT_DSCALE_MASK >> N_SHORT_DSCALE_SHIFT)
#define N_SHORT_WEIGHT_SIGN	0x0040
#define N_SHORT_WEIGHT_MASK	0x003F
#define N_SHORT_WEIGHT_MAX	N_SHORT_WEIGHT_MASK
#define N_SHORT_WEIGHT_MIN	(-(N_SHORT_WEIGHT_MASK + 1))
#define N_DSCALE_MASK	0x3FFF

static const int128 pow10_128[] = {
	1,
	10,
	100,
	1000,
	10000,
	100000,
	1000000,
	10000000,
	100000000,
	1000000000,
	(int128) 10000000000LL,
	(int128) 100000000000LL,
	(int128) 1000000000000LL,
	(int128) 10000000000000LL,
	(int128) 100000000000000LL,
	(int128) 1000000000000000LL,
	(int128) 10000000000000000LL,
	(int128) 100000000000000000LL,
	(int128) 1000000000000000000LL,
};

/* 10^n for 0 <= n <= 38, as an int128. */
static int128
pow10i(int n)
{
	int128		r;

	Assert(n >= 0 && n <= 38);
	if (n <= 18)
		return pow10_128[n];
	r = pow10_128[18];
	n -= 18;
	while (n > 18)
	{
		r *= pow10_128[18];
		n -= 18;
	}
	return r * pow10_128[n];
}

static uint16
read_u16(const char *p)
{
	uint16		v;

	memcpy(&v, p, sizeof(uint16));
	return v;
}

/*
 * A numeric Datum as an integer at a scale.  False when it is NaN or an
 * infinity, when its display scale is not `scale`, or when its value needs
 * more than `digits` digits or `width` bytes.  The value is read where it
 * is, short varlena header or not; a compressed or external value is
 * detoasted first, into the current memory context.
 */
bool
vexec_numeric_to_scaled(Datum num, int scale, int digits, int width, int128 *result)
{
	varlena    *vl = (varlena *) DatumGetPointer(num);
	const char *payload;
	int			len;
	uint16		header;
	int			sign;
	int			dscale;
	int			weight;
	int			ndigits;
	const char *dp;
	int128		v = 0;
	int			i;

	/* the scaled layout holds 38 digits at most, at a scale of 38 at most */
	if (scale < 0 || scale > 38)
		return false;
	if (VARATT_IS_EXTERNAL(vl) || VARATT_IS_COMPRESSED(vl))
		vl = detoast_attr(vl);
	payload = VARDATA_ANY(vl);
	len = (int) VARSIZE_ANY_EXHDR(vl);
	if (len < (int) sizeof(uint16))
		return false;
	header = read_u16(payload);

	if ((header & N_SIGN_MASK) == N_SPECIAL)
		return false;			/* NaN, +Infinity, -Infinity */
	if ((header & N_SIGN_MASK) == N_SHORT)
	{
		sign = (header & N_SHORT_SIGN) ? N_NEG : N_POS;
		dscale = (header & N_SHORT_DSCALE_MASK) >> N_SHORT_DSCALE_SHIFT;
		weight = (header & N_SHORT_WEIGHT_SIGN) ? (int) (~N_SHORT_WEIGHT_MASK | (header & N_SHORT_WEIGHT_MASK))
			: (int) (header & N_SHORT_WEIGHT_MASK);
		dp = payload + sizeof(uint16);
		ndigits = (len - (int) sizeof(uint16)) / (int) sizeof(int16);
	}
	else
	{
		int16		w;

		if (len < (int) (2 * sizeof(uint16)))
			return false;
		sign = header & N_SIGN_MASK;
		dscale = header & N_DSCALE_MASK;
		memcpy(&w, payload + sizeof(uint16), sizeof(int16));
		weight = w;
		dp = payload + 2 * sizeof(uint16);
		ndigits = (len - 2 * (int) sizeof(uint16)) / (int) sizeof(int16);
	}
	if (dscale != scale)
		return false;
	if (ndigits == 0)
	{
		*result = 0;			/* zero has no digits */
		return true;
	}

	/*
	 * value = sum of d(k) * NBASE^(weight - k), and the scaled integer is
	 * value * 10^scale: each digit times 10^e(k), e(k) = DEC_DIGITS *
	 * (weight - k) + scale.  A digit whose e(k) is negative reaches below
	 * the display scale: its decimal digits there are the zeros padding the
	 * last base-NBASE digit, which divide away; anything else there would be
	 * finer than the value's own scale, which a numeric never holds.
	 */
	for (i = 0; i < ndigits; i++)
	{
		int16		d;
		int			e = DEC_DIGITS * (weight - i) + scale;
		int128		part;

		memcpy(&d, dp + i * sizeof(int16), sizeof(int16));
		if (d < 0 || d >= NBASE)
			return false;
		if (d == 0)
			continue;
		if (e >= 0)
		{
			/* d * 10^e within 38 digits, tested before it is computed */
			if (e > 37 || d >= pow10i(38 - e))
				return false;
			part = (int128) d * pow10i(e);
			if (v >= pow10i(38) - part)
				return false;
		}
		else
		{
			int128		p;

			if (-e >= DEC_DIGITS)
				return false;
			p = pow10i(-e);
			if (d % p != 0)
				return false;
			part = d / p;
		}
		v += part;
	}

	if (digits > 0 && digits <= 38 && v >= pow10i(digits))
		return false;
	if (width == 8 && v > (int128) PG_INT64_MAX)
		return false;
	*result = sign == N_NEG ? -v : v;
	return true;
}

/*
 * A numeric Datum's display scale, or -1 for NaN and the infinities.  Read
 * from its header word, as vexec_numeric_to_scaled() reads it.
 */
int
vexec_numeric_dscale(Datum num)
{
	varlena    *vl = (varlena *) DatumGetPointer(num);
	const char *payload;
	uint16		header;
	int			result;
	bool		copied = false;

	if (VARATT_IS_EXTERNAL(vl) || VARATT_IS_COMPRESSED(vl))
	{
		vl = detoast_attr(vl);
		copied = true;
	}
	if (VARSIZE_ANY_EXHDR(vl) < (int) sizeof(uint16))
		result = -1;
	else
	{
		payload = VARDATA_ANY(vl);
		header = read_u16(payload);
		if ((header & N_SIGN_MASK) == N_SPECIAL)
			result = -1;
		else if ((header & N_SIGN_MASK) == N_SHORT)
			result = (header & N_SHORT_DSCALE_MASK) >> N_SHORT_DSCALE_SHIFT;
		else
			result = header & N_DSCALE_MASK;
	}
	if (copied)
		pfree(vl);
	return result;
}

/*
 * A numeric Datum's parts, as its header and digits hold them: what
 * numeric.c's NumericVar holds, for writing a NumericAggState's
 * serialization (numericvar_serialize, numeric.c:7522-7533).  False for NaN
 * and the infinities, which parts->special names.  The digits point into the
 * value, detoasted into the current memory context where it must be.
 */
bool
vexec_numeric_parts(Datum num, VexecNumericParts *parts)
{
	varlena    *vl = (varlena *) DatumGetPointer(num);
	const char *payload;
	int			len;
	uint16		header;

	memset(parts, 0, sizeof(VexecNumericParts));
	if (VARATT_IS_EXTERNAL(vl) || VARATT_IS_COMPRESSED(vl))
		vl = detoast_attr(vl);
	payload = VARDATA_ANY(vl);
	len = (int) VARSIZE_ANY_EXHDR(vl);
	if (len < (int) sizeof(uint16))
		elog(ERROR, "vexec: a numeric of %d bytes", len);
	header = read_u16(payload);
	if ((header & N_SIGN_MASK) == N_SPECIAL)
	{
		/* NUMERIC_NAN 0xC000, NUMERIC_PINF 0xD000, NUMERIC_NINF 0xF000 */
		switch (header & 0xF000)
		{
			case 0xD000:
				parts->special = VEXEC_NUMERIC_PINF;
				break;
			case 0xF000:
				parts->special = VEXEC_NUMERIC_NINF;
				break;
			default:
				parts->special = VEXEC_NUMERIC_NAN;
				break;
		}
		return false;
	}
	if ((header & N_SIGN_MASK) == N_SHORT)
	{
		parts->sign = (header & N_SHORT_SIGN) ? N_NEG : N_POS;
		parts->dscale = (header & N_SHORT_DSCALE_MASK) >> N_SHORT_DSCALE_SHIFT;
		parts->weight = (header & N_SHORT_WEIGHT_SIGN) ?
			(int) (~N_SHORT_WEIGHT_MASK | (header & N_SHORT_WEIGHT_MASK)) :
			(int) (header & N_SHORT_WEIGHT_MASK);
		parts->digits = payload + sizeof(uint16);
		parts->ndigits = (len - (int) sizeof(uint16)) / (int) sizeof(int16);
	}
	else
	{
		int16		w;

		if (len < (int) (2 * sizeof(uint16)))
			elog(ERROR, "vexec: a numeric of %d bytes", len);
		parts->sign = header & N_SIGN_MASK;
		parts->dscale = header & N_DSCALE_MASK;
		memcpy(&w, payload + sizeof(uint16), sizeof(int16));
		parts->weight = w;
		parts->digits = payload + 2 * sizeof(uint16);
		parts->ndigits = (len - 2 * (int) sizeof(uint16)) / (int) sizeof(int16);
	}
	return true;
}

/*
 * An integer at a scale as a numeric Datum, written into the batch's arena
 * -- or palloc'd, when batch is NULL -- with a 4-byte header: the form
 * numeric's accessors take without a copy (§3.4.2).  It is the numeric PostgreSQL's make_result() would make of the
 * same value and display scale (numeric.c:7577-7660): digits stripped of
 * leading and trailing zeros, zero with no digits, the short form wherever
 * it fits.
 */
Datum
vexec_scaled_to_numeric(VexecBatch *batch, int128 value, int scale)
{
	char		dec[48];		/* decimal digits, most significant first */
	int			ndec = 0;
	int16		digits[32];
	int			ndigits = 0;
	int			weight;
	int			sign = value < 0 ? N_NEG : N_POS;
	uint128		u = value < 0 ? (uint128) (-(value + 1)) + 1 : (uint128) value;
	int			intdigits;
	int			first;
	int			total;
	int			len;
	char	   *p;
	int			i;

	/*
	 * The digits below are laid out in a fixed buffer sized for 38 digits
	 * at a scale of 38 at most, the scaled layout's bounds (types.c).
	 */
	if (scale < 0 || scale > 38)
		elog(ERROR, "a scaled numeric's scale must be between 0 and 38, not %d", scale);

	/* the decimal digits of |value| */
	{
		char		rev[48];
		int			n = 0;

		do
		{
			rev[n++] = (char) (u % 10);
			u /= 10;
		} while (u != 0);
		for (i = 0; i < n; i++)
			dec[i] = rev[n - 1 - i];
		ndec = n;
	}

	/*
	 * Group them into base-NBASE digits aligned on the decimal point, which
	 * is `scale` digits from the right: the integer part's groups to its
	 * left, the fraction's to its right, the last group padded with zeros.
	 */
	intdigits = ndec - scale;	/* may be <= 0 */
	{
		int			lead = intdigits > 0 ? (DEC_DIGITS - intdigits % DEC_DIGITS) % DEC_DIGITS : 0;
		int			fracpad;
		int			start;		/* the position of dec[0] within the
								 * grouped string */
		int			pos;
		char		grouped[96];	/* lead 3 + leading zeros 38 + 39 digits +
									 * pad 3 */

		/*
		 * Lay the digits out as [lead zeros][integer digits][fraction
		 * digits][pad zeros], with the fraction's own leading zeros when the
		 * value is below 1.
		 */
		memset(grouped, 0, sizeof(grouped));
		pos = lead;
		if (intdigits <= 0)
			pos += -intdigits;	/* the fraction's leading zeros */
		start = pos;
		memcpy(grouped + start, dec, ndec);
		total = start + ndec;
		fracpad = (DEC_DIGITS - (Max(scale, 0) % DEC_DIGITS)) % DEC_DIGITS;
		total += (scale > 0 || intdigits <= 0) ? fracpad : 0;
		total = TYPEALIGN(DEC_DIGITS, total);
		if (total > (int) sizeof(grouped))
			elog(ERROR, "scaled numeric of %d digits is too long", ndec);

		ndigits = total / DEC_DIGITS;
		for (i = 0; i < ndigits; i++)
			digits[i] = (int16) (grouped[i * 4] * 1000 + grouped[i * 4 + 1] * 100 +
								 grouped[i * 4 + 2] * 10 + grouped[i * 4 + 3]);
		/* the integer part's groups have weights ndigits_int - 1 .. 0 */
		weight = (intdigits > 0 ? (lead + intdigits) / DEC_DIGITS : 0) - 1;
	}

	/* strip leading and trailing zero digits; zero has none */
	first = 0;
	while (first < ndigits && digits[first] == 0)
	{
		first++;
		weight--;
	}
	while (ndigits > first && digits[ndigits - 1] == 0)
		ndigits--;
	ndigits -= first;
	if (ndigits == 0)
	{
		weight = 0;
		sign = N_POS;
	}

	if (scale <= N_SHORT_DSCALE_MAX && weight <= N_SHORT_WEIGHT_MAX && weight >= N_SHORT_WEIGHT_MIN)
	{
		uint16		header;

		len = VARHDRSZ + sizeof(uint16) + ndigits * sizeof(int16);
		p = batch ? vexec_arena_alloc(&batch->arena, len, sizeof(int32), NULL, NULL) : palloc(len);
		SET_VARSIZE(p, len);
		header = (uint16) ((sign == N_NEG ? (N_SHORT | N_SHORT_SIGN) : N_SHORT) |
						   (scale << N_SHORT_DSCALE_SHIFT) |
						   (weight < 0 ? N_SHORT_WEIGHT_SIGN : 0) |
						   (weight & N_SHORT_WEIGHT_MASK));
		memcpy(p + VARHDRSZ, &header, sizeof(uint16));
		memcpy(p + VARHDRSZ + sizeof(uint16), digits + first, ndigits * sizeof(int16));
	}
	else
	{
		uint16		header;
		int16		w = (int16) weight;

		len = VARHDRSZ + 2 * sizeof(uint16) + ndigits * sizeof(int16);
		p = batch ? vexec_arena_alloc(&batch->arena, len, sizeof(int32), NULL, NULL) : palloc(len);
		SET_VARSIZE(p, len);
		header = (uint16) (sign | (scale & N_DSCALE_MASK));
		memcpy(p + VARHDRSZ, &header, sizeof(uint16));
		memcpy(p + VARHDRSZ + sizeof(uint16), &w, sizeof(int16));
		memcpy(p + VARHDRSZ + 2 * sizeof(uint16), digits + first, ndigits * sizeof(int16));
	}
	return PointerGetDatum(p);
}
