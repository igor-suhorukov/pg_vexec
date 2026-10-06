/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * write.c
 *	  Arrow IPC messages from the C Data Interface's structures
 *	  (pg_vector_executor.md §3.10, "No library"; §5, V7_0).
 *
 * A message is the encapsulation's prefix, a flatbuffer Message padded to
 * 8 bytes, and a body (arrow/docs/source/format/Columnar.rst:1218-1248).
 * A Schema message carries each field's name, nullability, type, children
 * and metadata (arrow/format/Schema.fbs:483-486, 520-539, 564-578).  A
 * RecordBatch message carries a FieldNode for each field and a Buffer for
 * each of its buffers, in a pre-order walk of the fields
 * (arrow/format/Message.fbs:34-43, 86-120; Columnar.rst:1318-1348), each
 * view field's count of variadic buffers (Columnar.rst:1360-1392), and the
 * body: the buffers end to end, each starting 8-byte aligned
 * (Columnar.rst:1303-1309), as the buffer listing orders them
 * (Columnar.rst:1172-1187).
 *
 * Nothing of the arrays is copied: the body is pieces pointing into their
 * buffers, with the zero bytes that pad each to 8.  What the C Data
 * Interface holds and IPC does not is left out: a validity bitmap where
 * there is no NULL, and a view array's trailing buffer of variadic sizes
 * (arrow/docs/source/format/CDataInterface.rst:559-567).  Unions have no
 * validity bitmap (Columnar.rst:867-869), as MetadataVersion V5 has it
 * (Schema.fbs:48-51).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "port/pg_bitutils.h"

#include "ipc/flatbuf.h"
#include "ipc/ipc.h"

const char	vexec_ipc_eos[VEXEC_IPC_EOS_LEN] = {'\xff', '\xff', '\xff', '\xff', 0, 0, 0, 0};

/* Padding, and the one offset of an empty array that has none. */
static const int64 zeros[1];

#ifdef WORDS_BIGENDIAN
#define IPC_HOST_ENDIANNESS	1
#else
#define IPC_HOST_ENDIANNESS	0		/* Schema.fbs:544, Little */
#endif

struct VexecIpcWriter
{
	MemoryContext cxt;			/* the writer's */
	MemoryContext msgcxt;		/* the last message's, emptied by each call */
	VexecIpcMessage msg;
};

/* A RecordBatch's flattening, as the walk finds it. */
typedef struct Body
{
	int64	   *nodes;			/* length, null_count: a FieldNode */
	int			nnodes;
	int64	   *buffers;		/* offset, length: a Buffer */
	int			nbuffers;
	int64	   *variadic;		/* each view field's variadic buffers */
	int			nvariadic;
	VexecIpcPiece *pieces;
	int			npieces;
	int64		len;
} Body;

VexecIpcWriter *
vexec_ipc_writer_create(MemoryContext parent)
{
	MemoryContext cxt = AllocSetContextCreate(parent, "vexec IPC writer",
											  ALLOCSET_SMALL_SIZES);
	VexecIpcWriter *w = MemoryContextAllocZero(cxt, sizeof(VexecIpcWriter));

	w->cxt = cxt;
	w->msgcxt = AllocSetContextCreate(cxt, "vexec IPC message", ALLOCSET_DEFAULT_SIZES);
	return w;
}

void
vexec_ipc_writer_free(VexecIpcWriter *writer)
{
	MemoryContextDelete(writer->cxt);
}

/* Room for one more of n elements, the array doubled when full. */
static void *
grow(void *array, int n, size_t elem)
{
	if (n == 0)
		return palloc(16 * elem);
	if (n >= 16 && (n & (n - 1)) == 0)
		return repalloc_huge(array, 2 * (Size) n * elem);
	return array;
}

