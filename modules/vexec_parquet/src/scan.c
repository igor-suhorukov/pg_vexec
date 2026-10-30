/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * scan.c
 *	  Scans of a vexec_parquet table: its units -- row groups -- claimed one
 *	  at a time, a parallel scan's among its participants by a counter in
 *	  its shared memory; each read whole, every column of it, into the
 *	  slot's rows (vexec's source reads its own columns, vexec.c); rows the
 *	  snapshot does not see -- tombstones -- left out.
 *
 *	  Fetches by TID: a row of the write buffer, else of the running scan's
 *	  unit (an UPDATE's or a DELETE's fetch of the row its scan returned),
 *	  else of the row group the last fetch read, kept, else the row group
 *	  read.  ANALYZE's blocks are the table's rows cut in as many parts as
 *	  relation_size() says it has blocks; a bitmap's and TABLESAMPLE's
 *	  blocks are TIDs' blocks, 291 row ids each.
 *
 *	  The engine's handles -- a file, its reader, a row group's columns --
 *	  are freed by the scan's end, or by its memory context's reset where an
 *	  error ends it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/tableam.h"
#include "access/tsmapi.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/tidbitmap.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "storage/predicate.h"
#include "storage/read_stream.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "pg_otel_probe_api.h"
#include "vexec_parquet.h"
#include "stats.h"

typedef struct VpqParallelScanData
{
	ParallelTableScanDescData base;
	pg_atomic_uint64 next_unit;
	Size		listing_len;
	char		listing[FLEXIBLE_ARRAY_MEMBER];
} VpqParallelScanData;

/* The engine's handles of one file and one of its row groups */
typedef struct UnitData
{
	int			file;			/* the listing's file the handles are of, -1 none */
	vpq_file   *handle;
	vpq_reader *reader;
	VpqReadCol *rcols;			/* per attribute */
	int		   *fcols;			/* the file's columns read, attribute by attribute */
	int			nfcols;
	int		   *chunk_index;	/* per attribute: its array in the chunk, -1 none */
	int			unit;			/* the listing's unit read, -1 none */
	vpq_chunk  *chunk;
	uint64		first_rowid;
	int64		rows;
	uint64	   *dead;
	int			ndead;
	int			deadpos;
} UnitData;

#define POS_FROM_END PG_INT64_MAX

typedef struct VpqScanDescData
{
	TableScanDescData base;
	VpqTable   *t;
	VpqTable   *slot_t;			/* the rows' reading where a slot's descriptor is not the relation's */
	TupleDesc	slot_desc;
	VpqListing *listing;
	MemoryContext cxt;
	MemoryContext row_cxt;
	MemoryContext unit_cxt;
	VpqParallelScanData *pscan;
	int			next_unit;		/* a serial scan's next claim (vexec's source) */
	UnitData	u;
	int64		row;			/* a parallel slot path's next row of its unit */
	int			pos_unit;		/* a serial slot path's: its unit, -1 before the first, nunits past the last */
	int64		pos_row;		/* and its row there, the last returned; POS_FROM_END to come back from the end */
	/* ANALYZE */
	int64	   *unit_start;		/* each unit's first row among all */
	int64		an_rpb;
	int64		an_next;
	int64		an_to;
	/* a bitmap's and TABLESAMPLE's block */
	BlockNumber block;
	OffsetNumber offsets[VPQ_ROWS_PER_BLOCK];
	int			noffsets;
	int			nextoff;
	BlockNumber sample_nblocks;
	BlockNumber sample_block;
	bool		sample_started;
	OffsetNumber sample_off;
	bool		ended;
	int64		units_read;		/* OT5: for pg_otel_probe's span of the scan */
} VpqScanDescData;

static List *active_scans = NIL;	/* the backend's running scans, for fetches */

static void
unit_release(UnitData *u)
{
	if (u->chunk)
		vpq_chunk_release(u->chunk);
	u->chunk = NULL;
	u->unit = -1;
	u->dead = NULL;
	u->ndead = 0;
	u->deadpos = 0;
}

static void
file_release(UnitData *u)
{
	unit_release(u);
	if (u->reader)
		vpq_reader_close(u->reader);
	if (u->handle)
		vpq_file_close(u->handle);
	u->reader = NULL;
	u->handle = NULL;
	u->file = -1;
}

static void
scan_cleanup(void *arg)
{
	VpqScanDesc s = (VpqScanDesc) arg;

	if (s->ended)
		return;
	s->ended = true;
	active_scans = list_delete_ptr(active_scans, s);
	file_release(&s->u);
	if (s->listing)
		vpq_listing_release(s->listing);
	s->listing = NULL;
}

/* The units' first rows among all of the listing's rows, for ANALYZE */
static void
unit_starts(VpqScanDesc s)
{
	VpqListing *l = s->listing;
	int64		g = 0;

	s->unit_start = MemoryContextAlloc(s->cxt, sizeof(int64) * (l->nunits + 1));
	for (int i = 0; i < l->nunits; i++)
	{
		s->unit_start[i] = g;
		g += l->files[l->units[i].file].unit_rows[l->units[i].rg];
	}
	s->unit_start[l->nunits] = g;
}

