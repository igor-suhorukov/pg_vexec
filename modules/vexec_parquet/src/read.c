/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * read.c
 *	  A file's column as a column of the table: which of the file's columns
 *	  it is, how its type becomes the column's, and its values as Datums.
 *
 * An own file's column is found by its field id, the attribute's number;
 * one the file lacks -- an attribute added after it was written -- reads as
 * the attribute's missing value.  An external file's is found by its name,
 * else by its name regardless of case where one alone matches, and must be
 * there.  Each pair of a column's kind and a file's type has a read mode;
 * a pair with none is refused.  An external file's values are checked as
 * PostgreSQL's input would check them: text's encoding, a varchar's or a
 * char's length, a numeric's precision, a NOT NULL column's NULLs; an own
 * file's were checked as they were written.  An external file's string of
 * any other type goes through the type's input function, its binary through
 * the type's receive function (PQ15, pg_vector_executor.md §3.39), as COPY
 * and COPY BINARY read them: PostGIS's geometry from WKB, its SRID the
 * file's CRS's where the column's typmod has none (geo.c).  Its list of flat
 * elements is read as an array, its map as hstore or jsonb (nested.c, PQ20).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type.h"
#include "common/int.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "varatt.h"

#include "vexec_parquet.h"
#include "convert.h"

static bool
is_int_type(int id)
{
	switch (id)
	{
		case VPQ_T_INT8:
		case VPQ_T_INT16:
		case VPQ_T_INT32:
		case VPQ_T_INT64:
		case VPQ_T_UINT8:
		case VPQ_T_UINT16:
		case VPQ_T_UINT32:
		case VPQ_T_UINT64:
			return true;
		default:
			return false;
	}
}

int
vpq_int_width(int id, bool *is_unsigned)
{
	*is_unsigned = id == VPQ_T_UINT8 || id == VPQ_T_UINT16 || id == VPQ_T_UINT32 || id == VPQ_T_UINT64;
	switch (id)
	{
		case VPQ_T_INT8:
		case VPQ_T_UINT8:
			return 1;
		case VPQ_T_INT16:
		case VPQ_T_UINT16:
			return 2;
		case VPQ_T_INT32:
		case VPQ_T_UINT32:
			return 4;
		case VPQ_T_INT64:
		case VPQ_T_UINT64:
			return 8;
		default:
			return 0;
	}
}

static bool
is_string_type(int id)
{
	return id == VPQ_T_STRING || id == VPQ_T_LARGE_STRING || id == VPQ_T_STRING_VIEW;
}

static bool
is_binary_type(int id)
{
	return id == VPQ_T_BINARY || id == VPQ_T_LARGE_BINARY || id == VPQ_T_BINARY_VIEW || id == VPQ_T_FIXED_BINARY;
}

static bool
is_bytes_type(int id)
{
	return id == VPQ_T_STRING || id == VPQ_T_BINARY || id == VPQ_T_LARGE_STRING ||
		id == VPQ_T_LARGE_BINARY || id == VPQ_T_STRING_VIEW || id == VPQ_T_BINARY_VIEW;
}

static VpqReadMode
bytes_mode(int id)
{
	switch (id)
	{
		case VPQ_T_LARGE_STRING:
		case VPQ_T_LARGE_BINARY:
			return VPQ_RM_LARGE_STRING;
		case VPQ_T_STRING_VIEW:
		case VPQ_T_BINARY_VIEW:
			return VPQ_RM_VIEW;
		default:
			return VPQ_RM_STRING;
	}
}

