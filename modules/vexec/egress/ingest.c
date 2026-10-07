/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * ingest.c
 *	  A client's stream of Arrow record batches as a source of rows
 *	  (pg_vector_executor.md §3.16): the egress API's ingest calls, the
 *	  streams a session has open, vexec.ingest_stream(), and the windows of
 *	  rows VecIngest reads.
 *
 * A caller -- vexec_flight, for Flight SQL's ingest and its prepared
 * updates -- begins a stream with the client's Schema message and a read
 * function, and runs "INSERT INTO t (...) SELECT ... FROM
 * vexec.ingest_stream(handle) AS s(...)", whose column definition list
 * names each column's type.  One reader reads the stream, once:
 *
 *	VecIngest		the function scan's vector path (plan/ingest.c): each
 *					RecordBatch cut into windows of at most VEXEC_BATCH_ROWS
 *					rows.  Where the list names the type the column's Arrow
 *					type names, its buffers are the window's column as they
 *					lie -- booleans, integers, floats, utf8 and binary, uuid
 *					-- or after a check of each value: a date32, time64 or
 *					timestamp in microseconds within PostgreSQL's range, a
 *					decimal128 at the column's scale within its precision,
 *					narrowed to 64 bits where the type's scaled layout has
 *					them, text without NUL.  Where any column is otherwise,
 *					every value of the window is read as a parameter of that
 *					type is (params.c).
 *	the function	vexec.ingest_stream() itself, a row a call, where the
 *					planner chose PostgreSQL's function scan: on a cluster's
 *					coordinator, below its Motion.
 *
 * The client's Arrow is checked as input: the IPC reader checks each
 * message (offsets, views, UTF-8 ...), and the checks above the rest.  A
 * stream lives in the transaction it began in: the session's list of them
 * is emptied as each transaction ends.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "datatype/timestamp.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#include "vexec.h"
#include "batch/arrow_abi.h"
#include "batch/batch.h"
#include "egress/egress.h"
#include "ipc/ipc.h"

typedef struct IngestStream
{
	MemoryContext mcxt;
	int64		handle;
	char	   *schema_md;		/* the Schema message, for its readers */
	size_t		schema_len;
	struct ArrowSchema schema;
	int			ncols;
	VexecIngestColumn *columns;
	VexecEgressReadFn read;
	void	   *arg;
	int64		rows;
	bool		finished;		/* read returned the end */
	bool		opened;			/* a reader has it */
} IngestStream;

/* How a column of the stream becomes a window's column. */
typedef enum IngestCopy
{
	IC_VALUES,					/* a value at a time, as a parameter */
	IC_BITS,					/* bool: the bitmap */
	IC_FIXED,					/* values at their width */
	IC_DATE,					/* date32 at Arrow's epoch, checked */
	IC_TIME,					/* time64[us], checked */
	IC_TIMESTAMP,				/* timestamp[us] at Arrow's epoch, checked */
	IC_DECIMAL,					/* decimal128 at the column's scale, checked */
	IC_OFFSETS,					/* utf8 (checked for NUL) and binary */
	IC_NULL						/* Arrow's null: every row NULL */
} IngestCopy;

typedef struct IngestCol
{
	int			copy;			/* IngestCopy */
	int			width;			/* IC_FIXED */
	int			scale;			/* IC_DECIMAL */
	int128		limit;			/* IC_DECIMAL: 10 ^ the type's digits */
	bool		narrow;			/* IC_DECIMAL: to the 64-bit scaled layout */
	bool		text;			/* IC_OFFSETS: utf8 */
} IngestCol;

struct VexecIngestCursor
{
	IngestStream *stream;
	MemoryContext mcxt;
	MemoryContext msgcxt;		/* the current message's arrays */
	MemoryContext rowcxt;		/* a row's values, read as parameters */
	int			ncols;
	IngestCol  *cols;
	int			nzero;			/* columns read without a copy */
	bool		all_columns;	/* every column so: IC_VALUES in none */
	void	   *values;			/* params.c's state, for IC_VALUES */
	Datum	   *row_values;
	bool	   *row_nulls;
	struct ArrowArray array;	/* the current RecordBatch */
	bool		have_array;
	int64		next;			/* its next row */
	bool		ended;
};

