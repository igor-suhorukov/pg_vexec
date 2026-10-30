/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * nested.c
 *	  A file's nested columns read as PostgreSQL's arrays, hstore and jsonb
 *	  (pg_vector_executor.md §3.40, PQ20): a list of flat elements as an
 *	  array of its elements' type, a map of strings to strings as hstore,
 *	  any map of flat keys and values as a jsonb object.
 *
 * A list's elements are read as a column of the array's element type is
 * (read.c's modes: its encoding checked, its range, through the type's
 * input or receive function where no other mode reads it), each NULL kept,
 * and made a one-dimensional array of lower bound 1; an empty list is an
 * empty array.  A map's keys and values are read as text and made hstore by
 * hstore's own constructor, hstore(text[], text[]), which orders its keys
 * and keeps one of a key given twice; or its keys as text and its values as
 * JSON's own -- numbers, booleans, strings, null; dates and times in ISO
 * 8601, the others their text, a float's infinities and NaN too, as
 * to_jsonb() makes them -- made a jsonb object, a key given twice its last
 * value.  hstore is
 * another extension, found in the database by name, its type and
 * constructor the members of the extension "hstore" (pg_depend).
 *
 * These are a row's values, made a row at a time: on the CPU, and in vexec's
 * batches as Datums (vexec.c's generic_column()).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "nodes/value.h"
#include "parser/parse_func.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/syscache.h"
#include "varatt.h"

#include "vexec_parquet.h"

/* Whether a type is hstore's, of the extension "hstore" */
bool
vpq_nested_is_hstore(Oid typid)
{
	Oid			ext = get_extension_oid("hstore", true);
	HeapTuple	tup;
	bool		is;

	if (!OidIsValid(ext) || getExtensionOfObject(TypeRelationId, typid) != ext)
		return false;
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tup))
		return false;
	is = strcmp(NameStr(((Form_pg_type) GETSTRUCT(tup))->typname), "hstore") == 0;
	ReleaseSysCache(tup);
	return is;
}

/* The element type of an array type of PostgreSQL's own form -- not int2vector's, not oidvector's -- else InvalidOid */
Oid
vpq_nested_array_element(Oid typid)
{
	Oid			elem = get_element_type(typid);

	if (!OidIsValid(elem) || get_typlen(typid) != -1 || get_array_type(elem) != typid ||
		get_typsubscript(typid, NULL) != F_ARRAY_SUBSCRIPT_HANDLER)
		return InvalidOid;
	return elem;
}

/* hstore's constructor of two arrays of text, hstore(text[], text[]); InvalidOid where it has none */
static Oid
hstore_constructor(Oid hstore_type)
{
	Oid			ext = get_extension_oid("hstore", true);
	Oid			args[2] = {TEXTARRAYOID, TEXTARRAYOID};
	char	   *nspname;
	Oid			f;

	if (!OidIsValid(ext))
		return InvalidOid;
	nspname = get_namespace_name(get_extension_schema(ext));
	if (nspname == NULL)
		return InvalidOid;
	f = LookupFuncName(list_make2(makeString(nspname), makeString(pstrdup("hstore"))), 2, args, true);
	if (!OidIsValid(f) || getExtensionOfObject(ProcedureRelationId, f) != ext || get_func_rettype(f) != hstore_type)
		return InvalidOid;
	return f;
}

/* The PostgreSQL type a jsonb object's key or value of an Arrow type is read as, as columns() declares one */
static Oid
json_child_type(const vpq_type *t, int32 *typmod)
{
	*typmod = -1;
	switch (t->id)
	{
		case VPQ_T_BOOL:
			return BOOLOID;
		case VPQ_T_INT8:
		case VPQ_T_INT16:
		case VPQ_T_UINT8:
			return INT2OID;
		case VPQ_T_INT32:
		case VPQ_T_UINT16:
			return INT4OID;
		case VPQ_T_INT64:
		case VPQ_T_UINT32:
			return INT8OID;
		case VPQ_T_UINT64:
		case VPQ_T_DECIMAL32:
		case VPQ_T_DECIMAL64:
		case VPQ_T_DECIMAL128:
		case VPQ_T_DECIMAL256:
			return NUMERICOID;
		case VPQ_T_FLOAT16:
		case VPQ_T_FLOAT32:
			return FLOAT4OID;
		case VPQ_T_FLOAT64:
			return FLOAT8OID;
		case VPQ_T_DATE32:
		case VPQ_T_DATE64:
			return DATEOID;
		case VPQ_T_TIME32:
		case VPQ_T_TIME64:
			return TIMEOID;
		case VPQ_T_TIMESTAMP:
			return t->has_tz ? TIMESTAMPTZOID : TIMESTAMPOID;
		case VPQ_T_BINARY:
		case VPQ_T_LARGE_BINARY:
		case VPQ_T_BINARY_VIEW:
			return BYTEAOID;
		case VPQ_T_FIXED_BINARY:
			return t->uuid ? UUIDOID : BYTEAOID;
		default:
			return TEXTOID;
	}
}