TableScanDesc
vpq_scan_begin(Relation rel, Snapshot snapshot, int nkeys, ScanKeyData *key,
			   ParallelTableScanDesc pscan, uint32 flags)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "vexec_parquet scan",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	VpqScanDesc s = palloc0(sizeof(VpqScanDescData));
	MemoryContextCallback *cb;

	s->base.rs_rd = rel;
	s->base.rs_snapshot = snapshot;
	s->base.rs_nkeys = nkeys;
	s->base.rs_key = key;
	s->base.rs_flags = flags;
	s->base.rs_parallel = pscan;
	s->cxt = cxt;
	s->row_cxt = AllocSetContextCreate(cxt, "vexec_parquet row", ALLOCSET_DEFAULT_SIZES);
	s->unit_cxt = AllocSetContextCreate(cxt, "vexec_parquet unit", ALLOCSET_DEFAULT_SIZES);
	s->u.file = -1;
	s->u.unit = -1;
	s->pos_unit = -1;
	s->pos_row = -1;
	s->t = vpq_table(rel);
	s->pscan = (VpqParallelScanData *) pscan;

	cb = palloc0(sizeof(MemoryContextCallback));
	cb->func = scan_cleanup;
	cb->arg = s;
	MemoryContextRegisterResetCallback(cxt, cb);

	if (!s->t->external)
		vpq_buffer_flush_visible(rel, snapshot);
	if (s->pscan != NULL && s->t->external && s->pscan->listing_len > 0)
		s->listing = vpq_listing_deserialize(rel, s->t, s->pscan->listing, snapshot);
	else
		s->listing = vpq_listing(rel, s->t, snapshot);

	if (snapshot && IsMVCCSnapshot(snapshot) && IsolationIsSerializable() && !s->t->external)
	{
		PredicateLockRelation(rel, snapshot);
		vpq_conflict_out(rel, &s->t->storage, snapshot);
	}
	/* a sequential scan counted as heap's initscan() counts one, a rescan too */
	if (flags & SO_TYPE_SEQSCAN)
		pgstat_count_heap_scan(rel);
	MemoryContextSwitchTo(TopMemoryContext);
	active_scans = lappend(active_scans, s);
	MemoryContextSwitchTo(old);
	return (TableScanDesc) s;
}

/* A scan of every row of every file a transaction may see, for an index build */
TableScanDesc
vpq_scan_begin_all(Relation rel)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "vexec_parquet scan",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	VpqScanDesc s = palloc0(sizeof(VpqScanDescData));
	MemoryContextCallback *cb;

	s->base.rs_rd = rel;
	s->base.rs_flags = SO_TYPE_SEQSCAN;
	s->cxt = cxt;
	s->row_cxt = AllocSetContextCreate(cxt, "vexec_parquet row", ALLOCSET_DEFAULT_SIZES);
	s->unit_cxt = AllocSetContextCreate(cxt, "vexec_parquet unit", ALLOCSET_DEFAULT_SIZES);
	s->u.file = -1;
	s->u.unit = -1;
	s->pos_unit = -1;
	s->pos_row = -1;
	s->t = vpq_table(rel);
	cb = palloc0(sizeof(MemoryContextCallback));
	cb->func = scan_cleanup;
	cb->arg = s;
	MemoryContextRegisterResetCallback(cxt, cb);
	s->listing = vpq_listing_all(s->t);
	MemoryContextSwitchTo(TopMemoryContext);
	active_scans = lappend(active_scans, s);
	MemoryContextSwitchTo(old);
	return (TableScanDesc) s;
}

void
vpq_scan_end(TableScanDesc scan)
{
	VpqScanDesc s = (VpqScanDesc) scan;

	scan_cleanup(s);
	/* a parallel scan's snapshot, or a catalog scan's, registered for the scan (heap_endscan()) */
	if (scan->rs_flags & SO_TEMP_SNAPSHOT)
		UnregisterSnapshot(scan->rs_snapshot);
	MemoryContextDelete(s->cxt);
}

void
vpq_scan_rescan(TableScanDesc scan, ScanKeyData *key, bool set_params, bool allow_strat,
				bool allow_sync, bool allow_pagemode)
{
	VpqScanDesc s = (VpqScanDesc) scan;

	unit_release(&s->u);
	s->next_unit = 0;
	s->row = 0;
	s->pos_unit = -1;
	s->pos_row = -1;
	if (key && scan->rs_nkeys > 0)
		memcpy(scan->rs_key, key, sizeof(ScanKeyData) * scan->rs_nkeys);
	if (scan->rs_flags & SO_TYPE_SEQSCAN)
		pgstat_count_heap_scan(scan->rs_rd);
}

VpqListing *
vpq_scan_listing(TableScanDesc scan)
{
	return ((VpqScanDesc) scan)->listing;
}

VpqTable *
vpq_scan_table(TableScanDesc scan)
{
	return ((VpqScanDesc) scan)->t;
}

/* The scan's next unit, claimed as a parallel scan's participants claim theirs */
bool
vpq_scan_claim_unit(TableScanDesc scan, int *unit)
{
	VpqScanDesc s = (VpqScanDesc) scan;

	if (s->pscan != NULL)
	{
		uint64		u = pg_atomic_fetch_add_u64(&s->pscan->next_unit, 1);

		if (u >= (uint64) s->listing->nunits)
			return false;
		*unit = (int) u;
		return true;
	}
	if (s->next_unit >= s->listing->nunits)
		return false;
	*unit = s->next_unit++;
	return true;
}

/* PQ12: whether the scan is a parallel scan's, its units shared among its participants */
bool
vpq_scan_is_parallel(TableScanDesc scan)
{
	return ((VpqScanDesc) scan)->pscan != NULL;
}

/*
 * PQ12: the scan's next of nslots slots -- in place of its listing's units,
 * which every participant cuts into the same slots -- claimed as its units
 * are; false past the last
 */
bool
vpq_scan_claim_slot(TableScanDesc scan, int nslots, int *slot)
{
	VpqScanDesc s = (VpqScanDesc) scan;

	if (s->pscan != NULL)
	{
		uint64		u = pg_atomic_fetch_add_u64(&s->pscan->next_unit, 1);

		if (u >= (uint64) nslots)
			return false;
		*slot = (int) u;
		return true;
	}
	if (s->next_unit >= nslots)
		return false;
	*slot = s->next_unit++;
	return true;
}

