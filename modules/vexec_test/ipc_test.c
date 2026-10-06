/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * ipc_test.c
 *	  vexec's Arrow IPC codec, tested from SQL (pg_vector_executor.md §5,
 *	  V7_0).
 *
 *	vexec_test.ipc(query, format, varlena, bool, temporal, numeric)
 *		Every batch of a query, built and exported as the export check
 *		builds them (vexec_test.c), written as messages -- a Schema message
 *		whenever the export's schema changes, a RecordBatch message a batch
 *		-- and read back: the schema compared with the export's, the batch
 *		validated by nanoarrow's FULL validation and vexec_test's check of
 *		views, and compared with the export value by value, bit for bit,
 *		read once where it lies and once from an unaligned copy.
 *	vexec_test.ipc_stream(query, format, ...)
 *		The same batches as streams, one for each run of batches of one
 *		schema: its Schema message, its RecordBatch messages and the
 *		end-of-stream marker, with the export's format strings.
 *	vexec_test.ipc_reencode(stream, aligned)
 *		A stream read by vexec's reader, each batch validated by nanoarrow
 *		as well, and written back by vexec's writer.
 *	vexec_test.ipc_messages(stream), vexec_test.ipc_patch(stream, ...)
 *		Where a stream's messages lie, and a stream with one number of a
 *		RecordBatch message changed: malformed input, made in SQL.
 *	vexec_test.raw(value)
 *		A value's bytes as PostgreSQL holds them, for test/vexec/ipc_check.py
 *		to compare with what Arrow's own reader reads.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupmacs.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"
#include "varatt.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wshadow=compatible-local"
#include "nanoarrow/nanoarrow.h"
#pragma GCC diagnostic pop

#include "vexec.h"
#include "batch/batch.h"
#include "batch/export.h"
#include "ipc/ipc.h"
#include "vexec_test.h"

PG_FUNCTION_INFO_V1(vexec_test_ipc);
PG_FUNCTION_INFO_V1(vexec_test_ipc_stream);
PG_FUNCTION_INFO_V1(vexec_test_ipc_reencode);
PG_FUNCTION_INFO_V1(vexec_test_ipc_messages);
PG_FUNCTION_INFO_V1(vexec_test_ipc_patch);
PG_FUNCTION_INFO_V1(vexec_test_raw);

static void
na_ok(int code, struct ArrowError *error, const char *what)
{
	if (code != NANOARROW_OK)
		ereport(ERROR,
				(errmsg("vexec_test: %s: %s", what, ArrowErrorMessage(error))));
}

static void
layout_config(FunctionCallInfo fcinfo, int arg, VexecLayoutConfig *cfg)
{
	cfg->format = (uint8) setting_value("vexec.batch_format", text_to_cstring(PG_GETARG_TEXT_PP(arg)),
										format_names, lengthof(format_names));
	cfg->varlena = (uint8) setting_value("vexec.batch_varlena_layout", text_to_cstring(PG_GETARG_TEXT_PP(arg + 1)),
										 varlena_names, lengthof(varlena_names));
	cfg->boolean = (uint8) setting_value("vexec.batch_bool_layout", text_to_cstring(PG_GETARG_TEXT_PP(arg + 2)),
										 bool_names, lengthof(bool_names));
	cfg->temporal = (uint8) setting_value("vexec.batch_temporal_layout", text_to_cstring(PG_GETARG_TEXT_PP(arg + 3)),
										  temporal_names, lengthof(temporal_names));
	cfg->numeric = (uint8) setting_value("vexec.batch_numeric_layout", text_to_cstring(PG_GETARG_TEXT_PP(arg + 4)),
										 numeric_names, lengthof(numeric_names));
}

/* ---- a query's batches, exported as the export check exports them ---- */

typedef struct Batches
{
	Rows		rows;
	VexecBatch *batch;
	VexecLayoutConfig cfg;
	char	  **names;
	int			nbatches;
	bool		done;
} Batches;

static void
batches_open(Batches *b, const char *query, const VexecLayoutConfig *cfg)
{
	int			c;

	memset(b, 0, sizeof(*b));
	rows_open(&b->rows, query, NULL);
	b->batch = vexec_batch_create(CurrentMemoryContext, b->rows.natts, b->rows.types);
	b->cfg = *cfg;
	b->names = palloc0(sizeof(char *) * Max(b->rows.natts, 1));
	for (c = 0; c < b->rows.natts; c++)
		b->names[c] = pstrdup(NameStr(TupleDescAttr(b->rows.desc, c)->attname));
}

