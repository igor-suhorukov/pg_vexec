/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * am.c
 *	  The table access method vexec_parquet: its handler, and the callbacks
 *	  scan.c, dml.c and write.c do not hold.
 *
 *	  A table of its own files gets a storage -- its catalogs' key -- with
 *	  each relfilenumber: CREATE TABLE, TRUNCATE and a rewrite make a new
 *	  one, and drop the old one's rows at once and its directory where the
 *	  transaction commits.  VACUUM removes the files no catalog row names
 *	  whose writer is no longer running -- an aborted or crashed writer's --
 *	  and VACUUM FULL and CLUSTER write the table's visible rows into a new
 *	  storage, folding its deletions out.  An index takes every row of every
 *	  file a transaction may see, a deleted row as heap's recently dead one.
 *
 *	  A table that reads files (location) is read-only: its writes are
 *	  refused, TRUNCATE leaves its files with a NOTICE, as pg_restore -j
 *	  runs it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/index.h"
#include "catalog/pg_am_d.h"
#include "commands/progress.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"

#include "vexec_parquet.h"

static TableAmRoutine routine;
static TupleTableSlotOps slot_ops;
static bool routine_made = false;

static HeapTuple
vpq_slot_copy_heap_tuple(TupleTableSlot *slot)
{
	HeapTuple	tuple = TTSOpsVirtual.copy_heap_tuple(slot);

	tuple->t_self = slot->tts_tid;
	tuple->t_tableOid = slot->tts_tableOid;
	return tuple;
}

/*
 * The xmax of a slot's row, from its table's tombstones: none for a row of a
 * table that reads files, or for a slot that holds no table's row
 */
static TransactionId
row_xmax(TupleTableSlot *slot)
{
	Relation	rel;
	uint64		rowid;
	TransactionId xmax = InvalidTransactionId;

	if (!OidIsValid(slot->tts_tableOid) || !vpq_tid_rowid(&slot->tts_tid, &rowid))
		return InvalidTransactionId;
	rel = RelationIdGetRelation(slot->tts_tableOid);
	if (!RelationIsValid(rel))
		return InvalidTransactionId;
	if (rel->rd_tableam == vpq_routine() && !vpq_table(rel)->external)
	{
		VpqStorage	st;

		vpq_storage_of(rel, &rel->rd_locator, &st);
		xmax = vpq_row_xmax(&st, rowid);
	}
	RelationClose(rel);
	return xmax;
}

/*
 * A row's system columns: xmin its writer's where the access method knows it
 * -- a row the running transaction buffered (ON CONFLICT's and MERGE's
 * "cannot affect row a second time" read it) -- else a frozen row's; xmax
 * the transaction that deletes, updates or locks it, as heap's (DELETE ...
 * RETURNING xmax, and an upsert's RETURNING xmax = 0 telling an insert from
 * an update); cmin and cmax none
 */
static Datum
vpq_slot_getsysattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	VpqSlot    *s = (VpqSlot *) slot;

	*isnull = false;
	switch (attnum)
	{
		case MinTransactionIdAttributeNumber:
			return TransactionIdGetDatum(TransactionIdIsValid(s->xmin) ? s->xmin : FrozenTransactionId);
		case MaxTransactionIdAttributeNumber:
			if (!s->xmax_known)
			{
				s->xmax = row_xmax(slot);
				s->xmax_known = true;
			}
			return TransactionIdGetDatum(s->xmax);
		case MinCommandIdAttributeNumber:
		case MaxCommandIdAttributeNumber:
			return CommandIdGetDatum(FirstCommandId);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a vexec_parquet row has no system column %d", attnum)));
	}
	return (Datum) 0;
}

/* A row's writer, on a slot of the access method's (another slot keeps none) */
void
vpq_slot_set_xmin(TupleTableSlot *slot, TransactionId xmin)
{
	if (slot->tts_ops == &slot_ops)
		((VpqSlot *) slot)->xmin = xmin;
}

