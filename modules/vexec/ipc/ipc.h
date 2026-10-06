/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * ipc.h
 *	  Arrow IPC messages, written and read by vexec's own code
 *	  (pg_vector_executor.md §3.10, "No library"; §5, V7_0).
 *
 * The codec turns the C Data Interface's structures -- what V0's export
 * makes of a batch (batch/export.c), or what a caller builds itself -- into
 * Arrow IPC's encapsulated messages, and messages back into those
 * structures (arrow/docs/source/format/Columnar.rst:1191-1309, 1470-1493).
 * The flatbuffer tables are those of Arrow's format/Message.fbs and
 * format/Schema.fbs, written and read here with no flatbuffers library
 * (flatbuf.h).  V7 sends batches across Motions with it, and V10's egress
 * API sends results to clients and reads their parameters.
 *
 * Writing.  A message goes out as pieces: the encapsulation's prefix and
 * the flatbuffer metadata, which the writer makes in memory of its own, and
 * the body's buffers where the arrays hold them, each followed by the zero
 * bytes that pad it to a multiple of 8.  Nothing of the arrays is copied, so
 * they must outlive the message's use; the message itself lives until the
 * writer's next call.  The writer trusts its input, which is vexec's own:
 * a format it does not know, or an array at a non-zero offset, is an
 * internal error.
 *
 * Reading.  A message comes from another process, or from a client, so
 * everything in it is checked before it is used: the flatbuffer's offsets
 * within its bytes, each buffer within the body, every buffer long enough
 * for its field's length, offsets, views, list and union offsets and type
 * ids within their bounds, and UTF-8 in utf8 columns, offsets and views
 * alike ("Unicode with UTF-8 encoding", arrow/format/Schema.fbs:170).  A
 * malformed message fails with ERRCODE_INVALID_BINARY_REPRESENTATION, and
 * nothing is read outside its bytes.  An array of more than 2^31 - 1 values
 * is refused as not supported (Columnar.rst:300-305).  The arrays point into
 * the body where a buffer's address is a multiple of 8, and into a copy where
 * it is not; every buffer may be read in whole 8-byte words, as vexec reads
 * bitmaps.  A consumer reads values wider than 8 bytes, decimals and
 * month_day_nano intervals, with memcpy.  The structures are palloc'd in the
 * memory context current at the call, and their release callbacks only mark
 * them released: the context frees them, and the caller keeps the body alive
 * as long as the arrays.
 *
 * Types, by the C Data Interface's format strings
 * (arrow/docs/source/format/CDataInterface.rst:95-250): null; bool;
 * signed and unsigned integers of 8 to 64 bits; float16, float32,
 * float64; decimal32, decimal64, decimal128 and decimal256; fixed-size
 * binary; binary and utf8 with 32- or 64-bit offsets, and as views; date32
 * and date64; time32 and time64; timestamps with any unit and time zone;
 * durations; the three intervals; list and large list; fixed-size list;
 * struct; map; dense and sparse unions.  Each field's name, nullability and
 * metadata go across, extension types with them (ARROW:extension:name and
 * ARROW:extension:metadata, Columnar.rst:1685-1712), and the schema's own
 * metadata.  Not handled, in writing or reading: dictionaries, run-end
 * encoding, list views, and compressed bodies; neither V7 nor V10 sends
 * them, and a stream that holds one is refused with
 * ERRCODE_FEATURE_NOT_SUPPORTED.  So is a stream of the other endianness
 * than the server's (the writer writes the server's), and metadata before
 * V4; the writer writes V5.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_IPC_H
#define VEXEC_IPC_H

#include "lib/stringinfo.h"
#include "utils/memutils.h"

#include "batch/arrow_abi.h"

#include "vexec.h"

/* A message's kinds: MessageHeader's values (Message.fbs). */
typedef enum VexecIpcKind
{
	VEXEC_IPC_END = 0,			/* the end-of-stream marker */
	VEXEC_IPC_SCHEMA = 1,
	VEXEC_IPC_DICTIONARY_BATCH = 2, /* refused when read: never returned */
	VEXEC_IPC_RECORD_BATCH = 3
} VexecIpcKind;

/* A piece of a message: bytes written as they are. */
typedef struct VexecIpcPiece
{
	const void *data;
	size_t		len;
} VexecIpcPiece;

