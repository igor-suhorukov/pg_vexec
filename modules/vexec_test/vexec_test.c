/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_test.c
 *	  vexec's batch layer, tested from SQL (pg_vector_executor.md §5 V0,
 *	  §6.2 "Layouts").
 *
 *	vexec_test.roundtrip(query, typmods)
 *		Every batch of a query's rows, every column of it through every
 *		layout its type has, between every pair of them and back, and through
 *		its dictionary and constant encodings; then out through a slot under
 *		both formats, and compacted.  Every value must come back as it went
 *		in.
 *	vexec_test.export(query, format, varlena, bool, temporal, numeric, dict)
 *		The export check: every batch, in a configuration, exported through
 *		the C Data Interface and checked three ways -- nanoarrow's FULL
 *		validation; vexec's own check of views, which nanoarrow does not
 *		validate (bounds, prefix, inline padding) [R:arrow_layouts §6.3], and
 *		of UTF-8; and every value read back through nanoarrow and compared
 *		with the row's.
 *	vexec_test.scaled(value, scale, width)
 *		A numeric through its scaled form and back.
 *
 * A module of the tests alone: nanoarrow is vendored here, and never in
 * vexec.  It reaches vexec by symbol, so vexec must be preloaded; without
 * it this library fails to load with an unresolved symbol.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "datatype/timestamp.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "varatt.h"

/*
 * nanoarrow first: it defines the C Data Interface's structs under the
 * guard vexec's copy of them shares (batch/arrow_abi.h), with the stream
 * interface beside them.  Its inline functions are Arrow's code, not held
 * to PostgreSQL's warnings.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wshadow=compatible-local"
#include "nanoarrow/nanoarrow.h"
#pragma GCC diagnostic pop

#include "vexec.h"
#include "batch/batch.h"
#include "batch/export.h"

PG_MODULE_MAGIC_EXT(
					.name = "vexec_test",
					.version = VEXEC_VERSION
);

PG_FUNCTION_INFO_V1(vexec_test_roundtrip);
PG_FUNCTION_INFO_V1(vexec_test_export);
PG_FUNCTION_INFO_V1(vexec_test_scaled);

/* A query's rows, a batch at a time, through an SPI cursor. */
typedef struct Rows
{
	Portal		portal;
	TupleDesc	desc;
	int			natts;
	VexecType **types;
	Datum	   *values;			/* [row * natts + col] */
	bool	   *nulls;
	int			nrows;
} Rows;

static void
rows_open(Rows *rows, const char *query, ArrayType *typmods)
{
	SPIPlanPtr	plan;
	int			i;
	int32	   *mods = NULL;
	int			nmods = 0;

	plan = SPI_prepare(query, 0, NULL);
	if (plan == NULL)
		elog(ERROR, "vexec_test: SPI_prepare failed: %s", SPI_result_code_string(SPI_result));
	rows->portal = SPI_cursor_open(NULL, plan, NULL, NULL, true);
	rows->desc = CreateTupleDescCopy(rows->portal->tupDesc);
	rows->natts = rows->desc->natts;
	if (typmods != NULL)
	{
		Datum	   *elems;
		bool	   *enulls;

		deconstruct_array_builtin(typmods, INT4OID, &elems, &enulls, &nmods);
		mods = palloc(sizeof(int32) * Max(nmods, 1));
		for (i = 0; i < nmods; i++)
			mods[i] = enulls[i] ? -1 : DatumGetInt32(elems[i]);
	}
	rows->types = palloc(sizeof(VexecType *) * Max(rows->natts, 1));
	for (i = 0; i < rows->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(rows->desc, i);
		int32		typmod = (i < nmods && mods[i] >= 0) ? mods[i] : att->atttypmod;

		rows->types[i] = vexec_type_make(att->atttypid, typmod, att->attcollation);
	}
	rows->values = palloc(sizeof(Datum) * VEXEC_BATCH_ROWS * Max(rows->natts, 1));
	rows->nulls = palloc(sizeof(bool) * VEXEC_BATCH_ROWS * Max(rows->natts, 1));
}

/* The next batch's rows, deformed; false at the end. */
static bool
rows_next(Rows *rows)
{
	uint64		i;

	SPI_cursor_fetch(rows->portal, true, VEXEC_BATCH_ROWS);
	rows->nrows = (int) SPI_processed;
	for (i = 0; i < SPI_processed; i++)
		heap_deform_tuple(SPI_tuptable->vals[i], SPI_tuptable->tupdesc,
						  &rows->values[i * rows->natts], &rows->nulls[i * rows->natts]);
	return rows->nrows > 0;
}

