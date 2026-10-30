/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * stats.h
 *	  An own file's statistics, as vexec_parquet.files keeps them (stats.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef VPQ_STATS_H
#define VPQ_STATS_H

#include "vexec_parquet.h"

/* How a column's least and greatest values are kept */
typedef enum VpqStatsClass
{
	VPQ_STATS_NONE,				/* NULLs counted alone */
	VPQ_STATS_INT,				/* int64: integers, bool, date, time, timestamps */
	VPQ_STATS_FLOAT,			/* double, NaNs left out and flagged */
	VPQ_STATS_DECIMAL,			/* int128 at the column's scale */
	VPQ_STATS_BYTES				/* bytewise: strings, binaries, uuid */
} VpqStatsClass;

typedef struct VpqRawStat
{
	VpqStatsClass klass;
	int64		nulls;
	bool		has_minmax;
	bool		may_nan;
	int64		imin;
	int64		imax;
	double		dmin;
	double		dmax;
	int128		nmin;
	int128		nmax;
	const char *bmin;
	int			bminlen;
	const char *bmax;
	int			bmaxlen;
} VpqRawStat;

extern VpqStatsClass vpq_stats_class(VpqKind kind);
extern bytea *vpq_stats_build(int nrg, const int64 *rg_rows, int ncols, const AttrNumber *attnums,
							  const VpqKind *kinds, const VpqRawStat *stats, const int64 *bytes,
							  const char *facts, Size facts_len);
extern int	vpq_stats_row_groups(bytea *raw, int64 **rg_rows);

#endif							/* VPQ_STATS_H */