/* The next batch, exported; an empty result is one empty batch. */
static bool
batches_next(Batches *b, struct ArrowSchema *schema, struct ArrowArray *array)
{
	if (b->done)
		return false;
	if (b->nbatches > 0)
		SPI_freetuptable(SPI_tuptable);
	if (!rows_next(&b->rows))
	{
		b->done = true;
		if (b->nbatches > 0)
			return false;
	}
	rows_fill(&b->rows, b->batch);
	(void) vexec_batch_apply_config(b->batch, &b->cfg);
	vexec_batch_export(b->batch, (const char *const *) b->names, schema, array);
	b->nbatches++;
	return true;
}

static void
batches_close(Batches *b)
{
	SPI_cursor_close(b->rows.portal);
}

/* ---- comparing what went out with what came back ---- */

/* The bytes of the interface's metadata encoding. */
static int64
metadata_len(const char *m)
{
	const char *p = m;
	int32		n,
				l;
	int			i;

	if (m == NULL)
		return 0;
	memcpy(&n, p, 4);
	p += 4;
	for (i = 0; i < 2 * n; i++)
	{
		memcpy(&l, p, 4);
		p += 4 + l;
	}
	return p - m;
}

/* Whether two schemas are the same, formats, names, flags and metadata. */
static bool
same_schema(const struct ArrowSchema *a, const struct ArrowSchema *b, const char *path, bool report)
{
	const char *what = NULL;
	int64		i;

	if (strcmp(a->format, b->format) != 0)
		what = psprintf("format \"%s\" came back as \"%s\"", a->format, b->format);
	else if (strcmp(a->name ? a->name : "", b->name ? b->name : "") != 0)
		what = psprintf("name \"%s\" came back as \"%s\"", a->name, b->name);
	else if (a->flags != b->flags)
		what = psprintf("flags %lld came back as %lld", (long long) a->flags, (long long) b->flags);
	else if (metadata_len(a->metadata) != metadata_len(b->metadata) ||
			 memcmp(a->metadata ? a->metadata : "", b->metadata ? b->metadata : "",
					metadata_len(a->metadata)) != 0)
		what = "its metadata came back otherwise";
	else if (a->n_children != b->n_children || (a->dictionary == NULL) != (b->dictionary == NULL))
		what = "its children came back otherwise";
	if (what)
	{
		if (report)
			ereport(ERROR, (errmsg("vexec_test: IPC check, %s: %s", path, what)));
		return false;
	}
	for (i = 0; i < a->n_children; i++)
		if (!same_schema(a->children[i], b->children[i],
						 psprintf("%s.%s", path, a->children[i]->name), report))
			return false;
	return true;
}

static void
differ(const char *path, int64 row, const char *what)
{
	ereport(ERROR,
			(errmsg("vexec_test: IPC check, %s row %lld: %s", path, (long long) row, what)));
}

