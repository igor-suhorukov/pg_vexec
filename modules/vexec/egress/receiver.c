/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * receiver.c
 *	  The egress API's receiver: a statement's result as Arrow IPC messages
 *	  (pg_vector_executor.md §3.15, V10; vexec_egress.h).
 *
 * Each column of the result has a fixed Arrow type, chosen by its type
 * when the receiver starts (the table in vexec_egress.h), so that the
 * stream's schema holds before the first batch (Columnar.rst:1470-1477).
 * The rows go into a batch of up to VEXEC_BATCH_ROWS rows, a column at a
 * time in Arrow's buffers, and each full batch goes out as a RecordBatch
 * message through vexec's IPC writer (ipc/), its buffers in a memory
 * context that is reset as soon as the caller has sent it.  A value its
 * column's type cannot hold fails the statement, naming the column.
 *
 * Batches.  Where a vector node is at the top of the plan, run.c hands the
 * receiver the node's batches instead of rows (vexec_egress_send_batch).
 * A column whose layout already is its Arrow type's -- fixed-width numbers
 * at their width, bit-packed booleans, decimals at 16 bytes, dates and
 * timestamps at Arrow's epoch, intervals as month_day_nano, utf8 and
 * binary with offsets -- goes out from the batch's own buffers; any other
 * is converted a value at a time, from the Datum the batch layer gives
 * (vexec_vec_datum).  Rows the node's kernels sent to PostgreSQL's
 * evaluator are resolved in row order, as a vector parent resolves them
 * (exec/node.c), so that a row that raises raises where PostgreSQL would.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/transam.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "datatype/timestamp.h"
#include "executor/tuptable.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "utils/xid8.h"
#include "varatt.h"

#include "vexec_egress.h"

#include "vexec.h"
#include "batch/arrow_abi.h"
#include "batch/batch.h"
#include "egress/egress.h"
#include "exec/exec.h"
#include "expr/expr.h"
#include "ipc/ipc.h"

/* Arrow's buffers are written as the machine holds them, and said little-endian. */
#ifdef WORDS_BIGENDIAN
#error "vexec's egress writes Arrow little-endian, as ipc/ says"
#endif

/*
 * A batch goes out early when its variable-length bytes pass this, so that
 * a session's memory stays at one modest batch however wide its rows; and
 * always before its offsets would leave int32.
 */
#define EGRESS_BATCH_BYTES	(32 * 1024 * 1024)

/* How a column goes out. */
typedef enum EgressKind
{
	EGRESS_BOOL,
	EGRESS_INT,					/* any integer format, from an integer type */
	EGRESS_FLOAT4,
	EGRESS_FLOAT8,
	EGRESS_DECIMAL,
	EGRESS_DATE,
	EGRESS_TIME,
	EGRESS_TIMESTAMP,
	EGRESS_INTERVAL,
	EGRESS_UUID,
	EGRESS_TEXT,				/* text's bytes: text, varchar, bpchar, json */
	EGRESS_NAME,
	EGRESS_BYTEA,
	EGRESS_OUTPUT				/* the type's output function */
} EgressKind;

/* A growable buffer that may pass 1GB, which a StringInfo may not. */
typedef struct EgressBytes
{
	char	   *data;
	int64		len;
	int64		cap;
} EgressBytes;

typedef struct EgressColumn
{
	const char *name;
	Oid			typid;
	int32		typmod;
	VexecType  *type;			/* vexec's view of it: base type, utf8 */
	uint8		kind;			/* EgressKind */
	char		format[32];		/* Arrow's format string */
	int			width;			/* fixed-width values: bytes a value */
	bool		natural;		/* the type's own format, not the caller's */
	bool		is_signed;		/* EGRESS_INT */
	int64		min;			/* EGRESS_INT: the format's range */
	uint64		max;
	int16		digits;			/* EGRESS_DECIMAL: vexec's bound */
	int16		scale;
	bool		not_null;
	bool		to_utf8;		/* text of another encoding */
	bool		check_utf8;		/* SQL_ASCII: bytes checked as UTF-8 */
	FmgrInfo	output;			/* EGRESS_OUTPUT */
	char	   *metadata;		/* the field's, C Data Interface encoding */

	/* the batch being built, in the receiver's batch context */
	uint64	   *validity;		/* NULL until the first NULL */
	int64		nulls;
	char	   *values;			/* fixed width; bits for bool */
	int32	   *offsets;		/* utf8, binary */
	EgressBytes data;

	/* what the next message sends of the column */
	const void *out_validity;
	const void *out_values;
	const void *out_data;
	int64		out_nulls;
} EgressColumn;

typedef struct EgressReceiver
{
	DestReceiver pub;			/* first */
	VexecEgressWriteFn write;
	void	   *arg;
	VexecEgressField *fields;	/* the caller's, copied, or NULL */
	int			nfields;
	MemoryContext mcxt;			/* the receiver's */
	MemoryContext batchcxt;		/* a batch's buffers */
	bool		started;
	int			ncols;
	EgressColumn *cols;
	struct ArrowSchema schema;	/* "+s", a child a column */
	struct ArrowSchema **children;
	VexecIpcWriter *writer;
	int			nrows;			/* rows in the batch being built */
	int64		bytes;			/* its variable-length bytes */
	int64		rows_sent;
	int64		batches_sent;
	int64		bytes_sent;
	int64		vector_batches; /* sent from a vector node's buffers */
} EgressReceiver;

