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
 * heap.c: heap's page reader (§3.5.2).  A batch points into the pages it was
 * read from, past the scan's current one, as many as its share of the pins
 * the buffer manager allows the backend where its scan has no ring of
 * buffers, and this many where it has (X2, §3.18); the values of the pages
 * past them are copied.
 */
#define VEXEC_HEAP_PINS		8

typedef struct VexecHeapReader VexecHeapReader;
struct VexecSortBound;

/*
 * A qual heap's page reader checks on a tuple's attribute as its walk
 * reaches it, before the tuple is deformed (X3, §3.18; prefilter.c builds
 * them).  A tuple a check rejects is one the qual rejects; a value the
 * reader cannot read as it lies passes.
 */
typedef enum VexecHeapCheckKind
{
	VEXEC_CHECK_INT,			/* an integer, oid, bool, date or timestamp
								 * compared with constants */
	VEXEC_CHECK_BYTES,			/* text or bytea equal to one of constants */
	VEXEC_CHECK_LIKE,			/* text LIKE a literal pattern */
	VEXEC_CHECK_NULL			/* IS NULL */
} VexecHeapCheckKind;

typedef struct VexecHeapCheck
{
	AttrNumber	attnum;
	int16		attlen;
	VexecHeapCheckKind kind;
	bool		negate;			/* <>, NOT LIKE, IS NOT NULL */
	bool		is_unsigned;	/* INT: an oid */
	int			cmp;			/* INT: COMPARE_EQ, _LT, _LE, _GT, _GE */
	int			like_kind;		/* LIKE: VEXEC_LIKE_* (expr.h) */
	int			nvalues;		/* INT and BYTES: equal to any of them */
	int64	   *ints;
	const char **bytes;			/* BYTES, and LIKE's literal */
	int		   *lens;
} VexecHeapCheck;

extern VexecHeapCheck *vexec_heap_checks(Relation rel, Index scanrelid, List *quals,
										 int *ncheck, int *nquals);

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
extern void vexec_heap_reader_pin_stats(VexecHeapReader *hr, int *maxpins, int64 *copied);
extern void vexec_heap_reader_set_checks(VexecHeapReader *hr, VexecHeapCheck *checks,
										 int nchecks);
extern int64 vexec_heap_reader_prefiltered(VexecHeapReader *hr);
extern void vexec_heap_reader_set_bound(VexecHeapReader *hr, struct VexecSortBound *bound);
struct VexecJoinFilter;
extern void vexec_heap_reader_set_join_filter(VexecHeapReader *hr, struct VexecJoinFilter *jf,
											  AttrNumber attno);

#endif							/* VEXEC_SOURCE_SOURCE_H */
