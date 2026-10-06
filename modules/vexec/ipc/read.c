/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * read.c
 *	  Arrow IPC messages back to the C Data Interface's structures, checked
 *	  as a client's input (pg_vector_executor.md §3.10, "No library";
 *	  §3.16; §5, V7_0).
 *
 * A message comes from another process or a client, so nothing in it is
 * trusted.  The flatbuffer is read through flatbuf.c's checked reader.  A
 * Schema message's fields (arrow/format/Schema.fbs:520-539) become the
 * interface's format strings, names, flags and metadata
 * (arrow/docs/source/format/CDataInterface.rst:95-250, 353-402), each type's
 * parameters checked, and a map's entries and key not nullable
 * (Schema.fbs:127-138).  A RecordBatch message's FieldNodes and Buffers
 * (arrow/format/Message.fbs:34-43, 86-120; Schema.fbs:548-559) are walked
 * in the schema's pre-order (arrow/docs/source/format/Columnar.rst:
 * 1318-1392), and must be exactly as many as its fields and their buffers.
 * Each buffer must lie within the body and be long enough for its field's
 * values; offsets must not decrease and stay within their data or child
 * (Columnar.rst:447-449, 544-549); views must lie within their variadic
 * buffers, carry their value's prefix and pad an inline value with zeros
 * (Columnar.rst:492-524); a fixed-size list's child, a struct's and a
 * sparse union's children must be long enough (Columnar.rst:737-740,
 * 936-938); a union's type ids must be among its own, and a dense union's
 * offsets within the child they choose, never decreasing for one child
 * (Columnar.rst:885-888); a validity bitmap must hold as many NULLs as its
 * FieldNode says; utf8 must be UTF-8 (Schema.fbs:170).  Arrow allows U+0000
 * in UTF-8, so PostgreSQL's verifier, which does not, is applied a
 * character at a time.
 *
 * Refused as not supported: dictionaries, compressed bodies
 * (Message.fbs:74-81, 102-103), streams of the other endianness
 * (Columnar.rst:1455-1468), run-end encoding, list views, metadata before
 * V4 (Schema.fbs:31-52), a V4 union with NULLs of its own, and arrays longer
 * than 2^31 - 1 values, the limit Arrow allows an implementation
 * (Columnar.rst:300-305).
 *
 * The arrays point into the body where a buffer lies 8-byte aligned, and
 * into an aligned copy where it does not, or where the body ends before the
 * buffer's last 8-byte word; copies are bounded by the body's length, so
 * that buffers made to overlap cannot multiply the memory a message takes.  A schema's names and metadata are bounded by its length
 * likewise, since a flatbuffer may refer to one string or table many times.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/int.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"

#include "ipc/flatbuf.h"
#include "ipc/ipc.h"

/* What a buffer of no bytes points to: also an empty array's one offset. */
static const int64 empty[2];

#define IPC_MAX_LENGTH	PG_INT32_MAX

#ifdef WORDS_BIGENDIAN
#define IPC_HOST_ENDIANNESS	1
#else
#define IPC_HOST_ENDIANNESS	0
#endif

/* A field, as an error names it. */
#define FIELD(s)	((s)->name && strlen((s)->name) < 64 ? (s)->name : "...")

static void
release_schema(struct ArrowSchema *s)
{
	int64		i;

	for (i = 0; i < s->n_children; i++)
		if (s->children[i]->release)
			s->children[i]->release(s->children[i]);
	s->release = NULL;
}

static void
release_array(struct ArrowArray *a)
{
	int64		i;

	for (i = 0; i < a->n_children; i++)
		if (a->children[i]->release)
			a->children[i]->release(a->children[i]);
	a->release = NULL;
}

/* UTF-8 as Arrow has it: no overlong form, no surrogate, NUL allowed. */
static bool
utf8_valid(const char *s, int64 len)
{
	const unsigned char *p = (const unsigned char *) s;

	while (len > 0)
	{
		uint64		word;
		int			l;

		/* ASCII eight bytes at a time */
		if (len >= 8)
		{
			memcpy(&word, p, 8);
			if ((word & UINT64CONST(0x8080808080808080)) == 0)
			{
				p += 8;
				len -= 8;
				continue;
			}
		}
		if (*p < 0x80)
		{
			p++;
			len--;
			continue;
		}
		l = pg_utf_mblen(p);
		if (l > len || !pg_utf8_islegal(p, l))
			return false;
		p += l;
		len -= l;
	}
	return true;
}

