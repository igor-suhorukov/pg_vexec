/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * flatbuf.c
 *	  A flatbuffer builder and a checked reader, for Arrow IPC's tables
 *	  alone (pg_vector_executor.md §3.10, "No library"), and Arrow's format
 *	  strings (arrow/docs/source/format/CDataInterface.rst:95-250) parsed
 *	  into the parameters of Schema.fbs's type tables
 *	  (arrow/format/Schema.fbs:85-444).
 *
 * flatbuf.h describes the binary format.  The builder needs no deduplicated
 * vtables or shared strings; it writes every scalar it is given, defaults
 * included.  The reader reads every scalar by bytes, so an offset at any
 * alignment is safe to follow; it checks that each object lies within the
 * buffer, and the caller checks what the values mean.  Every position it
 * computes is a sum of a few 32-bit offsets and lengths, or a 32-bit count
 * times an element's width, computed in 64 bits, where none can overflow.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "ipc/flatbuf.h"

/* ---- building ---- */

void
fb_init(FbBuilder *b)
{
	memset(b, 0, sizeof(*b));
	b->cap = 1024;
	b->buf = palloc(b->cap);
	b->minalign = 4;
}

/* Room for n more bytes before what is written. */
static void
fb_reserve(FbBuilder *b, size_t n)
{
	size_t		cap = b->cap;
	char	   *buf;

	if (cap - b->size >= n)
		return;
	while (cap - b->size < n)
		cap *= 2;
	if (cap > (size_t) PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("Arrow IPC metadata of more than %d bytes", PG_INT32_MAX)));
	buf = palloc(cap);
	memcpy(buf + cap - b->size, b->buf + b->cap - b->size, b->size);
	pfree(b->buf);
	b->buf = buf;
	b->cap = cap;
}

static inline char *
fb_head(FbBuilder *b)
{
	return b->buf + b->cap - b->size;
}

static void
fb_put(FbBuilder *b, const void *p, size_t n)
{
	fb_reserve(b, n);
	b->size += n;
	if (p)
		memcpy(fb_head(b), p, n);
	else
		memset(fb_head(b), 0, n);
}

static void
fb_put_le(FbBuilder *b, uint64 v, int width)
{
	uint8		bytes[8];
	int			i;

	for (i = 0; i < width; i++)
		bytes[i] = (uint8) (v >> (8 * i));
	fb_put(b, bytes, width);
}

/* Pad so that len more bytes end aligned to align, counted from the end. */
static void
fb_prep(FbBuilder *b, size_t align, size_t len)
{
	if (align > b->minalign)
		b->minalign = align;
	fb_put(b, NULL, (align - (b->size + len) % align) % align);
}

/* The uint32 offset, from where it is about to be written, to an object. */
static uint32
fb_refer(FbBuilder *b, uint32 object)
{
	return (uint32) (b->size + 4 - object);
}

uint32
fb_string(FbBuilder *b, const char *s, size_t len)
{
	fb_prep(b, 4, len + 1);
	fb_put(b, NULL, 1);
	fb_put(b, s, len);
	fb_put_le(b, len, 4);
	return (uint32) b->size;
}

/* A vector of tables, or strings. */
uint32
fb_offsets(FbBuilder *b, const uint32 *offsets, int n)
{
	int			i;

	fb_prep(b, 4, (size_t) n * 4);
	for (i = n - 1; i >= 0; i--)
		fb_put_le(b, fb_refer(b, offsets[i]), 4);
	fb_put_le(b, n, 4);
	return (uint32) b->size;
}

/* A vector of n integers of width bytes, in the host's order. */
uint32
fb_ints(FbBuilder *b, const void *values, int n, int width)
{
	int			i;

	fb_prep(b, Max(width, 4), (size_t) n * width);
	for (i = n - 1; i >= 0; i--)
	{
		int64		v;

		if (width == 8)
			memcpy(&v, (const char *) values + (size_t) i * 8, 8);
		else
			v = ((const int32 *) values)[i];
		fb_put_le(b, (uint64) v, width);
	}
	fb_put_le(b, n, 4);
	return (uint32) b->size;
}