static bool egress_receive_slot(TupleTableSlot *slot, DestReceiver *self);
static void egress_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static void egress_shutdown(DestReceiver *self);
static void egress_destroy(DestReceiver *self);

/* The structures' release callbacks: the memory is the receiver's. */
static void
release_schema(struct ArrowSchema *s)
{
	s->release = NULL;
}

static void
release_array(struct ArrowArray *a)
{
	a->release = NULL;
}

/* ---------------------------------------------------------------------
 * Columns
 * ---------------------------------------------------------------------
 */

/* Field metadata in the C Data Interface's encoding (CDataInterface.rst:353-383). */
static char *
encode_metadata(int n, const char *const *keys, const char *const *values)
{
	Size		len = sizeof(int32);
	char	   *buf;
	char	   *p;
	int32		v;
	int			i;

	if (n == 0)
		return NULL;
	for (i = 0; i < n; i++)
		len += 2 * sizeof(int32) + strlen(keys[i]) + strlen(values[i]);
	buf = palloc(len);
	p = buf;
	v = n;
	memcpy(p, &v, sizeof(int32));
	p += sizeof(int32);
	for (i = 0; i < n; i++)
	{
		v = (int32) strlen(keys[i]);
		memcpy(p, &v, sizeof(int32));
		p += sizeof(int32);
		memcpy(p, keys[i], v);
		p += v;
		v = (int32) strlen(values[i]);
		memcpy(p, &v, sizeof(int32));
		p += sizeof(int32);
		memcpy(p, values[i], v);
		p += v;
	}
	return buf;
}

/* An integer format's width, signedness and range; false if it is none. */
static bool
integer_format(const char *format, int *width, bool *is_signed, int64 *min, uint64 *max)
{
	if (format[0] == '\0' || format[1] != '\0')
		return false;
	switch (format[0])
	{
		case 'c':
			*width = 1, *is_signed = true, *min = PG_INT8_MIN, *max = PG_INT8_MAX;
			return true;
		case 'C':
			*width = 1, *is_signed = false, *min = 0, *max = PG_UINT8_MAX;
			return true;
		case 's':
			*width = 2, *is_signed = true, *min = PG_INT16_MIN, *max = PG_INT16_MAX;
			return true;
		case 'S':
			*width = 2, *is_signed = false, *min = 0, *max = PG_UINT16_MAX;
			return true;
		case 'i':
			*width = 4, *is_signed = true, *min = PG_INT32_MIN, *max = PG_INT32_MAX;
			return true;
		case 'I':
			*width = 4, *is_signed = false, *min = 0, *max = PG_UINT32_MAX;
			return true;
		case 'l':
			*width = 8, *is_signed = true, *min = PG_INT64_MIN, *max = PG_INT64_MAX;
			return true;
		case 'L':
			*width = 8, *is_signed = false, *min = 0, *max = PG_UINT64_MAX;
			return true;
	}
	return false;
}

