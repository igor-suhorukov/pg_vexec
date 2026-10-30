/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * catalog.c
 *	  The extension's catalogs: vexec_parquet.storages, an own table's
 *	  storage's counters, and vexec_parquet.files, its files -- each with an
 *	  UNLOGGED twin for UNLOGGED and temporary tables, whose rows a crash
 *	  takes with it.  vexec_parquet.tombstones has a file of its own
 *	  (tombstones.c).  After ch_table_engines' catalog.c.
 *
 * A file is visible to a snapshot when its row is.  A file's row is
 * inserted under the command id its rows were inserted with, so a command
 * never sees its own rows and later commands do.
 *
 * A storage's row id and file counters move in place, as a sequence's do:
 * never back, gaps allowed, so that a row id an aborted writer took is never
 * given again while an index may name it.  An in-place update refuses a row
 * the running transaction inserted (heapam.c, heap_inplace_lock()), so the
 * counters of a storage made in the running transaction are kept in the
 * backend and written before it commits.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "varatt.h"

#include "vexec_parquet.h"

/* vexec_parquet.storages's columns */
#define Natts_storages 5
#define Anum_storages_spc 1
#define Anum_storages_relnumber 2
#define Anum_storages_backend 3
#define Anum_storages_next_rowid 4
#define Anum_storages_next_file 5

/* vexec_parquet.files's columns */
#define Natts_files 9
#define Anum_files_spc 1
#define Anum_files_relnumber 2
#define Anum_files_backend 3
#define Anum_files_file_id 4
#define Anum_files_first_rowid 5
#define Anum_files_rows 6
#define Anum_files_bytes 7
#define Anum_files_fxid 8
#define Anum_files_stats 9

/*
 * Bumped by every write of this process's to a catalog: a listing kept for
 * a snapshot (listing.c) is not kept past one, which the snapshot -- under
 * the same command id -- would not tell
 */
uint64		vpq_catalog_writes = 0;

void
vpq_catalog_init(void)
{
}

/*
 * A snapshot for a catalog read whose caller has none: the active one, or
 * the latest, registered for the read (an index scan's visibility asks one
 * of the two)
 */
Snapshot
vpq_catalog_snapshot(bool *registered)
{
	if (ActiveSnapshotSet())
	{
		*registered = false;
		return GetActiveSnapshot();
	}
	*registered = true;
	return RegisterSnapshot(GetLatestSnapshot());
}

void
vpq_catalog_snapshot_done(Snapshot snapshot, bool registered)
{
	if (registered)
		UnregisterSnapshot(snapshot);
}

/* A catalog of the storage's persistence: its UNLOGGED twin for an UNLOGGED or temporary table */
Relation
vpq_catalog_open(const char *name, char persistence, LOCKMODE lockmode)
{
	Oid			nsp = get_namespace_oid("vexec_parquet", true);
	char	   *relname;
	Oid			relid;

	if (!OidIsValid(nsp))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_SCHEMA),
				 errmsg("the extension vexec_parquet is not installed in this database")));
	relname = persistence == RELPERSISTENCE_PERMANENT ? (char *) name : psprintf("%s_unlogged", name);
	relid = get_relname_relid(relname, nsp);
	if (!OidIsValid(relid))
		elog(ERROR, "vexec_parquet's catalog %s is missing", relname);
	return table_open(relid, lockmode);
}

/* A catalog's only index */
Oid
vpq_catalog_index(Relation catalog)
{
	List	   *indexes = RelationGetIndexList(catalog);

	if (list_length(indexes) != 1)
		elog(ERROR, "vexec_parquet's catalog %s has %d indexes", RelationGetRelationName(catalog),
			 list_length(indexes));
	return linitial_oid(indexes);
}

int
vpq_catalog_storage_keys(const VpqStorage *st, ScanKeyData *keys)
{
	ScanKeyInit(&keys[0], 1, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(st->spc));
	ScanKeyInit(&keys[1], 2, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(st->relnumber));
	ScanKeyInit(&keys[2], 3, BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(st->backend));
	return 3;
}

