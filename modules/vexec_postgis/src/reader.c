/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * reader.c
 *	  vexec_postgis's reader of PostGIS's serialized geometries, written from
 *	  the format's description, liblwgeom/gserialized.txt
 *	  (pg_vector_executor.md §3.17).
 *
 * PostGIS installs no header of liblwgeom's, and is under the GPL; so this
 * reads the serialized form with code of its own, from the description, and
 * PostGIS's code was read only to check behaviour, as principle 10 of the
 * plan has it.  What the description gives, and this reads:
 *
 *	- the header: a varlena size, 3 bytes of SRID, a byte of flags --
 *	  HasZ 0x01, HasM 0x02, HasBBox 0x04, IsGeodetic 0x08, a reserved 0x10,
 *	  HasExtendedFlags 0x20, and the version bits 0x40 and 0x80;
 *	- the v2 serialization's optional elements: 8 bytes of extended flags,
 *	  of which IsSolid (0x01) alone is described, then a box of
 *	  2 * ndims floats, xmin, xmax, ymin, ymax first;
 *	- the body, each element a uint32 type and a uint32 count of points,
 *	  rings or members: a point's and a line's and a circular string's
 *	  doubles, ndims of them a point, after them; a polygon's rings' counts
 *	  of points; a collection's members, each serialized as a geometry.
 *
 * The rule (§3.17).  The reader decides nothing for a version, a flag or a
 * type the description does not give, or for a value it would have to read
 * past its end: such a value is "not known", and the pack leaves its row to
 * PostGIS's own function.  Nor does it give a box with a NaN in it, cached
 * or of the coordinates, so that no NaN reaches a comparison.  So:
 *
 *	- v2 alone: the version bits 0x40 set and 0x80 clear, as PostGIS writes
 *	  every geometry since its version 3 (checked in its serializer,
 *	  gserialized2.c); v1 values, which only an older PostGIS wrote, are not
 *	  read;
 *	- not the reserved flag (the description's IsLightPoint, which the code
 *	  does not have), not IsGeodetic -- geography's -- and no extended flag
 *	  but IsSolid;
 *	- the types the description gives a body to: point, line, polygon,
 *	  circular string, and the collections -- multipoint, multiline,
 *	  multipolygon, geometry collection, compound curve.  Not NURBS curves,
 *	  type 16, which it does not describe, nor triangles, TINs, polyhedral
 *	  surfaces, curve polygons, multicurves or multisurfaces.
 *
 * What it gives (GsInfo):
 *
 *	- whether the geometry is empty, as PostGIS's predicates test it first:
 *	  a point, a line or a circular string with no points; a polygon with
 *	  no rings, or none with a point; a collection with no members, or with
 *	  one member, itself empty.  A collection whose first member is not
 *	  empty is not; of any other the reader cannot tell cheaply, and says
 *	  it does not know;
 *	- the box PostGIS's predicates short-circuit on (gserialized_get_gbox_p()
 *	  in PostGIS, a fact of its behaviour): the cached box, its floats
 *	  widened to doubles; or, for the geometries that keep none -- a point,
 *	  a line of one or two points, a multipoint or a multiline of one
 *	  member of those -- the box of its coordinates, rounded outward to
 *	  floats as PostGIS rounds a box it computes: the largest float not
 *	  above the minimum and the smallest not below the maximum, each bounded
 *	  by FLT_MAX, as gserialized_gist_2d.c:203-206 shows PostGIS doing.
 *
 * Every offset is checked against the bytes at hand: a toasted value is
 * read through a slice of its first GS_PREFIX bytes, enough for a header,
 * a box and the first elements of a body; what lies past them is not read.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "fmgr.h"
#include "varatt.h"

#include "reader.h"

/* The flags (gserialized.txt, FLAGS (V2)) */
#define GF_Z			0x01
#define GF_M			0x02
#define GF_BBOX			0x04
#define GF_GEODETIC		0x08
#define GF_RESERVED		0x10
#define GF_EXTENDED		0x20
#define GF_VERSION		0xC0	/* VersionBit1 and VersionBit2 */
#define GF_V2			0x40
#define GX_SOLID		UINT64CONST(0x01)	/* the extended flags' IsSolid */

/* the bytes of a toasted value read: the header, a box, a body's start */
#define GS_PREFIX		128

typedef struct Bytes
{
	const uint8 *p;
	Size		len;
	int			ndims;
} Bytes;

static bool
rd_u32(const Bytes *b, Size off, uint32 *v)
{
	if (off > b->len || b->len - off < sizeof(uint32))
		return false;
	memcpy(v, b->p + off, sizeof(uint32));
	return true;
}

static bool
rd_u64(const Bytes *b, Size off, uint64 *v)
{
	if (off > b->len || b->len - off < sizeof(uint64))
		return false;
	memcpy(v, b->p + off, sizeof(uint64));
	return true;
}

static bool
rd_f64(const Bytes *b, Size off, double *v)
{
	if (off > b->len || b->len - off < sizeof(double))
		return false;
	memcpy(v, b->p + off, sizeof(double));
	return true;
}

/* The largest float not above d, bounded by FLT_MAX. */
static double
round_down(double d)
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

/* The smallest float not below d, bounded by FLT_MAX. */
static double
round_up(double d)
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

static bool
is_collection(uint32 type)
{
	switch (type)
	{
		case GS_MULTIPOINT:
		case GS_MULTILINE:
		case GS_MULTIPOLYGON:
		case GS_COLLECTION:
		case GS_COMPOUND:
			return true;
		default:
			return false;
	}
}

