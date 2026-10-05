/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * convert.c
 *	  Conversions between a type's shapes (pg_vector_executor.md §3.4.2).
 *
 * One function per pair of layouts.  Each converts a column's values in
 * place: a flat column's, a dictionary's values, or a constant's one value.
 * A conversion the values cannot take returns false and leaves the column
 * as it was; the batch then keeps PostgreSQL's layout in that column, as
 * the format rules say:
 *
 *	timestamp	a finite value from 294247-01-10 04:00:54.775807 on
 *				reaches int64's maximum, +infinity's, when shifted to
 *				the Unix epoch
 *	interval	an infinite one, or one whose time part is beyond about 292
 *				years, overflows month_day_nano's nanoseconds
 *	numeric		NaN, an infinity, or a value of another display scale has
 *				no scaled form
 *	offsets		more than 2^31 - 1 bytes of values
 *
 * Dates always fit: every finite date shifted to the Unix epoch is an
 * int32, and the infinities stay at int32's extremes in both formats, as a
 * timestamp's do at int64's (§3.4.2).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "datatype/timestamp.h"
#include "utils/date.h"
#include "utils/timestamp.h"
#include "varatt.h"

#include "vexec.h"
#include "batch/batch.h"

/* the largest |time| an interval may have in nanoseconds: INT64_MAX / 1000 µs */
#define INTERVAL_NS_MAX_USECS	(PG_INT64_MAX / 1000)

static inline bool
valid(const VexecVec *v, int i)
{
	return v->validity == NULL || vexec_bit(v->validity, i);
}

/* bool: a byte a value <-> a bit a value */
static void
bool_byte_to_bit(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	const uint8 *bytes = v->values;
	uint64	   *bits = vexec_bitmap_alloc(batch, v->nvalues, false);
	int			w;

	for (w = 0; w < VEXEC_WORDS(v->nvalues); w++)
	{
		uint64		word = 0;
		int			base = w * 64;
		int			n = Min(64, v->nvalues - base);
		int			b;

		for (b = 0; b < n; b++)
			word |= (uint64) (bytes[base + b] != 0) << b;
		bits[w] = word;
	}
	v->values = bits;
	v->shape = *to;
}

static void
bool_bit_to_byte(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	const uint64 *bits = v->values;
	uint8	   *bytes = vexec_batch_alloc0(batch, TYPEALIGN(8, Max(v->nvalues, 1)));
	int			i;

	for (i = 0; i < v->nvalues; i++)
		bytes[i] = vexec_bit(bits, i);
	v->values = bytes;
	v->shape = *to;
}

/* date: days from 2000-01-01 <-> from 1970-01-01 */
static void
date_shift(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	const int32 *in = v->values;
	int32	   *out = vexec_batch_alloc0(batch, sizeof(int32) * Max(v->nvalues, 1));
	bool		to_arrow = to->arrow_values;
	int			i;

	for (i = 0; i < v->nvalues; i++)
	{
		int32		d = in[i];

		if (valid(v, i) && !DATE_NOT_FINITE(d))
		{
			if (to_arrow)
				d += (int32) VEXEC_EPOCH_DAYS;
			else
			{
				/* an Arrow date PostgreSQL cannot hold, from a source */
				if (d < -POSTGRES_EPOCH_JDATE + (int32) VEXEC_EPOCH_DAYS ||
					d > DATE_END_JULIAN - POSTGRES_EPOCH_JDATE - 1 + (int32) VEXEC_EPOCH_DAYS)
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("date out of range")));
				d -= (int32) VEXEC_EPOCH_DAYS;
			}
		}
		out[i] = d;
	}
	v->values = out;
	v->shape = *to;
}

