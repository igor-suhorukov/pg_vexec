/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * condcache.c
 *	  The condition cache (pg_vector_executor.md §3.28, F7): for a vector
 *	  scan's quals that are the same for every statement -- their functions
 *	  immutable, no parameter, no subplan -- the granules of a unit of
 *	  storage that never changes where no row is true of them, so that the
 *	  next scan of the same quals has its source pass them over, as
 *	  ClickHouse's query condition cache passes its marks over
 *	  (CH:src/Interpreters/Cache/QueryConditionCache.h; on by default in
 *	  26.9, its ClickBench Q20 to Q23 1.7 to 6 times faster).
 *
 * The key.  The scan's quals, copied with their Vars naming the scan's
 * relation as 1, printed by nodeToString() with every location left out,
 * hashed twice into 128 bits: two statements' quals of one hash are taken
 * for the same -- the same functions, operators, constants and collations
 * over the same attributes.  A source names a unit the same way, by 128
 * bits no other unit has while vexec may keep them (vexec_source.h,
 * VexecSourceBatch.unit_id): a MergeTree part by its storage, name, rows
 * and time of writing, which never come back once the part is gone.
 *
 * What is kept.  An entry a unit, quals and run of CC_GRANULES of the
 * unit's granules: a bit a granule, set where a scan saw every row of the
 * granule and none passed the quals -- no row of it deleted, none left to
 * PostgreSQL's evaluator, no running bound or join filter before the quals
 * (vecscan.c, cc_batch_done()).  A bit is only ever set: a row of a unit
 * that never changes is true of immutable quals for every statement or for
 * none.  And a bit a granule a scan of the quals has seen whole, none of its
 * rows true or some (W34, pg_vector_executor.md §3.30): the device's search
 * of a unit's granules (vecscan.c, vexec_scan_units_none()) or a CPU scan's
 * batches, so that a device request of a unit whose every granule was seen
 * searches none -- the bits of those no row of which is true are kept
 * already.  The table full, every entry goes at once, as streams.c's of the
 * ClickHouse engines go; an entry of a part merged away is never asked for
 * again.
 *
 * In shared memory: vexec.condition_cache_size of it from the server's
 * start, an open-addressed table under one LWLock, shared to find an entry
 * and set its bits -- atomically -- exclusive to add one.
 * vexec.enable_condition_cache off, a scan neither reads it nor fills it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "fmgr.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/builtins.h"

#include "vexec.h"
#include "exec/exec.h"

#define CC_GRANULES		4096	/* an entry's run of a unit's granules */
#define CC_WORDS		(CC_GRANULES / 64)

typedef struct CcKey
{
	uint64		unit[2];
	uint64		quals[2];
	uint32		run;			/* granules [run * CC_GRANULES, (run + 1) * CC_GRANULES) */
	uint32		pad;
} CcKey;

typedef struct CcEntry
{
	uint32		hash;			/* 0: empty */
	uint32		pad;
	CcKey		key;
	pg_atomic_uint64 none[CC_WORDS];	/* a bit a granule no row of which is true */
	pg_atomic_uint64 seen[CC_WORDS];	/* W34: a bit a granule a scan saw whole */
} CcEntry;

/*
 * F21 (§3.38): a relation's figures under its quals' order-free key
 * (vexec_condcache_tally_key()) -- the granules scans of them saw whole and,
 * of those, the ones no row of which is true, each counted as its bit is
 * first set -- from which planning takes the share of a scan's granules the
 * cache leaves it.  CC_TALLIES of them, emptied with the table.
 */
#define CC_TALLIES		256

typedef struct CcTally
{
	Oid			dbid;
	Oid			relid;
	uint64		key[2];
	bool		used;
	pg_atomic_uint64 seen;
	pg_atomic_uint64 none;
	pg_atomic_uint64 rows;		/* of the granules seen, where the scans counted them */
} CcTally;

