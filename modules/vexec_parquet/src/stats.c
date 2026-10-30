/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * stats.c
 *	  A file's statistics, by row group and column: an own file's as
 *	  vexec_parquet.files keeps them, computed as its rows were written, so
 *	  that planning and pruning read no footer; an external file's from its
 *	  footer, as Arrow's reader trusts them.
 *
 * The catalog's form, all integers native:
 *
 *	uint32 magic, int32 row groups, int32 columns
 *	int64 rows of each row group
 *	per column: int16 attnum, uint8 class (stats.h)
 *	"VPQ2": uint32 the file's facts' bytes and them (engine.h), what a device
 *		unit asks of its footer, so that none is read (pg_vector_executor.md
 *		§3.34, PQ1); "VPQ1", of files written before, has none
 *	per row group and column: int64 compressed bytes, int64 NULLs, uint8
 *		flags (1 least and greatest kept, 2 NaNs left out), and where kept:
 *		int64 x2 (INT), double x2 (FLOAT), int128 x2 (DECIMAL), or int32
 *		length and bytes x2 (BYTES)
 *
 * Read a column at a time, as a scan asks for it (PQ3): an index of the
 * entries' places is made in one pass, and a column's entries decoded where
 * asked -- planning a query of a few columns decodes those alone.
 *
 * The least and greatest are by the type's own order -- a float's NaNs
 * left out, flagged -- and a string's or a binary's bytewise, which a
 * text's collation honours only for equality, or as "C" (filter.c).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "common/int.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "utils/date.h"
#include "utils/timestamp.h"
#include "varatt.h"

#include "vexec_parquet.h"
#include "convert.h"
#include "stats.h"

#define VPQ_STATS_MAGIC 0x31515056	/* "VPQ1" */
#define VPQ_STATS_MAGIC2 0x32515056 /* "VPQ2": the file's facts after its columns */

#define STAT_MINMAX		0x01
#define STAT_NAN		0x02

VpqStatsClass
vpq_stats_class(VpqKind kind)
{
	switch (kind)
	{
		case VPQ_K_BOOL:
		case VPQ_K_CHAR:
		case VPQ_K_INT2:
		case VPQ_K_INT4:
		case VPQ_K_INT8:
		case VPQ_K_OID:
		case VPQ_K_DATE:
		case VPQ_K_TIME:
		case VPQ_K_TIMESTAMP:
		case VPQ_K_TIMESTAMPTZ:
			return VPQ_STATS_INT;
		case VPQ_K_FLOAT4:
		case VPQ_K_FLOAT8:
			return VPQ_STATS_FLOAT;
		case VPQ_K_NUMERIC:
			return VPQ_STATS_DECIMAL;
		case VPQ_K_TEXT:
		case VPQ_K_BYTEA:
		case VPQ_K_UUID:
			return VPQ_STATS_BYTES;
		default:
			return VPQ_STATS_NONE;
	}
}

int
vpq_bytes_cmp(const char *a, int alen, const char *b, int blen)
{
	int			c = memcmp(a, b, Min(alen, blen));

	if (c != 0)
		return c;
	return alen < blen ? -1 : alen > blen ? 1 : 0;
}