/* A catalog row and its index entry, under the given command id */
void
vpq_catalog_insert(Relation catalog, HeapTuple tuple, CommandId cid)
{
	Relation	index;
	IndexInfo  *ii;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	TupleTableSlot *slot;
	EState	   *estate;

	heap_insert(catalog, tuple, cid, 0, NULL);
	vpq_catalog_writes++;
	/*
	 * The command's id used, so that the next CommandCounterIncrement()
	 * advances past the row: a caller may pass GetCurrentCommandId(false),
	 * as the RI fast path's table_tuple_lock() does
	 */
	(void) GetCurrentCommandId(true);

	index = index_open(vpq_catalog_index(catalog), RowExclusiveLock);
	ii = BuildIndexInfo(index);
	estate = CreateExecutorState();
	slot = MakeSingleTupleTableSlot(RelationGetDescr(catalog), &TTSOpsHeapTuple);
	ExecStoreHeapTuple(tuple, slot, false);
	GetPerTupleExprContext(estate)->ecxt_scantuple = slot;
	FormIndexDatum(ii, slot, estate, values, isnull);
	(void) index_insert(index, values, isnull, &tuple->t_self, catalog,
						index->rd_index->indisunique ? UNIQUE_CHECK_YES : UNIQUE_CHECK_NO, false, ii);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	index_close(index, RowExclusiveLock);
}

/*
 * The state of a catalog row's inserter, in progress checked before
 * committed (heapam_visibility.c)
 */
VpqWriterState
vpq_writer_state(HeapTupleHeader h)
{
	TransactionId xmin = HeapTupleHeaderGetRawXmin(h);

	if (HeapTupleHeaderXminFrozen(h))
		return VPQ_WRITER_COMMITTED;
	if (h->t_infomask & HEAP_XMIN_INVALID)
		return VPQ_WRITER_ABORTED;
	if (TransactionIdIsCurrentTransactionId(xmin))
		return VPQ_WRITER_US;
	if (h->t_infomask & HEAP_XMIN_COMMITTED)
		return VPQ_WRITER_COMMITTED;
	if (TransactionIdIsInProgress(xmin))
		return VPQ_WRITER_IN_PROGRESS;
	if (TransactionIdDidCommit(xmin))
		return VPQ_WRITER_COMMITTED;
	return VPQ_WRITER_ABORTED;
}

/* Whether a catalog row is gone: deleted by the running transaction or by one that committed */
bool
vpq_row_deleted(HeapTupleHeader h)
{
	TransactionId xmax;

	if (h->t_infomask & HEAP_XMAX_INVALID || HEAP_XMAX_IS_LOCKED_ONLY(h->t_infomask))
		return false;
	if (h->t_infomask & HEAP_XMAX_IS_MULTI)
		return false;
	xmax = HeapTupleHeaderGetRawXmax(h);
	if (!TransactionIdIsValid(xmax))
		return false;
	if (TransactionIdIsCurrentTransactionId(xmax))
		return true;
	if (TransactionIdIsInProgress(xmax))
		return false;
	return TransactionIdDidCommit(xmax);
}

/*
 * Storages and their counters
 */

void
vpq_storage_create(const VpqStorage *st)
{
	Relation	catalog = vpq_catalog_open("storages", st->persistence, RowExclusiveLock);
	Datum		values[Natts_storages];
	bool		nulls[Natts_storages];
	HeapTuple	tuple;

	memset(nulls, false, sizeof(nulls));
	values[Anum_storages_spc - 1] = ObjectIdGetDatum(st->spc);
	values[Anum_storages_relnumber - 1] = ObjectIdGetDatum(st->relnumber);
	values[Anum_storages_backend - 1] = Int32GetDatum(st->backend);
	values[Anum_storages_next_rowid - 1] = Int64GetDatum(0);
	values[Anum_storages_next_file - 1] = Int64GetDatum(1);
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	vpq_catalog_insert(catalog, tuple, GetCurrentCommandId(true));
	heap_freetuple(tuple);
	table_close(catalog, RowExclusiveLock);
	CommandCounterIncrement();
}