typedef struct CcShared
{
	LWLock	   *lock;
	uint32		nslots;			/* a power of two, twice the entries or more */
	uint32		used;
	pg_atomic_uint64 kept;		/* granules set since the server started */
	pg_atomic_uint64 passed;	/* granules handed to a source to pass over */
	pg_atomic_uint64 emptied;	/* times the full table was emptied */
	CcTally		tallies[CC_TALLIES];	/* F21 */
	CcEntry		slots[FLEXIBLE_ARRAY_MEMBER];
} CcShared;

int			vexec_condition_cache_size = 64 * 1024; /* kB */
bool		vexec_enable_condition_cache = true;

static CcShared *cc = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

/* The slots vexec.condition_cache_size holds: a power of two, none under 16 */
static uint32
slot_count(void)
{
	Size		bytes = (Size) vexec_condition_cache_size * 1024;
	uint32		n;

	if (bytes < offsetof(CcShared, slots) + 16 * sizeof(CcEntry))
		return 0;
	n = (uint32) Min((bytes - offsetof(CcShared, slots)) / sizeof(CcEntry), (Size) PG_UINT32_MAX / 2);
	return pg_prevpower2_32(n);
}

static Size
shared_size(void)
{
	return add_size(offsetof(CcShared, slots), mul_size(slot_count(), sizeof(CcEntry)));
}

static void
cc_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	if (slot_count() == 0)
		return;
	RequestAddinShmemSpace(shared_size());
	RequestNamedLWLockTranche("vexec condition cache", 1);
}

static void
cc_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();
	if (slot_count() == 0)
		return;
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	cc = ShmemInitStruct("vexec condition cache", shared_size(), &found);
	if (!found)
	{
		cc->lock = &(GetNamedLWLockTranche("vexec condition cache"))->lock;
		cc->nslots = slot_count();
		cc->used = 0;
		pg_atomic_init_u64(&cc->kept, 0);
		pg_atomic_init_u64(&cc->passed, 0);
		pg_atomic_init_u64(&cc->emptied, 0);
		for (int i = 0; i < CC_TALLIES; i++)
		{
			cc->tallies[i].used = false;
			pg_atomic_init_u64(&cc->tallies[i].seen, 0);
			pg_atomic_init_u64(&cc->tallies[i].none, 0);
			pg_atomic_init_u64(&cc->tallies[i].rows, 0);
		}
		for (uint32 i = 0; i < cc->nslots; i++)
		{
			cc->slots[i].hash = 0;
			for (int w = 0; w < CC_WORDS; w++)
			{
				pg_atomic_init_u64(&cc->slots[i].none[w], 0);
				pg_atomic_init_u64(&cc->slots[i].seen[w], 0);
			}
		}
	}
	LWLockRelease(AddinShmemInitLock);
}

/* From _PG_init: the table is made with the postmaster's shared memory. */
void
vexec_condcache_install(void)
{
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = cc_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = cc_shmem_startup;
}

/* Whether a scan may use the cache: made at the server's start, and on */
bool
vexec_condcache_enabled(void)
{
	return cc != NULL && vexec_enable_condition_cache;
}

/* ---- the key ---- */

/* Whether quals may be keyed: no parameter, no subplan, no Var but the scan relation's own */
static bool
not_keyable_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param) || IsA(node, SubPlan) || IsA(node, AlternativeSubPlan) || IsA(node, SubLink))
		return true;
	if (IsA(node, Var))
	{
		Var		   *v = (Var *) node;

		return v->varlevelsup != 0 || v->varno != *(Index *) context;
	}
	return expression_tree_walker(node, not_keyable_walker, context);
}

/* A copy of the quals whose Vars name relation 1, as any scan of the attributes would */
static Node *
normalize_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		Var		   *v = (Var *) copyObject(node);

		v->varno = 1;
		v->varnosyn = 1;
		v->varattnosyn = v->varattno;
		v->location = -1;
		return (Node *) v;
	}
	return expression_tree_mutator(node, normalize_mutator, context);
}