/* A row's xmax where the access method knows it: a new row's, or an update's */
void
vpq_slot_set_xmax(TupleTableSlot *slot, TransactionId xmax)
{
	if (slot->tts_ops == &slot_ops)
	{
		((VpqSlot *) slot)->xmax = xmax;
		((VpqSlot *) slot)->xmax_known = true;
	}
}

/* Whether the row's writer is the running transaction: RI's triggers ask of a PK row */
static bool
vpq_slot_is_current_xact_tuple(TupleTableSlot *slot)
{
	TransactionId xmin = ((VpqSlot *) slot)->xmin;

	return TransactionIdIsValid(xmin) && TransactionIdIsNormal(xmin) &&
		TransactionIdIsCurrentTransactionId(xmin);
}

static void
vpq_slot_clear(TupleTableSlot *slot)
{
	((VpqSlot *) slot)->xmin = InvalidTransactionId;
	((VpqSlot *) slot)->xmax = InvalidTransactionId;
	((VpqSlot *) slot)->xmax_known = false;
	TTSOpsVirtual.clear(slot);
}

static const TupleTableSlotOps *
vpq_slot_callbacks(Relation rel)
{
	return &slot_ops;
}

static void
refuse_write(Relation rel)
{
	if (vpq_table(rel)->external)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a vexec_parquet table that reads files is read-only: \"%s\"",
						RelationGetRelationName(rel)),
				 errhint("Make a table of its own files, without location, to write into.")));
}

/*
 * Index fetches
 */
static IndexFetchTableData *
vpq_index_fetch_begin(Relation rel, uint32 flags)
{
	IndexFetchTableData *scan = palloc0(sizeof(IndexFetchTableData));

	scan->rel = rel;
	scan->flags = flags;
	return scan;
}

static void
vpq_index_fetch_reset(IndexFetchTableData *scan)
{
}

static void
vpq_index_fetch_end(IndexFetchTableData *scan)
{
	pfree(scan);
}

static bool
vpq_index_fetch_tuple(IndexFetchTableData *scan, ItemPointer tid, Snapshot snapshot,
					  TupleTableSlot *slot, bool *call_again, bool *all_dead)
{
	*call_again = false;
	if (all_dead)
		*all_dead = false;
	return vpq_fetch_row(scan->rel, tid, snapshot, slot);
}

static bool
vpq_tuple_fetch_row_version(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
{
	return vpq_fetch_row(rel, tid, snapshot, slot);
}

/* A TID of a row id a file of the scan holds, or the write buffer (heapam_tuple_tid_valid()'s block test) */
static bool
vpq_tuple_tid_valid(TableScanDesc scan, ItemPointer tid)
{
	uint64		rowid;
	CommandId	cid;

	if (!vpq_tid_rowid(tid, &rowid))
		return false;
	if (vpq_buffer_row_cid(scan->rs_rd, rowid, &cid))
		return true;
	return vpq_file_entry_of_row(vpq_scan_listing(scan), rowid) >= 0;
}

static bool
vpq_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot, Snapshot snapshot)
{
	VpqTable   *t = vpq_table(rel);
	uint64		rowid;
	VpqFileRow	row;

	if (t->external)
		return true;
	if (!vpq_tid_rowid(&slot->tts_tid, &rowid))
		return false;
	/* a row of the write buffer: the running transaction's, as the snapshot sees its command */
	if (!vpq_buffer_has_row(rel, rowid, snapshot) && !vpq_file_of_row(&t->storage, rowid, snapshot, &row))
		return false;
	return vpq_row_survives(&t->storage, rowid, snapshot);
}

static TransactionId
vpq_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	/*
	 * A row's deletion is a tombstone, not a dead tuple: bottom-up deletion's
	 * guesses find nothing (its final ndeltids 0), and simple deletion's
	 * entries are the index's own LP_DEAD ones, which stay as it marked them
	 */
	if (delstate->bottomup)
		delstate->ndeltids = 0;
	return InvalidTransactionId;
}

/*
 * Inserts
 */
static void
vpq_tuple_insert(Relation rel, TupleTableSlot *slot, CommandId cid, uint32 options,
				 BulkInsertStateData *bistate)
{
	refuse_write(rel);
	vpq_buffer_insert(rel, slot, cid, true);
}