static void
add_node(Body *b, int64 length, int64 nulls)
{
	b->nodes = grow(b->nodes, b->nnodes, 2 * sizeof(int64));
	b->nodes[2 * b->nnodes] = length;
	b->nodes[2 * b->nnodes + 1] = nulls;
	b->nnodes++;
}

static void
add_piece(Body *b, const void *data, size_t len)
{
	b->pieces = grow(b->pieces, b->npieces, sizeof(VexecIpcPiece));
	b->pieces[b->npieces].data = data;
	b->pieces[b->npieces].len = len;
	b->npieces++;
}

/* A buffer of len bytes at data, then the zeros that pad it to 8. */
static void
add_buffer(Body *b, const void *data, int64 len)
{
	int			pad = (int) ((8 - len % 8) % 8);

	if (len < 0 || (len > 0 && data == NULL))
		elog(ERROR, "vexec IPC writer: a buffer of %lld bytes at %p",
			 (long long) len, data);
	b->buffers = grow(b->buffers, b->nbuffers, 2 * sizeof(int64));
	b->buffers[2 * b->nbuffers] = b->len;
	b->buffers[2 * b->nbuffers + 1] = len;
	b->nbuffers++;
	if (len > 0)
	{
		add_piece(b, data, len);
		if (pad)
			add_piece(b, zeros, pad);
	}
	b->len += len + pad;
}

/* The NULLs of an array that does not say, counted in its bitmap. */
static int64
null_count(const struct ArrowArray *a)
{
	const uint8 *bits = a->buffers[0];
	int64		valid;

	if (a->null_count >= 0)
		return a->null_count;
	if (bits == NULL)
		return 0;
	valid = pg_popcount((const char *) bits, (int) (a->length / 8));
	if (a->length % 8)
		valid += pg_popcount32(bits[a->length / 8] & ((1u << (a->length % 8)) - 1));
	return a->length - valid;
}

static void
check_array(const struct ArrowSchema *s, const struct ArrowArray *a, int nbuffers)
{
	if (a->offset != 0)
		elog(ERROR, "vexec IPC writer: field \"%s\" is at offset %lld, not 0",
			 s->name ? s->name : "", (long long) a->offset);
	if (a->length < 0 || a->n_children != s->n_children ||
		(nbuffers >= 0 && a->n_buffers != nbuffers) || a->n_buffers < 0)
		elog(ERROR, "vexec IPC writer: field \"%s\" has %lld rows, %lld children and %lld buffers",
			 s->name ? s->name : "", (long long) a->length,
			 (long long) a->n_children, (long long) a->n_buffers);
}

static void
parse(const struct ArrowSchema *s, IpcType *t, int depth)
{
	check_stack_depth();
	if (depth > IPC_MAX_DEPTH)
		elog(ERROR, "vexec IPC writer: fields nested deeper than %d", IPC_MAX_DEPTH);
	if (!ipc_parse_format(s->format, t))
		elog(ERROR, "vexec IPC writer: \"%s\" is not an Arrow format", s->format);
	if (s->dictionary != NULL || t->id == IPC_RUN_END || t->id == IPC_LIST_VIEW ||
		t->id == IPC_LARGE_LIST_VIEW || (t->id == IPC_UNION && t->nids != s->n_children))
		elog(ERROR, "vexec IPC writer: field \"%s\" of format \"%s\"%s is not supported",
			 s->name ? s->name : "", s->format,
			 s->dictionary ? ", dictionary-encoded," : "");
}

