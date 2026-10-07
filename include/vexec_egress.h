/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_egress.h
 *	  The egress API: a statement's result as Arrow IPC messages, and a
 *	  client's parameter batches as values, for an extension that serves
 *	  Arrow to clients -- vexec_flight's Flight SQL endpoint
 *	  (pg_vector_executor.md §3.15, V10).
 *
 * vexec publishes it through a rendezvous variable, as it publishes the
 * batch-source registry (vexec_source.h): the extension finds it at run
 * time, and links nothing of vexec's.  Everything here is a type, a macro
 * or an inline function.
 *
 * Only while the vector executor is active.  The API refuses -- every entry
 * but active() raises ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE, naming
 * vexec.mode -- unless vexec.mode is auto or force in the session, read at
 * each call, so that role and database settings and a session's own SET
 * apply.  Without vexec there is no routine to find.
 *
 * Results.  receiver() gives a DestReceiver of vexec's own, which a caller
 * hands to PortalRun as it would any receiver.  It turns the statement's
 * result into one Schema message when the executor starts it, and a
 * RecordBatch message for each batch of up to 1,024 rows, each handed to
 * the caller's write function as soon as it is made.  Where a vector node
 * is at the top of the plan, the batches come from it, with no row formed;
 * otherwise the rows are gathered into batches.  Nothing is held but the
 * batch being sent, so a client that reads slowly holds the session's memory
 * at one batch.
 *
 * The Arrow types.  A column's type gives its Arrow type, so the stream's
 * schema is fixed before its first batch (§3.15):
 *
 *	bool						bool
 *	"char", int2, int4, int8	int8, int16, int32, int64
 *	oid, xid, cid				uint32
 *	xid8						uint64
 *	float4, float8				float32, float64
 *	numeric(p,s) that vexec		decimal128(p, s); numeric(p,-s)'s
 *	holds scaled, 38 digits		decimal128(p + s, 0), numeric(p,s>p)'s
 *	at most (§3.4.2)			decimal128(s, s)
 *	date						date32
 *	time						time64[us]
 *	timestamp, timestamptz		timestamp[us], timestamp[us, UTC]
 *	interval					month_day_nano
 *	uuid						fixed_size_binary(16), arrow.uuid
 *	text, varchar, bpchar,		utf8
 *	name
 *	bytea						binary
 *	json, jsonb					utf8, arrow.json
 *	any other type				utf8, its output function's text: the
 *								reg* types by name, numeric without a
 *								typmod or past 38 digits, arrays,
 *								composites, ranges ...
 *
 * and a domain its base type's.  Every field's metadata names the column's
 * PostgreSQL type, "pg_type" (its schema and name) and "pg_typmod", as
 * vexec's export does, and the caller's own keys follow.  A value its
 * column's Arrow type cannot hold fails the statement, naming the column:
 * a date, timestamp or interval at ±infinity, a timestamp whose shift to
 * Arrow's epoch leaves int64, time's 24:00:00, an interval whose time part
 * leaves int64 in nanoseconds, a NaN or infinity in a bounded numeric
 * (ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE, or ERRCODE_DATETIME_VALUE_OUT_OF_RANGE).
 * Text in a database whose encoding is not UTF8 is converted to UTF-8.
 *
 * Messages.  A message is handed out as pieces, the body's buffers where
 * they lie, which the write function sends as they are, valid until it
 * returns.  Flight's FlightData takes the metadata as its data_header and
 * the pieces as its data_body; a stream takes prefix, metadata and pieces.
 *
 * Parameters.  params_begin() takes the Schema message a client's parameter
 * stream starts with, and params_batch() each RecordBatch message, whose
 * rows it hands to the caller's row function as values of the parameters'
 * types.  The client's Arrow is checked as input (vexec's IPC reader): a
 * malformed message fails with ERRCODE_INVALID_BINARY_REPRESENTATION.  A
 * utf8 value is read by its parameter type's input function, as a
 * parameter in text format is; an int8 for a "char" parameter is its byte,
 * as "char" goes out; a value of any other Arrow type becomes the
 * PostgreSQL type its Arrow type names, and then the parameter's type by
 * assignment, as INSERT assigns values to columns.
 *
 * Ingest (minor version 1, §3.16).  ingest_begin() takes the Schema message
 * a client's stream of rows starts with, and a read function vexec calls for
 * each later message as a statement reads the stream: the client's rows go
 * into a table by "INSERT INTO t (...) SELECT ... FROM
 * vexec.ingest_stream(handle) AS s(...)", the handle ingest_handle()'s, in
 * the session that began it, and VecInsert writes them a batch of columns
 * at a time where the target allows it.  ingest_columns() gives each column
 * of the stream the PostgreSQL type its Arrow type names, as a parameter of
 * no type would read it; the column definition list may name those types,
 * which are read without copying where they can be, or any other the values
 * can be assigned to, as parameters are.  The stream is read once, by one
 * statement.  ingest_finished() says the read function has returned the
 * end; ingest_end() unregisters it.  A stream lives no longer than the
 * transaction it began in.
 *
 * Every call runs on the backend's main thread and may ereport.
 *
 * Versions: as vexec_source.h's.  The major version is in the rendezvous
 * name; minor additions go at the end of the routine, whose size says how
 * much of it the vexec that published it has.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_EGRESS_H
