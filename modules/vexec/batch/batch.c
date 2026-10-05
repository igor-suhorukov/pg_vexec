/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * batch.c
 *	  Batches, their columns and their memory (pg_vector_executor.md §3.4.1,
 *	  §3.4.3, §3.4.5).
 *
 * A batch owns a memory context, reset when its consumer is done with it,
 * and an arena of chunks for the varlena values it writes.  Its buffers are
 * aligned to 64 bytes, and padded to a whole word of rows, so that loops
 * over 64 rows at a time need no tail (§3.4.1).  Nothing here allocates per
 * row: a column's buffers are allocated once for the batch's rows.
 *
 * A column is flat, constant or dictionary-encoded (§3.4.3).  What reads
 * the encodings it cannot use flattens the column first.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "varatt.h"

#include "vexec.h"
#include "batch/batch.h"

#define ARENA_CHUNK		(64 * 1024)

/*
 * A batch for columns of the given types, in their build shapes.  The
 * header and the types live in parent; everything else in the batch's own
 * context, which a reset empties.
 */
VexecBatch *
vexec_batch_create(MemoryContext parent, int ncols, VexecType *const *types)
{
	VexecBatch *batch;

	batch = MemoryContextAllocZero(parent, sizeof(VexecBatch));
	batch->mcxt = AllocSetContextCreate(parent, "vexec batch", ALLOCSET_DEFAULT_SIZES);
	batch->ncols = ncols;
	batch->types = MemoryContextAlloc(parent, sizeof(VexecType *) * Max(ncols, 1));
	memcpy(batch->types, types, sizeof(VexecType *) * ncols);
	vexec_batch_reset(batch);
	return batch;
}

/* Forget the batch's rows and free their memory, keeping its columns' types. */
void
vexec_batch_reset(VexecBatch *batch)
{
	int			i;

	MemoryContextReset(batch->mcxt);
	batch->nrows = 0;
	batch->selection = NULL;
	batch->filling = false;
	batch->cols = MemoryContextAllocZero(batch->mcxt, sizeof(VexecVec) * Max(batch->ncols, 1));
	for (i = 0; i < batch->ncols; i++)
	{
		batch->cols[i].type = batch->types[i];
		vexec_type_build_shape(batch->types[i], &batch->cols[i].shape);
	}
	memset(&batch->arena, 0, sizeof(VexecArena));
	batch->arena.mcxt = batch->mcxt;
}

void
vexec_batch_free(VexecBatch *batch)
{
	MemoryContextDelete(batch->mcxt);
	pfree(batch->types);
	pfree(batch);
}

void *
vexec_batch_alloc(VexecBatch *batch, Size size)
{
	return MemoryContextAllocAligned(batch->mcxt, Max(size, 1), VEXEC_ALIGN, MCXT_ALLOC_HUGE);
}