/* A column's Arrow type, from its type and what the caller asks. */
static void
column_init(EgressColumn *c, Form_pg_attribute att, const VexecEgressField *field)
{
	const char *keys[16];
	const char *values[16];
	int			nmeta = 0;
	const char *extension = NULL;
	Oid			basetype;
	int			i;

	memset(c, 0, sizeof(*c));
	c->name = pstrdup(field && field->name ? field->name : NameStr(att->attname));
	c->typid = att->atttypid;
	c->typmod = att->atttypmod;
	c->type = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
	c->not_null = field && field->not_null;
	basetype = c->type->basetype;

	switch (basetype)
	{
		case BOOLOID:
			c->kind = EGRESS_BOOL;
			strcpy(c->format, "b");
			break;
		case CHAROID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case OIDOID:
		case XIDOID:
		case CIDOID:
		case XID8OID:
			c->kind = EGRESS_INT;
			strcpy(c->format,
				   basetype == CHAROID ? "c" :
				   basetype == INT2OID ? "s" :
				   basetype == INT4OID ? "i" :
				   basetype == INT8OID ? "l" :
				   basetype == XID8OID ? "L" : "I");
			break;
		case FLOAT4OID:
			c->kind = EGRESS_FLOAT4;
			c->width = 4;
			strcpy(c->format, "f");
			break;
		case FLOAT8OID:
			c->kind = EGRESS_FLOAT8;
			c->width = 8;
			strcpy(c->format, "g");
			break;
		case NUMERICOID:
			if (c->type->scaled_width > 0)
			{
				/*
				 * A typmod bounds it to 38 digits: decimal128.  A scale above
				 * the precision, numeric(3,10), needs as many digits as its
				 * scale.
				 */
				c->kind = EGRESS_DECIMAL;
				c->width = 16;
				c->digits = c->type->digits;
				c->scale = c->type->scale;
				snprintf(c->format, sizeof(c->format), "d:%d,%d",
						 Max(c->digits, c->scale), c->scale);
			}
			else
				c->kind = EGRESS_OUTPUT;
			break;
		case DATEOID:
			c->kind = EGRESS_DATE;
			c->width = 4;
			strcpy(c->format, "tdD");
			break;
		case TIMEOID:
			c->kind = EGRESS_TIME;
			c->width = 8;
			strcpy(c->format, "ttu");
			break;
		case TIMESTAMPOID:
			c->kind = EGRESS_TIMESTAMP;
			c->width = 8;
			strcpy(c->format, "tsu:");
			break;
		case TIMESTAMPTZOID:
			c->kind = EGRESS_TIMESTAMP;
			c->width = 8;
			strcpy(c->format, "tsu:UTC");
			break;
		case INTERVALOID:
			c->kind = EGRESS_INTERVAL;
			c->width = 16;
			strcpy(c->format, "tin");
			break;
		case UUIDOID:
			c->kind = EGRESS_UUID;
			c->width = UUID_LEN;
			strcpy(c->format, "w:16");
			extension = "arrow.uuid";
			break;
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case JSONOID:
			c->kind = EGRESS_TEXT;
			strcpy(c->format, "u");
			if (basetype == JSONOID)
				extension = "arrow.json";
			break;
		case NAMEOID:
			c->kind = EGRESS_NAME;
			strcpy(c->format, "u");
			break;
		case BYTEAOID:
			c->kind = EGRESS_BYTEA;
			strcpy(c->format, "z");
			break;
		default:
			c->kind = EGRESS_OUTPUT;
			break;
	}
	if (c->kind == EGRESS_OUTPUT)
	{
		Oid			typoutput;
		bool		typisvarlena;

		getTypeOutputInfo(c->typid, &typoutput, &typisvarlena);
		fmgr_info(typoutput, &c->output);
		strcpy(c->format, "u");
		if (basetype == JSONBOID)
			extension = "arrow.json";
	}
	if (c->kind == EGRESS_TEXT || c->kind == EGRESS_NAME || c->kind == EGRESS_OUTPUT)
	{
		int			enc = GetDatabaseEncoding();

		c->to_utf8 = enc != PG_UTF8 && enc != PG_SQL_ASCII;
		c->check_utf8 = enc == PG_SQL_ASCII;
	}
	c->natural = field == NULL || field->format == NULL || strcmp(field->format, c->format) == 0;
	if (c->kind == EGRESS_INT)
	{
		const char *format = field && field->format ? field->format : c->format;

		if (!integer_format(format, &c->width, &c->is_signed, &c->min, &c->max))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("column \"%s\" cannot go out as Arrow format \"%s\"", c->name, format)));
		strlcpy(c->format, format, sizeof(c->format));
	}
	else if (field && field->format && strcmp(field->format, c->format) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("column \"%s\" of type %s cannot go out as Arrow format \"%s\"",
						c->name, format_type_be(c->typid), field->format)));

	/* the metadata: the PostgreSQL type, an extension's, the caller's */
	if (!(field && field->bare))
	{
		keys[nmeta] = "pg_type";
		values[nmeta++] = psprintf("%s.%s", c->type->nspname, c->type->name);
		keys[nmeta] = "pg_typmod";
		values[nmeta++] = psprintf("%d", c->typmod);
	}
	if (extension)
	{
		keys[nmeta] = "ARROW:extension:name";
		values[nmeta++] = extension;
		keys[nmeta] = "ARROW:extension:metadata";
		values[nmeta++] = "";
	}
	if (field && field->nmetadata > 0)
	{
		if (nmeta + field->nmetadata > lengthof(keys))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("too many metadata keys for column \"%s\"", c->name)));
		for (i = 0; i < field->nmetadata; i++)
		{
			keys[nmeta] = field->keys[i];
			values[nmeta++] = field->values[i];
		}
	}
	c->metadata = encode_metadata(nmeta, keys, values);
}

/* The schema of the columns: a struct, a child a column. */
static void
schema_init(EgressReceiver *r)
{
	int			i;

	memset(&r->schema, 0, sizeof(r->schema));
	r->schema.format = "+s";
	r->schema.name = "";
	r->schema.n_children = r->ncols;
	r->children = palloc0(sizeof(struct ArrowSchema *) * Max(r->ncols, 1));
	r->schema.children = r->children;
	r->schema.release = release_schema;
	for (i = 0; i < r->ncols; i++)
	{
		struct ArrowSchema *s = palloc0(sizeof(struct ArrowSchema));
		EgressColumn *c = &r->cols[i];

		s->format = c->format;
		s->name = c->name;
		s->metadata = c->metadata;
		s->flags = c->not_null ? 0 : ARROW_FLAG_NULLABLE;
		s->release = release_schema;
		r->children[i] = s;
	}
}