/*
 * The key of a scan's quals over its relation scanrelid, false where they
 * may not be kept: a function not immutable, a parameter, a subplan
 */
bool
vexec_condcache_key(List *quals, Index scanrelid, uint64 *key)
{
	char	   *text;
	const char *p;
	StringInfoData buf;

	if (quals == NIL || contain_mutable_functions((Node *) quals) ||
		not_keyable_walker((Node *) quals, &scanrelid))
		return false;
	text = nodeToString(normalize_mutator((Node *) quals, NULL));
	/* every location as -1: where the statement's text put them is no part of the quals */
	initStringInfo(&buf);
	for (p = text; *p;)
	{
		if (strncmp(p, ":location ", 10) == 0)
		{
			appendStringInfoString(&buf, ":location -1");
			p += 10;
			if (*p == '-')
				p++;
			while (*p >= '0' && *p <= '9')
				p++;
			continue;
		}
		appendStringInfoChar(&buf, *p++);
	}
	key[0] = hash_bytes_extended((const unsigned char *) buf.data, buf.len, UINT64CONST(0x5bd1e9955bd1e995));
	key[1] = hash_bytes_extended((const unsigned char *) buf.data, buf.len, UINT64CONST(0xc2b2ae3d27d4eb4f));
	if (key[0] == 0 && key[1] == 0)
		key[1] = 1;
	pfree(buf.data);
	pfree(text);
	return true;
}

/* ---- the table ---- */

static uint32
key_hash(const CcKey *key)
{
	uint32		h = hash_bytes((const unsigned char *) key, sizeof(CcKey));

	return h == 0 ? 1 : h;
}

static void
make_key(CcKey *key, const uint64 *quals, const uint64 *unit, uint32 run)
{
	memset(key, 0, sizeof(*key));
	key->unit[0] = unit[0];
	key->unit[1] = unit[1];
	key->quals[0] = quals[0];
	key->quals[1] = quals[1];
	key->run = run;
}

/* The key's slot, or the empty one its probe ends at; under the lock */
static CcEntry *
probe(const CcKey *key, uint32 h)
{
	uint32		mask = cc->nslots - 1;

	for (uint32 i = h & mask;; i = (i + 1) & mask)
	{
		CcEntry    *e = &cc->slots[i];

		if (e->hash == 0 || (e->hash == h && memcmp(&e->key, key, sizeof(CcKey)) == 0))
			return e;
	}
}

/*
 * A unit's granules of ngranules no row of which is true of the quals, a bit
 * each in out (zeroed here, (ngranules + 63) / 64 words): how many of
 * [begin, end) are set, 0 where none is
 */
int64
vexec_condcache_get(const uint64 *quals, const uint64 *unit, uint32 ngranules, uint32 begin, uint32 end,
					uint64 *out)
{
	uint32		nwords = (ngranules + 63) / 64;
	uint32		nruns = (ngranules + CC_GRANULES - 1) / CC_GRANULES;
	int64		n = 0;

	if (ngranules == 0)
		return 0;
	memset(out, 0, sizeof(uint64) * nwords);
	if (cc == NULL)
		return 0;
	LWLockAcquire(cc->lock, LW_SHARED);
	for (uint32 r = 0; r < nruns; r++)
	{
		CcKey		key;
		CcEntry    *e;

		make_key(&key, quals, unit, r);
		e = probe(&key, key_hash(&key));
		if (e->hash == 0)
			continue;
		for (uint32 w = 0; w < CC_WORDS && r * CC_WORDS + w < nwords; w++)
			out[r * CC_WORDS + w] = pg_atomic_read_u64(&e->none[w]);
	}
	LWLockRelease(cc->lock);
	/* the last word's bits past the unit's granules: none */
	if (ngranules % 64 != 0)
		out[nwords - 1] &= (UINT64CONST(1) << (ngranules % 64)) - 1;
	for (uint32 g = begin; g < Min(end, ngranules); g++)
		n += (out[g / 64] >> (g % 64)) & 1;
	if (n > 0)
		pg_atomic_fetch_add_u64(&cc->passed, (uint64) n);
	return n;
}