/* The read mode of a column's kind from a file's type; false for a pair with none */
static bool
mode_for(VpqColumn *col, const vpq_type *ft, bool own, VpqReadMode *mode)
{
	int			id = ft->id;

	if (id == VPQ_T_NULL)
	{
		*mode = VPQ_RM_NULL;
		return true;
	}
	switch (col->kind)
	{
		case VPQ_K_BOOL:
			if (id == VPQ_T_BOOL)
			{
				*mode = VPQ_RM_BOOL;
				return true;
			}
			break;
		case VPQ_K_CHAR:
			if (id == VPQ_T_INT8 || id == VPQ_T_UINT8)
			{
				*mode = VPQ_RM_FIXED;
				return true;
			}
			break;
		case VPQ_K_INT2:
		case VPQ_K_INT4:
		case VPQ_K_INT8:
			if ((col->kind == VPQ_K_INT2 && id == VPQ_T_INT16) ||
				(col->kind == VPQ_K_INT4 && id == VPQ_T_INT32) ||
				(col->kind == VPQ_K_INT8 && id == VPQ_T_INT64))
			{
				*mode = VPQ_RM_FIXED;
				return true;
			}
			if (is_int_type(id))
			{
				*mode = VPQ_RM_INT;
				return true;
			}
			break;
		case VPQ_K_OID:
			if (id == VPQ_T_UINT32)
			{
				*mode = VPQ_RM_FIXED;
				return true;
			}
			if (is_int_type(id))
			{
				*mode = VPQ_RM_INT;
				return true;
			}
			break;
		case VPQ_K_FLOAT4:
		case VPQ_K_FLOAT8:
			if ((col->kind == VPQ_K_FLOAT4 && id == VPQ_T_FLOAT32) ||
				(col->kind == VPQ_K_FLOAT8 && id == VPQ_T_FLOAT64))
			{
				*mode = VPQ_RM_FIXED;
				return true;
			}
			if (id == VPQ_T_FLOAT16 || (col->kind == VPQ_K_FLOAT8 && id == VPQ_T_FLOAT32))
			{
				*mode = VPQ_RM_FLOAT;
				return true;
			}
			break;
		case VPQ_K_DATE:
			if (id == VPQ_T_DATE32)
			{
				*mode = VPQ_RM_DATE32;
				return true;
			}
			if (id == VPQ_T_DATE64)
			{
				*mode = VPQ_RM_DATE64;
				return true;
			}
			/* PQ6 (pg_vector_executor.md §3.35): a file's integer of days since 1970, declared a date */
			if (!own && is_int_type(id))
			{
				*mode = VPQ_RM_INT_DAYS;
				return true;
			}
			break;
		case VPQ_K_TIME:
			if (id == VPQ_T_TIME64 && ft->unit == VPQ_UNIT_US)
			{
				*mode = VPQ_RM_FIXED;
				return true;
			}
			if (id == VPQ_T_TIME32 || id == VPQ_T_TIME64)
			{
				*mode = VPQ_RM_TIME;
				return true;
			}
			break;
		case VPQ_K_TIMESTAMP:
		case VPQ_K_TIMESTAMPTZ:
			if (id == VPQ_T_TIMESTAMP)
			{
				*mode = VPQ_RM_TIMESTAMP;
				return true;
			}
			/* PQ6: a file's integer of seconds since 1970, declared a timestamp -- ClickHouse's DateTime */
			if (!own && is_int_type(id))
			{
				*mode = VPQ_RM_INT_SECONDS;
				return true;
			}
			break;
		case VPQ_K_NUMERIC:
		case VPQ_K_NUMERIC_ANY:
			if (id == VPQ_T_DECIMAL32 || id == VPQ_T_DECIMAL64 || id == VPQ_T_DECIMAL128 ||
				id == VPQ_T_DECIMAL256)
			{
				*mode = VPQ_RM_DECIMAL;
				return true;
			}
			if (is_int_type(id))
			{
				*mode = VPQ_RM_INT_NUMERIC;
				return true;
			}
			if (own && id == VPQ_T_BINARY)
			{
				*mode = VPQ_RM_IMAGE_VARLENA;
				return true;
			}
			break;
		case VPQ_K_TEXT:
		case VPQ_K_BPCHAR:
		case VPQ_K_JSON:
			if (!own && col->kind == VPQ_K_JSON && is_string_type(id))
			{
				/* JSON from elsewhere is checked by json's input */
				*mode = VPQ_RM_TEXT_INPUT;
				return true;
			}
			if (is_bytes_type(id))
			{
				*mode = bytes_mode(id);
				return true;
			}
			if (id == VPQ_T_DICTIONARY && is_bytes_type(ft->value_id))
			{
				*mode = VPQ_RM_DICT;
				return true;
			}
			break;
		case VPQ_K_BYTEA:
			if (is_bytes_type(id))
			{
				*mode = bytes_mode(id);
				return true;
			}
			if (id == VPQ_T_DICTIONARY && is_bytes_type(ft->value_id))
			{
				*mode = VPQ_RM_DICT;
				return true;
			}
			if (id == VPQ_T_FIXED_BINARY)
			{
				*mode = VPQ_RM_FIXED_BINARY;
				return true;
			}
			break;
		case VPQ_K_UUID:
			if (id == VPQ_T_FIXED_BINARY && ft->byte_width == 16)
			{
				*mode = VPQ_RM_FIXED;
				return true;
			}
			break;
		case VPQ_K_FIXED:
			if (own && id == VPQ_T_FIXED_BINARY && ft->byte_width == col->typlen)
			{
				*mode = VPQ_RM_IMAGE_FIXED;
				return true;
			}
			break;
		case VPQ_K_VARLENA:
			if (own && id == VPQ_T_BINARY)
			{
				*mode = VPQ_RM_IMAGE_VARLENA;
				return true;
			}
			break;
		case VPQ_K_DROPPED:
			return false;
	}
	/* from elsewhere, a string through the type's input function */
	if (!own && is_string_type(id))
	{
		*mode = VPQ_RM_TEXT_INPUT;
		return true;
	}
	/* PQ15: from elsewhere, a binary through the type's receive function, as COPY BINARY reads a value */
	if (!own && is_binary_type(id) && col->recv)
	{
		*mode = VPQ_RM_RECV;
		return true;
	}
	/* PQ20 (§3.40): from elsewhere, a list as an array of its elements' type, a map as hstore or jsonb */
	if (!own && (id == VPQ_T_LIST || id == VPQ_T_LARGE_LIST) && OidIsValid(col->elemtype))
	{
		*mode = VPQ_RM_LIST;
		return true;
	}
	if (!own && id == VPQ_T_MAP && (col->hstore || col->basetype == JSONBOID))
	{
		*mode = col->hstore ? VPQ_RM_MAP_HSTORE : VPQ_RM_MAP_JSONB;
		return true;
	}
	return false;
}