static List *streams = NIL;		/* IngestStream *, this session's */
static int64 next_handle = 0;

/* ---------------------------------------------------------------------
 * The session's streams
 * ---------------------------------------------------------------------
 */

static void
ingest_xact_callback(XactEvent event, void *arg)
{
	(void) arg;
	switch (event)
	{
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			list_free(streams);
			streams = NIL;
			break;
		default:
			break;
	}
}

void
vexec_ingest_install(void)
{
	RegisterXactCallback(ingest_xact_callback, NULL);
}

static IngestStream *
find_stream(int64 handle)
{
	ListCell   *lc;

	foreach(lc, streams)
	{
		IngestStream *s = lfirst(lc);

		if (s->handle == handle)
			return s;
	}
	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("no ingest stream " INT64_FORMAT " in this session", handle),
			 errhint("A stream is begun through vexec's egress API, and read once, in its transaction.")));
	return NULL;				/* keep the compiler quiet */
}

void *
vexec_egress_ingest_begin(const char *metadata, size_t len, VexecEgressReadFn read, void *arg)
{
	MemoryContext mcxt;
	MemoryContext old;
	IngestStream *s;
	int			i;

	vexec_egress_check_active();
	mcxt = AllocSetContextCreate(CurrentMemoryContext, "vexec ingest stream",
								 ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(mcxt);
	s = palloc0(sizeof(IngestStream));
	s->mcxt = mcxt;
	s->schema_md = palloc(Max(len, 1));
	memcpy(s->schema_md, metadata, len);
	s->schema_len = len;
	vexec_ipc_read_schema(s->schema_md, len, &s->schema);
	s->ncols = (int) s->schema.n_children;
	if (s->ncols < 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("the client's stream has no columns")));
	s->columns = palloc0(sizeof(VexecIngestColumn) * s->ncols);
	for (i = 0; i < s->ncols; i++)
	{
		struct ArrowSchema *f = s->schema.children[i];
		VexecIngestColumn *c = &s->columns[i];

		c->name = pstrdup(f->name ? f->name : "");
		vexec_egress_param_natural(f, i + 1, &c->type, &c->typmod);
		c->nullable = (f->flags & ARROW_FLAG_NULLABLE) != 0;
	}
	s->read = read;
	s->arg = arg;
	s->handle = ++next_handle;
	MemoryContextSwitchTo(TopMemoryContext);
	streams = lappend(streams, s);
	MemoryContextSwitchTo(old);
	return s;
}

int
vexec_egress_ingest_columns(void *stream, const VexecIngestColumn **columns)
{
	IngestStream *s = stream;

	*columns = s->columns;
	return s->ncols;
}

int64
vexec_egress_ingest_handle(void *stream)
{
	return ((IngestStream *) stream)->handle;
}

int64
vexec_egress_ingest_rows(void *stream)
{
	return ((IngestStream *) stream)->rows;
}

bool
vexec_egress_ingest_finished(void *stream)
{
	return ((IngestStream *) stream)->finished;
}

void
vexec_egress_ingest_end(void *stream)
{
	IngestStream *s = stream;
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	streams = list_delete_ptr(streams, s);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(s->mcxt);
}

/* ---------------------------------------------------------------------
 * A stream's reader
 * ---------------------------------------------------------------------
 */

