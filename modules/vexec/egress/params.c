/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * params.c
 *	  A client's parameter batches, to values of a statement's parameters
 *	  (pg_vector_executor.md §3.15, V10; vexec_egress.h).
 *
 * A Flight SQL client binds a prepared statement's parameters by DoPut: a
 * stream of Arrow, a column a parameter and a row a set of values.  The
 * stream comes from a client, so vexec's IPC reader checks it as input
 * before a value is read (ipc/read.c).  Each column is then read by one of
 * two rules:
 *
 *	utf8		the parameter type's input function reads the text, as it
 *				reads a parameter sent in text format over PostgreSQL's
 *				protocol;
 *	the rest	the value becomes the PostgreSQL type its Arrow type names --
 *				int2, int4 or int8 for the integers, float4 or float8,
 *				numeric for the decimals, bytea, uuid, date, time,
 *				timestamp or timestamptz, interval -- and then the
 *				parameter's type by assignment, the coercion INSERT applies
 *				to a column (PG19:src/include/parser/parse_coerce.h:40-45),
 *				compiled once a column as PL/pgSQL compiles its casts, over
 *				a CaseTestExpr (PG19:src/pl/plpgsql/src/pl_exec.c:8085-8160).
 *
 * A value outside the PostgreSQL type's range fails, as its input function
 * fails: an Arrow date past PostgreSQL's dates does not become infinity
 * (§3.16).  Arrow's units finer than a microsecond are rounded down.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "common/int.h"
#include "datatype/timestamp.h"
#include "executor/executor.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_coerce.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"
#include "varatt.h"

#include "vexec.h"
#include "batch/arrow_abi.h"
#include "batch/batch.h"
#include "egress/egress.h"
#include "ipc/ipc.h"

/* What an Arrow column of parameters holds, as far as reading it goes. */
typedef enum ArrowParamKind
{
	PK_NULL,
	PK_BOOL,
	PK_INT,						/* signed, width 1 to 8 */
	PK_UINT,					/* unsigned, width 1 to 8 */
	PK_FLOAT16,
	PK_FLOAT4,
	PK_FLOAT8,
	PK_DECIMAL,
	PK_UTF8,					/* offsets */
	PK_LARGE_UTF8,
	PK_UTF8_VIEW,
	PK_BINARY,
	PK_LARGE_BINARY,
	PK_BINARY_VIEW,
	PK_FIXED_BINARY,
	PK_UUID,
	PK_DATE32,
	PK_DATE64,
	PK_TIME,					/* unit_us: µs per unit, or -1000 for ns */
	PK_TIMESTAMP,
	PK_DURATION,
	PK_INTERVAL_YM,
	PK_INTERVAL_DT,
	PK_INTERVAL_MDN
} ArrowParamKind;

typedef struct ParamCol
{
	int			kind;			/* ArrowParamKind */
	int			width;			/* bytes a value: integers, decimals,
								 * fixed-size binary, times */
	int			precision;		/* decimals */
	int			scale;
	int64		unit_us;		/* temporal: µs a unit, or 0 for ns */
	Oid			natural;		/* the PostgreSQL type the Arrow type names */
	Oid			type;			/* the parameter's */
	int32		typmod;
	FmgrInfo	input;			/* utf8: the parameter type's input */
	Oid			ioparam;
	ExprState  *cast;			/* natural to type, or NULL when they are one */
	bool		stream;			/* a column of an ingest stream, not a
								 * parameter: errors name it so */
} ParamCol;

typedef struct ParamsState
{
	MemoryContext mcxt;
	MemoryContext rowcxt;		/* a row's values, reset after each */
	struct ArrowSchema schema;	/* the client's */
	int			nparams;
	ParamCol   *cols;
	ExprContext *econtext;		/* the casts' */
	Datum	   *values;
	bool	   *isnull;
} ParamsState;