bool
vpq_storage_exists(const VpqStorage *st)
{
	Relation	catalog = vpq_catalog_open("storages", st->persistence, AccessShareLock);
	ScanKeyData keys[3];
	SysScanDesc scan;
	bool		found;

	vpq_catalog_storage_keys(st, keys);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, SnapshotSelf, 3, keys);
	found = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return found;
}

/* Every row of a storage in a catalog, as the running transaction sees them, deleted */
static void
delete_rows(const char *name, const VpqStorage *st)
{
	Relation	catalog;
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;

	/*
	 * Rows inserted under the running command's id -- a file the commit's
	 * flush made, before ON COMMIT DELETE ROWS deletes it -- are its
	 * delete's only after the counter moves past them
	 */
	CommandCounterIncrement();
	catalog = vpq_catalog_open(name, st->persistence, RowExclusiveLock);
	vpq_catalog_storage_keys(st, keys);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, SnapshotSelf, 3, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		simple_heap_delete(catalog, &tuple->t_self);
	systable_endscan(scan);
	table_close(catalog, RowExclusiveLock);
	vpq_catalog_writes++;
}

/* A storage's rows gone from every catalog, in the running transaction */
void
vpq_storage_forget(const VpqStorage *st)
{
	delete_rows("files", st);
	delete_rows("tombstones", st);
	delete_rows("storages", st);
	CommandCounterIncrement();
}

/* A storage's files and deletions gone, its counters kept: nontransactional truncation */
void
vpq_storage_truncate(const VpqStorage *st)
{
	delete_rows("files", st);
	delete_rows("tombstones", st);
	CommandCounterIncrement();
}

typedef struct LocalCounters
{
	VpqStorage	st;				/* hash key */
	uint64		next_rowid;
	uint64		next_file;
	bool		dirty;
} LocalCounters;

static HTAB *local_counters = NULL;

static LocalCounters *
local_entry(const VpqStorage *st, bool create, bool *found)
{
	VpqStorage	key;

	memset(&key, 0, sizeof(key));
	key.spc = st->spc;
	key.relnumber = st->relnumber;
	key.backend = st->backend;
	key.persistence = st->persistence;
	if (local_counters == NULL)
	{
		HASHCTL		ctl;

		if (!create)
		{
			*found = false;
			return NULL;
		}
		ctl.keysize = sizeof(VpqStorage);
		ctl.entrysize = sizeof(LocalCounters);
		ctl.hcxt = TopTransactionContext;
		local_counters = hash_create("vexec_parquet counters", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	return hash_search(local_counters, &key, create ? HASH_ENTER : HASH_FIND, found);
}

/* Takes nrowids row ids and nfiles file ids of a storage, returning the first of each */
void
vpq_counters_take(const VpqStorage *st, uint64 nrowids, uint64 nfiles, uint64 *first_rowid,
				  uint64 *first_file)
{
	Relation	catalog;
	ScanKeyData keys[3];
	HeapTuple	oldtup;
	void	   *state;
	bool		found;
	LocalCounters *local = local_entry(st, false, &found);
	Datum		values[Natts_storages];
	bool		nulls[Natts_storages];

	if (local && found)
	{
		*first_rowid = local->next_rowid;
		*first_file = local->next_file;
		if (local->next_rowid + nrowids > VPQ_MAX_ROWID)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("the vexec_parquet table has used all its row ids")));
		local->next_rowid += nrowids;
		local->next_file += nfiles;
		local->dirty = true;
		return;
	}

	catalog = vpq_catalog_open("storages", st->persistence, RowExclusiveLock);
	vpq_catalog_storage_keys(st, keys);
	{
		SysScanDesc scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, SnapshotSelf, 3, keys);
		HeapTuple	tuple = systable_getnext(scan);

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "vexec_parquet: no storage %u/%u/%d", st->spc, st->relnumber, st->backend);
		if (TransactionIdIsCurrentTransactionId(HeapTupleHeaderGetXmin(tuple->t_data)))
		{
			heap_deform_tuple(tuple, RelationGetDescr(catalog), values, nulls);
			systable_endscan(scan);
			table_close(catalog, RowExclusiveLock);
			local = local_entry(st, true, &found);
			local->next_rowid = DatumGetInt64(values[Anum_storages_next_rowid - 1]);
			local->next_file = DatumGetInt64(values[Anum_storages_next_file - 1]);
			local->dirty = false;
			vpq_counters_take(st, nrowids, nfiles, first_rowid, first_file);
			return;
		}
		systable_endscan(scan);
	}

	systable_inplace_update_begin(catalog, vpq_catalog_index(catalog), true, NULL, 3, keys, &oldtup, &state);
	if (!HeapTupleIsValid(oldtup))
		elog(ERROR, "vexec_parquet: no storage %u/%u/%d", st->spc, st->relnumber, st->backend);
	heap_deform_tuple(oldtup, RelationGetDescr(catalog), values, nulls);
	*first_rowid = DatumGetInt64(values[Anum_storages_next_rowid - 1]);
	*first_file = DatumGetInt64(values[Anum_storages_next_file - 1]);
	if (*first_rowid + nrowids > VPQ_MAX_ROWID)
	{
		systable_inplace_update_cancel(state);
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("the vexec_parquet table has used all its row ids")));
	}
	{
		HeapTuple	newtup;
		Datum		v[Natts_storages];
		bool		n[Natts_storages];
		bool		repl[Natts_storages];

		memset(repl, false, sizeof(repl));
		memset(n, false, sizeof(n));
		v[Anum_storages_next_rowid - 1] = Int64GetDatum(*first_rowid + nrowids);
		v[Anum_storages_next_file - 1] = Int64GetDatum(*first_file + nfiles);
		repl[Anum_storages_next_rowid - 1] = true;
		repl[Anum_storages_next_file - 1] = true;
		newtup = heap_modify_tuple(oldtup, RelationGetDescr(catalog), v, n, repl);
		systable_inplace_update_finish(state, newtup);
		heap_freetuple(newtup);
	}
	heap_freetuple(oldtup);
	table_close(catalog, RowExclusiveLock);
}