/* The Message, and its header's table. */
static void
read_message(const char *metadata, size_t len, VexecIpcHeader *h, FbTable *header,
			 int *version)
{
	FbTable		msg;
	int64		v;
	int64		type;

	memset(h, 0, sizeof(*h));
	fb_root(metadata, len, &msg);
	v = fb_int(&msg, 0, 2, 0);
	if (v < IPC_V4 || v > IPC_V5)
		ipc_unsupported("The message's metadata version is V%lld; V4 and V5 are read.",
						(long long) v + 1);
	type = (uint8) fb_int(&msg, 1, 1, 0);
	switch (type)
	{
		case IPC_HEADER_SCHEMA:
			h->kind = VEXEC_IPC_SCHEMA;
			break;
		case IPC_HEADER_RECORD_BATCH:
			h->kind = VEXEC_IPC_RECORD_BATCH;
			break;
		case IPC_HEADER_DICTIONARY:
			ipc_unsupported("A DictionaryBatch message: dictionaries are not read.");
			break;
		case 0:
			ipc_malformed("A message without a header.");
			break;
		default:
			ipc_unsupported("A message of header type %lld: only Schema and RecordBatch messages are read.",
							(long long) type);
	}
	if (!fb_table(&msg, 2, header))
		ipc_malformed("A message without its header.");
	h->body_len = fb_int(&msg, 3, 8, 0);
	if (h->body_len < 0)
		ipc_malformed("A message body of %lld bytes.", (long long) h->body_len);
	h->metadata = metadata;
	h->metadata_len = len;
	*version = (int) v;
}

void
vexec_ipc_read_message(const char *metadata, size_t len, VexecIpcHeader *header)
{
	FbTable		h;
	int			version;

	read_message(metadata, len, header, &h, &version);
}

bool
vexec_ipc_read_prefix(const char *data, size_t len, VexecIpcHeader *header, size_t *consumed)
{
	const uint8 *p = (const uint8 *) data;
	size_t		prefix = 4;
	int32		mlen;

	memset(header, 0, sizeof(*header));
	*consumed = 0;
	if (len == 0)
		return true;			/* the end of data: VEXEC_IPC_END */
	if (len < 4)
	{
		*consumed = 4;
		return false;
	}
	mlen = (int32) (p[0] | p[1] << 8 | p[2] << 16 | (uint32) p[3] << 24);
	if (mlen == -1)				/* the continuation marker, from 0.15 on */
	{
		if (len < 8)
		{
			*consumed = 8;
			return false;
		}
		prefix = 8;
		mlen = (int32) (p[4] | p[5] << 8 | p[6] << 16 | (uint32) p[7] << 24);
	}
	if (mlen < 0)
		ipc_malformed("A message's metadata length of %d.", mlen);
	*consumed = prefix + mlen;
	if (mlen == 0)
		return true;			/* the end-of-stream marker */
	if (len < prefix + mlen)
		return false;
	vexec_ipc_read_message(data + prefix, mlen, header);
	return true;
}

/* ---- schemas ---- */

/*
 * What a schema may still make of its bytes: fields, and bytes of names,
 * formats and metadata.  A distinct field takes at least 16 bytes of a
 * flatbuffer, and each byte of a string one; more means the flatbuffer
 * refers to its objects many times.
 */
typedef struct SchemaRead
{
	int64		fields;
	int64		bytes;
} SchemaRead;

static void
charge(SchemaRead *r, int64 bytes)
{
	r->bytes -= bytes;
	if (r->bytes < 0)
		ipc_malformed("The schema's names and metadata are longer than its bytes allow.");
}

/* A name or a time zone: UTF-8, with no NUL, which a C string cannot hold. */
static char *
read_text(SchemaRead *r, const FbTable *t, int id, const char *what)
{
	const char *s;
	uint32		len;

	if (!fb_string_at(t, id, &s, &len))
		return NULL;
	charge(r, len);
	if (memchr(s, '\0', len) != NULL)
		ipc_unsupported("%s holds a NUL byte.", what);
	if (!utf8_valid(s, len))
		ipc_malformed("%s is not valid UTF-8.", what);
	return pnstrdup(s, len);
}

/* KeyValues, in the C Data Interface's encoding (CDataInterface.rst:353-383). */
static const char *
read_metadata(SchemaRead *r, const FbTable *t, int id)
{
	FbVector	v;
	StringInfoData buf;
	int32		n;
	uint32		i;
	int			k;

	if (!fb_vector(t, id, 4, &v) || v.n == 0)
		return NULL;
	charge(r, 4 + (int64) v.n * 8);
	initStringInfo(&buf);
	n = (int32) v.n;
	appendBinaryStringInfo(&buf, &n, sizeof(int32));
	for (i = 0; i < v.n; i++)
	{
		FbTable		kv;

		fb_vector_table(&v, i, &kv);
		for (k = 0; k < 2; k++)
		{
			const char *s;
			uint32		len;
			int32		l;

			if (!fb_string_at(&kv, k, &s, &len))
				ipc_malformed("A metadata entry without its %s.", k ? "value" : "key");
			charge(r, len);
			l = (int32) len;
			appendBinaryStringInfo(&buf, &l, sizeof(int32));
			appendBinaryStringInfo(&buf, s, (int) len);
		}
	}
	return buf.data;
}

/*
 * A field's type table as a format string, checked; *kids the children its
 * type must have, -1 for any.
 */