/* How a column whose type the list names reads, the Arrow type given. */
static void
column_copy(IngestCol *ic, const struct ArrowSchema *field, const VexecIngestColumn *natural,
			Oid type, int32 typmod)
{
	const char *f = field->format;

	ic->copy = IC_VALUES;
	if (type != natural->type)
		return;
	if (strcmp(f, "n") == 0)
		ic->copy = IC_NULL;
	else if (strcmp(f, "b") == 0)
		ic->copy = IC_BITS;
	else if (strcmp(f, "s") == 0 || strcmp(f, "i") == 0 || strcmp(f, "l") == 0 ||
			 strcmp(f, "f") == 0 || strcmp(f, "g") == 0)
	{
		ic->copy = IC_FIXED;
		ic->width = f[0] == 's' ? 2 : (f[0] == 'i' || f[0] == 'f') ? 4 : 8;
	}
	else if (strcmp(f, "w:16") == 0 && type == UUIDOID)
	{
		ic->copy = IC_FIXED;
		ic->width = 16;
	}
	else if (strcmp(f, "tdD") == 0)
		ic->copy = IC_DATE;
	else if (strcmp(f, "ttu") == 0)
		ic->copy = IC_TIME;
	else if (strncmp(f, "tsu:", 4) == 0)
		ic->copy = IC_TIMESTAMP;
	else if (strcmp(f, "u") == 0 && type == TEXTOID && GetDatabaseEncoding() == PG_UTF8)
	{
		ic->copy = IC_OFFSETS;
		ic->text = true;
	}
	else if (strcmp(f, "z") == 0 && type == BYTEAOID)
		ic->copy = IC_OFFSETS;
	else if (f[0] == 'd' && f[1] == ':')
	{
		int			precision;
		int			scale;
		int			bits = 128;
		VexecType  *vt;

		if (sscanf(f, "d:%d,%d,%d", &precision, &scale, &bits) < 2 || bits != 128 ||
			typmod < 0)
			return;
		vt = vexec_type_make(type, typmod, InvalidOid);
		if (vt->scaled_width == 0 || vt->scale != scale || vt->digits <= 0 || vt->digits > 38)
			return;
		ic->copy = IC_DECIMAL;
		ic->scale = scale;
		ic->narrow = vt->scaled_width == 8;
		ic->limit = 1;
		for (int d = 0; d < vt->digits; d++)
			ic->limit *= 10;
	}
}

VexecIngestCursor *
vexec_ingest_open(int64 handle, int ncols, const Oid *types, const int32 *typmods)
{
	IngestStream *s = find_stream(handle);
	VexecIngestCursor *c;
	MemoryContext mcxt;
	MemoryContext old;
	int			i;

	if (s->opened)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the ingest stream " INT64_FORMAT " has a reader already", handle),
				 errdetail("A client's stream is read once, by one statement.")));
	if (ncols != s->ncols)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("the column definition list of vexec.ingest_stream(" INT64_FORMAT ") has %d columns, and its stream has %d",
						handle, ncols, s->ncols)));

	mcxt = AllocSetContextCreate(CurrentMemoryContext, "vexec ingest reader",
								 ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(mcxt);
	c = palloc0(sizeof(VexecIngestCursor));
	c->stream = s;
	c->mcxt = mcxt;
	c->msgcxt = AllocSetContextCreate(mcxt, "vexec ingest message", ALLOCSET_DEFAULT_SIZES);
	c->rowcxt = AllocSetContextCreate(mcxt, "vexec ingest row", ALLOCSET_DEFAULT_SIZES);
	c->ncols = ncols;
	c->cols = palloc0(sizeof(IngestCol) * ncols);
	c->row_values = palloc(sizeof(Datum) * ncols);
	c->row_nulls = palloc(sizeof(bool) * ncols);
	c->values = vexec_egress_stream_columns(s->schema_md, s->schema_len, ncols, types, typmods);
	c->all_columns = true;
	for (i = 0; i < ncols; i++)
	{
		column_copy(&c->cols[i], s->schema.children[i], &s->columns[i], types[i], typmods[i]);
		if (c->cols[i].copy == IC_VALUES)
			c->all_columns = false;
		else
			c->nzero++;
	}
	s->opened = true;
	MemoryContextSwitchTo(old);
	return c;
}