static void
vpq_tuple_insert_speculative(Relation rel, TupleTableSlot *slot, CommandId cid, uint32 options,
							 BulkInsertStateData *bistate, uint32 specToken)
{
	refuse_write(rel);
	vpq_buffer_insert(rel, slot, cid, true);
}

/*
 * A speculative insertion that met a conflict: its row deleted under the
 * command's id, as heap_abort_speculative() kills its tuple
 */
static void
vpq_tuple_complete_speculative(Relation rel, TupleTableSlot *slot, uint32 specToken, bool succeeded)
{
	VpqTable   *t;

	if (succeeded)
		return;
	t = vpq_table(rel);
	vpq_tombstone_insert(&t->storage, VPQ_TID_TO_ROWID(&slot->tts_tid), VPQ_TOMB_DELETED, 0,
						 GetCurrentCommandId(true));
}

static void
vpq_multi_insert(Relation rel, TupleTableSlot **slots, int nslots, CommandId cid, uint32 options,
				 BulkInsertStateData *bistate)
{
	refuse_write(rel);
	for (int i = 0; i < nslots; i++)
		vpq_buffer_insert(rel, slots[i], cid, true);
}

static void
vpq_finish_bulk_insert(Relation rel, uint32 options)
{
	vpq_buffer_flush_rel(RelationGetRelid(rel));
}

/*
 * Storage
 */
static char *
storage_dir_of(Relation rel, const VpqStorage *st)
{
	VpqOptions	opts;
	char	   *root;

	vpq_relation_options(rel, &opts);
	root = vpq_storage_root_of(&opts, st);
	return vpq_storage_dir(st, root, MyDatabaseId);
}

static void
vpq_relation_set_new_filelocator(Relation rel, const RelFileLocator *newrlocator, char persistence,
								 TransactionId *freezeXid, MultiXactId *minmulti)
{
	VpqOptions	opts;
	VpqStorage	st;
	VpqStorage	old;

	*freezeXid = InvalidTransactionId;
	*minmulti = InvalidMultiXactId;
	vpq_relation_options(rel, &opts);
	if (opts.location != NULL)
	{
		/* a new relation's, or TRUNCATE's of a table that reads files: its files stay */
		if (rel->rd_locator.relNumber != newrlocator->relNumber && OidIsValid(RelationGetRelid(rel)) &&
			rel->rd_rel->relkind == RELKIND_RELATION && rel->rd_createSubid == InvalidSubTransactionId)
			ereport(NOTICE,
					(errmsg("TRUNCATE leaves the files of \"%s\", which it reads from its location",
							RelationGetRelationName(rel))));
		return;
	}

	/* the old storage, where this is TRUNCATE's or a SET LOGGED's new one */
	vpq_storage_of(rel, &rel->rd_locator, &old);
	if ((rel->rd_locator.relNumber != newrlocator->relNumber || rel->rd_locator.spcOid != newrlocator->spcOid) &&
		vpq_storage_exists(&old))
	{
		vpq_buffer_discard_rel(RelationGetRelid(rel));
		vpq_storage_drop_later(&old, storage_dir_of(rel, &old));
	}

	memset(&st, 0, sizeof(st));
	st.spc = newrlocator->spcOid;
	st.relnumber = newrlocator->relNumber;
	st.backend = persistence == RELPERSISTENCE_TEMP ? rel->rd_backend : -1;
	if (persistence == RELPERSISTENCE_TEMP && st.backend < 0)
		st.backend = MyProcNumber;
	st.persistence = persistence;
	vpq_storage_create(&st);
	vpq_note_new_storage(&st);
}

/* Every row gone, at once: a table made in the running transaction, ON COMMIT DELETE ROWS */
static void
vpq_relation_nontransactional_truncate(Relation rel)
{
	VpqTable   *t = vpq_table(rel);
	List	   *files;
	ListCell   *lc;

	if (t->external)
		return;
	vpq_buffer_discard_rel(RelationGetRelid(rel));
	files = vpq_files_all(&t->storage);
	foreach(lc, files)
	{
		VpqFileRow *row = lfirst(lc);

		vpq_pending_delete(vpq_file_path(t->dir, row->file_id, (uint64) row->fxid), false, true);
	}
	/* ON COMMIT DELETE ROWS runs at commit, where no snapshot is active */
	{
		bool		pushed = !ActiveSnapshotSet();

		if (pushed)
			PushActiveSnapshot(GetLatestSnapshot());
		vpq_storage_truncate(&t->storage);
		if (pushed)
			PopActiveSnapshot();
	}
}

