/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * rows.c
 *	  Rows into a batch, a batch's values as Datums, and rows out of a batch
 *	  through a slot (pg_vector_executor.md §3.6, §3.7).
 *
 * Rows in.  A batch filled from rows -- a row child's slots, the slot path
 * of every access method without a source -- is written in its columns'
 * build shapes, the PostgreSQL format's with numerics as Datums, and then
 * converted to the shapes the format in effect gives them
 * (vexec_batch_apply_config).  Varlena values are copied into the batch's
 * arena with their headers, since a child's slot is valid only until its
 * next call: short headers kept for the types whose functions take them,
 * 4-byte headers at the type's alignment for the others; compressed and
 * external values kept as they are, detoasted when something needs their
 * bytes; expanded objects flattened, as datumCopy does
 * (PG19:src/backend/utils/adt/datum.c:143-154).
 *
 * Datums.  vexec_vec_datum() gives a row's value as PostgreSQL holds it,
 * whatever the column's shape: the fallback evaluator, a row parent and
 * tuplesort read values this way.  Under the PostgreSQL format it costs a
 * load, and varlena Datums point into the batch; under the Arrow format
 * bools are unpacked, temporal values shifted back, intervals rebuilt and
 * varlena values copied, unless their Datums were kept beside their views
 * (§3.4.2).  What it writes goes into the batch's arena, and lives as long
 * as the batch.
 *
 * Rows out.  vexec_batch_store_row() fills a virtual slot with a row's
 * values; the row is valid until the batch is reset, the usual rule for a
 * node's result slot.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/tupmacs.h"
#include "datatype/timestamp.h"
#include "executor/tuptable.h"
#include "utils/date.h"
#include "utils/expandeddatum.h"
#include "varatt.h"

#include "vexec.h"
#include "batch/batch.h"

/* Every column empty, in its build shape, room for a batch's rows. */
void
vexec_batch_begin_rows(VexecBatch *batch)
{
	int			i;

	if (batch->nrows != 0)
		elog(ERROR, "vexec batch begins its rows with %d rows in it", batch->nrows);
	for (i = 0; i < batch->ncols; i++)
	{
		VexecVec   *v = &batch->cols[i];
		VexecShape	shape;

		vexec_type_build_shape(v->type, &shape);
		vexec_vec_init(batch, v, &shape, 0);
		/* room for VEXEC_BATCH_ROWS: vec_init allocated for none */
		switch (shape.layout)
		{
			case VEXEC_FIXED:
				v->values = vexec_batch_alloc0(batch, (Size) shape.stride * VEXEC_BATCH_ROWS);
				break;
			case VEXEC_BYTE_BOOL:
				v->values = vexec_batch_alloc0(batch, VEXEC_BATCH_ROWS);
				break;
			case VEXEC_DATUM:
				v->values = vexec_batch_alloc0(batch, sizeof(Datum) * VEXEC_BATCH_ROWS);
				break;
			default:
				elog(ERROR, "unexpected build layout %d", shape.layout);
		}
	}
	batch->filling = true;
}

/*
 * A varlena or cstring Datum copied into the batch's arena, in the form the
 * batch holds it (above).
 */
Datum
vexec_varlena_copy(VexecBatch *batch, const VexecType *type, Datum value)
{
	varlena    *vl = (varlena *) DatumGetPointer(value);
	Size		align = Max(type->alignby, sizeof(int32));
	char	   *p;

	if (type->typlen == -2)
	{
		Size		len = strlen(DatumGetCString(value)) + 1;

		p = vexec_arena_alloc(&batch->arena, len, 1, NULL, NULL);
		memcpy(p, DatumGetCString(value), len);
		return PointerGetDatum(p);
	}

	if (VARATT_IS_EXTERNAL_EXPANDED(vl))
	{
		ExpandedObjectHeader *eoh = DatumGetEOHP(value);
		Size		len = EOH_get_flat_size(eoh);

		p = vexec_arena_alloc(&batch->arena, len, MAXIMUM_ALIGNOF, NULL, NULL);
		EOH_flatten_into(eoh, p, len);
		return PointerGetDatum(p);
	}
	if (VARATT_IS_EXTERNAL_INDIRECT(vl))
	{
		varatt_indirect redirect;

		VARATT_EXTERNAL_GET_POINTER(redirect, vl);
		return vexec_varlena_copy(batch, type, PointerGetDatum(redirect.pointer));
	}
	if (VARATT_IS_EXTERNAL(vl))
	{
		/* a TOAST pointer is copied as a pointer, not fetched (§3.5.2) */
		Size		len = VARSIZE_ANY(vl);

		p = vexec_arena_alloc(&batch->arena, len, 1, NULL, NULL);
		memcpy(p, vl, len);
		return PointerGetDatum(p);
	}
	if (VARATT_IS_SHORT(vl))
	{
		Size		len = VARSIZE_SHORT(vl);

		if (type->short_ok)
		{
			p = vexec_arena_alloc(&batch->arena, len, 1, NULL, NULL);
			memcpy(p, vl, len);
		}
		else
		{
			Size		datalen = len - VARHDRSZ_SHORT;

			p = vexec_arena_alloc(&batch->arena, datalen + VARHDRSZ, align, NULL, NULL);
			SET_VARSIZE(p, datalen + VARHDRSZ);
			memcpy(VARDATA(p), VARDATA_SHORT(vl), datalen);
		}
		return PointerGetDatum(p);
	}

	/* a 4-byte header, compressed or not */
	{
		Size		len = VARSIZE(vl);

		p = vexec_arena_alloc(&batch->arena, len, align, NULL, NULL);
		memcpy(p, vl, len);
		return PointerGetDatum(p);
	}
}

