/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * frame.c
 *	  A batch's rows as an Arrow IPC frame, and a frame back into a batch
 *	  (pg_vector_executor.md §3.10, "The frame"; frame.h).
 *
 * The sender's side.  A batch's columns are taken in the shapes they have,
 * a constant or a dictionary flattened first, and each shape set is a
 * schema of the writer's, made once: its Schema message, as a frame, and
 * the hash of that message's bytes, which names it in each batch's frame.
 * A batch's rows go out through V7_0's writer (ipc/write.c), whose message
 * is pieces pointing into the arrays: the buffers of the batch itself where
 * the rows are all of it, in order, else the rows' values gathered into
 * memory of the frame's own -- a Redistribute's partition of a batch, which
 * copies the rows each target gets (§3.10, "What remains").  The pieces are
 * then copied, once, into the frame's bytea.
 *
 * The receiver's side.  A frame comes from another process, so V7_0's reader
 * checks every offset and buffer of its message (ipc/read.c), and this file
 * checks the rest: a schema's fields must be the receiver's columns, of its
 * types, each in one of the shapes vexec has for its type
 * (vexec_type_shapes()); a datum column's every value a whole varlena,
 * neither a TOAST pointer nor longer than its slot, at its alignment, or a
 * C string with its NUL in its slot.  A batch's columns then point into the
 * frame: its fixed-width buffers, its views, and Datums made for a datum
 * column's values.  A frame whose bytes do not lie 8-byte aligned -- a
 * bytea a receive function made, behind its 4-byte header -- is copied
 * once, so that its buffers do (§3.10: "Where a transport's own framing
 * shifts it, the receiver copies the frame once").
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "common/hashfn.h"
#include "utils/expandeddatum.h"
#include "varatt.h"

#include "vexec.h"
#include "batch/arrow_abi.h"
#include "batch/batch.h"
#include "ipc/ipc.h"
#include "motion/frame.h"

/* A frame's header: vexec's own, before the IPC message. */
typedef struct FrameHeader
{
	uint32		magic;
	uint32		reserved;		/* 0 */
	uint64		schema;			/* the hash of its schema's message */
} FrameHeader;

StaticAssertDecl(sizeof(FrameHeader) == VEXEC_FRAME_HEADER && VEXEC_FRAME_HEADER % 8 == 0,
				 "a frame's header keeps its message 8-byte aligned");

/* A shape set's schema, as the writer or the reader keeps it. */
typedef struct FrameSchema
{
	VexecShape *shapes;			/* a column's each */
	uint64		hash;
	struct ArrowSchema arrow;
	bytea	   *frame;			/* the writer's: the schema message's frame */
	char	   *metadata;		/* the reader's: the message, to compare */
	size_t		metadata_len;
} FrameSchema;

struct VexecFrameWriter
{
	MemoryContext mcxt;			/* the writer's, its schemas' */
	int			ncols;
	VexecType **types;
	VexecIpcWriter *ipc;
	List	   *schemas;		/* FrameSchema, the newest last */
	VexecBatch *scratch;		/* a batch's flattened copies and gathers */

	/* the batch being sent (vexec_frame_begin()) */
	VexecBatch *batch;
	VexecVec  **cols;			/* its columns, flat */
	VexecShape *shapes;
	FrameSchema *schema;
};

struct VexecFrameReader
{
	MemoryContext mcxt;
	int			ncols;
	VexecType **types;
	List	   *schemas;		/* FrameSchema */
};

static void
release_noop_array(struct ArrowArray *a)
{
	a->release = NULL;
}

static void
release_noop_schema(struct ArrowSchema *s)
{
	s->release = NULL;
}

/* ---------------------------------------------------------------------
 * A shape in a field's metadata
 * ---------------------------------------------------------------------
 */

/* The C Data Interface's format string for a shape (frame.h's table). */
static char *
shape_format(const VexecShape *s)
{
	switch (s->layout)
	{
		case VEXEC_FIXED:
		case VEXEC_SCALED:
			return psprintf("w:%d", s->stride);
		case VEXEC_BYTE_BOOL:
			return pstrdup("w:1");
		case VEXEC_BIT_BOOL:
			return pstrdup("b");
		case VEXEC_VIEW:
			return pstrdup("vz");
		default:
			return pstrdup("z");	/* datum, offsets */
	}
}

/* Metadata in the interface's encoding: int32 n, then each key and value. */
static char *
encode_metadata(int n, const char *const *keys, const char *const *values)
{
	StringInfoData buf;
	int32		count = n;
	int			i;

	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, &count, sizeof(int32));
	for (i = 0; i < n; i++)
	{
		int32		kl = (int32) strlen(keys[i]);
		int32		vl = (int32) strlen(values[i]);

		appendBinaryStringInfo(&buf, &kl, sizeof(int32));
		appendBinaryStringInfo(&buf, keys[i], kl);
		appendBinaryStringInfo(&buf, &vl, sizeof(int32));
		appendBinaryStringInfo(&buf, values[i], vl);
	}
	return buf.data;
}