static void
rows_fill(Rows *rows, VexecBatch *batch)
{
	int			r;

	vexec_batch_reset(batch);
	vexec_batch_begin_rows(batch);
	for (r = 0; r < rows->nrows; r++)
		vexec_batch_add_values(batch, &rows->values[r * rows->natts], &rows->nulls[r * rows->natts]);
}

static char *
value_text(const VexecType *type, Datum d, bool isnull)
{
	Oid			out;
	bool		varlena;

	if (isnull)
		return "NULL";
	getTypeOutputInfo(type->typid, &out, &varlena);
	return OidOutputFunctionCall(out, d);
}

/* Whether two values of a type are the same value, bytes and all. */
static bool
same_value(const VexecType *type, Datum a, Datum b)
{
	if (datum_image_eq(a, b, type->typbyval, type->typlen))
		return true;
	/* a numeric may come back in the short form where it went in long */
	if (type->tclass == VEXEC_TC_NUMERIC)
		return strcmp(value_text(type, a, false), value_text(type, b, false)) == 0;
	return false;
}

/* Every value of a column must be the row's. */
static void
check_vec(VexecBatch *batch, const VexecVec *v, int col, Rows *rows, const char *what)
{
	int			r;

	for (r = 0; r < rows->nrows; r++)
	{
		bool		isnull;
		Datum		d = vexec_vec_datum(batch, v, r, &isnull);
		Datum		want = rows->values[r * rows->natts + col];
		bool		want_null = rows->nulls[r * rows->natts + col];

		if (isnull != want_null || (!isnull && !same_value(v->type, d, want)))
			ereport(ERROR,
					(errmsg("vexec_test: column %d (%s), %s: row %d is %s, not %s",
							col + 1, v->type->name, what, r,
							value_text(v->type, d, isnull),
							value_text(v->type, want, want_null))));
	}
}

static const char *
shape_text(const VexecVec *v)
{
	const char *enc = v->encoding == VEXEC_DICT ? " dictionary" :
		v->encoding == VEXEC_CONST ? " constant" : "";

	return psprintf("%s%s", vexec_shape_name(v->type, &v->shape), enc);
}

typedef struct ColumnStats
{
	int			pairs;			/* conversions checked */
	int			kept;			/* conversions the values could not take */
	int			dicts;
	int			consts;
} ColumnStats;

/* One column of a batch, through every shape, every pair of them, and back. */
static void
roundtrip_column(VexecBatch *batch, int col, Rows *rows, ColumnStats *stats)
{
	VexecVec	built = batch->cols[col];
	VexecShape	shapes[8];
	int			n = vexec_type_shapes(built.type, shapes, lengthof(shapes));
	int			a,
				b;

	check_vec(batch, &built, col, rows, "as built");
	for (a = 0; a < n; a++)
	{
		VexecVec	A = built;

		if (!vexec_vec_convert(batch, &A, &shapes[a]))
		{
			stats->kept++;
			continue;
		}
		check_vec(batch, &A, col, rows, psprintf("in %s", shape_text(&A)));

		for (b = 0; b < n; b++)
		{
			VexecVec	B = A;
			VexecVec	back;

			if (!vexec_vec_convert(batch, &B, &shapes[b]))
			{
				stats->kept++;
				continue;
			}
			check_vec(batch, &B, col, rows,
					  psprintf("from %s to %s", shape_text(&A), shape_text(&B)));
			back = B;
			if (!vexec_vec_convert(batch, &back, &shapes[a]))
				ereport(ERROR,
						(errmsg("vexec_test: column %d (%s): %s does not convert back to %s",
								col + 1, built.type->name, shape_text(&B), shape_text(&A))));
			check_vec(batch, &back, col, rows,
					  psprintf("from %s to %s and back", shape_text(&A), shape_text(&B)));
			stats->pairs++;
		}

		/* dictionary-encoded, its dictionary converted, and flattened */
		{
			VexecVec	D = A;

			if (vexec_vec_encode_dict(batch, &D))
			{
				stats->dicts++;
				check_vec(batch, &D, col, rows, psprintf("as %s", shape_text(&D)));
				for (b = 0; b < n; b++)
				{
					VexecVec	D2 = D;

					if (!vexec_vec_convert(batch, &D2, &shapes[b]))
						continue;
					check_vec(batch, &D2, col, rows, psprintf("as %s", shape_text(&D2)));
					vexec_vec_flatten(batch, &D2);
					check_vec(batch, &D2, col, rows, psprintf("as %s, flattened", shape_text(&D2)));
				}
			}
		}
		{
			VexecVec	C = A;

			if (vexec_vec_encode_const(batch, &C))
			{
				stats->consts++;
				check_vec(batch, &C, col, rows, psprintf("as %s", shape_text(&C)));
				for (b = 0; b < n; b++)
				{
					VexecVec	C2 = C;

					if (!vexec_vec_convert(batch, &C2, &shapes[b]))
						continue;
					check_vec(batch, &C2, col, rows, psprintf("as %s", shape_text(&C2)));
					vexec_vec_flatten(batch, &C2);
					check_vec(batch, &C2, col, rows, psprintf("as %s, flattened", shape_text(&C2)));
				}
			}
		}
	}
}