void *
vexec_batch_alloc0(VexecBatch *batch, Size size)
{
	return MemoryContextAllocAligned(batch->mcxt, Max(size, 1), VEXEC_ALIGN,
									 MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
}

/*
 * A bitmap of nbits, all set or all clear.  Bits past nbits are clear, as
 * Arrow asks of a validity bitmap's padding (Columnar.rst:328-329).
 */
uint64 *
vexec_bitmap_alloc(VexecBatch *batch, int nbits, bool set)
{
	int			nwords = VEXEC_WORDS(Max(nbits, 1));
	uint64	   *bits = vexec_batch_alloc0(batch, sizeof(uint64) * nwords);

	if (set)
	{
		int			i;

		for (i = 0; i < nbits / 64; i++)
			bits[i] = ~UINT64CONST(0);
		if (nbits % 64)
			bits[nbits / 64] = (UINT64CONST(1) << (nbits % 64)) - 1;
	}
	return bits;
}

/*
 * len bytes of the arena, aligned to align, with the chunk and offset they
 * are at: a view's buffer index and offset.  A value longer than a chunk
 * gets a chunk of its own.
 */
char *
vexec_arena_alloc(VexecArena *arena, Size len, Size align, int *chunkno, int32 *offset)
{
	VexecArenaChunk *c = NULL;
	int64		start = 0;

	if (arena->nchunks > 0)
	{
		c = &arena->chunks[arena->nchunks - 1];
		start = TYPEALIGN(align, c->used);
		if (start + (int64) len > c->size)
			c = NULL;
	}
	if (c == NULL)
	{
		int64		size = Max((int64) len + (int64) align, ARENA_CHUNK);

		if (size > (int64) MaxAllocHugeSize || size > PG_INT32_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("a value of %zu bytes is too long for a batch", len)));
		if (arena->nchunks == arena->maxchunks)
		{
			int			newmax = Max(arena->maxchunks * 2, 8);

			if (arena->chunks == NULL)
				arena->chunks = MemoryContextAlloc(arena->mcxt, sizeof(VexecArenaChunk) * newmax);
			else
				arena->chunks = repalloc(arena->chunks, sizeof(VexecArenaChunk) * newmax);
			arena->maxchunks = newmax;
		}
		c = &arena->chunks[arena->nchunks++];
		c->data = MemoryContextAllocAligned(arena->mcxt, size, VEXEC_ALIGN, MCXT_ALLOC_HUGE);
		c->size = size;
		c->used = 0;
		start = 0;
	}
	c->used = start + len;
	if (chunkno)
		*chunkno = (int) (c - arena->chunks);
	if (offset)
		*offset = (int32) start;
	return c->data + start;
}

/* Whether [p, p + len) lies in one of the arena's chunks, and where. */
bool
vexec_arena_find(const VexecArena *arena, const char *p, Size len, int *chunkno, int32 *offset)
{
	int			i;

	/* the newest chunks are the likeliest */
	for (i = arena->nchunks - 1; i >= 0; i--)
	{
		const VexecArenaChunk *c = &arena->chunks[i];

		if (p >= c->data && p + len <= c->data + c->used)
		{
			*chunkno = i;
			*offset = (int32) (p - c->data);
			return true;
		}
	}
	return false;
}

/*
 * Set a column up empty, in a shape, for nvalues values: its buffers
 * allocated, its validity left NULL until a NULL is written.
 */
void
vexec_vec_init(VexecBatch *batch, VexecVec *v, const VexecShape *shape, int nvalues)
{
	const VexecType *type = v->type;

	memset(v, 0, sizeof(VexecVec));
	v->type = type;
	v->shape = *shape;
	v->encoding = VEXEC_FLAT;
	v->nvalues = nvalues;
	switch (shape->layout)
	{
		case VEXEC_FIXED:
		case VEXEC_SCALED:
			v->values = vexec_batch_alloc0(batch, (Size) shape->stride * Max(nvalues, 1));
			break;
		case VEXEC_BYTE_BOOL:
			v->values = vexec_batch_alloc0(batch, TYPEALIGN(8, Max(nvalues, 1)));
			break;
		case VEXEC_BIT_BOOL:
			v->values = vexec_bitmap_alloc(batch, nvalues, false);
			break;
		case VEXEC_DATUM:
			v->values = vexec_batch_alloc0(batch, sizeof(Datum) * Max(nvalues, 1));
			break;
		case VEXEC_VIEW:
			v->values = vexec_batch_alloc0(batch, sizeof(VexecView) * Max(nvalues, 1));
			break;
		case VEXEC_OFFSETS:
			v->values = vexec_batch_alloc0(batch, sizeof(int32) * (nvalues + 1));
			break;
	}
}

int
vexec_vec_null_count(const VexecBatch *batch, const VexecVec *v)
{
	int			n = v->encoding == VEXEC_CONST ? 1 : v->nvalues;
	int			count = 0;
	int			i;

	if (v->validity == NULL)
		return 0;
	for (i = 0; i < n / 64; i++)
		count += 64 - pg_popcount64(v->validity[i]);
	for (i = (n / 64) * 64; i < n; i++)
		count += !vexec_bit(v->validity, i);
	if (v->encoding == VEXEC_CONST)
		return count ? batch->nrows : 0;
	return count;
}