/* A value of a field's metadata, by its key, or NULL. */
static char *
metadata_value(const char *metadata, const char *key)
{
	const char *p = metadata;
	int32		n;
	int			i;

	if (metadata == NULL)
		return NULL;
	memcpy(&n, p, sizeof(int32));
	p += sizeof(int32);
	for (i = 0; i < n; i++)
	{
		int32		kl,
					vl;
		const char *k,
				   *v;

		memcpy(&kl, p, sizeof(int32));
		k = p + sizeof(int32);
		memcpy(&vl, k + kl, sizeof(int32));
		v = k + kl + sizeof(int32);
		p = v + vl;
		if (kl == (int32) strlen(key) && memcmp(k, key, kl) == 0)
			return pnstrdup(v, vl);
	}
	return NULL;
}

/* A shape set's record batch schema: a struct of the columns' fields. */
static void
build_schema(MemoryContext mcxt, int ncols, VexecType *const *types, const VexecShape *shapes,
			 struct ArrowSchema *out)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	int			i;

	memset(out, 0, sizeof(*out));
	out->format = "+s";
	out->name = "";
	out->n_children = ncols;
	out->children = palloc0(sizeof(struct ArrowSchema *) * Max(ncols, 1));
	out->release = release_noop_schema;
	for (i = 0; i < ncols; i++)
	{
		struct ArrowSchema *c = palloc0(sizeof(struct ArrowSchema));
		const VexecShape *s = &shapes[i];
		const char *keys[2] = {"vexec:shape", "vexec:type"};
		const char *values[2];

		values[0] = psprintf("%d,%d,%d,%d,%d", s->layout, s->arrow_values ? 1 : 0,
							 s->width, s->stride, s->scale);
		values[1] = psprintf("%u", types[i]->typid);
		c->format = shape_format(s);
		c->name = psprintf("c%d", i + 1);
		c->metadata = encode_metadata(2, keys, values);
		c->flags = ARROW_FLAG_NULLABLE;
		c->release = release_noop_schema;
		out->children[i] = c;
	}
	MemoryContextSwitchTo(old);
}

/* ---------------------------------------------------------------------
 * The sender's side
 * ---------------------------------------------------------------------
 */

VexecFrameWriter *
vexec_frame_writer_create(MemoryContext parent, int ncols, VexecType *const *types)
{
	MemoryContext mcxt = AllocSetContextCreate(parent, "vexec frame writer",
											   ALLOCSET_DEFAULT_SIZES);
	VexecFrameWriter *w = MemoryContextAllocZero(mcxt, sizeof(VexecFrameWriter));

	w->mcxt = mcxt;
	w->ncols = ncols;
	w->types = MemoryContextAlloc(mcxt, sizeof(VexecType *) * Max(ncols, 1));
	memcpy(w->types, types, sizeof(VexecType *) * ncols);
	w->ipc = vexec_ipc_writer_create(mcxt);
	w->scratch = vexec_batch_create(mcxt, 0, NULL);
	w->shapes = MemoryContextAllocZero(mcxt, sizeof(VexecShape) * Max(ncols, 1));
	w->cols = MemoryContextAllocZero(mcxt, sizeof(VexecVec *) * Max(ncols, 1));
	return w;
}