bytea *
vpq_stats_build(int nrg, const int64 *rg_rows, int ncols, const AttrNumber *attnums,
				const VpqKind *kinds, const VpqRawStat *stats, const int64 *bytes,
				const char *facts, Size facts_len)
{
	StringInfoData buf;
	uint32		magic = VPQ_STATS_MAGIC2;
	int32		n32;
	uint32		flen = (uint32) facts_len;
	bytea	   *result;

	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, &magic, sizeof(magic));
	n32 = nrg;
	appendBinaryStringInfo(&buf, &n32, sizeof(n32));
	n32 = ncols;
	appendBinaryStringInfo(&buf, &n32, sizeof(n32));
	appendBinaryStringInfo(&buf, rg_rows, sizeof(int64) * nrg);
	for (int c = 0; c < ncols; c++)
	{
		int16		a = attnums[c];
		uint8		k = (uint8) vpq_stats_class(kinds[c]);

		appendBinaryStringInfo(&buf, &a, sizeof(a));
		appendBinaryStringInfo(&buf, &k, sizeof(k));
	}
	appendBinaryStringInfo(&buf, &flen, sizeof(flen));
	if (flen > 0)
		appendBinaryStringInfo(&buf, facts, flen);
	for (int g = 0; g < nrg; g++)
		for (int c = 0; c < ncols; c++)
		{
			const VpqRawStat *s = &stats[g * ncols + c];
			int64		b = bytes ? bytes[g * ncols + c] : 0;
			uint8		flags = (s->has_minmax ? STAT_MINMAX : 0) | (s->may_nan ? STAT_NAN : 0);

			appendBinaryStringInfo(&buf, &b, sizeof(b));
			appendBinaryStringInfo(&buf, &s->nulls, sizeof(s->nulls));
			appendBinaryStringInfo(&buf, &flags, sizeof(flags));
			if (!s->has_minmax)
				continue;
			switch (vpq_stats_class(kinds[c]))
			{
				case VPQ_STATS_INT:
					appendBinaryStringInfo(&buf, &s->imin, sizeof(int64));
					appendBinaryStringInfo(&buf, &s->imax, sizeof(int64));
					break;
				case VPQ_STATS_FLOAT:
					appendBinaryStringInfo(&buf, &s->dmin, sizeof(double));
					appendBinaryStringInfo(&buf, &s->dmax, sizeof(double));
					break;
				case VPQ_STATS_DECIMAL:
					appendBinaryStringInfo(&buf, &s->nmin, sizeof(int128));
					appendBinaryStringInfo(&buf, &s->nmax, sizeof(int128));
					break;
				case VPQ_STATS_BYTES:
					{
						int32		len = s->bminlen;

						appendBinaryStringInfo(&buf, &len, sizeof(len));
						appendBinaryStringInfo(&buf, s->bmin, len);
						len = s->bmaxlen;
						appendBinaryStringInfo(&buf, &len, sizeof(len));
						appendBinaryStringInfo(&buf, s->bmax, len);
					}
					break;
				case VPQ_STATS_NONE:
					break;
			}
		}
	result = (bytea *) palloc(VARHDRSZ + buf.len);
	SET_VARSIZE(result, VARHDRSZ + buf.len);
	memcpy(VARDATA(result), buf.data, buf.len);
	pfree(buf.data);
	return result;
}

typedef struct Reader
{
	const char *p;
	const char *end;
} Reader;

static void
take(Reader *r, void *out, Size n)
{
	if (r->p + n > r->end)
		elog(ERROR, "vexec_parquet: a file's statistics in the catalog are cut short");
	memcpy(out, r->p, n);
	r->p += n;
}

/* The header read: the row groups and columns; *magic VPQ1's or VPQ2's */
static void
open_reader(bytea *raw, Reader *r, int32 *nrg, int32 *ncols, uint32 *magic)
{
	r->p = VARDATA_ANY(raw);
	r->end = r->p + VARSIZE_ANY_EXHDR(raw);
	take(r, magic, sizeof(*magic));
	if (*magic != VPQ_STATS_MAGIC && *magic != VPQ_STATS_MAGIC2)
		elog(ERROR, "vexec_parquet: a file's statistics in the catalog are not of this version");
	take(r, nrg, sizeof(int32));
	take(r, ncols, sizeof(int32));
	if (*nrg < 0 || *ncols < 0)
		elog(ERROR, "vexec_parquet: a file's statistics in the catalog are corrupt");
}

/* Past the columns' attnums and classes, and a VPQ2's facts: *facts and *facts_len, or NULL */
static void
pass_header(Reader *r, int32 nrg, int32 ncols, uint32 magic, const char **facts, uint32 *facts_len)
{
	*facts = NULL;
	*facts_len = 0;
	if ((Size) (r->end - r->p) < sizeof(int64) * (Size) nrg + 3 * (Size) ncols)
		elog(ERROR, "vexec_parquet: a file's statistics in the catalog are cut short");
	r->p += sizeof(int64) * (Size) nrg + 3 * (Size) ncols;
	if (magic == VPQ_STATS_MAGIC2)
	{
		take(r, facts_len, sizeof(*facts_len));
		if ((Size) (r->end - r->p) < *facts_len)
			elog(ERROR, "vexec_parquet: a file's statistics in the catalog are cut short");
		*facts = *facts_len > 0 ? r->p : NULL;
		r->p += *facts_len;
	}
}

