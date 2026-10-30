/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * listing.c
 *	  A scan's files and its units, their row groups.
 *
 *	  An own table's files are the rows of vexec_parquet.files its scan's
 *	  snapshot sees, each with its row groups' rows and statistics as the
 *	  catalog keeps them, and the facts a device unit asks of its footer: no
 *	  footer is read to plan, prune or hand a unit to the GPU (pg_vector_
 *	  executor.md §3.34, PQ1).  A listing is kept for its snapshot, so that
 *	  the planner's estimates and the scan's start read the catalog once
 *	  (PQ3), and a column's statistics are decoded as they are first asked
 *	  for.  An external table's are its location's, listed as a statement
 *	  first reads the table and kept for the rest of the statement, each
 *	  footer read once in the process (the engine's cache) and its facts
 *	  kept in shared memory, so that another process reads none of it again
 *	  (footers.c, PQ4); a parallel scan's workers get the leader's listing
 *	  through the scan's shared memory, so that every participant reads the
 *	  same files.  An external file's row ids are its first row's place in
 *	  the listing and a row's in the file: stable while the listing is.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#include "vexec_parquet.h"
#include "stats.h"

typedef struct ExternalCache
{
	Oid			relid;
	LocalTransactionId lxid;
	TimestampTz stmt;
	char	   *location;
	VpqListing *listing;
} ExternalCache;

static ExternalCache cache;		/* one: a statement's last external listing */

/* The process's own listings read and columns' statistics decoded (vexec_parquet.read_stats()) */
VpqListingStats vpq_listing_stats;

/*
 * An own table's listings kept for their snapshots (PQ3).  A snapshot sees
 * what another saw where both are MVCC snapshots of the transaction, its
 * subtransaction and command, of the same bounds and the same transactions
 * in progress -- GetSnapshotData() makes them alike while no transaction
 * ends, and the active snapshots are copies, whose snapXactCompletionCount
 * is none -- and this process wrote no catalog row since
 * (vpq_catalog_writes).  Kept in the transaction's memory, forgotten at
 * its end.
 */
typedef struct OwnCache
{
	Oid			relid;
	VpqStorage	storage;
	LocalTransactionId lxid;
	SubTransactionId subid;
	CommandId	curcid;
	TransactionId xmin;
	TransactionId xmax;
	uint32		xcnt;
	int32		subxcnt;
	bool		suboverflowed;
	TransactionId *xip;
	TransactionId *subxip;
	uint64		writes;
	VpqListing *listing;
} OwnCache;

#define OWN_CACHED 4
static OwnCache own_cache[OWN_CACHED];
static int	own_next = 0;

/* Whether a cached listing's snapshot and the key's see the same rows of the catalogs */
static bool
own_key_equal(const OwnCache *c, const OwnCache *k)
{
	return c->listing != NULL && c->relid == k->relid && c->lxid == k->lxid && c->subid == k->subid &&
		c->curcid == k->curcid && c->xmin == k->xmin && c->xmax == k->xmax && c->xcnt == k->xcnt &&
		c->subxcnt == k->subxcnt && c->suboverflowed == k->suboverflowed && c->writes == k->writes &&
		c->storage.spc == k->storage.spc && c->storage.relnumber == k->storage.relnumber &&
		c->storage.backend == k->storage.backend &&
		(k->xcnt == 0 || memcmp(c->xip, k->xip, sizeof(TransactionId) * k->xcnt) == 0) &&
		(k->subxcnt <= 0 || memcmp(c->subxip, k->subxip, sizeof(TransactionId) * k->subxcnt) == 0);
}

static VpqListing *
new_listing(void)
{
	MemoryContext cxt = AllocSetContextCreate(TopTransactionContext, "vexec_parquet listing",
											  ALLOCSET_DEFAULT_SIZES);
	VpqListing *l = MemoryContextAllocZero(cxt, sizeof(VpqListing));

	l->cxt = cxt;
	l->refs = 1;
	return l;
}

void
vpq_listing_release(VpqListing *l)
{
	if (l == NULL)
		return;
	if (--l->refs == 0)
	{
		if (cache.listing == l)
			cache.listing = NULL;
		for (int i = 0; i < OWN_CACHED; i++)
			if (own_cache[i].listing == l)
				own_cache[i].listing = NULL;
		MemoryContextDelete(l->cxt);
	}
}

/* The units of the listing's files, every row group of each in turn */
static void
make_units(VpqListing *l)
{
	int			n = 0;

	for (int i = 0; i < l->nfiles; i++)
		n += l->files[i].nunits;
	l->nunits = n;
	l->units = MemoryContextAlloc(l->cxt, sizeof(VpqUnitRef) * Max(n, 1));
	n = 0;
	for (int i = 0; i < l->nfiles; i++)
		for (int g = 0; g < l->files[i].nunits; g++)
		{
			l->units[n].file = i;
			l->units[n].rg = g;
			n++;
		}
}

static void
row_groups_of_footer(VpqFileEntry *f, vpq_file *h)
{
	int64		first = 0;

	f->nunits = vpq_file_row_groups(h);
	f->unit_rows = palloc(sizeof(int64) * Max(f->nunits, 1));
	f->unit_first = palloc(sizeof(int64) * Max(f->nunits, 1));
	for (int g = 0; g < f->nunits; g++)
	{
		f->unit_rows[g] = vpq_file_row_group_rows(h, g);
		f->unit_first[g] = first;
		first += f->unit_rows[g];
	}
	f->rows = first;
}

static VpqListing *
list_external(Relation rel, VpqTable *t)
{
	VpqListing *l;
	vpq_entry  *entries = NULL;
	int64		n = 0;
	int			rc;
	uint64		next_rowid = 0;
	MemoryContext old;

	vpq_engine_ready();
	rc = vpq_list(t->opts.location, &entries, &n);
	if (rc == VPQ_NOT_FOUND)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FILE),
				 errmsg("no file or directory at the location \"%s\" of vexec_parquet table \"%s\"",
						t->opts.location, RelationGetRelationName(rel))));
	if (rc != VPQ_OK)
		vpq_engine_error(rc, psprintf("listing \"%s\"", t->opts.location));

	l = new_listing();
	old = MemoryContextSwitchTo(l->cxt);
	PG_TRY();
	{
		l->nfiles = (int) n;
		l->files = palloc0(sizeof(VpqFileEntry) * Max(n, 1));
		for (int64 i = 0; i < n; i++)
		{
			VpqFileEntry *f = &l->files[i];
			vpq_file   *h;

			f->path = pstrdup(entries[i].path);
			f->size = entries[i].size;
			f->mtime = entries[i].mtime_ns;
			f->file_id = i;
			h = vpq_open_file(f);
			row_groups_of_footer(f, h);
			vpq_file_close(h);
			f->first_rowid = next_rowid;
			next_rowid += (uint64) f->rows;
			l->rows += f->rows;
			l->bytes += f->size;
		}
	}
	PG_FINALLY();
	{
		vpq_list_free(entries, n);
	}
	PG_END_TRY();
	make_units(l);
	MemoryContextSwitchTo(old);
	return l;
}

