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

#include "access/relscan.h"
#include "utils/relcache.h"

#include "vexec_source.h"

#include "batch/batch.h"

extern const VexecSourceRoutine *vexec_source_for(Relation rel, const char **how);

/*
 * heap.c: heap's page reader (§3.5.2).  A batch points into at most this
 * many of the pages it was read from, past the scan's current one; the
 * values of the pages past them are copied.
 */
#define VEXEC_HEAP_PINS		8

typedef struct VexecHeapReader VexecHeapReader;
struct VexecSortBound;

extern bool vexec_heap_reader_possible(Relation rel);
extern VexecHeapReader *vexec_heap_reader_begin(TableScanDesc scan, VexecBatch *batch,
												int nattrs, const AttrNumber *attrs,
												bool need_tid);
extern VexecHeapReader *vexec_heap_reader_begin_bitmap(TableScanDesc scan, VexecBatch *batch,
													   int nattrs, const AttrNumber *attrs,
													   bool need_tid, TupleTableSlot *slot);
extern bool vexec_heap_reader_next(VexecHeapReader *hr, VexecBatch *in);
extern void vexec_heap_reader_rescan(VexecHeapReader *hr);
extern void vexec_heap_reader_end(VexecHeapReader *hr);
extern int64 vexec_heap_reader_pages(VexecHeapReader *hr);
extern void vexec_heap_reader_bitmap_pages(VexecHeapReader *hr, uint64 *exact, uint64 *lossy);
extern void vexec_heap_reader_set_bound(VexecHeapReader *hr, struct VexecSortBound *bound);

#endif							/* VEXEC_SOURCE_SOURCE_H */