/* A vector of n structs, each of per int64 fields: FieldNode, Buffer. */
uint32
fb_structs64(FbBuilder *b, const int64 *values, int n, int per)
{
	int			i;

	fb_prep(b, 8, (size_t) n * per * 8);
	for (i = n * per - 1; i >= 0; i--)
		fb_put_le(b, (uint64) values[i], 8);
	fb_put_le(b, n, 4);
	return (uint32) b->size;
}

void
fb_start(FbBuilder *b)
{
	memset(b->fields, 0, sizeof(b->fields));
	b->nfields = 0;
	b->table_start = b->size;
}

static void
fb_field(FbBuilder *b, int id)
{
	Assert(id < FB_MAX_FIELDS);
	b->fields[id] = (uint32) b->size;
	b->nfields = Max(b->nfields, id + 1);
}

void
fb_add_int(FbBuilder *b, int id, int64 value, int width)
{
	fb_prep(b, width, width);
	fb_put_le(b, (uint64) value, width);
	fb_field(b, id);
}

void
fb_add_offset(FbBuilder *b, int id, uint32 object)
{
	fb_prep(b, 4, 4);
	fb_put_le(b, fb_refer(b, object), 4);
	fb_field(b, id);
}

/*
 * The table's int32, then its vtable just before it: each field's position
 * from the table's start, the table's size, the vtable's.
 */
uint32
fb_end(FbBuilder *b)
{
	uint32		table;
	int32		to_vtable;
	int			i;

	fb_prep(b, 4, 4);
	fb_put(b, NULL, 4);
	table = (uint32) b->size;
	if (table - b->table_start > PG_UINT16_MAX)
		elog(ERROR, "vexec IPC writer: a table of %zu bytes", table - b->table_start);
	for (i = b->nfields - 1; i >= 0; i--)
		fb_put_le(b, b->fields[i] ? table - b->fields[i] : 0, 2);
	fb_put_le(b, table - b->table_start, 2);
	fb_put_le(b, 4 + 2 * b->nfields, 2);
	to_vtable = (int32) (b->size - table);
	for (i = 0; i < 4; i++)
		b->buf[b->cap - table + i] = (char) ((uint32) to_vtable >> (8 * i));
	return table;
}

/* The root's offset first; the length a multiple of the largest alignment. */
const char *
fb_finish(FbBuilder *b, uint32 root, size_t *len)
{
	fb_prep(b, b->minalign, 4);
	fb_put_le(b, fb_refer(b, root), 4);
	*len = b->size;
	return fb_head(b);
}

/* ---- reading ---- */

