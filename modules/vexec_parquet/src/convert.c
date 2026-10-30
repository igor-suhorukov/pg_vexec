/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * convert.c
 *	  Values between PostgreSQL's representations and Arrow's: numeric and
 *	  a decimal's scaled integer, dates and timestamps between the two
 *	  epochs.
 *
 * numeric's on-disk format -- a varlena of a header, a weight, and base
 * 10000 digits (numeric.c's NumericShort and NumericLong) -- is read and
 * made here directly, as pg_upgrade keeps it stable, so that a row's
 * numeric(p,s) becomes a decimal128's integer, and back, without numeric's
 * text form.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/int128.h"
#include "datatype/timestamp.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"
#include "varatt.h"

#include "convert.h"

#define NBASE		10000
#define NUMERIC_SIGN_MASK	0xC000
#define NUMERIC_POS			0x0000
#define NUMERIC_NEG			0x4000
#define NUMERIC_SHORT		0x8000
#define NUMERIC_SPECIAL		0xC000
#define NUMERIC_SHORT_SIGN_MASK			0x2000
#define NUMERIC_SHORT_DSCALE_MASK		0x1F80
#define NUMERIC_SHORT_DSCALE_SHIFT		7
#define NUMERIC_SHORT_DSCALE_MAX		(NUMERIC_SHORT_DSCALE_MASK >> NUMERIC_SHORT_DSCALE_SHIFT)
#define NUMERIC_SHORT_WEIGHT_SIGN_MASK	0x0040
#define NUMERIC_SHORT_WEIGHT_MASK		0x003F
#define NUMERIC_SHORT_WEIGHT_MAX		NUMERIC_SHORT_WEIGHT_MASK
#define NUMERIC_SHORT_WEIGHT_MIN		(-(NUMERIC_SHORT_WEIGHT_MASK+1))
#define NUMERIC_DSCALE_MASK			0x3FFF

static const int64 pow10_64[] = {
	1LL, 10LL, 100LL, 1000LL, 10000LL, 100000LL, 1000000LL, 10000000LL, 100000000LL,
	1000000000LL, 10000000000LL, 100000000000LL, 1000000000000LL, 10000000000000LL,
	100000000000000LL, 1000000000000000LL, 10000000000000000LL, 100000000000000000LL,
	1000000000000000000LL
};

/* The unscaled integer of a numeric at a scale: false where it is not one exactly, or not finite */
bool
vpq_numeric_to_int128(Datum d, int scale, int128 *out)
{
	struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
	const uint8 *p = (const uint8 *) VARDATA_ANY(v);
	Size		len = VARSIZE_ANY_EXHDR(v);
	uint16		header;
	bool		neg;
	int			weight;
	int			ndigits;
	const uint8 *digits;
	int128		acc = 0;
	int128		limit;

	if (len < sizeof(uint16))
		return false;
	memcpy(&header, p, sizeof(uint16));
	if ((header & NUMERIC_SIGN_MASK) == NUMERIC_SHORT)
	{
		neg = (header & NUMERIC_SHORT_SIGN_MASK) != 0;
		weight = (header & NUMERIC_SHORT_WEIGHT_SIGN_MASK ? ~NUMERIC_SHORT_WEIGHT_MASK : 0) |
			(header & NUMERIC_SHORT_WEIGHT_MASK);
		digits = p + sizeof(uint16);
		ndigits = (int) ((len - sizeof(uint16)) / sizeof(int16));
	}
	else if ((header & NUMERIC_SIGN_MASK) == NUMERIC_SPECIAL)
		return false;
	else
	{
		int16		w;

		if (len < sizeof(uint16) + sizeof(int16))
			return false;
		neg = (header & NUMERIC_SIGN_MASK) == NUMERIC_NEG;
		memcpy(&w, p + sizeof(uint16), sizeof(int16));
		weight = w;
		digits = p + sizeof(uint16) + sizeof(int16);
		ndigits = (int) ((len - sizeof(uint16) - sizeof(int16)) / sizeof(int16));
	}
	/* 10^38 bounds a decimal128's magnitude */
	limit = (int128) pow10_64[18] * pow10_64[18] * 100;
	for (int i = 0; i < ndigits; i++)
	{
		int16		dig;
		int			exp10;

		memcpy(&dig, digits + i * sizeof(int16), sizeof(int16));
		if (dig == 0)
			continue;
		exp10 = 4 * (weight - i) + scale;	/* the digit's power of ten in the unscaled integer */
		if (exp10 < 0)
		{
			int64		div;

			if (exp10 < -4)
				return false;
			div = pow10_64[-exp10];
			if (dig % div != 0)
				return false;
			acc += dig / div;
		}
		else
		{
			int128		term = dig;

			if (exp10 > 38)
				return false;
			for (int e = exp10; e > 0;)
			{
				int			step = Min(e, 18);

				term *= pow10_64[step];
				e -= step;
				if (term > limit)
					return false;
			}
			acc += term;
		}
		if (acc > limit)
			return false;
	}
	*out = neg ? -acc : acc;
	if ((Pointer) v != DatumGetPointer(d))
		pfree(v);
	return true;
}

/* A numeric of an unscaled integer at a scale, as numeric.c's make_result() makes one */
Datum
vpq_int128_to_numeric(int128 value, int scale)
{
	char		digits[64];
	int			nd = 0;
	bool		neg = value < 0;
	uint128		mag = neg ? (uint128) (-(value + 1)) + 1 : (uint128) value;
	char		dec[160];
	int			ndec;
	int			intlen;
	int			lpad;
	int			fraclen;
	int			rpad;
	int			ngroups;
	int16		groups[48];
	int			weight;
	int			first;
	int			last;
	int			nkeep;
	Size		size;
	struct varlena *result;
	uint8	   *p;

	if (scale < 0)
		scale = 0;
	/* the magnitude's decimal digits, most significant first */
	do
	{
		digits[nd++] = '0' + (int) (mag % 10);
		mag /= 10;
	} while (mag != 0);
	ndec = 0;
	/* at least one digit before the point */
	for (int i = nd; i <= scale; i++)
		dec[ndec++] = '0';
	for (int i = nd - 1; i >= 0; i--)
		dec[ndec++] = digits[i];
	intlen = ndec - scale;
	lpad = (4 - intlen % 4) % 4;
	fraclen = scale;
	rpad = (4 - fraclen % 4) % 4;
	ngroups = (lpad + ndec + rpad) / 4;
	{
		char		padded[200];
		int			k = 0;

		for (int i = 0; i < lpad; i++)
			padded[k++] = '0';
		memcpy(padded + k, dec, ndec);
		k += ndec;
		for (int i = 0; i < rpad; i++)
			padded[k++] = '0';
		for (int g = 0; g < ngroups; g++)
			groups[g] = (int16) ((padded[g * 4] - '0') * 1000 + (padded[g * 4 + 1] - '0') * 100 +
								 (padded[g * 4 + 2] - '0') * 10 + (padded[g * 4 + 3] - '0'));
	}
	weight = (lpad + intlen) / 4 - 1;
	first = 0;
	while (first < ngroups && groups[first] == 0)
	{
		first++;
		weight--;
	}
	last = ngroups - 1;
	while (last >= first && groups[last] == 0)
		last--;
	nkeep = last - first + 1;
	if (nkeep <= 0)
	{
		nkeep = 0;
		weight = 0;
		neg = false;
	}

	if (scale <= NUMERIC_SHORT_DSCALE_MAX && weight <= NUMERIC_SHORT_WEIGHT_MAX &&
		weight >= NUMERIC_SHORT_WEIGHT_MIN)
	{
		uint16		header;

		size = VARHDRSZ + sizeof(uint16) + nkeep * sizeof(int16);
		result = (struct varlena *) palloc(size);
		SET_VARSIZE(result, size);
		header = NUMERIC_SHORT | (neg ? NUMERIC_SHORT_SIGN_MASK : 0) |
			(scale << NUMERIC_SHORT_DSCALE_SHIFT) |
			(weight < 0 ? NUMERIC_SHORT_WEIGHT_SIGN_MASK : 0) | (weight & NUMERIC_SHORT_WEIGHT_MASK);
		p = (uint8 *) VARDATA(result);
		memcpy(p, &header, sizeof(uint16));
		p += sizeof(uint16);
	}
	else
	{
		uint16		sign_dscale = (neg ? NUMERIC_NEG : NUMERIC_POS) | (scale & NUMERIC_DSCALE_MASK);
		int16		w = (int16) weight;

		size = VARHDRSZ + sizeof(uint16) + sizeof(int16) + nkeep * sizeof(int16);
		result = (struct varlena *) palloc(size);
		SET_VARSIZE(result, size);
		p = (uint8 *) VARDATA(result);
		memcpy(p, &sign_dscale, sizeof(uint16));
		memcpy(p + sizeof(uint16), &w, sizeof(int16));
		p += sizeof(uint16) + sizeof(int16);
	}
	if (nkeep > 0)
		memcpy(p, &groups[first], nkeep * sizeof(int16));
	return PointerGetDatum(result);
}

/* A numeric made a typmod's, as an assignment does: rounded, its precision checked */
Datum
vpq_numeric_coerce(Datum value, int32 typmod)
{
	if (typmod < (int32) VARHDRSZ)
		return value;
	return DirectFunctionCall2(numeric, value, Int32GetDatum(typmod));
}

/* An integer's decimal digits, for messages and a decimal's slow path */
char *
vpq_int128_out(int128 value, int scale)
{
	return DatumGetCString(DirectFunctionCall1(numeric_out, vpq_int128_to_numeric(value, scale)));
}

int128
vpq_pow10_128(int n)
{
	int128		r = 1;

	while (n > 0)
	{
		int			step = Min(n, 18);

		r *= pow10_64[step];
		n -= step;
	}
	return r;
}