/*
 * The entry of the key, made where there is none -- every entry going first
 * where the table is full; under the exclusive lock
 */
static CcEntry *
entry_made(const CcKey *key, uint32 h)
{
	CcEntry    *e;

	/* full: every entry goes, the slots emptied for the ones to come */
	if (cc->used >= cc->nslots / 2)
	{
		for (uint32 i = 0; i < cc->nslots; i++)
		{
			if (cc->slots[i].hash == 0)
				continue;
			cc->slots[i].hash = 0;
			for (int w = 0; w < CC_WORDS; w++)
			{
				pg_atomic_write_u64(&cc->slots[i].none[w], 0);
				pg_atomic_write_u64(&cc->slots[i].seen[w], 0);
			}
		}
		cc->used = 0;
		/* F21: the figures with the bits they counted */
		for (int i = 0; i < CC_TALLIES; i++)
		{
			cc->tallies[i].used = false;
			pg_atomic_write_u64(&cc->tallies[i].seen, 0);
			pg_atomic_write_u64(&cc->tallies[i].none, 0);
			pg_atomic_write_u64(&cc->tallies[i].rows, 0);
		}
		pg_atomic_fetch_add_u64(&cc->emptied, 1);
	}
	e = probe(key, h);
	if (e->hash == 0)
	{
		e->key = *key;
		for (int w = 0; w < CC_WORDS; w++)
		{
			pg_atomic_write_u64(&e->none[w], 0);
			pg_atomic_write_u64(&e->seen[w], 0);
		}
		e->hash = h;
		cc->used++;
	}
	return e;
}

/* Granule g of the unit: no row of it is true of the quals -- and so it was seen whole */
int
vexec_condcache_set(const uint64 *quals, const uint64 *unit, int64 granule)
{
	CcKey		key;
	uint32		h;
	CcEntry    *e;
	uint32		bit = (uint32) (granule % CC_GRANULES);
	int			first = 0;

	if (cc == NULL || granule < 0 || granule >= (int64) PG_UINT32_MAX)
		return 0;
	make_key(&key, quals, unit, (uint32) (granule / CC_GRANULES));
	h = key_hash(&key);
	LWLockAcquire(cc->lock, LW_SHARED);
	e = probe(&key, h);
	if (e->hash == 0)
	{
		LWLockRelease(cc->lock);
		LWLockAcquire(cc->lock, LW_EXCLUSIVE);
		e = entry_made(&key, h);
	}
	if ((pg_atomic_fetch_or_u64(&e->seen[bit / 64], UINT64CONST(1) << (bit % 64)) &
		 (UINT64CONST(1) << (bit % 64))) == 0)
		first |= 2;
	if ((pg_atomic_fetch_or_u64(&e->none[bit / 64], UINT64CONST(1) << (bit % 64)) &
		 (UINT64CONST(1) << (bit % 64))) == 0)
	{
		pg_atomic_fetch_add_u64(&cc->kept, 1);
		first |= 1;
	}
	LWLockRelease(cc->lock);
	return first;
}

/*
 * W34: granules of the unit a scan of the quals saw whole -- those no row of
 * which is true set by vexec_condcache_set() as well -- n of them, in
 * granules
 */
