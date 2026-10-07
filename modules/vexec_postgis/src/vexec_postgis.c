/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_postgis.c
 *	  vexec's kernel pack for PostGIS (pg_vector_executor.md §3.17, VK).
 *
 * It declares, of PostGIS 3.7.0rc2's geometry functions, read in its code
 * (postgis/gserialized_gist_2d.c, postgis/lwgeom_ogc.c,
 * postgis/lwgeom_geos_predicates.c, and the liblwgeom functions they call):
 *
 *	never raises	&& and the other eleven 2-D box operators -- their
 *					functions geometry_overlaps, _same, _contains, _within,
 *					_left, _overleft, _below, _overbelow, _overright,
 *					_right, _overabove and _above -- which compare two boxes
 *					taken by gserialized_datum_get_box2df_p(): the cached
 *					one, read from a slice of a toasted value; or one
 *					computed from the coordinates, by liblwgeom functions
 *					whose errors are a malformed value's, which PostGIS
 *					never stores; or an empty box for an empty geometry.
 *					And ST_SRID(geometry), which reads the header alone.
 *	check			ST_X and ST_Y, which raise on a geometry that is not a
 *					point and give NULL for an empty point: their check is
 *					that the value is a point, by the pack's reader.
 *	prefilter		ST_Intersects, ST_Touches, ST_Overlaps, ST_Crosses,
 *					ST_Disjoint, ST_Contains, ST_ContainsProperly,
 *					ST_Covers, ST_Within, ST_CoveredBy and ST_Equals, which
 *					each raise on geometries of different SRIDs, answer for
 *					an empty one, and then short-circuit on their two boxes
 *					before GEOS is called.  The prefilter gives those
 *					answers alone, and reports every other row undecided --
 *					different SRIDs, so that PostGIS raises its error; an
 *					empty geometry; boxes that do not decide; and a value
 *					its reader does not know -- which vexec then sends to
 *					PostGIS's own function.
 *
 *	  The short-circuits, as PostGIS 3.7.0rc2 has them:
 *
 *	  | ST_Intersects, _Touches, _Overlaps, _Crosses | boxes do not overlap | false |
 *	  | ST_Disjoint                                  | boxes do not overlap | true  |
 *	  | ST_Contains, _ContainsProperly, _Covers      | 2nd box not in 1st   | false |
 *	  | ST_Within, _CoveredBy                        | 1st box not in 2nd   | false |
 *	  | ST_Equals                                    | boxes differ (below) | false |
 *
 *	  ST_Equals compares its boxes through gbox_same_2d_float(), whose
 *	  minimums' tests compare one box's value with itself (liblwgeom/gbox.c:
 *	  195-203): only the maximums decide, and the prefilter compares only
 *	  them, so as to give the answers PostGIS gives and no other.
 *
 * The boxes are what the predicates' gserialized_get_gbox_p() gives, from
 * the reader (reader.c): floats, cached or rounded outward.  Every answer
 * outside a prefilter's is the function's own, called through fmgr: by vexec
 * for the box operators, ST_SRID, and ST_X and ST_Y where the check passes;
 * by PostgreSQL's evaluator for the rows the prefilters leave undecided.
 *
 * Versions.  The pack names PostGIS 3.7.0rc2, whose code was read.  PostGIS's
 * master at e08d79a is the same in every file read, but its version,
 * 3.7.0dev, names every commit of master, so the pack does not name it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "vexec_kernels.h"

#include "reader.h"

PG_MODULE_MAGIC_EXT(
					.name = "vexec_postgis",
					.version = "1.0"
);

PG_FUNCTION_INFO_V1(vexec_postgis_is_point);
PG_FUNCTION_INFO_V1(vexec_postgis_prefilter);
PG_FUNCTION_INFO_V1(vexec_postgis_reader);
PG_FUNCTION_INFO_V1(vexec_postgis_declarations);

/* ---- the boxes' tests, PostGIS's short-circuits ---- */

static bool
boxes_overlap(const GsInfo *a, const GsInfo *b)
{
	return !(a->xmax < b->xmin || a->ymax < b->ymin || a->xmin > b->xmax || a->ymin > b->ymax);
}