/* The message's bytes behind a header, as a bytea: the one copy of them. */
static bytea *
message_frame(const VexecIpcMessage *msg, uint64 schema)
{
	Size		len = VEXEC_FRAME_HEADER + 8 + msg->metadata_len + (Size) msg->body_len;
	bytea	   *frame;
	char	   *p;
	FrameHeader hdr;
	int			i;

	if (len > MaxAllocSize - VARHDRSZ)
		return NULL;
	frame = palloc(VARHDRSZ + len);
	SET_VARSIZE(frame, VARHDRSZ + len);
	p = VARDATA(frame);
	hdr.magic = VEXEC_FRAME_MAGIC;
	hdr.reserved = 0;
	hdr.schema = schema;
	memcpy(p, &hdr, sizeof(hdr));
	p += sizeof(hdr);
	memcpy(p, msg->prefix, 8);
	p += 8;
	memcpy(p, msg->metadata, msg->metadata_len);
	p += msg->metadata_len;
	for (i = 0; i < msg->npieces; i++)
	{
		memcpy(p, msg->pieces[i].data, msg->pieces[i].len);
		p += msg->pieces[i].len;
	}
	Assert(p == VARDATA(frame) + len);
	return frame;
}

/* The writer's schema of the shapes in w->shapes: found, or made. */
static FrameSchema *
writer_schema(VexecFrameWriter *w)
{
	FrameSchema *fs;
	const VexecIpcMessage *msg;
	MemoryContext old;
	int			i;

	foreach_ptr(FrameSchema, s, w->schemas)
	{
		for (i = 0; i < w->ncols; i++)
			if (!vexec_shape_equal(&s->shapes[i], &w->shapes[i]))
				break;
		if (i == w->ncols)
			return s;
	}

	old = MemoryContextSwitchTo(w->mcxt);
	fs = palloc0(sizeof(FrameSchema));
	fs->shapes = palloc(sizeof(VexecShape) * Max(w->ncols, 1));
	memcpy(fs->shapes, w->shapes, sizeof(VexecShape) * w->ncols);
	build_schema(w->mcxt, w->ncols, w->types, fs->shapes, &fs->arrow);
	msg = vexec_ipc_write_schema(w->ipc, &fs->arrow);
	fs->hash = hash_bytes_extended((const unsigned char *) msg->metadata,
								   (int) msg->metadata_len, 0);
	fs->frame = message_frame(msg, fs->hash);
	w->schemas = lappend(w->schemas, fs);
	MemoryContextSwitchTo(old);
	return fs;
}

/* One column's buffers for the frame, and its array. */
typedef struct ColOut
{
	struct ArrowArray arr;
	const void *bufs[4];
	int64		sizes[1];
} ColOut;

/* The rows' validity: the column's own bitmap, or the rows' bits gathered. */
static const void *
out_validity(VexecBatch *scratch, const VexecVec *v, const int *rows, int n, bool all,
			 int64 *nulls)
{
	uint64	   *bits;
	int64		count = 0;
	int			i;

	*nulls = 0;
	if (v->validity == NULL)
		return NULL;
	if (all)
	{
		for (i = 0; i < n; i++)
			count += !vexec_bit(v->validity, i);
		*nulls = count;
		return count ? v->validity : NULL;
	}
	bits = vexec_bitmap_alloc(scratch, n, false);
	for (i = 0; i < n; i++)
	{
		if (vexec_bit(v->validity, rows[i]))
			vexec_bit_set(bits, i);
		else
			count++;
	}
	*nulls = count;
	return count ? bits : NULL;
}

/* Fixed-width values, stride bytes each: as they are, or the rows' gathered. */
static const void *
out_fixed(VexecBatch *scratch, const VexecVec *v, int stride, const int *rows, int n, bool all)
{
	const char *in = v->values;
	char	   *out;
	int			i;

	if (all)
		return in;
	out = vexec_batch_alloc(scratch, (Size) stride * Max(n, 1));
	switch (stride)
	{
		case 1:
			for (i = 0; i < n; i++)
				out[i] = in[rows[i]];
			break;
		case 4:
			for (i = 0; i < n; i++)
				((uint32 *) out)[i] = ((const uint32 *) in)[rows[i]];
			break;
		case 8:
			for (i = 0; i < n; i++)
				((uint64 *) out)[i] = ((const uint64 *) in)[rows[i]];
			break;
		default:
			for (i = 0; i < n; i++)
				memcpy(out + (Size) i * stride, in + (Size) rows[i] * stride, stride);
			break;
	}
	return out;
}

/* Bit-packed values: as they are, or the rows' bits gathered. */
static const void *
out_bits(VexecBatch *scratch, const VexecVec *v, const int *rows, int n, bool all)
{
	uint64	   *bits;
	int			i;

	if (all)
		return v->values;
	bits = vexec_bitmap_alloc(scratch, n, false);
	for (i = 0; i < n; i++)
		if (vexec_bit((const uint64 *) v->values, rows[i]))
			vexec_bit_set(bits, i);
	return bits;
}