static inline uint64
rd_le(const uint8 *p, int width)
{
	uint64		v = 0;
	int			i;

	for (i = width - 1; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

/* A signed integer of width bytes. */
static inline int64
rd_int(const uint8 *p, int width)
{
	uint64		v = rd_le(p, width);

	switch (width)
	{
		case 1:
			return (int8) v;
		case 2:
			return (int16) v;
		case 4:
			return (int32) v;
		default:
			return (int64) v;
	}
}

/* The table at pos: it, and its vtable, within the buffer. */
static void
fb_table_at(const uint8 *buf, size_t len, uint64 pos, FbTable *t)
{
	int64		vt;

	if (pos + 4 > len)
		ipc_malformed("A flatbuffer table at %llu lies past its %zu bytes.",
					  (unsigned long long) pos, len);
	vt = (int64) pos - rd_int(buf + pos, 4);
	if (vt < 0 || (uint64) vt + 4 > len)
		ipc_malformed("A flatbuffer vtable at %lld lies outside its %zu bytes.",
					  (long long) vt, len);
	t->buf = buf;
	t->len = len;
	t->pos = pos;
	t->vt = vt;
	t->vtlen = (uint16) rd_le(buf + vt, 2);
	t->tlen = (uint16) rd_le(buf + vt + 2, 2);
	if (t->vtlen < 4 || t->vtlen % 2 != 0 || (uint64) vt + t->vtlen > len)
		ipc_malformed("A flatbuffer vtable at %lld of %u bytes does not fit its %zu bytes.",
					  (long long) vt, t->vtlen, len);
	if (t->tlen < 4 || pos + t->tlen > len)
		ipc_malformed("A flatbuffer table at %llu of %u bytes does not fit its %zu bytes.",
					  (unsigned long long) pos, t->tlen, len);
}

void
fb_root(const char *buf, size_t len, FbTable *t)
{
	if (len < 8)
		ipc_malformed("A flatbuffer of %zu bytes.", len);
	fb_table_at((const uint8 *) buf, len, rd_le((const uint8 *) buf, 4), t);
}

/* Where a field of width bytes lies, or 0 when it is absent. */
static size_t
fb_field_pos(const FbTable *t, int id, int width)
{
	uint16		at;

	if (4 + 2 * id + 2 > t->vtlen)
		return 0;
	at = (uint16) rd_le(t->buf + t->vt + 4 + 2 * id, 2);
	if (at == 0)
		return 0;
	if (at < 4 || at + width > t->tlen)
		ipc_malformed("Field %d of a flatbuffer table at %zu lies outside the table's %u bytes.",
					  id, t->pos, t->tlen);
	return t->pos + at;
}

bool
fb_has(const FbTable *t, int id)
{
	return fb_field_pos(t, id, 1) != 0;
}

int64
fb_int(const FbTable *t, int id, int width, int64 dflt)
{
	size_t		p = fb_field_pos(t, id, width);

	return p ? rd_int(t->buf + p, width) : dflt;
}

/* What an offset field refers to: an object with at least 4 bytes. */
static bool
fb_target(const FbTable *t, int id, uint64 *target)
{
	size_t		p = fb_field_pos(t, id, 4);

	if (p == 0)
		return false;
	*target = p + rd_le(t->buf + p, 4);
	if (*target + 4 > t->len)
		ipc_malformed("Field %d of a flatbuffer table at %zu refers past its %zu bytes.",
					  id, t->pos, t->len);
	return true;
}

bool
fb_table(const FbTable *t, int id, FbTable *sub)
{
	uint64		target;

	if (!fb_target(t, id, &target))
		return false;
	fb_table_at(t->buf, t->len, target, sub);
	return true;
}

bool
fb_string_at(const FbTable *t, int id, const char **s, uint32 *len)
{
	uint64		target;

	if (!fb_target(t, id, &target))
		return false;
	*len = (uint32) rd_le(t->buf + target, 4);
	if (target + 4 + *len > t->len)
		ipc_malformed("A flatbuffer string of %u bytes at %llu lies past its %zu bytes.",
					  *len, (unsigned long long) target, t->len);
	*s = (const char *) t->buf + target + 4;
	return true;
}

bool
fb_vector(const FbTable *t, int id, int width, FbVector *v)
{
	uint64		target;

	if (!fb_target(t, id, &target))
		return false;
	v->buf = t->buf;
	v->len = t->len;
	v->n = (uint32) rd_le(t->buf + target, 4);
	v->pos = target + 4;
	if (v->pos + (uint64) v->n * width > t->len)
		ipc_malformed("A flatbuffer vector of %u elements at %llu lies past its %zu bytes.",
					  v->n, (unsigned long long) target, t->len);
	return true;
}

void
fb_vector_table(const FbVector *v, uint32 i, FbTable *sub)
{
	size_t		p = v->pos + (size_t) i * 4;

	Assert(i < v->n);
	fb_table_at(v->buf, v->len, p + rd_le(v->buf + p, 4), sub);
}

int64
fb_vector_int(const FbVector *v, uint32 i, int width)
{
	Assert(i < v->n);
	return rd_int(v->buf + v->pos + (size_t) i * width, width);
}

/* Element i of a vector of structs of two int64s: FieldNode, Buffer. */
void
fb_vector_pair(const FbVector *v, uint32 i, int64 *a, int64 *b)
{
	const uint8 *p = v->buf + v->pos + (size_t) i * 16;

	Assert(i < v->n);
	*a = rd_int(p, 8);
	*b = rd_int(p + 8, 8);
}

/* ---- format strings ---- */

/* A decimal integer at *p, in [min, max]; *p moves past it. */
static bool
parse_int(const char **p, int32 min, int32 max, int32 *out)
{
	const char *s = *p;
	bool		neg = (*s == '-');
	int64		v = 0;

	if (neg)
		s++;
	if (*s < '0' || *s > '9')
		return false;
	while (*s >= '0' && *s <= '9')
	{
		v = v * 10 + (*s++ - '0');
		if (v > (int64) PG_INT32_MAX + 1)
			return false;
	}
	if (neg)
		v = -v;
	if (v < min || v > max)
		return false;
	*out = (int32) v;
	*p = s;
	return true;
}

static int
unit_of(char c, const char *units)
{
	const char *u = c ? strchr(units, c) : NULL;

	return u ? (int) (u - units) : -1;
}

/* A union's type ids, "I,J,...": each in [0, 127], none twice. */
static bool
parse_ids(const char *p, IpcType *t)
{
	int			i;

	while (*p != '\0')
	{
		int32		id;

		if (t->nids == 128 || !parse_int(&p, 0, 127, &id) || (*p != ',' && *p != '\0'))
			return false;
		for (i = 0; i < t->nids; i++)
			if (t->ids[i] == id)
				return false;
		t->ids[t->nids++] = (int8) id;
		if (*p == ',' && *++p == '\0')
			return false;
	}
	return true;
}

/*
 * A format string's type: false for one that is not Arrow's.  The types
 * the codec refuses -- run-end encoding, list views -- parse, so that the
 * caller names them.
 */
bool
ipc_parse_format(const char *f, IpcType *t)
{
	const char *p;

	memset(t, 0, offsetof(IpcType, ids));
	t->tz = "";
	if (f[0] != '\0' && f[1] == '\0')
	{
		static const char ints[] = "cCsSiIlL";
		const char *in = strchr(ints, f[0]);

		if (in)
		{
			t->id = IPC_INT;
			t->width = 1 << ((in - ints) / 2);
			t->bits = t->width * 8;
			t->is_signed = (in - ints) % 2 == 0;
			return true;
		}
		switch (f[0])
		{
			case 'n':
				t->id = IPC_NULL;
				return true;
			case 'b':
				t->id = IPC_BOOL;
				return true;
			case 'e':
			case 'f':
			case 'g':
				t->id = IPC_FLOAT;
				t->unit = f[0] - 'e';
				t->width = 2 << t->unit;
				return true;
			case 'z':
			case 'u':
			case 'Z':
			case 'U':
				t->id = f[0] == 'z' ? IPC_BINARY : f[0] == 'u' ? IPC_UTF8 :
					f[0] == 'Z' ? IPC_LARGE_BINARY : IPC_LARGE_UTF8;
				t->width = (f[0] == 'z' || f[0] == 'u') ? 4 : 8;
				return true;
		}
		return false;
	}
	if (strcmp(f, "vz") == 0 || strcmp(f, "vu") == 0)
	{
		t->id = f[1] == 'z' ? IPC_BINARY_VIEW : IPC_UTF8_VIEW;
		t->width = 16;
		return true;
	}
	if (f[0] == 'd' && f[1] == ':')
	{
		p = f + 2;
		t->id = IPC_DECIMAL;
		t->bits = 128;
		if (!parse_int(&p, 1, 76, &t->precision) || *p++ != ',' ||
			!parse_int(&p, PG_INT32_MIN, PG_INT32_MAX, &t->scale))
			return false;
		if (*p == ',' && (p++, !parse_int(&p, 32, 256, &t->bits)))
			return false;
		if (*p != '\0' ||
			(t->bits != 32 && t->bits != 64 && t->bits != 128 && t->bits != 256))
			return false;
		t->width = t->bits / 8;
		return true;
	}
	if ((f[0] == 'w' && f[1] == ':') || (f[0] == '+' && f[1] == 'w' && f[2] == ':'))
	{
		p = f + (f[0] == 'w' ? 2 : 3);
		t->id = f[0] == 'w' ? IPC_FIXED_BINARY : IPC_FIXED_LIST;
		if (!parse_int(&p, 0, PG_INT32_MAX, &t->size) || *p != '\0')
			return false;
		t->width = f[0] == 'w' ? t->size : 0;
		return true;
	}
	if (f[0] == 't' && f[1] != '\0')
	{
		static const char kinds[] = "dtDis";
		static const char *const units[] = {"Dm", "smun", "smun", "MDn", "smun"};
		static const IpcTypeId ids[] = {IPC_DATE, IPC_TIME, IPC_DURATION, IPC_INTERVAL,
		IPC_TIMESTAMP};
		const char *kind = strchr(kinds, f[1]);

		if (kind == NULL || (t->unit = unit_of(f[2], units[kind - kinds])) < 0)
			return false;
		t->id = ids[kind - kinds];
		switch (t->id)
		{
			case IPC_DATE:
				t->width = t->unit == 0 ? 4 : 8;
				break;
			case IPC_TIME:
				t->width = t->unit < 2 ? 4 : 8;
				t->bits = t->width * 8;
				break;
			case IPC_INTERVAL:
				t->width = 4 << t->unit;
				break;
			default:
				t->width = 8;
				break;
		}
		if (t->id == IPC_TIMESTAMP)
		{
			t->tz = f + 4;
			return f[3] == ':';
		}
		return f[3] == '\0';
	}
	if (f[0] != '+')
		return false;
	if (strcmp(f, "+l") == 0 || strcmp(f, "+L") == 0 || strcmp(f, "+m") == 0)
	{
		t->id = f[1] == 'l' ? IPC_LIST : f[1] == 'L' ? IPC_LARGE_LIST : IPC_MAP;
		t->width = f[1] == 'L' ? 8 : 4;
		return true;
	}
	if (strcmp(f, "+s") == 0 || strcmp(f, "+r") == 0)
	{
		t->id = f[1] == 's' ? IPC_STRUCT : IPC_RUN_END;
		return true;
	}
	if (strcmp(f, "+vl") == 0 || strcmp(f, "+vL") == 0)
	{
		t->id = f[2] == 'l' ? IPC_LIST_VIEW : IPC_LARGE_LIST_VIEW;
		return true;
	}
	if (f[1] == 'u' && (f[2] == 'd' || f[2] == 's') && f[3] == ':')
	{
		t->id = IPC_UNION;
		t->dense = f[2] == 'd';
		return parse_ids(f + 4, t);
	}
	return false;
}

/*
 * The buffers of an array of a type (Columnar.rst:1172-1187), in the C Data
 * Interface or in IPC: for views without their variadic buffers, the
 * interface's sizes buffer counted; for unions as V5 has them.
 */
int
ipc_buffers(const IpcType *t, bool c_data)
{
	switch (t->id)
	{
		case IPC_NULL:
			return 0;
		case IPC_STRUCT:
		case IPC_FIXED_LIST:
			return 1;
		case IPC_UNION:
			return t->dense ? 2 : 1;
		case IPC_BINARY:
		case IPC_UTF8:
		case IPC_LARGE_BINARY:
		case IPC_LARGE_UTF8:
			return 3;
		case IPC_BINARY_VIEW:
		case IPC_UTF8_VIEW:
			return c_data ? 3 : 2;
		default:
			return 2;
	}
}

