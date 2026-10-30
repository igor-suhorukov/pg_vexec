/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * geo.c
 *	  GeoParquet's columns read as PostGIS's types (pg_vector_executor.md
 *	  §3.39, PQ16-PQ18).
 *
 * A file column of geometries is one GeoParquet's metadata names -- the
 * footer's key "geo", of every version, the only sign of one in a file of
 * GeoParquet 1.x without Parquet's geospatial types -- or one of Parquet's
 * logical types GEOMETRY and GEOGRAPHY, which GeoParquet 2.0 writes, with
 * the key or without it.  Its values are WKB, which read.c hands to the
 * declared type's receive function (PQ15).  For PostGIS's geometry:
 *
 *	- its SRID is its CRS's: the logical type's CRS where it writes one,
 *	  else GeoParquet's crs of the column, else -- neither naming one --
 *	  OGC:CRS84, both formats' default, SRID 4326.  A CRS of EPSG's -- an
 *	  "EPSG:<n>", a PROJJSON's id -- is SRID <n>, Parquet's "srid:<n>" <n>,
 *	  OGC:CRS84 4326; GeoParquet's null, and any other, unknown, 0;
 *	- a column whose typmod has an SRID reads no file of another SRID known:
 *	  an error as the file is opened for it; one whose typmod has none
 *	  gives the file's SRID to each value of WKB without one of its own, as
 *	  EWKB carries one (PQ17) -- an EWKB's own SRID stays;
 *	- a column of spherical edges -- Parquet's GEOGRAPHY, GeoParquet's edges
 *	  "spherical" -- is not of geometries: an error;
 *	- a scan's && of the column and a constant prunes a unit by its bounds
 *	  (PQ18): its column chunk's GeospatialStatistics, else GeoParquet's
 *	  bbox of the file's column.  && compares the two geometries' bounding
 *	  boxes as floats, each rounded outward, whatever their SRIDs
 *	  (gserialized_overlaps_2d()), and so do the bounds here.
 *
 * PostGIS is another extension: its types and functions are found in the
 * database by name, as members of the extension "postgis" (pg_depend), never
 * by symbol.  Where it is not installed nothing here applies: a binary
 * column is read through any declared type's receive function, as COPY
 * BINARY reads it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>

#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "mb/pg_wchar.h"
#include "nodes/miscnodes.h"
#include "nodes/value.h"
#include "parser/parse_func.h"
#include "port/pg_bswap.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "vexec_parquet.h"

/* EWKB's flag of a type that an SRID follows */
#define EWKB_SRID_FLAG	0x20000000

/* OGC:CRS84's SRID in PostGIS's spatial_ref_sys: EPSG's 4326, of the same datum */
#define CRS84_SRID		4326

static Oid
postgis_extension(void)
{
	return get_extension_oid("postgis", true);
}

/* Whether a type is PostGIS's geometry or geography: VPQ_GEO_GEOMETRY, VPQ_GEO_GEOGRAPHY, else VPQ_GEO_NONE */
int
vpq_geo_type(Oid typid)
{
	Oid			ext = postgis_extension();
	HeapTuple	tup;
	int			kind = VPQ_GEO_NONE;

	if (!OidIsValid(ext) || getExtensionOfObject(TypeRelationId, typid) != ext)
		return VPQ_GEO_NONE;
	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tup))
		return VPQ_GEO_NONE;
	if (strcmp(NameStr(((Form_pg_type) GETSTRUCT(tup))->typname), "geometry") == 0)
		kind = VPQ_GEO_GEOMETRY;
	else if (strcmp(NameStr(((Form_pg_type) GETSTRUCT(tup))->typname), "geography") == 0)
		kind = VPQ_GEO_GEOGRAPHY;
	ReleaseSysCache(tup);
	return kind;
}

/* PostGIS's function of a name and argument types, InvalidOid where it has none */
static Oid
postgis_function(const char *name, int nargs, const Oid *argtypes)
{
	Oid			ext = postgis_extension();
	char	   *nspname;
	Oid			f;

	if (!OidIsValid(ext))
		return InvalidOid;
	nspname = get_namespace_name(get_extension_schema(ext));
	if (nspname == NULL)
		return InvalidOid;
	f = LookupFuncName(list_make2(makeString(nspname), makeString(pstrdup(name))), nargs, argtypes, true);
	if (!OidIsValid(f) || getExtensionOfObject(ProcedureRelationId, f) != ext)
		return InvalidOid;
	return f;
}