/*
 * A datum column's values, whole, end to end: each a varlena with its
 * header -- a 4-byte one starting at the type's alignment, the bytes
 * before it the slot of the value before -- or a C string with its NUL.
 * A TOAST pointer's value is fetched, an expanded object flattened: the
 * receiver can read neither, as gp_core's tuples have them (gp_motion.c,
 * motion_tuple()).
 */
static void
out_datums(VexecBatch *scratch, const VexecVec *v, const int *rows, int n, bool all,
		   const void **offsets_out, const void **data_out)
{
	const VexecType *type = v->type;
	const Datum *datums = v->values;
	Size		align = Max((Size) type->alignby, sizeof(int32));
	const char **ptrs = vexec_batch_alloc(scratch, sizeof(char *) * Max(n, 1));
	Size	   *lens = vexec_batch_alloc(scratch, sizeof(Size) * Max(n, 1));
	int32	   *offsets = vexec_batch_alloc(scratch, sizeof(int32) * (n + 1));
	MemoryContext old = MemoryContextSwitchTo(scratch->mcxt);
	Size		bound = 0;
	Size		pos = 0;
	char	   *data;
	int			i;

	/* each value's bytes, and room for them all */
	for (i = 0; i < n; i++)
	{
		int			row = all ? i : rows[i];
		const char *p;

		ptrs[i] = NULL;
		lens[i] = 0;
		if (v->validity != NULL && !vexec_bit(v->validity, row))
			continue;
		p = DatumGetPointer(datums[row]);
		if (type->typlen == -2)
		{
			lens[i] = strlen(p) + 1;
			ptrs[i] = p;
			bound += lens[i];
			continue;
		}
		if (VARATT_IS_EXTERNAL_EXPANDED((varlena *) p))
		{
			ExpandedObjectHeader *eoh = DatumGetEOHP(datums[row]);
			Size		len = EOH_get_flat_size(eoh);
			char	   *flat = palloc(len);

			EOH_flatten_into(eoh, flat, len);
			p = flat;
		}
		else if (VARATT_IS_EXTERNAL((varlena *) p))
			p = (const char *) detoast_external_attr((varlena *) p);
		ptrs[i] = p;
		lens[i] = VARSIZE_ANY(p);
		bound += lens[i] + (VARATT_IS_SHORT(p) ? 0 : align - 1);
	}
	MemoryContextSwitchTo(old);

	data = vexec_batch_alloc(scratch, Max(bound, 1));
	for (i = 0; i < n; i++)
	{
		if (ptrs[i] != NULL && type->typlen != -2 && !VARATT_IS_SHORT(ptrs[i]))
			pos = TYPEALIGN(align, pos);
		if (pos > PG_INT32_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("a frame's column cannot hold more than %d bytes", PG_INT32_MAX)));
		offsets[i] = (int32) pos;
		if (ptrs[i] != NULL)
		{
			memcpy(data + pos, ptrs[i], lens[i]);
			pos += lens[i];
		}
	}
	offsets[n] = (int32) pos;
	*offsets_out = offsets;
	*data_out = data;
}

/*
 * A view column's views, with the values a view does not hold inline
 * copied into one buffer, in the rows' order: the views of a batch point
 * into all its arena's chunks, and a partition's into a part of them.
 */