/* The counters kept in the backend, written before the transaction commits */
static void
write_local_counters(void)
{
	HASH_SEQ_STATUS seq;
	LocalCounters *local;

	if (local_counters == NULL)
		return;
	hash_seq_init(&seq, local_counters);
	while ((local = hash_seq_search(&seq)) != NULL)
	{
		Relation	catalog;
		ScanKeyData keys[3];
		SysScanDesc scan;
		HeapTuple	tuple;

		if (!local->dirty)
			continue;
		catalog = vpq_catalog_open("storages", local->st.persistence, RowExclusiveLock);
		vpq_catalog_storage_keys(&local->st, keys);
		scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, SnapshotSelf, 3, keys);
		tuple = systable_getnext(scan);
		/* a storage its own subtransaction's abort took with it is no more */
		if (HeapTupleIsValid(tuple))
		{
			Datum		v[Natts_storages];
			bool		n[Natts_storages];
			bool		repl[Natts_storages];
			HeapTuple	newtup;

			memset(repl, false, sizeof(repl));
			memset(n, false, sizeof(n));
			v[Anum_storages_next_rowid - 1] = Int64GetDatum(local->next_rowid);
			v[Anum_storages_next_file - 1] = Int64GetDatum(local->next_file);
			repl[Anum_storages_next_rowid - 1] = true;
			repl[Anum_storages_next_file - 1] = true;
			newtup = heap_modify_tuple(tuple, RelationGetDescr(catalog), v, n, repl);
			CatalogTupleUpdate(catalog, &tuple->t_self, newtup);
			heap_freetuple(newtup);
		}
		systable_endscan(scan);
		table_close(catalog, RowExclusiveLock);
		local->dirty = false;
	}
}

bool
vpq_counters_pending(void)
{
	HASH_SEQ_STATUS seq;
	LocalCounters *local;

	if (local_counters == NULL)
		return false;
	hash_seq_init(&seq, local_counters);
	while ((local = hash_seq_search(&seq)) != NULL)
		if (local->dirty)
		{
			hash_seq_term(&seq);
			return true;
		}
	return false;
}