/* The SRID of a PostGIS typmod, postgis_typmod_srid()'s: 0 none */
static int32
typmod_srid(int32 typmod)
{
	Oid			argtype = INT4OID;
	Oid			f;

	if (typmod < 0)
		return 0;
	f = postgis_function("postgis_typmod_srid", 1, &argtype);
	if (!OidIsValid(f))
		return 0;
	return DatumGetInt32(OidFunctionCall1(f, Int32GetDatum(typmod)));
}

/* Whether a function is PostGIS's geometry_overlaps(geometry, geometry): the operator && */
bool
vpq_geo_overlaps_func(Oid funcid)
{
	Oid			ext = postgis_extension();
	HeapTuple	tup;
	bool		is;

	if (!OidIsValid(ext))
		return false;
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tup))
		return false;
	{
		Form_pg_proc p = (Form_pg_proc) GETSTRUCT(tup);

		is = strcmp(NameStr(p->proname), "geometry_overlaps") == 0 && p->pronargs == 2 &&
			vpq_geo_type(p->proargtypes.values[0]) == VPQ_GEO_GEOMETRY &&
			vpq_geo_type(p->proargtypes.values[1]) == VPQ_GEO_GEOMETRY;
	}
	ReleaseSysCache(tup);
	return is && getExtensionOfObject(ProcedureRelationId, funcid) == ext;
}

/*
 * A constant geometry's bounds, as box3d() and ST_XMin() and the others make
 * them: false for an empty one, which has none
 */
bool
vpq_geo_const_box(Datum geom, Oid geomtype, double box[4])
{
	static const char *const names[4] = {"st_xmin", "st_xmax", "st_ymin", "st_ymax"};
	Oid			to_box = postgis_function("box3d", 1, &geomtype);
	Oid			boxtype;
	FmgrInfo	flinfo;
	Datum		b;
	LOCAL_FCINFO(fcinfo, 1);

	if (!OidIsValid(to_box))
		return false;
	fmgr_info(to_box, &flinfo);
	InitFunctionCallInfoData(*fcinfo, &flinfo, 1, InvalidOid, NULL, NULL);
	fcinfo->args[0].value = geom;
	fcinfo->args[0].isnull = false;
	b = FunctionCallInvoke(fcinfo);
	if (fcinfo->isnull)
		return false;
	boxtype = get_func_rettype(to_box);
	for (int i = 0; i < 4; i++)
	{
		Oid			f = postgis_function(names[i], 1, &boxtype);

		if (!OidIsValid(f))
			return false;
		box[i] = DatumGetFloat8(OidFunctionCall1(f, b));
	}
	return isfinite(box[0]) && isfinite(box[1]) && isfinite(box[2]) && isfinite(box[3]) &&
		box[0] <= box[1] && box[2] <= box[3];
}

/* A double as the float PostGIS's bounding boxes round it to: next_float_down(), next_float_up() (lwgeom_api.c) */
static double
float_down(double d)
{
	float		f;

	if (d > (double) FLT_MAX)
		return FLT_MAX;
	if (d <= (double) -FLT_MAX)
		return -FLT_MAX;
	f = (float) d;
	if ((double) f <= d)
		return f;
	return nextafterf(f, -FLT_MAX);
}

static double
float_up(double d)
{
	float		f;

	if (d >= (double) FLT_MAX)
		return FLT_MAX;
	if (d < (double) -FLT_MAX)
		return -FLT_MAX;
	f = (float) d;
	if ((double) f >= d)
		return f;
	return nextafterf(f, FLT_MAX);
}

/*
 * Whether two bounds -- xmin, xmax, ymin, ymax -- overlap as && takes them:
 * each rounded outward to floats, as PostGIS's boxes of values within them
 * are, box2df_overlaps()'s test
 */