static char *
type_format(SchemaRead *r, const char *name, int type, const FbTable *tt, uint32 nkids,
			int64 *flags, int *kids)
{
	static const char *const units = "smun";
	char	   *f = NULL;
	int64		u,
				w,
				p,
				s;

	*kids = 0;
	switch (type)
	{
		case IPC_NULL:
			return "n";
		case IPC_BOOL:
			return "b";
		case IPC_BINARY:
			return "z";
		case IPC_UTF8:
			return "u";
		case IPC_LARGE_BINARY:
			return "Z";
		case IPC_LARGE_UTF8:
			return "U";
		case IPC_BINARY_VIEW:
			return "vz";
		case IPC_UTF8_VIEW:
			return "vu";
		case IPC_INT:
			w = fb_int(tt, 0, 4, 0);
			u = (w == 8) ? 0 : (w == 16) ? 1 : (w == 32) ? 2 : (w == 64) ? 3 : -1;
			if (u < 0)
				ipc_malformed("Field \"%s\" is an Int of %lld bits.", name, (long long) w);
			return pnstrdup((fb_int(tt, 1, 1, 0) ? "csil" : "CSIL") + u, 1);
		case IPC_FLOAT:
			u = fb_int(tt, 0, 2, 0);
			if (u < 0 || u > 2)
				ipc_malformed("Field \"%s\" is a FloatingPoint of precision %lld.", name, (long long) u);
			return pnstrdup("efg" + u, 1);
		case IPC_DECIMAL:
			p = fb_int(tt, 0, 4, 0);
			s = fb_int(tt, 1, 4, 0);
			w = fb_int(tt, 2, 4, 128);
			u = (w == 32) ? 9 : (w == 64) ? 18 : (w == 128) ? 38 : (w == 256) ? 76 : 0;
			if (u == 0 || p < 1 || p > u)
				ipc_malformed("Field \"%s\" is a Decimal of %lld bits and precision %lld.",
							  name, (long long) w, (long long) p);
			f = w == 128 ? psprintf("d:%lld,%lld", (long long) p, (long long) s) :
				psprintf("d:%lld,%lld,%lld", (long long) p, (long long) s, (long long) w);
			break;
		case IPC_DATE:
			u = fb_int(tt, 0, 2, 1);
			if (u < 0 || u > 1)
				ipc_malformed("Field \"%s\" is a Date of unit %lld.", name, (long long) u);
			return u ? "tdm" : "tdD";
		case IPC_TIME:
			u = fb_int(tt, 0, 2, 1);
			w = fb_int(tt, 1, 4, 32);
			if (u < 0 || u > 3 || w != (u < 2 ? 32 : 64))
				ipc_malformed("Field \"%s\" is a Time of unit %lld and %lld bits.",
							  name, (long long) u, (long long) w);
			f = psprintf("tt%c", units[u]);
			break;
		case IPC_TIMESTAMP:
			{
				char	   *tz;

				u = fb_int(tt, 0, 2, 0);
				if (u < 0 || u > 3)
					ipc_malformed("Field \"%s\" is a Timestamp of unit %lld.", name, (long long) u);
				tz = read_text(r, tt, 1, "A time zone");
				f = psprintf("ts%c:%s", units[u], tz ? tz : "");
				break;
			}
		case IPC_DURATION:
			u = fb_int(tt, 0, 2, 1);
			if (u < 0 || u > 3)
				ipc_malformed("Field \"%s\" is a Duration of unit %lld.", name, (long long) u);
			f = psprintf("tD%c", units[u]);
			break;
		case IPC_INTERVAL:
			u = fb_int(tt, 0, 2, 0);
			if (u < 0 || u > 2)
				ipc_malformed("Field \"%s\" is an Interval of unit %lld.", name, (long long) u);
			f = psprintf("ti%c", "MDn"[u]);
			break;
		case IPC_FIXED_BINARY:
		case IPC_FIXED_LIST:
			w = fb_int(tt, 0, 4, 0);
			if (w < 0)
				ipc_malformed("Field \"%s\" has a fixed size of %lld.", name, (long long) w);
			f = psprintf(type == IPC_FIXED_BINARY ? "w:%lld" : "+w:%lld", (long long) w);
			*kids = type == IPC_FIXED_LIST ? 1 : 0;
			break;
		case IPC_LIST:
		case IPC_LARGE_LIST:
			*kids = 1;
			return type == IPC_LIST ? "+l" : "+L";
		case IPC_STRUCT:
			*kids = -1;
			return "+s";
		case IPC_MAP:
			*kids = 1;
			if (fb_int(tt, 0, 1, 0))
				*flags |= ARROW_FLAG_MAP_KEYS_SORTED;
			return "+m";
		case IPC_UNION:
			{
				StringInfoData buf;
				FbVector	ids;
				bool		has_ids = fb_vector(tt, 1, 4, &ids);
				bool		seen[128] = {0};
				uint32		i;

				u = fb_int(tt, 0, 2, 0);
				if (u < 0 || u > 1 || nkids > 128 || (has_ids && ids.n != nkids))
					ipc_malformed("Field \"%s\" is a Union of mode %lld, %u children and %u type ids.",
								  name, (long long) u, nkids, has_ids ? ids.n : nkids);
				initStringInfo(&buf);
				appendStringInfoString(&buf, u ? "+ud:" : "+us:");
				for (i = 0; i < nkids; i++)
				{
					int64		id = has_ids ? fb_vector_int(&ids, i, 4) : i;

					if (id < 0 || id > 127 || seen[id])
						ipc_malformed("Field \"%s\" is a Union with type id %lld.", name, (long long) id);
					seen[id] = true;
					appendStringInfo(&buf, i ? ",%d" : "%d", (int) id);
				}
				*kids = -1;
				f = buf.data;
				break;
			}
		case IPC_RUN_END:
		case IPC_LIST_VIEW:
		case IPC_LARGE_LIST_VIEW:
			ipc_unsupported("Field \"%s\" is %s, which is not read.", name,
							type == IPC_RUN_END ? "run-end encoded" : "a list view");
			break;
		case IPC_NONE:
			ipc_malformed("Field \"%s\" has no type.", name);
			break;
		default:
			ipc_unsupported("Field \"%s\" is of Arrow type %d, which is not read.", name, type);
	}
	charge(r, strlen(f));
	return f;
}