/* ALTER TABLE ... SET TABLESPACE: the files copied into the new storage's directory */
static void
vpq_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	VpqTable   *t = vpq_table(rel);
	VpqStorage	to;
	char	   *to_dir;

	if (t->external)
		return;
	vpq_buffer_flush_rel(RelationGetRelid(rel));
	memset(&to, 0, sizeof(to));
	to.spc = newrlocator->spcOid;
	to.relnumber = newrlocator->relNumber;
	to.backend = t->storage.backend;
	to.persistence = t->storage.persistence;
	vpq_storage_create(&to);
	vpq_note_new_storage(&to);
	to_dir = vpq_storage_dir(&to, t->root, MyDatabaseId);
	vpq_files_copy(&t->storage, &to, t->dir, to_dir);
	vpq_storage_drop_later(&t->storage, t->dir);
}

/*
 * VACUUM FULL, CLUSTER: the rows the snapshot sees written into the new
 * relation's storage, in the index's order where one is given
 */
static void
vpq_relation_copy_for_cluster(Relation OldTable, Relation NewTable, Relation OldIndex, bool use_sort,
							  TransactionId OldestXmin, Snapshot snapshot, TransactionId *xid_cutoff,
							  MultiXactId *multi_cutoff, double *num_tuples, double *tups_vacuumed,
							  double *tups_recently_dead)
{
	TableScanDesc scan;
	TupleTableSlot *slot;
	Tuplesortstate *sort = NULL;
	CommandId	cid = GetCurrentCommandId(true);
	int64		total = 0;

	refuse_write(OldTable);
	*xid_cutoff = InvalidTransactionId;
	*multi_cutoff = InvalidMultiXactId;
	*num_tuples = 0;
	*tups_vacuumed = 0;
	*tups_recently_dead = 0;
	vpq_buffer_flush_rel(RelationGetRelid(OldTable));
	if (snapshot == NULL)
		snapshot = GetActiveSnapshot();
	if (OldIndex != NULL)
		sort = tuplesort_begin_cluster(RelationGetDescr(OldTable), OldIndex, maintenance_work_mem, NULL,
									   TUPLESORT_NONE);
	slot = table_slot_create(OldTable, NULL);
	scan = table_beginscan(OldTable, snapshot, 0, NULL, 0);
	{
		VpqListing *l = vpq_scan_listing(scan);

		total = l->rows;
	}
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		CHECK_FOR_INTERRUPTS();
		if (sort)
		{
			HeapTuple	tuple = ExecCopySlotHeapTuple(slot);

			tuplesort_putheaptuple(sort, tuple);
			heap_freetuple(tuple);
		}
		else
			vpq_buffer_insert(NewTable, slot, cid, false);
		*num_tuples += 1;
	}
	table_endscan(scan);
	if (sort)
	{
		HeapTuple	tuple;

		tuplesort_performsort(sort);
		while ((tuple = tuplesort_getheaptuple(sort, true)) != NULL)
		{
			CHECK_FOR_INTERRUPTS();
			ExecForceStoreHeapTuple(heap_copytuple(tuple), slot, true);
			vpq_buffer_insert(NewTable, slot, cid, false);
		}
		tuplesort_end(sort);
	}
	ExecDropSingleTupleTableSlot(slot);
	vpq_buffer_flush_rel(RelationGetRelid(NewTable));
	*tups_vacuumed = (double) Max(total - (int64) *num_tuples, 0);
}

typedef struct DeadRows
{
	uint64	   *ids;
	int			n;
} DeadRows;