/* A column of the nested column's child: its elements', keys' or values' type, the column's name for errors */
static VpqColumn *
child_column(VpqTable *t, VpqColumn *col, Oid typid, int32 typmod)
{
	VpqColumn  *c = palloc0(sizeof(VpqColumn));

	c->attnum = col->attnum;
	c->name = col->name;
	c->typid = typid;
	c->typmod = typmod;
	c->basetype = getBaseTypeAndTypmod(typid, &c->typmod);
	get_typlenbyvalalign(c->basetype, &c->typlen, &c->typbyval, &c->typalign);
	c->collation = col->collation;
	vpq_column_kind(c, t->utf8);
	c->recv = vpq_type_has_receive(typid);
	return c;
}

static void
refuse(VpqColumn *col, const char *fname, VpqReadCol *rc, vpq_file *file, const char *hint)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("column \"%s\" of type %s of a vexec_parquet table cannot be read from the file's column \"%s\" of type %s",
					col->name, format_type_with_typemod(col->typid, col->typmod), fname, rc->ftype.name),
			 errdetail("The file is \"%s\".", vpq_file_location(file)),
			 hint ? errhint("%s", hint) : 0));
}

/*
 * A nested column's read: its children read as columns of their types --
 * the array's element type, hstore's text, a jsonb object's types of the
 * file's -- and what makes the row's value of them
 */
void
vpq_nested_setup(VpqTable *t, VpqReadCol *rc, vpq_file *file, const char *fname)
{
	VpqColumn  *col = rc->col;
	vpq_type	kids[2];
	bool		nullable[2];
	int			n = vpq_file_column_children(file, rc->fcol, kids, nullable);

	rc->kids = palloc0(sizeof(VpqReadCol) * 2);
	if (rc->mode == VPQ_RM_LIST)
	{
		VpqColumn  *e;

		if (n != 1)
			refuse(col, fname, rc, file, NULL);
		/* an array's typmod is its elements' */
		e = child_column(t, col, col->elemtype, col->typmod);
		if (!vpq_read_child_setup(&rc->kids[0], e, rc->fcol, &kids[0]))
			refuse(col, fname, rc, file, NULL);
		rc->elemtype = col->elemtype;
		get_typlenbyvalalign(col->elemtype, &rc->elmlen, &rc->elmbyval, &rc->elmalign);
		return;
	}
	if (n != 2)
		refuse(col, fname, rc, file, NULL);
	if (rc->mode == VPQ_RM_MAP_HSTORE)
	{
		Oid			f = hstore_constructor(col->basetype);

		for (int k = 0; k < 2; k++)
			if (!vpq_read_child_setup(&rc->kids[k], child_column(t, col, TEXTOID, -1), rc->fcol, &kids[k]) ||
				rc->kids[k].mode == VPQ_RM_TEXT_INPUT || rc->kids[k].mode == VPQ_RM_RECV)
				refuse(col, fname, rc, file, "A map of other keys or values than strings is read as jsonb.");
		if (!OidIsValid(f))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_FUNCTION),
					 errmsg("hstore's function hstore(text[], text[]) is not in the database")));
		fmgr_info(f, &rc->input);
		return;
	}
	for (int k = 0; k < 2; k++)
	{
		int32		typmod;
		Oid			typid = json_child_type(&kids[k], &typmod);
		VpqColumn  *c = child_column(t, col, typid, typmod);
		Oid			outfunc;
		bool		varlena;

		if (!vpq_read_child_setup(&rc->kids[k], c, rc->fcol, &kids[k]))
			refuse(col, fname, rc, file, NULL);
		getTypeOutputInfo(c->typid, &outfunc, &varlena);
		fmgr_info(outfunc, &rc->kids[k].output);
	}
}