bool
vpq_geo_boxes_overlap(const double a[4], const double b[4])
{
	return !(float_down(a[0]) > float_up(b[1]) || float_down(b[0]) > float_up(a[1]) ||
			 float_down(a[2]) > float_up(b[3]) || float_down(b[2]) > float_up(a[3]));
}

/*
 * GeoParquet's metadata of a file, the footer's key "geo" as jsonb; NULL
 * where it has none, or none that parses.  Its strings are its CRSs' names
 * and the like, which a database not of UTF-8 reads with '?' for each byte
 * past ASCII: what is read of it -- keys, encodings, authorities' codes,
 * numbers -- is ASCII.
 */
static Jsonb *
geo_metadata(vpq_file *file)
{
	size_t		len;
	const char *s = vpq_file_geo_metadata(file, &len);
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	char	   *text;
	Datum		d;

	if (s == NULL)
		return NULL;
	text = palloc(len + 1);
	memcpy(text, s, len);
	text[len] = '\0';
	if (strlen(text) != len)
		return NULL;
	if (GetDatabaseEncoding() != PG_UTF8)
		for (size_t i = 0; i < len; i++)
			if ((unsigned char) text[i] >= 0x80)
				text[i] = '?';
	if (!DirectInputFunctionCallSafe(jsonb_in, text, JSONBOID, -1, (Node *) &escontext, &d))
		return NULL;
	return DatumGetJsonbP(d);
}

static bool
json_key(JsonbContainer *c, const char *key, JsonbValue *out)
{
	if (c == NULL || !JsonContainerIsObject(c))
		return false;
	return getKeyJsonValueFromContainer(c, key, strlen(key), out) != NULL;
}

/* A JSON string's or number's text, NUL-terminated; NULL for another value */
static char *
json_text(JsonbValue *v)
{
	if (v->type == jbvString)
		return pnstrdup(v->val.string.val, v->val.string.len);
	if (v->type == jbvNumeric)
		return DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(v->val.numeric)));
	return NULL;
}

/* A string of digits as an SRID, within PostGIS's; 0 for any other */
static int32
digits_srid(const char *s)
{
	int64		n = 0;

	if (*s == '\0')
		return 0;
	for (; *s; s++)
	{
		if (*s < '0' || *s > '9')
			return 0;
		n = n * 10 + (*s - '0');
		if (n > 999999)
			return 0;
	}
	return (int32) n;
}

/* An authority's code as an SRID: EPSG's its number, OGC's CRS84 4326; 0 any other */
static int32
authority_srid(const char *authority, const char *code)
{
	if (pg_strcasecmp(authority, "EPSG") == 0)
		return digits_srid(code);
	if (pg_strcasecmp(authority, "OGC") == 0 && pg_strcasecmp(code, "CRS84") == 0)
		return CRS84_SRID;
	return 0;
}

/* A PROJJSON's id ({"authority": ..., "code": ...}) as an SRID: 0 unknown */
static int32
id_srid(JsonbContainer *id)
{
	JsonbValue	authority;
	JsonbValue	code;
	char	   *a;
	char	   *c;

	if (!json_key(id, "authority", &authority) || !json_key(id, "code", &code))
		return 0;
	a = json_text(&authority);
	c = json_text(&code);
	return a != NULL && c != NULL ? authority_srid(a, c) : 0;
}

/* A PROJJSON's SRID, of its "id" or the first of its "ids" known: 0 unknown */
static int32
projjson_srid(JsonbContainer *c)
{
	JsonbValue	v;

	if (json_key(c, "id", &v) && v.type == jbvBinary)
		return id_srid(v.val.binary.data);
	if (json_key(c, "ids", &v) && v.type == jbvBinary && JsonContainerIsArray(v.val.binary.data))
	{
		int			n = JsonContainerSize(v.val.binary.data);

		for (int i = 0; i < n; i++)
		{
			JsonbValue *e = getIthJsonbValueFromContainer(v.val.binary.data, i);
			int32		srid;

			if (e != NULL && e->type == jbvBinary && (srid = id_srid(e->val.binary.data)) > 0)
				return srid;
		}
	}
	return 0;
}

/*
 * A CRS written as text -- Parquet's logical types', or a string of
 * GeoParquet's -- as an SRID: "srid:<n>", "<authority>:<code>", an OGC URN,
 * a PROJJSON object; 0 unknown
 */