/* The columns of a result, and its schema, in the receiver's context. */
static void
columns_init(EgressReceiver *r, TupleDesc desc)
{
	MemoryContext old = MemoryContextSwitchTo(r->mcxt);
	int			i;

	if (r->fields != NULL && r->nfields != desc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the egress was given %d fields for a result of %d columns",
						r->nfields, desc->natts)));
	r->ncols = desc->natts;
	r->cols = palloc0(sizeof(EgressColumn) * Max(r->ncols, 1));
	for (i = 0; i < r->ncols; i++)
		column_init(&r->cols[i], TupleDescAttr(desc, i), r->fields ? &r->fields[i] : NULL);
	schema_init(r);
	r->writer = vexec_ipc_writer_create(r->mcxt);
	MemoryContextSwitchTo(old);
}

/* ---------------------------------------------------------------------
 * Building a batch, a value at a time
 * ---------------------------------------------------------------------
 */

static void
bytes_append(EgressReceiver *r, EgressBytes *b, const char *p, int64 len)
{
	if (b->len + len > b->cap)
	{
		int64		cap = Max(b->cap * 2, 1024);

		while (cap < b->len + len)
			cap *= 2;
		if (b->data == NULL)
			b->data = MemoryContextAllocExtended(r->batchcxt, cap, MCXT_ALLOC_HUGE);
		else
			b->data = repalloc_huge(b->data, cap);
		b->cap = cap;
	}
	memcpy(b->data + b->len, p, len);
	b->len += len;
}

/* The column's buffers for a batch, empty. */
static void
column_begin(EgressReceiver *r, EgressColumn *c)
{
	c->validity = NULL;
	c->nulls = 0;
	c->values = NULL;
	c->offsets = NULL;
	memset(&c->data, 0, sizeof(c->data));
	switch (c->kind)
	{
		case EGRESS_BOOL:
			c->values = MemoryContextAllocZero(r->batchcxt, VEXEC_WORDS(VEXEC_BATCH_ROWS) * 8);
			break;
		case EGRESS_TEXT:
		case EGRESS_NAME:
		case EGRESS_BYTEA:
		case EGRESS_OUTPUT:
			c->offsets = MemoryContextAllocZero(r->batchcxt, sizeof(int32) * (VEXEC_BATCH_ROWS + 1));
			break;
		default:
			c->values = MemoryContextAllocZero(r->batchcxt, (Size) c->width * VEXEC_BATCH_ROWS);
			break;
	}
}

static void
batch_begin(EgressReceiver *r)
{
	int			i;

	for (i = 0; i < r->ncols; i++)
		column_begin(r, &r->cols[i]);
	r->nrows = 0;
	r->bytes = 0;
}

/*
 * Arrow's name of a type, by its C Data format string, for a message: the
 * types a value can fail to fit, by name; any other as its format string.
 */
static const char *
arrow_type_name(const char *format)
{
	if (strcmp(format, "u") == 0)
		return "utf8";
	if (strcmp(format, "tdD") == 0)
		return "date32";
	if (strcmp(format, "ttu") == 0)
		return "time64[us]";
	if (strcmp(format, "tsu:") == 0)
		return "timestamp[us]";
	if (strncmp(format, "tsu:", 4) == 0)
		return psprintf("timestamp[us, tz=%s]", format + 4);
	if (strcmp(format, "tin") == 0)
		return "month_day_nano_interval";
	if (strncmp(format, "d:", 2) == 0)
	{
		int			precision;
		int			scale;

		if (sscanf(format + 2, "%d,%d", &precision, &scale) == 2)
			return psprintf("decimal128(%d, %d)", precision, scale);
	}
	return format;
}

pg_noreturn static void
cannot_hold(EgressColumn *c, int sqlstate, const char *what)
{
	ereport(ERROR,
			(errcode(sqlstate),
			 errmsg("value of column \"%s\" cannot be sent as Arrow type %s",
					c->name, arrow_type_name(c->format)),
			 errdetail("The column holds %s, which a stream of Arrow cannot carry in that type.", what)));
}

/* Bytes of utf8 or binary, converted to UTF-8 where they are text of another encoding. */
static void
append_varlen(EgressReceiver *r, EgressColumn *c, int row, const char *p, int64 len)
{
	char	   *converted = NULL;

	if (c->to_utf8 && len > 0)
	{
		converted = pg_server_to_any(p, (int) len, PG_UTF8);
		if (converted != p)
		{
			p = converted;
			len = strlen(converted);
		}
		else
			converted = NULL;
	}
	else if (c->check_utf8 && !pg_verify_mbstr(PG_UTF8, p, (int) len, true))
		cannot_hold(c, ERRCODE_CHARACTER_NOT_IN_REPERTOIRE, "bytes that are not UTF-8");
	if ((int64) c->data.len + len > PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a value of column \"%s\" is too long for an Arrow batch", c->name)));
	bytes_append(r, &c->data, p, len);
	c->offsets[row + 1] = (int32) c->data.len;
	r->bytes += len;
	if (converted)
		pfree(converted);
}

/* An integer Datum of the column's type, widened. */
static int128
integer_value(EgressColumn *c, Datum d)
{
	switch (c->type->basetype)
	{
		case CHAROID:
			return (int8) DatumGetChar(d);
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
			return DatumGetInt32(d);
		case INT8OID:
			return DatumGetInt64(d);
		case XID8OID:
			return (int128) U64FromFullTransactionId(DatumGetFullTransactionId(d));
		default:				/* oid, xid, cid */
			return (int128) DatumGetUInt32(d);
	}
}