void
vpq_counters_xact_callback(XactEvent event)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			write_local_counters();
			break;
		default:
			/* TopTransactionContext takes the table with it */
			local_counters = NULL;
			break;
	}
}

/*
 * Files
 */

static VpqFileRow *
deform_file(Relation catalog, HeapTuple tuple)
{
	Datum		values[Natts_files];
	bool		nulls[Natts_files];
	VpqFileRow *row = palloc0(sizeof(VpqFileRow));

	heap_deform_tuple(tuple, RelationGetDescr(catalog), values, nulls);
	row->file_id = DatumGetInt64(values[Anum_files_file_id - 1]);
	row->first_rowid = (uint64) DatumGetInt64(values[Anum_files_first_rowid - 1]);
	row->rows = DatumGetInt64(values[Anum_files_rows - 1]);
	row->bytes = DatumGetInt64(values[Anum_files_bytes - 1]);
	row->fxid = DatumGetInt64(values[Anum_files_fxid - 1]);
	row->stats = nulls[Anum_files_stats - 1] ? NULL :
		DatumGetByteaPCopy(values[Anum_files_stats - 1]);
	row->xmin = HeapTupleHeaderXminFrozen(tuple->t_data) ? FrozenTransactionId :
		HeapTupleHeaderGetRawXmin(tuple->t_data);
	return row;
}

/* The storage's files the snapshot sees, by their first row ids */
List *
vpq_files_visible(const VpqStorage *st, Snapshot snapshot)
{
	Relation	catalog = vpq_catalog_open("files", st->persistence, AccessShareLock);
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *files = NIL;

	vpq_catalog_storage_keys(st, keys);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, snapshot, 3, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		files = lappend(files, deform_file(catalog, tuple));
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return files;
}

/* Every file of the storage that a transaction may still see: its writer did not abort */
List *
vpq_files_all(const VpqStorage *st)
{
	Relation	catalog = vpq_catalog_open("files", st->persistence, AccessShareLock);
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	List	   *files = NIL;

	vpq_catalog_storage_keys(st, keys);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, SnapshotAny, 3, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		if (vpq_writer_state(tuple->t_data) == VPQ_WRITER_ABORTED)
			continue;
		files = lappend(files, deform_file(catalog, tuple));
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return files;
}

/*
 * The file holding a row, as the snapshot sees it: an MVCC, self or any
 * snapshot through the catalog's own visibility; a dirty one as heap's
 * HeapTupleSatisfiesDirty() judges a tuple, a file a transaction in progress
 * writes visible, its writer in xmin (*writer)
 */
bool
vpq_file_of_row(const VpqStorage *st, uint64 rowid, Snapshot snapshot, VpqFileRow *out)
{
	Relation	catalog = vpq_catalog_open("files", st->persistence, AccessShareLock);
	Relation	index = index_open(vpq_catalog_index(catalog), AccessShareLock);
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		dirty = snapshot != NULL && snapshot->snapshot_type == SNAPSHOT_DIRTY;
	bool		found = false;

	vpq_catalog_storage_keys(st, keys);
	ScanKeyInit(&keys[3], Anum_files_first_rowid, BTLessEqualStrategyNumber, F_INT8LE,
				Int64GetDatum((int64) rowid));
	scan = systable_beginscan_ordered(catalog, index, dirty ? SnapshotAny : snapshot, 4, keys);
	while (HeapTupleIsValid(tuple = systable_getnext_ordered(scan, BackwardScanDirection)))
	{
		VpqFileRow *row;

		if (dirty || snapshot == SnapshotAny)
		{
			VpqWriterState ws = vpq_writer_state(tuple->t_data);

			if (ws == VPQ_WRITER_ABORTED || vpq_row_deleted(tuple->t_data))
				continue;
			if (dirty && ws == VPQ_WRITER_IN_PROGRESS)
				snapshot->xmin = HeapTupleHeaderGetRawXmin(tuple->t_data);
		}
		row = deform_file(catalog, tuple);
		if (rowid >= row->first_rowid && rowid < row->first_rowid + (uint64) row->rows)
		{
			*out = *row;
			found = true;
		}
		/* the greatest first row id at or below the row's is the only file that can hold it */
		break;
	}
	systable_endscan_ordered(scan);
	index_close(index, AccessShareLock);
	table_close(catalog, AccessShareLock);
	return found;
}