/* One field's FieldNode and buffers, then its children's. */
static void
write_array(Body *b, const struct ArrowSchema *s, const struct ArrowArray *a, int depth)
{
	IpcType		t;
	int64		n = a->length;
	int64		nulls;
	int			i;

	parse(s, &t, depth);
	check_array(s, a, (t.id == IPC_BINARY_VIEW || t.id == IPC_UTF8_VIEW) ? -1 :
				ipc_buffers(&t, true));
	switch (t.id)
	{
		case IPC_NULL:
			add_node(b, n, n);
			break;
		case IPC_UNION:
			add_node(b, n, 0);
			add_buffer(b, a->buffers[0], n);
			if (t.dense)
				add_buffer(b, a->buffers[1], n * 4);
			break;
		default:
			nulls = null_count(a);
			add_node(b, n, nulls);
			add_buffer(b, nulls ? a->buffers[0] : NULL, nulls ? (n + 7) / 8 : 0);
			switch (t.id)
			{
				case IPC_STRUCT:
				case IPC_FIXED_LIST:
					break;
				case IPC_BOOL:
					add_buffer(b, a->buffers[1], (n + 7) / 8);
					break;
				case IPC_BINARY_VIEW:
				case IPC_UTF8_VIEW:
					{
						int64		nvariadic = a->n_buffers - 3;
						const int64 *sizes;

						if (nvariadic < 0)
							elog(ERROR, "vexec IPC writer: a view array of %lld buffers",
								 (long long) a->n_buffers);
						sizes = a->buffers[a->n_buffers - 1];
						add_buffer(b, a->buffers[1], n * 16);
						for (i = 0; i < nvariadic; i++)
							add_buffer(b, a->buffers[2 + i], sizes[i]);
						b->variadic = grow(b->variadic, b->nvariadic, sizeof(int64));
						b->variadic[b->nvariadic++] = nvariadic;
						break;
					}
				case IPC_BINARY:
				case IPC_UTF8:
				case IPC_LARGE_BINARY:
				case IPC_LARGE_UTF8:
				case IPC_LIST:
				case IPC_LARGE_LIST:
				case IPC_MAP:
					{
						const void *offsets = a->buffers[1];

						if (offsets == NULL && n == 0)
							offsets = zeros;
						add_buffer(b, offsets, (n + 1) * t.width);
						if (a->n_buffers == 3)
							add_buffer(b, a->buffers[2], t.width == 4 ?
									   ((const int32 *) offsets)[n] :
									   ((const int64 *) offsets)[n]);
						break;
					}
				default:		/* one buffer of fixed-width values */
					add_buffer(b, a->buffers[1], n * t.width);
					break;
			}
			break;
	}
	for (i = 0; i < s->n_children; i++)
		write_array(b, s->children[i], a->children[i], depth + 1);
}

/* The interface's metadata (CDataInterface.rst:353-383) as KeyValues. */
static uint32
write_metadata(FbBuilder *fb, const char *metadata)
{
	const char *p = metadata;
	int32		n;
	uint32	   *kvs;
	int			i;

	if (metadata == NULL)
		return 0;
	memcpy(&n, p, sizeof(int32));
	p += sizeof(int32);
	if (n <= 0)
		return 0;
	kvs = palloc(sizeof(uint32) * n);
	for (i = 0; i < n; i++)
	{
		int32		klen,
					vlen;
		const char *key,
				   *value;
		uint32		k,
					v;

		memcpy(&klen, p, sizeof(int32));
		key = p + sizeof(int32);
		memcpy(&vlen, key + klen, sizeof(int32));
		value = key + klen + sizeof(int32);
		p = value + vlen;
		k = fb_string(fb, key, klen);
		v = fb_string(fb, value, vlen);
		fb_start(fb);
		fb_add_offset(fb, 0, k);
		fb_add_offset(fb, 1, v);
		kvs[i] = fb_end(fb);
	}
	return fb_offsets(fb, kvs, n);
}