/* Append one value, or a NULL, to row `row` of a column. */
static void
column_append(EgressReceiver *r, EgressColumn *c, int row, Datum d, bool isnull)
{
	if (isnull)
	{
		if (c->not_null)
			ereport(ERROR,
					(errcode(ERRCODE_NOT_NULL_VIOLATION),
					 errmsg("column \"%s\" of the result holds a NULL, which its field does not admit",
							c->name)));
		if (c->validity == NULL)
		{
			int			i;

			c->validity = MemoryContextAllocZero(r->batchcxt, VEXEC_WORDS(VEXEC_BATCH_ROWS) * 8);
			for (i = 0; i < row; i++)
				vexec_bit_set(c->validity, i);
		}
		c->nulls++;
		if (c->offsets)
			c->offsets[row + 1] = c->offsets[row];
		return;
	}
	if (c->validity)
		vexec_bit_set(c->validity, row);

	switch (c->kind)
	{
		case EGRESS_BOOL:
			if (DatumGetBool(d))
				vexec_bit_set((uint64 *) c->values, row);
			break;
		case EGRESS_INT:
			{
				int128		v = integer_value(c, d);

				if (v < (int128) c->min || v > (int128) c->max)
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("a value of column \"%s\" is out of range for Arrow's %s",
									c->name, c->format)));
				if (c->is_signed || c->width < 8)
				{
					int64		v64 = (int64) v;

					/* little-endian: the low bytes are the value */
					memcpy(c->values + (Size) row * c->width, &v64, c->width);
				}
				else
				{
					uint64		u64 = (uint64) v;

					memcpy(c->values + (Size) row * 8, &u64, 8);
				}
				break;
			}
		case EGRESS_FLOAT4:
			{
				float4		f = DatumGetFloat4(d);

				memcpy(c->values + (Size) row * 4, &f, 4);
				break;
			}
		case EGRESS_FLOAT8:
			{
				float8		f = DatumGetFloat8(d);

				memcpy(c->values + (Size) row * 8, &f, 8);
				break;
			}
		case EGRESS_DECIMAL:
			{
				int128		v;

				if (!vexec_numeric_to_scaled(d, c->scale, c->digits, 16, &v))
				{
					int			dscale = vexec_numeric_dscale(d);

					/* NaN, an infinity, or a value not at the typmod's scale */
					if (numeric_is_nan(DatumGetNumeric(d)) || numeric_is_inf(DatumGetNumeric(d)) ||
						dscale == c->scale)
						cannot_hold(c, ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
									"NaN, an infinity, or a value beyond its precision");
					v = 0;
					if (!vexec_numeric_to_scaled(DirectFunctionCall2(numeric, d,
																	 Int32GetDatum(c->typmod)),
												 c->scale, c->digits, 16, &v))
						cannot_hold(c, ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
									"a value beyond its precision");
				}
				memcpy(c->values + (Size) row * 16, &v, 16);
				break;
			}
		case EGRESS_DATE:
			{
				DateADT		v = DatumGetDateADT(d);
				int32		a;

				if (DATE_NOT_FINITE(v))
					cannot_hold(c, ERRCODE_DATETIME_VALUE_OUT_OF_RANGE, "an infinite date");
				a = v + (int32) VEXEC_EPOCH_DAYS;
				memcpy(c->values + (Size) row * 4, &a, 4);
				break;
			}
		case EGRESS_TIME:
			{
				TimeADT		v = DatumGetTimeADT(d);

				if (v >= USECS_PER_DAY)
					cannot_hold(c, ERRCODE_DATETIME_VALUE_OUT_OF_RANGE, "24:00:00");
				memcpy(c->values + (Size) row * 8, &v, 8);
				break;
			}
		case EGRESS_TIMESTAMP:
			{
				Timestamp	v = DatumGetTimestamp(d);
				int64		a;

				if (TIMESTAMP_NOT_FINITE(v))
					cannot_hold(c, ERRCODE_DATETIME_VALUE_OUT_OF_RANGE, "an infinite timestamp");
				if (pg_add_s64_overflow(v, VEXEC_EPOCH_USECS, &a))
					cannot_hold(c, ERRCODE_DATETIME_VALUE_OUT_OF_RANGE,
								"a timestamp past Arrow's last microsecond, 294247-01-10 04:00:54.775807");
				memcpy(c->values + (Size) row * 8, &a, 8);
				break;
			}
		case EGRESS_INTERVAL:
			{
				Interval   *iv = DatumGetIntervalP(d);
				int32		mdn[2];
				int64		ns;

				if (INTERVAL_NOT_FINITE(iv))
					cannot_hold(c, ERRCODE_DATETIME_VALUE_OUT_OF_RANGE, "an infinite interval");
				if (pg_mul_s64_overflow(iv->time, 1000, &ns))
					cannot_hold(c, ERRCODE_DATETIME_VALUE_OUT_OF_RANGE,
								"an interval whose time part leaves int64 in nanoseconds");
				mdn[0] = iv->month;
				mdn[1] = iv->day;
				memcpy(c->values + (Size) row * 16, mdn, 8);
				memcpy(c->values + (Size) row * 16 + 8, &ns, 8);
				break;
			}
		case EGRESS_UUID:
			memcpy(c->values + (Size) row * UUID_LEN, DatumGetUUIDP(d)->data, UUID_LEN);
			break;
		case EGRESS_TEXT:
		case EGRESS_BYTEA:
			{
				varlena    *vl = pg_detoast_datum_packed((varlena *) DatumGetPointer(d));

				append_varlen(r, c, row, VARDATA_ANY(vl), VARSIZE_ANY_EXHDR(vl));
				if ((Pointer) vl != DatumGetPointer(d))
					pfree(vl);
				break;
			}
		case EGRESS_NAME:
			{
				const char *s = NameStr(*DatumGetName(d));

				append_varlen(r, c, row, s, strlen(s));
				break;
			}
		case EGRESS_OUTPUT:
			{
				char	   *s = OutputFunctionCall(&c->output, d);

				append_varlen(r, c, row, s, strlen(s));
				pfree(s);
				break;
			}
	}
}