/* The batch's rows out through a virtual slot, as a row parent takes them. */
static void
check_rows_out(VexecBatch *batch, Rows *rows, TupleTableSlot *slot, const char *what)
{
	int			r,
				c;

	for (r = 0; r < batch->nrows; r++)
	{
		vexec_batch_store_row(batch, r, slot);
		for (c = 0; c < rows->natts; c++)
		{
			Datum		want = rows->values[r * rows->natts + c];
			bool		want_null = rows->nulls[r * rows->natts + c];

			if (slot->tts_isnull[c] != want_null ||
				(!want_null && !same_value(rows->types[c], slot->tts_values[c], want)))
				ereport(ERROR,
						(errmsg("vexec_test: the row-out slot, %s: row %d column %d (%s) is %s, not %s",
								what, r, c + 1, rows->types[c]->name,
								value_text(rows->types[c], slot->tts_values[c], slot->tts_isnull[c]),
								value_text(rows->types[c], want, want_null))));
		}
	}
}

/*
 * Keep every row but those r % 3 == 1, compact, and check that the rows
 * that stayed are the ones selected, in order.
 */
static void
check_compaction(VexecBatch *batch, Rows *rows)
{
	int			kept[VEXEC_BATCH_ROWS];
	int			nkept = 0;
	int			r,
				c;

	batch->selection = vexec_bitmap_alloc(batch, batch->nrows, false);
	for (r = 0; r < batch->nrows; r++)
		if (r % 3 != 1)
		{
			vexec_bit_set(batch->selection, r);
			kept[nkept++] = r;
		}
	if (vexec_batch_selected(batch) != nkept)
		elog(ERROR, "vexec_test: the selection counts %d rows, not %d",
			 vexec_batch_selected(batch), nkept);
	vexec_batch_compact(batch);
	if (batch->nrows != nkept)
		elog(ERROR, "vexec_test: compaction kept %d rows, not %d", batch->nrows, nkept);
	for (r = 0; r < nkept; r++)
		for (c = 0; c < rows->natts; c++)
		{
			bool		isnull;
			Datum		d = vexec_vec_datum(batch, &batch->cols[c], r, &isnull);
			Datum		want = rows->values[kept[r] * rows->natts + c];
			bool		want_null = rows->nulls[kept[r] * rows->natts + c];

			if (isnull != want_null || (!isnull && !same_value(rows->types[c], d, want)))
				ereport(ERROR,
						(errmsg("vexec_test: after compaction, %s: row %d column %d (%s) is %s, not %s",
								shape_text(&batch->cols[c]), r, c + 1, rows->types[c]->name,
								value_text(rows->types[c], d, isnull),
								value_text(rows->types[c], want, want_null))));
		}
}

/*
 * vexec_test.roundtrip(query text, typmods int4[] DEFAULT NULL)
 *	RETURNS TABLE (col int, type text, layouts int, pairs int, kept int,
 *				   dictionaries int, constants int, rows int8, batches int)
 *
 * typmods overrides the query's columns' typmods, one an element, NULL or
 * -1 to keep it: so that a numeric of another display scale than a typmod's
 * -- which coercion never makes, and a table access method storing Datums
 * might -- can be fed to the scaled layout.
 */
