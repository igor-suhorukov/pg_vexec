/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * inset.c
 *	  An IN list's constants as a set (inset.h).
 *
 * A set's slots are sized when it is made, for the keys it is given at
 * most, so it never grows: at least twice that many and at least 16, a
 * power of two.  An ints or bytes set keeps a key once, however often the
 * list repeats it; a hashed set keeps each, since only its caller can tell
 * two of them equal, and a repeated key costs a slot and nothing else.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/memutils.h"

#include "vexec.h"
#include "expr/inset.h"

/* A set for at most maxkeys keys, empty, in mcxt. */
VexecInSet *
vexec_inset_create(MemoryContext mcxt, VexecInSetKind kind, int maxkeys)
{
	VexecInSet *s = MemoryContextAllocZero(mcxt, sizeof(VexecInSet));
	uint64		nslots = 16;
	uint32		i;

	while (nslots < (uint64) Max(maxkeys, 1) * 2)
		nslots <<= 1;
	s->kind = kind;
	s->maxkeys = maxkeys;
	s->mask = (uint32) (nslots - 1);
	s->mcxt = mcxt;
	s->min = PG_INT64_MAX;
	s->max = PG_INT64_MIN;
	switch (kind)
	{
		case VEXEC_INSET_INTS:
			s->ints = MemoryContextAllocExtended(mcxt, sizeof(int64) * nslots, MCXT_ALLOC_HUGE);
			s->used = MemoryContextAllocExtended(mcxt, nslots, MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
			break;
		case VEXEC_INSET_BYTES:
			s->bytes = MemoryContextAllocExtended(mcxt, sizeof(VexecInSetBytes) * nslots,
												  MCXT_ALLOC_HUGE);
			for (i = 0; i <= s->mask; i++)
				s->bytes[i].len = -1;
			break;
		case VEXEC_INSET_HASHED:
			s->hashed = MemoryContextAllocExtended(mcxt, sizeof(VexecInSetHashed) * nslots,
												   MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
			break;
	}
	return s;
}

void
vexec_inset_add_int(VexecInSet *s, int64 key)
{
	uint32		i;

	Assert(s->kind == VEXEC_INSET_INTS && s->nkeys < s->maxkeys);
	for (i = vexec_inset_mix64((uint64) key) & s->mask; s->used[i]; i = (i + 1) & s->mask)
		if (s->ints[i] == key)
			return;
	s->ints[i] = key;
	s->used[i] = 1;
	s->nkeys++;
	s->min = Min(s->min, key);
	s->max = Max(s->max, key);
}

/* The key's bytes are copied into the set's memory. */
void
vexec_inset_add_bytes(VexecInSet *s, const char *p, int len)
{
	uint32		hash = vexec_hash_bytes(p, len);
	uint32		i;
	char	   *copy;

	Assert(s->kind == VEXEC_INSET_BYTES && s->nkeys < s->maxkeys && len >= 0);
	for (i = vexec_inset_mix32(hash) & s->mask; s->bytes[i].len >= 0; i = (i + 1) & s->mask)
	{
		VexecInSetBytes *b = &s->bytes[i];

		if (b->hash == hash && b->len == len && memcmp(b->bytes, p, len) == 0)
			return;
	}
	copy = MemoryContextAlloc(s->mcxt, Max(len, 1));
	memcpy(copy, p, len);
	s->bytes[i].hash = hash;
	s->bytes[i].len = len;
	s->bytes[i].bytes = copy;
	s->lens |= UINT64CONST(1) << Min(len, 63);
	s->nkeys++;
}

/* The value is kept as it is given: the caller keeps what it points to. */
void
vexec_inset_add_hashed(VexecInSet *s, uint32 hash, Datum value)
{
	uint32		i;

	Assert(s->kind == VEXEC_INSET_HASHED && s->nkeys < s->maxkeys);
	for (i = vexec_inset_mix32(hash) & s->mask; s->hashed[i].used; i = (i + 1) & s->mask)
		;
	s->hashed[i].hash = hash;
	s->hashed[i].used = true;
	s->hashed[i].value = value;
	s->nkeys++;
}

/*
 * The keys given: an ints set whose keys' range spans at most
 * VEXEC_INSET_BITMAP_BITS values gets its bitmap, which its lookups read in
 * place of the slots.
 */
void
vexec_inset_finish(VexecInSet *s)
{
	uint64		span;
	uint32		i;

	if (s->kind != VEXEC_INSET_INTS || s->nkeys == 0)
		return;
	span = (uint64) s->max - (uint64) s->min;
	if (span >= VEXEC_INSET_BITMAP_BITS)
		return;
	s->bits = MemoryContextAllocZero(s->mcxt, sizeof(uint64) * ((span >> 6) + 1));
	for (i = 0; i <= s->mask; i++)
		if (s->used[i])
		{
			uint64		k = (uint64) s->ints[i] - (uint64) s->min;

			s->bits[k >> 6] |= UINT64CONST(1) << (k & 63);
		}
}