/*
 * A value's bytes, as the layout holds them: the comparable payload a
 * dictionary is built over.  FIXED and SCALED give the value's own bytes,
 * not its padding; varlena layouts their payload without the header.
 */
void
vexec_vec_value_bytes(const VexecVec *v, int i, const char **p, Size *len)
{
	switch (v->shape.layout)
	{
		case VEXEC_FIXED:
		case VEXEC_SCALED:
			*p = (const char *) v->values + (Size) i * v->shape.stride;
			*len = v->shape.width;
			break;
		case VEXEC_BYTE_BOOL:
			*p = (const char *) v->values + i;
			*len = 1;
			break;
		case VEXEC_BIT_BOOL:
			/* the bit itself: callers compare with vexec_bit() */
			*p = vexec_bit((const uint64 *) v->values, i) ? "\1" : "\0";
			*len = 1;
			break;
		case VEXEC_DATUM:
			{
				Datum		d = ((const Datum *) v->values)[i];

				if (v->type->typlen == -2)
				{
					*p = DatumGetCString(d);
					*len = strlen(*p);
				}
				else
				{
					varlena    *vl = (varlena *) DatumGetPointer(d);

					/* a compressed or external value is compared as stored */
					if (VARATT_IS_EXTERNAL(vl) || VARATT_IS_COMPRESSED(vl))
					{
						*p = (const char *) vl;
						*len = VARSIZE_ANY(vl);
					}
					else
					{
						*p = VARDATA_ANY(vl);
						*len = VARSIZE_ANY_EXHDR(vl);
					}
				}
				break;
			}
		case VEXEC_VIEW:
			{
				const VexecView *view = &((const VexecView *) v->values)[i];

				*len = view->inlined.size;
				if (view->inlined.size <= VEXEC_VIEW_INLINE)
					*p = view->inlined.data;
				else
					*p = v->buffers[view->ref.buffer_index] + view->ref.offset;
				break;
			}
		case VEXEC_OFFSETS:
			{
				const int32 *off = (const int32 *) v->values;

				*p = v->buffers[0] + off[i];
				*len = off[i + 1] - off[i];
				break;
			}
	}
}

/*
 * Make a buffer list for a VIEW column that is to hold views from another
 * VIEW column: the same buffers, shared.
 */
static void
share_buffers(VexecVec *dst, const VexecVec *src)
{
	dst->buffers = src->buffers;
	dst->buffer_sizes = src->buffer_sizes;
	dst->nbuffers = src->nbuffers;
}

/*
 * Copy value i of src into slot j of dst, in the same shape.  Varlena bytes
 * are not copied: a Datum or a view still points where src's did, so dst
 * must share src's buffers (VIEW) and live no longer than they do.  OFFSETS
 * are copied, in order: j must be dst's next slot.
 */
void
vexec_vec_copy_value(VexecBatch *batch, VexecVec *dst, int j, const VexecVec *src, int i)
{
	switch (src->shape.layout)
	{
		case VEXEC_FIXED:
		case VEXEC_SCALED:
			memcpy((char *) dst->values + (Size) j * dst->shape.stride,
				   (const char *) src->values + (Size) i * src->shape.stride,
				   src->shape.width);
			break;
		case VEXEC_BYTE_BOOL:
			((uint8 *) dst->values)[j] = ((const uint8 *) src->values)[i];
			break;
		case VEXEC_BIT_BOOL:
			if (vexec_bit((const uint64 *) src->values, i))
				vexec_bit_set((uint64 *) dst->values, j);
			else
				vexec_bit_clear((uint64 *) dst->values, j);
			break;
		case VEXEC_DATUM:
			((Datum *) dst->values)[j] = ((const Datum *) src->values)[i];
			break;
		case VEXEC_VIEW:
			((VexecView *) dst->values)[j] = ((const VexecView *) src->values)[i];
			if (src->datums)
			{
				if (dst->datums == NULL)
					dst->datums = vexec_batch_alloc0(batch, sizeof(Datum) * Max(dst->nvalues, 1));
				dst->datums[j] = src->datums[i];
			}
			break;
		case VEXEC_OFFSETS:
			{
				const int32 *so = (const int32 *) src->values;
				int32	   *dofs = (int32 *) dst->values;
				int32		len = so[i + 1] - so[i];
				int64		need = (int64) dofs[j] + len;

				if (need > dst->buffer_sizes[0])
				{
					int64		size = Max(need, dst->buffer_sizes[0] * 2);
					char	   *data;

					if (size > PG_INT32_MAX)
						ereport(ERROR,
								(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								 errmsg("a batch's offsets column cannot hold more than %d bytes",
										PG_INT32_MAX)));
					data = vexec_batch_alloc(batch, size);
					memcpy(data, dst->buffers[0], dofs[j]);
					dst->buffers[0] = data;
					dst->buffer_sizes[0] = size;
				}
				memcpy(dst->buffers[0] + dofs[j], src->buffers[0] + so[i], len);
				dofs[j + 1] = dofs[j] + len;
				if (src->datums)
				{
					if (dst->datums == NULL)
						dst->datums = vexec_batch_alloc0(batch, sizeof(Datum) * Max(dst->nvalues, 1));
					dst->datums[j] = src->datums[i];
				}
				break;
			}
	}
}