static bool
dead_row(ItemPointer itemptr, void *state)
{
	DeadRows   *d = state;
	uint64		rowid;
	int			lo = 0;
	int			hi = d->n - 1;

	if (!vpq_tid_rowid(itemptr, &rowid))
		return false;
	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (d->ids[mid] == rowid)
			return true;
		if (d->ids[mid] < rowid)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return false;
}

/*
 * The indexes' entries of rows every snapshot sees deleted removed, and
 * each index's cleanup run -- a GIN index's pending list among it
 */
static void
vacuum_indexes(Relation rel, BufferAccessStrategy bstrategy, double rows, DeadRows *dead,
			   bool bulk_delete)
{
	Relation   *indrels;
	int			nindexes;

	vac_open_indexes(rel, RowExclusiveLock, &nindexes, &indrels);
	for (int i = 0; i < nindexes; i++)
	{
		IndexVacuumInfo ivinfo;
		IndexBulkDeleteResult *stats = NULL;

		memset(&ivinfo, 0, sizeof(ivinfo));
		ivinfo.index = indrels[i];
		ivinfo.heaprel = rel;
		ivinfo.analyze_only = false;
		ivinfo.report_progress = false;
		ivinfo.estimated_count = true;
		ivinfo.message_level = DEBUG2;
		ivinfo.num_heap_tuples = rows;
		ivinfo.strategy = bstrategy;
		if (bulk_delete && dead->n > 0)
			stats = index_bulk_delete(&ivinfo, stats, dead_row, dead);
		stats = index_vacuum_cleanup(&ivinfo, stats);
		if (stats)
			pfree(stats);
	}
	vac_close_indexes(nindexes, indrels, NoLock);
}

/*
 * VACUUM: the storage's files no catalog row names, whose writer no longer
 * runs, removed; the indexes' entries of rows every snapshot sees deleted;
 * the relation's sizes as its files have them
 */
static void
vpq_relation_vacuum(Relation rel, const VacuumParams *params, BufferAccessStrategy bstrategy)
{
	VpqTable   *t = vpq_table(rel);
	TimestampTz starttime = GetCurrentTimestamp();
	int64		rows;
	int64		bytes;
	int64		nfiles;
	int64		recently_dead;
	int64		live;
	DeadRows	dead;

	if (t->external)
		return;
	vpq_sweep(rel, t);
	{
		bool		registered;
		Snapshot	snapshot = vpq_catalog_snapshot(&registered);

		vpq_totals(&t->storage, snapshot, &rows, &bytes, &nfiles);
		vpq_catalog_snapshot_done(snapshot, registered);
	}

	/*
	 * A deleted row stays in its file: the index entries of rows dead to
	 * every snapshot go, and the rows dead to one that still runs are the
	 * dead rows reported -- those a later VACUUM takes out of the indexes
	 */
	dead.ids = vpq_tombstones_dead_to_all(&t->storage, GetOldestNonRemovableTransactionId(rel),
										  &dead.n, &recently_dead);
	live = Max(rows - dead.n - recently_dead, 0);
	vacuum_indexes(rel, bstrategy, (double) live, &dead,
				   params->index_cleanup != VACOPTVALUE_DISABLED);
	vac_update_relstats(rel, (BlockNumber) Min((bytes + BLCKSZ - 1) / BLCKSZ, (int64) MaxBlockNumber),
						(double) live, 0, 0, RelationGetForm(rel)->relhasindex,
						InvalidTransactionId, InvalidMultiXactId, NULL, NULL, false);
	pgstat_report_vacuum(rel, live, recently_dead, starttime);
}

/*
 * Index builds: every row of every file a transaction may see -- a file of
 * a writer that did not abort -- a row deleted by a transaction that did not
 * abort handed as not alive, as heap hands a recently dead tuple, so that a
 * unique index checks the live rows alone
 */
