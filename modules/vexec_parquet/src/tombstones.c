/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * tombstones.c
 *	  vexec_parquet.tombstones and its UNLOGGED twin: a row's deletion, an
 *	  update's -- naming the row id of the new version -- or a lock on the
 *	  row, each a catalog row keyed by the row's id, whose visibility is
 *	  heap's, as ch_table_engines' tombstones are (its tombstones.c).
 *
 *	  A row of a visible file is visible to a snapshot when no deletion or
 *	  update the snapshot sees names it.  A deletion or an update is
 *	  inserted under its command's id, so the command itself still sees the
 *	  row and later ones do not, as heap's xmax and cmax have it.
 *
 *	  What a writer of a row asks -- whether another transaction is
 *	  deleting, updating or locking it, has done so and committed, or the
 *	  running one has -- is vpq_row_state()'s answer, read with SnapshotAny
 *	  and each catalog row's writer judged as HeapTupleSatisfiesUpdate()
 *	  judges a tuple's xmax.  Writers of a row take its page lock first
 *	  (dml.c), so that no two of them insert at once.  A lock holds while
 *	  its transaction runs, and means nothing after: the running transaction
 *	  deletes its own lock rows before it commits.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/predicate.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "vexec_parquet.h"

#define Natts_tombstones 6
#define Anum_tombstones_rowid 4
#define Anum_tombstones_kind 5
#define Anum_tombstones_successor 6

static int
row_keys(const VpqStorage *st, ScanKeyData *keys, StrategyNumber strategy, uint64 rowid)
{
	int			n = vpq_catalog_storage_keys(st, keys);

	if (strategy != InvalidStrategy)
		ScanKeyInit(&keys[n++], Anum_tombstones_rowid, strategy,
					strategy == BTEqualStrategyNumber ? F_INT8EQ :
					strategy == BTGreaterEqualStrategyNumber ? F_INT8GE : F_INT8LE,
					Int64GetDatum((int64) rowid));
	return n;
}

static void
deform(Relation catalog, HeapTuple tuple, uint64 *rowid, char *kind, uint64 *successor)
{
	Datum		values[Natts_tombstones];
	bool		nulls[Natts_tombstones];

	heap_deform_tuple(tuple, RelationGetDescr(catalog), values, nulls);
	*rowid = (uint64) DatumGetInt64(values[Anum_tombstones_rowid - 1]);
	*kind = DatumGetChar(values[Anum_tombstones_kind - 1]);
	*successor = nulls[Anum_tombstones_successor - 1] ? 0 :
		(uint64) DatumGetInt64(values[Anum_tombstones_successor - 1]);
}

/*
 * A row's state to a writer that would hold it in mode (a LockTupleMode; a
 * deletion's and an update's LockTupleExclusive): a lock in progress whose
 * mode does not conflict with mode leaves the row live
 */