/* A child's value as text: a string's own bytes, a date's or time's ISO 8601 (to_jsonb()'s), any other its type's output */
static void
child_text(VpqReadCol *k, Datum d, JsonbValue *out)
{
	out->type = jbvString;
	if (k->col->kind == VPQ_K_TEXT || k->col->kind == VPQ_K_BPCHAR)
	{
		text	   *t = DatumGetTextPP(d);

		out->val.string.val = VARDATA_ANY(t);
		out->val.string.len = VARSIZE_ANY_EXHDR(t);
	}
	else if (k->col->kind == VPQ_K_DATE || k->col->kind == VPQ_K_TIME || k->col->kind == VPQ_K_TIMESTAMP ||
			 k->col->kind == VPQ_K_TIMESTAMPTZ)
	{
		char	   *s = JsonEncodeDateTime(NULL, d, k->col->basetype, NULL);

		out->val.string.val = s;
		out->val.string.len = strlen(s);
	}
	else
	{
		char	   *s = OutputFunctionCall(&k->output, d);

		out->val.string.val = s;
		out->val.string.len = strlen(s);
	}
}

/* A child's value as JSON's: a number, a boolean, null, else its text -- a float's infinities and NaN too */
static void
child_json(VpqReadCol *k, Datum d, bool isnull, JsonbValue *out)
{
	if (isnull)
	{
		out->type = jbvNull;
		return;
	}
	switch (k->col->kind)
	{
		case VPQ_K_BOOL:
			out->type = jbvBool;
			out->val.boolean = DatumGetBool(d);
			return;
		case VPQ_K_INT2:
			out->type = jbvNumeric;
			out->val.numeric = int64_to_numeric(DatumGetInt16(d));
			return;
		case VPQ_K_INT4:
			out->type = jbvNumeric;
			out->val.numeric = int64_to_numeric(DatumGetInt32(d));
			return;
		case VPQ_K_INT8:
			out->type = jbvNumeric;
			out->val.numeric = int64_to_numeric(DatumGetInt64(d));
			return;
		case VPQ_K_FLOAT4:
		case VPQ_K_FLOAT8:
			{
				double		v = k->col->kind == VPQ_K_FLOAT4 ? DatumGetFloat4(d) : DatumGetFloat8(d);

				if (isfinite(v))
				{
					out->type = jbvNumeric;
					out->val.numeric = DatumGetNumeric(DirectFunctionCall1(float8_numeric, Float8GetDatum(v)));
					return;
				}
				break;
			}
		case VPQ_K_NUMERIC:
		case VPQ_K_NUMERIC_ANY:
			if (!numeric_is_nan(DatumGetNumeric(d)) && !numeric_is_inf(DatumGetNumeric(d)))
			{
				out->type = jbvNumeric;
				out->val.numeric = DatumGetNumeric(d);
				return;
			}
			break;
		default:
			break;
	}
	child_text(k, d, out);
}

/* A list's bounds in its child, of its offsets at i */
static void
list_bounds(const vpq_array *a, int64 i, int64 *lo, int64 *hi)
{
	if (a->type.id == VPQ_T_LARGE_LIST)
	{
		*lo = ((const int64 *) a->values)[i];
		*hi = ((const int64 *) a->values)[i + 1];
	}
	else
	{
		*lo = ((const int32 *) a->values)[i];
		*hi = ((const int32 *) a->values)[i + 1];
	}
}

