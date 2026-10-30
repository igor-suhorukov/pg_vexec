/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * dml.c
 *	  DELETE, UPDATE and row locks on tables of their own files, as
 *	  heap_delete(), heap_update() and heap_lock_tuple() do them (heapam.c),
 *	  a row's deletion, update or lock being a row of
 *	  vexec_parquet.tombstones (tombstones.c) where heap sets a tuple's xmax
 *	  -- as ch_table_engines' dml.c does them for its tables.
 *
 *	  A writer of a row reads the row's state and inserts its own row under
 *	  the page lock of the row's TID block, as heap does both under the
 *	  buffer's content lock: no two writers of a row insert at once.  One
 *	  that finds another transaction in progress deleting, updating or
 *	  locking the row in a conflicting mode lets the page lock go, takes the
 *	  row's tuple lock -- heap's heavyweight mode for its own -- and waits
 *	  for that transaction holding it, as heap_acquire_tuplock() makes
 *	  waiters queue in order.  Row locks conflict as heap's modes do (a lock
 *	  row keeps its mode); a deletion or an update is LockTupleExclusive,
 *	  whatever columns it changes.
 *
 *	  An update is a deletion naming the row id of the new version, which
 *	  goes into the write buffer as an inserted row does; READ COMMITTED's
 *	  EvalPlanQual follows the name as heap follows t_ctid.
 *
 *	  A table that reads files (location) is read-only: am.c refuses its
 *	  writes before they come here.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/predicate.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "vexec_parquet.h"

/* The heavyweight tuple lock a waiter of each mode queues under, as heap's (tupleLockExtraInfo) */
static const LOCKMODE tuple_lock_modes[4] = {AccessShareLock, RowShareLock, ExclusiveLock, AccessExclusiveLock};

#define VPQ_TUPLE_LOCK AccessExclusiveLock

static void
guard_row(Relation rel, ItemPointer tid)
{
	LockPage(rel, ItemPointerGetBlockNumber(tid), ExclusiveLock);
}

static void
unguard_row(Relation rel, ItemPointer tid)
{
	UnlockPage(rel, ItemPointerGetBlockNumber(tid), ExclusiveLock);
}

static void
row_storage(Relation rel, VpqStorage *st)
{
	vpq_storage_of(rel, &rel->rd_locator, st);
}

static bool
lock_row(Relation rel, ItemPointer tid, LOCKMODE hw, LockWaitPolicy policy)
{
	switch (policy)
	{
		case LockWaitBlock:
			LockTuple(rel, tid, hw);
			return true;
		case LockWaitSkip:
			return ConditionalLockTuple(rel, tid, hw, false);
		case LockWaitError:
			if (!ConditionalLockTuple(rel, tid, hw, log_lock_failures))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on row in relation \"%s\"",
								RelationGetRelationName(rel))));
			return true;
	}
	return false;
}

static bool
wait_for(Relation rel, ItemPointer tid, TransactionId xid, LockWaitPolicy policy, XLTW_Oper oper)
{
	switch (policy)
	{
		case LockWaitBlock:
			XactLockTableWait(xid, rel, tid, oper);
			return true;
		case LockWaitSkip:
			return ConditionalXactLockTableWait(xid, false);
		case LockWaitError:
			if (!ConditionalXactLockTableWait(xid, log_lock_failures))
				ereport(ERROR,
						(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
						 errmsg("could not obtain lock on row in relation \"%s\"",
								RelationGetRelationName(rel))));
			return true;
	}
	return false;
}

static void
fill_failure(TM_FailureData *tmfd, const VpqRowState *st, ItemPointer tid)
{
	if (st->updated)
		vpq_set_tid(&tmfd->ctid, st->successor);
	else
		tmfd->ctid = *tid;
	tmfd->xmax = st->xid;
	tmfd->cmax = st->status == VPQ_ROW_SELF_MODIFIED ? st->cid : InvalidCommandId;
}

static void
check_writable(Relation rel)
{
	if (vpq_table(rel)->external)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a vexec_parquet table that reads files is read-only: \"%s\"",
						RelationGetRelationName(rel))));
}

/* Whether the row is one the snapshot sees: the crosscheck of a REPEATABLE READ RI action */
static bool
row_visible(Relation rel, uint64 rowid, Snapshot snapshot)
{
	TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(rel), table_slot_callbacks(rel));
	ItemPointerData tid;
	bool		found;

	vpq_set_tid(&tid, rowid);
	found = vpq_fetch_row(rel, &tid, snapshot, slot);
	ExecDropSingleTupleTableSlot(slot);
	return found;
}

/*
 * A deletion of the row at tid, or an update's (kind, successor): its state
 * read under the page lock of its TID block, a writer in progress waited for
 * under the row's tuple lock, and the tombstone inserted under cid where
 * nothing is in the way.  The results are heap_delete()'s.
 */