static VpqListing *
list_own(VpqTable *t, Snapshot snapshot)
{
	VpqListing *l = new_listing();
	MemoryContext old = MemoryContextSwitchTo(l->cxt);
	List	   *files = snapshot ? vpq_files_visible(&t->storage, snapshot) : vpq_files_all(&t->storage);
	ListCell   *lc;
	int			i = 0;

	vpq_listing_stats.listings++;

	l->nfiles = list_length(files);
	l->files = palloc0(sizeof(VpqFileEntry) * Max(l->nfiles, 1));
	foreach(lc, files)
	{
		VpqFileRow *row = lfirst(lc);
		VpqFileEntry *f = &l->files[i++];

		f->path = vpq_file_path(t->dir, row->file_id, (uint64) row->fxid);
		f->size = row->bytes;
		f->mtime = -1;
		f->file_id = row->file_id;
		f->first_rowid = row->first_rowid;
		f->rows = row->rows;
		f->stats_raw = row->stats;
		f->xmin = row->xmin;
		if (row->stats != NULL)
		{
			int64		first = 0;

			f->facts = vpq_stats_facts(row->stats, &f->facts_len);

			f->nunits = vpq_stats_row_groups(row->stats, &f->unit_rows);
			f->unit_first = palloc(sizeof(int64) * Max(f->nunits, 1));
			for (int g = 0; g < f->nunits; g++)
			{
				f->unit_first[g] = first;
				first += f->unit_rows[g];
			}
		}
		l->rows += f->rows;
		l->bytes += f->size;
	}
	l->any_dead = snapshot ? vpq_tombstones_visible_any(&t->storage, snapshot) : false;
	make_units(l);
	MemoryContextSwitchTo(old);
	return l;
}

