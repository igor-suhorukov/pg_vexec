/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * devasked.c
 *	  The columns statements have asked a device for: a column the device
 *	  keeps not, priced as kept once a statement has asked for it (W20 of
 *	  pg_vector_executor.md §3.28; §8 item 21's other choice).
 *
 * The device's cache is filled by its own reads alone, and a column it keeps
 * is priced as no bytes to read (W7, §3.27; vexec_device_kept_rows()).  A
 * statement whose device read of a column the device keeps not would cost
 * more than the CPU's plan therefore never takes the device, and the column
 * is never kept: in ClickBench's protocol, each query's first try after a
 * restart, mergetree's Q19, Q38, Q40 and Q41 stay on the CPU at every try,
 * 8 to 12 ms where the device takes 5 to 7 with their columns kept.  So a
 * column is priced as kept once a statement has asked the device for it --
 * the planner priced a device read of it, whichever path then won -- and
 * the next statement takes the device and pays the uncached read, after
 * which the device keeps the column.
 *
 * An entry a column, by its database, relation and attribute, in a table of
 * shared memory under one LWLock; the statement that last moved it, by a
 * token of its planning (vexec_device_asked_token()), and its state:
 *
 *	- asked: a statement priced the device's read of the column, which the
 *	  device keeps not;
 *	- tried: a later statement priced it as kept -- every price of the column
 *	  in that statement's planning the same.  Were the device to keep none of
 *	  it after, a statement since prices it as the device keeps it, not kept,
 *	  and asks no more: the device then never read it -- that plan's CPU path
 *	  cheaper still -- or did not keep it, its cache too small.  W50
 *	  (pg_vector_executor.md §3.36): a statement tries columns only while
 *	  their bytes as the device decodes them fit its cache, as the provider
 *	  publishes it (vexec_device_cache_bytes()) -- ClickBench's Q23 at 100M
 *	  rows asked for its 105 columns, and its next try priced them all as
 *	  kept and read 14.8 GB on the device, 9.3 s, where a 3.8 GB cache kept
 *	  few.  A column past it is priced as the device keeps it and stays asked,
 *	  so that the statement's other prices of it are the same, and a later
 *	  statement may try it;
 *	- kept: the device has kept some of it since it was asked, and the column
 *	  is priced as kept whole.  A read of some of a column's units -- a key's
 *	  pruned granules, a condition cache's cut -- keeps those alone, which
 *	  W7's share of the relation's rows prices as hardly any; the statement
 *	  that asks for them again asks for those same units.  Kept no more, its
 *	  units given back for other columns, it is tried once more.
 *
 * An entry is a hint: in one of ASKED_PROBES slots by its column's hash, the
 * entry read longest ago of them giving way to a new one, whose column is
 * asked for again.  vexec.gpu_price_asked off, nothing is read or kept, and
 * a column is priced as the device keeps it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "common/hashfn.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "vexec.h"
#include "exec/device.h"
#include "exec/exec.h"

#define ASKED_SLOTS		1024	/* a power of two */
#define ASKED_PROBES	8		/* the slots a column's entry may take */

typedef enum AskedState
{
	ASKED_NONE = 0,
	ASKED_ASKED,
	ASKED_TRIED,
	ASKED_KEPT
} AskedState;

typedef struct AskedSlot
{
	Oid			database;
	Oid			relation;		/* InvalidOid: a free slot */
	int16		attnum;
	uint8		state;			/* AskedState */
	uint64		statement;		/* the token of the statement that moved it last */
	uint64		used;			/* the table's clock as it was last read */
} AskedSlot;

typedef struct AskedShared
{
	LWLock	   *lock;
	uint64		clock;
	AskedSlot	slots[ASKED_SLOTS];
} AskedShared;

static AskedShared *asked = NULL;
static uint64 plannings = 0;

/* W50: the statement whose planning tries columns now, and their bytes so far */
static uint64 trial_statement = 0;
static double trial_bytes = 0;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void
asked_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestAddinShmemSpace(sizeof(AskedShared));
	RequestNamedLWLockTranche("vexec device asked", 1);
}

static void
asked_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	asked = ShmemInitStruct("vexec device asked", sizeof(AskedShared), &found);
	if (!found)
	{
		memset(asked, 0, sizeof(AskedShared));
		asked->lock = &(GetNamedLWLockTranche("vexec device asked"))->lock;
	}
	LWLockRelease(AddinShmemInitLock);
}