/* The C Data Interface's metadata value of a key, or NULL. */
static char *
metadata_value(const char *metadata, const char *key)
{
	int32		n;
	const char *p = metadata;
	int			i;

	if (metadata == NULL)
		return NULL;
	memcpy(&n, p, sizeof(int32));
	p += sizeof(int32);
	for (i = 0; i < n; i++)
	{
		int32		kl;
		int32		vl;
		const char *k;

		memcpy(&kl, p, sizeof(int32));
		k = p + sizeof(int32);
		p = k + kl;
		memcpy(&vl, p, sizeof(int32));
		p += sizeof(int32);
		if (kl == (int32) strlen(key) && memcmp(k, key, kl) == 0)
			return pnstrdup(p, vl);
		p += vl;
	}
	return NULL;
}

/* The time unit of a temporal format's third letter: µs a unit, 0 for ns. */
static int64
time_unit(char c)
{
	switch (c)
	{
		case 's':
			return USECS_PER_SEC;
		case 'm':
			return 1000;
		case 'u':
			return 1;
		case 'n':
			return 0;
	}
	return -1;
}

/* What a message calls the value's column: a parameter, or a stream's column. */
static char *
value_name(const ParamCol *col, int param)
{
	return col->stream ? psprintf("column %d of the client's stream", param) :
		psprintf("parameter $%d", param);
}

pg_noreturn static void
not_a_parameter(const ParamCol *col, int param, const char *format)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("%s cannot be read from Arrow's type \"%s\"", value_name(col, param), format)));
}

/* How a client's column reads, and which PostgreSQL type it names. */
static void
param_kind(ParamCol *col, int param, const struct ArrowSchema *field)
{
	const char *f = field->format;

	col->width = 0;
	col->unit_us = 1;
	switch (f[0])
	{
		case 'n':
			col->kind = PK_NULL;
			col->natural = InvalidOid;
			return;
		case 'b':
			col->kind = PK_BOOL;
			col->natural = BOOLOID;
			return;
		case 'c':
		case 's':
		case 'i':
		case 'l':
			col->kind = PK_INT;
			col->width = f[0] == 'c' ? 1 : f[0] == 's' ? 2 : f[0] == 'i' ? 4 : 8;
			col->natural = col->width <= 2 ? INT2OID : col->width == 4 ? INT4OID : INT8OID;
			if (col->width == 1 && col->type == CHAROID)
				col->natural = CHAROID; /* the byte, as the egress sends "char" */
			return;
		case 'C':
		case 'S':
		case 'I':
		case 'L':
			col->kind = PK_UINT;
			col->width = f[0] == 'C' ? 1 : f[0] == 'S' ? 2 : f[0] == 'I' ? 4 : 8;
			col->natural = col->width == 1 ? INT2OID : col->width == 2 ? INT4OID : INT8OID;
			return;
		case 'e':
			col->kind = PK_FLOAT16;
			col->natural = FLOAT4OID;
			return;
		case 'f':
			col->kind = PK_FLOAT4;
			col->natural = FLOAT4OID;
			return;
		case 'g':
			col->kind = PK_FLOAT8;
			col->natural = FLOAT8OID;
			return;
		case 'd':
			{
				int			bw = 128;

				if (sscanf(f, "d:%d,%d,%d", &col->precision, &col->scale, &bw) < 2 ||
					(bw != 32 && bw != 64 && bw != 128 && bw != 256) ||
					col->precision < 1 || col->precision > 76 ||
					col->scale < -76 || col->scale > 76)
					not_a_parameter(col, param, f);
				col->kind = PK_DECIMAL;
				col->width = bw / 8;
				col->natural = NUMERICOID;
				return;
			}
		case 'u':
		case 'U':
			col->kind = f[0] == 'u' ? PK_UTF8 : PK_LARGE_UTF8;
			col->natural = TEXTOID;
			return;
		case 'z':
		case 'Z':
			col->kind = f[0] == 'z' ? PK_BINARY : PK_LARGE_BINARY;
			col->natural = BYTEAOID;
			return;
		case 'v':
			if (strcmp(f, "vu") == 0)
			{
				col->kind = PK_UTF8_VIEW;
				col->natural = TEXTOID;
				return;
			}
			if (strcmp(f, "vz") == 0)
			{
				col->kind = PK_BINARY_VIEW;
				col->natural = BYTEAOID;
				return;
			}
			break;
		case 'w':
			{
				char	   *ext = metadata_value(field->metadata, "ARROW:extension:name");

				col->width = atoi(f + 2);
				if (col->width <= 0)
					not_a_parameter(col, param, f);
				if (col->width == UUID_LEN && ext && strcmp(ext, "arrow.uuid") == 0)
				{
					col->kind = PK_UUID;
					col->natural = UUIDOID;
				}
				else
				{
					col->kind = PK_FIXED_BINARY;
					col->natural = BYTEAOID;
				}
				return;
			}
		case 't':
			switch (f[1])
			{
				case 'd':
					col->kind = f[2] == 'D' ? PK_DATE32 : PK_DATE64;
					col->natural = DATEOID;
					return;
				case 't':
					col->kind = PK_TIME;
					col->unit_us = time_unit(f[2]);
					col->width = f[2] == 's' || f[2] == 'm' ? 4 : 8;
					col->natural = TIMEOID;
					if (col->unit_us < 0)
						break;
					return;
				case 's':
					col->kind = PK_TIMESTAMP;
					col->unit_us = time_unit(f[2]);
					/* a time zone makes it an instant: timestamptz */
					col->natural = f[3] == ':' && f[4] != '\0' ? TIMESTAMPTZOID : TIMESTAMPOID;
					if (col->unit_us < 0)
						break;
					return;
				case 'D':
					col->kind = PK_DURATION;
					col->unit_us = time_unit(f[2]);
					col->natural = INTERVALOID;
					if (col->unit_us < 0)
						break;
					return;
				case 'i':
					col->kind = f[2] == 'M' ? PK_INTERVAL_YM :
						f[2] == 'D' ? PK_INTERVAL_DT : PK_INTERVAL_MDN;
					col->natural = INTERVALOID;
					if (f[2] != 'M' && f[2] != 'D' && f[2] != 'n')
						break;
					return;
			}
			break;
	}
	not_a_parameter(col, param, f);
}