/* Row ids in [lo, hi] of a file the scan's snapshot sees deleted, ascending */
uint64 *
vpq_scan_dead_rows(TableScanDesc scan, VpqFileEntry *f, uint64 lo, uint64 hi, int *n)
{
	VpqScanDesc s = (VpqScanDesc) scan;
	Snapshot	snapshot = scan->rs_snapshot;

	*n = 0;
	if (s->t->external || !s->listing->any_dead)
		return NULL;
	if (snapshot == NULL || snapshot->snapshot_type == SNAPSHOT_ANY)
	{
		bool		registered;
		uint64	   *dead;

		if (snapshot != NULL)
			return NULL;
		snapshot = vpq_catalog_snapshot(&registered);
		dead = vpq_tombstones_visible(&s->t->storage, snapshot, lo, hi, n);
		vpq_catalog_snapshot_done(snapshot, registered);
		return dead;
	}
	return vpq_tombstones_visible(&s->t->storage, snapshot, lo, hi, n);
}

/* The handles of a file of the listing: its footer, reader and every column's read mode */
static void
open_unit_file(VpqScanDesc s, int file)
{
	UnitData   *u = &s->u;
	VpqTable   *t = s->slot_t ? s->slot_t : s->t;
	VpqFileEntry *f = &s->listing->files[file];
	MemoryContext old;
	int			n = 0;

	if (u->file == file)
		return;
	file_release(u);
	MemoryContextReset(s->unit_cxt);
	u->handle = vpq_open_file(f);
	VPQ_CHECK(vpq_reader_open(u->handle, NULL, &u->reader), psprintf("opening \"%s\"", f->path));
	u->file = file;
	old = MemoryContextSwitchTo(s->unit_cxt);
	u->rcols = palloc0(sizeof(VpqReadCol) * Max(t->natts, 1));
	u->fcols = palloc(sizeof(int) * Max(t->natts, 1));
	u->chunk_index = palloc(sizeof(int) * Max(t->natts, 1));
	for (int i = 0; i < t->natts; i++)
	{
		u->chunk_index[i] = -1;
		if (t->cols[i].kind == VPQ_K_DROPPED)
			continue;
		vpq_read_col_setup(t, &t->cols[i], u->handle, &u->rcols[i], !t->external);
		if (u->rcols[i].fcol >= 0)
		{
			u->fcols[n] = u->rcols[i].fcol;
			u->chunk_index[i] = n++;
		}
	}
	u->nfcols = n;
	MemoryContextSwitchTo(old);
}

/* A unit read whole, its rows the snapshot does not see found */
static void
load_unit(VpqScanDesc s, int unit)
{
	UnitData   *u = &s->u;
	VpqUnitRef *ref = &s->listing->units[unit];
	VpqFileEntry *f = &s->listing->files[ref->file];

	if (u->unit == unit && u->file == ref->file)
		return;
	open_unit_file(s, ref->file);
	unit_release(u);
	VPQ_CHECK(vpq_reader_read(u->reader, ref->rg, u->fcols, u->nfcols, &u->chunk),
			  psprintf("reading row group %d of \"%s\"", ref->rg, f->path));
	s->units_read++;
	u->unit = unit;
	u->rows = f->unit_rows[ref->rg];
	if (vpq_chunk_rows(u->chunk) != u->rows)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("row group %d of \"%s\" has " INT64_FORMAT " rows where its footer says " INT64_FORMAT,
						ref->rg, f->path, vpq_chunk_rows(u->chunk), u->rows)));
	u->first_rowid = f->first_rowid + (uint64) f->unit_first[ref->rg];
	{
		MemoryContext old = MemoryContextSwitchTo(s->unit_cxt);

		u->dead = vpq_scan_dead_rows((TableScanDesc) s, f, u->first_rowid,
									 u->first_rowid + (uint64) u->rows - 1, &u->ndead);
		MemoryContextSwitchTo(old);
	}
	u->deadpos = 0;
}

static bool
row_dead(UnitData *u, uint64 rowid)
{
	while (u->deadpos < u->ndead && u->dead[u->deadpos] < rowid)
		u->deadpos++;
	if (u->deadpos < u->ndead && u->dead[u->deadpos] == rowid)
		return true;
	/* a fetch's rows come in any order: the binary search for one behind */
	if (u->deadpos > 0 && u->dead[u->deadpos - 1] >= rowid)
	{
		int			lo = 0;
		int			hi = u->deadpos - 1;

		while (lo <= hi)
		{
			int			mid = (lo + hi) / 2;

			if (u->dead[mid] == rowid)
				return true;
			if (u->dead[mid] < rowid)
				lo = mid + 1;
			else
				hi = mid - 1;
		}
	}
	return false;
}

/* Row r of the loaded unit into the slot, every attribute */
static void
store_row(VpqScanDesc s, TupleTableSlot *slot, int64 r)
{
	UnitData   *u = &s->u;
	VpqTable   *t = s->slot_t ? s->slot_t : s->t;
	MemoryContext old;

	ExecClearTuple(slot);
	MemoryContextReset(s->row_cxt);
	old = MemoryContextSwitchTo(s->row_cxt);
	for (int i = 0; i < slot->tts_tupleDescriptor->natts; i++)
	{
		if (i >= t->natts || t->cols[i].kind == VPQ_K_DROPPED)
		{
			slot->tts_values[i] = (Datum) 0;
			slot->tts_isnull[i] = true;
			continue;
		}
		if (u->chunk_index[i] < 0)
			slot->tts_values[i] = vpq_value_at(&u->rcols[i], NULL, r, &slot->tts_isnull[i]);
		else
			slot->tts_values[i] = vpq_value_at(&u->rcols[i], vpq_chunk_array(u->chunk, u->chunk_index[i]), r,
											   &slot->tts_isnull[i]);
	}
	MemoryContextSwitchTo(old);
	ExecStoreVirtualTuple(slot);
	vpq_set_tid(&slot->tts_tid, u->first_rowid + (uint64) r);
	slot->tts_tableOid = RelationGetRelid(s->base.rs_rd);
	if (u->file >= 0)
		vpq_slot_set_xmin(slot, s->listing->files[u->file].xmin);
}