#define VEXEC_EGRESS_H

#include "access/tupdesc.h"
#include "fmgr.h"
#include "tcop/dest.h"

#define VEXEC_EGRESS_RENDEZVOUS	"vexec/egress_v1"	/* the major version is
													 * in the name */
#define VEXEC_EGRESS_MINOR		1	/* 1: ingest (§3.16) */
#define VEXEC_EGRESS_MAGIC		0x56584531	/* "VXE1" */

/* The Arrow C Data Interface's structures: a caller defines them itself. */
struct ArrowSchema;
struct ArrowArray;

/* A message's kinds: MessageHeader's values (arrow/format/Message.fbs). */
#define VEXEC_EGRESS_SCHEMA			1
#define VEXEC_EGRESS_RECORD_BATCH	3

/* A piece of a message: bytes written as they are. */
typedef struct VexecEgressPiece
{
	const void *data;
	size_t		len;
} VexecEgressPiece;

/*
 * An Arrow IPC message.  The prefix's 8 bytes -- 0xFFFFFFFF, then
 * metadata_len as a little-endian int32 -- lie just before the metadata in
 * memory.  body_len is the pieces' bytes, a multiple of 8.
 */
typedef struct VexecEgressMessage
{
	int			kind;			/* VEXEC_EGRESS_SCHEMA, VEXEC_EGRESS_RECORD_BATCH */
	const char *prefix;
	const char *metadata;		/* the flatbuffer Message, zero-padded */
	size_t		metadata_len;	/* a multiple of 8 */
	const VexecEgressPiece *pieces;
	int			npieces;
	int64		body_len;
	int64		nrows;			/* a RecordBatch's rows */
} VexecEgressMessage;

/* Takes a message: its pieces are valid until it returns.  It may ereport. */
typedef void (*VexecEgressWriteFn) (void *arg, const VexecEgressMessage *msg);

/* Takes a parameter row: its values are valid until it returns. */
typedef void (*VexecEgressRowFn) (void *arg, int64 rownum,
								  const Datum *values, const bool *isnull);

/*
 * Reads a client's next Arrow IPC message: its flatbuffer metadata and its
 * body, valid until the next call; false at the end of the stream.  It may
 * ereport.
 */
typedef bool (*VexecEgressReadFn) (void *arg, const char **metadata, size_t *metadata_len,
								   const char **body, size_t *body_len);

/* A column of a client's stream, and the PostgreSQL type its Arrow type names. */
typedef struct VexecIngestColumn
{
	const char *name;			/* the field's name */
	Oid			type;
	int32		typmod;
	bool		nullable;
} VexecIngestColumn;

/*
 * What a caller asks of a result's column, beside its type's own Arrow
 * type.  format, when set, converts the column: an integer column -- int2,
 * int4, int8, oid -- to any integer format ("c", "C", "s", "S", "i", "I",
 * "l", "L"), a value out of the format's range failing the statement.
 */