static void
setup_input(VpqReadCol *rc)
{
	Oid			infunc;

	getTypeInputInfo(rc->col->typid, &infunc, &rc->ioparam);
	fmgr_info(infunc, &rc->input);
}

static void
setup_recv(VpqReadCol *rc)
{
	Oid			recvfunc;

	getTypeBinaryInputInfo(rc->col->typid, &recvfunc, &rc->ioparam);
	fmgr_info(recvfunc, &rc->input);
}

/*
 * PQ20: a nested column's child read as a column of its type -- a list's
 * elements, a map's keys or values -- by the mode of its Arrow type (nested.c);
 * false where it has none
 */
bool
vpq_read_child_setup(VpqReadCol *rc, VpqColumn *col, int fcol, const vpq_type *ft)
{
	memset(rc, 0, sizeof(*rc));
	rc->col = col;
	rc->fcol = fcol;
	rc->ftype = *ft;
	if (!mode_for(col, ft, false, &rc->mode))
		return false;
	if (rc->mode == VPQ_RM_TEXT_INPUT)
		setup_input(rc);
	else if (rc->mode == VPQ_RM_RECV)
		setup_recv(rc);
	return true;
}

/* An own column read from the buffer of its own kind (write.c's fetch) */
void
vpq_read_col_own(VpqColumn *col, VpqReadCol *rc)
{
	memset(rc, 0, sizeof(*rc));
	rc->col = col;
	rc->fcol = 0;
	rc->ftype = col->ftype;
	rc->own = true;
	if (!mode_for(col, &col->ftype, true, &rc->mode))
		elog(ERROR, "vexec_parquet: column \"%s\" has no read mode of its own type", col->name);
}