/* The assignment of a natural type to the parameter's, once (pl_exec.c's way). */
static void
param_cast(ParamCol *col, int param)
{
	CaseTestExpr *placeholder;
	Node	   *cast;

	/*
	 * The same type needs no cast, but for its typmod: a stream's column
	 * definition list may bound a numeric or a string, which a function's
	 * result is not checked against (§3.16).
	 */
	if (col->kind == PK_NULL || (col->natural == col->type && col->typmod < 0))
		return;
	placeholder = makeNode(CaseTestExpr);
	placeholder->typeId = col->natural;
	placeholder->typeMod = -1;
	placeholder->collation = get_typcollation(col->natural);
	cast = coerce_to_target_type(NULL, (Node *) placeholder, col->natural,
								 col->type, col->typmod,
								 COERCION_ASSIGNMENT, COERCE_IMPLICIT_CAST, -1);
	if (cast == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("%s is of type %s, and its Arrow values are of type %s",
						value_name(col, param), format_type_be(col->type),
						format_type_be(col->natural)),
				 errhint("Send it as a type that can be assigned to %s, or as utf8.",
						 format_type_be(col->type))));
	col->cast = ExecInitExpr(expression_planner((Expr *) cast), NULL);
}

void *
vexec_egress_params_begin(const char *metadata, size_t len, int nparams,
						  const Oid *types, const int32 *typmods)
{
	MemoryContext mcxt;
	MemoryContext old;
	ParamsState *st;
	int			i;

	vexec_egress_check_active();
	mcxt = AllocSetContextCreate(CurrentMemoryContext, "vexec egress parameters",
								 ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(mcxt);
	st = palloc0(sizeof(ParamsState));
	st->mcxt = mcxt;
	st->rowcxt = AllocSetContextCreate(mcxt, "vexec egress parameter row",
									   ALLOCSET_DEFAULT_SIZES);
	vexec_ipc_read_schema(metadata, len, &st->schema);
	if (st->schema.n_children != nparams)
		ereport(ERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("the parameters' batches supply %d columns, but the statement requires %d",
						(int) st->schema.n_children, nparams)));
	st->nparams = nparams;
	st->cols = palloc0(sizeof(ParamCol) * Max(nparams, 1));
	st->values = palloc(sizeof(Datum) * Max(nparams, 1));
	st->isnull = palloc(sizeof(bool) * Max(nparams, 1));
	st->econtext = CreateStandaloneExprContext();
	for (i = 0; i < nparams; i++)
	{
		ParamCol   *col = &st->cols[i];

		col->type = types[i];
		col->typmod = typmods ? typmods[i] : -1;
		param_kind(col, i + 1, st->schema.children[i]);
		if (col->natural == TEXTOID)
		{
			Oid			typinput;

			getTypeInputInfo(col->type, &typinput, &col->ioparam);
			fmgr_info(typinput, &col->input);
		}
		else
			param_cast(col, i + 1);
	}
	MemoryContextSwitchTo(old);
	return st;
}