/* Two arrays of one type: every valid value the same, bit for bit. */
static void
same_values(struct ArrowArrayView *a, struct ArrowArrayView *b, const char *path)
{
	int64		i;
	int64		k;

	if (a->storage_type != b->storage_type || a->length != b->length ||
		a->n_children != b->n_children)
		differ(path, -1, psprintf("type %d, %lld values came back as type %d, %lld values",
								  a->storage_type, (long long) a->length,
								  b->storage_type, (long long) b->length));
	if (a->null_count >= 0 && b->null_count >= 0 && a->null_count != b->null_count)
		differ(path, -1, psprintf("%lld NULLs came back as %lld",
								  (long long) a->null_count, (long long) b->null_count));
	for (i = 0; i < a->length; i++)
	{
		if (ArrowArrayViewIsNull(a, i) != ArrowArrayViewIsNull(b, i))
			differ(path, i, "its NULL came back otherwise");
		if (ArrowArrayViewIsNull(a, i))
			continue;
		switch (a->storage_type)
		{
			case NANOARROW_TYPE_NA:
			case NANOARROW_TYPE_STRUCT:
			case NANOARROW_TYPE_FIXED_SIZE_LIST:
				break;
			case NANOARROW_TYPE_SPARSE_UNION:
			case NANOARROW_TYPE_DENSE_UNION:
				if (ArrowArrayViewUnionTypeId(a, i) != ArrowArrayViewUnionTypeId(b, i) ||
					ArrowArrayViewUnionChildOffset(a, i) != ArrowArrayViewUnionChildOffset(b, i))
					differ(path, i, "its union value came back otherwise");
				break;
			case NANOARROW_TYPE_LIST:
			case NANOARROW_TYPE_LARGE_LIST:
			case NANOARROW_TYPE_MAP:
				if (ArrowArrayViewListChildOffset(a, i) != ArrowArrayViewListChildOffset(b, i) ||
					ArrowArrayViewListChildOffset(a, i + 1) != ArrowArrayViewListChildOffset(b, i + 1))
					differ(path, i, "its list came back otherwise");
				break;
			case NANOARROW_TYPE_BOOL:
				if (ArrowArrayViewGetIntUnsafe(a, i) != ArrowArrayViewGetIntUnsafe(b, i))
					differ(path, i, "another bool came back");
				break;
			case NANOARROW_TYPE_STRING:
			case NANOARROW_TYPE_BINARY:
			case NANOARROW_TYPE_LARGE_STRING:
			case NANOARROW_TYPE_LARGE_BINARY:
			case NANOARROW_TYPE_STRING_VIEW:
			case NANOARROW_TYPE_BINARY_VIEW:
				{
					struct ArrowBufferView x = ArrowArrayViewGetBytesUnsafe(a, i);
					struct ArrowBufferView y = ArrowArrayViewGetBytesUnsafe(b, i);

					if (x.size_bytes != y.size_bytes ||
						memcmp(x.data.data, y.data.data, x.size_bytes) != 0)
						differ(path, i, "other bytes came back");
					if ((a->storage_type == NANOARROW_TYPE_STRING_VIEW ||
						 a->storage_type == NANOARROW_TYPE_BINARY_VIEW) &&
						memcmp(a->buffer_views[1].data.as_uint8 + 16 * i,
							   b->buffer_views[1].data.as_uint8 + 16 * i, 16) != 0)
						differ(path, i, "another view came back");
					break;
				}
			default:
				{
					int64		w = a->layout.element_size_bits[1] / 8;

					if (memcmp(a->buffer_views[1].data.as_uint8 + i * w,
							   b->buffer_views[1].data.as_uint8 + i * w, w) != 0)
						differ(path, i, "another value came back");
					break;
				}
		}
	}
	for (k = 0; k < a->n_children; k++)
		same_values(a->children[k], b->children[k], psprintf("%s.%lld", path, (long long) k));
}

/*
 * An array read back: nanoarrow's FULL validation, and the views' check.
 * A schema nanoarrow cannot represent -- a fixed-size binary of no bytes,
 * which Arrow allows -- is no failure of vexec's when any schema is read,
 * and its batches are left to the views' check.
 */
static void
validate(const struct ArrowSchema *schema, const struct ArrowArray *array,
		 struct ArrowArrayView *view, bool any_schema)
{
	struct ArrowError error;
	int64		c;

	memset(&error, 0, sizeof(error));
	if (ArrowArrayViewInitFromSchema(view, schema, &error) != NANOARROW_OK)
	{
		na_ok(any_schema ? NANOARROW_OK : EINVAL, &error, "a schema read back");
		ArrowArrayViewInitFromType(view, NANOARROW_TYPE_NA);
		for (c = 0; c < array->n_children; c++)
			if (strcmp(schema->children[c]->format, "vu") == 0 ||
				strcmp(schema->children[c]->format, "vz") == 0)
				check_views(array->children[c], (int) c + 1);
		return;
	}
	na_ok(ArrowArrayViewSetArray(view, array, &error), &error, "an array read back");
	na_ok(ArrowArrayViewValidate(view, NANOARROW_VALIDATION_LEVEL_FULL, &error),
		  &error, "nanoarrow's full validation of an array read back");
	for (c = 0; c < array->n_children; c++)
		if (view->children[c]->storage_type == NANOARROW_TYPE_STRING_VIEW ||
			view->children[c]->storage_type == NANOARROW_TYPE_BINARY_VIEW)
			check_views(array->children[c], (int) c + 1);
}