/* inner's box within outer's */
static bool
box_inside(const GsInfo *inner, const GsInfo *outer)
{
	return !(inner->xmin < outer->xmin || inner->xmax > outer->xmax ||
			 inner->ymin < outer->ymin || inner->ymax > outer->ymax);
}

/* ST_Equals's comparison, which only the maximums decide (above) */
static bool
boxes_same(const GsInfo *a, const GsInfo *b)
{
	return a->xmax == b->xmax && a->ymax == b->ymax;
}

typedef enum Shortcut
{
	SC_NO_OVERLAP_FALSE,		/* intersects, touches, overlaps, crosses */
	SC_NO_OVERLAP_TRUE,			/* disjoint */
	SC_CONTAINS,				/* contains, containsproperly, covers */
	SC_WITHIN,					/* within, coveredby */
	SC_EQUALS
} Shortcut;

/*
 * A predicate's answer where its boxes decide it; undecided, by a soft
 * error, everywhere else.
 */
static Datum
prefilter(FunctionCallInfo fcinfo, Shortcut sc)
{
	GsInfo		a;
	GsInfo		b;

	gs_read(PG_GETARG_DATUM(0), &a);
	gs_read(PG_GETARG_DATUM(1), &b);
	if (a.known && b.known && !a.empty && !b.empty && a.has_box && b.has_box &&
		memcmp(a.srid, b.srid, sizeof(a.srid)) == 0)
	{
		switch (sc)
		{
			case SC_NO_OVERLAP_FALSE:
				if (!boxes_overlap(&a, &b))
					PG_RETURN_BOOL(false);
				break;
			case SC_NO_OVERLAP_TRUE:
				if (!boxes_overlap(&a, &b))
					PG_RETURN_BOOL(true);
				break;
			case SC_CONTAINS:
				if (!box_inside(&b, &a))
					PG_RETURN_BOOL(false);
				break;
			case SC_WITHIN:
				if (!box_inside(&a, &b))
					PG_RETURN_BOOL(false);
				break;
			case SC_EQUALS:
				if (!boxes_same(&a, &b))
					PG_RETURN_BOOL(false);
				break;
		}
	}
	ereturn(fcinfo->context, (Datum) 0,
			errcode(ERRCODE_DATA_EXCEPTION),
			errmsg("vexec_postgis: the boxes do not decide"));
}

#define PREFILTER(fn, sc) \
	static Datum fn(PG_FUNCTION_ARGS) { return prefilter(fcinfo, sc); }

PREFILTER(pf_intersects, SC_NO_OVERLAP_FALSE)
PREFILTER(pf_touches, SC_NO_OVERLAP_FALSE)
PREFILTER(pf_overlaps, SC_NO_OVERLAP_FALSE)
PREFILTER(pf_crosses, SC_NO_OVERLAP_FALSE)
PREFILTER(pf_disjoint, SC_NO_OVERLAP_TRUE)
PREFILTER(pf_contains, SC_CONTAINS)
PREFILTER(pf_containsproperly, SC_CONTAINS)
PREFILTER(pf_covers, SC_CONTAINS)
PREFILTER(pf_within, SC_WITHIN)
PREFILTER(pf_coveredby, SC_WITHIN)
PREFILTER(pf_equals, SC_EQUALS)

/*
 * ST_X's and ST_Y's check: the value is a point, which they raise on no
 * other way.  A value the reader does not know fails it, and goes to
 * PostGIS.
 */
Datum
vexec_postgis_is_point(PG_FUNCTION_ARGS)
{
	GsInfo		g;

	if (PG_ARGISNULL(0))
		PG_RETURN_BOOL(false);
	gs_read(PG_GETARG_DATUM(0), &g);
	PG_RETURN_BOOL(g.known && g.type == GS_POINT);
}

/* ---- the declarations ---- */

static const char *const geom[] = {"geometry"};
static const char *const geom_geom[] = {"geometry", "geometry"};