void
vpq_row_state(const VpqStorage *st, uint64 rowid, int mode, VpqRowState *state)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData keys[4];
	HeapTuple	tuple;
	int			nkeys;

	memset(state, 0, sizeof(*state));
	state->status = VPQ_ROW_LIVE;
	catalog = vpq_catalog_open("tombstones", st->persistence, AccessShareLock);
	nkeys = row_keys(st, keys, BTEqualStrategyNumber, rowid);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, SnapshotAny, nkeys, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		HeapTupleHeader h = tuple->t_data;
		TransactionId xmin = HeapTupleHeaderGetRawXmin(h);
		uint64		rid;
		char		kind;
		uint64		successor;
		VpqWriterState ws;

		if (vpq_row_deleted(h))
			continue;
		ws = vpq_writer_state(h);
		if (ws == VPQ_WRITER_ABORTED)
			continue;
		deform(catalog, tuple, &rid, &kind, &successor);
		if (kind == VPQ_TOMB_LOCKED)
		{
			int			held = (int) successor;	/* a lock row keeps its mode there */

			if (ws == VPQ_WRITER_US)
			{
				state->lock_ours = true;
				if (held >= mode)
					state->locked_by_us = true;
			}
			else if (ws == VPQ_WRITER_IN_PROGRESS && state->status == VPQ_ROW_LIVE &&
					 vpq_lock_modes_conflict(held, mode))
			{
				state->status = VPQ_ROW_BEING_MODIFIED;
				state->xid = xmin;
				state->lock_only = true;
			}
			continue;
		}
		switch (ws)
		{
			case VPQ_WRITER_COMMITTED:
				state->status = VPQ_ROW_MODIFIED;
				state->xid = xmin;
				state->updated = kind == VPQ_TOMB_UPDATED;
				state->successor = successor;
				state->lock_only = false;
				break;
			case VPQ_WRITER_IN_PROGRESS:
				if (state->status != VPQ_ROW_MODIFIED)
				{
					state->status = VPQ_ROW_BEING_MODIFIED;
					state->xid = xmin;
					state->lock_only = false;
					state->updated = kind == VPQ_TOMB_UPDATED;
					state->successor = successor;
				}
				break;
			case VPQ_WRITER_US:
				if (state->status == VPQ_ROW_LIVE ||
					(state->status == VPQ_ROW_BEING_MODIFIED && state->lock_only))
				{
					state->status = VPQ_ROW_SELF_MODIFIED;
					state->xid = xmin;
					state->cid = HeapTupleHeaderGetCmin(h);
					state->updated = kind == VPQ_TOMB_UPDATED;
					state->successor = successor;
					state->lock_only = false;
				}
				break;
			case VPQ_WRITER_ABORTED:
				break;
		}
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
}

/*
 * A row's xmax as heap's tuple would show it: the transaction that deleted
 * or updated it, running or committed, else one that holds a lock on it and
 * runs -- the running transaction's own before another's
 */
TransactionId
vpq_row_xmax(const VpqStorage *st, uint64 rowid)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData keys[4];
	HeapTuple	tuple;
	int			nkeys;
	TransactionId modifier = InvalidTransactionId;
	TransactionId locker = InvalidTransactionId;

	catalog = vpq_catalog_open("tombstones", st->persistence, AccessShareLock);
	nkeys = row_keys(st, keys, BTEqualStrategyNumber, rowid);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, SnapshotAny, nkeys, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		HeapTupleHeader h = tuple->t_data;
		VpqWriterState ws;
		uint64		rid;
		char		kind;
		uint64		successor;

		if (vpq_row_deleted(h))
			continue;
		ws = vpq_writer_state(h);
		if (ws == VPQ_WRITER_ABORTED)
			continue;
		deform(catalog, tuple, &rid, &kind, &successor);
		if (kind != VPQ_TOMB_LOCKED)
			modifier = HeapTupleHeaderGetRawXmin(h);
		else if (!TransactionIdIsValid(locker) || ws == VPQ_WRITER_US)
			locker = HeapTupleHeaderGetRawXmin(h);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return TransactionIdIsValid(modifier) ? modifier : locker;
}

/* The running transaction's lock rows, deleted before it commits */
typedef struct OwnLock
{
	Oid			catalog;
	ItemPointerData tid;
} OwnLock;

static List *own_locks = NIL;	/* in TopTransactionContext */

void
vpq_tombstone_insert(const VpqStorage *st, uint64 rowid, char kind, uint64 successor, CommandId cid)
{
	Relation	catalog;
	Datum		values[Natts_tombstones];
	bool		nulls[Natts_tombstones];
	HeapTuple	tuple;

	catalog = vpq_catalog_open("tombstones", st->persistence, RowExclusiveLock);
	memset(nulls, false, sizeof(nulls));
	values[0] = ObjectIdGetDatum(st->spc);
	values[1] = ObjectIdGetDatum(st->relnumber);
	values[2] = Int32GetDatum(st->backend);
	values[Anum_tombstones_rowid - 1] = Int64GetDatum((int64) rowid);
	values[Anum_tombstones_kind - 1] = CharGetDatum(kind);
	/* an update's new row id, a lock's mode */
	if (kind == VPQ_TOMB_UPDATED || kind == VPQ_TOMB_LOCKED)
		values[Anum_tombstones_successor - 1] = Int64GetDatum((int64) successor);
	else
		nulls[Anum_tombstones_successor - 1] = true;
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	vpq_catalog_insert(catalog, tuple, cid);
	if (kind == VPQ_TOMB_LOCKED)
	{
		MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
		OwnLock    *l = palloc(sizeof(OwnLock));

		l->catalog = RelationGetRelid(catalog);
		l->tid = tuple->t_self;
		own_locks = lappend(own_locks, l);
		MemoryContextSwitchTo(old);
	}
	heap_freetuple(tuple);
	table_close(catalog, RowExclusiveLock);
}