Datum
vexec_test_roundtrip(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ArrayType  *typmods = PG_ARGISNULL(1) ? NULL : PG_GETARG_ARRAYTYPE_P(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext per_query;
	MemoryContext work;
	Rows		rows;
	VexecBatch *batch;
	ColumnStats *stats;
	TupleTableSlot *slot;
	int64		nrows = 0;
	int			nbatches = 0;
	int			c;
	VexecLayoutConfig pg = {VEXEC_FORMAT_POSTGRES, VEXEC_VARLENA_FORMAT, VEXEC_BOOL_FORMAT,
	VEXEC_TEMPORAL_FORMAT, VEXEC_NUMERIC_FORMAT};
	VexecLayoutConfig arrow = {VEXEC_FORMAT_ARROW, VEXEC_VARLENA_FORMAT, VEXEC_BOOL_FORMAT,
	VEXEC_TEMPORAL_FORMAT, VEXEC_NUMERIC_FORMAT};

	InitMaterializedSRF(fcinfo, 0);
	per_query = CurrentMemoryContext;

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "vexec_test: SPI_connect failed");
	work = MemoryContextSwitchTo(per_query);
	MemoryContextSwitchTo(work);

	rows_open(&rows, query, typmods);
	batch = vexec_batch_create(CurrentMemoryContext, rows.natts, rows.types);
	stats = palloc0(sizeof(ColumnStats) * Max(rows.natts, 1));
	slot = MakeSingleTupleTableSlot(rows.desc, &TTSOpsVirtual);

	while (rows_next(&rows))
	{
		rows_fill(&rows, batch);
		for (c = 0; c < rows.natts; c++)
			roundtrip_column(batch, c, &rows, &stats[c]);

		/* out through a slot, as each format holds the batch */
		check_rows_out(batch, &rows, slot, "as built");
		(void) vexec_batch_apply_config(batch, &arrow);
		check_rows_out(batch, &rows, slot, "in the Arrow format");
		(void) vexec_batch_apply_config(batch, &pg);
		check_rows_out(batch, &rows, slot, "in the PostgreSQL format");
		check_compaction(batch, &rows);

		nrows += rows.nrows;
		nbatches++;
		SPI_freetuptable(SPI_tuptable);
	}

	for (c = 0; c < rows.natts; c++)
	{
		Datum		values[9];
		bool		nulls[9] = {0};
		VexecShape	shapes[8];

		values[0] = Int32GetDatum(c + 1);
		values[1] = CStringGetTextDatum(format_type_with_typemod(rows.types[c]->typid,
																 rows.types[c]->typmod));
		values[2] = Int32GetDatum(vexec_type_shapes(rows.types[c], shapes, lengthof(shapes)));
		values[3] = Int32GetDatum(stats[c].pairs);
		values[4] = Int32GetDatum(stats[c].kept);
		values[5] = Int32GetDatum(stats[c].dicts);
		values[6] = Int32GetDatum(stats[c].consts);
		values[7] = Int64GetDatum(nrows);
		values[8] = Int32GetDatum(nbatches);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	ExecDropSingleTupleTableSlot(slot);
	SPI_cursor_close(rows.portal);
	SPI_finish();
	PG_RETURN_VOID();
}

/* ---- the export check ---- */

static int
setting_value(const char *name, const char *value, const char *const *names, int n)
{
	int			i;

	for (i = 0; i < n; i++)
		if (strcmp(value, names[i]) == 0)
			return i;
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("vexec_test: \"%s\" is not a value of %s", value, name)));
	return 0;
}

static void
na_check(int code, struct ArrowError *error, const char *what, int col)
{
	if (code != NANOARROW_OK)
		ereport(ERROR,
				(errmsg("vexec_test: export check, column %d: %s: %s", col, what,
						ArrowErrorMessage(error))));
}

/*
 * vexec's own check of a view array, which nanoarrow's FULL validation
 * does not make (Columnar.rst:492-524): each view's size, its inline bytes
 * zero past the value, and a long value's buffer index, its range within
 * that buffer, and its prefix.
 */
static void
check_views(struct ArrowArray *a, int col)
{
	const uint64 *validity = a->buffers[0];
	const VexecView *views = a->buffers[1];
	int64_t		nvariadic = a->n_buffers - 3;
	const int64 *sizes = a->buffers[a->n_buffers - 1];
	int64_t		i;

	if (nvariadic < 0)
		elog(ERROR, "vexec_test: column %d: a view array of %ld buffers", col, (long) a->n_buffers);
	for (i = 0; i < a->length; i++)
	{
		const VexecView *v = &views[a->offset + i];
		int64_t		row = a->offset + i;

		if (validity && !((validity[row >> 6] >> (row & 63)) & 1))
			continue;
		if (v->inlined.size < 0)
			elog(ERROR, "vexec_test: column %d row %ld: a view of size %d", col, (long) i, v->inlined.size);
		if (v->inlined.size <= VEXEC_VIEW_INLINE)
		{
			int			k;

			for (k = v->inlined.size; k < VEXEC_VIEW_INLINE; k++)
				if (v->inlined.data[k] != 0)
					elog(ERROR, "vexec_test: column %d row %ld: an inline view not zero past its %d bytes",
						 col, (long) i, v->inlined.size);
		}
		else
		{
			const char *data;

			if (v->ref.buffer_index < 0 || v->ref.buffer_index >= nvariadic)
				elog(ERROR, "vexec_test: column %d row %ld: a view into buffer %d of %ld",
					 col, (long) i, v->ref.buffer_index, (long) nvariadic);
			if (v->ref.offset < 0 ||
				(int64) v->ref.offset + v->ref.size > sizes[v->ref.buffer_index])
				elog(ERROR, "vexec_test: column %d row %ld: a view of %d bytes at %d past its buffer's %ld",
					 col, (long) i, v->ref.size, v->ref.offset, (long) sizes[v->ref.buffer_index]);
			data = (const char *) a->buffers[2 + v->ref.buffer_index] + v->ref.offset;
			if (memcmp(v->ref.prefix, data, 4) != 0)
				elog(ERROR, "vexec_test: column %d row %ld: a view's prefix is not its value's",
					 col, (long) i);
		}
	}
}