/* ---------------------------------------------------------------------
 * Sending a batch
 * ---------------------------------------------------------------------
 */

/* What the next message sends of a column: what was built for it. */
static void
column_out_built(EgressColumn *c)
{
	c->out_validity = c->nulls > 0 ? c->validity : NULL;
	c->out_nulls = c->nulls;
	if (c->offsets)
	{
		c->out_values = c->offsets;
		c->out_data = c->data.data ? c->data.data : "";
	}
	else
	{
		c->out_values = c->values;
		c->out_data = NULL;
	}
}

/* The batch's arrays, from each column's out_* buffers, into a message. */
static void
send_columns(EgressReceiver *r, int nrows)
{
	MemoryContext old = MemoryContextSwitchTo(r->batchcxt);
	struct ArrowArray root;
	struct ArrowArray **children;
	const void *rootbuf[1] = {NULL};
	const VexecIpcMessage *msg;
	int			i;

	memset(&root, 0, sizeof(root));
	children = palloc0(sizeof(struct ArrowArray *) * Max(r->ncols, 1));
	root.length = nrows;
	root.n_buffers = 1;
	root.buffers = rootbuf;
	root.n_children = r->ncols;
	root.children = children;
	root.release = release_array;
	for (i = 0; i < r->ncols; i++)
	{
		EgressColumn *c = &r->cols[i];
		struct ArrowArray *a = palloc0(sizeof(struct ArrowArray));
		const void **bufs = palloc0(sizeof(void *) * 3);

		a->length = nrows;
		a->null_count = c->out_nulls;
		bufs[0] = c->out_validity;
		bufs[1] = c->out_values;
		bufs[2] = c->out_data;
		a->n_buffers = c->out_data ? 3 : 2;
		a->buffers = bufs;
		a->release = release_array;
		children[i] = a;
	}
	msg = vexec_ipc_write_batch(r->writer, &r->schema, &root);
	MemoryContextSwitchTo(old);

	r->rows_sent += nrows;
	r->batches_sent++;
	r->bytes_sent += msg->metadata_len + msg->body_len;
	vexec_egress_send(r->write, r->arg, msg);

	MemoryContextReset(r->batchcxt);
	batch_begin(r);
}

/* The rows built so far, as a message: none when there are none. */
static void
flush_rows(EgressReceiver *r)
{
	int			i;

	if (r->nrows == 0)
		return;
	for (i = 0; i < r->ncols; i++)
		column_out_built(&r->cols[i]);
	send_columns(r, r->nrows);
}

/* One row, from values as a slot holds them. */
static void
append_row(EgressReceiver *r, const Datum *values, const bool *isnull)
{
	int			i;

	for (i = 0; i < r->ncols; i++)
		column_append(r, &r->cols[i], r->nrows, values[i], isnull[i]);
	r->nrows++;
	if (r->nrows >= VEXEC_BATCH_ROWS || r->bytes >= EGRESS_BATCH_BYTES)
		flush_rows(r);
}

/* ---------------------------------------------------------------------
 * The DestReceiver
 * ---------------------------------------------------------------------
 */

static void
egress_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	EgressReceiver *r = (EgressReceiver *) self;
	const VexecIpcMessage *msg;

	(void) operation;

	/*
	 * A portal fetched more than once starts its receiver once a fetch: the
	 * stream goes on.
	 */
	if (r->started)
		return;
	vexec_egress_check_active();
	columns_init(r, typeinfo);
	r->started = true;
	msg = vexec_ipc_write_schema(r->writer, &r->schema);
	r->bytes_sent += msg->metadata_len;
	vexec_egress_send(r->write, r->arg, msg);
	batch_begin(r);
}

static bool
egress_receive_slot(TupleTableSlot *slot, DestReceiver *self)
{
	EgressReceiver *r = (EgressReceiver *) self;

	slot_getallattrs(slot);
	append_row(r, slot->tts_values, slot->tts_isnull);
	return true;
}