/* An empty OFFSETS column's data buffer, ready for vexec_vec_copy_value. */
static void
offsets_buffer_init(VexecBatch *batch, VexecVec *v, int64 size)
{
	v->nbuffers = 1;
	v->buffers = vexec_batch_alloc0(batch, sizeof(char *));
	v->buffer_sizes = vexec_batch_alloc0(batch, sizeof(int64));
	v->buffers[0] = vexec_batch_alloc(batch, Max(size, 64));
	v->buffer_sizes[0] = Max(size, 64);
}

/*
 * Turn a constant or dictionary-encoded column into a flat one, in the
 * shape of its values.
 */
void
vexec_vec_flatten(VexecBatch *batch, VexecVec *v)
{
	VexecVec	flat;
	const VexecVec *values;
	int			i;

	if (v->encoding == VEXEC_FLAT)
		return;
	vexec_batch_column_changed(batch, v);

	values = v->encoding == VEXEC_DICT ? v->dictionary : v;
	flat.type = v->type;
	vexec_vec_init(batch, &flat, &values->shape, batch->nrows);
	if (values->shape.layout == VEXEC_VIEW)
		share_buffers(&flat, values);
	else if (values->shape.layout == VEXEC_OFFSETS)
		offsets_buffer_init(batch, &flat, 64);

	if (v->validity != NULL)
		flat.validity = vexec_bitmap_alloc(batch, batch->nrows, false);
	for (i = 0; i < batch->nrows; i++)
	{
		int			from;

		if (vexec_vec_isnull(v, i))
		{
			/* a NULL row still has a slot: offsets stay monotonic */
			if (flat.shape.layout == VEXEC_OFFSETS)
				((int32 *) flat.values)[i + 1] = ((int32 *) flat.values)[i];
			continue;
		}
		if (flat.validity)
			vexec_bit_set(flat.validity, i);
		from = v->encoding == VEXEC_DICT ? v->codes[i] : 0;
		vexec_vec_copy_value(batch, &flat, i, values, from);
	}
	*v = flat;
}

/*
 * Whether value i of a DATUM column is compared by its stored image -- a
 * compressed value, or a TOAST pointer -- rather than by its payload, as
 * vexec_vec_value_bytes() gives it.  Two values compare equal only when
 * they are compared alike: an image is never equal to a payload whose bytes
 * happen to match it.
 */
static bool
value_is_image(const VexecVec *v, int i)
{
	varlena    *vl;

	if (v->shape.layout != VEXEC_DATUM || v->type->typlen == -2)
		return false;
	vl = (varlena *) DatumGetPointer(((const Datum *) v->values)[i]);
	return VARATT_IS_EXTERNAL(vl) || VARATT_IS_COMPRESSED(vl);
}