/* The row groups' rows alone */
int
vpq_stats_row_groups(bytea *raw, int64 **rg_rows)
{
	Reader		r;
	int32		nrg;
	int32		ncols;
	uint32		magic;

	open_reader(raw, &r, &nrg, &ncols, &magic);
	*rg_rows = palloc(sizeof(int64) * Max(nrg, 1));
	take(&r, *rg_rows, sizeof(int64) * nrg);
	return nrg;
}

/* The file's facts the catalog keeps (engine.h), or NULL: a file written before them */
const char *
vpq_stats_facts(bytea *raw, Size *len)
{
	Reader		r;
	int32		nrg;
	int32		ncols;
	uint32		magic;
	const char *facts;
	uint32		flen;

	open_reader(raw, &r, &nrg, &ncols, &magic);
	pass_header(&r, nrg, ncols, magic, &facts, &flen);
	*len = flen;
	return facts;
}

/* A kept integer as a Datum of the column's type */
static Datum
int_datum(VpqKind kind, int64 v)
{
	switch (kind)
	{
		case VPQ_K_BOOL:
			return BoolGetDatum(v != 0);
		case VPQ_K_CHAR:
			return CharGetDatum((char) v);
		case VPQ_K_INT2:
			return Int16GetDatum((int16) v);
		case VPQ_K_INT4:
		case VPQ_K_DATE:
			return Int32GetDatum((int32) v);
		case VPQ_K_OID:
			return ObjectIdGetDatum((Oid) v);
		default:
			return Int64GetDatum(v);
	}
}

static Datum
bytes_datum(const char *p, int len)
{
	bytea	   *b = palloc(VARHDRSZ + len);

	SET_VARSIZE(b, VARHDRSZ + len);
	memcpy(VARDATA(b), p, len);
	return PointerGetDatum(b);
}

/*
 * The index of an own file's statistics: each column's attnum and class,
 * and each entry's place, found in one pass that decodes nothing
 */
struct VpqStatsIndex
{
	int32		nrg;
	int32		ncols;
	AttrNumber *attnums;
	uint8	   *classes;
	uint32	   *offsets;		/* [rg * ncols + c]: the entry's place past the bytea's header */
};

VpqStatsIndex *
vpq_stats_index(bytea *raw)
{
	VpqStatsIndex *ix = palloc0(sizeof(VpqStatsIndex));
	Reader		r;
	Reader		h;
	uint32		magic;
	const char *base;
	const char *facts;
	uint32		flen;

	open_reader(raw, &r, &ix->nrg, &ix->ncols, &magic);
	base = VARDATA_ANY(raw);
	h = r;
	pass_header(&r, ix->nrg, ix->ncols, magic, &facts, &flen);
	h.p += sizeof(int64) * (Size) ix->nrg;
	ix->attnums = palloc(sizeof(AttrNumber) * Max(ix->ncols, 1));
	ix->classes = palloc(Max(ix->ncols, 1));
	for (int c = 0; c < ix->ncols; c++)
	{
		int16		a;

		take(&h, &a, sizeof(a));
		take(&h, &ix->classes[c], 1);
		ix->attnums[c] = a;
	}
	ix->offsets = palloc(sizeof(uint32) * Max((Size) ix->nrg * ix->ncols, 1));
	for (int g = 0; g < ix->nrg; g++)
		for (int c = 0; c < ix->ncols; c++)
		{
			uint8		flags;

			ix->offsets[(Size) g * ix->ncols + c] = (uint32) (r.p - base);
			if ((Size) (r.end - r.p) < 2 * sizeof(int64) + 1)
				elog(ERROR, "vexec_parquet: a file's statistics in the catalog are cut short");
			r.p += 2 * sizeof(int64);
			take(&r, &flags, 1);
			if (!(flags & STAT_MINMAX))
				continue;
			switch ((VpqStatsClass) ix->classes[c])
			{
				case VPQ_STATS_INT:
				case VPQ_STATS_FLOAT:
					r.p += 2 * sizeof(int64);
					break;
				case VPQ_STATS_DECIMAL:
					r.p += 2 * sizeof(int128);
					break;
				case VPQ_STATS_BYTES:
					for (int k = 0; k < 2; k++)
					{
						int32		len;

						take(&r, &len, sizeof(len));
						if (len < 0 || (Size) (r.end - r.p) < (Size) len)
							elog(ERROR, "vexec_parquet: a file's statistics in the catalog are cut short");
						r.p += len;
					}
					break;
				case VPQ_STATS_NONE:
					break;
			}
			if (r.p > r.end)
				elog(ERROR, "vexec_parquet: a file's statistics in the catalog are cut short");
		}
	return ix;
}