static void
out_views(VexecBatch *scratch, const VexecVec *v, const int *rows, int n, bool all,
		  const void **views_out, const void **data_out, int64 *data_len)
{
	const VexecView *views = v->values;
	VexecView  *out = vexec_batch_alloc(scratch, sizeof(VexecView) * Max(n, 1));
	Size		total = 0;
	Size		pos = 0;
	char	   *data;
	int			i;

	for (i = 0; i < n; i++)
	{
		int			row = all ? i : rows[i];

		if ((v->validity == NULL || vexec_bit(v->validity, row)) &&
			views[row].inlined.size > VEXEC_VIEW_INLINE)
			total += views[row].inlined.size;
	}
	if (total > PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a frame's column cannot hold more than %d bytes", PG_INT32_MAX)));
	data = total > 0 ? vexec_batch_alloc(scratch, total) : NULL;
	for (i = 0; i < n; i++)
	{
		int			row = all ? i : rows[i];
		const VexecView *in = &views[row];

		if (v->validity != NULL && !vexec_bit(v->validity, row))
		{
			memset(&out[i], 0, sizeof(VexecView));
			continue;
		}
		out[i] = *in;
		if (in->inlined.size > VEXEC_VIEW_INLINE)
		{
			memcpy(data + pos, v->buffers[in->ref.buffer_index] + in->ref.offset,
				   in->inlined.size);
			out[i].ref.buffer_index = 0;
			out[i].ref.offset = (int32) pos;
			pos += in->inlined.size;
		}
	}
	*views_out = out;
	*data_out = data;
	*data_len = (int64) total;
}

/* An offsets column's rows: as they are, or rebuilt from 0 and gathered. */
static void
out_offsets(VexecBatch *scratch, const VexecVec *v, const int *rows, int n, bool all,
			const void **offsets_out, const void **data_out)
{
	const int32 *in = v->values;
	int32	   *offsets;
	char	   *data;
	int64		total = 0;
	int			i;

	if (all && in[0] == 0)
	{
		*offsets_out = in;
		*data_out = v->buffers[0];
		return;
	}
	for (i = 0; i < n; i++)
	{
		int			row = all ? i : rows[i];

		total += in[row + 1] - in[row];
	}
	if (total > PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("a frame's column cannot hold more than %d bytes", PG_INT32_MAX)));
	offsets = vexec_batch_alloc(scratch, sizeof(int32) * (n + 1));
	data = vexec_batch_alloc(scratch, Max(total, 1));
	offsets[0] = 0;
	for (i = 0; i < n; i++)
	{
		int			row = all ? i : rows[i];
		int32		len = in[row + 1] - in[row];

		memcpy(data + offsets[i], v->buffers[0] + in[row], len);
		offsets[i + 1] = offsets[i] + len;
	}
	*offsets_out = offsets;
	*data_out = data;
}

/* One column of the rows, as the frame's array. */
static void
out_column(VexecBatch *scratch, const VexecVec *v, const int *rows, int n, bool all,
		   ColOut *c)
{
	int64		nulls;

	memset(c, 0, sizeof(*c));
	c->arr.length = n;
	c->arr.offset = 0;
	c->arr.buffers = c->bufs;
	c->arr.release = release_noop_array;
	c->bufs[0] = out_validity(scratch, v, rows, n, all, &nulls);
	c->arr.null_count = nulls;
	switch (v->shape.layout)
	{
		case VEXEC_FIXED:
		case VEXEC_SCALED:
			c->arr.n_buffers = 2;
			c->bufs[1] = out_fixed(scratch, v, v->shape.stride, rows, n, all);
			break;
		case VEXEC_BYTE_BOOL:
			c->arr.n_buffers = 2;
			c->bufs[1] = out_fixed(scratch, v, 1, rows, n, all);
			break;
		case VEXEC_BIT_BOOL:
			c->arr.n_buffers = 2;
			c->bufs[1] = out_bits(scratch, v, rows, n, all);
			break;
		case VEXEC_DATUM:
			c->arr.n_buffers = 3;
			out_datums(scratch, v, rows, n, all, &c->bufs[1], &c->bufs[2]);
			break;
		case VEXEC_OFFSETS:
			c->arr.n_buffers = 3;
			out_offsets(scratch, v, rows, n, all, &c->bufs[1], &c->bufs[2]);
			break;
		case VEXEC_VIEW:
			{
				const void *data;

				out_views(scratch, v, rows, n, all, &c->bufs[1], &data, &c->sizes[0]);
				if (c->sizes[0] > 0)
				{
					c->arr.n_buffers = 4;
					c->bufs[2] = data;
					c->bufs[3] = c->sizes;
				}
				else
				{
					c->arr.n_buffers = 3;
					c->bufs[2] = c->sizes;
				}
				break;
			}
		default:
			elog(ERROR, "vexec frame: a column of layout %d", v->shape.layout);
	}
}