/* Two values of one column's shape, compared as the layout holds them. */
static bool
values_equal(const VexecVec *a, int i, const VexecVec *b, int j)
{
	const char *pa,
			   *pb;
	Size		la,
				lb;

	if (a->shape.layout == VEXEC_BIT_BOOL)
		return vexec_bit((const uint64 *) a->values, i) ==
			vexec_bit((const uint64 *) b->values, j);
	if (value_is_image(a, i) != value_is_image(b, j))
		return false;
	vexec_vec_value_bytes(a, i, &pa, &la);
	vexec_vec_value_bytes(b, j, &pb, &lb);
	return la == lb && memcmp(pa, pb, la) == 0;
}

static uint32
value_hash(const VexecVec *v, int i)
{
	const char *p;
	Size		len;

	if (v->shape.layout == VEXEC_BIT_BOOL)
		return vexec_bit((const uint64 *) v->values, i);
	vexec_vec_value_bytes(v, i, &p, &len);
	return hash_bytes((const unsigned char *) p, (int) len) ^ (value_is_image(v, i) ? 0x5bd1e995 : 0);
}

/*
 * Dictionary-encode a flat column: a dictionary of its distinct non-NULL
 * values, in the order of their first rows, and a code a row.  As PAX's
 * dictionary encoding keeps a column, and as Arrow's dictionary layout is
 * (§3.4.3).  False, and the column unchanged, when it has no non-NULL
 * value.
 */
bool
vexec_vec_encode_dict(VexecBatch *batch, VexecVec *v)
{
	int			nslots = 1;
	int		   *slots;
	int32	   *codes;
	VexecVec   *dict;
	int			ndict = 0;
	int			i;

	if (v->encoding != VEXEC_FLAT)
		vexec_vec_flatten(batch, v);
	if (vexec_vec_null_count(batch, v) == batch->nrows)
		return false;
	vexec_batch_column_changed(batch, v);

	while (nslots < batch->nrows * 2)
		nslots <<= 1;
	slots = palloc(sizeof(int) * nslots);
	for (i = 0; i < nslots; i++)
		slots[i] = -1;
	codes = vexec_batch_alloc0(batch, sizeof(int32) * Max(batch->nrows, 1));

	/* first the distinct values' rows */
	for (i = 0; i < batch->nrows; i++)
	{
		uint32		h;
		int			s;

		if (vexec_vec_isnull(v, i))
			continue;
		h = value_hash(v, i);
		for (s = h & (nslots - 1);; s = (s + 1) & (nslots - 1))
		{
			if (slots[s] < 0)
			{
				slots[s] = i;
				codes[i] = ndict++;
				break;
			}
			if (values_equal(v, slots[s], v, i))
			{
				codes[i] = codes[slots[s]];
				break;
			}
		}
	}

	/* then the dictionary, one value per code, in code order */
	dict = vexec_batch_alloc0(batch, sizeof(VexecVec));
	dict->type = v->type;
	vexec_vec_init(batch, dict, &v->shape, ndict);
	if (v->shape.layout == VEXEC_VIEW)
		share_buffers(dict, v);
	else if (v->shape.layout == VEXEC_OFFSETS)
		offsets_buffer_init(batch, dict, 64);
	{
		int			next = 0;

		for (i = 0; i < batch->nrows; i++)
		{
			if (vexec_vec_isnull(v, i) || codes[i] != next)
				continue;
			vexec_vec_copy_value(batch, dict, next, v, i);
			next++;
		}
		Assert(next == ndict);
	}
	pfree(slots);

	v->encoding = VEXEC_DICT;
	v->codes = codes;
	v->dictionary = dict;
	/* validity, nvalues stay the rows' */
	v->values = NULL;
	v->datums = NULL;
	v->buffers = NULL;
	v->buffer_sizes = NULL;
	v->nbuffers = 0;
	return true;
}

/*
 * Make a flat column constant, when every row holds one value, or every row
 * is NULL.  False, and the column unchanged, otherwise.
 */