static double
vpq_index_build_range_scan(Relation table_rel, Relation index_rel, IndexInfo *index_info,
						   bool allow_sync, bool anyvisible, bool progress, BlockNumber start_blockno,
						   BlockNumber numblocks, IndexBuildCallback callback, void *callback_state,
						   TableScanDesc scan)
{
	VpqTable   *t = vpq_table(table_rel);
	EState	   *estate;
	ExprContext *econtext;
	TupleTableSlot *slot;
	ExprState  *predicate;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	double		reltuples = 0;
	uint64		lo = 0;
	uint64		hi = PG_UINT64_MAX;
	VpqDeadRow *dead;
	int			ndead;
	int			k = 0;
	bool		own_scan = true;

	if (t->external)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a vexec_parquet table that reads files cannot be indexed"),
				 errdetail("Its row identifiers last as long as a listing of its files.")));
	vpq_buffer_flush_rel(RelationGetRelid(table_rel));
	if (numblocks != InvalidBlockNumber)
	{
		lo = (uint64) start_blockno * VPQ_ROWS_PER_BLOCK;
		hi = ((uint64) start_blockno + numblocks) * VPQ_ROWS_PER_BLOCK;
	}
	dead = vpq_tombstones_all(&t->storage, &ndead);

	estate = CreateExecutorState();
	econtext = GetPerTupleExprContext(estate);
	slot = table_slot_create(table_rel, NULL);
	econtext->ecxt_scantuple = slot;
	predicate = ExecPrepareQual(index_info->ii_Predicate, estate);

	/* a parallel build's participants share the scan its leader began (with SnapshotAny) */
	if (scan == NULL)
		scan = vpq_scan_begin_all(table_rel);
	else
		own_scan = false;
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		uint64		rowid = VPQ_TID_TO_ROWID(&slot->tts_tid);
		bool		alive = true;

		CHECK_FOR_INTERRUPTS();
		if (rowid < lo || rowid >= hi)
			continue;
		while (k < ndead && dead[k].rowid < rowid)
			k++;
		if (k < ndead && dead[k].rowid == rowid)
			alive = false;
		MemoryContextReset(econtext->ecxt_per_tuple_memory);
		if (predicate != NULL && !ExecQual(predicate, econtext))
			continue;
		FormIndexDatum(index_info, slot, estate, values, isnull);
		callback(index_rel, &slot->tts_tid, values, isnull, alive, callback_state);
		reltuples += 1;
	}
	if (own_scan)
		table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	index_info->ii_ExpressionsState = NIL;
	index_info->ii_PredicateState = NULL;
	return reltuples;
}

static void
vpq_index_validate_scan(Relation table_rel, Relation index_rel, IndexInfo *index_info,
						Snapshot snapshot, ValidateIndexState *state)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("CREATE INDEX CONCURRENTLY is not supported on a vexec_parquet table")));
}

static uint64
vpq_relation_size(Relation rel, ForkNumber forkNumber)
{
	VpqTable   *t;
	int64		rows;
	int64		bytes;
	int64		nfiles;

	if (forkNumber != MAIN_FORKNUM && forkNumber != InvalidForkNumber)
		return 0;
	t = vpq_table(rel);
	if (t->external)
	{
		VpqListing *l = vpq_listing(rel, t, NULL);

		bytes = l->bytes;
		vpq_listing_release(l);
		return (uint64) bytes;
	}
	{
		bool		registered;
		Snapshot	snapshot = vpq_catalog_snapshot(&registered);

		vpq_totals(&t->storage, snapshot, &rows, &bytes, &nfiles);
		vpq_catalog_snapshot_done(snapshot, registered);
	}
	return (uint64) bytes;
}

static bool
vpq_relation_needs_toast_table(Relation rel)
{
	return false;
}

static Oid
vpq_relation_toast_am(Relation rel)
{
	return HEAP_TABLE_AM_OID;
}

static void
vpq_relation_estimate_size(Relation rel, int32 *attr_widths, BlockNumber *pages, double *tuples,
						   double *allvisfrac)
{
	VpqTable   *t = vpq_table(rel);
	int64		rows;
	int64		bytes;
	int64		nfiles;

	if (t->external)
	{
		VpqListing *l = vpq_listing(rel, t, NULL);

		rows = l->rows;
		bytes = l->bytes;
		vpq_listing_release(l);
	}
	else
	{
		bool		registered;
		Snapshot	snapshot = vpq_catalog_snapshot(&registered);

		vpq_totals(&t->storage, snapshot, &rows, &bytes, &nfiles);
		vpq_catalog_snapshot_done(snapshot, registered);
	}
	*tuples = (double) rows;
	*pages = rows > 0 ? (BlockNumber) Max(Min((bytes + BLCKSZ - 1) / BLCKSZ, (int64) MaxBlockNumber), 1) : 0;
	*allvisfrac = 0;
}