void
vexec_frame_begin(VexecFrameWriter *w, VexecBatch *batch, uint64 *schema, bytea **schema_frame)
{
	VexecBatch *scratch = w->scratch;
	int			i;

	if (batch->ncols != w->ncols)
		elog(ERROR, "vexec frame: a batch of %d columns for a writer of %d",
			 batch->ncols, w->ncols);

	/* each column flat, in the shape it has */
	vexec_batch_reset(scratch);
	scratch->nrows = batch->nrows;
	w->batch = batch;
	for (i = 0; i < w->ncols; i++)
	{
		w->cols[i] = &batch->cols[i];
		if (w->cols[i]->encoding != VEXEC_FLAT)
		{
			VexecVec   *copy = vexec_batch_alloc(scratch, sizeof(VexecVec));

			*copy = *w->cols[i];
			vexec_vec_flatten(scratch, copy);
			w->cols[i] = copy;
		}
		w->shapes[i] = w->cols[i]->shape;
		vexec_shape_normalize(&w->shapes[i]);
	}
	w->schema = writer_schema(w);
	*schema = w->schema->hash;
	*schema_frame = w->schema->frame;
}

bytea *
vexec_frame_rows(VexecFrameWriter *w, const int *rows, int nrows)
{
	ColOut	   *outs = palloc(sizeof(ColOut) * Max(w->ncols, 1));
	struct ArrowArray **children = palloc(sizeof(struct ArrowArray *) * Max(w->ncols, 1));
	struct ArrowArray array;
	const void *nobuf[1] = {NULL};
	const VexecIpcMessage *msg;
	bool		all;
	int			i;

	/* the rows: all of the batch's, in order, or some of them */
	all = nrows == w->batch->nrows;
	for (i = 0; all && i < nrows; i++)
		all = rows[i] == i;

	for (i = 0; i < w->ncols; i++)
	{
		out_column(w->scratch, w->cols[i], rows, nrows, all, &outs[i]);
		children[i] = &outs[i].arr;
	}
	memset(&array, 0, sizeof(array));
	array.length = nrows;
	array.null_count = 0;
	array.n_buffers = 1;
	array.buffers = nobuf;
	array.n_children = w->ncols;
	array.children = children;
	array.release = release_noop_array;
	msg = vexec_ipc_write_batch(w->ipc, &w->schema->arrow, &array);
	return message_frame(msg, w->schema->hash);
}

/* ---------------------------------------------------------------------
 * The receiver's side
 * ---------------------------------------------------------------------
 */

VexecFrameReader *
vexec_frame_reader_create(MemoryContext parent, int ncols, VexecType *const *types)
{
	MemoryContext mcxt = AllocSetContextCreate(parent, "vexec frame reader",
											   ALLOCSET_SMALL_SIZES);
	VexecFrameReader *r = MemoryContextAllocZero(mcxt, sizeof(VexecFrameReader));

	r->mcxt = mcxt;
	r->ncols = ncols;
	r->types = MemoryContextAlloc(mcxt, sizeof(VexecType *) * Max(ncols, 1));
	memcpy(r->types, types, sizeof(VexecType *) * ncols);
	return r;
}

#define frame_malformed(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION), \
			 errmsg("a vector Motion's frame is malformed"), \
			 errdetail_internal(__VA_ARGS__)))

/*
 * A schema's field, checked against the reader's column: its type, and a
 * shape vexec has for the type, written in the format that shape has.
 */
static void
read_field_shape(VexecFrameReader *r, int i, const struct ArrowSchema *f, VexecShape *shape)
{
	const VexecType *type = r->types[i];
	char	   *text = metadata_value(f->metadata, "vexec:shape");
	char	   *typid = metadata_value(f->metadata, "vexec:type");
	VexecShape	shapes[8];
	int			nshapes;
	int			layout,
				arrow,
				width,
				stride,
				scale;
	int			k;

	if (text == NULL || typid == NULL)
		frame_malformed("Field %d has no shape or type.", i + 1);
	if (strtoul(typid, NULL, 10) != type->typid)
		frame_malformed("Field %d is of type %s, where the Motion's column is of type %u.",
								 i + 1, typid, type->typid);
	if (sscanf(text, "%d,%d,%d,%d,%d", &layout, &arrow, &width, &stride, &scale) != 5 ||
		layout < 0 || layout > 255 || scale < PG_INT16_MIN || scale > PG_INT16_MAX)
		frame_malformed("Field %d's shape \"%s\" is not a shape.", i + 1, text);
	memset(shape, 0, sizeof(*shape));
	shape->layout = (uint8) layout;
	shape->arrow_values = arrow != 0;
	shape->width = width;
	shape->stride = stride;
	shape->scale = (int16) scale;

	/*
	 * One of the type's shapes, in its format -- or, for a numeric, any
	 * scaled shape: a kernel's result is scaled at the scale its arithmetic
	 * gives, whatever the column's typmod (extract(), the numeric operators),
	 * in the scaled layout's bounds of 8 or 16 bytes and a scale of 0 to 38
	 * (batch/numeric.c).
	 */
	nshapes = vexec_type_shapes(type, shapes, lengthof(shapes));
	for (k = 0; k < Min(nshapes, (int) lengthof(shapes)); k++)
		if (vexec_shape_equal(&shapes[k], shape))
			break;
	if (k == Min(nshapes, (int) lengthof(shapes)) &&
		!(type->tclass == VEXEC_TC_NUMERIC && shape->layout == VEXEC_SCALED &&
		  (shape->width == 8 || shape->width == 16) && shape->stride == shape->width &&
		  shape->scale >= 0 && shape->scale <= 38))
		frame_malformed("Field %d's shape \"%s\" is not one of its type's.", i + 1, text);
	vexec_shape_normalize(shape);
	if (strcmp(f->format, shape_format(shape)) != 0)
		frame_malformed("Field %d of shape \"%s\" is of format \"%s\".",
								 i + 1, text, f->format);
}