/* Before the transaction commits: its lock rows go; a prepared transaction keeps its locks */
static void
release_own_locks(void)
{
	ListCell   *lc;
	Relation	catalog = NULL;

	/* the last command's lock rows were inserted under its id: deleted under the next */
	CommandCounterIncrement();
	foreach(lc, own_locks)
	{
		OwnLock    *l = lfirst(lc);
		HeapTupleData tuple;
		Buffer		buffer;
		bool		ours;

		if (catalog == NULL || RelationGetRelid(catalog) != l->catalog)
		{
			if (catalog)
				table_close(catalog, RowExclusiveLock);
			catalog = table_open(l->catalog, RowExclusiveLock);
		}
		tuple.t_self = l->tid;
		if (!heap_fetch(catalog, SnapshotAny, &tuple, &buffer, false))
			continue;
		ours = TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetRawXmin(tuple.t_data)) &&
			(tuple.t_data->t_infomask & HEAP_XMAX_INVALID);
		ReleaseBuffer(buffer);
		if (ours)
		{
			simple_heap_delete(catalog, &l->tid);
			vpq_catalog_writes++;
		}
	}
	if (catalog)
		table_close(catalog, RowExclusiveLock);
	own_locks = NIL;
}

bool
vpq_tombstones_pending(void)
{
	return own_locks != NIL;
}

void
vpq_tombstones_xact_callback(XactEvent event)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
			if (own_locks != NIL)
				release_own_locks();
			break;
		case XACT_EVENT_PRE_PREPARE:
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
		case XACT_EVENT_PREPARE:
			own_locks = NIL;
			break;
	}
}

/* Row ids of deletions and updates in [lo, hi] the snapshot sees, ascending, without repeats */
uint64 *
vpq_tombstones_visible(const VpqStorage *st, Snapshot snapshot, uint64 lo, uint64 hi, int *n)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData keys[5];
	HeapTuple	tuple;
	int			nkeys;
	int			cap = 0;
	uint64	   *ids = NULL;

	*n = 0;
	catalog = vpq_catalog_open("tombstones", st->persistence, AccessShareLock);
	nkeys = vpq_catalog_storage_keys(st, keys);
	ScanKeyInit(&keys[nkeys++], Anum_tombstones_rowid, BTGreaterEqualStrategyNumber, F_INT8GE,
				Int64GetDatum((int64) lo));
	ScanKeyInit(&keys[nkeys++], Anum_tombstones_rowid, BTLessEqualStrategyNumber, F_INT8LE,
				Int64GetDatum((int64) hi));
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, snapshot, nkeys, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		uint64		rowid;
		char		kind;
		uint64		successor;

		deform(catalog, tuple, &rowid, &kind, &successor);
		if (kind == VPQ_TOMB_LOCKED)
			continue;
		if (*n > 0 && ids[*n - 1] == rowid)
			continue;
		if (*n == cap)
		{
			cap = cap ? cap * 2 : 64;
			ids = ids ? repalloc_huge(ids, sizeof(uint64) * cap) :
				palloc_extended(sizeof(uint64) * cap, MCXT_ALLOC_HUGE);
		}
		ids[(*n)++] = rowid;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return ids;
}