static TM_Result
modify_row(Relation rel, ItemPointer tid, CommandId cid, Snapshot crosscheck, bool wait,
		   TM_FailureData *tmfd, char kind, uint64 successor, bool *lock_ours)
{
	VpqStorage	storage;
	uint64		rowid;
	VpqRowState st;
	TM_Result	result;
	XLTW_Oper	oper = kind == VPQ_TOMB_UPDATED ? XLTW_Update : XLTW_Delete;
	bool		have_tuple_lock = false;

	if (!vpq_tid_rowid(tid, &rowid))
		elog(ERROR, "vexec_parquet: (%u,%u) names no row", ItemPointerGetBlockNumberNoCheck(tid),
			 ItemPointerGetOffsetNumberNoCheck(tid));
	if (IsInParallelMode())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TRANSACTION_STATE),
				 errmsg("cannot %s tuples during a parallel operation",
						kind == VPQ_TOMB_UPDATED ? "update" : "delete")));

	row_storage(rel, &storage);
	for (;;)
	{
		guard_row(rel, tid);
		vpq_row_state(&storage, rowid, LockTupleExclusive, &st);
		if (st.status == VPQ_ROW_BEING_MODIFIED && wait)
		{
			unguard_row(rel, tid);
			if (!have_tuple_lock)
			{
				LockTuple(rel, tid, VPQ_TUPLE_LOCK);
				have_tuple_lock = true;
			}
			wait_for(rel, tid, st.xid, LockWaitBlock, oper);
			continue;
		}
		break;
	}
	switch (st.status)
	{
		case VPQ_ROW_LIVE:
			result = TM_Ok;
			break;
		case VPQ_ROW_SELF_MODIFIED:
			if (st.cid < cid)
			{
				unguard_row(rel, tid);
				if (have_tuple_lock)
					UnlockTuple(rel, tid, VPQ_TUPLE_LOCK);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("attempted to %s invisible tuple",
								kind == VPQ_TOMB_UPDATED ? "update" : "delete")));
			}
			result = TM_SelfModified;
			break;
		case VPQ_ROW_BEING_MODIFIED:
			result = TM_BeingModified;
			break;
		case VPQ_ROW_MODIFIED:
		default:
			result = st.updated ? TM_Updated : TM_Deleted;
			break;
	}
	if (crosscheck != InvalidSnapshot && result == TM_Ok && !row_visible(rel, rowid, crosscheck))
	{
		/* in a file the crosscheck snapshot does not see: newer than it */
		result = TM_Updated;
		st.status = VPQ_ROW_MODIFIED;
		st.updated = false;
		st.xid = InvalidTransactionId;
	}
	if (result == TM_Ok)
	{
		CheckForSerializableConflictIn(rel, NULL, InvalidBlockNumber);
		vpq_tombstone_insert(&storage, rowid, kind, successor, cid);
	}
	else
		fill_failure(tmfd, &st, tid);
	if (lock_ours)
		*lock_ours = st.lock_ours;
	unguard_row(rel, tid);
	if (have_tuple_lock)
		UnlockTuple(rel, tid, VPQ_TUPLE_LOCK);
	return result;
}

TM_Result
vpq_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, uint32 options, Snapshot snapshot,
				 Snapshot crosscheck, bool wait, TM_FailureData *tmfd)
{
	TM_Result	result;

	check_writable(rel);
	result = modify_row(rel, tid, cid, crosscheck, wait, tmfd, VPQ_TOMB_DELETED, 0, NULL);
	if (result == TM_Ok)
		pgstat_count_heap_delete(rel);
	return result;
}

/*
 * An update: the new version's row id taken first, which its tombstone
 * names, and the new row buffered under it once the old one is the
 * update's.  The TID changes, so every index takes the new row.  A lock the
 * running transaction held on the old version shows as the new version's
 * xmax, as heap_update() carries it over -- ON CONFLICT DO UPDATE locks
 * first, and RETURNING xmax = 0 tells its inserts from its updates.
 */
TM_Result
vpq_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot, CommandId cid, uint32 options,
				 Snapshot snapshot, Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
				 LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes)
{
	uint64		newid;
	TM_Result	result;
	bool		lock_ours = false;

	check_writable(rel);
	*lockmode = LockTupleExclusive;
	*update_indexes = TU_None;
	newid = vpq_buffer_next_rowid(rel, cid);
	result = modify_row(rel, otid, cid, crosscheck, wait, tmfd, VPQ_TOMB_UPDATED, newid, &lock_ours);
	if (result != TM_Ok)
		return result;
	vpq_buffer_insert(rel, slot, cid, false);
	if (lock_ours)
		vpq_slot_set_xmax(slot, GetCurrentTransactionId());
	if (VPQ_TID_TO_ROWID(&slot->tts_tid) != newid)
		elog(ERROR, "vexec_parquet: an updated row got row id " UINT64_FORMAT " where its tombstone names " UINT64_FORMAT,
			 VPQ_TID_TO_ROWID(&slot->tts_tid), newid);
	*update_indexes = TU_All;
	pgstat_count_heap_update(rel, false, false);
	return TM_Ok;
}