bool
vexec_vec_encode_const(VexecBatch *batch, VexecVec *v)
{
	VexecVec	c;
	int			first = -1;
	bool		allnull = true;
	int			i;

	if (v->encoding != VEXEC_FLAT)
		vexec_vec_flatten(batch, v);
	if (batch->nrows == 0)
		return false;
	for (i = 0; i < batch->nrows; i++)
	{
		if (vexec_vec_isnull(v, i))
		{
			if (!allnull)
				return false;
			continue;
		}
		if (allnull && i > 0)
			return false;		/* NULLs before a value */
		allnull = false;
		if (first < 0)
			first = i;
		else if (!values_equal(v, first, v, i))
			return false;
	}

	vexec_batch_column_changed(batch, v);
	c.type = v->type;
	vexec_vec_init(batch, &c, &v->shape, 1);
	c.encoding = VEXEC_CONST;
	if (allnull)
	{
		c.validity = vexec_bitmap_alloc(batch, 1, false);
		if (c.shape.layout == VEXEC_OFFSETS)
			offsets_buffer_init(batch, &c, 64);
	}
	else
	{
		if (v->shape.layout == VEXEC_VIEW)
			share_buffers(&c, v);
		else if (v->shape.layout == VEXEC_OFFSETS)
			offsets_buffer_init(batch, &c, 64);
		vexec_vec_copy_value(batch, &c, 0, v, first);
	}
	*v = c;
	return true;
}

/* How many rows the selection keeps. */
int
vexec_batch_selected(const VexecBatch *batch)
{
	int			n = 0;
	int			i;

	if (batch->selection == NULL)
		return batch->nrows;
	for (i = 0; i < VEXEC_WORDS(batch->nrows); i++)
		n += pg_popcount64(batch->selection[i]);
	return n;
}

/*
 * Keep only the selected rows, in their order.  Below a threshold of
 * selected rows a batch is compacted, so that kernels do not walk rows they
 * would skip (§3.4.1).  A dictionary column keeps its dictionary and
 * compacts its codes; a constant one stays as it is.
 */
void
vexec_batch_compact(VexecBatch *batch)
{
	int			nkeep;
	int		   *rows;
	int			c;
	int			i;

	if (batch->selection == NULL)
		return;
	batch->filling = false;
	nkeep = 0;
	rows = palloc(sizeof(int) * Max(batch->nrows, 1));
	for (i = 0; i < batch->nrows; i++)
		if (vexec_bit(batch->selection, i))
			rows[nkeep++] = i;

	for (c = 0; c < batch->ncols; c++)
	{
		VexecVec   *v = &batch->cols[c];
		VexecVec	out;

		if (v->encoding == VEXEC_CONST)
			continue;
		if (v->encoding == VEXEC_DICT)
		{
			out = *v;
			out.nvalues = nkeep;
			out.validity = NULL;
			if (v->validity)
			{
				out.validity = vexec_bitmap_alloc(batch, nkeep, false);
				for (i = 0; i < nkeep; i++)
					if (vexec_bit(v->validity, rows[i]))
						vexec_bit_set(out.validity, i);
			}
			out.codes = vexec_batch_alloc0(batch, sizeof(int32) * Max(nkeep, 1));
			for (i = 0; i < nkeep; i++)
				out.codes[i] = v->codes[rows[i]];
			*v = out;
			continue;
		}
		out.type = v->type;
		vexec_vec_init(batch, &out, &v->shape, nkeep);
		if (v->validity)
		{
			out.validity = vexec_bitmap_alloc(batch, nkeep, false);
			for (i = 0; i < nkeep; i++)
				if (vexec_bit(v->validity, rows[i]))
					vexec_bit_set(out.validity, i);
		}
		if (v->shape.layout == VEXEC_VIEW)
			share_buffers(&out, v);
		else if (v->shape.layout == VEXEC_OFFSETS)
			offsets_buffer_init(batch, &out, v->buffer_sizes ? v->buffer_sizes[0] : 64);
		for (i = 0; i < nkeep; i++)
		{
			if (vexec_vec_isnull(v, rows[i]) && v->shape.layout == VEXEC_OFFSETS)
			{
				((int32 *) out.values)[i + 1] = ((int32 *) out.values)[i];
				continue;
			}
			vexec_vec_copy_value(batch, &out, i, v, rows[i]);
		}
		*v = out;
	}
	pfree(rows);
	batch->nrows = nkeep;
	batch->selection = NULL;
}