void
vpq_file_insert(const VpqStorage *st, const VpqFileRow *row, CommandId cid)
{
	Relation	catalog = vpq_catalog_open("files", st->persistence, RowExclusiveLock);
	Datum		values[Natts_files];
	bool		nulls[Natts_files];
	HeapTuple	tuple;

	memset(nulls, false, sizeof(nulls));
	values[Anum_files_spc - 1] = ObjectIdGetDatum(st->spc);
	values[Anum_files_relnumber - 1] = ObjectIdGetDatum(st->relnumber);
	values[Anum_files_backend - 1] = Int32GetDatum(st->backend);
	values[Anum_files_file_id - 1] = Int64GetDatum(row->file_id);
	values[Anum_files_first_rowid - 1] = Int64GetDatum((int64) row->first_rowid);
	values[Anum_files_rows - 1] = Int64GetDatum(row->rows);
	values[Anum_files_bytes - 1] = Int64GetDatum(row->bytes);
	values[Anum_files_fxid - 1] = Int64GetDatum(row->fxid);
	if (row->stats)
		values[Anum_files_stats - 1] = PointerGetDatum(row->stats);
	else
		nulls[Anum_files_stats - 1] = true;
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	vpq_catalog_insert(catalog, tuple, cid);
	heap_freetuple(tuple);
	table_close(catalog, RowExclusiveLock);
}

/*
 * A storage's files, rows and counters given to another storage: ALTER
 * TABLE ... SET TABLESPACE, the files copied into the new storage's
 * directory
 */
void
vpq_files_copy(const VpqStorage *from, const VpqStorage *to, const char *from_dir, const char *to_dir)
{
	List	   *files = vpq_files_visible(from, SnapshotSelf);
	ListCell   *lc;
	uint64		first_rowid;
	uint64		first_file;
	uint64		max_rowid = 0;
	uint64		max_file = 0;

	if (!vpq_is_s3(to_dir))
		VPQ_CHECK(vpq_make_dir(to_dir), "making a storage's directory");
	foreach(lc, files)
	{
		VpqFileRow *row = lfirst(lc);
		char	   *src = vpq_file_path(from_dir, row->file_id, (uint64) row->fxid);
		char	   *dst = vpq_file_path(to_dir, row->file_id, (uint64) row->fxid);

		vpq_pending_delete(dst, false, false);
		VPQ_CHECK(vpq_copy_file(src, dst), psprintf("copying \"%s\"", src));
		vpq_file_insert(to, row, GetCurrentCommandId(true));
		max_rowid = Max(max_rowid, row->first_rowid + (uint64) row->rows);
		max_file = Max(max_file, (uint64) row->file_id + 1);
	}
	vpq_tombstones_copy(from, to);
	/* the counters past every row id and file id the old storage gave */
	vpq_counters_take(from, 0, 0, &first_rowid, &first_file);
	max_rowid = Max(max_rowid, first_rowid);
	max_file = Max(max_file, first_file);
	vpq_counters_take(to, max_rowid, max_file, &first_rowid, &first_file);
	CommandCounterIncrement();
}

/* The storage's rows, bytes and files the snapshot sees */
void
vpq_totals(const VpqStorage *st, Snapshot snapshot, int64 *rows, int64 *bytes, int64 *files)
{
	Relation	catalog = vpq_catalog_open("files", st->persistence, AccessShareLock);
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;

	*rows = *bytes = *files = 0;
	vpq_catalog_storage_keys(st, keys);
	scan = systable_beginscan(catalog, vpq_catalog_index(catalog), true, snapshot, 3, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;

		*rows += DatumGetInt64(heap_getattr(tuple, Anum_files_rows, RelationGetDescr(catalog), &isnull));
		*bytes += DatumGetInt64(heap_getattr(tuple, Anum_files_bytes, RelationGetDescr(catalog), &isnull));
		(*files)++;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
}