/* A Schema message's frame: checked, and kept where it is new. */
static void
read_schema(VexecFrameReader *r, uint64 hash, const char *metadata, size_t len)
{
	FrameSchema *fs;
	MemoryContext old;
	int			i;

	foreach_ptr(FrameSchema, s, r->schemas)
	{
		if (s->hash != hash)
			continue;
		if (s->metadata_len != len || memcmp(s->metadata, metadata, len) != 0)
			frame_malformed("Two schemas of one hash.");
		return;
	}
	old = MemoryContextSwitchTo(r->mcxt);
	fs = palloc0(sizeof(FrameSchema));
	fs->hash = hash;
	fs->metadata = palloc(Max(len, 1));
	memcpy(fs->metadata, metadata, len);
	fs->metadata_len = len;
	if (hash != hash_bytes_extended((const unsigned char *) metadata, (int) len, 0))
		frame_malformed("A schema whose hash is not its own.");
	vexec_ipc_read_schema(fs->metadata, len, &fs->arrow);
	if (fs->arrow.n_children != r->ncols)
		frame_malformed("A schema of %lld fields, for a Motion of %d columns.",
								 (long long) fs->arrow.n_children, r->ncols);
	fs->shapes = palloc(sizeof(VexecShape) * Max(r->ncols, 1));
	for (i = 0; i < r->ncols; i++)
		read_field_shape(r, i, fs->arrow.children[i], &fs->shapes[i]);
	r->schemas = lappend(r->schemas, fs);
	MemoryContextSwitchTo(old);
}

/*
 * A datum column's Datums, into the frame's data: each value a whole
 * varlena within its slot -- never a TOAST pointer, a 4-byte header at its
 * alignment -- or a C string whose NUL is within its slot.
 */
static Datum *
in_datums(VexecBatch *into, const VexecType *type, const uint64 *validity, int n,
		  const int32 *offsets, const char *data, int col)
{
	Datum	   *datums = vexec_batch_alloc0(into, sizeof(Datum) * Max(n, 1));
	Size		align = Max((Size) type->alignby, sizeof(int32));
	int			i;

	for (i = 0; i < n; i++)
	{
		const char *p = data + offsets[i];
		int64		slot = (int64) offsets[i + 1] - offsets[i];
		int64		len;

		if (validity != NULL && !vexec_bit(validity, i))
			continue;
		if (type->typlen == -2)
		{
			if (slot <= 0 || memchr(p, '\0', slot) == NULL)
				frame_malformed("Column %d, row %d: a string without its NUL.", col + 1, i);
		}
		else
		{
			if (slot < 1)
				frame_malformed("Column %d, row %d: an empty value.", col + 1, i);
			if (VARATT_IS_1B_E(p))
				frame_malformed("Column %d, row %d: a TOAST pointer.", col + 1, i);
			if (VARATT_IS_1B(p))
				len = VARSIZE_1B(p);
			else
			{
				if (slot < VARHDRSZ || (uintptr_t) p % align != 0)
					frame_malformed("Column %d, row %d: a value out of its alignment.",
											 col + 1, i);
				len = VARSIZE_4B(p);
				if (len < VARHDRSZ)
					frame_malformed("Column %d, row %d: a value shorter than its header.",
											 col + 1, i);
			}
			if (len > slot || len < 1)
				frame_malformed("Column %d, row %d: a value of %lld bytes in a slot of %lld.",
										 col + 1, i, (long long) len, (long long) slot);
		}
		datums[i] = PointerGetDatum(p);
	}
	return datums;
}