static void
read_field(SchemaRead *r, const FbTable *fld, struct ArrowSchema *out, int depth)
{
	FbTable		tt;
	FbVector	kids;
	int			type;
	int			nkids;
	char	   *name;
	uint32		i;

	memset(&tt, 0, sizeof(tt));
	check_stack_depth();
	if (depth > IPC_MAX_DEPTH)
		ipc_unsupported("Fields nested deeper than %d.", IPC_MAX_DEPTH);
	if (--r->fields < 0)
		ipc_malformed("The schema has more fields than its bytes can hold.");
	name = read_text(r, fld, 0, "A field's name");
	out->name = name ? name : "";
	if (fb_has(fld, 4))
		ipc_unsupported("Field \"%s\" is dictionary-encoded: dictionaries are not read.",
						FIELD(out));
	out->flags = fb_int(fld, 1, 1, 0) ? ARROW_FLAG_NULLABLE : 0;
	type = (uint8) fb_int(fld, 2, 1, 0);
	if (!fb_vector(fld, 5, 4, &kids))
		kids.n = 0;
	if (kids.n > r->fields)
		ipc_malformed("The schema has more fields than its bytes can hold.");
	if (type != IPC_NONE && !fb_table(fld, 3, &tt))
		ipc_malformed("Field \"%s\" has no type table.", FIELD(out));
	out->format = type_format(r, FIELD(out), type, &tt, kids.n, &out->flags, &nkids);
	if (nkids >= 0 && kids.n != nkids)
		ipc_malformed("Field \"%s\" of format \"%s\" has %u children.",
					  FIELD(out), out->format, kids.n);
	out->metadata = read_metadata(r, fld, 6);
	out->n_children = kids.n;
	out->children = palloc(sizeof(struct ArrowSchema *) * Max(kids.n, 1));
	for (i = 0; i < kids.n; i++)
	{
		FbTable		kid;

		out->children[i] = palloc0(sizeof(struct ArrowSchema));
		fb_vector_table(&kids, i, &kid);
		read_field(r, &kid, out->children[i], depth + 1);
	}
	/* Schema.fbs:127-138: neither the entries nor the key may be nullable */
	if (type == IPC_MAP &&
		(strcmp(out->children[0]->format, "+s") != 0 || out->children[0]->n_children != 2 ||
		 (out->children[0]->flags & ARROW_FLAG_NULLABLE) ||
		 (out->children[0]->children[0]->flags & ARROW_FLAG_NULLABLE)))
		ipc_malformed("Field \"%s\" is a Map whose entries are not a struct of a key and a value, neither nullable.",
					  FIELD(out));
	out->release = release_schema;
}

void
vexec_ipc_read_schema(const char *metadata, size_t len, struct ArrowSchema *out)
{
	VexecIpcHeader h;
	FbTable		schema;
	FbVector	v;
	SchemaRead	r;
	int			version;
	int64		e;
	uint32		i;

	memset(out, 0, sizeof(*out));
	read_message(metadata, len, &h, &schema, &version);
	if (h.kind != VEXEC_IPC_SCHEMA)
		ipc_malformed("A RecordBatch message where a Schema message is expected.");
	e = fb_int(&schema, 0, 2, 0);
	if (e != 0 && e != 1)
		ipc_malformed("A schema of endianness %lld.", (long long) e);
	if (e != IPC_HOST_ENDIANNESS)
		ipc_unsupported("The stream is %s-endian, and this server is not.", e ? "big" : "little");
	if (fb_vector(&schema, 3, 8, &v))
		for (i = 0; i < v.n; i++)
			if (fb_vector_int(&v, i, 8) != 0)
				ipc_unsupported("The stream uses Arrow IPC feature %lld.",
								(long long) fb_vector_int(&v, i, 8));
	r.fields = len / 16 + 1;
	r.bytes = 4 * (int64) len + 65536;
	if (!fb_vector(&schema, 1, 4, &v))
		v.n = 0;
	if (v.n > r.fields)
		ipc_malformed("The schema has more fields than its bytes can hold.");
	out->format = "+s";
	out->name = "";
	out->metadata = read_metadata(&r, &schema, 2);
	out->n_children = v.n;
	out->children = palloc(sizeof(struct ArrowSchema *) * Max(v.n, 1));
	for (i = 0; i < v.n; i++)
	{
		FbTable		fld;

		out->children[i] = palloc0(sizeof(struct ArrowSchema));
		fb_vector_table(&v, i, &fld);
		read_field(&r, &fld, out->children[i], 1);
	}
	out->release = release_schema;
}

