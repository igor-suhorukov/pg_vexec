/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * batch.h
 *	  The logical batch, its layouts, and the two in-memory formats
 *	  (pg_vector_executor.md §3.4).
 *
 * A batch is a header and a column per output attribute, up to
 * VEXEC_BATCH_ROWS rows.  How a column holds its values is its layout, a
 * VexecShape here: a VexecLayout (vexec_source.h) and the few parameters
 * that go with it.  A format gives each type its shape:
 *
 *	the PostgreSQL format	values as PostgreSQL holds them, in what a Datum
 *							holds or points at: by-value types at their
 *							width, PostgreSQL's epochs, a byte per bool,
 *							fixed-length by-reference values at the stride
 *							PostgreSQL's arrays give them, varlena values as
 *							Datums pointing at headered values;
 *	the Arrow format		Arrow's columnar format with its standard types:
 *							bit-packed booleans, the Unix epoch,
 *							month_day_nano intervals, string views.
 *
 * vexec.batch_format chooses the format, and four settings change it one
 * structure at a time (§3.4.4); vexec_type_shape() applies them.  The
 * validity and selection bitmaps, the row count, and fixed-width numbers,
 * scaled numerics and uuids are the same in both.
 *
 * Every conversion between two shapes of a type is in convert.c; reading a
 * column's value as a Datum, filling a batch from rows and handing rows out
 * through a slot are in rows.c; Arrow's C Data Interface is in export.c.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_BATCH_H
#define VEXEC_BATCH_H

#include "access/tupdesc.h"
#include "executor/tuptable.h"
#include "utils/memutils.h"

#include "vexec_source.h"

#include "vexec.h"

#ifndef HAVE_INT128
#error "vexec needs a 128-bit integer type for its scaled numerics"
#endif

/*
 * Rows a batch holds at most.  A compile-time constant, so that loops may
 * specialise (§3.4.1).  A multiple of 64: the bitmaps are uint64 words.
 */
#define VEXEC_BATCH_ROWS	1024
#define VEXEC_WORDS(nrows)	(((nrows) + 63) / 64)

/* The alignment of a batch's buffers (§3.4.1). */
#define VEXEC_ALIGN			64

/*
 * The epoch shifts between PostgreSQL's 2000-01-01 and Arrow's 1970-01-01
 * (PG19:src/include/datatype/timestamp.h:234-235).
 */
#define VEXEC_EPOCH_DAYS	INT64CONST(10957)
#define VEXEC_EPOCH_USECS	INT64CONST(946684800000000)

/*
 * What kind of type a column holds, as far as its layouts go.  Each class
 * has the shapes vexec_type_shapes() lists.
 */
typedef enum VexecTypeClass
{
	VEXEC_TC_BOOL,				/* bool */
	VEXEC_TC_FIXED,				/* any other by-value type: int2, int4, int8,
								 * float4, float8, oid, money, time, "char",
								 * enums ... */
	VEXEC_TC_DATE,				/* date */
	VEXEC_TC_TIMESTAMP,			/* timestamp, timestamptz */
	VEXEC_TC_INTERVAL,			/* interval */
	VEXEC_TC_BYREF,				/* fixed-length by-reference: uuid, name,
								 * tid, point, macaddr, timetz ... */
	VEXEC_TC_NUMERIC,			/* numeric */
	VEXEC_TC_VARLENA,			/* text, bytea, jsonb, arrays ... */
	VEXEC_TC_CSTRING			/* typlen -2 */
} VexecTypeClass;

/* A column's type, as the batch layer needs it; made once per column. */
typedef struct VexecType
{
	Oid			typid;			/* the column's own type, a domain's too */
	int32		typmod;
	Oid			collation;
	Oid			basetype;		/* what the layouts follow */
	int32		basetypmod;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	uint8		alignby;		/* typalign in bytes */
	uint8		tclass;			/* VexecTypeClass */
	int32		pg_stride;		/* BYREF: TYPEALIGN(typalign, typlen) */
	/* numeric */
	uint8		scaled_width;	/* 0: no scaled layout; 8 or 16 */
	int16		scale;			/* the stored scale: max(typmod's scale, 0) */
	int16		digits;			/* the typmod's precision, plus a negative
								 * scale's size */
	/* varlena */
	bool		short_ok;		/* its functions take a short header as it is */
	bool		utf8;			/* text in a UTF8 database: Arrow's utf8 */
	char	   *name;			/* typname */
	char	   *nspname;		/* its schema's */
} VexecType;

