/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * convert.h
 *	  Values between PostgreSQL's representations and Arrow's (convert.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef VPQ_CONVERT_H
#define VPQ_CONVERT_H

#include "fmgr.h"

/* Days and microseconds from 1970-01-01 to 2000-01-01, PostgreSQL's epoch */
#define VPQ_EPOCH_DAYS		10957
#define VPQ_EPOCH_USECS		INT64CONST(946684800000000)

extern bool vpq_numeric_to_int128(Datum d, int scale, int128 *out);
extern Datum vpq_int128_to_numeric(int128 value, int scale);
extern Datum vpq_numeric_coerce(Datum value, int32 typmod);
extern char *vpq_int128_out(int128 value, int scale);
extern int128 vpq_pow10_128(int n);

#endif							/* VPQ_CONVERT_H */