bool
vpq_scan_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot)
{
	VpqScanDesc s = (VpqScanDesc) scan;
	UnitData   *u = &s->u;

	/* a rewrite's scan reads by the old descriptor: its columns' types, not the relation's */
	if (slot->tts_tupleDescriptor != s->slot_desc)
	{
		MemoryContext old = MemoryContextSwitchTo(s->cxt);

		s->slot_desc = slot->tts_tupleDescriptor;
		s->slot_t = vpq_desc_matches(scan->rs_rd, s->slot_desc) ? NULL :
			vpq_table_for_desc(scan->rs_rd, s->slot_desc);
		MemoryContextSwitchTo(old);
		file_release(u);
	}

	/* a parallel scan: units claimed among its participants, forward */
	if (s->pscan != NULL)
	{
		for (;;)
		{
			int64		r;

			if (u->unit < 0 || s->row >= u->rows)
			{
				int			unit;

				CHECK_FOR_INTERRUPTS();
				unit_release(u);
				if (!vpq_scan_claim_unit(scan, &unit))
				{
					ExecClearTuple(slot);
					return false;
				}
				load_unit(s, unit);
				s->row = 0;
				continue;
			}
			r = s->row++;
			if (u->ndead > 0 && row_dead(u, u->first_rowid + (uint64) r))
				continue;
			store_row(s, slot, r);
			pgstat_count_heap_getnext(scan->rs_rd);
			return true;
		}
	}

	/* a serial scan: a place among the units, moved either way (a SCROLL cursor's, LIMIT's) */
	for (;;)
	{
		int			nunits = s->listing->nunits;

		if (ScanDirectionIsBackward(direction))
		{
			if (s->pos_unit >= nunits)
			{
				s->pos_unit = nunits - 1;
				s->pos_row = POS_FROM_END;
			}
			if (s->pos_unit < 0)
			{
				ExecClearTuple(slot);
				return false;
			}
			load_unit(s, s->pos_unit);
			if (s->pos_row == POS_FROM_END)
				s->pos_row = u->rows;
			if (--s->pos_row < 0)
			{
				s->pos_unit--;
				s->pos_row = POS_FROM_END;
				CHECK_FOR_INTERRUPTS();
				continue;
			}
		}
		else
		{
			if (s->pos_unit < 0)
			{
				s->pos_unit = 0;
				s->pos_row = -1;
			}
			if (s->pos_unit >= nunits)
			{
				ExecClearTuple(slot);
				return false;
			}
			load_unit(s, s->pos_unit);
			if (s->pos_row == POS_FROM_END)
				s->pos_row = -1;
			if (++s->pos_row >= u->rows)
			{
				s->pos_unit++;
				s->pos_row = -1;
				CHECK_FOR_INTERRUPTS();
				continue;
			}
		}
		if (u->ndead > 0 && row_dead(u, u->first_rowid + (uint64) s->pos_row))
			continue;
		store_row(s, slot, s->pos_row);
		pgstat_count_heap_getnext(scan->rs_rd);
		return true;
	}
}

/*
 * Parallel scans
 */
Size
vpq_parallelscan_estimate(Relation rel)
{
	VpqTable   *t = vpq_table(rel);
	Size		size = offsetof(VpqParallelScanData, listing);

	if (t->external)
	{
		VpqListing *l = vpq_listing(rel, t, NULL);

		size += vpq_listing_serialized_size(l);
		vpq_listing_release(l);
	}
	return MAXALIGN(size);
}

Size
vpq_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan)
{
	VpqParallelScanData *p = (VpqParallelScanData *) pscan;
	VpqTable   *t = vpq_table(rel);
	Size		size = offsetof(VpqParallelScanData, listing);

	p->base.phs_locator = rel->rd_locator;
	p->base.phs_syncscan = false;
	pg_atomic_init_u64(&p->next_unit, 0);
	p->listing_len = 0;
	if (t->external)
	{
		VpqListing *l = vpq_listing(rel, t, NULL);

		p->listing_len = vpq_listing_serialized_size(l);
		vpq_listing_serialize(l, p->listing);
		vpq_listing_release(l);
		size += p->listing_len;
	}
	return MAXALIGN(size);
}

void
vpq_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan)
{
	VpqParallelScanData *p = (VpqParallelScanData *) pscan;

	pg_atomic_write_u64(&p->next_unit, 0);
}

/*
 * Fetches by TID.  A fetch reads the columns its slot needs -- those a
 * batch source's caller named (fetch_columns(), the source contract's minor
 * 4, as ch_table_engines' VC16 reads vexec's late columns alone), else
 * every one -- each from the data page that holds its row, read alone and
 * kept for the next fetch in it (vpq_reader_read_page(), as
 * ch_table_engines' VC34 reads a row where it lies); a column whose type a
 * point read does not make, or every column with
 * vexec_parquet.enable_point_reads off, from its column chunk read whole
 * and kept.  The cache holds one file's reader, the file as its listing
 * named it -- its path, size and time -- and the pages of one row group of it
 * at a time, and goes at the transaction's end.
 */
typedef struct FetchCol
{
	vpq_chunk  *chunk;			/* the column's rows [first, first + rows) of
								 * the row group, or NULL */
	int64		first;
	int64		rows;
	bool		fresh;			/* W55: read for this fetch by fetch_pages() */
	bool		whole;			/* W55: its point read found none: read whole */
} FetchCol;

typedef struct FetchCache
{
	Oid			relid;
	RelFileNumber relnumber;
	char	   *path;
	int64		size;
	int64		mtime;
	int			rg;
	VpqTable   *t;
	vpq_file   *handle;
	vpq_reader *reader;
	VpqReadCol *rcols;
	FetchCol   *cols;			/* per attribute */
	int			natts;
	uint64		first_rowid;
	int64		rows;
	MemoryContext cxt;
} FetchCache;

static FetchCache fc;

VpqFetchStats vpq_fetch_stats;

/* The columns a batch source's caller named for a slot's fetches (fetch_columns()) */
typedef struct FetchSlot
{
	TupleTableSlot *slot;
	Oid			relid;
	Bitmapset  *attrs;			/* in the slot's memory context; NULL none */
	MemoryContextCallback forget;
	struct FetchSlot *next;
} FetchSlot;