/* A message's bytes, and what reading their prefix says. */
static void
message_bytes(const VexecIpcMessage *msg, StringInfo buf, VexecIpcHeader *h, size_t *consumed)
{
	int64		pieces = 0;
	int			i;

	resetStringInfo(buf);
	vexec_ipc_append(buf, msg, true);
	for (i = 0; i < msg->npieces; i++)
		pieces += msg->pieces[i].len;
	if (msg->metadata_len % 8 != 0 || msg->body_len % 8 != 0 || pieces != msg->body_len ||
		msg->prefix + 8 != msg->metadata)
		elog(ERROR, "vexec_test: a message of %zu bytes of metadata and %lld of body",
			 msg->metadata_len, (long long) msg->body_len);
	if (!vexec_ipc_read_prefix(buf->data, buf->len, h, consumed) || h->kind != msg->kind ||
		*consumed != 8 + msg->metadata_len || h->body_len != msg->body_len ||
		*consumed + h->body_len != (size_t) buf->len)
		elog(ERROR, "vexec_test: a message's prefix does not read back");
}

/*
 * vexec_test.ipc(query text, format text, varlena text, bool text,
 *				  temporal text, numeric text)
 *	RETURNS TABLE (col int, type text, arrow text, rows int8, nulls int8,
 *				   schemas int, batches int)
 */
Datum
vexec_test_ipc(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	VexecLayoutConfig cfg;
	Batches		b;
	VexecIpcWriter *writer;
	struct ArrowSchema schema,
				rschema;
	struct ArrowArray array;
	StringInfoData buf;
	char	  **formats;
	int64	   *nulls;
	int64		nrows = 0;
	int			nschemas = 0;
	int			c;

	layout_config(fcinfo, 1, &cfg);
	InitMaterializedSRF(fcinfo, 0);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "vexec_test: SPI_connect failed");
	batches_open(&b, query, &cfg);
	writer = vexec_ipc_writer_create(CurrentMemoryContext);
	initStringInfo(&buf);
	memset(&rschema, 0, sizeof(rschema));
	formats = palloc0(sizeof(char *) * Max(b.rows.natts, 1));
	nulls = palloc0(sizeof(int64) * Max(b.rows.natts, 1));

	while (batches_next(&b, &schema, &array))
	{
		const VexecIpcMessage *msg;
		VexecIpcHeader h;
		size_t		consumed;
		struct ArrowArray back,
					shifted;
		struct ArrowArrayView va,
					vb,
					vs;
		struct ArrowError error;
		char	   *copy;

		if (rschema.release == NULL || !same_schema(&schema, &rschema, "", false))
		{
			if (rschema.release)
				rschema.release(&rschema);
			msg = vexec_ipc_write_schema(writer, &schema);
			message_bytes(msg, &buf, &h, &consumed);
			vexec_ipc_read_schema(h.metadata, h.metadata_len, &rschema);
			same_schema(&schema, &rschema, "the schema", true);
			nschemas++;
		}

		msg = vexec_ipc_write_batch(writer, &schema, &array);
		if (msg->nrows != b.rows.nrows)
			elog(ERROR, "vexec_test: a RecordBatch message of %lld rows", (long long) msg->nrows);
		message_bytes(msg, &buf, &h, &consumed);
		vexec_ipc_read_batch(&rschema, h.metadata, h.metadata_len, buf.data + consumed,
							 h.body_len, &back);
		validate(&rschema, &back, &vb, false);

		/* once more from an address that is not 8-byte aligned: copies */
		copy = palloc(buf.len + 1);
		memcpy(copy + 1, buf.data, buf.len);
		vexec_ipc_read_batch(&rschema, h.metadata - buf.data + copy + 1, h.metadata_len,
							 copy + 1 + consumed, h.body_len, &shifted);
		validate(&rschema, &shifted, &vs, false);

		memset(&error, 0, sizeof(error));
		na_ok(ArrowArrayViewInitFromSchema(&va, &schema, &error), &error, "the export's schema");
		na_ok(ArrowArrayViewSetArray(&va, &array, &error), &error, "the export's array");
		for (c = 0; c < b.rows.natts; c++)
		{
			const char *f = schema.children[c]->format;

			same_values(va.children[c], vb.children[c], b.names[c]);
			same_values(va.children[c], vs.children[c], psprintf("%s, unaligned", b.names[c]));
			nulls[c] += back.children[c]->null_count;
			/* a column's format may differ from batch to batch: name them all */
			if (formats[c] == NULL)
				formats[c] = pstrdup(f);
			else if (strstr(psprintf("; %s; ", formats[c]), psprintf("; %s; ", f)) == NULL)
				formats[c] = psprintf("%s; %s", formats[c], f);
		}
		ArrowArrayViewReset(&va);
		ArrowArrayViewReset(&vb);
		ArrowArrayViewReset(&vs);
		back.release(&back);
		shifted.release(&shifted);
		if (back.release != NULL || shifted.release != NULL)
			elog(ERROR, "vexec_test: a released array is not marked released");
		pfree(copy);
		schema.release(&schema);
		array.release(&array);
		nrows += b.rows.nrows;
	}
	if (rschema.release)
		rschema.release(&rschema);

	for (c = 0; c < b.rows.natts; c++)
	{
		Datum		values[7];
		bool		isnull[7] = {0};

		values[0] = Int32GetDatum(c + 1);
		values[1] = CStringGetTextDatum(format_type_with_typemod(b.rows.types[c]->typid,
																 b.rows.types[c]->typmod));
		values[2] = CStringGetTextDatum(formats[c] ? formats[c] : "");
		values[3] = Int64GetDatum(nrows);
		values[4] = Int64GetDatum(nulls[c]);
		values[5] = Int32GetDatum(nschemas);
		values[6] = Int32GetDatum(b.nbatches);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, isnull);
	}
	vexec_ipc_writer_free(writer);
	batches_close(&b);
	SPI_finish();
	PG_RETURN_VOID();
}