/* ---- record batches ---- */

typedef struct BatchRead
{
	const char *body;
	int64		body_len;		/* as the message gives it */
	int64		copied;			/* bytes copied for alignment */
	bool		v4;				/* unions have a validity bitmap */
	FbVector	nodes;
	FbVector	buffers;
	FbVector	variadic;
	uint32		inode;
	uint32		ibuffer;
	uint32		ivariadic;
} BatchRead;

/* A field of the caller's schema: one the reader could have made. */
static void
schema_type(const struct ArrowSchema *s, IpcType *t, int depth)
{
	check_stack_depth();
	if (depth > IPC_MAX_DEPTH || !ipc_parse_format(s->format, t) || s->dictionary != NULL ||
		t->id == IPC_RUN_END || t->id == IPC_LIST_VIEW || t->id == IPC_LARGE_LIST_VIEW ||
		(t->id == IPC_UNION && t->nids != s->n_children))
		elog(ERROR, "vexec IPC reader: a schema's field of format \"%s\" is not read", s->format);
}

/* The fields, buffers and view fields a schema's batch has. */
static void
count_fields(const struct ArrowSchema *s, bool v4, int64 *nfields, int64 *nbuffers,
			 int64 *nviews, int depth)
{
	IpcType		t;
	int64		i;

	schema_type(s, &t, depth);
	(*nfields)++;
	*nbuffers += ipc_buffers(&t, false) + (v4 && t.id == IPC_UNION ? 1 : 0);
	if (t.id == IPC_BINARY_VIEW || t.id == IPC_UTF8_VIEW)
		(*nviews)++;
	for (i = 0; i < s->n_children; i++)
		count_fields(s->children[i], v4, nfields, nbuffers, nviews, depth + 1);
}

static int64
mul(int64 a, int64 b, const struct ArrowSchema *s)
{
	int64		r;

	if (pg_mul_s64_overflow(a, b, &r))
		ipc_malformed("Field \"%s\" needs more than 2^63 bytes.", FIELD(s));
	return r;
}

/* The next Buffer, which must lie within the body. */
static void
next_buffer(BatchRead *r, int64 *offset, int64 *length)
{
	int64		end;

	fb_vector_pair(&r->buffers, r->ibuffer, offset, length);
	if (*offset < 0 || *length < 0 || pg_add_s64_overflow(*offset, *length, &end) ||
		end > r->body_len)
		ipc_malformed("Buffer %u, %lld bytes at %lld, lies outside the body's %lld bytes.",
					  r->ibuffer, (long long) *length, (long long) *offset,
					  (long long) r->body_len);
	r->ibuffer++;
}

/*
 * A buffer's first need bytes, 8-byte aligned and readable in whole 8-byte
 * words, as vexec reads bitmaps: in the body, or in a copy padded with
 * zeros.
 */