/* A decimal's digits, at a scale, as numeric_out prints its value. */
static char *
decimal_text(struct ArrowDecimal *dec, int scale)
{
	struct ArrowBuffer buf;
	char	   *digits;
	bool		neg;
	int			len;
	StringInfoData out;

	ArrowBufferInit(&buf);
	if (ArrowDecimalAppendDigitsToBuffer(dec, &buf) != NANOARROW_OK)
		elog(ERROR, "vexec_test: could not print a decimal");
	digits = pnstrdup((char *) buf.data, buf.size_bytes);
	ArrowBufferReset(&buf);

	neg = digits[0] == '-';
	if (neg)
		digits++;
	len = strlen(digits);
	initStringInfo(&out);
	if (neg)
		appendStringInfoChar(&out, '-');
	if (scale == 0)
		appendStringInfoString(&out, digits);
	else if (len > scale)
	{
		appendBinaryStringInfo(&out, digits, len - scale);
		appendStringInfoChar(&out, '.');
		appendStringInfoString(&out, digits + len - scale);
	}
	else
	{
		appendStringInfoString(&out, "0.");
		for (; len < scale; len++)
			appendStringInfoChar(&out, '0');
		appendStringInfoString(&out, digits);
	}
	if (strcmp(out.data, "-0") == 0 || (neg && strspn(out.data + 1, "0.") == strlen(out.data + 1)))
		return out.data + 1;	/* numeric prints no negative zero */
	return out.data;
}

/* The bytes a varlena value holds: its payload, detoasted. */
static void
payload(const VexecType *type, Datum d, const char **p, Size *len)
{
	if (type->typlen == -2)
	{
		*p = DatumGetCString(d);
		*len = strlen(*p);
	}
	else
	{
		varlena    *vl = pg_detoast_datum_packed((varlena *) DatumGetPointer(d));

		*p = VARDATA_ANY(vl);
		*len = VARSIZE_ANY_EXHDR(vl);
	}
}

static void
mismatch(int col, int64 row, const VexecType *type, Datum d, const char *what)
{
	ereport(ERROR,
			(errmsg("vexec_test: export check, column %d (%s) row %ld: %s; the row holds %s",
					col, type->name, (long) row, what, value_text(type, d, false))));
}