/*
 * The scan's listing: an own table's by the snapshot, an external table's
 * the statement's
 */
VpqListing *
vpq_listing(Relation rel, VpqTable *t, Snapshot snapshot)
{
	if (!t->external)
	{
		VpqListing *l;
		bool		registered = false;
		bool		keyed;
		OwnCache	key;

		/* SnapshotAny: every file a transaction may see, every row of each -- an index build's */
		if (snapshot != NULL && snapshot->snapshot_type == SNAPSHOT_ANY)
			return list_own(t, NULL);
		if (snapshot == NULL)
			snapshot = vpq_catalog_snapshot(&registered);
		keyed = snapshot->snapshot_type == SNAPSHOT_MVCC && !snapshot->takenDuringRecovery;
		if (keyed)
		{
			memset(&key, 0, sizeof(key));
			key.relid = RelationGetRelid(rel);
			key.storage = t->storage;
			key.lxid = MyProc->vxid.lxid;
			key.subid = GetCurrentSubTransactionId();
			key.curcid = snapshot->curcid;
			key.xmin = snapshot->xmin;
			key.xmax = snapshot->xmax;
			key.xcnt = snapshot->xcnt;
			key.subxcnt = snapshot->subxcnt;
			key.suboverflowed = snapshot->suboverflowed;
			key.xip = snapshot->xip;
			key.subxip = snapshot->subxip;
			key.writes = vpq_catalog_writes;
			for (int i = 0; i < OWN_CACHED; i++)
			{
				OwnCache   *c = &own_cache[i];

				if (own_key_equal(c, &key))
				{
					vpq_catalog_snapshot_done(snapshot, registered);
					c->listing->refs++;
					return c->listing;
				}
			}
		}
		l = list_own(t, snapshot);
		if (keyed)
		{
			OwnCache   *c = &own_cache[own_next];
			MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

			own_next = (own_next + 1) % OWN_CACHED;
			if (c->listing != NULL)
			{
				VpqListing *gone = c->listing;

				c->listing = NULL;
				vpq_listing_release(gone);
			}
			if (c->xip)
				pfree(c->xip);
			if (c->subxip)
				pfree(c->subxip);
			*c = key;
			c->xip = key.xcnt > 0 ? palloc(sizeof(TransactionId) * key.xcnt) : NULL;
			if (key.xcnt > 0)
				memcpy(c->xip, key.xip, sizeof(TransactionId) * key.xcnt);
			c->subxip = key.subxcnt > 0 ? palloc(sizeof(TransactionId) * key.subxcnt) : NULL;
			if (key.subxcnt > 0)
				memcpy(c->subxip, key.subxip, sizeof(TransactionId) * key.subxcnt);
			c->listing = l;
			l->refs++;
			MemoryContextSwitchTo(old);
		}
		vpq_catalog_snapshot_done(snapshot, registered);
		return l;
	}
	if (cache.listing != NULL && cache.relid == RelationGetRelid(rel) &&
		cache.lxid == MyProc->vxid.lxid && cache.stmt == GetCurrentStatementStartTimestamp() &&
		strcmp(cache.location, t->opts.location) == 0)
	{
		cache.listing->refs++;
		return cache.listing;
	}
	if (cache.listing != NULL)
	{
		VpqListing *old = cache.listing;

		cache.listing = NULL;
		vpq_listing_release(old);
	}
	cache.listing = list_external(rel, t);
	cache.relid = RelationGetRelid(rel);
	cache.lxid = MyProc->vxid.lxid;
	cache.stmt = GetCurrentStatementStartTimestamp();
	cache.location = MemoryContextStrdup(cache.listing->cxt, t->opts.location);
	cache.listing->refs++;
	return cache.listing;
}

/* Every file of an own table a transaction may see, its rows whatever their deletions: index builds */
VpqListing *
vpq_listing_all(VpqTable *t)
{
	return list_own(t, NULL);
}

