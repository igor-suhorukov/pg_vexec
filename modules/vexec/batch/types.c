/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * types.c
 *	  A type's layouts in each format: the capability oracle's answer for
 *	  types (pg_vector_executor.md §3.3.2, §3.4).
 *
 * Every type has a layout in each format: one with no layout of its own is
 * held as Datums, or as views of its bytes.  What a type can be held as
 * depends on its class:
 *
 *	bool			a byte a value (PostgreSQL), or a bit (Arrow)
 *	by-value		the type's width, the same bytes in both
 *	date			int32 days from 2000-01-01, or from 1970-01-01
 *	timestamp[tz]	int64 µs from 2000-01-01, or from 1970-01-01
 *	interval		PostgreSQL's {µs, days, months}, or {months, days, ns}
 *	by-reference	typlen bytes at PostgreSQL's array stride, or packed
 *	numeric			scaled int64 or int128 where the typmod bounds it to 18
 *					or 38 digits, else as the other varlena types
 *	varlena			Datums, views, or offsets
 *
 * The PostgreSQL format picks the first of each, and holds varlena values
 * as Datums; the Arrow format picks the second, and holds them as views.
 * The four per-structure settings override one class each (§3.4.4).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "vexec.h"
#include "batch/batch.h"

/*
 * The digits a numeric typmod bounds a value to, and the scale it stores
 * every finite value at: coercion to the typmod gives every finite value
 * that scale, or 0 where the typmod's is negative
 * (PG19:src/backend/utils/adt/numeric.c:1266-1289).  The typmod's layout is
 * ((precision << 16) | (scale & 0x7ff)) + VARHDRSZ, its scale an 11-bit
 * two's-complement number (numeric.c:874-928, whose decoders are static).
 */