/* A layout and its parameters. */
typedef struct VexecShape
{
	uint8		layout;			/* VexecLayout */
	bool		arrow_values;	/* DATE, TIMESTAMP, INTERVAL: Arrow's */
	int16		scale;			/* SCALED */
	int32		width;			/* FIXED: value bytes, typlen's up to 32767;
								 * SCALED: 8 or 16 */
	int32		stride;			/* FIXED: bytes from one value to the next */
} VexecShape;

/*
 * The format and the per-structure settings in effect, as a node reads them
 * when it starts (§3.4.4), or as a test gives them.
 */
typedef struct VexecLayoutConfig
{
	uint8		format;			/* VexecBatchFormat */
	uint8		varlena;		/* VexecVarlenaSetting */
	uint8		boolean;		/* VexecBoolSetting */
	uint8		temporal;		/* VexecTemporalSetting */
	uint8		numeric;		/* VexecNumericSetting */
} VexecLayoutConfig;

/*
 * Varlena bytes a batch writes: chunks that never move, each a data buffer
 * a view may point into.
 */
typedef struct VexecArenaChunk
{
	char	   *data;
	int64		size;
	int64		used;
} VexecArenaChunk;

typedef struct VexecArena
{
	MemoryContext mcxt;
	VexecArenaChunk *chunks;
	int			nchunks;
	int			maxchunks;
} VexecArena;

/*
 * A column of a batch.  The source contract's VexecColumn, with what vexec
 * needs to write into one.
 */
typedef struct VexecVec
{
	const VexecType *type;
	VexecShape	shape;
	uint8		encoding;		/* VexecEncoding */
	int			nvalues;		/* FLAT: the batch's rows; CONST: 1; DICT: the
								 * batch's rows of codes */
	uint64	   *validity;		/* NULL: no NULLs; else 1 = valid */
	void	   *values;			/* FIXED, BOOL, SCALED: the values; DATUM:
								 * Datums; VIEW: views; OFFSETS: int32
								 * offsets, nvalues + 1 of them */
	char	  **buffers;		/* VIEW: data buffers; OFFSETS: one */
	int64	   *buffer_sizes;
	int			nbuffers;
	Datum	   *datums;			/* VIEW, OFFSETS: the Datums kept beside, or
								 * NULL */
	int32	   *codes;			/* DICT */
	struct VexecVec *dictionary;	/* DICT: FLAT values, never NULL */
} VexecVec;

typedef struct VexecBatch
{
	int			nrows;
	int			ncols;
	uint64	   *selection;		/* NULL: every row; else 1 = selected */
	VexecVec   *cols;
	VexecType **types;			/* the columns' types, which outlive resets */
	bool		filling;		/* rows may be added: every column is flat,
								 * in its build shape, with room for
								 * VEXEC_BATCH_ROWS */
	MemoryContext mcxt;			/* the batch's memory, reset with it */
	VexecArena	arena;
} VexecBatch;

/* An Arrow view (Columnar.rst:492-524): 16 bytes. */
typedef union VexecView
{
	struct
	{
		int32		size;
		char		data[12];
	}			inlined;
	struct
	{
		int32		size;
		char		prefix[4];
		int32		buffer_index;
		int32		offset;
	}			ref;
	int64		align_dummy;
} VexecView;

#define VEXEC_VIEW_INLINE	12

/* bits */
static inline bool
vexec_bit(const uint64 *bits, int i)
{
	return (bits[i >> 6] >> (i & 63)) & 1;
}

static inline void
vexec_bit_set(uint64 *bits, int i)
{
	bits[i >> 6] |= UINT64CONST(1) << (i & 63);
}

static inline void
vexec_bit_clear(uint64 *bits, int i)
{
	bits[i >> 6] &= ~(UINT64CONST(1) << (i & 63));
}

/*
 * A column of the batch is no longer as rows are added to it: converted,
 * encoded or compacted.  A copy of one, outside the batch's array, leaves
 * the batch as it is.
 */
static inline void
vexec_batch_column_changed(VexecBatch *batch, const VexecVec *v)
{
	if (v >= batch->cols && v < batch->cols + batch->ncols)
		batch->filling = false;
}

/* Whether row i of a column is NULL, whatever its encoding. */
static inline bool
vexec_vec_isnull(const VexecVec *v, int row)
{
	if (v->encoding == VEXEC_CONST)
		row = 0;
	return v->validity != NULL && !vexec_bit(v->validity, row);
}

/* types.c */
extern VEXEC_API VexecType *vexec_type_make(Oid typid, int32 typmod, Oid collation);
extern VEXEC_API VexecLayoutConfig vexec_layout_config(void);
extern VEXEC_API void vexec_type_shape(const VexecType *type,
											 const VexecLayoutConfig *cfg,
											 VexecShape *shape);