/* timestamp[tz]: µs from 2000-01-01 <-> from 1970-01-01 */
static bool
timestamp_shift(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	const int64 *in = v->values;
	int64	   *out;
	bool		to_arrow = to->arrow_values;
	int			i;

	/*
	 * A finite value that would reach INT64_MAX shifted, which holds
	 * +infinity in both epochs: from 294247-01-10 04:00:54.775807 on, one
	 * microsecond earlier than int64's own overflow.
	 */
	if (to_arrow)
	{
		for (i = 0; i < v->nvalues; i++)
			if (valid(v, i) && !TIMESTAMP_NOT_FINITE(in[i]) &&
				in[i] >= PG_INT64_MAX - VEXEC_EPOCH_USECS)
				return false;	/* keeps PostgreSQL's epoch */
	}
	out = vexec_batch_alloc0(batch, sizeof(int64) * Max(v->nvalues, 1));
	for (i = 0; i < v->nvalues; i++)
	{
		int64		t = in[i];

		if (valid(v, i) && !TIMESTAMP_NOT_FINITE(t))
		{
			if (to_arrow)
				t += VEXEC_EPOCH_USECS;
			else
			{
				if (t < MIN_TIMESTAMP + VEXEC_EPOCH_USECS)
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range")));
				t -= VEXEC_EPOCH_USECS;
			}
		}
		out[i] = t;
	}
	v->values = out;
	v->shape = *to;
	return true;
}

/*
 * interval: PostgreSQL's {int64 µs; int32 days; int32 months}
 * (PG19:src/include/datatype/timestamp.h:47-53) <-> Arrow's month_day_nano
 * {int32 months; int32 days; int64 ns} (Schema.fbs:412-419).
 */
static bool
interval_restructure(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	const char *in = v->values;
	char	   *out;
	int			i;

	if (to->arrow_values)
	{
		for (i = 0; i < v->nvalues; i++)
		{
			const Interval *iv = (const Interval *) (in + (Size) i * 16);

			if (!valid(v, i))
				continue;
			if (INTERVAL_NOT_FINITE(iv) ||
				iv->time > INTERVAL_NS_MAX_USECS || iv->time < -INTERVAL_NS_MAX_USECS)
				return false;	/* keeps PostgreSQL's layout */
		}
	}
	out = vexec_batch_alloc0(batch, (Size) 16 * Max(v->nvalues, 1));
	for (i = 0; i < v->nvalues; i++)
	{
		char	   *o = out + (Size) i * 16;
		const char *p = in + (Size) i * 16;

		if (!valid(v, i))
			continue;
		if (to->arrow_values)
		{
			const Interval *iv = (const Interval *) p;
			int64		ns = iv->time * 1000;

			memcpy(o, &iv->month, sizeof(int32));
			memcpy(o + 4, &iv->day, sizeof(int32));
			memcpy(o + 8, &ns, sizeof(int64));
		}
		else
		{
			Interval   *iv = (Interval *) o;
			int32		months;
			int32		days;
			int64		ns;

			memcpy(&months, p, sizeof(int32));
			memcpy(&days, p + 4, sizeof(int32));
			memcpy(&ns, p + 8, sizeof(int64));
			iv->time = ns / 1000;
			iv->day = days;
			iv->month = months;
		}
	}
	v->values = out;
	v->shape = *to;
	return true;
}

/* fixed-length by-reference: one stride to another (timetz 16 <-> 12) */
static void
restride(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	char	   *out = vexec_batch_alloc0(batch, (Size) to->stride * Max(v->nvalues, 1));
	int			i;

	for (i = 0; i < v->nvalues; i++)
		memcpy(out + (Size) i * to->stride,
			   (const char *) v->values + (Size) i * v->shape.stride,
			   v->shape.width);
	v->values = out;
	v->shape = *to;
}

/* numeric: scaled -> Datums */
static void
scaled_to_datum(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	Datum	   *out = vexec_batch_alloc0(batch, sizeof(Datum) * Max(v->nvalues, 1));
	int			i;

	for (i = 0; i < v->nvalues; i++)
	{
		int128		value;

		if (!valid(v, i))
			continue;
		if (v->shape.width == 8)
		{
			int64		v64;

			memcpy(&v64, (const char *) v->values + (Size) i * 8, sizeof(int64));
			value = v64;
		}
		else
			memcpy(&value, (const char *) v->values + (Size) i * 16, sizeof(int128));
		out[i] = vexec_scaled_to_numeric(batch, value, v->shape.scale);
	}
	v->values = out;
	v->shape = *to;
}