#define LIB "$libdir/postgis-3"
#define BOXOP(name, symbol) {name, 2, geom_geom, symbol, LIB, VEXEC_DECL_NEVER_RAISES, NULL}
#define PREDICATE(name, symbol, fn) {name, 2, geom_geom, symbol, LIB, VEXEC_DECL_PREFILTER, fn}

static const VexecKernelDecl decls[] = {
	BOXOP("geometry_overlaps", "gserialized_overlaps_2d"),	/* && */
	BOXOP("geometry_same", "gserialized_same_2d"),	/* ~= */
	BOXOP("geometry_contains", "gserialized_contains_2d"),	/* ~ */
	BOXOP("geometry_within", "gserialized_within_2d"),	/* @ */
	BOXOP("geometry_left", "gserialized_left_2d"),	/* << */
	BOXOP("geometry_overleft", "gserialized_overleft_2d"),	/* &< */
	BOXOP("geometry_below", "gserialized_below_2d"),	/* <<| */
	BOXOP("geometry_overbelow", "gserialized_overbelow_2d"),	/* &<| */
	BOXOP("geometry_overright", "gserialized_overright_2d"),	/* &> */
	BOXOP("geometry_right", "gserialized_right_2d"),	/* >> */
	BOXOP("geometry_overabove", "gserialized_overabove_2d"),	/* |&> */
	BOXOP("geometry_above", "gserialized_above_2d"),	/* |>> */
	{"st_srid", 1, geom, "LWGEOM_get_srid", LIB, VEXEC_DECL_NEVER_RAISES, NULL},
	{"st_x", 1, geom, "LWGEOM_x_point", LIB, VEXEC_DECL_CHECK, vexec_postgis_is_point},
	{"st_y", 1, geom, "LWGEOM_y_point", LIB, VEXEC_DECL_CHECK, vexec_postgis_is_point},
	PREDICATE("st_intersects", "ST_Intersects", pf_intersects),
	PREDICATE("st_touches", "touches", pf_touches),
	PREDICATE("st_overlaps", "overlaps", pf_overlaps),
	PREDICATE("st_crosses", "crosses", pf_crosses),
	PREDICATE("st_disjoint", "disjoint", pf_disjoint),
	PREDICATE("st_contains", "contains", pf_contains),
	PREDICATE("st_containsproperly", "containsproperly", pf_containsproperly),
	PREDICATE("st_covers", "covers", pf_covers),
	PREDICATE("st_within", "within", pf_within),
	PREDICATE("st_coveredby", "coveredby", pf_coveredby),
	PREDICATE("st_equals", "ST_Equals", pf_equals),
};

static const char *const versions[] = {"3.7.0rc2", NULL};

static const VexecKernelPack pack = {
	.size = sizeof(VexecKernelPack),
	.minor = VEXEC_KERNELS_MINOR,
	.name = "vexec_postgis",
	.extension = "postgis",
	.versions = versions,
	.ndecls = lengthof(decls),
	.decls = decls,
};

/*
 * Registered while the postmaster preloads libraries, on every server, in
 * any order with vexec.  Loaded otherwise -- by a call of its SQL functions
 * -- the pack registers nothing.
 */
void
_PG_init(void)
{
	if (process_shared_preload_libraries_in_progress)
		vexec_register_kernel_pack(&pack);
}

/* ---- for a person and the pack's tests ---- */

static const VexecKernelDecl *
decl_named(const char *name)
{
	int			i;

	for (i = 0; i < (int) lengthof(decls); i++)
		if (strcmp(decls[i].name, name) == 0)
			return &decls[i];
	return NULL;
}

/*
 * vexec_postgis.prefilter(predicate, a, b): the named predicate's
 * prefilter's answer, as vexec takes it -- NULL where it leaves the row to
 * PostGIS.
 */
