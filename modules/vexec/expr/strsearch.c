/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * strsearch.c
 *	  A literal's search in text, for LIKE '%...%' (X4 of
 *	  pg_vector_executor.md §3.18).
 *
 * H7's substring kind (k_text.c) and the heap reader's checks
 * (source/heap.c) search a value's bytes for a literal of the pattern.
 * glibc's memmem() takes about 20 ns over a URL of 75 bytes; comparing a
 * literal's first and last bytes at 32 positions at once, and its middle
 * only where both match -- W. Mula's search, which Velox has as simdStrstr
 * and calls nowhere (§3.18) -- takes about 3, with the same answers.  Where
 * the CPU has AVX2, which PostgreSQL's own test finds at run time
 * (port/pg_cpu.h), as its checksum chooses its AVX2 variant (storage/page/
 * checksum.c), that search runs; elsewhere memmem().  A literal of one byte
 * is memchr()'s.
 *
 * A batch's rows are searched by one function chosen once, its loop over
 * the rows compiled with the search inlined.  Where the values lie end to
 * end -- the offsets layout, Arrow's, which PAX's and ao_column's readers
 * hand -- the column's bytes are searched at once and each hit mapped to
 * its row by the offsets, as ClickHouse's LIKE does
 * (CH:src/Functions/MatchImpl.h:195-233): about half the time a value at a
 * time takes (§3.19).  A hit that runs across two values is none, and the
 * search goes on from the next value's start.
 *
 * Every variant finds the same byte sequences, so the answer is LIKE's
 * wherever H7 takes a pattern as a substring (k_text.c).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <string.h>

#ifdef USE_AVX2_WITH_RUNTIME_CHECK
#include <immintrin.h>
#include "port/pg_cpu.h"
#endif

#include "vexec.h"
#include "batch/batch.h"
#include "expr/expr.h"
#include "expr/kernel.h"

typedef void (*ContainsRowsFn) (const VexecVec *v, const uint64 *active, int nrows,
								const char *lit, int litlen, uint64 *matches);
typedef const char *(*FindFn) (const char *s, size_t len, const char *lit, size_t litlen);

/* ---- the scalar search ---- */

static inline const char *
find_scalar(const char *s, size_t len, const char *lit, size_t litlen)
{
	if (litlen == 1)
		return memchr(s, lit[0], len);
	return memmem(s, len, lit, litlen);
}

/*
 * The rows of a batch whose value holds the literal: a value at a time, or,
 * values end to end, the column's bytes at once (above).  `find` inlined.
 */
static inline void
contains_rows_with(const VexecVec *v, const uint64 *active, int nrows, const char *lit,
				   int litlen, uint64 *matches, FindFn find)
{
	if (v->encoding == VEXEC_FLAT && v->shape.layout == VEXEC_OFFSETS && nrows > 0)
	{
		const int32 *off = (const int32 *) v->values;
		const char *buf = v->buffers[0];
		int			first = vexec_bits_next(active, nrows, 0);
		int			last = first;
		int			row;
		const char *pos;
		const char *end;

		if (first < 0)
			return;
		for (int r = first; r >= 0; r = vexec_bits_next(active, nrows, r + 1))
			last = r;
		row = first;
		pos = buf + off[first];
		end = buf + off[last + 1];
		while (pos < end && (pos = find(pos, end - pos, lit, litlen)) != NULL)
		{
			/* the value the hit starts in; a hit past its end is none */
			while (buf + off[row + 1] <= pos)
				row++;
			if (pos + litlen <= buf + off[row + 1])
				vexec_bit_set(matches, row);
			pos = buf + off[row + 1];
			row++;
		}
		return;
	}
	VEXEC_FOREACH_ROW(active, nrows, i)
	{
		const char *p;
		Size		len;

		vexec_vec_value_bytes(v, vexec_arg_row(v, i), &p, &len);
		if (len >= (Size) litlen && find(p, len, lit, litlen) != NULL)
			vexec_bit_set(matches, i);
	}
}

static void
contains_rows_scalar(const VexecVec *v, const uint64 *active, int nrows, const char *lit,
					 int litlen, uint64 *matches)
{
	contains_rows_with(v, active, nrows, lit, litlen, matches, find_scalar);
}

/* ---- AVX2 ---- */

#ifdef USE_AVX2_WITH_RUNTIME_CHECK

/*
 * The literal's first and last bytes compared at 32 positions at once, its
 * middle only where both match; the tail, past the last 32 positions whose
 * last byte lies in the value, a position at a time.  litlen >= 2.
 */