/* A field's type table, its members as Schema.fbs:85-444 has them. */
static uint32
write_type(FbBuilder *fb, const IpcType *t, const struct ArrowSchema *s)
{
	uint32		tz = 0;
	uint32		ids = 0;

	if (t->id == IPC_TIMESTAMP && t->tz[0] != '\0')
		tz = fb_string(fb, t->tz, strlen(t->tz));
	if (t->id == IPC_UNION)
	{
		int32		v[128];
		int			i;

		for (i = 0; i < t->nids; i++)
			v[i] = t->ids[i];
		ids = fb_ints(fb, v, t->nids, 4);
	}
	fb_start(fb);
	switch (t->id)
	{
		case IPC_INT:
			fb_add_int(fb, 0, t->bits, 4);
			fb_add_int(fb, 1, t->is_signed, 1);
			break;
		case IPC_DECIMAL:
			fb_add_int(fb, 0, t->precision, 4);
			fb_add_int(fb, 1, t->scale, 4);
			fb_add_int(fb, 2, t->bits, 4);
			break;
		case IPC_TIME:
			fb_add_int(fb, 0, t->unit, 2);
			fb_add_int(fb, 1, t->bits, 4);
			break;
		case IPC_TIMESTAMP:
			fb_add_int(fb, 0, t->unit, 2);
			if (tz)
				fb_add_offset(fb, 1, tz);
			break;
		case IPC_FLOAT:
		case IPC_DATE:
		case IPC_INTERVAL:
		case IPC_DURATION:
			fb_add_int(fb, 0, t->unit, 2);
			break;
		case IPC_FIXED_BINARY:
		case IPC_FIXED_LIST:
			fb_add_int(fb, 0, t->size, 4);
			break;
		case IPC_MAP:
			fb_add_int(fb, 0, (s->flags & ARROW_FLAG_MAP_KEYS_SORTED) != 0, 1);
			break;
		case IPC_UNION:
			fb_add_int(fb, 0, t->dense, 2);
			fb_add_offset(fb, 1, ids);
			break;
		default:				/* a table without members */
			break;
	}
	return fb_end(fb);
}

static uint32
write_field(FbBuilder *fb, const struct ArrowSchema *s, int depth)
{
	IpcType		t;
	uint32	   *kids = palloc(sizeof(uint32) * Max(s->n_children, 1));
	uint32		children,
				metadata,
				type,
				name = 0;
	int			i;

	parse(s, &t, depth);
	for (i = 0; i < s->n_children; i++)
		kids[i] = write_field(fb, s->children[i], depth + 1);
	children = fb_offsets(fb, kids, (int) s->n_children);
	metadata = write_metadata(fb, s->metadata);
	type = write_type(fb, &t, s);
	if (s->name)
		name = fb_string(fb, s->name, strlen(s->name));
	fb_start(fb);
	if (name)
		fb_add_offset(fb, 0, name);
	fb_add_int(fb, 1, (s->flags & ARROW_FLAG_NULLABLE) != 0, 1);
	fb_add_int(fb, 2, t.id, 1);
	fb_add_offset(fb, 3, type);
	fb_add_offset(fb, 5, children);
	if (metadata)
		fb_add_offset(fb, 6, metadata);
	return fb_end(fb);
}

/* A call's message, in a context emptied of the last one's. */
static MemoryContext
begin_message(VexecIpcWriter *w, const struct ArrowSchema *schema, FbBuilder *fb)
{
	MemoryContext old;

	MemoryContextReset(w->msgcxt);
	memset(&w->msg, 0, sizeof(w->msg));
	old = MemoryContextSwitchTo(w->msgcxt);
	if (strcmp(schema->format, "+s") != 0)
		elog(ERROR, "vexec IPC writer: a record batch's schema of format \"%s\"", schema->format);
	fb_init(fb);
	return old;
}

/*
 * The Message around a header (Message.fbs:156-161), and the prefix before
 * it: the continuation marker and the metadata's length, padded to 8.
 */