/* numeric: Datums -> scaled, when every value has a scaled form */
static bool
datum_to_scaled(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	const Datum *in = v->values;
	char	   *out = vexec_batch_alloc0(batch, (Size) to->width * Max(v->nvalues, 1));
	MemoryContext old = MemoryContextSwitchTo(batch->mcxt);
	int			i;

	for (i = 0; i < v->nvalues; i++)
	{
		int128		value;

		if (!valid(v, i))
			continue;
		if (!vexec_numeric_to_scaled(in[i], to->scale, v->type->digits, to->width, &value))
		{
			MemoryContextSwitchTo(old);
			return false;		/* keeps its varlena layout */
		}
		if (to->width == 8)
		{
			int64		v64 = (int64) value;

			memcpy(out + (Size) i * 8, &v64, sizeof(int64));
		}
		else
			memcpy(out + (Size) i * 16, &value, sizeof(int128));
	}
	MemoryContextSwitchTo(old);
	v->values = out;
	v->shape = *to;
	return true;
}

/*
 * A Datum's payload: the bytes a view or offsets hold.  A compressed or
 * external value is detoasted into the arena first, and *datum then names
 * the detoasted copy, which is what the column keeps beside its views.
 */
static void
datum_payload(VexecBatch *batch, const VexecType *type, Datum *datum,
			  const char **p, Size *len)
{
	if (type->typlen == -2)
	{
		*p = DatumGetCString(*datum);
		*len = strlen(*p);
		return;
	}
	else
	{
		varlena    *vl = (varlena *) DatumGetPointer(*datum);

		if (VARATT_IS_EXTERNAL(vl) || VARATT_IS_COMPRESSED(vl))
		{
			MemoryContext old = MemoryContextSwitchTo(batch->mcxt);
			varlena    *plain = detoast_attr(vl);

			MemoryContextSwitchTo(old);
			*datum = vexec_varlena_copy(batch, type, PointerGetDatum(plain));
			pfree(plain);
			vl = (varlena *) DatumGetPointer(*datum);
		}
		*p = VARDATA_ANY(vl);
		*len = VARSIZE_ANY_EXHDR(vl);
	}
}

/* The arena's chunks as a column's data buffers, as they are now. */
static void
arena_buffers(VexecBatch *batch, VexecVec *v)
{
	int			i;
	int			n = batch->arena.nchunks;

	v->nbuffers = n;
	v->buffers = vexec_batch_alloc0(batch, sizeof(char *) * Max(n, 1));
	v->buffer_sizes = vexec_batch_alloc0(batch, sizeof(int64) * Max(n, 1));
	for (i = 0; i < n; i++)
	{
		v->buffers[i] = batch->arena.chunks[i].data;
		v->buffer_sizes[i] = batch->arena.chunks[i].used;
	}
}

static void
view_set(VexecView *view, const char *p, Size len, int chunk, int32 offset)
{
	memset(view, 0, sizeof(VexecView));
	view->inlined.size = (int32) len;
	if (len <= VEXEC_VIEW_INLINE)
		memcpy(view->inlined.data, p, len);
	else
	{
		memcpy(view->ref.prefix, p, 4);
		view->ref.buffer_index = chunk;
		view->ref.offset = offset;
	}
}

/*
 * Datums -> views over the same bytes.  The views point past each value's
 * header into the arena, which a batch's own values are in; a value
 * elsewhere is copied into it once.  The Datums are kept beside the views,
 * for fmgr, row parents and tuplesort (§3.4.2).
 */
static void
datum_to_view(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	const Datum *in = v->values;
	VexecView  *views = vexec_batch_alloc0(batch, sizeof(VexecView) * Max(v->nvalues, 1));
	Datum	   *datums = vexec_batch_alloc0(batch, sizeof(Datum) * Max(v->nvalues, 1));
	int			i;

	for (i = 0; i < v->nvalues; i++)
	{
		Datum		d;
		const char *p;
		Size		len;
		int			chunk = 0;
		int32		offset = 0;

		if (!valid(v, i))
			continue;
		d = in[i];
		datum_payload(batch, v->type, &d, &p, &len);
		if (len > PG_INT32_MAX)
			elog(ERROR, "value too long for a view");
		if (len > VEXEC_VIEW_INLINE &&
			!vexec_arena_find(&batch->arena, p, len, &chunk, &offset))
		{
			d = vexec_varlena_copy(batch, v->type, d);
			datum_payload(batch, v->type, &d, &p, &len);
			if (!vexec_arena_find(&batch->arena, p, len, &chunk, &offset))
				elog(ERROR, "a value copied into the batch is not in its arena");
		}
		view_set(&views[i], p, len, chunk, offset);
		datums[i] = d;
	}
	v->values = views;
	v->datums = datums;
	arena_buffers(batch, v);
	v->shape = *to;
}

