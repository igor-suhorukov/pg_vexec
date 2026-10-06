/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * flatbuf.h
 *	  The flatbuffers of Arrow IPC messages, built and read by vexec's own
 *	  code (pg_vector_executor.md §3.10, "No library"), and what the IPC
 *	  writer and reader share: Arrow's types, by their C Data Interface
 *	  format strings.
 *
 * A flatbuffer, as Arrow's messages use it, is little-endian throughout.
 * It starts with a uint32 offset to its root table.  A table starts with an
 * int32, which subtracted from the table's position gives its vtable: a
 * uint16 of the vtable's bytes, a uint16 of the table's, then a uint16 per
 * field id, that field's position within the table, 0 when the field is
 * absent and takes its default.  A field that refers to a table, a string
 * or a vector holds a uint32 offset forward from where it lies.  A string
 * is a uint32 length, its bytes and a NUL; a vector a uint32 count and its
 * elements, scalars and structs inline, tables as offsets.  A union is two
 * fields, its type's id, a uint8, and the table.
 *
 * The builder writes as flatbuffers' own does, from the end of its buffer
 * towards its start, so that whatever an object refers to is written before
 * it; alignment is counted from the end, and the finished buffer's length
 * is a multiple of the largest alignment used.  The reader trusts nothing:
 * every offset, vtable, string and vector is checked against the buffer's
 * length before it is read, and is read by bytes, whatever its alignment.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_IPC_FLATBUF_H
#define VEXEC_IPC_FLATBUF_H

/* The two ways input fails: malformed, or not supported. */
#define ipc_malformed(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION), \
			 errmsg("malformed Arrow IPC message"), \
			 errdetail(__VA_ARGS__)))
#define ipc_unsupported(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), \
			 errmsg("unsupported Arrow IPC message"), \
			 errdetail(__VA_ARGS__)))

/* How deep fields may nest, as Arrow's own C++ reader allows. */
#define IPC_MAX_DEPTH		64

/* The largest field id of a table the codec writes: Field's custom_metadata. */
#define FB_MAX_FIELDS		8

/* A flatbuffer being built. */
typedef struct FbBuilder
{
	char	   *buf;			/* cap bytes; what is written is at its end */
	size_t		cap;
	size_t		size;			/* bytes written */
	size_t		minalign;		/* the largest alignment used */
	/* the table being built */
	uint32		fields[FB_MAX_FIELDS];	/* each field's position, counted
										 * from the end; 0: absent */
	int			nfields;
	size_t		table_start;
} FbBuilder;

/*
 * Objects are named by their position counted from the buffer's end, as
 * the builder returns it.
 */
extern void fb_init(FbBuilder *b);
extern uint32 fb_string(FbBuilder *b, const char *s, size_t len);
extern uint32 fb_offsets(FbBuilder *b, const uint32 *offsets, int n);
extern uint32 fb_ints(FbBuilder *b, const void *values, int n, int width);
extern uint32 fb_structs64(FbBuilder *b, const int64 *values, int n, int per);
extern void fb_start(FbBuilder *b);
extern void fb_add_int(FbBuilder *b, int id, int64 value, int width);
extern void fb_add_offset(FbBuilder *b, int id, uint32 object);
extern uint32 fb_end(FbBuilder *b);
extern const char *fb_finish(FbBuilder *b, uint32 root, size_t *len);

/* A table, or a vector, of a flatbuffer being read: checked when found. */
typedef struct FbTable
{
	const uint8 *buf;
	size_t		len;
	size_t		pos;			/* the table */
	size_t		vt;				/* its vtable */
	uint16		vtlen;
	uint16		tlen;
} FbTable;

typedef struct FbVector
{
	const uint8 *buf;
	size_t		len;
	size_t		pos;			/* the first element */
	uint32		n;
} FbVector;

extern void fb_root(const char *buf, size_t len, FbTable *t);
extern bool fb_has(const FbTable *t, int id);
extern int64 fb_int(const FbTable *t, int id, int width, int64 dflt);
extern bool fb_table(const FbTable *t, int id, FbTable *sub);
extern bool fb_string_at(const FbTable *t, int id, const char **s, uint32 *len);
extern bool fb_vector(const FbTable *t, int id, int width, FbVector *v);
extern void fb_vector_table(const FbVector *v, uint32 i, FbTable *sub);
extern int64 fb_vector_int(const FbVector *v, uint32 i, int width);
extern void fb_vector_pair(const FbVector *v, uint32 i, int64 *a, int64 *b);

/* Message.fbs:152-154, MessageHeader; Schema.fbs:31-52, MetadataVersion */
#define IPC_HEADER_SCHEMA			1
#define IPC_HEADER_DICTIONARY		2
#define IPC_HEADER_RECORD_BATCH		3
#define IPC_V4						3
#define IPC_V5						4

/* Schema.fbs:450-477: the Type union's members, by their ids */
typedef enum IpcTypeId
{
	IPC_NONE,
	IPC_NULL,
	IPC_INT,
	IPC_FLOAT,
	IPC_BINARY,
	IPC_UTF8,
	IPC_BOOL,
	IPC_DECIMAL,
	IPC_DATE,
	IPC_TIME,
	IPC_TIMESTAMP,
	IPC_INTERVAL,
	IPC_LIST,
	IPC_STRUCT,
	IPC_UNION,
	IPC_FIXED_BINARY,
	IPC_FIXED_LIST,
	IPC_MAP,
	IPC_DURATION,
	IPC_LARGE_BINARY,
	IPC_LARGE_UTF8,
	IPC_LARGE_LIST,
	IPC_RUN_END,
	IPC_BINARY_VIEW,
	IPC_UTF8_VIEW,
	IPC_LIST_VIEW,
	IPC_LARGE_LIST_VIEW
} IpcTypeId;

/*
 * A type, as its format string gives it (CDataInterface.rst:95-250), with
 * the parameters of its table in Schema.fbs.
 */
typedef struct IpcType
{
	int			id;				/* IpcTypeId */
	int			width;			/* bytes a value of a fixed-width type, or an
								 * offset of binary, utf8, list and map; 0
								 * for bool's bits and the rest */
	int			bits;			/* Int's, Decimal's and Time's bitWidth */
	bool		is_signed;		/* Int */
	int			unit;			/* FloatingPoint's precision; the date,
								 * time, duration and interval units */
	int32		precision;		/* Decimal */
	int32		scale;
	int32		size;			/* FixedSizeBinary's byteWidth,
								 * FixedSizeList's listSize */
	const char *tz;				/* Timestamp: its time zone, "" for none */
	bool		dense;			/* Union */
	int			nids;
	int8		ids[128];		/* Union: each child's type id */
} IpcType;

extern bool ipc_parse_format(const char *format, IpcType *t);
extern int	ipc_buffers(const IpcType *t, bool c_data);

#endif							/* VEXEC_IPC_FLATBUF_H */