static void
egress_shutdown(DestReceiver *self)
{
	EgressReceiver *r = (EgressReceiver *) self;

	if (r->started)
		flush_rows(r);
}

static void
egress_destroy(DestReceiver *self)
{
	EgressReceiver *r = (EgressReceiver *) self;

	MemoryContextDelete(r->mcxt);	/* r with it */
}

DestReceiver *
vexec_egress_receiver(VexecEgressWriteFn write, void *arg,
					  const VexecEgressField *fields, int nfields)
{
	MemoryContext mcxt;
	EgressReceiver *r;
	int			i;

	vexec_egress_check_active();
	mcxt = AllocSetContextCreate(CurrentMemoryContext, "vexec egress", ALLOCSET_DEFAULT_SIZES);
	r = MemoryContextAllocZero(mcxt, sizeof(EgressReceiver));
	r->pub.receiveSlot = egress_receive_slot;
	r->pub.rStartup = egress_startup;
	r->pub.rShutdown = egress_shutdown;
	r->pub.rDestroy = egress_destroy;
	r->pub.mydest = DestNone;
	r->write = write;
	r->arg = arg;
	r->mcxt = mcxt;
	r->batchcxt = AllocSetContextCreate(mcxt, "vexec egress batch", ALLOCSET_DEFAULT_SIZES);
	if (fields != NULL)
	{
		/* the caller's fields, kept: the strings stay the caller's */
		r->fields = MemoryContextAlloc(mcxt, sizeof(VexecEgressField) * Max(nfields, 1));
		for (i = 0; i < nfields; i++)
			r->fields[i] = fields[i];
		r->nfields = nfields;
	}
	return &r->pub;
}

bool
vexec_egress_is_receiver(DestReceiver *dest)
{
	return dest != NULL && dest->receiveSlot == egress_receive_slot;
}

void
vexec_egress_receiver_counts(DestReceiver *dest, int64 *rows, int64 *batches, int64 *bytes)
{
	EgressReceiver *r = (EgressReceiver *) dest;

	if (!vexec_egress_is_receiver(dest))
		elog(ERROR, "not vexec's egress receiver");
	if (rows)
		*rows = r->rows_sent;
	if (batches)
		*batches = r->batches_sent;
	if (bytes)
		*bytes = r->bytes_sent;
}

/* A result's Schema message, as its receiver would send it. */
void
vexec_egress_schema(TupleDesc desc, const VexecEgressField *fields, int nfields,
					VexecEgressWriteFn write, void *arg)
{
	DestReceiver *dest = vexec_egress_receiver(write, arg, fields, nfields);
	EgressReceiver *r = (EgressReceiver *) dest;
	const VexecIpcMessage *msg;

	columns_init(r, desc);
	msg = vexec_ipc_write_schema(r->writer, &r->schema);
	vexec_egress_send(write, arg, msg);
	egress_destroy(dest);
}

/* ---------------------------------------------------------------------
 * A vector node's batch (run.c)
 * ---------------------------------------------------------------------
 */

/* Whether a column holds a value at either of two sentinels, among its valid rows. */
static bool
has_sentinel32(const VexecVec *v, int n, int32 a, int32 b)
{
	const int32 *x = v->values;
	int			i;

	for (i = 0; i < n; i++)
		if ((v->validity == NULL || vexec_bit(v->validity, i)) && (x[i] == a || x[i] == b))
			return true;
	return false;
}

static bool
has_sentinel64(const VexecVec *v, int n, int64 a, int64 b)
{
	const int64 *x = v->values;
	int			i;

	for (i = 0; i < n; i++)
		if ((v->validity == NULL || vexec_bit(v->validity, i)) && (x[i] == a || x[i] == b))
			return true;
	return false;
}

/*
 * Whether a flat column of a compacted batch goes out from its own
 * buffers: its layout is its Arrow type's, and no value is one the type
 * cannot hold.
 */