/* views or offsets -> Datums: those kept beside, or copies with headers */
static void
bytes_to_datum(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	Datum	   *out;
	int			i;

	if (v->shape.layout == VEXEC_VIEW && v->datums != NULL)
		out = v->datums;
	else
	{
		out = vexec_batch_alloc0(batch, sizeof(Datum) * Max(v->nvalues, 1));
		for (i = 0; i < v->nvalues; i++)
		{
			bool		isnull;

			if (!valid(v, i))
				continue;
			out[i] = vexec_vec_datum(batch, v, i, &isnull);
		}
	}
	v->values = out;
	v->datums = NULL;
	v->buffers = NULL;
	v->buffer_sizes = NULL;
	v->nbuffers = 0;
	v->shape = *to;
}

/* Datums or views -> offsets: the payloads copied end to end */
static bool
to_offsets(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	int32	   *offsets = vexec_batch_alloc0(batch, sizeof(int32) * (v->nvalues + 1));
	const char **ptrs = palloc(sizeof(char *) * Max(v->nvalues, 1));
	Size	   *lens = palloc(sizeof(Size) * Max(v->nvalues, 1));
	Datum	   *datums = NULL;
	int64		total = 0;
	char	   *data;
	int			i;

	if (v->shape.layout == VEXEC_DATUM)
		datums = v->values;
	for (i = 0; i < v->nvalues; i++)
	{
		lens[i] = 0;
		ptrs[i] = NULL;
		if (!valid(v, i))
			continue;
		if (datums)
		{
			Datum		d = datums[i];

			datum_payload(batch, v->type, &d, &ptrs[i], &lens[i]);
		}
		else
			vexec_vec_value_bytes(v, i, &ptrs[i], &lens[i]);
		total += lens[i];
		if (total > PG_INT32_MAX)
		{
			pfree(ptrs);
			pfree(lens);
			return false;		/* §3.4.4: offsets keep a batch within int32 */
		}
	}
	data = vexec_batch_alloc(batch, Max(total, 1));
	for (i = 0; i < v->nvalues; i++)
	{
		if (lens[i] > 0)
			memcpy(data + offsets[i], ptrs[i], lens[i]);
		offsets[i + 1] = offsets[i] + (int32) lens[i];
	}
	pfree(ptrs);
	pfree(lens);

	v->values = offsets;
	v->datums = NULL;
	v->nbuffers = 1;
	v->buffers = vexec_batch_alloc0(batch, sizeof(char *));
	v->buffer_sizes = vexec_batch_alloc0(batch, sizeof(int64));
	v->buffers[0] = data;
	v->buffer_sizes[0] = Max(total, 1);
	v->shape = *to;
	return true;
}

/*
 * offsets -> views into the same buffer, without a copy.  The buffer's size
 * becomes the bytes the values use: an OFFSETS column's buffer may be
 * larger than that, and what lies past them was never written.
 */
static void
offsets_to_view(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	const int32 *off = v->values;
	VexecView  *views = vexec_batch_alloc0(batch, sizeof(VexecView) * Max(v->nvalues, 1));
	char	  **buffers = vexec_batch_alloc0(batch, sizeof(char *));
	int64	   *sizes = vexec_batch_alloc0(batch, sizeof(int64));
	int			i;

	for (i = 0; i < v->nvalues; i++)
	{
		if (!valid(v, i))
			continue;
		view_set(&views[i], v->buffers[0] + off[i], off[i + 1] - off[i], 0, off[i]);
	}
	buffers[0] = v->buffers[0];
	sizes[0] = off[v->nvalues];
	v->values = views;
	v->datums = NULL;
	v->buffers = buffers;
	v->buffer_sizes = sizes;
	v->nbuffers = 1;
	v->shape = *to;
}