/* From _PG_init: the table is made with the postmaster's shared memory. */
void
vexec_device_asked_install(void)
{
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = asked_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = asked_shmem_startup;
}

/*
 * A token for a statement's planning, which its asks carry: of the backend
 * -- its process and its start -- and its plannings so far, never 0.  Each
 * statement planned in a backend has its own, a statement planned in two
 * backends two; a function's statements planned as it runs theirs.
 */
uint64
vexec_device_asked_token(void)
{
	uint64		t = hash_combine64(hash_combine64((uint64) MyProcPid, (uint64) MyStartTimestamp), ++plannings);

	return t == 0 ? 1 : t;
}

/*
 * The rows of a column to price as the device keeps them, for a device read
 * the statement whose token is given prices: kept, the rows the device keeps
 * (W7), all, the relation's, and bytes, the column's over them as the device
 * decodes it (W50).  The column's entry moved as the comment at the top
 * says; 0 statement, or vexec.gpu_price_asked off, kept as it is.
 */
double
vexec_device_priced_kept(Oid relation, AttrNumber attnum, uint64 statement, double kept, double all,
						 double bytes)
{
	AskedSlot  *slot = NULL;
	AskedSlot  *victim = NULL;
	double		priced = kept;
	uint32		h;

	if (asked == NULL || !vexec_gpu_price_asked || statement == 0 || attnum <= 0 || all <= 0)
		return kept;
	h = murmurhash32(hash_combine(hash_combine(murmurhash32((uint32) MyDatabaseId), (uint32) relation),
								  (uint32) attnum));
	LWLockAcquire(asked->lock, LW_EXCLUSIVE);
	for (int i = 0; i < ASKED_PROBES; i++)
	{
		AskedSlot  *s = &asked->slots[(h + i) & (ASKED_SLOTS - 1)];

		if (s->relation == relation && s->attnum == attnum && s->database == MyDatabaseId)
		{
			slot = s;
			break;
		}
		/* a free slot, else the one read longest ago */
		if (victim == NULL ||
			(victim->relation != InvalidOid && (s->relation == InvalidOid || s->used < victim->used)))
			victim = s;
	}
	if (slot == NULL)
	{
		/* asked first: priced as the device keeps it */
		slot = victim;
		slot->database = MyDatabaseId;
		slot->relation = relation;
		slot->attnum = (int16) attnum;
		slot->state = ASKED_ASKED;
		slot->statement = statement;
	}
	else if (slot->statement == statement)
	{
		/* asked again in the same statement's planning: priced as it was */
		if (slot->state != ASKED_ASKED)
			priced = all;
	}
	else if (kept > 0 && slot->state != ASKED_NONE)
	{
		/* kept since it was asked: whole */
		slot->state = ASKED_KEPT;
		slot->statement = statement;
		priced = all;
	}
	else if (slot->state == ASKED_ASKED || slot->state == ASKED_KEPT)
	{
		/* asked by another statement, or kept no more: tried once, W50 within the cache */
		if (trial_statement != statement)
		{
			trial_statement = statement;
			trial_bytes = 0;
		}
		if (trial_bytes + Max(bytes, 0) <= (double) vexec_device_cache_bytes())
		{
			trial_bytes += Max(bytes, 0);
			slot->state = ASKED_TRIED;
			priced = all;
		}
		else
			slot->state = ASKED_ASKED;
		slot->statement = statement;
	}
	slot->used = ++asked->clock;
	LWLockRelease(asked->lock);
	return priced;
}

/*
 * vexec.device_asked(): each column's entry -- its relation of the session's
 * database, its attribute and its state -- for the tests.
 */
PG_FUNCTION_INFO_V1(vexec_device_asked);
Datum
vexec_device_asked(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	static const char *const names[] = {"none", "asked", "tried", "kept"};

	InitMaterializedSRF(fcinfo, 0);
	if (asked == NULL)
		return (Datum) 0;
	LWLockAcquire(asked->lock, LW_SHARED);
	for (int i = 0; i < ASKED_SLOTS; i++)
	{
		const AskedSlot *s = &asked->slots[i];
		Datum		values[3];
		bool		nulls[3] = {false, false, false};

		if (s->relation == InvalidOid || s->database != MyDatabaseId)
			continue;
		values[0] = ObjectIdGetDatum(s->relation);
		values[1] = Int16GetDatum(s->attnum);
		values[2] = CStringGetTextDatum(names[s->state < lengthof(names) ? s->state : 0]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	LWLockRelease(asked->lock);
	return (Datum) 0;
}
