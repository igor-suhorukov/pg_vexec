/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * inset.h
 *	  An IN list's constants as a set, looked up once a row: x = ANY of a
 *	  constant array, x <> ALL of one, and the heap reader's check of
 *	  column = ANY (pg_vector_executor.md §3.7).
 *
 * PostgreSQL decides x = ANY (array) by comparing x with the elements in
 * turn, and, where the array is a constant of MIN_ARRAY_SIZE_FOR_HASHED_SAOP
 * elements or more and the operator -- or, for <> ALL, its negator -- hashes
 * both sides by one function, by one lookup in a hash table of the elements
 * (PG19:src/backend/optimizer/util/clauses.c:2814-2905,
 * convert_saop_to_hashed_saop(); src/backend/executor/execExprInterp.c:4257,
 * ExecEvalHashedScalarArrayOp()).  The planner prices it so: a hash and a
 * comparison a row, where the linear search is half the elements'
 * comparisons (src/backend/optimizer/path/costsize.c:5041-5071).
 *
 * vexec compared x with each element, a kernel's pass over the batch an
 * element, where the planner had priced one lookup: 16,000 elements took
 * 69.3 s on 800,000 rows where PostgreSQL's index took 73 ms, an outside
 * review found (2026-10-09).  The heap reader's check of column = ANY
 * compared each tuple's value with each element too (source/heap.c).  Both
 * look their values up in a set instead, where the elements are as many as
 * PostgreSQL's threshold.
 *
 * A set holds keys of one kind:
 *
 *	ints	int64s: integers of every width, oid, "char", bool, date and
 *			timestamps in PostgreSQL's epoch, and floats' bits with every
 *			NaN one and -0 made 0, so that equal keys are exactly the values
 *			the equality takes as equal;
 *	bytes	byte strings, equal where their bytes are: text and bpchar under a
 *			deterministic collation, which texteq() and bpchareq() compare
 *			by bytes (PG19:src/backend/utils/adt/varlena.c, varchar.c), and
 *			uuid's 16 bytes;
 *	hashed	Datums with the caller's hash, equal by the caller's equality:
 *			numeric's, by hash_numeric() and numeric_eq(), whose display
 *			scales differ where their values do not (1.0 = 1.00).
 *
 * Open addressing with linear probing, the slots a power of two at least
 * twice the keys, so that a lookup that misses -- most rows of a selective
 * IN list -- ends at an empty slot within a few probes; an int key outside
 * the keys' range ends before any, and a byte key of a length no key has
 * before it is hashed.  Ints whose range spans at most
 * VEXEC_INSET_BITMAP_BITS values -- ids, dates, "char" -- are a bitmap over
 * the range besides, a lookup one bit's test.  A set is built once and only
 * read, so a parallel query's workers each build their own from the plan's
 * array.
 *
 * When.  A kernel compares a column with a constant 64 rows to an
 * instruction word (k_compare.c's X7), so comparing int4s with N elements
 * costs about 0.17 ns a row an element on this host, where a hashed lookup
 * costs about 10 ns a row and a bitmap's about 1 (2,000,000 rows, one
 * process, ~/.cache/pg_accel/saop/micro-*.out): a set of ints is taken from
 * 9 elements where it is a bitmap, from 64 where it is hashed.  Comparing
 * text costs about 5 ns a row an element, its lookup about 9: from 9.  The
 * heap reader's check compares a tuple's bytes with a value's only where
 * their lengths agree, so its set of byte keys is taken from 32.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_INSET_H
#define VEXEC_INSET_H

#include <math.h>

#include "exec/keyhash.h"

/* PostgreSQL's MIN_ARRAY_SIZE_FOR_HASHED_SAOP (clauses.c:2814) */
#define VEXEC_INSET_MIN		9
/* a set of ints without a bitmap, and the heap reader's set of byte keys (above) */
#define VEXEC_INSET_MIN_HASHED_INTS	64
#define VEXEC_INSET_MIN_READER_BYTES	32
/* the widest range of ints a bitmap is made over: 2^20 bits, 128 kB */
#define VEXEC_INSET_BITMAP_BITS	(UINT64CONST(1) << 20)

typedef enum VexecInSetKind
{
	VEXEC_INSET_INTS,
	VEXEC_INSET_BYTES,
	VEXEC_INSET_HASHED
} VexecInSetKind;

/* A bytes set's slot */
typedef struct VexecInSetBytes
{
	uint32		hash;
	int32		len;			/* -1: empty */
	const char *bytes;
} VexecInSetBytes;