/* The file's column of a table's column, and its read mode */
void
vpq_read_col_setup(VpqTable *t, VpqColumn *col, vpq_file *file, VpqReadCol *rc, bool own)
{
	int			ncols = vpq_file_columns(file);
	int			found = -1;
	int			nocase = -1;
	int			nocase_count = 0;
	const char *name;
	int			field_id;
	bool		nullable;
	bool		flat;

	memset(rc, 0, sizeof(*rc));
	rc->col = col;
	rc->fcol = -1;
	rc->own = own;
	for (int i = 0; i < ncols; i++)
	{
		vpq_type	type;

		(void) vpq_file_column(file, i, &name, &field_id, &type, &nullable);
		if (own)
		{
			if (field_id == col->attnum)
			{
				found = i;
				break;
			}
		}
		else if (strcmp(name, col->name) == 0)
		{
			found = i;
			break;
		}
		else if (pg_strcasecmp(name, col->name) == 0)
		{
			nocase = i;
			nocase_count++;
		}
	}
	if (found < 0 && !own && nocase_count == 1)
		found = nocase;
	if (found < 0)
	{
		if (own)
		{
			rc->mode = VPQ_RM_MISSING;
			return;
		}
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of a vexec_parquet table is not in the file \"%s\"",
						col->name, vpq_file_location(file))));
	}
	rc->fcol = found;
	flat = vpq_file_column(file, found, &name, &field_id, &rc->ftype, &nullable);
	/* PQ20: a nested column of flat children is not flat, and read by its own modes */
	if (!flat && !own && (rc->ftype.id == VPQ_T_LIST || rc->ftype.id == VPQ_T_LARGE_LIST || rc->ftype.id == VPQ_T_MAP))
		flat = true;
	if (!flat || !mode_for(col, &rc->ftype, own, &rc->mode))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" of type %s of a vexec_parquet table cannot be read from the file's column \"%s\" of type %s",
						col->name, format_type_with_typemod(col->typid, col->typmod), name,
						rc->ftype.name),
				 errdetail("The file is \"%s\".", vpq_file_location(file))));
	if (rc->mode == VPQ_RM_TEXT_INPUT)
		setup_input(rc);
	else if (rc->mode == VPQ_RM_RECV)
	{
		setup_recv(rc);
		rc->fname = name;
		rc->location = vpq_file_location(file);
		if (col->geo != VPQ_GEO_NONE)
			vpq_geo_read_setup(rc, file, name);
	}
	else if (rc->mode == VPQ_RM_LIST || rc->mode == VPQ_RM_MAP_HSTORE || rc->mode == VPQ_RM_MAP_JSONB)
		vpq_nested_setup(t, rc, file, name);
}

/* PQ15: where a value was read, for an error of its type's receive function, as COPY BINARY tells */
static void
recv_error_context(void *arg)
{
	VpqReadCol *rc = (VpqReadCol *) arg;

	errcontext("column \"%s\" of a vexec_parquet table, read from the column \"%s\" of the file \"%s\"",
			   rc->col->name, rc->fname, rc->location);
}

bool
vpq_array_null(const vpq_array *a, int64 row)
{
	int64		i;

	if (a->type.id == VPQ_T_NULL)
		return true;
	if (a->validity == NULL)
		return false;
	i = a->offset + row;
	return ((a->validity[i >> 3] >> (i & 7)) & 1) == 0;
}

static int64
int_at(const vpq_array *a, int id, int64 i, bool *unsigned_big)
{
	*unsigned_big = false;
	switch (id)
	{
		case VPQ_T_INT8:
			return ((const int8 *) a->values)[i];
		case VPQ_T_INT16:
			return ((const int16 *) a->values)[i];
		case VPQ_T_INT32:
			return ((const int32 *) a->values)[i];
		case VPQ_T_INT64:
			return ((const int64 *) a->values)[i];
		case VPQ_T_UINT8:
			return ((const uint8 *) a->values)[i];
		case VPQ_T_UINT16:
			return ((const uint16 *) a->values)[i];
		case VPQ_T_UINT32:
			return ((const uint32 *) a->values)[i];
		case VPQ_T_UINT64:
			{
				uint64		u = ((const uint64 *) a->values)[i];

				if (u > (uint64) PG_INT64_MAX)
					*unsigned_big = true;
				return (int64) u;
			}
	}
	return 0;
}

