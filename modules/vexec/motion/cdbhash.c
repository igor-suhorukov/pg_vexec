/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * cdbhash.c
 *	  The vector cdbhash: the segment each row of a batch belongs on, as
 *	  gp_core's cdbhash puts a row there (pg_vector_executor.md §3.10, "The
 *	  keys, read from the buffers"; frame.h).
 *
 * cdbhash hashes each key column with its operator class's hash function,
 * called with the default collation; rotates its running hash left one bit
 * before each column and XORs the column's hash in where the value is not
 * NULL; and reduces the result to a segment by jump consistent hashing
 * (the port's pg19/modules/gp_core/gp_hash.c, GpHashSegment()).  Here the
 * same, a batch's rows at a time: each column's hashes for all the rows,
 * then the fold, then the reduction.
 *
 * A column is hashed by a kernel where vexec knows its hash function's
 * arithmetic, reading the column through its layout, in both formats:
 *
 *	read as they are		hashint2, hashint4, hashint8, hashoid, hashenum,
 *							hashchar, hashbool, hashdate, timestamp_hash,
 *							timestamptz_hash and time_hash over integers;
 *							hashfloat4 and hashfloat8, with their -0 and
 *							NaN; hashtext, hashvarlena and hashbytea over a
 *							value's bytes -- a text's hash covers its bytes,
 *							not its header -- hashbpchar over them less its
 *							trailing blanks, hashname over a name's string,
 *							uuid_hash over its 16 bytes
 *	shifted in a register	a date or a timestamp under the Arrow format,
 *							back to PostgreSQL's epoch, its infinities as
 *							they are
 *	reproduced				hash_numeric from a scaled numeric's integer:
 *							its base-10000 digits about the decimal point,
 *							without the zeros before and after them, and
 *							the weight of the first, as hash_numeric hashes
 *							a numeric's digits and weight
 *							(PG19:src/backend/utils/adt/numeric.c:2720-2794)
 *
 * The text kernels hash the bytes as the functions do under a deterministic
 * collation, which the database's default is checked to be.  Any other key
 * goes through its hash function by fmgr, its Datums made for that column
 * alone (vexec_vec_datum()), with the default collation, as gp_core calls
 * it.  A legacy key never comes here: gp_core hashes it, row by row
 * (GpCoreApi.hash_segment).
 *
 * Every kernel is checked against gp_core's own cdbhash in the cluster leg,
 * for every key type of the semantics corpus in both formats (§6.2).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/detoast.h"
#include "catalog/pg_collation.h"
#include "common/hashfn.h"
#include "datatype/timestamp.h"
#include "utils/date.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/pg_locale.h"
#include "utils/uuid.h"
#include "varatt.h"

#include "vexec.h"
#include "batch/batch.h"
#include "motion/frame.h"

/* The kernels, by the arithmetic of the hash functions they stand for. */
typedef enum VexecCdbKernel
{
	CDB_UINT32,					/* hash_uint32 of the value as an int32 */
	CDB_INT8,					/* hashint8's fold of an int64 */
	CDB_FLOAT4,
	CDB_FLOAT8,
	CDB_BYTES,					/* hash_any of a varlena's payload */
	CDB_BPCHAR,					/* and less its trailing blanks */
	CDB_NAME,					/* hash_any of a name's string */
	CDB_UUID,					/* hash_any of 16 bytes */
	CDB_NUMERIC					/* hash_numeric of a scaled numeric */
} VexecCdbKernel;

/* Whether the database's default collation hashes text by its bytes. */
static bool
default_collation_deterministic(void)
{
	return pg_newlocale_from_collation(DEFAULT_COLLATION_OID)->deterministic;
}

/* The kernel for a hash function over a column of a type, or -1. */
static int
kernel_for(Oid hashfunc, const VexecType *type)
{
	switch (hashfunc)
	{
		case F_HASHINT2:
		case F_HASHINT4:
		case F_HASHOID:
		case F_HASHENUM:
		case F_HASHCHAR:
		case F_HASHBOOL:
		case F_HASHDATE:
			if (type->tclass == VEXEC_TC_BOOL ||
				((type->tclass == VEXEC_TC_FIXED || type->tclass == VEXEC_TC_DATE) &&
				 type->typlen >= 1 && type->typlen <= 4))
				return CDB_UINT32;
			return -1;
		case F_HASHINT8:
		case F_TIMESTAMP_HASH:
		case F_TIMESTAMPTZ_HASH:
		case F_TIME_HASH:
			if ((type->tclass == VEXEC_TC_FIXED || type->tclass == VEXEC_TC_TIMESTAMP) &&
				type->typlen == 8)
				return CDB_INT8;
			return -1;
		case F_HASHFLOAT4:
			return type->basetype == FLOAT4OID ? CDB_FLOAT4 : -1;
		case F_HASHFLOAT8:
			return type->basetype == FLOAT8OID ? CDB_FLOAT8 : -1;
		case F_HASHTEXT:
		case F_HASHVARLENA:
		case F_HASHBYTEA:
			if (type->tclass != VEXEC_TC_VARLENA)
				return -1;
			if (hashfunc == F_HASHTEXT && !default_collation_deterministic())
				return -1;
			return CDB_BYTES;
		case F_HASHBPCHAR:
			if (type->tclass != VEXEC_TC_VARLENA || !default_collation_deterministic())
				return -1;
			return CDB_BPCHAR;
		case F_HASHNAME:
			return type->basetype == NAMEOID ? CDB_NAME : -1;
		case F_UUID_HASH:
			return type->basetype == UUIDOID ? CDB_UUID : -1;
		case F_HASH_NUMERIC:
			return type->tclass == VEXEC_TC_NUMERIC ? CDB_NUMERIC : -1;
		default:
			return -1;
	}
}

bool
vexec_cdbhash_has_kernel(Oid hashfunc, const VexecType *type)
{
	return kernel_for(hashfunc, type) >= 0;
}

void
vexec_cdbhash_prepare(VexecCdbKey *key, Oid hashfunc, const VexecType *type)
{
	key->hashfunc = hashfunc;
	key->kernel = kernel_for(hashfunc, type);
	fmgr_info(hashfunc, &key->flinfo);
}

/*
 * Jump consistent hashing (Lamping and Veach, "A Fast, Minimal Memory,
 * Consistent Hash Algorithm", 2014), with the arithmetic cdbhash reduces its
 * hash by, so that a row lands on the segment gp_core's would put it on.
 */
int
vexec_jump_consistent_hash(uint64 key, int32 nsegs)
{
	int64		b = -1;
	int64		j = 0;

	while (j < nsegs)
	{
		b = j;
		key = key * 2862933555777941757ULL + 1;
		j = (b + 1) * ((double) (1LL << 31) / (double) ((key >> 33) + 1));
	}
	return (int) b;
}

/* hashint8's fold of an int64, before hash_uint32. */
static inline uint32
int8_fold(int64 val)
{
	uint32		lohalf = (uint32) val;
	uint32		hihalf = (uint32) (val >> 32);

	return lohalf ^ ((val >= 0) ? hihalf : ~hihalf);
}

/* A fixed-width column's value i as an int64, sign-extended from its width. */
static inline int64
fixed_int(const VexecVec *v, int i)
{
	const char *p = (const char *) v->values + (Size) i * v->shape.stride;

	switch (v->shape.width)
	{
		case 1:
			return (int64) *(const int8 *) p;
		case 2:
			return (int64) *(const int16 *) p;
		case 4:
			return (int64) *(const int32 *) p;
		default:
			return *(const int64 *) p;
	}
}

/* Value i of an int-like column, in PostgreSQL's epoch where it is a date. */
static inline int32
uint32_input(const VexecVec *v, int i)
{
	if (v->shape.layout == VEXEC_BIT_BOOL)
		return vexec_bit((const uint64 *) v->values, i);
	if (v->shape.layout == VEXEC_BYTE_BOOL)
		return ((const uint8 *) v->values)[i];
	if (v->type->tclass == VEXEC_TC_DATE)
	{
		int32		d = ((const int32 *) v->values)[i];

		if (v->shape.arrow_values && !DATE_NOT_FINITE(d))
			d -= (int32) VEXEC_EPOCH_DAYS;
		return d;
	}
	if (v->type->tclass == VEXEC_TC_FIXED && v->type->typlen == 1)
		return (int32) ((const char *) v->values)[i];	/* PG_GETARG_CHAR */
	return (int32) fixed_int(v, i);
}

/* Value i of an int64 column, in PostgreSQL's epoch where it is a timestamp. */
static inline int64
int8_input(const VexecVec *v, int i)
{
	int64		t = ((const int64 *) v->values)[i];

	if (v->type->tclass == VEXEC_TC_TIMESTAMP && v->shape.arrow_values &&
		!TIMESTAMP_NOT_FINITE(t))
		t -= VEXEC_EPOCH_USECS;
	return t;
}

/*
 * A varlena value's payload: its bytes without its header, a compressed or
 * TOAST-pointed one detoasted into the work batch's memory.
 */
static void
payload(VexecBatch *work, const VexecVec *v, int i, const char **p, Size *len)
{
	if (v->shape.layout == VEXEC_DATUM)
	{
		varlena    *vl = (varlena *) DatumGetPointer(((const Datum *) v->values)[i]);

		if (VARATT_IS_EXTERNAL(vl) || VARATT_IS_COMPRESSED(vl))
		{
			MemoryContext old = MemoryContextSwitchTo(work->mcxt);

			vl = detoast_attr(vl);
			MemoryContextSwitchTo(old);
		}
		*p = VARDATA_ANY(vl);
		*len = VARSIZE_ANY_EXHDR(vl);
		return;
	}
	vexec_vec_value_bytes(v, i, p, len);
}

/*
 * hash_numeric of value i of a scaled numeric: its digits in base 10000,
 * the first's weight that of its place about the decimal point, without the
 * zeros before and after them.  The sign is not hashed, as hash_numeric
 * does not hash it; zero is -1.
 */
static uint32
scaled_numeric_hash(const VexecVec *v, int i)
{
	int			scale = v->shape.scale;
	uint128		x;
	int16		digits[16];		/* 38 integer and 38 fraction digits at most */
	int			ndigits = 0;
	int			weight;
	int			first;
	int			last;

	if (v->shape.width == 8)
	{
		int64		s = ((const int64 *) v->values)[i];

		x = s < 0 ? (uint128) (-(int128) s) : (uint128) s;
	}
	else
	{
		int128		s;

		memcpy(&s, (const char *) v->values + (Size) i * 16, sizeof(s));
		x = s < 0 ? (uint128) (-s) : (uint128) s;
	}
	if (x == 0)
		return (uint32) -1;

	/*
	 * The integer part's groups, most significant first, then the
	 * fraction's: 4 decimal digits each, the fraction's last group padded
	 * with zeros to 4.
	 */
	{
		uint128		ipart = x;
		uint128		fpart;
		uint128		p10 = 1;
		int16		igroups[12];
		int			ni = 0;
		int			k;

		for (k = 0; k < scale; k++)
			p10 *= 10;
		ipart = x / p10;
		fpart = x % p10;
		while (ipart > 0)
		{
			igroups[ni++] = (int16) (ipart % 10000);
			ipart /= 10000;
		}
		for (k = ni - 1; k >= 0; k--)
			digits[ndigits++] = igroups[k];
		weight = ni - 1;

		/* the fraction's digits, from the most significant */
		if (scale > 0)
		{
			int			left = scale;

			while (left > 0)
			{
				int			take = Min(left, 4);
				uint128		div = 1;
				int16		group;

				for (k = 0; k < left - take; k++)
					div *= 10;
				group = (int16) ((fpart / div) % 10000);
				for (k = take; k < 4; k++)
					group *= 10;
				fpart %= div;
				digits[ndigits++] = group;
				left -= take;
			}
		}
		if (ni == 0)
			weight = -1;
	}

	/* without the zeros before and after: the weight is the first's */
	first = 0;
	while (first < ndigits && digits[first] == 0)
	{
		first++;
		weight--;
	}
	last = ndigits - 1;
	while (last > first && digits[last] == 0)
		last--;
	return hash_bytes((const unsigned char *) &digits[first],
					  (last - first + 1) * (int) sizeof(int16)) ^ (uint32) weight;
}

/* One row's hash of a key column, by its kernel. */
static inline uint32
kernel_hash(VexecBatch *work, const VexecCdbKey *key, const VexecVec *v, int i)
{
	switch ((VexecCdbKernel) key->kernel)
	{
		case CDB_UINT32:
			return hash_bytes_uint32((uint32) uint32_input(v, i));
		case CDB_INT8:
			return hash_bytes_uint32(int8_fold(int8_input(v, i)));
		case CDB_FLOAT4:
			{
				float4		f = ((const float4 *) v->values)[i];
				float8		f8;

				if (f == (float4) 0)
					return 0;
				f8 = f;
				if (isnan(f8))
					f8 = get_float8_nan();
				return hash_bytes((const unsigned char *) &f8, sizeof(f8));
			}
		case CDB_FLOAT8:
			{
				float8		f = ((const float8 *) v->values)[i];

				if (f == (float8) 0)
					return 0;
				if (isnan(f))
					f = get_float8_nan();
				return hash_bytes((const unsigned char *) &f, sizeof(f));
			}
		case CDB_BYTES:
		case CDB_BPCHAR:
			{
				const char *p;
				Size		len;

				payload(work, v, i, &p, &len);
				if (key->kernel == CDB_BPCHAR)
					while (len > 0 && p[len - 1] == ' ')
						len--;
				return hash_bytes((const unsigned char *) p, (int) len);
			}
		case CDB_NAME:
			{
				const char *p = (const char *) v->values + (Size) i * v->shape.stride;

				return hash_bytes((const unsigned char *) p, (int) strnlen(p, NAMEDATALEN));
			}
		case CDB_UUID:
			return hash_bytes((const unsigned char *) v->values + (Size) i * v->shape.stride,
							  UUID_LEN);
		case CDB_NUMERIC:
			if (v->shape.layout == VEXEC_SCALED)
				return scaled_numeric_hash(v, i);
			break;
	}
	pg_unreachable();
	return 0;
}

/* One row's hash of a key column through its hash function, by fmgr. */
static uint32
fmgr_hash(VexecBatch *work, VexecCdbKey *key, const VexecVec *v, int i)
{
	LOCAL_FCINFO(fcinfo, 1);
	bool		isnull;
	Datum		d = vexec_vec_datum(work, v, i, &isnull);
	uint32		h;

	InitFunctionCallInfoData(*fcinfo, &key->flinfo, 1, DEFAULT_COLLATION_OID, NULL, NULL);
	fcinfo->args[0].value = d;
	fcinfo->args[0].isnull = false;
	h = DatumGetUInt32(FunctionCallInvoke(fcinfo));
	if (fcinfo->isnull)
		elog(ERROR, "function %u returned NULL", key->hashfunc);
	return h;
}

void
vexec_cdbhash(VexecBatch *work, int nkeys, VexecCdbKey *keys, VexecVec *const *cols,
			  const int *rows, int nrows, int nsegs, int *segs)
{
	uint32	   *hash = vexec_batch_alloc0(work, sizeof(uint32) * Max(nrows, 1));
	int			k;
	int			j;

	for (k = 0; k < nkeys; k++)
	{
		const VexecVec *v = cols[k];
		VexecCdbKey *key = &keys[k];
		const VexecVec *values = v->encoding == VEXEC_DICT ? v->dictionary : v;
		bool		by_kernel = key->kernel >= 0 &&
			!(key->kernel == CDB_NUMERIC && values->shape.layout != VEXEC_SCALED);
		uint32	   *dict = NULL;
		uint32		one = 0;

		/* a constant's one value, a dictionary's values, hashed once */
		if (v->encoding == VEXEC_CONST && !vexec_vec_isnull(v, 0))
			one = by_kernel ? kernel_hash(work, key, v, 0) : fmgr_hash(work, key, v, 0);
		else if (v->encoding == VEXEC_DICT)
		{
			int			d;

			dict = vexec_batch_alloc(work, sizeof(uint32) * Max(values->nvalues, 1));
			for (d = 0; d < values->nvalues; d++)
				dict[d] = by_kernel ? kernel_hash(work, key, values, d) :
					fmgr_hash(work, key, values, d);
		}

		for (j = 0; j < nrows; j++)
		{
			int			i = rows[j];

			/* rotate the running hash left one bit at each column */
			hash[j] = (hash[j] << 1) | ((hash[j] & 0x80000000) ? 1 : 0);
			if (vexec_vec_isnull(v, i))
				continue;
			if (v->encoding == VEXEC_CONST)
				hash[j] ^= one;
			else if (v->encoding == VEXEC_DICT)
				hash[j] ^= dict[v->codes[i]];
			else
				hash[j] ^= by_kernel ? kernel_hash(work, key, v, i) :
					fmgr_hash(work, key, v, i);
		}
	}
	for (j = 0; j < nrows; j++)
		segs[j] = vexec_jump_consistent_hash(hash[j], nsegs);
}