bool
vexec_frame_decode(VexecFrameReader *r, const char *data, size_t len, VexecBatch *into)
{
	FrameHeader hdr;
	VexecIpcHeader ih;
	size_t		consumed;
	FrameSchema *fs = NULL;
	struct ArrowArray array;
	MemoryContext old;
	int			n;
	int			i;

	vexec_batch_reset(into);

	if (len < VEXEC_FRAME_HEADER)
		frame_malformed("A frame shorter than its header.");
	memcpy(&hdr, data, sizeof(hdr));
	if (hdr.magic != VEXEC_FRAME_MAGIC || hdr.reserved != 0)
		frame_malformed("A frame without vexec's header.");

	/* its buffers 8-byte aligned, or the frame copied once so that they are */
	if ((uintptr_t) data % 8 != 0)
	{
		char	   *copy = vexec_batch_alloc(into, len);

		memcpy(copy, data, len);
		data = copy;
	}
	data += VEXEC_FRAME_HEADER;
	len -= VEXEC_FRAME_HEADER;
	if (!vexec_ipc_read_prefix(data, len, &ih, &consumed) || consumed + (size_t) ih.body_len != len)
		frame_malformed("A frame that is not one whole message.");

	if (ih.kind == VEXEC_IPC_SCHEMA)
	{
		read_schema(r, hdr.schema, ih.metadata, ih.metadata_len);
		return false;
	}
	if (ih.kind != VEXEC_IPC_RECORD_BATCH)
		frame_malformed("A frame that is neither a schema nor a record batch.");

	foreach_ptr(FrameSchema, s, r->schemas)
		if (s->hash == hdr.schema)
			fs = s;
	if (fs == NULL)
		frame_malformed("A record batch of a schema not sent before it.");

	old = MemoryContextSwitchTo(into->mcxt);
	vexec_ipc_read_batch(&fs->arrow, ih.metadata, ih.metadata_len, data + consumed,
						 (size_t) ih.body_len, &array);
	MemoryContextSwitchTo(old);
	if (array.length > VEXEC_BATCH_ROWS)
		frame_malformed("A record batch of %lld rows.", (long long) array.length);
	n = (int) array.length;

	for (i = 0; i < r->ncols; i++)
	{
		struct ArrowArray *c = array.children[i];
		VexecVec   *v = &into->cols[i];
		const VexecShape *shape = &fs->shapes[i];

		v->shape = *shape;
		v->encoding = VEXEC_FLAT;
		v->nvalues = n;
		v->validity = (uint64 *) c->buffers[0];
		v->datums = NULL;
		v->codes = NULL;
		v->dictionary = NULL;
		v->buffers = NULL;
		v->buffer_sizes = NULL;
		v->nbuffers = 0;
		switch (shape->layout)
		{
			case VEXEC_FIXED:
			case VEXEC_SCALED:
			case VEXEC_BYTE_BOOL:
			case VEXEC_BIT_BOOL:
				v->values = (void *) c->buffers[1];
				break;
			case VEXEC_DATUM:
				v->values = in_datums(into, v->type, v->validity, n, c->buffers[1],
									  c->buffers[2], i);
				break;
			case VEXEC_OFFSETS:
				v->values = (void *) c->buffers[1];
				v->nbuffers = 1;
				v->buffers = vexec_batch_alloc(into, sizeof(char *));
				v->buffer_sizes = vexec_batch_alloc(into, sizeof(int64));
				v->buffers[0] = (char *) c->buffers[2];
				v->buffer_sizes[0] = ((const int32 *) c->buffers[1])[n];
				break;
			case VEXEC_VIEW:
				v->values = (void *) c->buffers[1];
				v->nbuffers = (int) c->n_buffers - 3;
				v->buffers = (char **) &c->buffers[2];
				v->buffer_sizes = (int64 *) c->buffers[c->n_buffers - 1];
				break;
			default:
				frame_malformed("A column of no layout.");
		}
	}
	into->nrows = n;
	into->selection = NULL;
	return true;
}