extern VEXEC_API void vexec_type_build_shape(const VexecType *type, VexecShape *shape);
extern VEXEC_API int vexec_type_shapes(const VexecType *type, VexecShape *shapes, int max);
extern VEXEC_API bool vexec_shape_equal(const VexecShape *a, const VexecShape *b);
extern VEXEC_API void vexec_shape_normalize(VexecShape *shape);
extern VEXEC_API char *vexec_shape_name(const VexecType *type, const VexecShape *shape);
extern VEXEC_API const char *vexec_layout_name(int layout);
extern VEXEC_API const char *vexec_type_class_name(int tclass);

/* batch.c */
extern VEXEC_API VexecBatch *vexec_batch_create(MemoryContext parent, int ncols,
													 VexecType *const *types);
extern VEXEC_API void vexec_batch_reset(VexecBatch *batch);
extern VEXEC_API void vexec_batch_free(VexecBatch *batch);
extern VEXEC_API void *vexec_batch_alloc(VexecBatch *batch, Size size);
extern VEXEC_API void *vexec_batch_alloc0(VexecBatch *batch, Size size);
extern VEXEC_API uint64 *vexec_bitmap_alloc(VexecBatch *batch, int nbits, bool set);
extern VEXEC_API char *vexec_arena_alloc(VexecArena *arena, Size len, Size align,
											  int *chunkno, int32 *offset);
extern VEXEC_API bool vexec_arena_find(const VexecArena *arena, const char *p, Size len,
											int *chunkno, int32 *offset);
extern VEXEC_API void vexec_vec_init(VexecBatch *batch, VexecVec *v,
										  const VexecShape *shape, int nvalues);
extern VEXEC_API int vexec_vec_null_count(const VexecBatch *batch, const VexecVec *v);
extern VEXEC_API void vexec_vec_flatten(VexecBatch *batch, VexecVec *v);
extern VEXEC_API bool vexec_vec_encode_dict(VexecBatch *batch, VexecVec *v);
extern VEXEC_API bool vexec_vec_encode_const(VexecBatch *batch, VexecVec *v);
extern VEXEC_API int vexec_batch_selected(const VexecBatch *batch);
extern VEXEC_API void vexec_batch_compact(VexecBatch *batch);
extern VEXEC_API void vexec_vec_value_bytes(const VexecVec *v, int i,
												 const char **p, Size *len);
extern VEXEC_API void vexec_vec_copy_value(VexecBatch *batch, VexecVec *dst, int j,
												const VexecVec *src, int i);

/* convert.c */
extern VEXEC_API bool vexec_vec_convert(VexecBatch *batch, VexecVec *v,
											 const VexecShape *to);
extern VEXEC_API bool vexec_batch_apply_config(VexecBatch *batch,
													const VexecLayoutConfig *cfg);

/* A numeric's parts (numeric.c's NumericVar), or which special value it is. */
typedef enum VexecNumericSpecial
{
	VEXEC_NUMERIC_FINITE,
	VEXEC_NUMERIC_NAN,
	VEXEC_NUMERIC_PINF,
	VEXEC_NUMERIC_NINF
} VexecNumericSpecial;

typedef struct VexecNumericParts
{
	int			special;		/* VexecNumericSpecial */
	int			sign;			/* NUMERIC_POS 0x0000, NUMERIC_NEG 0x4000 */
	int			weight;
	int			dscale;
	int			ndigits;
	const char *digits;			/* int16s in base 10000, unaligned */
} VexecNumericParts;

/* numeric.c */
extern VEXEC_API bool vexec_numeric_parts(Datum num, VexecNumericParts *parts);
extern VEXEC_API bool vexec_numeric_to_scaled(Datum num, int scale, int digits,
												   int width, int128 *result);
extern VEXEC_API Datum vexec_scaled_to_numeric(VexecBatch *batch, int128 value, int scale);
extern VEXEC_API int vexec_numeric_dscale(Datum num);

/* rows.c */
extern VEXEC_API void vexec_batch_begin_rows(VexecBatch *batch);
extern VEXEC_API void vexec_batch_add_values(VexecBatch *batch, const Datum *values,
												  const bool *isnull);
extern VEXEC_API void vexec_vec_set_null(VexecBatch *batch, VexecVec *v, int row);
extern VEXEC_API void vexec_batch_add_slot(VexecBatch *batch, TupleTableSlot *slot,
												const AttrNumber *attnums);
extern VEXEC_API Datum vexec_vec_datum(VexecBatch *batch, const VexecVec *v, int row,
											bool *isnull);
extern VEXEC_API void vexec_batch_store_row(VexecBatch *batch, int row,
												 TupleTableSlot *slot);
extern VEXEC_API Datum vexec_varlena_copy(VexecBatch *batch, const VexecType *type,
											   Datum value);

#endif							/* VEXEC_BATCH_H */