static FetchSlot *fetch_slots;

/* A slot's registration gone with the memory context it was made in */
static void
fetch_slot_forget(void *arg)
{
	for (FetchSlot **p = &fetch_slots; *p != NULL; p = &(*p)->next)
		if (*p == (FetchSlot *) arg)
		{
			*p = (*p)->next;
			break;
		}
}

void
vpq_fetch_set_columns(Relation rel, TupleTableSlot *slot, int n, const AttrNumber *attnums)
{
	FetchSlot  *fs;
	MemoryContext old;

	for (fs = fetch_slots; fs != NULL; fs = fs->next)
		if (fs->slot == slot)
			break;
	old = MemoryContextSwitchTo(slot->tts_mcxt);
	if (fs == NULL)
	{
		fs = palloc0(sizeof(FetchSlot));
		fs->slot = slot;
		fs->forget.func = fetch_slot_forget;
		fs->forget.arg = fs;
		MemoryContextRegisterResetCallback(slot->tts_mcxt, &fs->forget);
		fs->next = fetch_slots;
		fetch_slots = fs;
	}
	fs->relid = RelationGetRelid(rel);
	bms_free(fs->attrs);
	fs->attrs = NULL;
	for (int i = 0; i < n; i++)
		if (attnums[i] > 0)
			fs->attrs = bms_add_member(fs->attrs, attnums[i]);
	MemoryContextSwitchTo(old);
}

/* The columns a fetch into the slot reads: false where every one */
static bool
fetch_slot_columns(Relation rel, TupleTableSlot *slot, const Bitmapset **attrs)
{
	if (!vpq_enable_fetch_columns)
		return false;
	for (FetchSlot *fs = fetch_slots; fs != NULL; fs = fs->next)
		if (fs->slot == slot && fs->relid == RelationGetRelid(rel))
		{
			*attrs = fs->attrs;
			return true;
		}
	return false;
}

void
vpq_fetch_reset(void)
{
	if (fc.cols != NULL)
		for (int i = 0; i < fc.natts; i++)
			if (fc.cols[i].chunk)
				vpq_chunk_release(fc.cols[i].chunk);
	if (fc.reader)
		vpq_reader_close(fc.reader);
	if (fc.handle)
		vpq_file_close(fc.handle);
	if (fc.cxt)
		MemoryContextDelete(fc.cxt);
	memset(&fc, 0, sizeof(fc));
}

/*
 * A row group of a file the fetch cache's, its columns set up and none read.
 * W55 (pg_vector_executor.md §3.36): another row group of the file keeps its
 * reader -- the file's footer parsed once -- its columns' pages let go: the
 * reader made again parsed ClickBench's footer of 226 row groups, about 0.1 s,
 * for each of Q23's rows in a row group of its own
 */