/* Row r of an exported column, read through nanoarrow, against the row's value. */
static void
check_value(struct ArrowArrayView *view, struct ArrowSchemaView *sv, int64 r,
			const VexecType *type, Datum d, bool isnull, int col)
{
	struct ArrowArrayView *vals = view;
	int64		i = r;

	if (ArrowArrayViewIsNull(view, r) != isnull)
		ereport(ERROR,
				(errmsg("vexec_test: export check, column %d (%s) row %ld: %s where the row has %s",
						col, type->name, (long) r, isnull ? "a value" : "NULL",
						isnull ? "NULL" : value_text(type, d, false))));
	if (isnull)
		return;
	if (view->dictionary != NULL)
	{
		vals = view->dictionary;
		i = ArrowArrayViewGetIntUnsafe(view, r);
	}

	switch (type->tclass)
	{
		case VEXEC_TC_BOOL:
			if ((ArrowArrayViewGetIntUnsafe(vals, i) != 0) != DatumGetBool(d))
				mismatch(col, r, type, d, "another bool");
			break;
		case VEXEC_TC_DATE:
			{
				int64		want = DatumGetDateADT(d);

				if (sv->type == NANOARROW_TYPE_DATE32)
					want += VEXEC_EPOCH_DAYS;
				if (ArrowArrayViewGetIntUnsafe(vals, i) != want)
					mismatch(col, r, type, d, psprintf("%ld days", (long) ArrowArrayViewGetIntUnsafe(vals, i)));
				break;
			}
		case VEXEC_TC_TIMESTAMP:
			{
				int64		want = DatumGetTimestamp(d);

				if (sv->type == NANOARROW_TYPE_TIMESTAMP)
					want += VEXEC_EPOCH_USECS;
				if (ArrowArrayViewGetIntUnsafe(vals, i) != want)
					mismatch(col, r, type, d, psprintf("%ld µs", (long) ArrowArrayViewGetIntUnsafe(vals, i)));
				break;
			}
		case VEXEC_TC_INTERVAL:
			{
				Interval   *iv = DatumGetIntervalP(d);

				if (sv->type == NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO)
				{
					struct ArrowInterval ai;

					ArrowIntervalInit(&ai, NANOARROW_TYPE_INTERVAL_MONTH_DAY_NANO);
					ArrowArrayViewGetIntervalUnsafe(vals, i, &ai);
					if (ai.months != iv->month || ai.days != iv->day || ai.ns != iv->time * 1000)
						mismatch(col, r, type, d, "another month_day_nano");
				}
				else
				{
					struct ArrowBufferView b = ArrowArrayViewGetBytesUnsafe(vals, i);

					if (b.size_bytes != 16 || memcmp(b.data.data, iv, 16) != 0)
						mismatch(col, r, type, d, "other bytes");
				}
				break;
			}
		case VEXEC_TC_NUMERIC:
			if (sv->type == NANOARROW_TYPE_DECIMAL64 || sv->type == NANOARROW_TYPE_DECIMAL128)
			{
				struct ArrowDecimal dec;
				char	   *got;
				char	   *want = value_text(type, d, false);

				ArrowDecimalInit(&dec, sv->decimal_bitwidth, sv->decimal_precision, sv->decimal_scale);
				ArrowArrayViewGetDecimalUnsafe(vals, i, &dec);
				got = decimal_text(&dec, sv->decimal_scale);
				if (strcmp(got, want) != 0)
					mismatch(col, r, type, d, psprintf("decimal %s", got));
				break;
			}
			pg_fallthrough;
		case VEXEC_TC_VARLENA:
		case VEXEC_TC_CSTRING:
			{
				struct ArrowBufferView b = ArrowArrayViewGetBytesUnsafe(vals, i);
				const char *p;
				Size		len;

				payload(type, d, &p, &len);
				if (b.size_bytes != (int64) len || memcmp(b.data.data, p, len) != 0)
					mismatch(col, r, type, d, psprintf("%ld other bytes", (long) b.size_bytes));
				if ((sv->type == NANOARROW_TYPE_STRING || sv->type == NANOARROW_TYPE_STRING_VIEW) &&
					!pg_verify_mbstr(PG_UTF8, b.data.data, (int) b.size_bytes, true))
					mismatch(col, r, type, d, "utf8 that is not UTF-8");
				break;
			}
		case VEXEC_TC_BYREF:
			{
				struct ArrowBufferView b = ArrowArrayViewGetBytesUnsafe(vals, i);

				if (b.size_bytes != type->typlen ||
					memcmp(b.data.data, DatumGetPointer(d), type->typlen) != 0)
					mismatch(col, r, type, d, "other bytes");
				break;
			}
		case VEXEC_TC_FIXED:
			switch (type->basetype)
			{
				case FLOAT4OID:
					{
						float4		got = (float4) ArrowArrayViewGetDoubleUnsafe(vals, i);
						float4		want = DatumGetFloat4(d);

						if (!(isnan(got) && isnan(want)) &&
							(got != want || signbit(got) != signbit(want)))
							mismatch(col, r, type, d, "another float4");
						break;
					}
				case FLOAT8OID:
					{
						float8		got = ArrowArrayViewGetDoubleUnsafe(vals, i);
						float8		want = DatumGetFloat8(d);

						if (!(isnan(got) && isnan(want)) &&
							(got != want || signbit(got) != signbit(want)))
							mismatch(col, r, type, d, "another float8");
						break;
					}
				case OIDOID:
					if (ArrowArrayViewGetUIntUnsafe(vals, i) != DatumGetObjectId(d))
						mismatch(col, r, type, d, "another oid");
					break;
				default:
					{
						int64		want;

						switch (type->typlen)
						{
							case 1:
								want = DatumGetChar(d);
								break;
							case 2:
								want = DatumGetInt16(d);
								break;
							case 4:
								want = DatumGetInt32(d);
								break;
							default:
								want = DatumGetInt64(d);
								break;
						}
						if (ArrowArrayViewGetIntUnsafe(vals, i) != want)
							mismatch(col, r, type, d,
									 psprintf("%ld", (long) ArrowArrayViewGetIntUnsafe(vals, i)));
						break;
					}
			}
			break;
	}
}

static const char *const format_names[] = {"postgres", "arrow"};
static const char *const varlena_names[] = {"format", "datum", "view", "offsets"};
static const char *const bool_names[] = {"format", "byte", "bit"};
static const char *const temporal_names[] = {"format", "postgres", "arrow"};
static const char *const numeric_names[] = {"format", "scaled", "varlena"};