static float
half_to_float(uint16 h)
{
	uint32		sign = (uint32) (h >> 15) & 1;
	uint32		exp = (h >> 10) & 0x1f;
	uint32		mant = h & 0x3ff;
	uint32		f;
	float		out;

	if (exp == 0)
	{
		if (mant == 0)
			f = sign << 31;
		else
		{
			exp = 127 - 15 + 1;
			while ((mant & 0x400) == 0)
			{
				mant <<= 1;
				exp--;
			}
			mant &= 0x3ff;
			f = (sign << 31) | (exp << 23) | (mant << 13);
		}
	}
	else if (exp == 31)
		f = (sign << 31) | 0x7f800000 | (mant << 13);
	else
		f = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
	memcpy(&out, &f, sizeof(out));
	return out;
}

/* A decimal's unscaled integer, as wide as a decimal128's at most */
int128
vpq_decimal_at(const vpq_array *a, const vpq_type *t, int64 i)
{
	switch (t->id)
	{
		case VPQ_T_DECIMAL32:
			return ((const int32 *) a->values)[i];
		case VPQ_T_DECIMAL64:
			return ((const int64 *) a->values)[i];
		case VPQ_T_DECIMAL128:
			{
				int128		v;

				memcpy(&v, a->values + i * 16, 16);
				return v;
			}
		case VPQ_T_DECIMAL256:
			{
				int128		lo;
				int128		hi;

				memcpy(&lo, a->values + i * 32, 16);
				memcpy(&hi, a->values + i * 32 + 16, 16);
				if (!((hi == 0 && lo >= 0) || (hi == -1 && lo < 0)))
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("a decimal256 value of a Parquet file passes 38 digits")));
				return lo;
			}
	}
	return 0;
}

int64
vpq_to_usecs(int64 v, int unit)
{
	switch (unit)
	{
		case VPQ_UNIT_S:
			if (v > PG_INT64_MAX / 1000000 || v < PG_INT64_MIN / 1000000)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			return v * 1000000;
		case VPQ_UNIT_MS:
			if (v > PG_INT64_MAX / 1000 || v < PG_INT64_MIN / 1000)
				ereport(ERROR,
						(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
						 errmsg("timestamp out of range")));
			return v * 1000;
		case VPQ_UNIT_US:
			return v;
		default:
			return v / 1000 - (v % 1000 < 0 ? 1 : 0);
	}
}

/* A string's bytes, and the length its offsets give it */
const char *
vpq_bytes_at(const vpq_array *a, VpqReadMode mode, int64 i, int *len)
{
	switch (mode)
	{
		case VPQ_RM_STRING:
			{
				const int32 *off = (const int32 *) a->values;

				*len = off[i + 1] - off[i];
				return (const char *) a->data + off[i];
			}
		case VPQ_RM_LARGE_STRING:
			{
				const int64 *off = (const int64 *) a->values;

				*len = (int) (off[i + 1] - off[i]);
				return (const char *) a->data + off[i];
			}
		case VPQ_RM_VIEW:
			{
				const char *view = (const char *) a->values + i * 16;
				int32		size;
				int32		buf;
				int32		offset;

				memcpy(&size, view, 4);
				*len = size;
				if (size <= 12)
					return view + 4;
				memcpy(&buf, view + 8, 4);
				memcpy(&offset, view + 12, 4);
				return (const char *) a->buffers[buf] + offset;
			}
		case VPQ_RM_FIXED_BINARY:
			*len = a->type.byte_width;
			return (const char *) a->values + i * a->type.byte_width;
		default:
			*len = 0;
			return NULL;
	}
}

/* A dictionary's index at a row */
int64
vpq_dict_index(const vpq_array *a, int64 i)
{
	switch (a->type.index_width)
	{
		case 1:
			return ((const int8 *) a->values)[i];
		case 2:
			return ((const int16 *) a->values)[i];
		case 4:
			return ((const int32 *) a->values)[i];
		default:
			return ((const int64 *) a->values)[i];
	}
}