typedef struct VexecEgressField
{
	const char *name;			/* NULL: the column's own */
	bool		not_null;		/* the field is not nullable; a NULL in it
								 * fails the statement */
	const char *format;			/* NULL: the column type's own */
	int			nmetadata;		/* the caller's keys, after vexec's */
	const char *const *keys;
	const char *const *values;
	bool		bare;			/* without pg_type and pg_typmod, for a
								 * result whose schema a protocol fixes, as
								 * Flight SQL's catalog commands' */
} VexecEgressField;

typedef struct VexecEgressRoutine
{
	uint32		magic;			/* VEXEC_EGRESS_MAGIC */
	int			minor;			/* VEXEC_EGRESS_MINOR it was built with */
	Size		size;			/* later members read as absent below it */

	/* Whether the vector executor is active: vexec.mode at auto or force. */
	bool		(*active) (void);

	/*
	 * A receiver of a statement's result.  fields, when not NULL, has
	 * nfields entries, one per column of the result.  The receiver lives in
	 * the memory context current at the call; destroy it with its
	 * rDestroy.
	 */
	DestReceiver *(*receiver) (VexecEgressWriteFn write, void *arg,
							   const VexecEgressField *fields, int nfields);

	/* What a receiver has sent: rows, RecordBatch messages, bytes. */
	void		(*receiver_counts) (DestReceiver *receiver, int64 *rows,
									int64 *batches, int64 *bytes);

	/*
	 * A result's Schema message, from its tuple descriptor (a planned
	 * statement's resultDesc), as its receiver would send it.
	 */
	void		(*schema) (TupleDesc desc, const VexecEgressField *fields,
						   int nfields, VexecEgressWriteFn write, void *arg);

	/*
	 * Arrays a caller made itself, as a Schema message, when with_schema,
	 * and a RecordBatch message: for results no statement makes, such as
	 * Flight SQL's server information.  schema is a struct ("+s") of the
	 * fields, array a struct array of the columns at offset 0.
	 */
	void		(*arrays) (const struct ArrowSchema *schema,
						   const struct ArrowArray *array, bool with_schema,
						   VexecEgressWriteFn write, void *arg);

	/* A message's kind from its flatbuffer metadata, checked. */
	int			(*message_kind) (const char *metadata, size_t len);

	/*
	 * A client's parameter batches, for a statement whose nparams
	 * parameters have these types: begin with the stream's Schema message,
	 * whose fields must be as many; then each RecordBatch message, whose
	 * rows go to row in order, the number of them returned; then end.  The
	 * state lives in the memory context current at begin.
	 */
	void	   *(*params_begin) (const char *metadata, size_t len,
								 int nparams, const Oid *types,
								 const int32 *typmods);
	int64		(*params_batch) (void *state, const char *metadata, size_t len,
								 const char *body, size_t body_len,
								 VexecEgressRowFn row, void *arg);
	void		(*params_end) (void *state);

	/* minor 1: ingest (§3.16) */
	void	   *(*ingest_begin) (const char *metadata, size_t len,
								 VexecEgressReadFn read, void *arg);
	int			(*ingest_columns) (void *stream, const VexecIngestColumn **columns);
	int64		(*ingest_handle) (void *stream);
	int64		(*ingest_rows) (void *stream);
	bool		(*ingest_finished) (void *stream);
	void		(*ingest_end) (void *stream);
} VexecEgressRoutine;

/* The routine vexec published in this process, or NULL. */
static inline const VexecEgressRoutine *
vexec_egress_lookup(void)
{
	const VexecEgressRoutine **rv;

	rv = (const VexecEgressRoutine **) find_rendezvous_variable(VEXEC_EGRESS_RENDEZVOUS);
	if (*rv == NULL || (*rv)->magic != VEXEC_EGRESS_MAGIC)
		return NULL;
	return *rv;
}

/* Whether a routine has a member: it was built with a struct that long. */
#define VEXEC_EGRESS_HAS(routine, member) \
	((routine)->size >= offsetof(VexecEgressRoutine, member) + sizeof((routine)->member) && \
	 (routine)->member != NULL)

#endif							/* VEXEC_EGRESS_H */