Datum
vexec_postgis_prefilter(PG_FUNCTION_ARGS)
{
	char	   *name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const VexecKernelDecl *d = decl_named(name);
	ErrorSaveContext escontext = {T_ErrorSaveContext};
	LOCAL_FCINFO(pfcinfo, 2);
	FmgrInfo	flinfo;
	Datum		result;

	if (d == NULL || d->kind != VEXEC_DECL_PREFILTER)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("vexec_postgis has no prefilter for \"%s\"", name)));
	MemSet(&flinfo, 0, sizeof(flinfo));
	flinfo.fn_addr = d->fn;
	flinfo.fn_oid = InvalidOid;
	flinfo.fn_nargs = 2;
	flinfo.fn_mcxt = CurrentMemoryContext;
	InitFunctionCallInfoData(*pfcinfo, &flinfo, 2, PG_GET_COLLATION(), (Node *) &escontext, NULL);
	pfcinfo->args[0] = fcinfo->args[1];
	pfcinfo->args[1] = fcinfo->args[2];
	result = FunctionCallInvoke(pfcinfo);
	if (SOFT_ERROR_OCCURRED(&escontext))
		PG_RETURN_NULL();
	PG_RETURN_DATUM(result);
}

/*
 * vexec_postgis.reader(geometry): what the pack's reader makes of a value --
 * whether it knows it, its type, whether it is empty, its SRID's bytes, and
 * the predicates' box, NULL where it has none.
 */
Datum
vexec_postgis_reader(PG_FUNCTION_ARGS)
{
	GsInfo		g;
	TupleDesc	tupdesc;
	Datum		values[8];
	bool		nulls[8] = {false, false, false, false, false, false, false, false};
	bytea	   *srid = palloc(VARHDRSZ + 3);

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	gs_read(PG_GETARG_DATUM(0), &g);
	SET_VARSIZE(srid, VARHDRSZ + 3);
	memcpy(VARDATA(srid), g.srid, 3);
	values[0] = BoolGetDatum(g.known);
	values[1] = Int32GetDatum((int32) g.type);
	values[2] = BoolGetDatum(g.empty);
	values[3] = PointerGetDatum(srid);
	values[4] = Float8GetDatum(g.xmin);
	values[5] = Float8GetDatum(g.xmax);
	values[6] = Float8GetDatum(g.ymin);
	values[7] = Float8GetDatum(g.ymax);
	if (!g.known)
		nulls[1] = nulls[2] = true;
	if (!g.known || !g.has_box)
		nulls[4] = nulls[5] = nulls[6] = nulls[7] = true;
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * vexec_postgis.declarations(): each function the pack declares, by its name,
 * argument types and C symbol, its declaration, the PostGIS versions it
 * holds for, and whether the pack registered in this server.
 */
Datum
vexec_postgis_declarations(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	const VexecKernelRegistry *reg = vexec_find_kernel_registry();
	Datum		vers[lengthof(versions)];
	int			nvers = 0;
	bool		registered = false;
	int			i;

	InitMaterializedSRF(fcinfo, 0);
	while (versions[nvers] != NULL)
	{
		vers[nvers] = CStringGetTextDatum(versions[nvers]);
		nvers++;
	}
	for (i = 0; reg != NULL && i < reg->npacks; i++)
		registered |= reg->packs[i] == &pack;
	for (i = 0; i < (int) lengthof(decls); i++)
	{
		Datum		values[6];
		bool		nulls[6] = {false, false, false, false, false, false};
		Datum		args[2];
		int			a;

		for (a = 0; a < decls[i].nargs; a++)
			args[a] = CStringGetTextDatum(decls[i].argtypes[a]);
		values[0] = CStringGetTextDatum(decls[i].name);
		values[1] = PointerGetDatum(construct_array_builtin(args, decls[i].nargs, TEXTOID));
		values[2] = CStringGetTextDatum(decls[i].symbol);
		values[3] = CStringGetTextDatum(decls[i].kind == VEXEC_DECL_NEVER_RAISES ? "never raises" :
										decls[i].kind == VEXEC_DECL_CHECK ? "check: a point" :
										"prefilter: the boxes' short-circuit");
		values[4] = PointerGetDatum(construct_array_builtin(vers, nvers, TEXTOID));
		values[5] = BoolGetDatum(registered);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	PG_RETURN_VOID();
}