static bool
is_described(uint32 type)
{
	switch (type)
	{
		case GS_POINT:
		case GS_LINE:
		case GS_POLYGON:
		case GS_CIRCSTRING:
			return true;
		default:
			return is_collection(type);
	}
}

/*
 * Whether the element at off, of this type and count, is empty: 1, 0, or -1
 * where the reader cannot tell.  A collection's first member alone is
 * looked at, and no collection's within it.
 */
static int
emptiness(const Bytes *b, Size off, uint32 type, uint32 count, bool top)
{
	uint32		i;

	if (!is_described(type))
		return -1;
	switch (type)
	{
		case GS_POINT:
		case GS_LINE:
		case GS_CIRCSTRING:
			return count == 0 ? 1 : 0;
		case GS_POLYGON:
			/* empty without rings, or with rings of no points */
			for (i = 0; i < count; i++)
			{
				uint32		npoints;

				if (!rd_u32(b, off + 8 + (Size) i * 4, &npoints))
					return -1;
				if (npoints > 0)
					return 0;
			}
			return 1;
		default:
			{
				uint32		mtype;
				uint32		mcount;
				int			e;

				if (count == 0)
					return 1;
				if (!top || !rd_u32(b, off + 8, &mtype) || !rd_u32(b, off + 12, &mcount) ||
					is_collection(mtype))
					return -1;
				e = emptiness(b, off + 8, mtype, mcount, false);
				if (e == 0)
					return 0;
				if (e == 1 && count == 1)
					return 1;
				return -1;
			}
	}
}

/*
 * The box of npoints points at off, rounded outward; false where a
 * coordinate is NaN or past the bytes at hand.
 */
static bool
points_box(const Bytes *b, Size off, uint32 npoints, GsInfo *info)
{
	double		xmin = 0,
				xmax = 0,
				ymin = 0,
				ymax = 0;
	uint32		i;

	for (i = 0; i < npoints; i++)
	{
		Size		at = off + (Size) i * b->ndims * sizeof(double);
		double		x;
		double		y;

		if (!rd_f64(b, at, &x) || !rd_f64(b, at + sizeof(double), &y) || isnan(x) || isnan(y))
			return false;
		if (i == 0 || x < xmin)
			xmin = x;
		if (i == 0 || x > xmax)
			xmax = x;
		if (i == 0 || y < ymin)
			ymin = y;
		if (i == 0 || y > ymax)
			ymax = y;
	}
	info->xmin = round_down(xmin);
	info->xmax = round_up(xmax);
	info->ymin = round_down(ymin);
	info->ymax = round_up(ymax);
	return true;
}

/*
 * The box of a geometry that keeps none: a point, a line of one or two
 * points, a multipoint or a multiline of one such member.  False for any
 * other, which keeps its box where PostGIS wrote it.
 */
static bool
computed_box(const Bytes *b, Size off, uint32 type, uint32 count, GsInfo *info)
{
	uint32		mtype;
	uint32		mcount;

	switch (type)
	{
		case GS_POINT:
			return count == 1 && points_box(b, off + 8, 1, info);
		case GS_LINE:
			return (count == 1 || count == 2) && points_box(b, off + 8, count, info);
		case GS_MULTIPOINT:
		case GS_MULTILINE:
			if (count != 1 || !rd_u32(b, off + 8, &mtype) || !rd_u32(b, off + 12, &mcount))
				return false;
			if (type == GS_MULTIPOINT && mtype == GS_POINT && mcount == 1)
				return points_box(b, off + 16, 1, info);
			if (type == GS_MULTILINE && mtype == GS_LINE && (mcount == 1 || mcount == 2))
				return points_box(b, off + 16, mcount, info);
			return false;
		default:
			return false;
	}
}

void
gs_read(Datum d, GsInfo *info)
{
	struct varlena *v = (struct varlena *) DatumGetPointer(d);
	Bytes		b;
	uint8		flags;
	Size		off = 8;
	uint32		type;
	uint32		count;
	int			empty;

	memset(info, 0, sizeof(GsInfo));
	if (VARATT_IS_EXTENDED(v))
		v = (struct varlena *) PG_DETOAST_DATUM_SLICE(d, 0, GS_PREFIX);
	b.p = (const uint8 *) v;
	b.len = VARSIZE(v);
	if (b.len < 8)
		return;
	memcpy(info->srid, b.p + 4, 3);
	flags = b.p[7];
	if ((flags & GF_VERSION) != GF_V2 || (flags & (GF_RESERVED | GF_GEODETIC)) != 0)
		return;
	if (flags & GF_EXTENDED)
	{
		uint64		ext;

		if (!rd_u64(&b, off, &ext) || (ext & ~GX_SOLID) != 0)
			return;
		off += sizeof(uint64);
	}
	b.ndims = 2 + ((flags & GF_Z) ? 1 : 0) + ((flags & GF_M) ? 1 : 0);
	if (flags & GF_BBOX)
	{
		float		box[4];
		Size		size = 2 * b.ndims * sizeof(float);

		if (off > b.len || b.len - off < size)
			return;
		memcpy(box, b.p + off, sizeof(box));
		if (isnan(box[0]) || isnan(box[1]) || isnan(box[2]) || isnan(box[3]))
			return;
		info->xmin = box[0];
		info->xmax = box[1];
		info->ymin = box[2];
		info->ymax = box[3];
		info->has_box = true;
		off += size;
	}
	if (!rd_u32(&b, off, &type) || !rd_u32(&b, off + 4, &count) || !is_described(type))
		return;
	empty = emptiness(&b, off, type, count, true);
	if (empty < 0)
		return;
	info->type = type;
	info->empty = empty == 1;
	if (!info->empty && !info->has_box)
		info->has_box = computed_box(&b, off, type, count, info);
	info->known = true;
}