static bytea *
to_bytea(StringInfo s)
{
	bytea	   *out = palloc(VARHDRSZ + s->len);

	SET_VARSIZE(out, VARHDRSZ + s->len);
	memcpy(VARDATA(out), s->data, s->len);
	return out;
}

/*
 * vexec_test.ipc_stream(query text, format text, ...)
 *	RETURNS TABLE (stream bytea, formats text[], batches int, rows int8)
 */
Datum
vexec_test_ipc_stream(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	VexecLayoutConfig cfg;
	Batches		b;
	VexecIpcWriter *writer;
	struct ArrowSchema schema,
				last;
	struct ArrowArray array;
	StringInfoData stream;
	Datum	   *formats = NULL;
	int			nbatches = 0;
	int64		nrows = 0;
	bool		open = false;
	int			c;

	layout_config(fcinfo, 1, &cfg);
	InitMaterializedSRF(fcinfo, 0);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "vexec_test: SPI_connect failed");
	batches_open(&b, query, &cfg);
	writer = vexec_ipc_writer_create(CurrentMemoryContext);
	initStringInfo(&stream);
	memset(&last, 0, sizeof(last));

	for (;;)
	{
		bool		more = batches_next(&b, &schema, &array);

		/* a stream ends where its schema does */
		if (open && (!more || !same_schema(&schema, &last, "", false)))
		{
			Datum		values[4];
			bool		isnull[4] = {0};

			appendBinaryStringInfo(&stream, vexec_ipc_eos, VEXEC_IPC_EOS_LEN);
			values[0] = PointerGetDatum(to_bytea(&stream));
			values[1] = PointerGetDatum(construct_array_builtin(formats, b.rows.natts, TEXTOID));
			values[2] = Int32GetDatum(nbatches);
			values[3] = Int64GetDatum(nrows);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, isnull);
			resetStringInfo(&stream);
			last.release(&last);
			open = false;
		}
		if (!more)
			break;
		if (!open)
		{
			vexec_ipc_append(&stream, vexec_ipc_write_schema(writer, &schema), true);
			vexec_ipc_read_schema(stream.data + 8, stream.len - 8, &last);
			formats = palloc(sizeof(Datum) * Max(b.rows.natts, 1));
			for (c = 0; c < b.rows.natts; c++)
				formats[c] = CStringGetTextDatum(schema.children[c]->format);
			nbatches = 0;
			nrows = 0;
			open = true;
		}
		vexec_ipc_append(&stream, vexec_ipc_write_batch(writer, &schema, &array), true);
		nbatches++;
		nrows += b.rows.nrows;
		schema.release(&schema);
		array.release(&array);
	}
	vexec_ipc_writer_free(writer);
	batches_close(&b);
	SPI_finish();
	PG_RETURN_VOID();
}

static void
stream_error(const char *detail)
{
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
			 errmsg("malformed Arrow IPC stream"),
			 errdetail_internal("%s", detail)));
}