void
vexec_ingest_close(VexecIngestCursor *c)
{
	if (c == NULL)
		return;
	vexec_egress_params_end(c->values);
	MemoryContextDelete(c->mcxt);
}

/* Whether every column is read without a copy, as EXPLAIN tells. */
bool
vexec_ingest_zero_copy(VexecIngestCursor *c)
{
	return c != NULL && c->all_columns;
}

/* The stream's next RecordBatch with rows in it; false at its end. */
static bool
next_array(VexecIngestCursor *c)
{
	IngestStream *s = c->stream;

	for (;;)
	{
		const char *md;
		size_t		mdlen;
		const char *body;
		size_t		bodylen;
		VexecIpcHeader hdr;
		MemoryContext old;

		if (c->ended)
			return false;
		c->have_array = false;
		MemoryContextReset(c->msgcxt);
		if (!s->read(s->arg, &md, &mdlen, &body, &bodylen))
		{
			c->ended = true;
			s->finished = true;
			return false;
		}
		vexec_ipc_read_message(md, mdlen, &hdr);
		if (hdr.kind != VEXEC_IPC_RECORD_BATCH)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("the client's stream sends another Schema message after its first")));
		old = MemoryContextSwitchTo(c->msgcxt);
		vexec_ipc_read_batch(&s->schema, md, mdlen, body, bodylen, &c->array);
		MemoryContextSwitchTo(old);
		if (c->array.length == 0)
			continue;
		c->have_array = true;
		c->next = 0;
		return true;
	}
}

pg_noreturn static void
out_of_range(int column, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			 errmsg("column %d of the client's stream holds %s out of PostgreSQL's range",
					column, what)));
}

static inline bool
row_valid(const uint8 *validity, int64 i)
{
	return validity == NULL || ((validity[i >> 3] >> (i & 7)) & 1);
}