/* At a transaction's end its listings' memory went with it */
void
vpq_listings_xact_end(void)
{
	memset(&cache, 0, sizeof(cache));
	memset(own_cache, 0, sizeof(own_cache));
	own_next = 0;
}

/*
 * A listing as bytes, for a parallel scan's workers: each file's path,
 * size, modification time and row groups' rows
 */
Size
vpq_listing_serialized_size(VpqListing *l)
{
	Size		size = sizeof(int32);

	for (int i = 0; i < l->nfiles; i++)
		size += sizeof(int32) + strlen(l->files[i].path) + 1 + sizeof(int64) * 4 + sizeof(int32) +
			sizeof(int64) * l->files[i].nunits;
	return MAXALIGN(size);
}

void
vpq_listing_serialize(VpqListing *l, char *out)
{
	char	   *p = out;
	int32		n = l->nfiles;

	memcpy(p, &n, sizeof(n));
	p += sizeof(n);
	for (int i = 0; i < l->nfiles; i++)
	{
		VpqFileEntry *f = &l->files[i];
		int32		len = (int32) strlen(f->path) + 1;
		int64		v[4] = {f->size, f->mtime, f->file_id, (int64) f->first_rowid};
		int32		nu = f->nunits;

		memcpy(p, &len, sizeof(len));
		p += sizeof(len);
		memcpy(p, f->path, len);
		p += len;
		memcpy(p, v, sizeof(v));
		p += sizeof(v);
		memcpy(p, &nu, sizeof(nu));
		p += sizeof(nu);
		memcpy(p, f->unit_rows, sizeof(int64) * nu);
		p += sizeof(int64) * nu;
	}
}

VpqListing *
vpq_listing_deserialize(Relation rel, VpqTable *t, const char *in, Snapshot snapshot)
{
	VpqListing *l = new_listing();
	MemoryContext old = MemoryContextSwitchTo(l->cxt);
	const char *p = in;
	int32		n;

	memcpy(&n, p, sizeof(n));
	p += sizeof(n);
	l->nfiles = n;
	l->files = palloc0(sizeof(VpqFileEntry) * Max(n, 1));
	for (int i = 0; i < n; i++)
	{
		VpqFileEntry *f = &l->files[i];
		int32		len;
		int64		v[4];
		int32		nu;
		int64		first = 0;

		memcpy(&len, p, sizeof(len));
		p += sizeof(len);
		f->path = pstrdup(p);
		p += len;
		memcpy(v, p, sizeof(v));
		p += sizeof(v);
		f->size = v[0];
		f->mtime = v[1];
		f->file_id = v[2];
		f->first_rowid = (uint64) v[3];
		memcpy(&nu, p, sizeof(nu));
		p += sizeof(nu);
		f->nunits = nu;
		f->unit_rows = palloc(sizeof(int64) * Max(nu, 1));
		f->unit_first = palloc(sizeof(int64) * Max(nu, 1));
		memcpy(f->unit_rows, p, sizeof(int64) * nu);
		p += sizeof(int64) * nu;
		for (int g = 0; g < nu; g++)
		{
			f->unit_first[g] = first;
			first += f->unit_rows[g];
		}
		f->rows = first;
		l->rows += f->rows;
		l->bytes += f->size;
	}
	make_units(l);
	MemoryContextSwitchTo(old);
	return l;
}

/*
 * A file of the listing, open: of its facts where the listing or shared
 * memory has them, no footer read (PQ1, PQ4); else its footer, read once in
 * the process, and an external file's facts kept in shared memory for the
 * next process
 */