/* A stream's bytes, copied to an address 8-byte aligned, or one past it. */
static const char *
stream_bytes(bytea *in, bool aligned, size_t *len)
{
	char	   *copy;

	*len = VARSIZE_ANY_EXHDR(in);
	copy = palloc_aligned(*len + 8, 8, 0);
	memcpy(copy + (aligned ? 0 : 1), VARDATA_ANY(in), *len);
	return copy + (aligned ? 0 : 1);
}

/* vexec_test.ipc_reencode(stream bytea, aligned bool) RETURNS bytea */
Datum
vexec_test_ipc_reencode(PG_FUNCTION_ARGS)
{
	size_t		len;
	const char *data = stream_bytes(PG_GETARG_BYTEA_PP(0), PG_GETARG_BOOL(1), &len);
	VexecIpcWriter *writer = vexec_ipc_writer_create(CurrentMemoryContext);
	struct ArrowSchema schema;
	StringInfoData out;
	size_t		pos = 0;

	memset(&schema, 0, sizeof(schema));
	initStringInfo(&out);
	for (;;)
	{
		VexecIpcHeader h;
		size_t		consumed;

		if (!vexec_ipc_read_prefix(data + pos, len - pos, &h, &consumed))
			stream_error("The stream ends within a message's prefix or metadata.");
		if (h.kind == VEXEC_IPC_END)
			break;
		pos += consumed;
		if (h.body_len > (int64) (len - pos))
			stream_error("The stream ends within a message's body.");
		if (h.kind == VEXEC_IPC_SCHEMA)
		{
			if (schema.release)
				stream_error("A second Schema message.");
			vexec_ipc_read_schema(h.metadata, h.metadata_len, &schema);
			vexec_ipc_append(&out, vexec_ipc_write_schema(writer, &schema), true);
		}
		else
		{
			struct ArrowArray array;
			struct ArrowArrayView view;

			if (schema.release == NULL)
				stream_error("A RecordBatch message before the Schema message.");
			vexec_ipc_read_batch(&schema, h.metadata, h.metadata_len, data + pos, h.body_len,
								 &array);
			validate(&schema, &array, &view, true);
			ArrowArrayViewReset(&view);
			vexec_ipc_append(&out, vexec_ipc_write_batch(writer, &schema, &array), true);
			array.release(&array);
		}
		pos += h.body_len;
	}
	appendBinaryStringInfo(&out, vexec_ipc_eos, VEXEC_IPC_EOS_LEN);
	vexec_ipc_writer_free(writer);
	PG_RETURN_BYTEA_P(to_bytea(&out));
}

/*
 * vexec_test.ipc_messages(stream bytea)
 *	RETURNS TABLE (n int, kind text, start int8, metadata_len int8,
 *				   body_start int8, body_len int8)
 */
Datum
vexec_test_ipc_messages(PG_FUNCTION_ARGS)
{
	size_t		len;
	const char *data = stream_bytes(PG_GETARG_BYTEA_PP(0), true, &len);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	static const char *const kinds[] = {"end", "schema", "dictionary batch", "record batch"};
	size_t		pos = 0;
	int			n = 0;

	InitMaterializedSRF(fcinfo, 0);
	for (;;)
	{
		VexecIpcHeader h;
		size_t		consumed;
		Datum		values[6];
		bool		isnull[6] = {0};

		if (!vexec_ipc_read_prefix(data + pos, len - pos, &h, &consumed))
			stream_error("The stream ends within a message's prefix or metadata.");
		values[0] = Int32GetDatum(++n);
		values[1] = CStringGetTextDatum(kinds[h.kind]);
		values[2] = Int64GetDatum(pos);
		values[3] = Int64GetDatum(h.metadata_len);
		values[4] = Int64GetDatum(pos + consumed);
		values[5] = Int64GetDatum(h.body_len);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, isnull);
		if (h.kind == VEXEC_IPC_END)
			break;
		pos += consumed + h.body_len;
		if (pos > len)
			stream_error("The stream ends within a message's body.");
	}
	PG_RETURN_VOID();
}

/* Test code: a valid flatbuffer's tables, followed unchecked. */
static uint32
le32(const uint8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32) p[3] << 24;
}

