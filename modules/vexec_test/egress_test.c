/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * egress_test.c
 *	  vexec's egress API tested from SQL (pg_vector_executor.md §5, V10;
 *	  vexec_egress.h).
 *
 *	egress(query)		the query through the egress's receiver, as a Flight
 *						session's statement goes (SPI with the receiver as its
 *						destination): the stream of IPC messages it wrote --
 *						its schema, its batches, the end-of-stream marker --
 *						its fields' Arrow types as the schema says them, and
 *						what it counted, the batches a vector node's own
 *						buffers went out as among them
 *	egress_params(...)	a stream of parameter batches -- pyarrow's, or the
 *						egress's own -- read back as the values of given
 *						types, a row of text each
 *
 * The egress is reached as vexec_flight reaches it, through the rendezvous
 * variable; the vector batches' count, by symbol.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/tuplestore.h"

#include "vexec_egress.h"

#include "vexec.h"
#include "batch/arrow_abi.h"
#include "egress/egress.h"
#include "ipc/ipc.h"

PG_FUNCTION_INFO_V1(vexec_test_egress);
PG_FUNCTION_INFO_V1(vexec_test_egress_params);

static const VexecEgressRoutine *
egress(void)
{
	const VexecEgressRoutine *eg = vexec_egress_lookup();

	if (eg == NULL)
		elog(ERROR, "vexec's egress API is not published");
	return eg;
}

/* A message, appended to the stream: prefix, metadata, body. */
static void
collect(void *arg, const VexecEgressMessage *msg)
{
	StringInfo	out = arg;
	int			i;

	appendBinaryStringInfo(out, msg->prefix, 8);
	appendBinaryStringInfo(out, msg->metadata, (int) msg->metadata_len);
	for (i = 0; i < msg->npieces; i++)
		appendBinaryStringInfo(out, msg->pieces[i].data, (int) msg->pieces[i].len);
}

/* A key's value in C Data metadata, or NULL. */
static char *
metadata_value(const char *metadata, const char *key)
{
	int32		n;
	int32		len;
	int			i;

	if (metadata == NULL)
		return NULL;
	memcpy(&n, metadata, 4);
	metadata += 4;
	for (i = 0; i < n; i++)
	{
		bool		match;

		memcpy(&len, metadata, 4);
		match = len == (int32) strlen(key) && memcmp(metadata + 4, key, len) == 0;
		metadata += 4 + len;
		memcpy(&len, metadata, 4);
		if (match)
			return pnstrdup(metadata + 4, len);
		metadata += 4 + len;
	}
	return NULL;
}

/*
 * The fields of a stream's schema, as vexec's reader reads them: "name
 * format", and the extension's name after the format where a field has one.
 */
static Datum
schema_fields(const char *data, size_t len)
{
	VexecIpcHeader h;
	size_t		consumed;
	struct ArrowSchema schema;
	Datum	   *texts;
	int			i;
	ArrayType  *result;

	if (!vexec_ipc_read_prefix(data, len, &h, &consumed) || h.kind != VEXEC_IPC_SCHEMA)
		elog(ERROR, "the stream does not start with a schema");
	vexec_ipc_read_schema(h.metadata, h.metadata_len, &schema);
	texts = palloc(sizeof(Datum) * Max(schema.n_children, 1));
	for (i = 0; i < schema.n_children; i++)
	{
		const struct ArrowSchema *f = schema.children[i];
		char	   *ext = metadata_value(f->metadata, "ARROW:extension:name");

		texts[i] = CStringGetTextDatum(ext ? psprintf("%s %s %s", f->name, f->format, ext)
									   : psprintf("%s %s", f->name, f->format));
	}
	result = construct_array_builtin(texts, (int) schema.n_children, TEXTOID);
	schema.release(&schema);
	return PointerGetDatum(result);
}

/*
 * egress(query text, OUT rows int8, OUT batches int8, OUT vector_batches
 * int8, OUT fields text[], OUT stream bytea)
 */
