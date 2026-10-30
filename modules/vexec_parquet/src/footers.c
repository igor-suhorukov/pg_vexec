/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * footers.c
 *	  External files' facts in shared memory (pg_vector_executor.md §3.34,
 *	  PQ4): what a scan asks of a file's footer -- its row groups, its
 *	  columns, each chunk's codec, place, bytes and statistics (engine.h) --
 *	  made by the first process that reads the footer and kept, so that the
 *	  next process -- every harness try's fresh backend, a parallel scan's
 *	  workers -- plans, prunes and hands device units without reading it.
 *
 * The key.  A file's location, size and modification time, as its listing
 * has them: a file that changes is another key, and its old entry is never
 * asked for again.  An own table's files keep their facts in their catalog
 * rows instead (stats.c).
 *
 * What is kept.  vexec_parquet.footer_cache_size of the server's shared
 * memory from its start: an open-addressed table of slots, and a ring of
 * the entries' bytes -- a slot's location, then its facts -- written one
 * after another, a lap beginning again at the ring's start; an entry is
 * there while no later one was written over its bytes, a slot whose bytes
 * are gone is free again.  So the oldest entries go first, as the ring
 * passes over them.  One LWLock: shared to find an entry and copy it out,
 * exclusive to add one.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

#include "vexec_parquet.h"

#define FF_PROBES		16		/* slots looked at for a key */
#define FF_SLOT_BYTES	(16 * 1024) /* the ring's bytes a slot, at least */

typedef struct FfSlot
{
	uint64		hash;			/* 0: never used */
	int64		size;
	int64		mtime;
	uint64		pos;			/* its bytes' place in the ring, counted from the server's start */
	uint32		len;			/* uint32 its location's bytes, the location, the facts */
	uint32		pad;
} FfSlot;

typedef struct FfShared
{
	LWLock	   *lock;
	uint32		nslots;			/* a power of two */
	uint64		ring_bytes;
	uint64		head;			/* the next entry's place: bytes written since the start */
	pg_atomic_uint64 hits;
	pg_atomic_uint64 misses;
	pg_atomic_uint64 kept;
	FfSlot		slots[FLEXIBLE_ARRAY_MEMBER];	/* then the ring */
} FfShared;

int			vpq_footer_cache_size = 64 * 1024;	/* kB */
bool		vpq_enable_footer_cache = true;

static FfShared *ff = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* The slots of vexec_parquet.footer_cache_size: a power of two, 0 where too small for one entry */
static uint32
slot_count(void)
{
	Size		bytes = (Size) vpq_footer_cache_size * 1024;
	Size		n;

	if (bytes < 1024 * 1024)
		return 0;
	n = Max(bytes / FF_SLOT_BYTES, 256);
	return pg_prevpower2_32((uint32) Min(n, (Size) PG_UINT32_MAX / 2));
}

static Size
shared_size(void)
{
	Size		bytes = (Size) vpq_footer_cache_size * 1024;

	return Max(bytes, add_size(offsetof(FfShared, slots), mul_size(slot_count(), sizeof(FfSlot))) + 1024 * 1024);
}

static char *
ring(void)
{
	return (char *) ff + MAXALIGN(offsetof(FfShared, slots) + (Size) ff->nslots * sizeof(FfSlot));
}

static void
ff_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	if (slot_count() == 0)
		return;
	RequestAddinShmemSpace(shared_size());
	RequestNamedLWLockTranche("vexec_parquet footer cache", 1);
}

static void
ff_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();
	if (slot_count() == 0)
		return;
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	ff = ShmemInitStruct("vexec_parquet footer cache", shared_size(), &found);
	if (!found)
	{
		ff->lock = &(GetNamedLWLockTranche("vexec_parquet footer cache"))->lock;
		ff->nslots = slot_count();
		ff->ring_bytes = shared_size() - MAXALIGN(offsetof(FfShared, slots) + (Size) ff->nslots * sizeof(FfSlot));
		ff->head = 0;
		pg_atomic_init_u64(&ff->hits, 0);
		pg_atomic_init_u64(&ff->misses, 0);
		pg_atomic_init_u64(&ff->kept, 0);
		memset(ff->slots, 0, (Size) ff->nslots * sizeof(FfSlot));
	}
	LWLockRelease(AddinShmemInitLock);
}

/* From _PG_init, while the postmaster preloads the library */
void
vpq_footers_install(void)
{
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = ff_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = ff_shmem_startup;
}

bool
vpq_footers_enabled(void)
{
	return ff != NULL && vpq_enable_footer_cache;
}

static uint64
key_hash(const char *path, int64 size, int64 mtime)
{
	uint64		h = hash_bytes_extended((const unsigned char *) path, (int) strlen(path),
										(uint64) size * 0x9E3779B97F4A7C15ULL ^ (uint64) mtime);

	return h != 0 ? h : 1;
}

/* Whether a slot's bytes are still in the ring: no later entry written over them */
static inline bool
slot_live(const FfSlot *s)
{
	return s->hash != 0 && s->pos + s->len <= ff->head && ff->head - s->pos <= ff->ring_bytes;
}

static bool
slot_is(const FfSlot *s, uint64 hash, const char *path, uint32 plen, int64 size, int64 mtime)
{
	uint32		stored;
	const char *bytes;

	if (s->hash != hash || s->size != size || s->mtime != mtime || !slot_live(s))
		return false;
	bytes = ring() + s->pos % ff->ring_bytes;
	memcpy(&stored, bytes, sizeof(stored));
	return stored == plen && memcmp(bytes + sizeof(stored), path, plen) == 0;
}