static const char *
buffer_at(BatchRead *r, int64 offset, int64 length, int64 need,
		  const struct ArrowSchema *s, const char *what)
{
	int64		words = TYPEALIGN64(8, need);
	const char *p;
	char	   *copy;

	if (length < need)
		ipc_malformed("Field \"%s\": its %s buffer holds %lld bytes, not the %lld it needs.",
					  FIELD(s), what, (long long) length, (long long) need);
	if (need == 0)
		return (const char *) empty;
	p = r->body + offset;
	if ((uintptr_t) p % 8 == 0 && offset + words <= r->body_len)
		return p;
	if (pg_add_s64_overflow(r->copied, words, &r->copied) ||
		r->copied > r->body_len + 8 * (int64) r->buffers.n)
		ipc_malformed("The message's buffers overlap.");
	copy = MemoryContextAllocAligned(CurrentMemoryContext, words, 8,
									 MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	memcpy(copy, p, need);
	return copy;
}

/* A buffer not read: it must lie within the body all the same. */
static void
skip_buffer(BatchRead *r)
{
	int64		offset,
				length;

	next_buffer(r, &offset, &length);
}

static const char *
take_buffer(BatchRead *r, int64 need, const struct ArrowSchema *s, const char *what)
{
	int64		offset,
				length;

	next_buffer(r, &offset, &length);
	return buffer_at(r, offset, length, need, s, what);
}

static inline bool
is_valid(const uint8 *validity, int64 i)
{
	return validity == NULL || ((validity[i / 8] >> (i % 8)) & 1);
}

static inline int64
offset_at(const char *offsets, int width, int64 i)
{
	return width == 4 ? ((const int32 *) offsets)[i] : ((const int64 *) offsets)[i];
}

/* A validity bitmap, holding as many NULLs as its FieldNode says. */
static const uint8 *
take_validity(BatchRead *r, int64 n, int64 nulls, const struct ArrowSchema *s)
{
	const uint8 *bits;
	int64		valid;

	if (nulls == 0)
	{
		skip_buffer(r);
		return NULL;
	}
	bits = (const uint8 *) take_buffer(r, (n + 7) / 8, s, "validity");
	valid = pg_popcount((const char *) bits, (int) (n / 8));
	if (n % 8)
		valid += pg_popcount32(bits[n / 8] & ((1u << (n % 8)) - 1));
	if (n - valid != nulls)
		ipc_malformed("Field \"%s\": its validity bitmap holds %lld NULLs, its FieldNode %lld.",
					  FIELD(s), (long long) (n - valid), (long long) nulls);
	return bits;
}

/*
 * Offsets, from 0 up and never decreasing; *last the last of them.  An
 * array of no values may have no offsets at all.
 */
static const char *
take_offsets(BatchRead *r, int64 n, int width, const struct ArrowSchema *s, int64 *last)
{
	int64		offset,
				length,
				o = 0,
				i;
	const char *offsets;

	next_buffer(r, &offset, &length);
	if (n == 0 && length == 0)
		offsets = (const char *) empty;
	else
		offsets = buffer_at(r, offset, length, mul(n + 1, width, s), s, "offsets");
	for (i = 0; i <= n; i++)
	{
		int64		next = offset_at(offsets, width, i);

		if (next < o)
			ipc_malformed("Field \"%s\": offset %lld is %lld, %s.", FIELD(s), (long long) i,
						  (long long) next, i ? "less than the one before" : "less than 0");
		o = next;
	}
	*last = o;
	return offsets;
}

/* An out-of-line view's value, for the UTF-8 check. */
typedef struct ViewRange
{
	int32		buffer;
	int32		offset;
	int32		size;
	int32		row;
} ViewRange;

static int
range_cmp(const void *a, const void *b)
{
	const ViewRange *x = a;
	const ViewRange *y = b;

	if (x->buffer != y->buffer)
		return x->buffer < y->buffer ? -1 : 1;
	return (x->offset > y->offset) - (x->offset < y->offset);
}

static inline bool
utf8_continuation(char c)
{
	return ((unsigned char) c & 0xC0) == 0x80;
}

/*
 * Views (Columnar.rst:492-524): each valid one's size, an inline value's
 * zero padding, an out-of-line value's buffer, range and prefix, and utf8's
 * UTF-8.  Views may share their bytes, so out-of-line values are checked as
 * the ranges they cover, each byte once: a range of UTF-8 holds values of
 * UTF-8 exactly where each value starts and ends on a character.
 */
static void
check_views(const struct ArrowSchema *s, const char *views, const uint8 *validity, int64 n,
			const char *const *data, const int64 *sizes, int64 nvariadic, bool utf8)
{
	ViewRange  *ranges = NULL;
	int64		nranges = 0;
	int64		i,
				j,
				k;

	for (i = 0; i < n; i++)
	{
		const char *v = views + 16 * i;
		int32		size,
					buffer,
					offset;

		if (!is_valid(validity, i))
			continue;
		memcpy(&size, v, 4);
		if (size < 0)
			ipc_malformed("Field \"%s\": row %lld's view has a size of %d.",
						  FIELD(s), (long long) i, size);
		if (size <= 12)
		{
			for (k = size; k < 12; k++)
				if (v[4 + k] != 0)
					ipc_malformed("Field \"%s\": row %lld's inline view is not padded with zeros.",
								  FIELD(s), (long long) i);
			if (utf8 && !utf8_valid(v + 4, size))
				ipc_malformed("Field \"%s\": row %lld is not valid UTF-8.", FIELD(s), (long long) i);
			continue;
		}
		memcpy(&buffer, v + 8, 4);
		memcpy(&offset, v + 12, 4);
		if (buffer < 0 || buffer >= nvariadic || offset < 0 || (int64) offset + size > sizes[buffer])
			ipc_malformed("Field \"%s\": row %lld's view of %d bytes at %d of buffer %d lies outside its %lld buffers.",
						  FIELD(s), (long long) i, size, offset, buffer, (long long) nvariadic);
		if (memcmp(v + 4, data[buffer] + offset, 4) != 0)
			ipc_malformed("Field \"%s\": row %lld's view has a prefix that is not its value's.",
						  FIELD(s), (long long) i);
		if (!utf8)
			continue;
		if (ranges == NULL)
			ranges = MemoryContextAllocHuge(CurrentMemoryContext, sizeof(ViewRange) * n);
		ranges[nranges].buffer = buffer;
		ranges[nranges].offset = offset;
		ranges[nranges].size = size;
		ranges[nranges].row = (int32) i;
		nranges++;
	}
	if (nranges == 0)
		return;
	qsort(ranges, nranges, sizeof(ViewRange), range_cmp);
	for (i = 0; i < nranges; i = j)
	{
		const char *d = data[ranges[i].buffer];
		int64		end = (int64) ranges[i].offset + ranges[i].size;
		bool		ok;

		for (j = i + 1; j < nranges && ranges[j].buffer == ranges[i].buffer &&
			 ranges[j].offset <= end; j++)
			end = Max(end, (int64) ranges[j].offset + ranges[j].size);
		ok = utf8_valid(d + ranges[i].offset, end - ranges[i].offset);
		for (k = i; k < j; k++)
		{
			const ViewRange *v = &ranges[k];
			int64		e = (int64) v->offset + v->size;

			if (ok ? (utf8_continuation(d[v->offset]) || (e < end && utf8_continuation(d[e]))) :
				!utf8_valid(d + v->offset, v->size))
				ipc_malformed("Field \"%s\": row %d is not valid UTF-8.", FIELD(s), v->row);
		}
	}
	pfree(ranges);
}

/* One field's array, from its FieldNode and buffers, then its children's. */
static void
read_array(BatchRead *r, const struct ArrowSchema *s, struct ArrowArray *out, int depth)
{
	IpcType		t;
	int64		n,
				nulls,
				last = 0;
	const uint8 *validity = NULL;
	int64		i;

	schema_type(s, &t, depth);
	fb_vector_pair(&r->nodes, r->inode++, &n, &nulls);
	if (n < 0 || nulls < 0 || nulls > n)
		ipc_malformed("Field \"%s\": a FieldNode of %lld values, %lld of them NULL.",
					  FIELD(s), (long long) n, (long long) nulls);
	if (n > IPC_MAX_LENGTH)
		ipc_unsupported("Field \"%s\" has %lld values, more than %d.",
						FIELD(s), (long long) n, IPC_MAX_LENGTH);
	out->length = n;
	out->null_count = nulls;
	out->n_buffers = ipc_buffers(&t, true);
	out->buffers = palloc0(sizeof(void *) * 3);
	switch (t.id)
	{
		case IPC_NULL:
			out->null_count = n;
			break;
		case IPC_UNION:
			if (r->v4)
			{
				skip_buffer(r);
				if (nulls > 0)
					ipc_unsupported("Field \"%s\" is a union with NULLs of its own, as Arrow wrote them before 1.0.",
									FIELD(s));
			}
			out->null_count = 0;
			out->buffers[0] = take_buffer(r, n, s, "type ids");
			if (t.dense)
				out->buffers[1] = take_buffer(r, mul(n, 4, s), s, "offsets");
			break;
		default:
			validity = take_validity(r, n, nulls, s);
			out->buffers[0] = validity;
			switch (t.id)
			{
				case IPC_STRUCT:
				case IPC_FIXED_LIST:
					break;
				case IPC_BOOL:
					out->buffers[1] = take_buffer(r, (n + 7) / 8, s, "values");
					break;
				case IPC_BINARY:
				case IPC_UTF8:
				case IPC_LARGE_BINARY:
				case IPC_LARGE_UTF8:
					{
						const char *offsets = take_offsets(r, n, t.width, s, &last);
						const char *data = take_buffer(r, last, s, "data");

						out->buffers[1] = offsets;
						out->buffers[2] = data;
						if (t.id == IPC_UTF8 || t.id == IPC_LARGE_UTF8)
							for (i = 0; i < n; i++)
							{
								int64		a = offset_at(offsets, t.width, i);

								if (is_valid(validity, i) &&
									!utf8_valid(data + a, offset_at(offsets, t.width, i + 1) - a))
									ipc_malformed("Field \"%s\": row %lld is not valid UTF-8.",
												  FIELD(s), (long long) i);
							}
						break;
					}
				case IPC_BINARY_VIEW:
				case IPC_UTF8_VIEW:
					{
						const char *views = take_buffer(r, mul(n, 16, s), s, "views");
						int64		nvariadic = fb_vector_int(&r->variadic, r->ivariadic++, 8);
						int64	   *sizes = palloc(sizeof(int64) * Max(nvariadic, 1));

						out->n_buffers = 3 + nvariadic;
						out->buffers = repalloc0(out->buffers, sizeof(void *) * 3,
												 sizeof(void *) * out->n_buffers);
						out->buffers[0] = validity;
						out->buffers[1] = views;
						for (i = 0; i < nvariadic; i++)
						{
							int64		offset;

							next_buffer(r, &offset, &sizes[i]);
							out->buffers[2 + i] = buffer_at(r, offset, sizes[i], sizes[i], s, "variadic");
						}
						out->buffers[2 + nvariadic] = nvariadic ? sizes : empty;
						check_views(s, views, validity, n, (const char *const *) out->buffers + 2,
									sizes, nvariadic, t.id == IPC_UTF8_VIEW);
						break;
					}
				case IPC_LIST:
				case IPC_LARGE_LIST:
				case IPC_MAP:
					out->buffers[1] = take_offsets(r, n, t.width, s, &last);
					break;
				default:		/* fixed-width values */
					out->buffers[1] = take_buffer(r, mul(n, t.width, s), s, "values");
					break;
			}
			break;
	}

	out->n_children = s->n_children;
	out->children = palloc(sizeof(struct ArrowArray *) * Max(s->n_children, 1));
	for (i = 0; i < s->n_children; i++)
	{
		out->children[i] = palloc0(sizeof(struct ArrowArray));
		read_array(r, s->children[i], out->children[i], depth + 1);
	}

	/* what the children must hold */
	switch (t.id)
	{
		case IPC_LIST:
		case IPC_LARGE_LIST:
		case IPC_MAP:
			if (last > out->children[0]->length)
				ipc_malformed("Field \"%s\": its last offset, %lld, lies past its child's %lld values.",
							  FIELD(s), (long long) last, (long long) out->children[0]->length);
			break;
		case IPC_FIXED_LIST:
			if (mul(n, t.size, s) > out->children[0]->length)
				ipc_malformed("Field \"%s\" of %lld lists of %d has a child of %lld values.",
							  FIELD(s), (long long) n, t.size,
							  (long long) out->children[0]->length);
			break;
		case IPC_STRUCT:
		case IPC_UNION:
			for (i = 0; i < s->n_children; i++)
				if ((t.id == IPC_STRUCT || !t.dense) && out->children[i]->length < n)
					ipc_malformed("Field \"%s\" of %lld values has a child of %lld.",
								  FIELD(s), (long long) n, (long long) out->children[i]->length);
			if (t.id == IPC_UNION)
			{
				const int8 *types = out->buffers[0];
				const int32 *offsets = out->buffers[1];
				int8		child[128];
				int32		prev[128] = {0};

				memset(child, -1, sizeof(child));
				for (i = 0; i < t.nids; i++)
					child[t.ids[i]] = (int8) i;
				for (i = 0; i < n; i++)
				{
					int			id = types[i];

					if (id < 0 || child[id] < 0)
						ipc_malformed("Field \"%s\": row %lld has type id %d, which is not the union's.",
									  FIELD(s), (long long) i, id);
					/* a child's offsets do not decrease (Columnar.rst:885-888) */
					if (t.dense &&
						(offsets[i] < prev[id] || offsets[i] >= out->children[child[id]]->length))
						ipc_malformed("Field \"%s\": row %lld's offset, %d, is less than the last of its child's, or past its %lld values.",
									  FIELD(s), (long long) i, offsets[i],
									  (long long) out->children[child[id]]->length);
					if (t.dense)
						prev[id] = offsets[i];
				}
			}
			break;
		default:
			break;
	}
	out->release = release_array;
}

void
vexec_ipc_read_batch(const struct ArrowSchema *schema, const char *metadata, size_t len,
					 const char *body, size_t body_len, struct ArrowArray *out)
{
	VexecIpcHeader h;
	FbTable		rb;
	BatchRead	r;
	int			version;
	int64		length;
	int64		nfields = 0,
				nbuffers = 0,
				nviews = 0;
	int64		i;

	memset(out, 0, sizeof(*out));
	memset(&r, 0, sizeof(r));
	if (strcmp(schema->format, "+s") != 0)
		elog(ERROR, "vexec IPC reader: a record batch's schema of format \"%s\"", schema->format);
	read_message(metadata, len, &h, &rb, &version);
	if (h.kind != VEXEC_IPC_RECORD_BATCH)
		ipc_malformed("A Schema message where a RecordBatch message is expected.");
	if (fb_has(&rb, 3))
		ipc_unsupported("The message's body is compressed.");
	length = fb_int(&rb, 0, 8, 0);
	if (length < 0)
		ipc_malformed("A record batch of %lld rows.", (long long) length);
	if (length > IPC_MAX_LENGTH)
		ipc_unsupported("A record batch of %lld rows, more than %d.", (long long) length,
						IPC_MAX_LENGTH);
	if (h.body_len > (int64) body_len)
		ipc_malformed("The message's body has %lld bytes, and %zu are given.",
					  (long long) h.body_len, body_len);
	r.body = body;
	r.body_len = h.body_len;
	r.v4 = version == IPC_V4;
	if (!fb_vector(&rb, 1, 16, &r.nodes))
		r.nodes.n = 0;
	if (!fb_vector(&rb, 2, 16, &r.buffers))
		r.buffers.n = 0;
	if (!fb_vector(&rb, 4, 8, &r.variadic))
		r.variadic.n = 0;

	/* exactly the FieldNodes and Buffers the schema has */
	for (i = 0; i < schema->n_children; i++)
		count_fields(schema->children[i], r.v4, &nfields, &nbuffers, &nviews, 1);
	if (r.nodes.n != nfields || r.variadic.n != nviews)
		ipc_malformed("%u FieldNodes and %u variadic buffer counts, for a schema of %lld fields, %lld of them views.",
					  r.nodes.n, r.variadic.n, (long long) nfields, (long long) nviews);
	for (i = 0; i < r.variadic.n && nbuffers <= r.buffers.n; i++)
	{
		int64		count = fb_vector_int(&r.variadic, i, 8);

		if (count < 0 || count > r.buffers.n)
			ipc_malformed("A variadic buffer count of %lld.", (long long) count);
		nbuffers += count;
	}
	if (nbuffers != r.buffers.n)
		ipc_malformed("%u Buffers, for a schema whose fields have %lld.",
					  r.buffers.n, (long long) nbuffers);

	out->length = length;
	out->n_buffers = 1;
	out->buffers = palloc0(sizeof(void *));
	out->n_children = schema->n_children;
	out->children = palloc(sizeof(struct ArrowArray *) * Max(schema->n_children, 1));
	for (i = 0; i < schema->n_children; i++)
	{
		out->children[i] = palloc0(sizeof(struct ArrowArray));
		read_array(&r, schema->children[i], out->children[i], 1);
		if (out->children[i]->length != length)
			ipc_malformed("Field \"%s\" has %lld values, and its record batch %lld rows.",
						  FIELD(schema->children[i]), (long long) out->children[i]->length,
						  (long long) length);
	}
	out->release = release_array;
}