/* The index's column of an attnum, or -1: the file has none */
int
vpq_stats_index_column(VpqStatsIndex *ix, AttrNumber attnum)
{
	for (int c = 0; c < ix->ncols; c++)
		if (ix->attnums[c] == attnum)
			return c;
	return -1;
}

int
vpq_stats_index_row_groups(VpqStatsIndex *ix)
{
	return ix->nrg;
}

/*
 * Column c's entry of row group rg decoded: its compressed bytes, NULLs and
 * least and greatest as a column of the table's kind reads them, in the
 * current memory context
 */
void
vpq_stats_decode(VpqTable *t, bytea *raw, VpqStatsIndex *ix, int c, int rg, VpqStat *s, int64 *bytes)
{
	Reader		r;
	AttrNumber	attnum = ix->attnums[c];
	VpqColumn  *col = attnum >= 1 && attnum <= t->natts ? &t->cols[attnum - 1] : NULL;
	uint8		flags;

	memset(s, 0, sizeof(*s));
	r.p = VARDATA_ANY(raw) + ix->offsets[(Size) rg * ix->ncols + c];
	r.end = VARDATA_ANY(raw) + VARSIZE_ANY_EXHDR(raw);
	take(&r, bytes, sizeof(int64));
	take(&r, &s->nulls, sizeof(int64));
	take(&r, &flags, 1);
	s->may_nan = (flags & STAT_NAN) != 0;
	s->exact = true;
	if (!(flags & STAT_MINMAX))
		return;
	switch ((VpqStatsClass) ix->classes[c])
	{
		case VPQ_STATS_INT:
			{
				int64		lo;
				int64		hi;

				take(&r, &lo, sizeof(lo));
				take(&r, &hi, sizeof(hi));
				if (col && col->kind != VPQ_K_DROPPED && vpq_stats_class(col->kind) == VPQ_STATS_INT)
				{
					s->min = int_datum(col->kind, lo);
					s->max = int_datum(col->kind, hi);
					s->has_minmax = true;
				}
			}
			break;
		case VPQ_STATS_FLOAT:
			{
				double		lo;
				double		hi;

				take(&r, &lo, sizeof(lo));
				take(&r, &hi, sizeof(hi));
				if (col && (col->kind == VPQ_K_FLOAT4 || col->kind == VPQ_K_FLOAT8))
				{
					s->min = col->kind == VPQ_K_FLOAT4 ? Float4GetDatum((float4) lo) : Float8GetDatum(lo);
					s->max = col->kind == VPQ_K_FLOAT4 ? Float4GetDatum((float4) hi) : Float8GetDatum(hi);
					s->has_minmax = true;
				}
			}
			break;
		case VPQ_STATS_DECIMAL:
			{
				int128		lo;
				int128		hi;

				take(&r, &lo, sizeof(lo));
				take(&r, &hi, sizeof(hi));
				if (col && col->kind == VPQ_K_NUMERIC)
				{
					s->min = vpq_int128_to_numeric(lo, col->scale);
					s->max = vpq_int128_to_numeric(hi, col->scale);
					s->has_minmax = true;
				}
			}
			break;
		case VPQ_STATS_BYTES:
			{
				int32		len;
				const char *lo;
				int32		lolen;

				take(&r, &len, sizeof(len));
				lo = r.p;
				lolen = len;
				r.p += len;
				take(&r, &len, sizeof(len));
				if (r.p + len > r.end)
					elog(ERROR, "vexec_parquet: a file's statistics in the catalog are cut short");
				if (col && vpq_stats_class(col->kind) == VPQ_STATS_BYTES)
				{
					s->min = bytes_datum(lo, lolen);
					s->max = bytes_datum(r.p, len);
					s->bytewise = true;
					s->has_minmax = true;
				}
			}
			break;
		case VPQ_STATS_NONE:
			break;
	}
}

