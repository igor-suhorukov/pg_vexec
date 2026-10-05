/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * source.h
 *	  Where a scan's batches come from (pg_vector_executor.md §3.5).
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_SOURCE_SOURCE_H
#define VEXEC_SOURCE_SOURCE_H

#include "utils/relcache.h"

#include "vexec_source.h"

extern const VexecSourceRoutine *vexec_source_for(Relation rel, const char **how);

#endif							/* VEXEC_SOURCE_SOURCE_H */