/* A hashed set's slot */
typedef struct VexecInSetHashed
{
	uint32		hash;
	bool		used;
	Datum		value;
} VexecInSetHashed;

typedef struct VexecInSet
{
	uint8		kind;			/* VexecInSetKind */
	int			nkeys;			/* distinct keys; a hashed set's every key */
	int			maxkeys;
	uint32		mask;			/* the slots less one */
	MemoryContext mcxt;			/* where the set and its keys' bytes live */
	/* ints */
	int64	   *ints;
	uint8	   *used;
	int64		min;			/* the keys' range: PG_INT64_MAX, PG_INT64_MIN */
	int64		max;			/* while there are none */
	uint64	   *bits;			/* bit k: min + k a key; NULL: none made */
	/* bytes */
	VexecInSetBytes *bytes;
	uint64		lens;			/* bit n: a key of n bytes, bit 63 of 63 or more */
	/* hashed */
	VexecInSetHashed *hashed;
} VexecInSet;

extern VexecInSet *vexec_inset_create(MemoryContext mcxt, VexecInSetKind kind, int maxkeys);
extern void vexec_inset_add_int(VexecInSet *s, int64 key);
extern void vexec_inset_add_bytes(VexecInSet *s, const char *p, int len);
extern void vexec_inset_add_hashed(VexecInSet *s, uint32 hash, Datum value);
extern void vexec_inset_finish(VexecInSet *s);

/*
 * A float's key: the double's bits, a float4 widened first, which keeps its
 * value; every NaN one and -0 made 0, since float8_eq() takes NaN as equal
 * to NaN and -0 as equal to 0 (PG19:src/include/utils/float.h), as
 * hashfloat8() hashes them (src/backend/access/hash/hashfunc.c).
 */
static inline int64
vexec_inset_float_key(float8 v)
{
	int64		bits;

	if (isnan(v))
		return INT64CONST(0x7ff8000000000000);
	if (v == 0)
		v = 0.0;
	memcpy(&bits, &v, sizeof(bits));
	return bits;
}

/* bpchar's length without its trailing blanks, as bcTruelen() counts (varchar.c) */
static inline int
vexec_inset_bpchar_len(const char *s, int len)
{
	while (len > 0 && s[len - 1] == ' ')
		len--;
	return len;
}

/* murmur3's finalizers: the slot a key's hash starts at */
static inline uint32
vexec_inset_mix64(uint64 h)
{
	h ^= h >> 33;
	h *= UINT64CONST(0xff51afd7ed558ccd);
	h ^= h >> 33;
	h *= UINT64CONST(0xc4ceb9fe1a85ec53);
	h ^= h >> 33;
	return (uint32) h;
}

static inline uint32
vexec_inset_mix32(uint32 h)
{
	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;
	return h;
}

static inline bool
vexec_inset_has_int(const VexecInSet *s, int64 key)
{
	uint32		i;

	if (key < s->min || key > s->max)
		return false;
	if (s->bits != NULL)
	{
		uint64		k = (uint64) key - (uint64) s->min;

		return (s->bits[k >> 6] >> (k & 63)) & 1;
	}
	for (i = vexec_inset_mix64((uint64) key) & s->mask; s->used[i]; i = (i + 1) & s->mask)
		if (s->ints[i] == key)
			return true;
	return false;
}

/* A byte key's hash: CRC32C, as vexec's hash tables hash bytes (keyhash.h) */
static inline bool
vexec_inset_has_bytes(const VexecInSet *s, const char *p, int len)
{
	uint32		hash;
	uint32		i;

	if (!((s->lens >> Min(len, 63)) & 1))
		return false;
	hash = vexec_hash_bytes(p, len);
	for (i = vexec_inset_mix32(hash) & s->mask; s->bytes[i].len >= 0; i = (i + 1) & s->mask)
	{
		const VexecInSetBytes *b = &s->bytes[i];

		if (b->hash == hash && b->len == len && memcmp(b->bytes, p, len) == 0)
			return true;
	}
	return false;
}

/*
 * Whether a hashed set has a key equal to the caller's value, whose hash is
 * `hash`: eq(arg, key) for each key of that hash, until one is.
 */
static inline bool
vexec_inset_has_hashed(const VexecInSet *s, uint32 hash, bool (*eq) (void *arg, Datum key),
					   void *arg)
{
	uint32		i;

	for (i = vexec_inset_mix32(hash) & s->mask; s->hashed[i].used; i = (i + 1) & s->mask)
		if (s->hashed[i].hash == hash && eq(arg, s->hashed[i].value))
			return true;
	return false;
}

#endif							/* VEXEC_INSET_H */