static void
fetch_open(Relation rel, VpqTable *t, VpqFileEntry *f, int rg)
{
	MemoryContext old;

	if (fc.reader && fc.relid == RelationGetRelid(rel) && fc.relnumber == rel->rd_locator.relNumber &&
		fc.t == t && fc.size == f->size && fc.mtime == f->mtime && strcmp(fc.path, f->path) == 0)
	{
		if (fc.rg == rg)
			return;
		for (int i = 0; i < fc.natts; i++)
		{
			if (fc.cols[i].chunk)
				vpq_chunk_release(fc.cols[i].chunk);
			memset(&fc.cols[i], 0, sizeof(FetchCol));
		}
		fc.rg = rg;
		fc.first_rowid = f->first_rowid + (uint64) f->unit_first[rg];
		fc.rows = f->unit_rows[rg];
		return;
	}
	vpq_fetch_reset();
	fc.cxt = AllocSetContextCreate(TopMemoryContext, "vexec_parquet fetch", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(fc.cxt);
	fc.relid = RelationGetRelid(rel);
	fc.relnumber = rel->rd_locator.relNumber;
	fc.path = pstrdup(f->path);
	fc.size = f->size;
	fc.mtime = f->mtime;
	fc.rg = rg;
	fc.t = t;
	fc.handle = vpq_open_file(f);
	VPQ_CHECK(vpq_reader_open(fc.handle, NULL, &fc.reader), psprintf("opening \"%s\"", f->path));
	fc.rcols = palloc0(sizeof(VpqReadCol) * Max(t->natts, 1));
	fc.cols = palloc0(sizeof(FetchCol) * Max(t->natts, 1));
	fc.natts = t->natts;
	for (int i = 0; i < t->natts; i++)
	{
		fc.rcols[i].fcol = -1;
		if (t->cols[i].kind != VPQ_K_DROPPED)
			vpq_read_col_setup(t, &t->cols[i], fc.handle, &fc.rcols[i], !t->external);
	}
	fc.first_rowid = f->first_rowid + (uint64) f->unit_first[rg];
	fc.rows = f->unit_rows[rg];
	MemoryContextSwitchTo(old);
}

/* Attribute i's column holding row r of the cache's row group: its page read where no kept one holds it */
static const vpq_array *
fetch_column(int i, int64 r, int64 *at)
{
	FetchCol   *c = &fc.cols[i];
	int			fcol = fc.rcols[i].fcol;

	*at = r;
	if (fcol < 0)
		return NULL;
	if (c->chunk != NULL && r >= c->first && r < c->first + c->rows)
	{
		if (!c->fresh)
			vpq_fetch_stats.kept++;
		c->fresh = false;
		*at = r - c->first;
		return vpq_chunk_array(c->chunk, 0);
	}
	if (c->chunk != NULL)
		vpq_chunk_release(c->chunk);
	c->chunk = NULL;
	if (vpq_enable_point_reads && !c->whole)
	{
		int64_t		first;
		int64_t		rows;
		bool		indexed;
		int			rc = vpq_reader_read_page(fc.reader, fc.rg, fcol, r, &c->chunk, &first, &rows, &indexed);

		if (rc == VPQ_OK)
		{
			c->first = first;
			c->rows = rows;
			vpq_fetch_stats.pages++;
			if (indexed)
				vpq_fetch_stats.indexed++;
			*at = r - c->first;
			return vpq_chunk_array(c->chunk, 0);
		}
		if (rc != VPQ_NOT_FOUND)
			VPQ_CHECK(rc, psprintf("reading row %lld of row group %d of \"%s\"", (long long) r, fc.rg, fc.path));
	}
	VPQ_CHECK(vpq_reader_read(fc.reader, fc.rg, &fcol, 1, &c->chunk),
			  psprintf("reading row group %d of \"%s\"", fc.rg, fc.path));
	c->first = 0;
	c->rows = fc.rows;
	vpq_fetch_stats.whole++;
	return vpq_chunk_array(c->chunk, 0);
}

/*
 * W55 (pg_vector_executor.md §3.36): the data pages a fetch of row r needs --
 * of the columns its slot reads, none kept holding the row -- read at once on
 * vexec_parquet.fetch_threads threads (vpq_reader_read_pages()),
 * fetch_column() taking them;
 * a column whose point read found none read whole by it, as before.
 * ClickBench's Q23 over its own file of 100M rows fetched its ten rows' 105
 * columns a page at a time, 1.1 s on one CPU
 */
static void
fetch_pages(int64 r, int natts, bool some, const Bitmapset *attrs)
{
	int		   *at = palloc(sizeof(int) * Max(natts, 1));
	int		   *cols = palloc(sizeof(int) * Max(natts, 1));
	vpq_chunk **chunks;
	int64_t    *firsts;
	int64_t    *rows;
	bool	   *indexed;
	int		   *rcs;
	int			n = 0;
	int			rc;

	for (int i = 0; i < natts && i < fc.natts; i++)
	{
		FetchCol   *c = &fc.cols[i];

		if (fc.t->cols[i].kind == VPQ_K_DROPPED || (some && !bms_is_member(i + 1, attrs)) || fc.rcols[i].fcol < 0 ||
			c->whole || (c->chunk != NULL && r >= c->first && r < c->first + c->rows))
			continue;
		at[n] = i;
		cols[n++] = fc.rcols[i].fcol;
	}
	/* one page alone: fetch_column()'s own read */
	if (n < 2)
	{
		pfree(at);
		pfree(cols);
		return;
	}
	chunks = palloc0(sizeof(vpq_chunk *) * n);
	firsts = palloc0(sizeof(int64_t) * n);
	rows = palloc0(sizeof(int64_t) * n);
	indexed = palloc0(sizeof(bool) * n);
	rcs = palloc0(sizeof(int) * n);
	rc = vpq_reader_read_pages(fc.reader, fc.rg, n, cols, r, vpq_fetch_threads, chunks, firsts, rows, indexed, rcs);
	if (rc != VPQ_OK)
	{
		for (int k = 0; k < n; k++)
			if (chunks[k] != NULL)
				vpq_chunk_release(chunks[k]);
		vpq_engine_error(rc, psprintf("reading row %lld of row group %d of \"%s\"", (long long) r, fc.rg, fc.path));
	}
	for (int k = 0; k < n; k++)
	{
		FetchCol   *c = &fc.cols[at[k]];

		if (rcs[k] != VPQ_OK || chunks[k] == NULL)
		{
			c->whole = true;
			continue;
		}
		if (c->chunk != NULL)
			vpq_chunk_release(c->chunk);
		c->chunk = chunks[k];
		c->first = firsts[k];
		c->rows = rows[k];
		c->fresh = true;
		vpq_fetch_stats.pages++;
		if (indexed[k])
			vpq_fetch_stats.indexed++;
	}
	pfree(at);
	pfree(cols);
	pfree(chunks);
	pfree(firsts);
	pfree(rows);
	pfree(indexed);
	pfree(rcs);
}

/* Row r of the cache's row group into the slot: the columns `attrs` names alone where `some` */
static void
store_fetched(Relation rel, int64 r, TupleTableSlot *slot, uint64 rowid, bool some, const Bitmapset *attrs)
{
	TupleDesc	desc = slot->tts_tupleDescriptor;
	Datum	   *values = palloc0(sizeof(Datum) * Max(desc->natts, 1));
	bool	   *nulls = palloc(sizeof(bool) * Max(desc->natts, 1));
	HeapTuple	tuple;

	if (vpq_enable_point_reads && vpq_fetch_threads > 1)
		fetch_pages(r, desc->natts, some, attrs);
	for (int i = 0; i < desc->natts; i++)
	{
		const vpq_array *a;
		int64		at;

		nulls[i] = true;
		if (i >= fc.t->natts || fc.t->cols[i].kind == VPQ_K_DROPPED || (some && !bms_is_member(i + 1, attrs)))
			continue;
		a = fetch_column(i, r, &at);
		values[i] = vpq_value_at(&fc.rcols[i], a, at, &nulls[i]);
		vpq_fetch_stats.values++;
	}
	vpq_fetch_stats.fetches++;
	tuple = heap_form_tuple(desc, values, nulls);
	ExecForceStoreHeapTuple(tuple, slot, true);
	vpq_set_tid(&slot->tts_tid, rowid);
	slot->tts_tableOid = RelationGetRelid(rel);
	pfree(values);
	pfree(nulls);
}

static void
store_values(Relation rel, VpqTable *t, VpqReadCol *rcols, int *chunk_index, vpq_chunk *chunk, int64 r,
			 TupleTableSlot *slot, uint64 rowid)
{
	TupleDesc	desc = slot->tts_tupleDescriptor;
	Datum	   *values = palloc0(sizeof(Datum) * Max(desc->natts, 1));
	bool	   *nulls = palloc(sizeof(bool) * Max(desc->natts, 1));
	HeapTuple	tuple;

	for (int i = 0; i < desc->natts; i++)
	{
		nulls[i] = true;
		if (i >= t->natts || t->cols[i].kind == VPQ_K_DROPPED)
			continue;
		values[i] = vpq_value_at(&rcols[i], chunk_index[i] < 0 ? NULL : vpq_chunk_array(chunk, chunk_index[i]),
								 r, &nulls[i]);
	}
	tuple = heap_form_tuple(desc, values, nulls);
	ExecForceStoreHeapTuple(tuple, slot, true);
	vpq_set_tid(&slot->tts_tid, rowid);
	slot->tts_tableOid = RelationGetRelid(rel);
	pfree(values);
	pfree(nulls);
}

/* A running scan of the relation whose unit holds the row: for SnapshotAny's fetch alone */
static bool
fetch_from_scan(Relation rel, uint64 rowid, TupleTableSlot *slot)
{
	ListCell   *lc;

	foreach(lc, active_scans)
	{
		VpqScanDesc s = lfirst(lc);
		UnitData   *u = &s->u;

		if (s->base.rs_rd != rel || u->unit < 0 || u->chunk == NULL || s->slot_t != NULL ||
			rowid < u->first_rowid || rowid >= u->first_rowid + (uint64) u->rows)
			continue;
		store_values(rel, s->t, u->rcols, u->chunk_index, u->chunk, (int64) (rowid - u->first_rowid), slot,
					 rowid);
		vpq_slot_set_xmin(slot, s->listing->files[u->file].xmin);
		return true;
	}
	return false;
}

/* The row group of an own file holding a row, and its first row's place */
static int
own_row_group(VpqFileEntry *f, uint64 rowid)
{
	int64		pos = (int64) (rowid - f->first_rowid);

	for (int g = 0; g < f->nunits; g++)
		if (pos >= f->unit_first[g] && pos < f->unit_first[g] + f->unit_rows[g])
			return g;
	return -1;
}

bool
vpq_fetch_row(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot)
{
	VpqTable   *t = vpq_table(rel);
	uint64		rowid;
	VpqFileEntry entry;
	VpqFileEntry *f;
	VpqListing *l = NULL;
	int			rg;
	bool		found = false;

	/* a dirty snapshot's writers in progress are each fetch's own (HeapTupleSatisfiesDirty()) */
	if (snapshot && snapshot->snapshot_type == SNAPSHOT_DIRTY)
	{
		snapshot->xmin = snapshot->xmax = InvalidTransactionId;
		snapshot->speculativeToken = 0;
	}
	if (!vpq_tid_rowid(tid, &rowid))
		return false;
	/* a slot of another descriptor than the relation's: its columns' types read */
	if (!vpq_desc_matches(rel, slot->tts_tupleDescriptor))
		t = vpq_table_for_desc(rel, slot->tts_tupleDescriptor);
	else if (snapshot && snapshot->snapshot_type == SNAPSHOT_ANY && fetch_from_scan(rel, rowid, slot))
		return true;
	if (!t->external)
	{
		VpqFileRow row;

		if (vpq_buffer_fetch(rel, rowid, snapshot, slot))
			return true;
		if (!vpq_file_of_row(&t->storage, rowid, snapshot, &row))
			return false;
		if (!vpq_row_survives(&t->storage, rowid, snapshot))
			return false;
		if (snapshot && IsMVCCSnapshot(snapshot) && IsolationIsSerializable())
			PredicateLockRelation(rel, snapshot);
		memset(&entry, 0, sizeof(entry));
		entry.path = vpq_file_path(t->dir, row.file_id, (uint64) row.fxid);
		entry.size = row.bytes;
		entry.mtime = -1;
		entry.file_id = row.file_id;
		entry.first_rowid = row.first_rowid;
		entry.rows = row.rows;
		entry.xmin = row.xmin;
		if (row.stats == NULL)
			return false;
		entry.nunits = vpq_stats_row_groups(row.stats, &entry.unit_rows);
		entry.unit_first = palloc(sizeof(int64) * Max(entry.nunits, 1));
		for (int g = 0, first = 0; g < entry.nunits; g++)
		{
			entry.unit_first[g] = first;
			first += entry.unit_rows[g];
		}
		f = &entry;
	}
	else
	{
		int			fi;

		l = vpq_listing(rel, t, NULL);
		fi = vpq_file_entry_of_row(l, rowid);
		if (fi < 0)
		{
			vpq_listing_release(l);
			return false;
		}
		f = &l->files[fi];
	}
	rg = own_row_group(f, rowid);
	if (rg >= 0)
	{
		const Bitmapset *attrs = NULL;
		bool		some = fetch_slot_columns(rel, slot, &attrs);

		fetch_open(rel, t, f, rg);
		store_fetched(rel, (int64) (rowid - fc.first_rowid), slot, rowid, some, attrs);
		vpq_slot_set_xmin(slot, f->xmin);
		found = true;
		/* a table of another descriptor's lives in the caller's memory: not kept */
		if (t != vpq_table(rel))
			vpq_fetch_reset();
	}
	if (l)
		vpq_listing_release(l);
	return found;
}

/*
 * ANALYZE: block b is the rows [b * rpb, (b + 1) * rpb) of all of the
 * listing's, rpb the rows over relation_size()'s blocks
 */
bool
vpq_scan_analyze_next_block(TableScanDesc scan, ReadStream *stream)
{
	VpqScanDesc s = (VpqScanDesc) scan;
	BufferAccessStrategy strategy;
	BlockNumber block = read_stream_next_block(stream, &strategy);
	int64		nblocks;

	if (!BlockNumberIsValid(block))
		return false;
	if (s->unit_start == NULL)
		unit_starts(s);
	nblocks = Max((s->listing->bytes + BLCKSZ - 1) / BLCKSZ, 1);
	s->an_rpb = Max((s->listing->rows + nblocks - 1) / nblocks, 1);
	s->an_next = (int64) block * s->an_rpb;
	s->an_to = Min(s->an_next + s->an_rpb, s->listing->rows);
	return true;
}

static int
unit_of_row(VpqScanDesc s, int64 g)
{
	int			lo = 0;
	int			hi = s->listing->nunits - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (g < s->unit_start[mid])
			hi = mid - 1;
		else if (g >= s->unit_start[mid + 1])
			lo = mid + 1;
		else
			return mid;
	}
	return -1;
}