vpq_file *
vpq_open_file(VpqFileEntry *f)
{
	vpq_file   *h = NULL;
	int			rc;

	vpq_engine_ready();
	if (f->facts == NULL && f->mtime >= 0)
	{
		MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(f->path));

		f->facts = vpq_footers_get(f->path, f->size, f->mtime, &f->facts_len);
		MemoryContextSwitchTo(old);
	}
	if (f->facts != NULL)
	{
		rc = vpq_file_open_facts(f->path, f->size, f->mtime, f->facts, f->facts_len, &h);
		if (rc == VPQ_NOT_FOUND)
			h = NULL;
		else if (rc != VPQ_OK)
			vpq_engine_error(rc, psprintf("opening \"%s\"", f->path));
	}
	if (h == NULL)
	{
		rc = vpq_file_open(f->path, f->size, f->mtime, &h);
		if (rc != VPQ_OK)
			vpq_engine_error(rc, psprintf("opening \"%s\"", f->path));
		if (f->mtime >= 0 && vpq_footers_enabled())
		{
			char	   *facts;
			size_t		len;

			/* the next process's: every chunk's statistics, for its plan and its pruning */
			if (vpq_file_facts(h, VPQ_FACTS_STATS, &facts, &len) == VPQ_OK)
			{
				MemoryContext old = MemoryContextSwitchTo(GetMemoryChunkContext(f->path));

				vpq_footers_put(f->path, f->size, f->mtime, facts, len);
				f->facts = palloc(len);
				memcpy((char *) f->facts, facts, len);
				f->facts_len = len;
				free(facts);
				MemoryContextSwitchTo(old);
			}
			/*
			 * and its footer's index, for their readers of some of its
			 * columns (PQ11) -- now where the session's scans read their
			 * units on the CPU; where a device may read them, as a reader
			 * on the CPU first reads the footer whole: the walk is about
			 * 10 ms of ClickBench's footer, every device query's cold try
			 */
			if (!vpq_device_may_read())
				vpq_file_index_keep(f, h);
		}
	}
	/* an external file changed since the statement listed it is not the file it listed */
	if (f->mtime >= 0 && vpq_file_size(h) != f->size)
	{
		vpq_file_close(h);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the file \"%s\" changed while the statement read it", f->path)));
	}
	return h;
}

/*
 * PQ11 (pg_vector_executor.md §3.37): an external file's footer's index in
 * the shared footer cache, beside its facts -- under its location marked,
 * which names no file -- so that a process reads the file's row groups
 * through a footer of its scan's columns alone
 */
static char *
index_key(const char *path)
{
	return psprintf("\x01index %s", path);
}

/* The index of an external file's footer from the shared footer cache, in the caller's memory; NULL none */
const char *
vpq_file_index_get(VpqFileEntry *f, Size *len)
{
	if (f->mtime < 0 || !vpq_enable_projected_footers || !vpq_footers_enabled())
		return NULL;
	return vpq_footers_get(index_key(f->path), f->size, f->mtime, len);
}

/*
 * The index of an external file's footer, which the process holds, kept in
 * the shared footer cache -- where no other process has kept it since
 */
void
vpq_file_index_keep(VpqFileEntry *f, vpq_file *h)
{
	char	   *index;
	size_t		len;
	Size		have;
	const char *kept;

	if (f->mtime < 0 || !vpq_enable_projected_footers || !vpq_footers_enabled())
		return;
	kept = vpq_footers_get(index_key(f->path), f->size, f->mtime, &have);
	if (kept != NULL)
	{
		pfree((char *) kept);
		return;
	}
	if (vpq_file_index(h, &index, &len) == VPQ_OK)
	{
		vpq_footers_put(index_key(f->path), f->size, f->mtime, index, len);
		free(index);
	}
}

/*
 * Whether vexec may hand the session's scans' units to a device for its
 * executor to read -- vexec.gpu on, device units allowed -- so that a scan
 * reads them on the CPU only where the device does not (PQ11, PQ12)
 */
bool
vpq_device_may_read(void)
{
	const char *gpu = GetConfigOption("vexec.gpu", true, false);

	return vpq_enable_device_units && gpu != NULL && strcmp(gpu, "off") != 0;
}

/* The listing's file holding a row id, or -1 */
int
vpq_file_entry_of_row(VpqListing *l, uint64 rowid)
{
	int			lo = 0;
	int			hi = l->nfiles - 1;

	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;
		VpqFileEntry *f = &l->files[mid];

		if (rowid < f->first_rowid)
			hi = mid - 1;
		else if (rowid >= f->first_rowid + (uint64) f->rows)
			lo = mid + 1;
		else
			return mid;
	}
	return -1;
}

/*
 * A file's statistics of a column, every row group's, decoded as it is
 * first asked for: an own file's from the catalog's, an external file's
 * from its footer -- or its facts, which keep it
 */