static uint32
field_at(const uint8 *m, uint32 table, int id)
{
	uint32		vt = table - le32(m + table);
	uint16		vtlen = m[vt] | m[vt + 1] << 8;
	uint16		at = 0;

	if (4 + 2 * id + 2 <= vtlen)
		at = m[vt + 4 + 2 * id] | m[vt + 5 + 2 * id] << 8;
	if (at == 0)
		elog(ERROR, "vexec_test: the table has no field %d", id);
	return table + at;
}

/*
 * vexec_test.ipc_patch(stream bytea, message int, what text, idx int,
 *						value int8) RETURNS bytea
 *
 * The stream with a number of its message-th message changed: what is
 * version, body_length, batch_length, node_length, node_nulls,
 * buffer_offset or buffer_length, the last four of FieldNode or Buffer idx
 * (from 0).
 */
Datum
vexec_test_ipc_patch(PG_FUNCTION_ARGS)
{
	bytea	   *in = PG_GETARG_BYTEA_PP(0);
	int			message = PG_GETARG_INT32(1);
	char	   *what = text_to_cstring(PG_GETARG_TEXT_PP(2));
	int			idx = PG_GETARG_INT32(3);
	int64		value = PG_GETARG_INT64(4);
	size_t		len = VARSIZE_ANY_EXHDR(in);
	bytea	   *out = palloc(VARHDRSZ + len);
	char	   *data = VARDATA(out);
	size_t		pos = 0;
	uint8	   *m = NULL;
	uint32		at;
	int			width = 8;
	int			n,
				i;

	SET_VARSIZE(out, VARHDRSZ + len);
	memcpy(data, VARDATA_ANY(in), len);
	for (n = 1; n <= message; n++)
	{
		VexecIpcHeader h;
		size_t		consumed;

		if (!vexec_ipc_read_prefix(data + pos, len - pos, &h, &consumed) || h.kind == VEXEC_IPC_END)
			elog(ERROR, "vexec_test: the stream has no message %d", message);
		m = (uint8 *) data + pos + consumed - h.metadata_len;
		pos += consumed + h.body_len;
	}
	if (strcmp(what, "version") == 0)
	{
		at = field_at(m, le32(m), 0);
		width = 2;
	}
	else if (strcmp(what, "body_length") == 0)
		at = field_at(m, le32(m), 3);
	else
	{
		uint32		header = field_at(m, le32(m), 2);
		uint32		vec;

		header += le32(m + header);
		if (strcmp(what, "batch_length") == 0)
			at = field_at(m, header, 0);
		else
		{
			if (strncmp(what, "node_", 5) != 0 && strncmp(what, "buffer_", 7) != 0)
				elog(ERROR, "vexec_test: \"%s\" is not a number ipc_patch changes", what);
			vec = field_at(m, header, what[0] == 'n' ? 1 : 2);
			vec += le32(m + vec);
			if (idx < 0 || (uint32) idx >= le32(m + vec))
				elog(ERROR, "vexec_test: there is no element %d", idx);
			at = vec + 4 + 16 * idx +
				((strcmp(what, "node_nulls") == 0 || strcmp(what, "buffer_length") == 0) ? 8 : 0);
		}
	}
	for (i = 0; i < width; i++)
		m[at + i] = (uint8) ((uint64) value >> (8 * i));
	PG_RETURN_BYTEA_P(out);
}

/* vexec_test.raw(value anyelement) RETURNS bytea */
Datum
vexec_test_raw(PG_FUNCTION_ARGS)
{
	Oid			typid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	Datum		d = PG_GETARG_DATUM(0);
	int16		typlen;
	bool		typbyval;
	char		word[8];
	const char *p;
	Size		len;
	bytea	   *out;

	get_typlenbyval(typid, &typlen, &typbyval);
	if (typbyval)
	{
		store_att_byval(word, d, typlen);
		p = word;
		len = typlen;
	}
	else if (typlen > 0)
	{
		p = DatumGetPointer(d);
		len = typlen;
	}
	else if (typlen == -1)
	{
		varlena    *v = pg_detoast_datum_packed((varlena *) DatumGetPointer(d));

		p = VARDATA_ANY(v);
		len = VARSIZE_ANY_EXHDR(v);
	}
	else
	{
		p = DatumGetCString(d);
		len = strlen(p);
	}
	out = palloc(VARHDRSZ + len);
	SET_VARSIZE(out, VARHDRSZ + len);
	memcpy(VARDATA(out), p, len);
	PG_RETURN_BYTEA_P(out);
}
