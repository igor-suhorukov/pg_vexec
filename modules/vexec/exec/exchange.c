/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * exchange.c
 *	  The exchange area: blocks of the main shared memory that a parallel
 *	  aggregation's partitions are cut from before any DSM segment (M1 of
 *	  pg_vector_executor.md §3.21).
 *
 * D2's partial groups go to their final stage through shared memory
 * (vecrepart.c), cut from DSM segments each participant makes.  Every page
 * of such a segment is reserved and zeroed as it is made -- PostgreSQL's
 * POSIX segments are posix_fallocate()d -- then faulted in by its writer and
 * by the participant that reads it, and given back to the system once read:
 * on ClickBench's Q33 over MergeTree posix_fallocate() alone took 12% of the
 * workers' cycles, and MADV_REMOVE 5% more.
 *
 * The area is vexec.exchange_memory of the main shared memory, requested as
 * the postmaster starts and never given back.  Its pages are faulted in once
 * in the server's life; a process that touches one later maps it, a fault
 * with no zeroing -- or none, where the main segment has huge pages, whose
 * mapping a forked process inherits whole.  It is cut into blocks of
 * VEXEC_EXCHANGE_BLOCK on a free list under a spinlock, the block freed
 * last given first: a participant claims one where it would have made a
 * segment, and the parallel query's last process to let go of its DSM gives
 * its blocks back (vecrepart.c).  With none to be had a participant makes a
 * segment, as without the area.  0, the default, makes none.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/spin.h"

#include "vexec.h"
#include "exec/exec.h"

/* A block's place in the area: its start aligned so, past the free list. */
#define VEXEC_EXCHANGE_ALIGN	((Size) 2 * 1024 * 1024)

typedef struct ExchangeArea
{
	slock_t		mutex;
	int32		nblocks;
	int32		nfree;
	int32		free_head;		/* -1: none */
	int32		next[FLEXIBLE_ARRAY_MEMBER];	/* a free block's next; -2 for one
												 * claimed */
} ExchangeArea;

static ExchangeArea *area = NULL;
static char *area_data = NULL;
static Size area_bytes = 0;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static int32
area_nblocks(void)
{
	return (int32) (((Size) vexec_exchange_memory * 1024 * 1024) / VEXEC_EXCHANGE_BLOCK);
}

static Size
area_size(int32 nblocks)
{
	Size		head = add_size(offsetof(ExchangeArea, next), mul_size(nblocks, sizeof(int32)));

	return add_size(add_size(head, VEXEC_EXCHANGE_ALIGN),
					mul_size(nblocks, VEXEC_EXCHANGE_BLOCK));
}

static void
exchange_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	if (area_nblocks() > 0)
		RequestAddinShmemSpace(area_size(area_nblocks()));
}

static void
exchange_shmem_startup(void)
{
	int32		nblocks = area_nblocks();
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();
	if (nblocks <= 0)
		return;
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	area = ShmemInitStruct("vexec exchange area", area_size(nblocks), &found);
	if (!found)
	{
		SpinLockInit(&area->mutex);
		area->nblocks = nblocks;
		area->nfree = nblocks;
		area->free_head = 0;
		for (int32 i = 0; i < nblocks; i++)
			area->next[i] = i + 1 < nblocks ? i + 1 : -1;
	}
	area_data = (char *) TYPEALIGN(VEXEC_EXCHANGE_ALIGN,
								   (char *) area + offsetof(ExchangeArea, next) +
								   sizeof(int32) * nblocks);
	area_bytes = (Size) nblocks * VEXEC_EXCHANGE_BLOCK;
	LWLockRelease(AddinShmemInitLock);
}

/* From _PG_init: the area is made with the postmaster's shared memory. */
void
vexec_exchange_install(void)
{
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = exchange_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = exchange_shmem_startup;
}

/* Whether the server has an area, and its setting asks for it in this session. */
bool
vexec_exchange_enabled(void)
{
	return area != NULL && vexec_enable_exchange_area;
}

/* A block claimed, its index; -1 where none is free. */
int32
vexec_exchange_claim(void)
{
	int32		b = -1;

	if (area == NULL)
		return -1;
	SpinLockAcquire(&area->mutex);
	if (area->free_head >= 0)
	{
		b = area->free_head;
		area->free_head = area->next[b];
		area->next[b] = -2;
		area->nfree--;
	}
	SpinLockRelease(&area->mutex);
	return b;
}

/*
 * A block given back, to be the next claimed: its pages are the warmest.
 * Called from a DSM's detach, during an error's cleanup too: a block that is
 * not a claimed one of the area is only reported.
 */
void
vexec_exchange_release(int32 b)
{
	if (area == NULL || b < 0 || b >= area->nblocks)
	{
		elog(WARNING, "vexec: block %d is not one of the exchange area's", (int) b);
		return;
	}
	SpinLockAcquire(&area->mutex);
	if (area->next[b] != -2)
	{
		SpinLockRelease(&area->mutex);
		elog(WARNING, "vexec: block %d of the exchange area given back twice", (int) b);
		return;
	}
	area->next[b] = area->free_head;
	area->free_head = b;
	area->nfree++;
	SpinLockRelease(&area->mutex);
}

/* A block's first byte. */
char *
vexec_exchange_block(int32 b)
{
	Assert(area != NULL && b >= 0 && b < area->nblocks);
	return area_data + (Size) b * VEXEC_EXCHANGE_BLOCK;
}

/* Whether memory is the area's. */
bool
vexec_exchange_holds(const void *p)
{
	return area_data != NULL && (const char *) p >= area_data &&
		(const char *) p < area_data + area_bytes;
}

/* The blocks free now, and in all, for vexec.exchange_area(). */
void
vexec_exchange_counts(int32 *nfree, int32 *nblocks)
{
	*nfree = 0;
	*nblocks = 0;
	if (area == NULL)
		return;
	SpinLockAcquire(&area->mutex);
	*nfree = area->nfree;
	*nblocks = area->nblocks;
	SpinLockRelease(&area->mutex);
}

PG_FUNCTION_INFO_V1(vexec_exchange_area);

/* vexec.exchange_area(): blocks, free, block_bytes */
Datum
vexec_exchange_area(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		values[3];
	bool		nulls[3] = {false, false, false};
	int32		nfree;
	int32		nblocks;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vexec.exchange_area() must return a record");
	vexec_exchange_counts(&nfree, &nblocks);
	values[0] = Int32GetDatum(nblocks);
	values[1] = Int32GetDatum(nfree);
	values[2] = Int64GetDatum((int64) VEXEC_EXCHANGE_BLOCK);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}