/* A window of a column, its buffers taken as they lie, or checked first. */
static void
take_column(VexecIngestCursor *c, int col, VexecBatch *batch, int64 start, int n)
{
	const IngestCol *ic = &c->cols[col];
	const struct ArrowArray *a = c->array.children[col];
	VexecVec   *v = &batch->cols[col];
	const uint8 *validity = (a->null_count != 0 && a->buffers[0] != NULL) ? a->buffers[0] : NULL;
	const char *vals = a->n_buffers > 1 ? a->buffers[1] : NULL;
	int			k;

	memset(&v->shape, 0, sizeof(VexecShape));
	v->encoding = VEXEC_FLAT;
	v->nvalues = n;
	/* a window starts at a multiple of 1024 rows: whole uint64 words */
	v->validity = validity != NULL ? (uint64 *) (validity + start / 8) : NULL;
	v->buffers = NULL;
	v->buffer_sizes = NULL;
	v->nbuffers = 0;
	v->datums = NULL;
	v->codes = NULL;
	v->dictionary = NULL;

	switch (ic->copy)
	{
		case IC_NULL:
			vexec_type_build_shape(v->type, &v->shape);
			v->encoding = VEXEC_CONST;
			v->nvalues = 1;
			v->values = vexec_batch_alloc0(batch, 16);
			v->validity = vexec_bitmap_alloc(batch, 1, false);
			return;
		case IC_BITS:
			v->shape.layout = VEXEC_BIT_BOOL;
			v->values = (void *) (vals + start / 8);
			break;
		case IC_FIXED:
			v->shape.layout = VEXEC_FIXED;
			v->shape.width = ic->width;
			v->shape.stride = ic->width;
			v->values = (void *) (vals + start * ic->width);
			break;
		case IC_DATE:
			{
				const int32 *d = (const int32 *) vals + start;

				for (k = 0; k < n; k++)
					if (row_valid(validity, start + k) &&
						!IS_VALID_DATE((int64) d[k] - VEXEC_EPOCH_DAYS))
						out_of_range(col + 1, "a date");
				v->shape.layout = VEXEC_FIXED;
				v->shape.arrow_values = true;
				v->shape.width = 4;
				v->shape.stride = 4;
				v->values = (void *) d;
				break;
			}
		case IC_TIME:
			{
				const int64 *t = (const int64 *) vals + start;

				for (k = 0; k < n; k++)
					if (row_valid(validity, start + k) && (t[k] < 0 || t[k] > USECS_PER_DAY))
						out_of_range(col + 1, "a time");
				v->shape.layout = VEXEC_FIXED;
				v->shape.width = 8;
				v->shape.stride = 8;
				v->values = (void *) t;
				break;
			}
		case IC_TIMESTAMP:
			{
				const int64 *t = (const int64 *) vals + start;
				bool		sentinel = false;

				for (k = 0; k < n; k++)
				{
					int64		ts;

					if (!row_valid(validity, start + k))
						continue;
					if (pg_sub_s64_overflow(t[k], VEXEC_EPOCH_USECS, &ts) ||
						!IS_VALID_TIMESTAMP(ts))
						out_of_range(col + 1, "a timestamp");
					sentinel |= t[k] == PG_INT64_MAX;
				}
				v->shape.layout = VEXEC_FIXED;
				v->shape.width = 8;
				v->shape.stride = 8;
				if (!sentinel)
				{
					v->shape.arrow_values = true;
					v->values = (void *) t;
					break;
				}

				/*
				 * Arrow has no infinities: its largest instant is a
				 * timestamp PostgreSQL holds, 294247-01-10
				 * 04:00:54.775807, which a batch at Arrow's epoch would
				 * read as infinity (§3.4.2).  A window holding it is
				 * copied at PostgreSQL's epoch.
				 */
				{
					int64	   *out = vexec_batch_alloc(batch, sizeof(int64) * Max(n, 1));

					for (k = 0; k < n; k++)
						out[k] = row_valid(validity, start + k) ? t[k] - VEXEC_EPOCH_USECS : 0;
					v->values = out;
				}
				break;
			}
		case IC_DECIMAL:
			{
				const char *p = vals + start * 16;

				for (k = 0; k < n; k++)
				{
					int128		x;

					if (!row_valid(validity, start + k))
						continue;
					memcpy(&x, p + k * 16, 16);
					if (x >= ic->limit || x <= -ic->limit)
						ereport(ERROR,
								(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
								 errmsg("numeric field overflow"),
								 errdetail("Column %d of the client's stream holds a value beyond its column's precision.",
										   col + 1)));
				}
				v->shape.layout = VEXEC_SCALED;
				v->shape.scale = ic->scale;
				if (!ic->narrow)
				{
					v->shape.width = 16;
					v->shape.stride = 16;
					v->values = (void *) p;
				}
				else
				{
					int64	   *out = vexec_batch_alloc(batch, sizeof(int64) * Max(n, 1));

					for (k = 0; k < n; k++)
					{
						int128		x;

						memcpy(&x, p + k * 16, 16);
						out[k] = row_valid(validity, start + k) ? (int64) x : 0;
					}
					v->shape.width = 8;
					v->shape.stride = 8;
					v->values = out;
				}
				break;
			}
		case IC_OFFSETS:
			{
				const int32 *off = (const int32 *) vals + start;
				const char *data = a->buffers[2];
				char	  **bufs = vexec_batch_alloc(batch, sizeof(char *));
				int64	   *sizes = vexec_batch_alloc(batch, sizeof(int64));

				if (ic->text)
					for (k = 0; k < n; k++)
						if (row_valid(validity, start + k) && off[k + 1] > off[k] &&
							memchr(data + off[k], '\0', off[k + 1] - off[k]) != NULL)
							ereport(ERROR,
									(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
									 errmsg("column %d of the client's stream holds a NUL character, which text cannot",
											col + 1)));
				bufs[0] = (char *) data;
				sizes[0] = ((const int32 *) vals)[a->length];
				v->shape.layout = VEXEC_OFFSETS;
				v->values = (void *) off;
				v->buffers = bufs;
				v->buffer_sizes = sizes;
				v->nbuffers = 1;
				break;
			}
		default:
			elog(ERROR, "vexec: unexpected ingest column kind %d", ic->copy);
	}
	vexec_shape_normalize(&v->shape);
}