static void
make_routine(void)
{
	TableAmRoutine *r = &routine;

	memset(r, 0, sizeof(*r));
	r->type = T_TableAmRoutine;
	r->slot_callbacks = vpq_slot_callbacks;
	r->scan_begin = vpq_scan_begin;
	r->scan_end = vpq_scan_end;
	r->scan_rescan = vpq_scan_rescan;
	r->scan_getnextslot = vpq_scan_getnextslot;
	r->parallelscan_estimate = vpq_parallelscan_estimate;
	r->parallelscan_initialize = vpq_parallelscan_initialize;
	r->parallelscan_reinitialize = vpq_parallelscan_reinitialize;
	r->index_fetch_begin = vpq_index_fetch_begin;
	r->index_fetch_reset = vpq_index_fetch_reset;
	r->index_fetch_end = vpq_index_fetch_end;
	r->index_fetch_tuple = vpq_index_fetch_tuple;
	r->tuple_fetch_row_version = vpq_tuple_fetch_row_version;
	r->tuple_tid_valid = vpq_tuple_tid_valid;
	r->tuple_get_latest_tid = vpq_tuple_get_latest_tid;
	r->tuple_satisfies_snapshot = vpq_tuple_satisfies_snapshot;
	r->index_delete_tuples = vpq_index_delete_tuples;
	r->tuple_insert = vpq_tuple_insert;
	r->tuple_insert_speculative = vpq_tuple_insert_speculative;
	r->tuple_complete_speculative = vpq_tuple_complete_speculative;
	r->multi_insert = vpq_multi_insert;
	r->tuple_delete = vpq_tuple_delete;
	r->tuple_update = vpq_tuple_update;
	r->tuple_lock = vpq_tuple_lock;
	r->finish_bulk_insert = vpq_finish_bulk_insert;
	r->relation_set_new_filelocator = vpq_relation_set_new_filelocator;
	r->relation_nontransactional_truncate = vpq_relation_nontransactional_truncate;
	r->relation_copy_data = vpq_relation_copy_data;
	r->relation_copy_for_cluster = vpq_relation_copy_for_cluster;
	r->relation_vacuum = vpq_relation_vacuum;
	r->scan_analyze_next_block = vpq_scan_analyze_next_block;
	r->scan_analyze_next_tuple = vpq_scan_analyze_next_tuple;
	r->index_build_range_scan = vpq_index_build_range_scan;
	r->index_validate_scan = vpq_index_validate_scan;
	r->relation_size = vpq_relation_size;
	r->relation_needs_toast_table = vpq_relation_needs_toast_table;
	r->relation_toast_am = vpq_relation_toast_am;
	r->relation_fetch_toast_slice = NULL;
	r->relation_estimate_size = vpq_relation_estimate_size;
	r->scan_bitmap_next_tuple = vpq_scan_bitmap_next_tuple;
	r->scan_sample_next_block = vpq_scan_sample_next_block;
	r->scan_sample_next_tuple = vpq_scan_sample_next_tuple;

	slot_ops = TTSOpsVirtual;
	slot_ops.base_slot_size = sizeof(VpqSlot);
	slot_ops.copy_heap_tuple = vpq_slot_copy_heap_tuple;
	slot_ops.getsysattr = vpq_slot_getsysattr;
	slot_ops.is_current_xact_tuple = vpq_slot_is_current_xact_tuple;
	slot_ops.clear = vpq_slot_clear;
	routine_made = true;
}

const TableAmRoutine *
vpq_routine(void)
{
	if (!routine_made)
		make_routine();
	return &routine;
}

PG_FUNCTION_INFO_V1(vexec_parquet_am_handler);