static void
ensure_stats(VpqTable *t, VpqFileEntry *f, AttrNumber attnum)
{
	MemoryContext cxt = GetMemoryChunkContext(f->unit_rows);
	MemoryContext old;

	if (f->stats == NULL)
	{
		old = MemoryContextSwitchTo(cxt);
		f->nstats = t->natts;
		f->stats = palloc0(sizeof(VpqStat) * Max(f->nunits * t->natts, 1));
		f->col_bytes = palloc0(sizeof(int64) * Max(f->nunits * t->natts, 1));
		f->stat_attnums = palloc0(Max(t->natts, 1));
		for (int i = 0; i < f->nunits * t->natts; i++)
			f->stats[i].nulls = -1;
		MemoryContextSwitchTo(old);
	}
	if (attnum < 1 || attnum > t->natts || f->stat_attnums[attnum - 1] != VPQ_STAT_UNREAD)
		return;
	old = MemoryContextSwitchTo(cxt);
	if (!t->external)
	{
		int			c = -1;

		if (f->stats_raw != NULL)
		{
			if (f->stats_index == NULL)
				f->stats_index = vpq_stats_index(f->stats_raw);
			c = vpq_stats_index_column(f->stats_index, attnum);
		}
		if (c < 0)
			f->stat_attnums[attnum - 1] = VPQ_STAT_ABSENT;
		else
		{
			int			n = Min(f->nunits, vpq_stats_index_row_groups(f->stats_index));

			for (int g = 0; g < n; g++)
				vpq_stats_decode(t, f->stats_raw, f->stats_index, c, g, &f->stats[g * t->natts + attnum - 1],
								 &f->col_bytes[g * t->natts + attnum - 1]);
			f->stat_attnums[attnum - 1] = VPQ_STAT_READ;
			vpq_listing_stats.stat_columns++;
		}
		MemoryContextSwitchTo(old);
		return;
	}
	{
		VpqColumn  *col = &t->cols[attnum - 1];
		vpq_file   *h = vpq_open_file(f);

		PG_TRY();
		{
			int			fcol = -1;
			int			ncols = vpq_file_columns(h);

			for (int i = 0; i < ncols && fcol < 0; i++)
			{
				const char *name;
				int			field_id;
				vpq_type	type;
				bool		nullable;

				(void) vpq_file_column(h, i, &name, &field_id, &type, &nullable);
				if (strcmp(name, col->name) == 0)
					fcol = i;
			}
			if (fcol < 0)
				for (int i = 0; i < ncols; i++)
				{
					const char *name;
					int			field_id;
					vpq_type	type;
					bool		nullable;

					(void) vpq_file_column(h, i, &name, &field_id, &type, &nullable);
					if (pg_strcasecmp(name, col->name) == 0)
					{
						if (fcol >= 0)
						{
							fcol = -1;
							break;
						}
						fcol = i;
					}
				}
			if (fcol >= 0)
			{
				for (int g = 0; g < f->nunits; g++)
				{
					(void) vpq_stat_from_footer(t, col, h, fcol, g, &f->stats[g * t->natts + attnum - 1]);
					f->col_bytes[g * t->natts + attnum - 1] = vpq_file_column_bytes(h, g, fcol);
				}
				/* PQ18: a geometry column's bounds, for && (geo.c) */
				if (col->geo == VPQ_GEO_GEOMETRY)
					vpq_geo_stats(h, fcol, f->nunits, &f->stats[attnum - 1], t->natts);
			}
		}
		PG_FINALLY();
		{
			vpq_file_close(h);
		}
		PG_END_TRY();
		f->stat_attnums[attnum - 1] = VPQ_STAT_READ;
		vpq_listing_stats.stat_columns++;
	}
	MemoryContextSwitchTo(old);
}

const VpqStat *
vpq_unit_stat(VpqTable *t, VpqFileEntry *f, int rg, AttrNumber attnum)
{
	if (attnum < 1 || attnum > t->natts || t->cols[attnum - 1].kind == VPQ_K_DROPPED)
		return NULL;
	ensure_stats(t, f, attnum);
	if (!t->external && f->stat_attnums[attnum - 1] != VPQ_STAT_READ)
		return NULL;
	return &f->stats[rg * t->natts + attnum - 1];
}

int64
vpq_unit_bytes(VpqTable *t, VpqFileEntry *f, int rg, AttrNumber attnum)
{
	if (attnum < 1 || attnum > t->natts)
		return 0;
	ensure_stats(t, f, attnum);
	return f->col_bytes[rg * t->natts + attnum - 1];
}