static bool
column_in_place(EgressReceiver *r, EgressColumn *c, VexecBatch *batch, VexecVec *v, int n)
{
	int			nulls;

	if (v->encoding != VEXEC_FLAT || v->nvalues < n)
		return false;
	switch (c->kind)
	{
		case EGRESS_BOOL:
			if (v->shape.layout != VEXEC_BIT_BOOL)
				return false;
			break;
		case EGRESS_INT:
			/* only in the type's own format: another checks ranges */
			if (!c->natural || v->shape.layout != VEXEC_FIXED ||
				v->shape.width != c->width || v->shape.stride != c->width)
				return false;
			break;
		case EGRESS_FLOAT4:
		case EGRESS_FLOAT8:
		case EGRESS_UUID:
			if (v->shape.layout != VEXEC_FIXED || v->shape.stride != c->width)
				return false;
			break;
		case EGRESS_DECIMAL:
			if (v->shape.layout != VEXEC_SCALED || v->shape.width != 16 ||
				v->shape.scale != c->scale)
				return false;
			break;
		case EGRESS_DATE:
			if (v->shape.layout != VEXEC_FIXED || !v->shape.arrow_values ||
				has_sentinel32(v, n, DATEVAL_NOBEGIN, DATEVAL_NOEND))
				return false;
			break;
		case EGRESS_TIMESTAMP:
			if (v->shape.layout != VEXEC_FIXED || !v->shape.arrow_values ||
				has_sentinel64(v, n, DT_NOBEGIN, DT_NOEND))
				return false;
			break;
		case EGRESS_TIME:
			if (v->shape.layout != VEXEC_FIXED || v->shape.stride != 8 ||
				has_sentinel64(v, n, USECS_PER_DAY, USECS_PER_DAY))
				return false;
			break;
		case EGRESS_INTERVAL:
			/* Arrow's layout is only kept where every value is finite (§3.4.2) */
			if (v->shape.layout != VEXEC_FIXED || !v->shape.arrow_values ||
				v->shape.stride != 16)
				return false;
			break;
		case EGRESS_TEXT:
		case EGRESS_BYTEA:
			if (v->shape.layout != VEXEC_OFFSETS || c->to_utf8 || c->check_utf8)
				return false;
			break;
		default:
			return false;
	}

	nulls = v->validity ? vexec_vec_null_count(batch, v) : 0;
	if (nulls > 0 && c->not_null)
		return false;			/* column_append raises it, naming the column */
	c->out_validity = nulls > 0 ? v->validity : NULL;
	c->out_nulls = nulls;
	c->out_data = NULL;
	if (c->kind == EGRESS_TEXT || c->kind == EGRESS_BYTEA)
	{
		const int32 *offsets = v->values;
		int32		base = offsets[0];

		if (base == 0)
			c->out_values = offsets;
		else
		{
			/* Arrow's readers take offsets that start anywhere; clients' may not */
			int32	   *rebased = MemoryContextAlloc(r->batchcxt, sizeof(int32) * (n + 1));
			int			i;

			for (i = 0; i <= n; i++)
				rebased[i] = offsets[i] - base;
			c->out_values = rebased;
		}
		c->out_data = v->buffers[0] + base;
	}
	else
		c->out_values = v->values;
	return true;
}

/*
 * A vector node's output batch, its columns in colmap's order (the
 * result's columns, junk left out): the rows its quals passed, from its
 * columns, and the rows its kernels left to PostgreSQL's evaluator, through
 * the node, in row order.  The number of rows sent.
 */
int64
vexec_egress_send_batch(DestReceiver *dest, VexecNode *node, VexecBatch *batch, const int *colmap)
{
	EgressReceiver *r = (EgressReceiver *) dest;
	int			n = batch->nrows;
	bool		redo;
	int64		sent = 0;
	int			i;

	Assert(vexec_egress_is_receiver(dest) && r->started);
	redo = (node->redo != NULL && vexec_bits_any(node->redo, n)) ||
		(node->child_redo != NULL && vexec_bits_any(node->child_redo, n));

	if (redo)
	{
		Datum	   *values = palloc(sizeof(Datum) * Max(r->ncols, 1));
		bool	   *isnull = palloc(sizeof(bool) * Max(r->ncols, 1));
		int			row;

		for (row = 0; row < n; row++)
		{
			if (batch->selection == NULL || vexec_bit(batch->selection, row))
			{
				for (i = 0; i < r->ncols; i++)
					values[i] = vexec_vec_datum(batch, &batch->cols[colmap[i]], row, &isnull[i]);
			}
			else if ((node->redo != NULL && vexec_bit(node->redo, row)) ||
					 (node->child_redo != NULL && vexec_bit(node->child_redo, row)))
			{
				TupleTableSlot *slot = vexec_resolve_row(node, row);

				if (slot == NULL)
					continue;	/* its quals rejected it */
				slot_getallattrs(slot);
				for (i = 0; i < r->ncols; i++)
				{
					values[i] = slot->tts_values[colmap[i]];
					isnull[i] = slot->tts_isnull[colmap[i]];
				}
			}
			else
				continue;
			append_row(r, values, isnull);
			sent++;
		}
		pfree(values);
		pfree(isnull);
		return sent;
	}

	/* the rows built so far go first, in order */
	flush_rows(r);
	vexec_batch_compact(batch);
	n = batch->nrows;
	if (n == 0)
		return 0;
	for (i = 0; i < r->ncols; i++)
	{
		EgressColumn *c = &r->cols[i];
		VexecVec   *v = &batch->cols[colmap[i]];

		if (!column_in_place(r, c, batch, v, n))
		{
			int			row;

			/* a value at a time, into the column's buffers */
			for (row = 0; row < n; row++)
			{
				bool		isnull;
				Datum		d = vexec_vec_datum(batch, v, row, &isnull);

				column_append(r, c, row, d, isnull);
			}
			column_out_built(c);
		}
	}
	send_columns(r, n);
	r->vector_batches++;
	return n;
}

/* The batches a vector node's batches went out as, for vexec_test. */
int64
vexec_egress_vector_batches(DestReceiver *dest)
{
	if (!vexec_egress_is_receiver(dest))
		elog(ERROR, "not vexec's egress receiver");
	return ((EgressReceiver *) dest)->vector_batches;
}