Datum
vexec_parquet_am_handler(PG_FUNCTION_ARGS)
{
	if (!vpq_preloaded)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("vexec_parquet must be loaded by shared_preload_libraries"),
				 errhint("Add vexec_parquet to shared_preload_libraries and restart the server.")));
	PG_RETURN_POINTER(vpq_routine());
}

/*
 * A dropped relation's storage: its catalogs' rows in the dropping
 * transaction, its directory where it commits (options.c's OAT_DROP)
 */
void
vpq_relation_dropped(Oid relid)
{
	Relation	rel = RelationIdGetRelation(relid);

	if (!RelationIsValid(rel))
		return;
	PG_TRY();
	{
		if (vpq_is_vpq_rel(rel) && rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			VpqTable   *t = vpq_table(rel);

			if (!t->external)
			{
				vpq_buffer_discard_rel(relid);
				if (vpq_storage_exists(&t->storage))
					vpq_storage_drop_later(&t->storage, t->dir);
			}
		}
	}
	PG_FINALLY();
	{
		RelationClose(rel);
	}
	PG_END_TRY();
}

/* Whether an own table has a file its running transaction can see (ALTER ... SET (storage)) */
bool
vpq_storage_has_files(Relation rel)
{
	VpqTable   *t = vpq_table(rel);

	if (t->external)
		return false;
	vpq_buffer_flush_rel(RelationGetRelid(rel));
	return vpq_files_all(&t->storage) != NIL;
}

/*
 * An external table's columns against its files: each file's, as a scan
 * would read them -- every column found, of a type it can be read from
 */
void
vpq_external_check(Relation rel)
{
	VpqTable   *t = vpq_table(rel);
	VpqListing *l;

	if (!t->external)
		return;
	l = vpq_listing(rel, t, NULL);
	PG_TRY();
	{
		for (int i = 0; i < l->nfiles; i++)
		{
			vpq_file   *h = vpq_open_file(&l->files[i]);

			PG_TRY(2);
			{
				for (int c = 0; c < t->natts; c++)
				{
					VpqReadCol rc;

					if (t->cols[c].kind == VPQ_K_DROPPED)
						continue;
					vpq_read_col_setup(t, &t->cols[c], h, &rc, false);
				}
			}
			PG_FINALLY(2);
			{
				vpq_file_close(h);
			}
			PG_END_TRY(2);
		}
	}
	PG_FINALLY();
	{
		vpq_listing_release(l);
	}
	PG_END_TRY();
}

/*
 * The files of an own table's directory that no catalog row names, whose
 * writer is no longer running: an aborted or crashed writer's, removed
 */
void
vpq_sweep(Relation rel, VpqTable *t)
{
	vpq_entry  *entries = NULL;
	int64		n = 0;
	List	   *files;
	ListCell   *lc;
	int			rc;

	vpq_engine_ready();
	rc = vpq_list_dir(t->dir, &entries, &n);
	if (rc != VPQ_OK)
		return;
	files = vpq_files_all(&t->storage);
	PG_TRY();
	{
		for (int64 i = 0; i < n; i++)
		{
			int64		file_id;
			uint64		fxid;
			bool		named = false;
			TransactionId xid;

			if (!vpq_file_name_parse(entries[i].path, &file_id, &fxid))
				continue;
			foreach(lc, files)
				if (((VpqFileRow *) lfirst(lc))->file_id == file_id)
				{
					named = true;
					break;
				}
			if (named)
				continue;
			xid = XidFromFullTransactionId(FullTransactionIdFromU64(fxid));
			if (TransactionIdIsValid(xid) && (TransactionIdIsCurrentTransactionId(xid) ||
											  TransactionIdIsInProgress(xid)))
				continue;
			rc = vpq_delete_file(entries[i].path);
			if (rc != VPQ_OK)
				ereport(WARNING,
						(errmsg("vexec_parquet: could not delete \"%s\": %s", entries[i].path, vpq_error())));
			else
				ereport(DEBUG1,
						(errmsg("vexec_parquet: removed \"%s\", a file no transaction wrote to the end",
								entries[i].path)));
		}
	}
	PG_FINALLY();
	{
		vpq_list_free(entries, n);
	}
	PG_END_TRY();
}
