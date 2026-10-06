/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * arrays.h
 *	  Arrow arrays the endpoint makes itself (arrays.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_FLIGHT_ARRAYS_H
#define VEXEC_FLIGHT_ARRAYS_H

#include "arrow_abi.h"

typedef struct ArrowCol
{
	struct ArrowSchema schema;
	struct ArrowArray array;
} ArrowCol;

extern ArrowCol *arrays_utf8(const char *name, bool nullable, const char *const *values, int64 n);
extern ArrowCol *arrays_int32(const char *name, bool nullable, const int32 *values,
							  const bool *nulls, int64 n);
extern ArrowCol *arrays_uint32(const char *name, bool nullable, const uint32 *values,
							   const bool *nulls, int64 n);
extern ArrowCol *arrays_int64(const char *name, bool nullable, const int64 *values,
							  const bool *nulls, int64 n);
extern ArrowCol *arrays_bool(const char *name, bool nullable, const bool *values,
							 const bool *nulls, int64 n);
extern ArrowCol *arrays_list_utf8(const char *name, bool nullable,
								  const char *const *const *lists, const int *lens, int64 n);
extern ArrowCol *arrays_empty_map_int32_list(const char *name, bool nullable);
extern ArrowCol *arrays_dense_union(const char *name, ArrowCol **children, int nchildren,
									const int8 *type_ids, const int32 *offsets, int64 n);
extern void arrays_batch(ArrowCol **cols, int ncols, int64 n,
						 struct ArrowSchema *schema, struct ArrowArray *array);

#endif							/* VEXEC_FLIGHT_ARRAYS_H */