static const VexecIpcMessage *
finish_message(VexecIpcWriter *w, FbBuilder *fb, int kind, uint32 header, int64 body_len,
			   MemoryContext old)
{
	uint32		root;
	const char *data;
	size_t		len;
	size_t		padded;
	char	   *buf;
	int			i;

	fb_start(fb);
	fb_add_int(fb, 0, IPC_V5, 2);
	fb_add_int(fb, 1, kind, 1);
	fb_add_offset(fb, 2, header);
	fb_add_int(fb, 3, body_len, 8);
	root = fb_end(fb);
	data = fb_finish(fb, root, &len);
	padded = TYPEALIGN(8, len);
	buf = palloc0(8 + padded);
	memset(buf, 0xff, 4);
	for (i = 0; i < 4; i++)
		buf[4 + i] = (char) (padded >> (8 * i));
	memcpy(buf + 8, data, len);
	w->msg.kind = kind;
	w->msg.prefix = buf;
	w->msg.metadata = buf + 8;
	w->msg.metadata_len = padded;
	w->msg.body_len = body_len;
	MemoryContextSwitchTo(old);
	return &w->msg;
}

const VexecIpcMessage *
vexec_ipc_write_schema(VexecIpcWriter *writer, const struct ArrowSchema *schema)
{
	FbBuilder	fb;
	MemoryContext old = begin_message(writer, schema, &fb);
	uint32	   *fields = palloc(sizeof(uint32) * Max(schema->n_children, 1));
	uint32		vector,
				metadata;
	int			i;

	for (i = 0; i < schema->n_children; i++)
		fields[i] = write_field(&fb, schema->children[i], 1);
	vector = fb_offsets(&fb, fields, (int) schema->n_children);
	metadata = write_metadata(&fb, schema->metadata);
	fb_start(&fb);
	fb_add_int(&fb, 0, IPC_HOST_ENDIANNESS, 2);
	fb_add_offset(&fb, 1, vector);
	if (metadata)
		fb_add_offset(&fb, 2, metadata);
	return finish_message(writer, &fb, VEXEC_IPC_SCHEMA, fb_end(&fb), 0, old);
}

const VexecIpcMessage *
vexec_ipc_write_batch(VexecIpcWriter *writer, const struct ArrowSchema *schema,
					  const struct ArrowArray *array)
{
	FbBuilder	fb;
	MemoryContext old = begin_message(writer, schema, &fb);
	Body		b;
	uint32		nodes,
				buffers,
				variadic = 0;
	int			i;

	memset(&b, 0, sizeof(b));
	check_array(schema, array, -1);
	if (array->n_buffers > 0 && null_count(array) > 0)
		elog(ERROR, "vexec IPC writer: a record batch with NULLs of its own");
	for (i = 0; i < schema->n_children; i++)
		write_array(&b, schema->children[i], array->children[i], 1);
	nodes = fb_structs64(&fb, b.nodes, b.nnodes, 2);
	buffers = fb_structs64(&fb, b.buffers, b.nbuffers, 2);
	if (b.nvariadic > 0)
		variadic = fb_ints(&fb, b.variadic, b.nvariadic, 8);
	fb_start(&fb);
	fb_add_int(&fb, 0, array->length, 8);
	fb_add_offset(&fb, 1, nodes);
	fb_add_offset(&fb, 2, buffers);
	if (variadic)
		fb_add_offset(&fb, 4, variadic);
	writer->msg.pieces = b.pieces;
	writer->msg.npieces = b.npieces;
	writer->msg.nrows = array->length;
	return finish_message(writer, &fb, VEXEC_IPC_RECORD_BATCH, fb_end(&fb), b.len, old);
}

void
vexec_ipc_append(StringInfo out, const VexecIpcMessage *msg, bool encapsulated)
{
	int			i;

	if (encapsulated)
		appendBinaryStringInfo(out, msg->prefix, 8);
	appendBinaryStringInfo(out, msg->metadata, (int) msg->metadata_len);
	for (i = 0; i < msg->npieces; i++)
	{
		if (msg->pieces[i].len > MaxAllocSize)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("an Arrow IPC buffer of %zu bytes is too long for a string",
							msg->pieces[i].len)));
		appendBinaryStringInfo(out, msg->pieces[i].data, (int) msg->pieces[i].len);
	}
}