/*
 * The stream's next window into a batch of its columns, in the layouts the
 * format in effect gives them; false at the end of the stream.
 */
bool
vexec_ingest_next_window(VexecIngestCursor *c, VexecBatch *batch, const VexecLayoutConfig *layout)
{
	int64		start;
	int			n;
	int			i;

	if ((!c->have_array || c->next >= c->array.length) && !next_array(c))
		return false;
	start = c->next;
	n = (int) Min((int64) VEXEC_BATCH_ROWS, c->array.length - start);
	vexec_batch_reset(batch);
	if (c->all_columns)
	{
		batch->nrows = n;
		for (i = 0; i < c->ncols; i++)
			take_column(c, i, batch, start, n);
	}
	else
	{
		int			r;

		vexec_batch_begin_rows(batch);
		for (r = 0; r < n; r++)
		{
			MemoryContext old = MemoryContextSwitchTo(c->rowcxt);

			for (i = 0; i < c->ncols; i++)
				c->row_values[i] = vexec_egress_stream_value(c->values, i, c->array.children[i],
															 start + r, &c->row_nulls[i]);
			MemoryContextSwitchTo(old);
			vexec_batch_add_values(batch, c->row_values, c->row_nulls);
			vexec_egress_stream_row_done(c->values);
			MemoryContextReset(c->rowcxt);
		}
	}
	c->next = start + n;
	c->stream->rows += n;
	vexec_batch_apply_config(batch, layout);
	return true;
}

/* The stream's next row, its values as the column definition list types them. */
bool
vexec_ingest_next_row(VexecIngestCursor *c, Datum *values, bool *isnull)
{
	int			i;

	if ((!c->have_array || c->next >= c->array.length) && !next_array(c))
		return false;
	for (i = 0; i < c->ncols; i++)
		values[i] = vexec_egress_stream_value(c->values, i, c->array.children[i], c->next,
											  &isnull[i]);
	vexec_egress_stream_row_done(c->values);
	c->next++;
	c->stream->rows++;
	return true;
}

/* ---------------------------------------------------------------------
 * vexec.ingest_stream(handle bigint) RETURNS SETOF record
 * ---------------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(vexec_ingest_stream);

Datum
vexec_ingest_stream(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	VexecIngestCursor *c;
	Datum	   *values;
	bool	   *nulls;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext old;
		TupleDesc	desc;
		Oid		   *types;
		int32	   *typmods;
		int			i;

		funcctx = SRF_FIRSTCALL_INIT();
		old = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("vexec.ingest_stream() is read through a column definition list")));
		desc = BlessTupleDesc(desc);
		types = palloc(sizeof(Oid) * Max(desc->natts, 1));
		typmods = palloc(sizeof(int32) * Max(desc->natts, 1));
		for (i = 0; i < desc->natts; i++)
		{
			types[i] = TupleDescAttr(desc, i)->atttypid;
			typmods[i] = TupleDescAttr(desc, i)->atttypmod;
		}
		funcctx->user_fctx = vexec_ingest_open(PG_GETARG_INT64(0), desc->natts, types, typmods);
		funcctx->tuple_desc = desc;
		MemoryContextSwitchTo(old);
	}
	funcctx = SRF_PERCALL_SETUP();
	c = funcctx->user_fctx;
	values = palloc(sizeof(Datum) * Max(funcctx->tuple_desc->natts, 1));
	nulls = palloc(sizeof(bool) * Max(funcctx->tuple_desc->natts, 1));
	if (vexec_ingest_next_row(c, values, nulls))
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc,
																   values, nulls)));
	vexec_ingest_close(c);
	funcctx->user_fctx = NULL;
	SRF_RETURN_DONE(funcctx);
}