/* Whether the snapshot sees any deletion or update of the storage's rows */
bool
vpq_tombstones_visible_any(const VpqStorage *st, Snapshot snapshot)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData keys[3];
	HeapTuple	tuple;
	int			nkeys;
	bool		found = false;

	catalog = vpq_catalog_open("tombstones", st->persistence, AccessShareLock);
	nkeys = vpq_catalog_storage_keys(st, keys);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, snapshot, nkeys, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		uint64		rowid;
		char		kind;
		uint64		successor;

		deform(catalog, tuple, &rowid, &kind, &successor);
		if (kind != VPQ_TOMB_LOCKED)
		{
			found = true;
			break;
		}
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return found;
}

/* Whether the snapshot sees a deletion or update of the row: an update's new row id into *successor */
bool
vpq_tombstone_visible(const VpqStorage *st, Snapshot snapshot, uint64 rowid, uint64 *successor)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData keys[4];
	HeapTuple	tuple;
	int			nkeys;
	bool		found = false;

	if (successor)
		*successor = 0;
	catalog = vpq_catalog_open("tombstones", st->persistence, AccessShareLock);
	nkeys = row_keys(st, keys, BTEqualStrategyNumber, rowid);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, snapshot, nkeys, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		uint64		rid;
		char		kind;
		uint64		succ;

		deform(catalog, tuple, &rid, &kind, &succ);
		if (kind == VPQ_TOMB_LOCKED)
			continue;
		found = true;
		if (successor && kind == VPQ_TOMB_UPDATED)
			*successor = succ;
		break;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return found;
}

/*
 * Whether the deletions of a row leave it to the snapshot: SnapshotAny sees
 * every row; SnapshotDirty a row being deleted, its deleter's xid in xmax, as
 * HeapTupleSatisfiesDirty() does; others a row no deletion they see names
 */
bool
vpq_row_survives(const VpqStorage *st, uint64 rowid, Snapshot snapshot)
{
	if (snapshot == NULL || snapshot->snapshot_type == SNAPSHOT_ANY ||
		snapshot->snapshot_type == SNAPSHOT_NON_VACUUMABLE)
		return true;
	if (snapshot->snapshot_type == SNAPSHOT_DIRTY)
	{
		VpqRowState state;

		vpq_row_state(st, rowid, LockTupleExclusive, &state);
		if (state.status == VPQ_ROW_MODIFIED || state.status == VPQ_ROW_SELF_MODIFIED)
			return false;
		if (state.status == VPQ_ROW_BEING_MODIFIED && !state.lock_only)
			snapshot->xmax = state.xid;
		return true;
	}
	return !vpq_tombstone_visible(st, snapshot, rowid, NULL);
}

/*
 * A storage's committed deletions and updates, and the running
 * transaction's, given to its copy (SET TABLESPACE)
 */
void
vpq_tombstones_copy(const VpqStorage *from, const VpqStorage *to)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData keys[3];
	HeapTuple	tuple;
	int			nkeys;
	CommandId	cid = GetCurrentCommandId(true);
	List	   *copies = NIL;
	ListCell   *lc;

	catalog = vpq_catalog_open("tombstones", from->persistence, AccessShareLock);
	nkeys = vpq_catalog_storage_keys(from, keys);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, SnapshotAny, nkeys, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		VpqWriterState ws = vpq_writer_state(tuple->t_data);
		uint64	   *t;
		char		kind;

		if (vpq_row_deleted(tuple->t_data) || (ws != VPQ_WRITER_COMMITTED && ws != VPQ_WRITER_US))
			continue;
		t = palloc(sizeof(uint64) * 3);
		deform(catalog, tuple, &t[0], &kind, &t[2]);
		if (kind == VPQ_TOMB_LOCKED)
			continue;
		t[1] = (uint64) kind;
		copies = lappend(copies, t);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	foreach(lc, copies)
	{
		uint64	   *t = lfirst(lc);

		vpq_tombstone_insert(to, t[0], (char) t[1], t[2], cid);
	}
}

/*
 * SERIALIZABLE: every writer of a storage's files and deletions, visible to
 * the snapshot or not, reported to SSI's conflict-out check, which passes
 * over the ones that are not concurrent
 */