/* Text from elsewhere checked, or made the database's encoding, as input is */
static char *
server_text(const char *bytes, int len, int *outlen)
{
	if (GetDatabaseEncoding() == PG_UTF8)
	{
		(void) pg_verify_mbstr(PG_UTF8, bytes, len, false);
		*outlen = len;
		return (char *) bytes;
	}
	else
	{
		char	   *conv = pg_any_to_server(bytes, len, PG_UTF8);

		/*
		 * the bytes themselves where nothing was converted -- an empty
		 * string, a SQL_ASCII database -- which end at no zero byte
		 */
		*outlen = conv == bytes ? len : (int) strlen(conv);
		return conv;
	}
}

static Datum
varlena_of(const char *bytes, int len)
{
	text	   *t = (text *) palloc(VARHDRSZ + len);

	SET_VARSIZE(t, VARHDRSZ + len);
	memcpy(VARDATA(t), bytes, len);
	return PointerGetDatum(t);
}

/* A string's bytes as the column's value: its encoding and its typmod as input would make them */
static Datum
string_datum(VpqReadCol *rc, const char *bytes, int len)
{
	VpqColumn  *col = rc->col;
	Datum		d;

	if (!rc->own && col->kind != VPQ_K_BYTEA)
		bytes = server_text(bytes, len, &len);
	d = varlena_of(bytes, len);
	if (!rc->own && col->typmod >= 0)
	{
		if (col->kind == VPQ_K_BPCHAR)
			d = DirectFunctionCall3(bpchar, d, Int32GetDatum(col->typmod), BoolGetDatum(false));
		else if (col->basetype == VARCHAROID)
			d = DirectFunctionCall3(varchar, d, Int32GetDatum(col->typmod), BoolGetDatum(false));
	}
	return d;
}

static Datum
numeric_of(VpqReadCol *rc, int128 v, int scale)
{
	VpqColumn  *col = rc->col;
	Datum		d = vpq_int128_to_numeric(v, scale);

	if (col->typmod >= (int32) VARHDRSZ && (scale != col->scale || rc->ftype.precision > col->precision ||
											 rc->mode == VPQ_RM_INT_NUMERIC))
		d = vpq_numeric_coerce(d, col->typmod);
	return d;
}