static char *
metadata_value(const char *metadata, const char *key)
{
	struct ArrowStringView value = {NULL, 0};

	if (metadata == NULL ||
		ArrowMetadataGetValue(metadata, ArrowCharView(key), &value) != NANOARROW_OK ||
		value.data == NULL)
		return NULL;
	return pnstrdup(value.data, value.size_bytes);
}

/*
 * vexec_test.export(query text, format text, varlena text, bool text,
 *					 temporal text, numeric text, dict bool)
 *	RETURNS TABLE (col int, type text, layout text, arrow text,
 *				   extension text, rows int8, nulls int8)
 */
Datum
vexec_test_export(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	VexecLayoutConfig cfg;
	bool		dict = PG_GETARG_BOOL(6);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Rows		rows;
	VexecBatch *batch;
	char	  **names;
	char	  **arrow_formats;
	char	  **extensions;
	char	  **layouts;
	int64	   *nnulls;
	int64		nrows = 0;
	int			c;

	cfg.format = (uint8) setting_value("vexec.batch_format", text_to_cstring(PG_GETARG_TEXT_PP(1)),
									   format_names, lengthof(format_names));
	cfg.varlena = (uint8) setting_value("vexec.batch_varlena_layout", text_to_cstring(PG_GETARG_TEXT_PP(2)),
										varlena_names, lengthof(varlena_names));
	cfg.boolean = (uint8) setting_value("vexec.batch_bool_layout", text_to_cstring(PG_GETARG_TEXT_PP(3)),
										bool_names, lengthof(bool_names));
	cfg.temporal = (uint8) setting_value("vexec.batch_temporal_layout", text_to_cstring(PG_GETARG_TEXT_PP(4)),
										 temporal_names, lengthof(temporal_names));
	cfg.numeric = (uint8) setting_value("vexec.batch_numeric_layout", text_to_cstring(PG_GETARG_TEXT_PP(5)),
										numeric_names, lengthof(numeric_names));

	InitMaterializedSRF(fcinfo, 0);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "vexec_test: SPI_connect failed");

	rows_open(&rows, query, NULL);
	batch = vexec_batch_create(CurrentMemoryContext, rows.natts, rows.types);
	names = palloc0(sizeof(char *) * Max(rows.natts, 1));
	arrow_formats = palloc0(sizeof(char *) * Max(rows.natts, 1));
	extensions = palloc0(sizeof(char *) * Max(rows.natts, 1));
	layouts = palloc0(sizeof(char *) * Max(rows.natts, 1));
	nnulls = palloc0(sizeof(int64) * Max(rows.natts, 1));
	for (c = 0; c < rows.natts; c++)
		names[c] = pstrdup(NameStr(TupleDescAttr(rows.desc, c)->attname));

	while (rows_next(&rows))
	{
		struct ArrowSchema schema;
		struct ArrowArray array;
		struct ArrowArrayView view;
		struct ArrowError error;
		int			r;

		rows_fill(&rows, batch);
		(void) vexec_batch_apply_config(batch, &cfg);
		if (dict)
			for (c = 0; c < rows.natts; c++)
				(void) vexec_vec_encode_dict(batch, &batch->cols[c]);
		for (c = 0; c < rows.natts; c++)
		{
			const char *s = shape_text(&batch->cols[c]);

			/* a column's layout may differ from batch to batch: name them all */
			if (layouts[c] == NULL)
				layouts[c] = pstrdup(s);
			else if (strstr(layouts[c], s) == NULL)
				layouts[c] = psprintf("%s, %s", layouts[c], s);
		}

		vexec_batch_export(batch, (const char *const *) names, &schema, &array);

		memset(&error, 0, sizeof(error));
		if (strcmp(schema.format, "+s") != 0 || schema.n_children != rows.natts ||
			array.n_children != rows.natts || array.length != rows.nrows)
			elog(ERROR, "vexec_test: the export is not a record batch of %d columns and %d rows",
				 rows.natts, rows.nrows);
		na_check(ArrowArrayViewInitFromSchema(&view, &schema, &error), &error, "its schema", 0);
		na_check(ArrowArrayViewSetArray(&view, &array, &error), &error, "its array", 0);
		na_check(ArrowArrayViewValidate(&view, NANOARROW_VALIDATION_LEVEL_FULL, &error),
				 &error, "nanoarrow's full validation", 0);

		for (c = 0; c < rows.natts; c++)
		{
			struct ArrowSchemaView sv;
			struct ArrowSchema *cs = schema.children[c];
			struct ArrowArray *ca = array.children[c];
			struct ArrowSchema *vs = cs->dictionary ? cs->dictionary : cs;
			struct ArrowArray *va = ca->dictionary ? ca->dictionary : ca;
			char	   *ext;

			na_check(ArrowSchemaViewInit(&sv, vs, &error), &error, "its column's schema", c + 1);
			if (strcmp(cs->name, names[c]) != 0)
				elog(ERROR, "vexec_test: column %d is named %s", c + 1, cs->name);
			if (metadata_value(cs->metadata, "pg_type") == NULL)
				elog(ERROR, "vexec_test: column %d has no pg_type", c + 1);
			if (sv.type == NANOARROW_TYPE_STRING_VIEW || sv.type == NANOARROW_TYPE_BINARY_VIEW)
				check_views(va, c + 1);
			for (r = 0; r < rows.nrows; r++)
				check_value(view.children[c], &sv, r, rows.types[c],
							rows.values[r * rows.natts + c], rows.nulls[r * rows.natts + c], c + 1);
			nnulls[c] += ca->null_count;

			/* a column's Arrow type may differ from batch to batch: name them all */
			{
				const char *f = cs->dictionary ? psprintf("%s of %s", cs->format, vs->format) : vs->format;

				ext = metadata_value(vs->metadata, "ARROW:extension:name");
				if (arrow_formats[c] == NULL)
				{
					arrow_formats[c] = pstrdup(f);
					extensions[c] = ext ? ext : "";
				}
				else if (strcmp(arrow_formats[c], f) != 0 ||
						 strcmp(extensions[c], ext ? ext : "") != 0)
				{
					if (strstr(arrow_formats[c], f) == NULL)
						arrow_formats[c] = psprintf("%s; %s", arrow_formats[c], f);
					if (ext && strstr(extensions[c], ext) == NULL)
						extensions[c] = extensions[c][0] ? psprintf("%s; %s", extensions[c], ext) : ext;
				}
			}
		}

		ArrowArrayViewReset(&view);
		schema.release(&schema);
		array.release(&array);
		if (schema.release != NULL || array.release != NULL)
			elog(ERROR, "vexec_test: a released export is not marked released");
		nrows += rows.nrows;
		SPI_freetuptable(SPI_tuptable);
	}

	for (c = 0; c < rows.natts; c++)
	{
		Datum		values[7];
		bool		nulls[7] = {0};

		values[0] = Int32GetDatum(c + 1);
		values[1] = CStringGetTextDatum(format_type_with_typemod(rows.types[c]->typid,
																 rows.types[c]->typmod));
		values[2] = CStringGetTextDatum(layouts[c] ? layouts[c] : "");
		values[3] = CStringGetTextDatum(arrow_formats[c] ? arrow_formats[c] : "");
		if (extensions[c] && extensions[c][0])
			values[4] = CStringGetTextDatum(extensions[c]);
		else
			nulls[4] = true;
		values[5] = Int64GetDatum(nrows);
		values[6] = Int64GetDatum(nnulls[c]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	SPI_cursor_close(rows.portal);
	SPI_finish();
	PG_RETURN_VOID();
}

/*
 * vexec_test.scaled(value numeric, scale int, width int) RETURNS text:
 * the scaled integer vexec reads the value as, and the numeric it writes
 * back from it, or "none" when the value has no scaled form at that scale.
 */
Datum
vexec_test_scaled(PG_FUNCTION_ARGS)
{
	Datum		num = PG_GETARG_DATUM(0);
	int			scale = PG_GETARG_INT32(1);
	int			width = PG_GETARG_INT32(2);
	int128		value;
	VexecType  *type;
	VexecBatch *batch;
	Datum		back;
	char		digits[48];
	char	   *p = digits + sizeof(digits) - 1;
	uint128		u;
	char	   *result;

	if (!vexec_numeric_to_scaled(num, scale, width == 8 ? 18 : 38, width, &value))
		PG_RETURN_TEXT_P(cstring_to_text("none"));

	*p = '\0';
	u = value < 0 ? (uint128) (-(value + 1)) + 1 : (uint128) value;
	do
	{
		*--p = (char) ('0' + (int) (u % 10));
		u /= 10;
	} while (u != 0);
	if (value < 0)
		*--p = '-';

	type = vexec_type_make(NUMERICOID, -1, InvalidOid);
	batch = vexec_batch_create(CurrentMemoryContext, 1, &type);
	back = vexec_scaled_to_numeric(batch, value, scale);
	result = psprintf("%s -> %s", p, DatumGetCString(DirectFunctionCall1(numeric_out, back)));
	vexec_batch_free(batch);
	PG_RETURN_TEXT_P(cstring_to_text(result));
}
