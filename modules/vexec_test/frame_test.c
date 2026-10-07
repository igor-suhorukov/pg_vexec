/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * frame_test.c
 *	  vexec's frames across Motions and its vector cdbhash, tested from SQL
 *	  (pg_vector_executor.md §3.10, §6.2; §5, V7).
 *
 *	vexec_test.frames(query, format, varlena, bool, temporal, numeric, parts)
 *		Every batch of a query, in a configuration, sent as frames as a
 *		Redistribute sends them: its rows dealt among "parts" segments, a
 *		frame each, after the schema of the batch's shapes where it is new;
 *		each frame decoded where it lies 8-byte aligned and from one byte
 *		past it, and every value compared with the row's.
 *	vexec_test.frame_patch(query, at, value, cut)
 *		The first batch's frame of a query, its schema read first, with byte
 *		"at" set to "value" and cut to "cut" bytes, decoded: an error where
 *		it is malformed, which must never read outside its bytes.
 *	vexec_test.cdbhash(query, nsegs, format, ...)
 *		Every row of a query, its columns a distribution key, each hashed
 *		with its type's default hash operator class's function, as a table's
 *		key is: the segment by the vector cdbhash, and by gp_core's own
 *		cdbhash a row at a time (GpCoreApi.hash_segment), which must be the
 *		same.  Needs gp_core of API 1.15.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/hash.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"
#include "varatt.h"

#include "gp_core_api.h"

#include "vexec.h"
#include "batch/batch.h"
#include "motion/frame.h"
#include "motion/motion.h"
#include "vexec_test.h"

PG_FUNCTION_INFO_V1(vexec_test_frames);
PG_FUNCTION_INFO_V1(vexec_test_frame_patch);
PG_FUNCTION_INFO_V1(vexec_test_cdbhash);

/*
 * Two values the same, a varlena by its payload: a frame carries a TOAST
 * pointer's value fetched, and a short header where the row had a long one.
 */