/*
 * A message as the writer makes it.  The prefix's 8 bytes -- 0xFFFFFFFF, then
 * metadata_len as a little-endian int32 -- lie just before the metadata, so
 * that prefix .. metadata + metadata_len is contiguous.  A Flight client
 * takes the metadata alone as FlightData's data_header, and the body as its
 * data_body (arrow/format/Flight.proto); a stream takes all three.
 */
typedef struct VexecIpcMessage
{
	int			kind;			/* VexecIpcKind */
	const char *prefix;			/* the encapsulation's 8 bytes */
	const char *metadata;		/* the flatbuffer Message, zero-padded */
	size_t		metadata_len;	/* a multiple of 8 */
	const VexecIpcPiece *pieces;	/* the body: buffers and their padding */
	int			npieces;
	int64		body_len;		/* the pieces' bytes: a multiple of 8 */
	int64		nrows;			/* RECORD_BATCH: the batch's rows */
} VexecIpcMessage;

/* The end-of-stream marker (Columnar.rst:1503-1508). */
#define VEXEC_IPC_EOS_LEN	8
extern VEXEC_API const char vexec_ipc_eos[VEXEC_IPC_EOS_LEN];

/* write.c */
typedef struct VexecIpcWriter VexecIpcWriter;

/* A writer, in a memory context of its own under parent. */
extern VEXEC_API VexecIpcWriter *vexec_ipc_writer_create(MemoryContext parent);
extern VEXEC_API void vexec_ipc_writer_free(VexecIpcWriter *writer);

/*
 * The Schema message of a record batch's schema: a struct ("+s") whose
 * children are the fields, its own metadata the schema's.
 */
extern VEXEC_API const VexecIpcMessage *vexec_ipc_write_schema(VexecIpcWriter *writer,
															   const struct ArrowSchema *schema);

/*
 * A RecordBatch message of a struct array, one child a field, at offset 0
 * and with no NULL of its own, under the schema of the stream's Schema
 * message.  A null_count of -1 is counted.
 */
extern VEXEC_API const VexecIpcMessage *vexec_ipc_write_batch(VexecIpcWriter *writer,
															  const struct ArrowSchema *schema,
															  const struct ArrowArray *array);

/* A message's bytes, appended: the prefix too when encapsulated. */
extern VEXEC_API void vexec_ipc_append(StringInfo out, const VexecIpcMessage *msg,
									   bool encapsulated);

/* read.c */

/* What a message's prefix and metadata say. */
typedef struct VexecIpcHeader
{
	int			kind;			/* VexecIpcKind */
	const char *metadata;		/* the flatbuffer Message */
	size_t		metadata_len;
	int64		body_len;		/* the body after the metadata */
} VexecIpcHeader;

/*
 * The encapsulated message at the start of data[0..len): false if len does
 * not yet hold its prefix and metadata, *consumed then the bytes it needs
 * to know more; true with *header filled and *consumed the prefix's and the
 * metadata's bytes, the body following them.  The end-of-stream marker,
 * and the end of data, read as VEXEC_IPC_END.  The format before 0.15,
 * without the continuation marker, is read too.
 */
extern VEXEC_API bool vexec_ipc_read_prefix(const char *data, size_t len,
											VexecIpcHeader *header, size_t *consumed);

/*
 * A flatbuffer Message alone, as Flight's data_header holds it, checked as
 * far as its kind and body length: a Schema or a RecordBatch.  A
 * DictionaryBatch, a Tensor or a SparseTensor is refused with
 * ERRCODE_FEATURE_NOT_SUPPORTED.
 */
extern VEXEC_API void vexec_ipc_read_message(const char *metadata, size_t len,
											 VexecIpcHeader *header);

/*
 * A Schema message's metadata, to the record batch's schema: a struct
 * ("+s") whose children are the fields.
 */
extern VEXEC_API void vexec_ipc_read_schema(const char *metadata, size_t len,
											struct ArrowSchema *out);

/*
 * A RecordBatch message's metadata and body, under the stream's schema, to
 * a struct array of the fields, checked as above.
 */
extern VEXEC_API void vexec_ipc_read_batch(const struct ArrowSchema *schema,
										   const char *metadata, size_t len,
										   const char *body, size_t body_len,
										   struct ArrowArray *out);

#endif							/* VEXEC_IPC_H */