void
vpq_conflict_out(Relation rel, const VpqStorage *st, Snapshot snapshot)
{
	static const char *const names[] = {"files", "tombstones"};

	for (int k = 0; k < lengthof(names); k++)
	{
		Relation	catalog = vpq_catalog_open(names[k], st->persistence, AccessShareLock);
		SysScanDesc scan;
		ScanKeyData keys[3];
		HeapTuple	tuple;
		int			nkeys = vpq_catalog_storage_keys(st, keys);
		TransactionId last = InvalidTransactionId;

		scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, SnapshotAny, nkeys, keys);
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			TransactionId xmin = HeapTupleHeaderGetXmin(tuple->t_data);

			if (TransactionIdIsNormal(xmin) && !TransactionIdEquals(xmin, last))
			{
				CheckForSerializableConflictOut(rel, xmin, snapshot);
				last = xmin;
			}
		}
		systable_endscan(scan);
		table_close(catalog, AccessShareLock);
	}
}

/*
 * Every deletion and update of a storage whose writer did not abort, by row
 * id: an index build hands their rows as not alive
 */
VpqDeadRow *
vpq_tombstones_all(const VpqStorage *st, int *n)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData keys[3];
	HeapTuple	tuple;
	int			nkeys;
	int			cap = 0;
	VpqDeadRow *all = NULL;

	*n = 0;
	catalog = vpq_catalog_open("tombstones", st->persistence, AccessShareLock);
	nkeys = vpq_catalog_storage_keys(st, keys);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, SnapshotAny, nkeys, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		uint64		rowid;
		char		kind;
		uint64		successor;

		if (vpq_row_deleted(tuple->t_data) || vpq_writer_state(tuple->t_data) == VPQ_WRITER_ABORTED)
			continue;
		deform(catalog, tuple, &rowid, &kind, &successor);
		if (kind == VPQ_TOMB_LOCKED)
			continue;
		if (*n > 0 && all[*n - 1].rowid == rowid)
			continue;
		if (*n == cap)
		{
			cap = cap ? cap * 2 : 64;
			all = all ? repalloc_huge(all, sizeof(VpqDeadRow) * cap) :
				palloc_extended(sizeof(VpqDeadRow) * cap, MCXT_ALLOC_HUGE);
		}
		all[(*n)++].rowid = rowid;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return all;
}

/*
 * Row ids whose deletion or update every snapshot sees: its writer
 * committed before horizon, ascending -- VACUUM removes their index entries
 */
uint64 *
vpq_tombstones_dead_to_all(const VpqStorage *st, TransactionId horizon, int *n,
						   int64 *recently_dead)
{
	Relation	catalog;
	SysScanDesc scan;
	ScanKeyData keys[3];
	HeapTuple	tuple;
	int			nkeys;
	int			cap = 0;
	uint64	   *ids = NULL;

	*n = 0;
	*recently_dead = 0;
	catalog = vpq_catalog_open("tombstones", st->persistence, AccessShareLock);
	nkeys = vpq_catalog_storage_keys(st, keys);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, SnapshotAny, nkeys, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		HeapTupleHeader h = tuple->t_data;
		TransactionId xmin = HeapTupleHeaderGetRawXmin(h);
		uint64		rowid;
		char		kind;
		uint64		successor;

		if (vpq_row_deleted(h) || vpq_writer_state(h) != VPQ_WRITER_COMMITTED)
			continue;
		deform(catalog, tuple, &rowid, &kind, &successor);
		if (kind == VPQ_TOMB_LOCKED)
			continue;
		/* dead to a snapshot that still runs: its index entries stay */
		if (!HeapTupleHeaderXminFrozen(h) && !TransactionIdPrecedes(xmin, horizon))
		{
			(*recently_dead)++;
			continue;
		}
		if (*n > 0 && ids[*n - 1] == rowid)
			continue;
		if (*n == cap)
		{
			cap = cap ? cap * 2 : 64;
			ids = ids ? repalloc_huge(ids, sizeof(uint64) * cap) :
				palloc_extended(sizeof(uint64) * cap, MCXT_ALLOC_HUGE);
		}
		ids[(*n)++] = rowid;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return ids;
}