/* ---------------------------------------------------------------------
 * Reading a value
 * ---------------------------------------------------------------------
 */

pg_noreturn static void
param_out_of_range(const ParamCol *col, int param, const char *what)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
			 errmsg("%s holds %s out of PostgreSQL's range", value_name(col, param), what)));
}

/* float16's bits as a float4 (IEEE 754 binary16). */
static float4
half_to_float(uint16 h)
{
	uint32		sign = (uint32) (h & 0x8000) << 16;
	uint32		exp = (h >> 10) & 0x1f;
	uint32		mant = h & 0x3ff;
	uint32		bits;
	float4		f;

	if (exp == 0)
	{
		/* zero or subnormal: mant * 2^-24 */
		f = (float4) mant / 16777216.0f;
		return sign ? -f : f;
	}
	if (exp == 31)
		bits = sign | 0x7f800000 | (mant << 13);
	else
		bits = sign | ((exp + 112) << 23) | (mant << 13);
	memcpy(&f, &bits, sizeof(f));
	return f;
}

/* A value's bytes, of utf8 or binary in any of their layouts. */
static void
varlen_bytes(const struct ArrowArray *a, int kind, int64 row, const char **p, int64 *len)
{
	int64		i = a->offset + row;

	switch (kind)
	{
		case PK_UTF8:
		case PK_BINARY:
			{
				const int32 *off = a->buffers[1];

				*p = (const char *) a->buffers[2] + off[i];
				*len = off[i + 1] - off[i];
				return;
			}
		case PK_LARGE_UTF8:
		case PK_LARGE_BINARY:
			{
				const int64 *off = a->buffers[1];

				*p = (const char *) a->buffers[2] + off[i];
				*len = off[i + 1] - off[i];
				return;
			}
		default:				/* views (Columnar.rst:492-524) */
			{
				const char *view = (const char *) a->buffers[1] + i * 16;
				int32		size;
				int32		buf;
				int32		offset;

				memcpy(&size, view, 4);
				*len = size;
				if (size <= 12)
				{
					*p = view + 4;
					return;
				}
				memcpy(&buf, view + 8, 4);
				memcpy(&offset, view + 12, 4);
				*p = (const char *) a->buffers[2 + buf] + offset;
				return;
			}
	}
}

/* A decimal's integer, of 32 to 256 bits, as text with its point placed. */
static char *
decimal_text(const char *p, int width, int scale)
{
	uint32		limbs[8];
	int			nlimbs = width / 4;
	bool		neg;
	char		digits[160];	/* 78 digits, and zeros up to a scale of 76 */
	int			nd = 0;
	StringInfoData out;
	int			i;

	memcpy(limbs, p, width);
	neg = (limbs[nlimbs - 1] & 0x80000000) != 0;
	if (neg)
	{
		/* two's complement: invert and add one */
		uint64		carry = 1;

		for (i = 0; i < nlimbs; i++)
		{
			uint64		v = (uint64) (~limbs[i]) + carry;

			limbs[i] = (uint32) v;
			carry = v >> 32;
		}
	}
	/* repeated division by 10 of the magnitude, most significant limb first */
	for (;;)
	{
		uint64		rem = 0;
		bool		zero = true;

		for (i = nlimbs - 1; i >= 0; i--)
		{
			uint64		cur = (rem << 32) | limbs[i];

			limbs[i] = (uint32) (cur / 10);
			rem = cur % 10;
			if (limbs[i] != 0)
				zero = false;
		}
		digits[nd++] = (char) ('0' + rem);
		if (zero)
			break;
	}
	initStringInfo(&out);
	if (neg)
		appendStringInfoChar(&out, '-');
	if (scale > 0)
	{
		/* at least one digit before the point */
		while (nd <= scale)
			digits[nd++] = '0';
		for (i = nd - 1; i >= scale; i--)
			appendStringInfoChar(&out, digits[i]);
		appendStringInfoChar(&out, '.');
		for (; i >= 0; i--)
			appendStringInfoChar(&out, digits[i]);
	}
	else
	{
		for (i = nd - 1; i >= 0; i--)
			appendStringInfoChar(&out, digits[i]);
		if (scale < 0)
			appendStringInfo(&out, "e%d", -scale);
	}
	return out.data;
}