static void
numeric_typmod_bounds(int32 typmod, int16 *digits, int16 *scale)
{
	int32		precision;
	int32		tscale;

	*digits = -1;
	*scale = 0;
	if (typmod < (int32) VARHDRSZ)
		return;
	precision = ((typmod - VARHDRSZ) >> 16) & 0xffff;
	tscale = (((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;
	*digits = (int16) (precision + (tscale < 0 ? -tscale : 0));
	*scale = (int16) Max(tscale, 0);
}

/*
 * Whether a varlena type's functions read a short header as it is, through
 * the _PP accessors (PG19:src/include/fmgr.h:289-295): text, varchar,
 * bpchar and bytea do; json is text.  numeric, jsonb and arrays copy a
 * short-header value on every call (numeric.h:63-67; jsonb.h:400-404;
 * array.h:261-263), so vexec writes them, and every type it does not know,
 * with a 4-byte header at the type's alignment (§3.4.2).
 */
static bool
type_takes_short_header(Oid basetype)
{
	switch (basetype)
	{
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case BYTEAOID:
		case JSONOID:
			return true;
		default:
			return false;
	}
}

/* Text whose bytes Arrow's utf8 may hold: in a UTF8 database (Schema.fbs:170-172). */
static bool
type_is_utf8_text(Oid basetype)
{
	switch (basetype)
	{
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
		case JSONOID:
			return GetDatabaseEncoding() == PG_UTF8;
		default:
			return false;
	}
}

/*
 * A column's type: what its layouts need to know, looked up once.  A
 * domain is held as its base type, and keeps its own OID for export.
 */
VexecType *
vexec_type_make(Oid typid, int32 typmod, Oid collation)
{
	VexecType  *t = palloc0(sizeof(VexecType));
	HeapTuple	tup;
	Form_pg_type typ;

	t->typid = typid;
	t->typmod = typmod;
	t->collation = collation;
	t->basetypmod = typmod;
	t->basetype = getBaseTypeAndTypmod(typid, &t->basetypmod);
	get_typlenbyvalalign(t->basetype, &t->typlen, &t->typbyval, &t->typalign);
	t->alignby = typalign_to_alignby(t->typalign);

	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for type %u", typid);
	typ = (Form_pg_type) GETSTRUCT(tup);
	t->name = pstrdup(NameStr(typ->typname));
	t->nspname = get_namespace_name(typ->typnamespace);
	ReleaseSysCache(tup);

	if (t->basetype == BOOLOID)
		t->tclass = VEXEC_TC_BOOL;
	else if (t->basetype == DATEOID)
		t->tclass = VEXEC_TC_DATE;
	else if (t->basetype == TIMESTAMPOID || t->basetype == TIMESTAMPTZOID)
		t->tclass = VEXEC_TC_TIMESTAMP;
	else if (t->basetype == INTERVALOID)
		t->tclass = VEXEC_TC_INTERVAL;
	else if (t->basetype == NUMERICOID)
		t->tclass = VEXEC_TC_NUMERIC;
	else if (t->typbyval)
		t->tclass = VEXEC_TC_FIXED;
	else if (t->typlen > 0)
		t->tclass = VEXEC_TC_BYREF;
	else if (t->typlen == -2)
		t->tclass = VEXEC_TC_CSTRING;
	else
		t->tclass = VEXEC_TC_VARLENA;

	/*
	 * A fixed-length by-reference value is passed as a pointer to its bytes,
	 * and arrays place element i + 1 at TYPEALIGN(typalign, end of i)
	 * (PG19:src/backend/utils/adt/arrayfuncs.c:3546-3547): 16 for timetz's
	 * 12 bytes, 8 for macaddr's 6.
	 */
	if (t->tclass == VEXEC_TC_BYREF || t->tclass == VEXEC_TC_INTERVAL)
		t->pg_stride = (int32) TYPEALIGN(t->alignby, t->typlen);
	else if (t->typlen > 0)
		t->pg_stride = t->typlen;

	if (t->tclass == VEXEC_TC_NUMERIC)
	{
		/*
		 * The scaled layout is for numerics bounded to 38 digits, at a scale
		 * of at most 38: a typmod may give a scale above its precision
		 * (numeric(3,10)), and one above 38 is left to the varlena layouts.
		 */
		numeric_typmod_bounds(t->basetypmod, &t->digits, &t->scale);
		if (t->scale <= 38 && t->digits > 0 && t->digits <= 18)
			t->scaled_width = 8;
		else if (t->scale <= 38 && t->digits > 0 && t->digits <= 38)
			t->scaled_width = 16;
	}

	t->short_ok = type_takes_short_header(t->basetype);
	t->utf8 = type_is_utf8_text(t->basetype);
	return t;
}

/*
 * The format and the per-structure settings in effect now.  Under
 * vexec.debug_layout_seed each of the four per-structure settings is drawn
 * anew on every call, from a stream the seed starts: the differential
 * runner's fourth session (§6.1), whose seed the run prints.
 */
VexecLayoutConfig
vexec_layout_config(void)
{
	VexecLayoutConfig cfg;

	cfg.format = (uint8) vexec_batch_format;
	cfg.varlena = (uint8) vexec_batch_varlena_layout;
	cfg.boolean = (uint8) vexec_batch_bool_layout;
	cfg.temporal = (uint8) vexec_batch_temporal_layout;
	cfg.numeric = (uint8) vexec_batch_numeric_layout;

	if (vexec_debug_layout_seed != 0)
	{
		static int	seeded = 0;
		static uint64 state;
		uint64		r;

		if (seeded != vexec_debug_layout_seed)
		{
			state = (uint64) vexec_debug_layout_seed * UINT64CONST(0x9E3779B97F4A7C15);
			seeded = vexec_debug_layout_seed;
		}
		/* splitmix64 */
		state += UINT64CONST(0x9E3779B97F4A7C15);
		r = state;
		r = (r ^ (r >> 30)) * UINT64CONST(0xBF58476D1CE4E5B9);
		r = (r ^ (r >> 27)) * UINT64CONST(0x94D049BB133111EB);
		r ^= r >> 31;
		cfg.varlena = (uint8) (r % 4);
		cfg.boolean = (uint8) ((r >> 8) % 3);
		cfg.temporal = (uint8) ((r >> 16) % 3);
		cfg.numeric = (uint8) ((r >> 24) % 3);
	}
	return cfg;
}

static void
shape_set(VexecShape *s, int layout, bool arrow_values, int width, int stride, int scale)
{
	memset(s, 0, sizeof(VexecShape));
	s->layout = (uint8) layout;
	s->arrow_values = arrow_values;
	s->width = width;
	s->stride = stride;
	s->scale = (int16) scale;
}

static int
varlena_layout(const VexecLayoutConfig *cfg)
{
	switch (cfg->varlena)
	{
		case VEXEC_VARLENA_DATUM:
			return VEXEC_DATUM;
		case VEXEC_VARLENA_VIEW:
			return VEXEC_VIEW;
		case VEXEC_VARLENA_OFFSETS:
			return VEXEC_OFFSETS;
		default:
			return cfg->format == VEXEC_FORMAT_ARROW ? VEXEC_VIEW : VEXEC_DATUM;
	}
}

static bool
temporal_arrow(const VexecLayoutConfig *cfg)
{
	switch (cfg->temporal)
	{
		case VEXEC_TEMPORAL_POSTGRES:
			return false;
		case VEXEC_TEMPORAL_ARROW:
			return true;
		default:
			return cfg->format == VEXEC_FORMAT_ARROW;
	}
}

/*
 * The shape a format and the per-structure settings give a type.  A batch
 * whose values that shape cannot hold keeps PostgreSQL's in that column
 * (convert.c says when).
 */
void
vexec_type_shape(const VexecType *type, const VexecLayoutConfig *cfg, VexecShape *shape)
{
	switch (type->tclass)
	{
		case VEXEC_TC_BOOL:
			{
				bool		bit;

				if (cfg->boolean == VEXEC_BOOL_BYTE)
					bit = false;
				else if (cfg->boolean == VEXEC_BOOL_BIT)
					bit = true;
				else
					bit = cfg->format == VEXEC_FORMAT_ARROW;
				shape_set(shape, bit ? VEXEC_BIT_BOOL : VEXEC_BYTE_BOOL, false, 1, 1, 0);
				break;
			}
		case VEXEC_TC_FIXED:
			shape_set(shape, VEXEC_FIXED, false, type->typlen, type->typlen, 0);
			break;
		case VEXEC_TC_DATE:
		case VEXEC_TC_TIMESTAMP:
			shape_set(shape, VEXEC_FIXED, temporal_arrow(cfg), type->typlen, type->typlen, 0);
			break;
		case VEXEC_TC_INTERVAL:
			shape_set(shape, VEXEC_FIXED, temporal_arrow(cfg), 16, 16, 0);
			break;
		case VEXEC_TC_BYREF:
			shape_set(shape, VEXEC_FIXED, false, type->typlen,
					  cfg->format == VEXEC_FORMAT_ARROW ? type->typlen : type->pg_stride, 0);
			break;
		case VEXEC_TC_NUMERIC:
			if (type->scaled_width > 0 && cfg->numeric != VEXEC_NUMERIC_VARLENA)
				shape_set(shape, VEXEC_SCALED, false, type->scaled_width, type->scaled_width,
						  type->scale);
			else
				shape_set(shape, varlena_layout(cfg), false, 0, 0, 0);
			break;
		case VEXEC_TC_VARLENA:
		case VEXEC_TC_CSTRING:
			shape_set(shape, varlena_layout(cfg), false, 0, 0, 0);
			break;
	}
}

/*
 * The shape rows are first written in (rows.c): the PostgreSQL format's,
 * with numeric as Datums, which every other shape converts from.
 */
void
vexec_type_build_shape(const VexecType *type, VexecShape *shape)
{
	VexecLayoutConfig cfg = {VEXEC_FORMAT_POSTGRES, VEXEC_VARLENA_DATUM, VEXEC_BOOL_BYTE,
	VEXEC_TEMPORAL_POSTGRES, VEXEC_NUMERIC_VARLENA};

	vexec_type_shape(type, &cfg, shape);
}

/* Every shape a type can be held in: what the conversions are tested over. */
int
vexec_type_shapes(const VexecType *type, VexecShape *shapes, int max)
{
	int			n = 0;

#define ADD(layout, arrow, width, stride, scale) \
	do { \
		if (n < max) \
			shape_set(&shapes[n], (layout), (arrow), (width), (stride), (scale)); \
		n++; \
	} while (0)

	switch (type->tclass)
	{
		case VEXEC_TC_BOOL:
			ADD(VEXEC_BYTE_BOOL, false, 1, 1, 0);
			ADD(VEXEC_BIT_BOOL, false, 1, 1, 0);
			break;
		case VEXEC_TC_FIXED:
			ADD(VEXEC_FIXED, false, type->typlen, type->typlen, 0);
			break;
		case VEXEC_TC_DATE:
		case VEXEC_TC_TIMESTAMP:
			ADD(VEXEC_FIXED, false, type->typlen, type->typlen, 0);
			ADD(VEXEC_FIXED, true, type->typlen, type->typlen, 0);
			break;
		case VEXEC_TC_INTERVAL:
			ADD(VEXEC_FIXED, false, 16, 16, 0);
			ADD(VEXEC_FIXED, true, 16, 16, 0);
			break;
		case VEXEC_TC_BYREF:
			ADD(VEXEC_FIXED, false, type->typlen, type->pg_stride, 0);
			if (type->pg_stride != type->typlen)
				ADD(VEXEC_FIXED, false, type->typlen, type->typlen, 0);
			break;
		case VEXEC_TC_NUMERIC:
			if (type->scaled_width > 0)
				ADD(VEXEC_SCALED, false, type->scaled_width, type->scaled_width, type->scale);
			pg_fallthrough;
		case VEXEC_TC_VARLENA:
		case VEXEC_TC_CSTRING:
			ADD(VEXEC_DATUM, false, 0, 0, 0);
			ADD(VEXEC_VIEW, false, 0, 0, 0);
			ADD(VEXEC_OFFSETS, false, 0, 0, 0);
			break;
	}
#undef ADD
	return n;
}

bool
vexec_shape_equal(const VexecShape *a, const VexecShape *b)
{
	return a->layout == b->layout &&
		a->arrow_values == b->arrow_values &&
		a->width == b->width &&
		a->stride == b->stride &&
		a->scale == b->scale;
}

const char *
vexec_layout_name(int layout)
{
	switch (layout)
	{
		case VEXEC_FIXED:
			return "fixed";
		case VEXEC_BYTE_BOOL:
			return "byte";
		case VEXEC_BIT_BOOL:
			return "bit";
		case VEXEC_SCALED:
			return "scaled";
		case VEXEC_DATUM:
			return "datum";
		case VEXEC_VIEW:
			return "view";
		case VEXEC_OFFSETS:
			return "offsets";
	}
	return "?";
}

const char *
vexec_type_class_name(int tclass)
{
	switch (tclass)
	{
		case VEXEC_TC_BOOL:
			return "bool";
		case VEXEC_TC_FIXED:
			return "by-value";
		case VEXEC_TC_DATE:
			return "date";
		case VEXEC_TC_TIMESTAMP:
			return "timestamp";
		case VEXEC_TC_INTERVAL:
			return "interval";
		case VEXEC_TC_BYREF:
			return "by-reference";
		case VEXEC_TC_NUMERIC:
			return "numeric";
		case VEXEC_TC_VARLENA:
			return "varlena";
		case VEXEC_TC_CSTRING:
			return "cstring";
	}
	return "?";
}

/* A shape as EXPLAIN and the tests name it: "fixed(12/16)", "scaled(8,2)". */
char *
vexec_shape_name(const VexecType *type, const VexecShape *s)
{
	switch (s->layout)
	{
		case VEXEC_FIXED:
			if (type->tclass == VEXEC_TC_DATE || type->tclass == VEXEC_TC_TIMESTAMP ||
				type->tclass == VEXEC_TC_INTERVAL)
				return psprintf("fixed(%d,%s)", s->width, s->arrow_values ? "arrow" : "postgres");
			if (s->stride != s->width)
				return psprintf("fixed(%d/%d)", s->width, s->stride);
			return psprintf("fixed(%d)", s->width);
		case VEXEC_SCALED:
			return psprintf("scaled(%d,%d)", s->width, s->scale);
		default:
			return pstrdup(vexec_layout_name(s->layout));
	}
}