static int32
crs_text_srid(const char *s, size_t len)
{
	char	   *crs = pnstrdup(s, len);
	char	   *p = crs;
	char	   *colon;
	size_t		n;

	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;
	n = strlen(p);
	while (n > 0 && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\n' || p[n - 1] == '\r'))
		p[--n] = '\0';
	if (*p == '{')
	{
		ErrorSaveContext escontext = {T_ErrorSaveContext};
		Datum		d;

		if (GetDatabaseEncoding() != PG_UTF8)
			for (size_t i = 0; i < n; i++)
				if ((unsigned char) p[i] >= 0x80)
					p[i] = '?';
		if (!DirectInputFunctionCallSafe(jsonb_in, p, JSONBOID, -1, (Node *) &escontext, &d))
			return 0;
		return projjson_srid(&DatumGetJsonbP(d)->root);
	}
	if (pg_strncasecmp(p, "srid:", 5) == 0)
		return digits_srid(p + 5);
	/* urn:ogc:def:crs:<authority>:<version>:<code>, its version empty or not */
	if (pg_strncasecmp(p, "urn:ogc:def:crs:", 16) == 0)
	{
		char	   *authority = p + 16;
		char	   *rest = strchr(authority, ':');
		char	   *code;

		if (rest == NULL)
			return 0;
		*rest++ = '\0';
		code = strrchr(rest, ':');
		return authority_srid(authority, code ? code + 1 : rest);
	}
	colon = strchr(p, ':');
	if (colon == NULL)
		return 0;
	*colon = '\0';
	return authority_srid(p, colon + 1);
}

/* GeoParquet's crs of a column as an SRID: null unknown, a PROJJSON object's, a string's */
static int32
crs_value_srid(JsonbValue *v)
{
	if (v->type == jbvString)
		return crs_text_srid(v->val.string.val, v->val.string.len);
	if (v->type == jbvBinary && JsonContainerIsObject(v->val.binary.data))
		return projjson_srid(v->val.binary.data);
	return 0;
}

/* GeoParquet's bbox of a column: [xmin, ymin, xmax, ymax], or of six with Z's; into xmin, xmax, ymin, ymax */
static bool
json_bbox(JsonbContainer *c, double out[4])
{
	double		v[6];
	int			n;

	if (!JsonContainerIsArray(c))
		return false;
	n = JsonContainerSize(c);
	if (n != 4 && n != 6)
		return false;
	for (int i = 0; i < n; i++)
	{
		JsonbValue *e = getIthJsonbValueFromContainer(c, i);
		char	   *t;

		if (e == NULL || e->type != jbvNumeric || (t = json_text(e)) == NULL)
			return false;
		v[i] = strtod(t, NULL);
	}
	out[0] = v[0];
	out[1] = v[n / 2];
	out[2] = v[1];
	out[3] = v[n / 2 + 1];
	return isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]) && isfinite(out[3]) &&
		out[0] <= out[1] && out[2] <= out[3];
}

/*
 * GeoParquet's geometry_types of a column as PostGIS's typmod names a type:
 * one type, or Geometry of several, with their dimensions' suffix -- Z, M,
 * ZM -- where they share it; NULL where they are none or do not
 */
static const char *
json_typmod_type(JsonbContainer *c)
{
	static const char *const types[] = {
		"Point", "LineString", "Polygon", "MultiPoint", "MultiLineString", "MultiPolygon", "GeometryCollection"
	};
	const char *type = NULL;
	char		dims[3] = "";
	int			n;

	if (!JsonContainerIsArray(c) || (n = JsonContainerSize(c)) == 0)
		return NULL;
	for (int i = 0; i < n; i++)
	{
		JsonbValue *e = getIthJsonbValueFromContainer(c, i);
		const char *t = NULL;
		const char *s;
		const char *d = NULL;
		int			len;

		if (e == NULL || e->type != jbvString)
			return NULL;
		s = e->val.string.val;
		len = e->val.string.len;
		for (int k = 0; k < (int) lengthof(types) && t == NULL; k++)
		{
			int			tl = strlen(types[k]);

			if (len >= tl && pg_strncasecmp(s, types[k], tl) == 0 && (len == tl || s[tl] == ' '))
			{
				t = types[k];
				d = len == tl ? "" : len == tl + 2 && (s[tl + 1] == 'Z' || s[tl + 1] == 'M') ? (s[tl + 1] == 'Z' ? "Z" : "M") :
					len == tl + 3 && s[tl + 1] == 'Z' && s[tl + 2] == 'M' ? "ZM" : NULL;
			}
		}
		if (t == NULL || d == NULL)
			return NULL;
		if (i == 0)
		{
			type = t;
			strlcpy(dims, d, sizeof(dims));
		}
		else
		{
			if (strcmp(dims, d) != 0)
				return NULL;
			if (type != t)
				type = "Geometry";
		}
	}
	return psprintf("%s%s", type, dims);
}

