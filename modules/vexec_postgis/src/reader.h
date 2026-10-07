/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * reader.h
 *	  What vexec_postgis reads of a serialized PostGIS geometry, with code
 *	  of its own written from the format's description, PostGIS's
 *	  liblwgeom/gserialized.txt (pg_vector_executor.md §3.17).  No PostGIS
 *	  header is included and no PostGIS code copied.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_POSTGIS_READER_H
#define VEXEC_POSTGIS_READER_H

#include "fmgr.h"

/* Geometry types, as numbers (liblwgeom.h.in) */
#define GS_POINT				1
#define GS_LINE					2
#define GS_POLYGON				3
#define GS_MULTIPOINT			4
#define GS_MULTILINE			5
#define GS_MULTIPOLYGON			6
#define GS_COLLECTION			7
#define GS_CIRCSTRING			8
#define GS_COMPOUND				9
#define GS_CURVEPOLY			10
#define GS_MULTICURVE			11
#define GS_MULTISURFACE			12
#define GS_POLYHEDRALSURFACE	13
#define GS_TRIANGLE				14
#define GS_TIN					15

/*
 * What a geometry's value told the reader.  Where known is false, nothing
 * else is to be relied on: a version, a flag or a type the reader does not
 * know, an offset past the value, or a cached box of NaN.  A geometry that
 * keeps no box and has a NaN coordinate is known, without a box.
 */
typedef struct GsInfo
{
	bool		known;
	uint8		srid[3];		/* as stored: equal bytes, equal SRIDs */
	uint32		type;
	bool		empty;
	bool		has_box;		/* box holds the predicates' box */
	double		xmin;
	double		xmax;
	double		ymin;
	double		ymax;
} GsInfo;

/*
 * Read a geometry Datum, as stored or detoasted: its header, its cached
 * box, and as much of its body as says whether it is empty and, for the
 * geometries that keep no box, what the predicates' box is.  Never raises
 * on any value: a toasted value is read through a slice of its first
 * bytes.
 */
extern void gs_read(Datum d, GsInfo *info);

#endif							/* VEXEC_POSTGIS_READER_H */