/*
 * A row lock (SELECT ... FOR UPDATE and the like, EvalPlanQual's lock of the
 * latest version): a lock row under cid, every mode as exclusive, the row in
 * *slot.  With TUPLE_LOCK_FLAG_FIND_LAST_VERSION an update by a committed
 * transaction is followed to its new version, which is locked instead, as
 * heapam_tuple_lock() follows one.
 */
TM_Result
vpq_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot, CommandId cid,
			   LockTupleMode mode, LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd)
{
	VpqStorage	storage;
	VpqRowState st;
	bool		own_version = false;
	CommandId	own_cid = InvalidCommandId;
	VpqTable   *t = vpq_table(rel);

	tmfd->traversed = false;
	/* a table that reads files: its rows are not PostgreSQL's to lock, nor anyone's to change */
	if (t->external)
	{
		if (!vpq_fetch_row(rel, tid, SnapshotAny, slot))
			return TM_Deleted;
		return TM_Ok;
	}
	row_storage(rel, &storage);
	/* a row this command inserted, still buffered: not the command's to lock (heapam_tuple_lock()) */
	{
		uint64		rowid;
		CommandId	inserted;

		if (vpq_tid_rowid(tid, &rowid) && vpq_buffer_row_cid(rel, rowid, &inserted) && inserted >= cid)
		{
			if (vpq_buffer_fetch(rel, rowid, SnapshotAny, slot))
				((VpqSlot *) slot)->xmin = GetCurrentTransactionId();
			return TM_Invisible;
		}
	}
	for (;;)
	{
		uint64		rowid;
		bool		have_tuple_lock = false;
		bool		lock = false;

		if (!vpq_tid_rowid(tid, &rowid))
			return TM_Deleted;
		for (;;)
		{
			guard_row(rel, tid);
			vpq_row_state(&storage, rowid, (int) mode, &st);
			if (st.status == VPQ_ROW_BEING_MODIFIED)
			{
				unguard_row(rel, tid);
				if (!have_tuple_lock)
				{
					if (!lock_row(rel, tid, tuple_lock_modes[mode & 3], wait_policy))
						return TM_WouldBlock;
					have_tuple_lock = true;
				}
				if (!wait_for(rel, tid, st.xid, wait_policy, XLTW_Lock))
				{
					UnlockTuple(rel, tid, tuple_lock_modes[mode & 3]);
					return TM_WouldBlock;
				}
				continue;
			}
			break;
		}
		lock = st.status == VPQ_ROW_LIVE && !(own_version && own_cid >= cid);
		if (lock && !st.locked_by_us)
			vpq_tombstone_insert(&storage, rowid, VPQ_TOMB_LOCKED, (uint64) mode, cid);
		unguard_row(rel, tid);
		if (have_tuple_lock)
			UnlockTuple(rel, tid, tuple_lock_modes[mode & 3]);
		switch (st.status)
		{
			case VPQ_ROW_LIVE:
				if (!lock)
				{
					tmfd->ctid = *tid;
					tmfd->xmax = GetCurrentTransactionId();
					tmfd->cmax = own_cid;
					return TM_SelfModified;
				}
				if (!vpq_fetch_row(rel, tid, SnapshotAny, slot))
					elog(ERROR, "vexec_parquet: row (%u,%u) of \"%s\" is in no file",
						 ItemPointerGetBlockNumber(tid), ItemPointerGetOffsetNumber(tid),
						 RelationGetRelationName(rel));
				return TM_Ok;
			case VPQ_ROW_SELF_MODIFIED:
				if (tmfd->traversed)
				{
					if (!st.updated)
						return TM_Deleted;
					own_version = true;
					own_cid = st.cid;
					vpq_set_tid(tid, st.successor);
					continue;
				}
				if (st.cid < cid)
					return TM_Invisible;
				fill_failure(tmfd, &st, tid);
				return TM_SelfModified;
			case VPQ_ROW_MODIFIED:
			default:
				if (st.updated && (flags & TUPLE_LOCK_FLAG_FIND_LAST_VERSION))
				{
					vpq_set_tid(tid, st.successor);
					tmfd->traversed = true;
					own_version = false;
					continue;
				}
				fill_failure(tmfd, &st, tid);
				return st.updated ? TM_Updated : TM_Deleted;
		}
	}
}

/* The latest version of the row at tid the scan's snapshot sees: updates followed to their new rows */
void
vpq_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid)
{
	Relation	rel = scan->rs_rd;
	VpqStorage	storage;
	Snapshot	snapshot = scan->rs_snapshot;

	if (vpq_table(rel)->external)
		return;
	row_storage(rel, &storage);
	for (;;)
	{
		uint64		successor;
		uint64		rowid;

		if (!vpq_tid_rowid(tid, &rowid) ||
			!vpq_tombstone_visible(&storage, snapshot, rowid, &successor) || successor == 0)
			return;
		vpq_set_tid(tid, successor);
	}
}