int
vexec_condcache_seen(const uint64 *quals, const uint64 *unit, const int32 *granules, int n)
{
	int			first = 0;

	if (cc == NULL)
		return 0;
	for (int i = 0; i < n;)
	{
		CcKey		key;
		uint32		h;
		CcEntry    *e;
		uint32		run;

		if (granules[i] < 0)
		{
			i++;
			continue;
		}
		run = (uint32) granules[i] / CC_GRANULES;
		make_key(&key, quals, unit, run);
		h = key_hash(&key);
		LWLockAcquire(cc->lock, LW_SHARED);
		e = probe(&key, h);
		if (e->hash == 0)
		{
			LWLockRelease(cc->lock);
			LWLockAcquire(cc->lock, LW_EXCLUSIVE);
			e = entry_made(&key, h);
		}
		/* the granules of this entry's run, in turn */
		for (; i < n && granules[i] >= 0 && (uint32) granules[i] / CC_GRANULES == run; i++)
		{
			uint32		bit = (uint32) granules[i] % CC_GRANULES;

			if ((pg_atomic_fetch_or_u64(&e->seen[bit / 64], UINT64CONST(1) << (bit % 64)) &
				 (UINT64CONST(1) << (bit % 64))) == 0)
				first++;
		}
		LWLockRelease(cc->lock);
	}
	return first;
}

/* W34: whether every one of these n granules of the unit was seen whole by a scan of the quals */
bool
vexec_condcache_all_seen(const uint64 *quals, const uint64 *unit, const int32 *granules, int n)
{
	bool		all = true;
	CcEntry    *e = NULL;
	uint32		run = PG_UINT32_MAX;

	if (cc == NULL || n <= 0)
		return false;
	LWLockAcquire(cc->lock, LW_SHARED);
	for (int i = 0; i < n && all; i++)
	{
		uint32		bit;

		if (granules[i] < 0)
		{
			all = false;
			break;
		}
		if ((uint32) granules[i] / CC_GRANULES != run)
		{
			CcKey		key;

			run = (uint32) granules[i] / CC_GRANULES;
			make_key(&key, quals, unit, run);
			e = probe(&key, key_hash(&key));
			if (e->hash == 0)
			{
				all = false;
				break;
			}
		}
		bit = (uint32) granules[i] % CC_GRANULES;
		all = (pg_atomic_read_u64(&e->seen[bit / 64]) >> (bit % 64)) & 1;
	}
	LWLockRelease(cc->lock);
	return all;
}

/* ---- F21: a relation's figures ---- */

/*
 * The key of a scan's quals whatever their order -- each qual normalized as
 * vexec_condcache_key() does, its operators' functions filled in as the
 * executor's are, hashed alone, the hashes summed -- so that planning's
 * restriction clauses and the plan's quals, ordered by their costs, key alike
 */
bool
vexec_condcache_tally_key(List *quals, Index scanrelid, uint64 *tkey)
{
	ListCell   *lc;

	tkey[0] = tkey[1] = 0;
	if (quals == NIL || contain_mutable_functions((Node *) quals) ||
		not_keyable_walker((Node *) quals, &scanrelid))
		return false;
	foreach(lc, quals)
	{
		Node	   *q = normalize_mutator((Node *) lfirst(lc), NULL);
		char	   *text;
		StringInfoData buf;
		const char *p;

		fix_opfuncids(q);
		text = nodeToString(q);
		initStringInfo(&buf);
		for (p = text; *p;)
		{
			if (strncmp(p, ":location ", 10) == 0)
			{
				appendStringInfoString(&buf, ":location -1");
				p += 10;
				if (*p == '-')
					p++;
				while (*p >= '0' && *p <= '9')
					p++;
				continue;
			}
			appendStringInfoChar(&buf, *p++);
		}
		tkey[0] += hash_bytes_extended((const unsigned char *) buf.data, buf.len, UINT64CONST(0x9e3779b97f4a7c15));
		tkey[1] += hash_bytes_extended((const unsigned char *) buf.data, buf.len, UINT64CONST(0x632be59bd9b4e019));
		pfree(buf.data);
		pfree(text);
	}
	if (tkey[0] == 0 && tkey[1] == 0)
		tkey[1] = 1;
	return true;
}