bool
vpq_scan_analyze_next_tuple(TableScanDesc scan, double *liverows, double *deadrows, TupleTableSlot *slot)
{
	VpqScanDesc s = (VpqScanDesc) scan;

	while (s->an_next < s->an_to)
	{
		int64		g = s->an_next++;
		int			unit = unit_of_row(s, g);
		int64		r;

		if (unit < 0)
			break;
		load_unit(s, unit);
		r = g - s->unit_start[unit];
		if (s->u.ndead > 0 && row_dead(&s->u, s->u.first_rowid + (uint64) r))
		{
			*deadrows += 1;
			continue;
		}
		store_row(s, slot, r);
		*liverows += 1;
		return true;
	}
	ExecClearTuple(slot);
	return false;
}

/*
 * Bitmap scans: each block's rows fetched by TID, through the fetch cache's
 * row group
 */
bool
vpq_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *recheck,
						   uint64 *lossy_pages, uint64 *exact_pages)
{
	VpqScanDesc s = (VpqScanDesc) scan;

	for (;;)
	{
		ItemPointerData tid;

		while (s->nextoff >= s->noffsets)
		{
			TBMIterateResult tbmres;

			CHECK_FOR_INTERRUPTS();
			if (!tbm_iterate(&scan->st.rs_tbmiterator, &tbmres))
				return false;
			s->block = tbmres.blockno;
			s->nextoff = 0;
			if (tbmres.lossy)
			{
				s->noffsets = VPQ_ROWS_PER_BLOCK;
				for (int i = 0; i < VPQ_ROWS_PER_BLOCK; i++)
					s->offsets[i] = (OffsetNumber) (i + 1);
				*recheck = true;
				(*lossy_pages)++;
			}
			else
			{
				OffsetNumber offsets[TBM_MAX_TUPLES_PER_PAGE];
				int			n = tbm_extract_page_tuple(&tbmres, offsets, TBM_MAX_TUPLES_PER_PAGE);

				s->noffsets = 0;
				for (int i = 0; i < n; i++)
					if (offsets[i] >= 1 && offsets[i] <= VPQ_ROWS_PER_BLOCK)
						s->offsets[s->noffsets++] = offsets[i];
				*recheck = tbmres.recheck;
				(*exact_pages)++;
			}
		}
		ItemPointerSet(&tid, s->block, s->offsets[s->nextoff++]);
		if (vpq_fetch_row(scan->rs_rd, &tid, scan->rs_snapshot, slot))
		{
			pgstat_count_heap_fetch(scan->rs_rd);
			return true;
		}
	}
}