Datum
vexec_test_egress(PG_FUNCTION_ARGS)
{
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const VexecEgressRoutine *eg = egress();
	StringInfoData stream;
	DestReceiver *dest;
	SPIExecuteOptions opts;
	TupleDesc	desc;
	Datum		values[5];
	bool		nulls[5] = {false, false, false, false, false};
	int64		rows;
	int64		batches;
	int64		bytes;
	bytea	   *b;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	initStringInfo(&stream);
	dest = eg->receiver(collect, &stream, NULL, 0);
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "SPI_connect failed");
	memset(&opts, 0, sizeof(opts));
	opts.dest = dest;
	(void) SPI_execute_extended(query, &opts);
	SPI_finish();
	eg->receiver_counts(dest, &rows, &batches, &bytes);
	values[0] = Int64GetDatum(rows);
	values[1] = Int64GetDatum(batches);
	values[2] = Int64GetDatum(vexec_egress_vector_batches(dest));
	dest->rDestroy(dest);
	appendBinaryStringInfo(&stream, vexec_ipc_eos, VEXEC_IPC_EOS_LEN);
	values[3] = schema_fields(stream.data, stream.len);
	b = palloc(VARHDRSZ + stream.len);
	SET_VARSIZE(b, VARHDRSZ + stream.len);
	memcpy(VARDATA(b), stream.data, stream.len);
	values[4] = PointerGetDatum(b);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(desc, values, nulls)));
}

typedef struct ParamRows
{
	Tuplestorestate *store;
	TupleDesc	desc;
	int			nparams;
	FmgrInfo   *out;
} ParamRows;

static void
param_row(void *arg, int64 rownum, const Datum *values, const bool *isnull)
{
	ParamRows  *pr = arg;
	Datum		row[2];
	bool		nulls[2] = {false, false};
	StringInfoData s;
	int			i;

	initStringInfo(&s);
	for (i = 0; i < pr->nparams; i++)
	{
		if (i > 0)
			appendStringInfoString(&s, " | ");
		if (isnull[i])
			appendStringInfoString(&s, "NULL");
		else
			appendStringInfoString(&s, OutputFunctionCall(&pr->out[i], values[i]));
	}
	row[0] = Int64GetDatum(rownum);
	row[1] = CStringGetTextDatum(s.data);
	tuplestore_putvalues(pr->store, pr->desc, row, nulls);
}

/*
 * egress_params(stream bytea, types regtype[], OUT rownum int8, OUT
 * row text): an encapsulated stream of parameter batches, read as values of
 * the types.
 */
Datum
vexec_test_egress_params(PG_FUNCTION_ARGS)
{
	bytea	   *stream = PG_GETARG_BYTEA_PP(0);
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(1);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const VexecEgressRoutine *eg = egress();
	const char *data = VARDATA_ANY(stream);
	size_t		len = VARSIZE_ANY_EXHDR(stream);
	size_t		pos = 0;
	Datum	   *elems;
	int			n;
	Oid		   *types;
	ParamRows	pr;
	void	   *state = NULL;
	int			i;

	InitMaterializedSRF(fcinfo, 0);
	deconstruct_array(arr, REGTYPEOID, sizeof(Oid), true, TYPALIGN_INT, &elems, NULL, &n);
	types = palloc(sizeof(Oid) * Max(n, 1));
	pr.out = palloc(sizeof(FmgrInfo) * Max(n, 1));
	for (i = 0; i < n; i++)
	{
		Oid			typoutput;
		bool		varlena;

		types[i] = DatumGetObjectId(elems[i]);
		getTypeOutputInfo(types[i], &typoutput, &varlena);
		fmgr_info(typoutput, &pr.out[i]);
	}
	pr.store = rsinfo->setResult;
	pr.desc = rsinfo->setDesc;
	pr.nparams = n;

	for (;;)
	{
		VexecIpcHeader h;
		size_t		consumed;

		if (!vexec_ipc_read_prefix(data + pos, len - pos, &h, &consumed))
			elog(ERROR, "the stream is cut short");
		if (h.kind == VEXEC_IPC_END)
			break;
		pos += consumed;
		if (h.body_len < 0 || (size_t) h.body_len > len - pos)
			elog(ERROR, "the stream is cut short");
		if (state == NULL)
			state = eg->params_begin(h.metadata, h.metadata_len, n, types, NULL);
		else
			(void) eg->params_batch(state, h.metadata, h.metadata_len, data + pos,
									(size_t) h.body_len, param_row, &pr);
		pos += h.body_len;
	}
	if (state)
		eg->params_end(state);
	return (Datum) 0;
}