static bool
convert_flat(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	const VexecShape *from = &v->shape;
	int			fl = from->layout;
	int			tl = to->layout;

	if (fl == VEXEC_BYTE_BOOL && tl == VEXEC_BIT_BOOL)
	{
		bool_byte_to_bit(batch, v, to);
		return true;
	}
	if (fl == VEXEC_BIT_BOOL && tl == VEXEC_BYTE_BOOL)
	{
		bool_bit_to_byte(batch, v, to);
		return true;
	}
	if (fl == VEXEC_FIXED && tl == VEXEC_FIXED)
	{
		switch (v->type->tclass)
		{
			case VEXEC_TC_DATE:
				date_shift(batch, v, to);
				return true;
			case VEXEC_TC_TIMESTAMP:
				return timestamp_shift(batch, v, to);
			case VEXEC_TC_INTERVAL:
				return interval_restructure(batch, v, to);
			case VEXEC_TC_BYREF:
				restride(batch, v, to);
				return true;
			default:
				break;
		}
	}
	else if (fl == VEXEC_SCALED || tl == VEXEC_SCALED)
	{
		VexecShape	datum_shape = {VEXEC_DATUM, false, 0, 0, 0};

		if (fl == VEXEC_SCALED)
		{
			scaled_to_datum(batch, v, &datum_shape);
			return tl == VEXEC_DATUM ? true : convert_flat(batch, v, to);
		}
		if (fl != VEXEC_DATUM)
		{
			VexecVec	save = *v;

			bytes_to_datum(batch, v, &datum_shape);
			if (!datum_to_scaled(batch, v, to))
			{
				*v = save;
				return false;
			}
			return true;
		}
		return datum_to_scaled(batch, v, to);
	}
	else if (fl == VEXEC_DATUM && tl == VEXEC_VIEW)
	{
		datum_to_view(batch, v, to);
		return true;
	}
	else if ((fl == VEXEC_VIEW || fl == VEXEC_OFFSETS) && tl == VEXEC_DATUM)
	{
		bytes_to_datum(batch, v, to);
		return true;
	}
	else if ((fl == VEXEC_DATUM || fl == VEXEC_VIEW) && tl == VEXEC_OFFSETS)
		return to_offsets(batch, v, to);
	else if (fl == VEXEC_OFFSETS && tl == VEXEC_VIEW)
	{
		offsets_to_view(batch, v, to);
		return true;
	}

	elog(ERROR, "vexec has no conversion from %s to %s for type %s",
		 vexec_shape_name(v->type, from), vexec_shape_name(v->type, to), v->type->name);
	return false;				/* keep the compiler quiet */
}

/*
 * Convert a column to another shape of its type.  A dictionary's values
 * are converted and its codes kept; a constant's one value.  False, and the
 * column unchanged, when its values cannot be held in that shape.
 */
bool
vexec_vec_convert(VexecBatch *batch, VexecVec *v, const VexecShape *to)
{
	if (vexec_shape_equal(&v->shape, to))
		return true;
	vexec_batch_column_changed(batch, v);
	if (v->encoding == VEXEC_DICT)
	{
		/* a dictionary of its own: copies of a column share theirs */
		VexecVec   *dict = MemoryContextAlloc(batch->mcxt, sizeof(VexecVec));

		*dict = *v->dictionary;
		if (!vexec_vec_convert(batch, dict, to))
			return false;
		v->dictionary = dict;
		v->shape = *to;
		return true;
	}
	return convert_flat(batch, v, to);
}

/*
 * Give every column of a batch the shape the format in effect and the
 * per-structure settings give its type, or PostgreSQL's where its values
 * cannot take that shape.  True when every column has its configured
 * shape.
 */
bool
vexec_batch_apply_config(VexecBatch *batch, const VexecLayoutConfig *cfg)
{
	bool		all = true;
	int			i;

	for (i = 0; i < batch->ncols; i++)
	{
		VexecVec   *v = &batch->cols[i];
		VexecShape	target;

		vexec_type_shape(v->type, cfg, &target);
		if (vexec_vec_convert(batch, v, &target))
			continue;
		all = false;

		/*
		 * A numeric that has no scaled form takes the format's layout for
		 * other numerics; a temporal value Arrow's cannot hold keeps
		 * PostgreSQL's; offsets beyond int32 stay views.
		 */
		if (target.layout == VEXEC_SCALED)
		{
			VexecLayoutConfig c = *cfg;

			c.numeric = VEXEC_NUMERIC_VARLENA;
			vexec_type_shape(v->type, &c, &target);
			if (!vexec_vec_convert(batch, v, &target))
			{
				VexecShape	view = {VEXEC_VIEW, false, 0, 0, 0};

				(void) vexec_vec_convert(batch, v, &view);
			}
		}
		else if (target.layout == VEXEC_OFFSETS)
		{
			VexecShape	view = {VEXEC_VIEW, false, 0, 0, 0};

			(void) vexec_vec_convert(batch, v, &view);
		}
	}
	return all;
}