/* The relation's tally of the key, made where make says and there is room; NULL else.  Under the lock */
static CcTally *
tally_of(Oid relid, const uint64 *tkey, bool make)
{
	uint32		h = (uint32) (tkey[0] ^ (tkey[1] >> 7) ^ relid ^ ((uint32) MyDatabaseId << 3));

	for (int k = 0; k < CC_TALLIES; k++)
	{
		CcTally    *t = &cc->tallies[(h + k) % CC_TALLIES];

		if (!t->used)
		{
			if (!make)
				return NULL;
			t->dbid = MyDatabaseId;
			t->relid = relid;
			t->key[0] = tkey[0];
			t->key[1] = tkey[1];
			pg_atomic_write_u64(&t->seen, 0);
			pg_atomic_write_u64(&t->none, 0);
			pg_atomic_write_u64(&t->rows, 0);
			t->used = true;
			return t;
		}
		if (t->dbid == MyDatabaseId && t->relid == relid && t->key[0] == tkey[0] && t->key[1] == tkey[1])
			return t;
	}
	return NULL;
}

/*
 * F21: granules a scan of the quals over relid saw whole first, of them those
 * it found none true of first, and their rows where it counted them (0 none)
 */
void
vexec_condcache_tally_add(Oid relid, const uint64 *tkey, int64 seen, int64 none, int64 rows)
{
	CcTally    *t;

	if (cc == NULL || (seen <= 0 && none <= 0))
		return;
	LWLockAcquire(cc->lock, LW_SHARED);
	t = tally_of(relid, tkey, false);
	if (t == NULL)
	{
		LWLockRelease(cc->lock);
		LWLockAcquire(cc->lock, LW_EXCLUSIVE);
		t = tally_of(relid, tkey, true);
	}
	if (t != NULL)
	{
		if (seen > 0)
			pg_atomic_fetch_add_u64(&t->seen, (uint64) seen);
		if (none > 0)
			pg_atomic_fetch_add_u64(&t->none, (uint64) none);
		if (rows > 0)
			pg_atomic_fetch_add_u64(&t->rows, (uint64) rows);
	}
	LWLockRelease(cc->lock);
}

/* F21: the relation's figures under the key, false where it has none */
bool
vexec_condcache_tally_get(Oid relid, const uint64 *tkey, uint64 *seen, uint64 *none, uint64 *rows)
{
	CcTally    *t;

	*seen = *none = *rows = 0;
	if (cc == NULL || !vexec_enable_condition_cache)
		return false;
	LWLockAcquire(cc->lock, LW_SHARED);
	t = tally_of(relid, tkey, false);
	if (t != NULL)
	{
		*seen = pg_atomic_read_u64(&t->seen);
		*none = pg_atomic_read_u64(&t->none);
		*rows = pg_atomic_read_u64(&t->rows);
	}
	LWLockRelease(cc->lock);
	return t != NULL && *seen > 0;
}

/*
 * vexec.condition_cache(): the entries now, the granules kept and passed over
 * since the server started, and how often the full table was emptied; NULLs
 * where no cache was made (vexec.condition_cache_size 0)
 */
PG_FUNCTION_INFO_V1(vexec_condition_cache);
Datum
vexec_condition_cache(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (cc == NULL)
		nulls[0] = nulls[1] = nulls[2] = nulls[3] = true;
	else
	{
		LWLockAcquire(cc->lock, LW_SHARED);
		values[0] = Int32GetDatum((int32) cc->used);
		LWLockRelease(cc->lock);
		values[1] = Int64GetDatum((int64) pg_atomic_read_u64(&cc->kept));
		values[2] = Int64GetDatum((int64) pg_atomic_read_u64(&cc->passed));
		values[3] = Int64GetDatum((int64) pg_atomic_read_u64(&cc->emptied));
	}
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}