static bool
same_payload(const VexecType *type, Datum a, Datum b)
{
	if (type->typlen == -1)
	{
		varlena    *x = pg_detoast_datum_packed((varlena *) DatumGetPointer(a));
		varlena    *y = pg_detoast_datum_packed((varlena *) DatumGetPointer(b));

		return VARSIZE_ANY_EXHDR(x) == VARSIZE_ANY_EXHDR(y) &&
			memcmp(VARDATA_ANY(x), VARDATA_ANY(y), VARSIZE_ANY_EXHDR(x)) == 0;
	}
	return datum_image_eq(a, b, type->typbyval, type->typlen);
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

/* A frame's bytes, 8-byte aligned or one byte past that. */
static const char *
frame_bytes(bytea *frame, bool aligned, size_t *len)
{
	char	   *buf;

	*len = VARSIZE(frame) - VARHDRSZ;
	buf = MemoryContextAllocAligned(CurrentMemoryContext, *len + 8, 8, MCXT_ALLOC_HUGE);
	if (!aligned)
		buf += 1;
	memcpy(buf, VARDATA(frame), *len);
	return buf;
}

typedef struct FrameColumn
{
	int64		rows;
	int64		nulls;
	char	   *layouts;
} FrameColumn;

Datum
vexec_test_frames(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			parts = PG_GETARG_INT32(6);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	VexecLayoutConfig cfg;
	Rows		rows;
	VexecBatch *batch;
	VexecBatch *into;
	VexecFrameWriter *writer;
	VexecFrameReader *reader;
	FrameColumn *cols;
	List	   *schemas = NIL;
	int64		frames = 0;
	int64		bytes = 0;
	int			nbatch = 0;
	int		   *part_rows;
	int			c;

	if (parts < 1 || parts > 64)
		elog(ERROR, "vexec_test: frames to %d parts", parts);
	layout_config(fcinfo, 1, &cfg);
	InitMaterializedSRF(fcinfo, 0);
	SPI_connect();
	rows_open(&rows, query, NULL);
	batch = vexec_batch_create(CurrentMemoryContext, rows.natts, rows.types);
	into = vexec_batch_create(CurrentMemoryContext, rows.natts, rows.types);
	writer = vexec_frame_writer_create(CurrentMemoryContext, rows.natts, rows.types);
	reader = vexec_frame_reader_create(CurrentMemoryContext, rows.natts, rows.types);
	cols = palloc0(sizeof(FrameColumn) * Max(rows.natts, 1));
	part_rows = palloc(sizeof(int) * VEXEC_BATCH_ROWS);

	while (rows_next(&rows))
	{
		uint64		hash;
		bytea	   *schema_frame;
		int			p;

		rows_fill(&rows, batch);
		(void) vexec_batch_apply_config(batch, &cfg);
		vexec_frame_begin(writer, batch, &hash, &schema_frame);
		if (!list_member_ptr(schemas, schema_frame))
		{
			size_t		len;
			const char *data = frame_bytes(schema_frame, true, &len);

			if (vexec_frame_decode(reader, data, len, into))
				elog(ERROR, "vexec_test: a schema's frame decoded as a batch");
			schemas = lappend(schemas, schema_frame);
			bytes += len;
		}

		/* the rows dealt among the parts, scrambled, each part a frame */
		for (p = 0; p < parts; p++)
		{
			bytea	   *frame;
			int			np = 0;
			int			i;
			int			pass;

			for (i = 0; i < rows.nrows; i++)
				if ((i * 7 + nbatch) % parts == p)
					part_rows[np++] = i;
			if (np == 0)
				continue;
			frame = vexec_frame_rows(writer, part_rows, np);
			if (frame == NULL)
				elog(ERROR, "vexec_test: a frame too long");
			frames++;
			bytes += VARSIZE(frame) - VARHDRSZ;
			for (pass = 0; pass < 2; pass++)
			{
				size_t		len;
				const char *data = frame_bytes(frame, pass == 0, &len);

				if (!vexec_frame_decode(reader, data, len, into))
					elog(ERROR, "vexec_test: a batch's frame decoded as a schema");
				if (into->nrows != np)
					elog(ERROR, "vexec_test: a frame of %d rows decoded as %d", np, into->nrows);
				for (c = 0; c < rows.natts; c++)
				{
					const VexecVec *v = &into->cols[c];
					int			j;

					for (j = 0; j < np; j++)
					{
						bool		isnull;
						Datum		d = vexec_vec_datum(into, v, j, &isnull);
						Datum		want = rows.values[part_rows[j] * rows.natts + c];
						bool		want_null = rows.nulls[part_rows[j] * rows.natts + c];

						if (isnull != want_null ||
							(!isnull && !same_payload(v->type, d, want)))
							ereport(ERROR,
									(errmsg("vexec_test: column %d (%s), %s frame: row %d is %s, not %s",
											c + 1, v->type->name, pass == 0 ? "an aligned" : "an unaligned",
											part_rows[j], value_text(v->type, d, isnull),
											value_text(v->type, want, want_null))));
						if (pass == 0)
						{
							cols[c].rows++;
							cols[c].nulls += isnull;
						}
					}
					if (pass == 0)
					{
						char	   *name = vexec_shape_name(v->type, &v->shape);

						if (cols[c].layouts == NULL)
							cols[c].layouts = name;
						else if (strstr(cols[c].layouts, name) == NULL)
							cols[c].layouts = psprintf("%s %s", cols[c].layouts, name);
					}
				}
			}
		}
		SPI_freetuptable(SPI_tuptable);
		nbatch++;
	}
	SPI_cursor_close(rows.portal);

	for (c = 0; c < rows.natts; c++)
	{
		Datum		values[8];
		bool		nulls[8] = {0};

		values[0] = Int32GetDatum(c + 1);
		values[1] = CStringGetTextDatum(rows.types[c]->name);
		values[2] = CStringGetTextDatum(cols[c].layouts ? cols[c].layouts : "");
		values[3] = Int64GetDatum(cols[c].rows);
		values[4] = Int64GetDatum(cols[c].nulls);
		values[5] = Int64GetDatum(frames);
		values[6] = Int32GetDatum(list_length(schemas));
		values[7] = Int64GetDatum(bytes);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	SPI_finish();
	return (Datum) 0;
}

Datum
vexec_test_frame_patch(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			at = PG_GETARG_INT32(1);
	int			value = PG_GETARG_INT32(2);
	int			cut = PG_ARGISNULL(3) ? 0 : PG_GETARG_INT32(3);
	Rows		rows;
	VexecBatch *batch;
	VexecBatch *into;
	VexecFrameWriter *writer;
	VexecFrameReader *reader;
	uint64		hash;
	bytea	   *schema_frame;
	bytea	   *frame;
	int		   *all;
	size_t		len;
	const char *data;
	char	   *patched;
	int			i;

	SPI_connect();
	rows_open(&rows, query, NULL);
	if (!rows_next(&rows))
		elog(ERROR, "vexec_test: no rows");
	batch = vexec_batch_create(CurrentMemoryContext, rows.natts, rows.types);
	into = vexec_batch_create(CurrentMemoryContext, rows.natts, rows.types);
	writer = vexec_frame_writer_create(CurrentMemoryContext, rows.natts, rows.types);
	reader = vexec_frame_reader_create(CurrentMemoryContext, rows.natts, rows.types);
	rows_fill(&rows, batch);
	vexec_frame_begin(writer, batch, &hash, &schema_frame);
	data = frame_bytes(schema_frame, true, &len);
	(void) vexec_frame_decode(reader, data, len, into);
	all = palloc(sizeof(int) * Max(rows.nrows, 1));
	for (i = 0; i < rows.nrows; i++)
		all[i] = i;
	frame = vexec_frame_rows(writer, all, rows.nrows);
	data = frame_bytes(frame, true, &len);

	/*
	 * the patched frame in a buffer of its exact length, so that a read past
	 * it is caught; "at" and "cut" counted from its end where negative
	 */
	if (cut < 0 && (size_t) -cut < len)
		len += cut;
	else if (cut > 0 && (size_t) cut < len)
		len = cut;
	if (at < 0)
		at = (int) len + at;
	patched = palloc(Max(len, 1));
	memcpy(patched, data, len);
	if (at >= 0 && (size_t) at < len)
		patched[at] = (char) value;
	(void) vexec_frame_decode(reader, patched, len, into);
	i = into->nrows;			/* SPI_finish() frees the batch */
	SPI_cursor_close(rows.portal);
	SPI_finish();
	PG_RETURN_TEXT_P(cstring_to_text(psprintf("decoded %d rows", i)));
}

/*
 * A type's hash function as a distribution key of it is hashed: its default
 * hash operator class's, for the type or one it is binary coercible to, as
 * the port's GpHashProcInOpfamily() finds it.
 */
static Oid
key_hash_function(Oid typid)
{
	Oid			opclass = GetDefaultOpClass(typid, HASH_AM_OID);
	Oid			family;
	Oid			proc;

	if (!OidIsValid(opclass))
		elog(ERROR, "vexec_test: type %s has no default hash operator class", format_type_be(typid));
	family = get_opclass_family(opclass);
	proc = get_opfamily_proc(family, typid, typid, HASHSTANDARD_PROC);
	if (!OidIsValid(proc))
	{
		Oid			intype = get_opclass_input_type(opclass);

		proc = get_opfamily_proc(family, intype, intype, HASHSTANDARD_PROC);
	}
	if (!OidIsValid(proc))
		elog(ERROR, "vexec_test: no hash function for type %s", format_type_be(typid));
	return proc;
}

Datum
vexec_test_cdbhash(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	int			nsegs = PG_GETARG_INT32(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const GpCoreApi *api = vexec_gp_core();
	VexecLayoutConfig cfg;
	Rows		rows;
	VexecBatch *batch;
	VexecBatch *work;
	VexecCdbKey *keys;
	VexecVec  **colptrs;
	Oid		   *funcs;
	void	   *hash;
	int		   *rowlist;
	int		   *segs;
	int64		nrows = 0;
	int64		mismatches = 0;
	int			kernels = 0;
	Datum		values[3];
	bool		nulls[3] = {0};
	int			c;

	if (api == NULL)
		elog(ERROR, "vexec_test: cdbhash() needs gp_core of API 1.%d", VEXEC_GP_CORE_MINOR);
	if (nsegs < 1)
		elog(ERROR, "vexec_test: %d segments", nsegs);
	layout_config(fcinfo, 2, &cfg);
	InitMaterializedSRF(fcinfo, 0);
	SPI_connect();
	rows_open(&rows, query, NULL);
	batch = vexec_batch_create(CurrentMemoryContext, rows.natts, rows.types);
	work = vexec_batch_create(CurrentMemoryContext, 0, NULL);
	keys = palloc0(sizeof(VexecCdbKey) * Max(rows.natts, 1));
	colptrs = palloc(sizeof(VexecVec *) * Max(rows.natts, 1));
	funcs = palloc(sizeof(Oid) * Max(rows.natts, 1));
	for (c = 0; c < rows.natts; c++)
	{
		funcs[c] = key_hash_function(rows.types[c]->typid);
		vexec_cdbhash_prepare(&keys[c], funcs[c], rows.types[c]);
		kernels += keys[c].kernel >= 0;
	}
	hash = api->hash_make(nsegs, rows.natts, funcs);
	rowlist = palloc(sizeof(int) * VEXEC_BATCH_ROWS);
	segs = palloc(sizeof(int) * VEXEC_BATCH_ROWS);
	for (c = 0; c < VEXEC_BATCH_ROWS; c++)
		rowlist[c] = c;

	while (rows_next(&rows))
	{
		int			r;

		rows_fill(&rows, batch);
		(void) vexec_batch_apply_config(batch, &cfg);
		vexec_batch_reset(work);
		for (c = 0; c < rows.natts; c++)
			colptrs[c] = &batch->cols[c];
		vexec_cdbhash(work, rows.natts, keys, colptrs, rowlist, rows.nrows, nsegs, segs);
		for (r = 0; r < rows.nrows; r++)
		{
			int			want = api->hash_segment(hash, &rows.values[r * rows.natts],
												 &rows.nulls[r * rows.natts]);

			if (segs[r] != want)
			{
				if (mismatches == 0)
					ereport(NOTICE,
							(errmsg("vexec_test: row %lld goes to segment %d, gp_core's %d",
									(long long) (nrows + r), segs[r], want)));
				mismatches++;
			}
		}
		nrows += rows.nrows;
		SPI_freetuptable(SPI_tuptable);
	}
	SPI_cursor_close(rows.portal);
	values[0] = Int64GetDatum(nrows);
	values[1] = Int32GetDatum(kernels);
	values[2] = Int64GetDatum(mismatches);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	SPI_finish();
	return (Datum) 0;
}