/* A file column's geometries as GeoParquet's metadata and Parquet's geospatial logical types tell of them */
void
vpq_geo_facts(vpq_file *file, int fcol, const char *name, VpqGeoFacts *g)
{
	const char *crs;
	size_t		crs_len;
	int			kind = vpq_file_column_geo(file, fcol, &crs, &crs_len);
	Jsonb	   *geo = geo_metadata(file);
	JsonbContainer *col = NULL;
	JsonbValue	v;

	memset(g, 0, sizeof(*g));
	if (geo != NULL)
	{
		JsonbValue	cols;
		JsonbValue	c;

		if (json_key(&geo->root, "columns", &cols) && cols.type == jbvBinary &&
			json_key(cols.val.binary.data, name, &c) && c.type == jbvBinary &&
			JsonContainerIsObject(c.val.binary.data))
			col = c.val.binary.data;
	}
	if (kind == VPQ_GEO_NONE && col == NULL)
		return;
	g->geo = true;
	g->spherical = kind == VPQ_GEO_GEOGRAPHY;
	g->wkb = true;
	if (kind != VPQ_GEO_NONE && crs_len > 0)
		g->srid = crs_text_srid(crs, crs_len);
	else if (col != NULL && json_key(col, "crs", &v))
		g->srid = crs_value_srid(&v);
	else
		g->srid = CRS84_SRID;
	if (col == NULL)
		return;
	if (json_key(col, "encoding", &v))
		g->wkb = v.type == jbvString && v.val.string.len == 3 && pg_strncasecmp(v.val.string.val, "WKB", 3) == 0;
	if (json_key(col, "edges", &v) && v.type == jbvString && v.val.string.len == 9 &&
		pg_strncasecmp(v.val.string.val, "spherical", 9) == 0)
		g->spherical = true;
	if (json_key(col, "bbox", &v) && v.type == jbvBinary)
		g->has_bbox = json_bbox(v.val.binary.data, g->bbox);
	if (json_key(col, "geometry_types", &v) && v.type == jbvBinary)
		g->typmod_type = json_typmod_type(v.val.binary.data);
}

/*
 * A geometry column's checks of a file's column of geometries, as read.c
 * opens it, and the SRID its values of WKB without one are given (PQ17)
 */
void
vpq_geo_read_setup(VpqReadCol *rc, vpq_file *file, const char *fname)
{
	VpqColumn  *col = rc->col;
	VpqGeoFacts g;
	int32		srid;

	if (col->geo != VPQ_GEO_GEOMETRY)
		return;
	vpq_geo_facts(file, rc->fcol, fname, &g);
	if (!g.geo)
		return;
	if (g.spherical)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" of type %s of a vexec_parquet table cannot be read from the file's column \"%s\" of geographies",
						col->name, format_type_with_typemod(col->typid, col->typmod), fname),
				 errdetail("The file's column has spherical edges, as Parquet's GEOGRAPHY or GeoParquet's \"edges\" says. The file is \"%s\".",
						   vpq_file_location(file)),
				 errhint("Declare the column geography.")));
	if (g.srid <= 0)
		return;
	srid = typmod_srid(col->typmod);
	if (srid > 0 && srid != g.srid)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("column \"%s\" of type %s of a vexec_parquet table cannot be read from the file's column \"%s\" of SRID %d",
						col->name, format_type_with_typemod(col->typid, col->typmod), fname, g.srid),
				 errdetail("The file's CRS is of SRID %d. The file is \"%s\".", g.srid, vpq_file_location(file))));
	if (srid <= 0)
		rc->geo_srid = g.srid;
}