/*
 * Mark row `row` of a column being filled NULL, making its validity bitmap
 * on the first: the rows before it valid, and each row after it valid as
 * it is written (vexec_bit_set).
 */
void
vexec_vec_set_null(VexecBatch *batch, VexecVec *v, int row)
{
	if (v->validity == NULL)
	{
		int			i;

		v->validity = vexec_bitmap_alloc(batch, VEXEC_BATCH_ROWS, false);
		for (i = 0; i < row / 64; i++)
			v->validity[i] = ~UINT64CONST(0);
		if (row % 64)
			v->validity[row / 64] = (UINT64CONST(1) << (row % 64)) - 1;
	}
	vexec_bit_clear(v->validity, row);
}

/* Append a row of values, one per column, as a slot holds them. */
void
vexec_batch_add_values(VexecBatch *batch, const Datum *values, const bool *isnull)
{
	int			row = batch->nrows;
	int			i;

	if (row >= VEXEC_BATCH_ROWS)
		elog(ERROR, "vexec batch is full");
	if (!batch->filling)
		elog(ERROR, "vexec batch takes no rows once its columns are converted, encoded or compacted");

	for (i = 0; i < batch->ncols; i++)
	{
		VexecVec   *v = &batch->cols[i];
		const VexecType *type = v->type;

		if (isnull[i])
		{
			vexec_vec_set_null(batch, v, row);
			if (v->shape.layout == VEXEC_DATUM)
				((Datum *) v->values)[row] = (Datum) 0;
		}
		else
		{
			Datum		d = values[i];

			if (v->validity)
				vexec_bit_set(v->validity, row);
			switch (v->shape.layout)
			{
				case VEXEC_BYTE_BOOL:
					((uint8 *) v->values)[row] = DatumGetBool(d) ? 1 : 0;
					break;
				case VEXEC_FIXED:
					{
						char	   *p = (char *) v->values + (Size) row * v->shape.stride;

						if (type->typbyval)
							store_att_byval(p, d, type->typlen);
						else
							memcpy(p, DatumGetPointer(d), type->typlen);
						break;
					}
				case VEXEC_DATUM:
					((Datum *) v->values)[row] = vexec_varlena_copy(batch, type, d);
					break;
				default:
					elog(ERROR, "unexpected build layout %d", v->shape.layout);
			}
		}
		v->nvalues = row + 1;
	}
	batch->nrows = row + 1;
}

/*
 * Append a slot's row.  attnums names, for each column, the slot's
 * attribute it reads (1-based); NULL reads them in order.
 */
void
vexec_batch_add_slot(VexecBatch *batch, TupleTableSlot *slot, const AttrNumber *attnums)
{
	Datum		values[MaxTupleAttributeNumber];
	bool		isnull[MaxTupleAttributeNumber];
	int			maxatt = 0;
	int			i;

	for (i = 0; i < batch->ncols; i++)
		maxatt = Max(maxatt, attnums ? attnums[i] : i + 1);
	slot_getsomeattrs(slot, maxatt);
	for (i = 0; i < batch->ncols; i++)
	{
		int			a = (attnums ? attnums[i] : i + 1) - 1;

		values[i] = slot->tts_values[a];
		isnull[i] = slot->tts_isnull[a];
	}
	vexec_batch_add_values(batch, values, isnull);
}

/*
 * Bytes as a varlena Datum of a type, in the arena: a short header where
 * the type's functions take one and the value fits, else a 4-byte header
 * at the type's alignment.  A cstring type gets its terminating zero.
 */
static Datum
bytes_as_datum(VexecBatch *batch, const VexecType *type, const char *data, Size len)
{
	char	   *p;

	if (type->typlen == -2)
	{
		p = vexec_arena_alloc(&batch->arena, len + 1, 1, NULL, NULL);
		memcpy(p, data, len);
		p[len] = '\0';
	}
	else if (type->short_ok && len + VARHDRSZ_SHORT <= VARATT_SHORT_MAX)
	{
		p = vexec_arena_alloc(&batch->arena, len + VARHDRSZ_SHORT, 1, NULL, NULL);
		SET_VARSIZE_SHORT(p, len + VARHDRSZ_SHORT);
		memcpy(p + VARHDRSZ_SHORT, data, len);
	}
	else
	{
		p = vexec_arena_alloc(&batch->arena, len + VARHDRSZ,
							  Max(type->alignby, sizeof(int32)), NULL, NULL);
		SET_VARSIZE(p, len + VARHDRSZ);
		memcpy(VARDATA(p), data, len);
	}
	return PointerGetDatum(p);
}