pg_attribute_target("avx2")
static inline const char *
find_avx2(const char *s, size_t len, const char *lit, size_t litlen)
{
	__m256i		first;
	__m256i		last;
	size_t		i = 0;

	if (litlen == 1)
		return memchr(s, lit[0], len);
	if (len < litlen)
		return NULL;
	first = _mm256_set1_epi8(lit[0]);
	last = _mm256_set1_epi8(lit[litlen - 1]);
	for (; i + litlen - 1 + 32 <= len; i += 32)
	{
		__m256i		bf = _mm256_loadu_si256((const __m256i *) (s + i));
		__m256i		bl = _mm256_loadu_si256((const __m256i *) (s + i + litlen - 1));
		uint32		mask = (uint32) _mm256_movemask_epi8(_mm256_and_si256(_mm256_cmpeq_epi8(first, bf),
																		  _mm256_cmpeq_epi8(last, bl)));

		while (mask != 0)
		{
			int			bit = pg_rightmost_one_pos32(mask);

			if (memcmp(s + i + bit + 1, lit + 1, litlen - 2) == 0)
				return s + i + bit;
			mask &= mask - 1;
		}
	}
	for (; i + litlen <= len; i++)
		if (s[i] == lit[0] && s[i + litlen - 1] == lit[litlen - 1] &&
			memcmp(s + i + 1, lit + 1, litlen - 2) == 0)
			return s + i;
	return NULL;
}

pg_attribute_target("avx2")
static void
contains_rows_avx2(const VexecVec *v, const uint64 *active, int nrows, const char *lit,
				   int litlen, uint64 *matches)
{
	contains_rows_with(v, active, nrows, lit, litlen, matches, find_avx2);
}

pg_attribute_target("avx2")
static const char *
find_bytes_avx2(const char *s, size_t len, const char *lit, size_t litlen)
{
	return find_avx2(s, len, lit, litlen);
}
#endif							/* USE_AVX2_WITH_RUNTIME_CHECK */

static const char *
find_bytes_scalar(const char *s, size_t len, const char *lit, size_t litlen)
{
	return find_scalar(s, len, lit, litlen);
}

/* ---- choosing, once ---- */

static void contains_rows_choose(const VexecVec *v, const uint64 *active, int nrows,
								 const char *lit, int litlen, uint64 *matches);
static const char *find_bytes_choose(const char *s, size_t len, const char *lit,
									 size_t litlen);

static ContainsRowsFn contains_rows_impl = contains_rows_choose;
static FindFn find_bytes_impl = find_bytes_choose;

static void
choose(void)
{
	contains_rows_impl = contains_rows_scalar;
	find_bytes_impl = find_bytes_scalar;
#ifdef USE_AVX2_WITH_RUNTIME_CHECK
	if (x86_feature_available(PG_AVX2))
	{
		contains_rows_impl = contains_rows_avx2;
		find_bytes_impl = find_bytes_avx2;
	}
#endif
}

static void
contains_rows_choose(const VexecVec *v, const uint64 *active, int nrows, const char *lit,
					 int litlen, uint64 *matches)
{
	choose();
	contains_rows_impl(v, active, nrows, lit, litlen, matches);
}

static const char *
find_bytes_choose(const char *s, size_t len, const char *lit, size_t litlen)
{
	choose();
	return find_bytes_impl(s, len, lit, litlen);
}

/*
 * The rows of `active` whose value holds the literal, a bit set in
 * `matches` for each; the others' bits are left as they are.  The values
 * read as bytes: plain, or a view's, or an offset's (k_text.c's plain()).
 */
void
vexec_contains_rows(const VexecVec *v, const uint64 *active, int nrows, const char *lit,
					int litlen, uint64 *matches)
{
	contains_rows_impl(v, active, nrows, lit, litlen, matches);
}

/* Where the literal first is in the bytes, or NULL; litlen >= 1. */
const char *
vexec_find_bytes(const char *s, size_t len, const char *lit, size_t litlen)
{
	return find_bytes_impl(s, len, lit, litlen);
}

/* Which search runs, for EXPLAIN and the tests. */
const char *
vexec_find_bytes_kind(void)
{
	if (find_bytes_impl == find_bytes_choose)
		choose();
#ifdef USE_AVX2_WITH_RUNTIME_CHECK
	if (find_bytes_impl == find_bytes_avx2)
		return "avx2";
#endif
	return "memmem";
}