/*
 * TABLESAMPLE: the TIDs' blocks up to the greatest row id's, each block's
 * rows as the method picks them, fetched by TID
 */
bool
vpq_scan_sample_next_block(TableScanDesc scan, SampleScanState *scanstate)
{
	VpqScanDesc s = (VpqScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;

	if (!s->sample_started)
	{
		uint64		max_rowid = 0;

		for (int i = 0; i < s->listing->nfiles; i++)
			max_rowid = Max(max_rowid, s->listing->files[i].first_rowid + (uint64) s->listing->files[i].rows);
		s->sample_nblocks = (BlockNumber) ((max_rowid + VPQ_ROWS_PER_BLOCK - 1) / VPQ_ROWS_PER_BLOCK);
	}
	if (tsm->NextSampleBlock)
	{
		BlockNumber b = tsm->NextSampleBlock(scanstate, s->sample_nblocks);

		s->sample_started = true;
		if (!BlockNumberIsValid(b))
			return false;
		s->sample_block = b;
	}
	else
	{
		if (!s->sample_started)
			s->sample_block = 0;
		else
			s->sample_block++;
		s->sample_started = true;
		if (s->sample_block >= s->sample_nblocks)
			return false;
	}
	return true;
}

bool
vpq_scan_sample_next_tuple(TableScanDesc scan, SampleScanState *scanstate, TupleTableSlot *slot)
{
	VpqScanDesc s = (VpqScanDesc) scan;
	TsmRoutine *tsm = scanstate->tsmroutine;

	for (;;)
	{
		OffsetNumber off = tsm->NextSampleTuple(scanstate, s->sample_block, VPQ_ROWS_PER_BLOCK);
		ItemPointerData tid;

		if (!OffsetNumberIsValid(off))
		{
			ExecClearTuple(slot);
			return false;
		}
		ItemPointerSet(&tid, s->sample_block, off);
		if (vpq_fetch_row(scan->rs_rd, &tid, scan->rs_snapshot, slot))
			return true;
	}
}

/*
 * pg_otel_probe's span of a scan of a table of Parquet files that vexec did
 * not take -- a Seq Scan, a Bitmap Heap Scan, a TABLESAMPLE's
 * (pg_vector_executor.md §3.41, OT5): the units it read, and the table's,
 * where a span's buffers and I/O times say nothing of the files' reads.
 * The scan's fields alone, so that it is safe on a failed statement's error
 * path; an index scan, which keeps no scan of its own, carries none.
 */
static void
scan_report(PlanState *ps, PgOtelProbeAttrs *attrs, int when)
{
	VpqScanDesc s = (VpqScanDesc) ((ScanState *) ps)->ss_currentScanDesc;

	if (s == NULL)
		return;
	attrs->add_int(attrs, "vexec_parquet.units_read", s->units_read);
	if (s->listing != NULL)
		attrs->add_int(attrs, "vexec_parquet.units", s->listing->nunits);
}

/* From _PG_init, while the postmaster preloads the library */
void
vpq_register_otel(void)
{
	pg_otel_probe_register_scan_reporter(vpq_routine(), scan_report, PG_OTEL_PROBE_ABORT_SAFE);
}