static uint32
wkb_uint32(const char *p, bool le)
{
	uint32		v;

	memcpy(&v, p, 4);
#ifdef WORDS_BIGENDIAN
	return le ? pg_bswap32(v) : v;
#else
	return le ? v : pg_bswap32(v);
#endif
}

static void
wkb_put_uint32(char *p, uint32 v, bool le)
{
#ifdef WORDS_BIGENDIAN
	v = le ? pg_bswap32(v) : v;
#else
	v = le ? v : pg_bswap32(v);
#endif
	memcpy(p, &v, 4);
}

/*
 * A value of WKB with an SRID, into out (len + 4 bytes): ISO's or OGC's WKB,
 * of either byte order, whose type has no SRID, as EWKB writes one -- its
 * type's flag set, the SRID after it, in the value's byte order -- which
 * PostGIS's WKB parser reads of ISO's types too; any other value as it is
 */
int
vpq_geo_wkb_with_srid(const char *wkb, int len, int32 srid, char *out)
{
	bool		le;
	uint32		type;

	if (len < 5 || (wkb[0] != 0 && wkb[0] != 1))
	{
		memcpy(out, wkb, len);
		return len;
	}
	le = wkb[0] == 1;
	type = wkb_uint32(wkb + 1, le);
	if (type & EWKB_SRID_FLAG)
	{
		memcpy(out, wkb, len);
		return len;
	}
	out[0] = wkb[0];
	wkb_put_uint32(out + 1, type | EWKB_SRID_FLAG, le);
	wkb_put_uint32(out + 5, (uint32) srid, le);
	memcpy(out + 9, wkb + 5, len - 5);
	return len + 4;
}

/*
 * A geometry column's bounds in each of a file's units (PQ18), into
 * stats[unit * stride]: its column chunk's GeospatialStatistics, else
 * GeoParquet's bbox of the file's column; none for spherical edges
 */
void
vpq_geo_stats(vpq_file *file, int fcol, int nunits, VpqStat *stats, int stride)
{
	const char *name;
	int			field_id;
	vpq_type	type;
	bool		nullable;
	VpqGeoFacts g;

	(void) vpq_file_column(file, fcol, &name, &field_id, &type, &nullable);
	vpq_geo_facts(file, fcol, name, &g);
	if (!g.geo || g.spherical)
		return;
	for (int u = 0; u < nunits; u++)
	{
		VpqStat    *s = &stats[(Size) u * stride];

		if (vpq_file_column_geo_bounds(file, u, fcol, &s->box[0], &s->box[1], &s->box[2], &s->box[3]))
			s->has_box = true;
		else if (g.has_bbox)
		{
			memcpy(s->box, g.bbox, sizeof(s->box));
			s->has_box = true;
		}
	}
}

/*
 * The type vexec_parquet.columns() declares a file column of geometries
 * as, where the database has PostGIS: geometry, of GeoParquet's
 * geometry_types and the CRS's SRID where they are known -- NULL for any
 * other column, and one of spherical edges or not of WKB
 */
char *
vpq_geo_type_sql(vpq_file *file, int fcol, const char *name)
{
	Oid			ext = postgis_extension();
	VpqGeoFacts g;
	Oid			geomtype;

	if (!OidIsValid(ext))
		return NULL;
	vpq_geo_facts(file, fcol, name, &g);
	if (!g.geo || g.spherical || !g.wkb)
		return NULL;
	geomtype = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid, CStringGetDatum("geometry"),
							   ObjectIdGetDatum(get_extension_schema(ext)));
	if (!OidIsValid(geomtype))
		return NULL;
	if (g.typmod_type != NULL && g.srid > 0)
		return psprintf("%s(%s,%d)", format_type_be(geomtype), g.typmod_type, g.srid);
	if (g.typmod_type != NULL)
		return psprintf("%s(%s)", format_type_be(geomtype), g.typmod_type);
	return format_type_be(geomtype);
}