/* An integer of a time unit as microseconds, rounded toward lower (up false) or higher bounds */
static int64
to_usecs(int64 v, int unit, bool up)
{
	switch (unit)
	{
		case VPQ_UNIT_S:
			return v * 1000000;
		case VPQ_UNIT_MS:
			return v * 1000;
		case VPQ_UNIT_US:
			return v;
		default:
			{
				int64		q = v / 1000;
				int64		r = v % 1000;

				if (r != 0 && up && v > 0)
					q++;
				else if (r != 0 && !up && v < 0)
					q--;
				return q;
			}
	}
}

/*
 * An external file's statistics of a row group's column, as a column of
 * the table's kind reads them: false where the footer has none this file's
 * type can give the kind
 */
bool
vpq_stat_from_footer(VpqTable *t, VpqColumn *col, vpq_file *file, int fcol, int rg, VpqStat *out)
{
	int64_t		nulls;
	bool		has_minmax;
	vpq_value	lo;
	vpq_value	hi;
	bool		lo_exact;
	bool		hi_exact;
	const char *name;
	int			field_id;
	vpq_type	type;
	bool		nullable;
	bool		is_unsigned;

	memset(out, 0, sizeof(*out));
	out->nulls = -1;
	if (!vpq_file_column_stats(file, rg, fcol, &nulls, &has_minmax, &lo, &hi, &lo_exact, &hi_exact))
		return false;
	out->nulls = nulls;
	(void) vpq_file_column(file, fcol, &name, &field_id, &type, &nullable);
	if (!has_minmax)
		return true;
	out->exact = lo_exact && hi_exact;
	switch (col->kind)
	{
		case VPQ_K_BOOL:
			if (type.id != VPQ_T_BOOL)
				return true;
			out->min = BoolGetDatum(lo.i != 0);
			out->max = BoolGetDatum(hi.i != 0);
			break;
		case VPQ_K_INT2:
		case VPQ_K_INT4:
		case VPQ_K_INT8:
			switch (type.id)
			{
				case VPQ_T_INT8:
				case VPQ_T_INT16:
				case VPQ_T_INT32:
				case VPQ_T_INT64:
				case VPQ_T_UINT8:
				case VPQ_T_UINT16:
				case VPQ_T_UINT32:
				case VPQ_T_UINT64:
					break;
				default:
					return true;
			}
			/* a file's wider values than the kind's own are the reader's errors: no bound from them */
			if ((col->kind == VPQ_K_INT2 && (lo.i < PG_INT16_MIN || hi.i > PG_INT16_MAX)) ||
				(col->kind == VPQ_K_INT4 && (lo.i < PG_INT32_MIN || hi.i > PG_INT32_MAX)))
				return true;
			out->min = int_datum(col->kind, lo.i);
			out->max = int_datum(col->kind, hi.i);
			break;
		case VPQ_K_FLOAT4:
		case VPQ_K_FLOAT8:
			if (type.id != VPQ_T_FLOAT32 && type.id != VPQ_T_FLOAT64)
				return true;
			if (isnan(lo.d) || isnan(hi.d))
				return true;
			out->min = col->kind == VPQ_K_FLOAT4 ? Float4GetDatum((float4) lo.d) : Float8GetDatum(lo.d);
			out->max = col->kind == VPQ_K_FLOAT4 ? Float4GetDatum((float4) hi.d) : Float8GetDatum(hi.d);
			/* a footer says nothing of NaNs, which PostgreSQL orders above every number */
			out->may_nan = true;
			break;
		case VPQ_K_DATE:
			if (type.id == VPQ_T_DATE32)
			{
				out->min = DateADTGetDatum((DateADT) (lo.i - VPQ_EPOCH_DAYS));
				out->max = DateADTGetDatum((DateADT) (hi.i - VPQ_EPOCH_DAYS));
			}
			else if (type.id == VPQ_T_DATE64)
			{
				int64		dlo = lo.i / 86400000 - (lo.i % 86400000 < 0 ? 1 : 0);
				int64		dhi = hi.i / 86400000 - (hi.i % 86400000 < 0 ? 1 : 0);

				out->min = DateADTGetDatum((DateADT) (dlo - VPQ_EPOCH_DAYS));
				out->max = DateADTGetDatum((DateADT) (dhi - VPQ_EPOCH_DAYS));
			}
			else if (vpq_int_width(type.id, &is_unsigned) > 0 && type.id != VPQ_T_UINT64)
			{
				/* PQ6 (§3.35): an integer of days since 1970; values past a date's are the reader's errors */
				if (lo.i < (int64) PG_INT32_MIN + VPQ_EPOCH_DAYS || hi.i > (int64) PG_INT32_MAX + VPQ_EPOCH_DAYS ||
					!IS_VALID_DATE((DateADT) (lo.i - VPQ_EPOCH_DAYS)) || !IS_VALID_DATE((DateADT) (hi.i - VPQ_EPOCH_DAYS)))
					return true;
				out->min = DateADTGetDatum((DateADT) (lo.i - VPQ_EPOCH_DAYS));
				out->max = DateADTGetDatum((DateADT) (hi.i - VPQ_EPOCH_DAYS));
			}
			else
				return true;
			break;
		case VPQ_K_TIMESTAMP:
		case VPQ_K_TIMESTAMPTZ:
			if (vpq_int_width(type.id, &is_unsigned) > 0 && type.id != VPQ_T_UINT64)
			{
				/* PQ6: an integer of seconds since 1970; values past a timestamp's are the reader's errors */
				int64		ulo;
				int64		uhi;

				if (pg_mul_s64_overflow(lo.i, USECS_PER_SEC, &ulo) || pg_mul_s64_overflow(hi.i, USECS_PER_SEC, &uhi) ||
					ulo < PG_INT64_MIN + VPQ_EPOCH_USECS || !IS_VALID_TIMESTAMP(ulo - VPQ_EPOCH_USECS) ||
					!IS_VALID_TIMESTAMP(uhi - VPQ_EPOCH_USECS))
					return true;
				out->min = TimestampGetDatum(ulo - VPQ_EPOCH_USECS);
				out->max = TimestampGetDatum(uhi - VPQ_EPOCH_USECS);
				break;
			}
			if (type.id != VPQ_T_TIMESTAMP)
				return true;
			out->min = TimestampGetDatum(to_usecs(lo.i, type.unit, false) - VPQ_EPOCH_USECS);
			out->max = TimestampGetDatum(to_usecs(hi.i, type.unit, true) - VPQ_EPOCH_USECS);
			break;
		case VPQ_K_TIME:
			if (type.id != VPQ_T_TIME32 && type.id != VPQ_T_TIME64)
				return true;
			out->min = Int64GetDatum(to_usecs(lo.i, type.unit, false));
			out->max = Int64GetDatum(to_usecs(hi.i, type.unit, true));
			break;
		case VPQ_K_NUMERIC:
		case VPQ_K_NUMERIC_ANY:
			if (type.id == VPQ_T_DECIMAL32 || type.id == VPQ_T_DECIMAL64 || type.id == VPQ_T_DECIMAL128)
			{
				int128		vlo = ((int128) lo.i128_hi << 64) | lo.i128_lo;
				int128		vhi = ((int128) hi.i128_hi << 64) | hi.i128_lo;

				out->min = vpq_int128_to_numeric(vlo, type.scale);
				out->max = vpq_int128_to_numeric(vhi, type.scale);
			}
			else
				return true;
			break;
		case VPQ_K_TEXT:
		case VPQ_K_BYTEA:
		case VPQ_K_UUID:
			if (lo.bytes == NULL || hi.bytes == NULL)
				return true;
			if (col->kind == VPQ_K_UUID && (type.id != VPQ_T_FIXED_BINARY || type.byte_width != 16))
				return true;
			out->min = bytes_datum(lo.bytes, lo.len);
			out->max = bytes_datum(hi.bytes, hi.len);
			out->bytewise = true;
			break;
		default:
			return true;
	}
	out->has_minmax = true;
	return true;
}