/* µs from a count of a unit; rounded down from nanoseconds. */
static bool
to_usecs(int64 v, int64 unit_us, int64 *us)
{
	if (unit_us == 0)
	{
		*us = v / 1000 - ((v % 1000) < 0 ? 1 : 0);
		return true;
	}
	return !pg_mul_s64_overflow(v, unit_us, us);
}

/* Row `row` of a client's column as a Datum of its natural type. */
static Datum
param_value(ParamCol *col, int param, const struct ArrowArray *a, int64 row, bool *isnull)
{
	int64		i = a->offset + row;
	const char *vals = a->n_buffers > 1 ? a->buffers[1] : NULL;

	*isnull = false;
	if (col->kind == PK_NULL ||
		(a->buffers[0] != NULL &&
		 !((((const uint8 *) a->buffers[0])[i >> 3] >> (i & 7)) & 1)))
	{
		*isnull = true;
		/* a domain's input sees a NULL too, as a NULL text parameter's does */
		if (col->natural == TEXTOID)
			return InputFunctionCall(&col->input, NULL, col->ioparam, col->typmod);
		return (Datum) 0;
	}

	switch (col->kind)
	{
		case PK_BOOL:
			return BoolGetDatum((((const uint8 *) vals)[i >> 3] >> (i & 7)) & 1);
		case PK_INT:
		case PK_UINT:
			{
				int64		v = 0;

				if (col->kind == PK_INT)
				{
					switch (col->width)
					{
						case 1:
							v = ((const int8 *) vals)[i];
							break;
						case 2:
							v = ((const int16 *) vals)[i];
							break;
						case 4:
							v = ((const int32 *) vals)[i];
							break;
						default:
							v = ((const int64 *) vals)[i];
							break;
					}
				}
				else
				{
					switch (col->width)
					{
						case 1:
							v = ((const uint8 *) vals)[i];
							break;
						case 2:
							v = ((const uint16 *) vals)[i];
							break;
						case 4:
							v = ((const uint32 *) vals)[i];
							break;
						default:
							{
								uint64		u = ((const uint64 *) vals)[i];

								if (u > (uint64) PG_INT64_MAX)
									ereport(ERROR,
											(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
											 errmsg("%s holds a value out of bigint's range",
													value_name(col, param))));
								v = (int64) u;
							}
							break;
					}
				}
				if (col->natural == CHAROID)
					return CharGetDatum((char) v);
				if (col->natural == INT2OID)
					return Int16GetDatum((int16) v);
				if (col->natural == INT4OID)
					return Int32GetDatum((int32) v);
				return Int64GetDatum(v);
			}
		case PK_FLOAT16:
			return Float4GetDatum(half_to_float(((const uint16 *) vals)[i]));
		case PK_FLOAT4:
			return Float4GetDatum(((const float4 *) vals)[i]);
		case PK_FLOAT8:
			return Float8GetDatum(((const float8 *) vals)[i]);
		case PK_DECIMAL:
			return DirectFunctionCall3(numeric_in,
									   CStringGetDatum(decimal_text(vals + i * col->width,
																	col->width, col->scale)),
									   ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
		case PK_UTF8:
		case PK_LARGE_UTF8:
		case PK_UTF8_VIEW:
			{
				const char *p;
				int64		len;
				char	   *s;

				varlen_bytes(a, col->kind, row, &p, &len);
				if (memchr(p, '\0', len) != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNTRANSLATABLE_CHARACTER),
							 errmsg("%s holds a NUL character, which text cannot", value_name(col, param))));
				s = pg_any_to_server(p, (int) len, PG_UTF8);
				if (s == p)
					s = pnstrdup(p, len);
				return InputFunctionCall(&col->input, s, col->ioparam, col->typmod);
			}
		case PK_BINARY:
		case PK_LARGE_BINARY:
		case PK_BINARY_VIEW:
		case PK_FIXED_BINARY:
			{
				const char *p;
				int64		len;
				bytea	   *b;

				if (col->kind == PK_FIXED_BINARY)
				{
					p = vals + i * col->width;
					len = col->width;
				}
				else
					varlen_bytes(a, col->kind, row, &p, &len);
				if (len > MaxAllocSize - VARHDRSZ)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("%s holds a value too long for bytea", value_name(col, param))));
				b = palloc(len + VARHDRSZ);
				SET_VARSIZE(b, len + VARHDRSZ);
				memcpy(VARDATA(b), p, len);
				return PointerGetDatum(b);
			}
		case PK_UUID:
			{
				pg_uuid_t  *u = palloc(sizeof(pg_uuid_t));

				memcpy(u->data, vals + i * UUID_LEN, UUID_LEN);
				return UUIDPGetDatum(u);
			}
		case PK_DATE32:
		case PK_DATE64:
			{
				int64		days;
				int64		d;

				if (col->kind == PK_DATE32)
					days = ((const int32 *) vals)[i];
				else
				{
					int64		ms = ((const int64 *) vals)[i];

					days = ms / 86400000 - ((ms % 86400000) < 0 ? 1 : 0);
				}
				d = days - VEXEC_EPOCH_DAYS;
				if (!IS_VALID_DATE(d))
					param_out_of_range(col, param, "a date");
				return DateADTGetDatum((DateADT) d);
			}
		case PK_TIME:
			{
				int64		v = col->width == 4 ? ((const int32 *) vals)[i] : ((const int64 *) vals)[i];
				int64		us;

				if (!to_usecs(v, col->unit_us, &us) || us < 0 || us > USECS_PER_DAY)
					param_out_of_range(col, param, "a time");
				return TimeADTGetDatum(us);
			}
		case PK_TIMESTAMP:
			{
				int64		us;
				int64		ts;

				if (!to_usecs(((const int64 *) vals)[i], col->unit_us, &us) ||
					pg_sub_s64_overflow(us, VEXEC_EPOCH_USECS, &ts) ||
					!IS_VALID_TIMESTAMP(ts))
					param_out_of_range(col, param, "a timestamp");
				return TimestampGetDatum(ts);
			}
		case PK_DURATION:
		case PK_INTERVAL_YM:
		case PK_INTERVAL_DT:
		case PK_INTERVAL_MDN:
			{
				Interval   *iv = palloc0(sizeof(Interval));

				if (col->kind == PK_DURATION)
				{
					if (!to_usecs(((const int64 *) vals)[i], col->unit_us, &iv->time))
						param_out_of_range(col, param, "a duration");
				}
				else if (col->kind == PK_INTERVAL_YM)
					iv->month = ((const int32 *) vals)[i];
				else if (col->kind == PK_INTERVAL_DT)
				{
					int32		dt[2];

					memcpy(dt, vals + i * 8, 8);
					iv->day = dt[0];
					iv->time = (int64) dt[1] * 1000;
				}
				else
				{
					int32		md[2];
					int64		ns;

					memcpy(md, vals + i * 16, 8);
					memcpy(&ns, vals + i * 16 + 8, 8);
					iv->month = md[0];
					iv->day = md[1];
					(void) to_usecs(ns, 0, &iv->time);
				}
				/* ±infinity's fields are PostgreSQL's own: not a client's */
				if (INTERVAL_NOT_FINITE(iv))
					param_out_of_range(col, param, "an interval");
				return IntervalPGetDatum(iv);
			}
	}
	elog(ERROR, "unexpected parameter kind %d", col->kind);
	return (Datum) 0;			/* keep the compiler quiet */
}