Datum
vpq_value_at(VpqReadCol *rc, const vpq_array *a, int64 row, bool *isnull)
{
	VpqColumn  *col = rc->col;
	int64		i;

	if (rc->mode == VPQ_RM_MISSING)
	{
		*isnull = col->missing_null;
		return col->missing;
	}
	if (rc->mode == VPQ_RM_NULL || vpq_array_null(a, row))
	{
		if (!rc->own && col->notnull)
			ereport(ERROR,
					(errcode(ERRCODE_NOT_NULL_VIOLATION),
					 errmsg("null value in column \"%s\" of a vexec_parquet table violates not-null constraint",
							col->name),
					 errdetail("The table's files hold a NULL where the column is declared NOT NULL.")));
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;
	i = a->offset + row;
	switch (rc->mode)
	{
		case VPQ_RM_FIXED:
			switch (col->kind)
			{
				case VPQ_K_CHAR:
					return CharGetDatum(((const int8 *) a->values)[i]);
				case VPQ_K_INT2:
					return Int16GetDatum(((const int16 *) a->values)[i]);
				case VPQ_K_INT4:
					return Int32GetDatum(((const int32 *) a->values)[i]);
				case VPQ_K_OID:
					return ObjectIdGetDatum(((const uint32 *) a->values)[i]);
				case VPQ_K_FLOAT4:
					return Float4GetDatum(((const float4 *) a->values)[i]);
				case VPQ_K_FLOAT8:
					return Float8GetDatum(((const float8 *) a->values)[i]);
				case VPQ_K_UUID:
					return PointerGetDatum(a->values + i * 16);
				default:
					return Int64GetDatum(((const int64 *) a->values)[i]);
			}
		case VPQ_RM_INT:
			{
				bool		big;
				int64		v = int_at(a, rc->ftype.id, i, &big);

				if (big || (col->kind == VPQ_K_INT2 && (v < PG_INT16_MIN || v > PG_INT16_MAX)) ||
					(col->kind == VPQ_K_INT4 && (v < PG_INT32_MIN || v > PG_INT32_MAX)) ||
					(col->kind == VPQ_K_OID && (v < 0 || v > (int64) PG_UINT32_MAX)))
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("value of column \"%s\" is out of range for type %s",
									col->name, format_type_be(col->typid))));
				switch (col->kind)
				{
					case VPQ_K_INT2:
						return Int16GetDatum((int16) v);
					case VPQ_K_INT4:
						return Int32GetDatum((int32) v);
					case VPQ_K_OID:
						return ObjectIdGetDatum((Oid) v);
					default:
						return Int64GetDatum(v);
				}
			}
		case VPQ_RM_BOOL:
			return BoolGetDatum((a->values[i >> 3] >> (i & 7)) & 1);
		case VPQ_RM_FLOAT:
			{
				double		v = rc->ftype.id == VPQ_T_FLOAT16 ?
					(double) half_to_float(((const uint16 *) a->values)[i]) :
					(double) ((const float4 *) a->values)[i];

				return col->kind == VPQ_K_FLOAT4 ? Float4GetDatum((float4) v) : Float8GetDatum(v);
			}
		case VPQ_RM_DATE32:
		case VPQ_RM_DATE64:
			{
				int64		days;

				if (rc->mode == VPQ_RM_DATE32)
				{
					int32		v = ((const int32 *) a->values)[i];

					if (DATE_NOT_FINITE(v))
						return DateADTGetDatum(v);
					days = v;
				}
				else
				{
					int64		ms = ((const int64 *) a->values)[i];

					days = ms / 86400000 - (ms % 86400000 < 0 ? 1 : 0);
				}
				days -= VPQ_EPOCH_DAYS;
				if (days < PG_INT32_MIN || days > PG_INT32_MAX || !IS_VALID_DATE((DateADT) days))
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("date out of range in column \"%s\"", col->name)));
				return DateADTGetDatum((DateADT) days);
			}
		case VPQ_RM_INT_DAYS:
			{
				bool		big;
				int64		v = int_at(a, rc->ftype.id, i, &big);

				/* PQ6: days since 1970, made PostgreSQL's, within its range */
				if (big || v < (int64) PG_INT32_MIN + VPQ_EPOCH_DAYS || v > (int64) PG_INT32_MAX + VPQ_EPOCH_DAYS ||
					!IS_VALID_DATE((DateADT) (v - VPQ_EPOCH_DAYS)))
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("date out of range in column \"%s\"", col->name)));
				return DateADTGetDatum((DateADT) (v - VPQ_EPOCH_DAYS));
			}
		case VPQ_RM_INT_SECONDS:
			{
				bool		big;
				int64		v = int_at(a, rc->ftype.id, i, &big);
				int64		us;

				/* PQ6: seconds since 1970, made PostgreSQL's microseconds, within its range */
				if (big || pg_mul_s64_overflow(v, USECS_PER_SEC, &us) || us < PG_INT64_MIN + VPQ_EPOCH_USECS ||
					!IS_VALID_TIMESTAMP(us - VPQ_EPOCH_USECS))
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range in column \"%s\"", col->name)));
				return TimestampGetDatum(us - VPQ_EPOCH_USECS);
			}
		case VPQ_RM_TIME:
			{
				int64		v = rc->ftype.id == VPQ_T_TIME32 ? ((const int32 *) a->values)[i] :
					((const int64 *) a->values)[i];

				v = vpq_to_usecs(v, rc->ftype.unit);
				if (v < 0 || v > USECS_PER_DAY)
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("time out of range in column \"%s\"", col->name)));
				return Int64GetDatum(v);
			}
		case VPQ_RM_TIMESTAMP:
			{
				int64		v = ((const int64 *) a->values)[i];

				if (rc->ftype.unit == VPQ_UNIT_US && TIMESTAMP_NOT_FINITE(v))
					return TimestampGetDatum(v);
				v = vpq_to_usecs(v, rc->ftype.unit) - VPQ_EPOCH_USECS;
				if (!IS_VALID_TIMESTAMP(v))
					ereport(ERROR,
							(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
							 errmsg("timestamp out of range in column \"%s\"", col->name)));
				return TimestampGetDatum(v);
			}
		case VPQ_RM_DECIMAL:
			return numeric_of(rc, vpq_decimal_at(a, &rc->ftype, i), rc->ftype.scale);
		case VPQ_RM_INT_NUMERIC:
			{
				bool		big;
				int64		v = int_at(a, rc->ftype.id, i, &big);
				int128		w = big ? (int128) (uint64) v : (int128) v;

				return numeric_of(rc, w, 0);
			}
		case VPQ_RM_STRING:
		case VPQ_RM_LARGE_STRING:
		case VPQ_RM_VIEW:
		case VPQ_RM_FIXED_BINARY:
			{
				int			len;
				const char *bytes = vpq_bytes_at(a, rc->mode, i, &len);

				return string_datum(rc, bytes, len);
			}
		case VPQ_RM_DICT:
			{
				const vpq_array *d = a->dictionary;
				int64		k = vpq_dict_index(a, i);
				int			len;
				const char *bytes;

				if (k < 0 || k >= d->length)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("a dictionary index of column \"%s\" is out of its dictionary", col->name)));
				bytes = vpq_bytes_at(d, bytes_mode(d->type.id), d->offset + k, &len);
				return string_datum(rc, bytes, len);
			}
		case VPQ_RM_IMAGE_FIXED:
			{
				const char *p = (const char *) a->values + i * col->typlen;

				if (col->typbyval)
				{
					Datum		tmp = 0;

					memcpy(&tmp, p, col->typlen);
					return fetch_att(&tmp, true, col->typlen);
				}
				else
				{
					char	   *copy = palloc(col->typlen);

					memcpy(copy, p, col->typlen);
					return PointerGetDatum(copy);
				}
			}
		case VPQ_RM_IMAGE_VARLENA:
			{
				int			len;
				const char *bytes = vpq_bytes_at(a, VPQ_RM_STRING, i, &len);

				return varlena_of(bytes, len);
			}
		case VPQ_RM_TEXT_INPUT:
			{
				int			len;
				const char *bytes = vpq_bytes_at(a, bytes_mode(rc->ftype.id), i, &len);
				char	   *cstr;

				bytes = server_text(bytes, len, &len);
				cstr = palloc(len + 1);
				memcpy(cstr, bytes, len);
				cstr[len] = '\0';
				return InputFunctionCall(&rc->input, cstr, rc->ioparam, col->typmod);
			}
		case VPQ_RM_RECV:
			{
				int			len;
				const char *bytes = vpq_bytes_at(a, rc->ftype.id == VPQ_T_FIXED_BINARY ? VPQ_RM_FIXED_BINARY :
												 bytes_mode(rc->ftype.id), i, &len);
				StringInfoData buf;
				Datum		d;
				ErrorContextCallback errcb;

				/* as COPY BINARY hands a field to it: its bytes ended by a zero byte, the cursor at its start */
				buf.maxlen = len + 5;
				buf.data = palloc(buf.maxlen);
				if (rc->geo_srid > 0)
					buf.len = vpq_geo_wkb_with_srid(bytes, len, rc->geo_srid, buf.data);
				else
				{
					memcpy(buf.data, bytes, len);
					buf.len = len;
				}
				buf.data[buf.len] = '\0';
				buf.cursor = 0;
				errcb.callback = recv_error_context;
				errcb.arg = rc;
				errcb.previous = error_context_stack;
				error_context_stack = &errcb;
				d = ReceiveFunctionCall(&rc->input, &buf, rc->ioparam, col->typmod);
				if (buf.cursor != buf.len)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
							 errmsg("incorrect binary data format")));
				error_context_stack = errcb.previous;
				return d;
			}
		case VPQ_RM_LIST:
		case VPQ_RM_MAP_HSTORE:
		case VPQ_RM_MAP_JSONB:
			return vpq_nested_value(rc, a, i);
		default:
			break;
	}
	elog(ERROR, "vexec_parquet: read mode %d of column \"%s\"", (int) rc->mode, col->name);
	return (Datum) 0;
}