/* A nested column's value at i, its array's own offset applied: an array, hstore or jsonb */
Datum
vpq_nested_value(VpqReadCol *rc, const vpq_array *a, int64 i)
{
	int64		lo;
	int64		hi;
	int			n;
	Datum	   *d[2];
	bool	   *nulls[2];
	int			dims[1];
	int			lbs[1] = {1};

	list_bounds(a, i, &lo, &hi);
	if (lo < 0 || hi < lo || hi - lo > (int64) MaxArraySize)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("a list of column \"%s\" has offsets %lld to %lld", rc->col->name, (long long) lo,
						(long long) hi)));
	n = (int) (hi - lo);
	for (int k = 0; k < (rc->mode == VPQ_RM_LIST ? 1 : 2); k++)
	{
		const vpq_array *c = k == 0 ? a->child : a->child2;

		d[k] = palloc(sizeof(Datum) * Max(n, 1));
		nulls[k] = palloc(sizeof(bool) * Max(n, 1));
		for (int j = 0; j < n; j++)
			d[k][j] = vpq_value_at(&rc->kids[k], c, lo + j, &nulls[k][j]);
	}
	dims[0] = n;
	switch (rc->mode)
	{
		case VPQ_RM_LIST:
			if (n == 0)
				return PointerGetDatum(construct_empty_array(rc->elemtype));
			return PointerGetDatum(construct_md_array(d[0], nulls[0], 1, dims, lbs, rc->elemtype, rc->elmlen,
													  rc->elmbyval, rc->elmalign));
		case VPQ_RM_MAP_HSTORE:
			{
				ArrayType  *keys = n == 0 ? construct_empty_array(TEXTOID) :
					construct_md_array(d[0], nulls[0], 1, dims, lbs, TEXTOID, -1, false, TYPALIGN_INT);
				ArrayType  *vals = n == 0 ? construct_empty_array(TEXTOID) :
					construct_md_array(d[1], nulls[1], 1, dims, lbs, TEXTOID, -1, false, TYPALIGN_INT);

				return FunctionCall2(&rc->input, PointerGetDatum(keys), PointerGetDatum(vals));
			}
		case VPQ_RM_MAP_JSONB:
			{
				JsonbInState st = {0};

				pushJsonbValue(&st, WJB_BEGIN_OBJECT, NULL);
				for (int j = 0; j < n; j++)
				{
					JsonbValue	k;
					JsonbValue	v;

					if (nulls[0][j])
						ereport(ERROR,
								(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
								 errmsg("a map of column \"%s\" has a NULL key", rc->col->name)));
					child_text(&rc->kids[0], d[0][j], &k);
					child_json(&rc->kids[1], d[1][j], nulls[1][j], &v);
					pushJsonbValue(&st, WJB_KEY, &k);
					pushJsonbValue(&st, WJB_VALUE, &v);
				}
				pushJsonbValue(&st, WJB_END_OBJECT, NULL);
				return JsonbPGetDatum(JsonbValueToJsonb(st.result));
			}
		default:
			break;
	}
	elog(ERROR, "vexec_parquet: read mode %d of a nested column \"%s\"", (int) rc->mode, rc->col->name);
	return (Datum) 0;
}

/*
 * The type vexec_parquet.columns() declares a nested column as: an array
 * of its elements' type, hstore for a map of strings to strings where the
 * database has hstore, jsonb for any other map; NULL for any other column
 */
char *
vpq_nested_type_sql(vpq_file *file, int fcol, const vpq_type *type)
{
	vpq_type	kids[2];
	bool		nullable[2];
	int			n = vpq_file_column_children(file, fcol, kids, nullable);
	const char *elem;

	if ((type->id == VPQ_T_LIST || type->id == VPQ_T_LARGE_LIST) && n == 1)
	{
		elem = vpq_type_sql(&kids[0]);
		return elem != NULL ? psprintf("%s[]", elem) : NULL;
	}
	if (type->id != VPQ_T_MAP || n != 2)
		return NULL;
	{
		bool		strings = true;

		for (int k = 0; k < 2; k++)
		{
			const char *sql = vpq_type_sql(&kids[k]);

			if (sql == NULL || strcmp(sql, "text") != 0)
				strings = false;
		}
		if (strings)
		{
			Oid			ext = get_extension_oid("hstore", true);

			if (OidIsValid(ext))
			{
				Oid			hstore = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("hstore"),
													 ObjectIdGetDatum(get_extension_schema(ext)));

				if (OidIsValid(hstore))
					return format_type_be(hstore);
			}
		}
	}
	return pstrdup("jsonb");
}