int64
vexec_egress_params_batch(void *state, const char *metadata, size_t len,
						  const char *body, size_t body_len,
						  VexecEgressRowFn row, void *arg)
{
	ParamsState *st = state;
	MemoryContext batchcxt;
	MemoryContext old;
	struct ArrowArray array;
	int64		n;
	int64		r;
	int			i;

	vexec_egress_check_active();
	batchcxt = AllocSetContextCreate(st->mcxt, "vexec egress parameter batch",
									 ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(batchcxt);
	vexec_ipc_read_batch(&st->schema, metadata, len, body, body_len, &array);
	MemoryContextSwitchTo(old);

	n = array.length;
	for (r = 0; r < n; r++)
	{
		old = MemoryContextSwitchTo(st->rowcxt);
		for (i = 0; i < st->nparams; i++)
		{
			ParamCol   *col = &st->cols[i];
			Datum		v = param_value(col, i + 1, array.children[i], r, &st->isnull[i]);

			if (col->cast != NULL)
			{
				st->econtext->caseValue_datum = v;
				st->econtext->caseValue_isNull = st->isnull[i];
				v = ExecEvalExpr(col->cast, st->econtext, &st->isnull[i]);
			}
			st->values[i] = v;
		}
		MemoryContextSwitchTo(old);
		row(arg, r, st->values, st->isnull);
		MemoryContextReset(st->rowcxt);
		ResetExprContext(st->econtext);
	}
	MemoryContextDelete(batchcxt);
	return n;
}

void
vexec_egress_params_end(void *state)
{
	ParamsState *st = state;

	FreeExprContext(st->econtext, true);
	MemoryContextDelete(st->mcxt);
}

/* ---------------------------------------------------------------------
 * For ingest (ingest.c): a client's stream read as columns, by the same
 * rules as parameters
 * ---------------------------------------------------------------------
 */

/*
 * The PostgreSQL type an Arrow field names, as a parameter of no type of its
 * own would read it: a decimal's typmod its precision and scale, where
 * numeric has them; a column of Arrow's null type, text.
 */
void
vexec_egress_param_natural(const struct ArrowSchema *field, int column,
						   Oid *type, int32 *typmod)
{
	ParamCol	col;

	memset(&col, 0, sizeof(col));
	col.type = InvalidOid;
	col.stream = true;
	param_kind(&col, column, field);
	*type = col.kind == PK_NULL ? TEXTOID : col.natural;
	*typmod = -1;
	if (col.kind == PK_DECIMAL && col.precision >= 1 &&
		col.precision <= NUMERIC_MAX_PRECISION && col.scale >= 0 && col.scale <= col.precision)
		*typmod = ((col.precision << 16) | col.scale) + VARHDRSZ;
}

/*
 * A stream's columns read as the types given, as params_begin() reads
 * parameters: its errors name each value a column of the client's stream.
 */
void *
vexec_egress_stream_columns(const char *metadata, size_t len, int ncols,
							const Oid *types, const int32 *typmods)
{
	ParamsState *st = vexec_egress_params_begin(metadata, len, ncols, types, typmods);
	int			i;

	for (i = 0; i < ncols; i++)
		st->cols[i].stream = true;
	return st;
}

/* A column's value at a row of a batch's column, as params_batch() reads it. */
Datum
vexec_egress_stream_value(void *state, int column, const struct ArrowArray *a,
						  int64 row, bool *isnull)
{
	ParamsState *st = state;
	ParamCol   *col = &st->cols[column];
	Datum		v = param_value(col, column + 1, a, row, isnull);

	if (col->cast != NULL)
	{
		st->econtext->caseValue_datum = v;
		st->econtext->caseValue_isNull = *isnull;
		v = ExecEvalExpr(col->cast, st->econtext, isnull);
	}
	return v;
}

/* Reset what the values of a row used. */
void
vexec_egress_stream_row_done(void *state)
{
	ParamsState *st = state;

	ResetExprContext(st->econtext);
}