/* A file's facts another process kept, copied out in the current memory context, or NULL */
char *
vpq_footers_get(const char *path, int64 size, int64 mtime, Size *len)
{
	uint64		hash;
	uint32		plen = (uint32) strlen(path);
	char	   *out = NULL;

	*len = 0;
	if (!vpq_footers_enabled() || mtime < 0)
		return NULL;
	hash = key_hash(path, size, mtime);
	LWLockAcquire(ff->lock, LW_SHARED);
	for (uint32 k = 0; k < Min(FF_PROBES, ff->nslots); k++)
	{
		FfSlot	   *s = &ff->slots[(hash + k) & (ff->nslots - 1)];

		if (s->hash == 0)
			break;
		if (slot_is(s, hash, path, plen, size, mtime))
		{
			*len = s->len - sizeof(uint32) - plen;
			out = palloc(Max(*len, 1));
			memcpy(out, ring() + s->pos % ff->ring_bytes + sizeof(uint32) + plen, *len);
			break;
		}
	}
	LWLockRelease(ff->lock);
	pg_atomic_fetch_add_u64(out ? &ff->hits : &ff->misses, 1);
	return out;
}

bool
vpq_footers_set_byte(const char *path, int64 size, int64 mtime, Size at, char value)
{
	uint64		hash;
	uint32		plen = (uint32) strlen(path);
	bool		set = false;

	if (!vpq_footers_enabled() || mtime < 0)
		return false;
	hash = key_hash(path, size, mtime);
	/* shared: the entry's bytes stay where they are; a byte read before or after it is written, both true */
	LWLockAcquire(ff->lock, LW_SHARED);
	for (uint32 k = 0; k < Min(FF_PROBES, ff->nslots); k++)
	{
		FfSlot	   *s = &ff->slots[(hash + k) & (ff->nslots - 1)];

		if (s->hash == 0)
			break;
		if (slot_is(s, hash, path, plen, size, mtime))
		{
			if (sizeof(uint32) + plen + at < s->len)
			{
				*(volatile char *) (ring() + s->pos % ff->ring_bytes + sizeof(uint32) + plen + at) = value;
				set = true;
			}
			break;
		}
	}
	LWLockRelease(ff->lock);
	return set;
}

/* A file's facts kept for the next process; one past an eighth of the ring is not */
void
vpq_footers_put(const char *path, int64 size, int64 mtime, const char *facts, Size facts_len)
{
	uint64		hash;
	uint32		plen = (uint32) strlen(path);
	Size		len = sizeof(uint32) + plen + facts_len;
	FfSlot	   *slot = NULL;
	FfSlot	   *oldest = NULL;
	uint64		pos;
	char	   *at;

	if (!vpq_footers_enabled() || mtime < 0 || len > ff->ring_bytes / 8)
		return;
	hash = key_hash(path, size, mtime);
	LWLockAcquire(ff->lock, LW_EXCLUSIVE);
	for (uint32 k = 0; k < Min(FF_PROBES, ff->nslots); k++)
	{
		FfSlot	   *s = &ff->slots[(hash + k) & (ff->nslots - 1)];

		if (slot_is(s, hash, path, plen, size, mtime))
		{
			/* another process kept it first */
			LWLockRelease(ff->lock);
			return;
		}
		if (slot == NULL && (s->hash == 0 || !slot_live(s)))
			slot = s;
		if (oldest == NULL || s->pos < oldest->pos)
			oldest = s;
		if (s->hash == 0)
			break;
	}
	if (slot == NULL)
		slot = oldest;
	/* the entry's bytes whole in the ring: past its end, the next lap's start */
	pos = ff->head;
	if (pos % ff->ring_bytes + len > ff->ring_bytes)
		pos += ff->ring_bytes - pos % ff->ring_bytes;
	at = ring() + pos % ff->ring_bytes;
	memcpy(at, &plen, sizeof(plen));
	memcpy(at + sizeof(plen), path, plen);
	memcpy(at + sizeof(plen) + plen, facts, facts_len);
	ff->head = pos + len;
	slot->hash = hash;
	slot->size = size;
	slot->mtime = mtime;
	slot->pos = pos;
	slot->len = (uint32) len;
	LWLockRelease(ff->lock);
	pg_atomic_fetch_add_u64(&ff->kept, 1);
}

/* The cache's entries and their bytes now, and its finds, misses and entries kept since the server started */
void
vpq_footers_stats(int64 *entries, int64 *bytes, int64 *hits, int64 *misses, int64 *kept)
{
	*entries = *bytes = *hits = *misses = *kept = 0;
	if (ff == NULL)
		return;
	LWLockAcquire(ff->lock, LW_SHARED);
	for (uint32 i = 0; i < ff->nslots; i++)
		if (slot_live(&ff->slots[i]))
		{
			(*entries)++;
			*bytes += ff->slots[i].len;
		}
	LWLockRelease(ff->lock);
	*hits = (int64) pg_atomic_read_u64(&ff->hits);
	*misses = (int64) pg_atomic_read_u64(&ff->misses);
	*kept = (int64) pg_atomic_read_u64(&ff->kept);
}