/* A value of a FIXED column as a Datum. */
static Datum
fixed_datum(VexecBatch *batch, const VexecVec *v, int row)
{
	const VexecType *type = v->type;
	const char *p = (const char *) v->values + (Size) row * v->shape.stride;

	switch (type->tclass)
	{
		case VEXEC_TC_DATE:
			{
				int32		d;

				memcpy(&d, p, sizeof(int32));
				if (v->shape.arrow_values && !DATE_NOT_FINITE(d))
					d -= (int32) VEXEC_EPOCH_DAYS;
				return DateADTGetDatum(d);
			}
		case VEXEC_TC_TIMESTAMP:
			{
				int64		t;

				memcpy(&t, p, sizeof(int64));
				if (v->shape.arrow_values && !TIMESTAMP_NOT_FINITE(t))
					t -= VEXEC_EPOCH_USECS;
				return Int64GetDatum(t);
			}
		case VEXEC_TC_INTERVAL:
			if (v->shape.arrow_values)
			{
				/* {int32 months; int32 days; int64 ns} back to PostgreSQL's */
				Interval   *iv;
				int32		months;
				int32		days;
				int64		ns;

				memcpy(&months, p, sizeof(int32));
				memcpy(&days, p + 4, sizeof(int32));
				memcpy(&ns, p + 8, sizeof(int64));
				iv = (Interval *) vexec_arena_alloc(&batch->arena, sizeof(Interval),
													sizeof(int64), NULL, NULL);
				iv->time = ns / 1000;
				iv->day = days;
				iv->month = months;
				return PointerGetDatum(iv);
			}
			if (((uintptr_t) p) % ALIGNOF_DOUBLE != 0)
			{
				/* a source's interval, not aligned: a copy */
				char	   *q = vexec_arena_alloc(&batch->arena, sizeof(Interval),
												  ALIGNOF_DOUBLE, NULL, NULL);

				memcpy(q, p, sizeof(Interval));
				return PointerGetDatum(q);
			}
			return PointerGetDatum(p);
		case VEXEC_TC_FIXED:
			return fetch_att(p, true, type->typlen);
		default:				/* BYREF */
			if (((uintptr_t) p) % type->alignby == 0)
				return PointerGetDatum(p);
			else
			{
				/* packed (the Arrow format) and not aligned: a copy */
				char	   *q = vexec_arena_alloc(&batch->arena, type->typlen, type->alignby,
												  NULL, NULL);

				memcpy(q, p, type->typlen);
				return PointerGetDatum(q);
			}
	}
}

/* A row's value as PostgreSQL holds it, whatever the column's shape. */
Datum
vexec_vec_datum(VexecBatch *batch, const VexecVec *v, int row, bool *isnull)
{
	if (vexec_vec_isnull(v, row))
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;
	if (v->encoding == VEXEC_CONST)
		row = 0;
	else if (v->encoding == VEXEC_DICT)
	{
		bool		dnull;

		return vexec_vec_datum(batch, v->dictionary, v->codes[row], &dnull);
	}

	switch (v->shape.layout)
	{
		case VEXEC_FIXED:
			return fixed_datum(batch, v, row);
		case VEXEC_BYTE_BOOL:
			return BoolGetDatum(((const uint8 *) v->values)[row] != 0);
		case VEXEC_BIT_BOOL:
			return BoolGetDatum(vexec_bit((const uint64 *) v->values, row));
		case VEXEC_SCALED:
			{
				int128		value;

				if (v->shape.width == 8)
				{
					int64		v64;

					memcpy(&v64, (const char *) v->values + (Size) row * 8, sizeof(int64));
					value = v64;
				}
				else
					memcpy(&value, (const char *) v->values + (Size) row * 16, sizeof(int128));
				return vexec_scaled_to_numeric(batch, value, v->shape.scale);
			}
		case VEXEC_DATUM:
			return ((const Datum *) v->values)[row];
		case VEXEC_VIEW:
		case VEXEC_OFFSETS:
			{
				const char *p;
				Size		len;

				if (v->datums != NULL)
					return v->datums[row];
				vexec_vec_value_bytes(v, row, &p, &len);
				return bytes_as_datum(batch, v->type, p, len);
			}
	}
	elog(ERROR, "unexpected layout %d", v->shape.layout);
	return (Datum) 0;			/* keep the compiler quiet */
}

/*
 * A row of the batch into a virtual slot of as many attributes as the
 * batch has columns, in their order.
 */
void
vexec_batch_store_row(VexecBatch *batch, int row, TupleTableSlot *slot)
{
	int			i;

	Assert(slot->tts_tupleDescriptor->natts >= batch->ncols);
	ExecClearTuple(slot);
	for (i = 0; i < batch->ncols; i++)
		slot->tts_values[i] = vexec_vec_datum(batch, &batch->cols[i], row, &slot->tts_isnull[i]);
	for (; i < slot->tts_tupleDescriptor->natts; i++)
	{
		slot->tts_values[i] = (Datum) 0;
		slot->tts_isnull[i] = true;
	}
	ExecStoreVirtualTuple(slot);
}
