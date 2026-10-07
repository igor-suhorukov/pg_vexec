/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * frame.h
 *	  Batches across ORCA's Motions as Arrow IPC frames, and the vector
 *	  cdbhash that sends each row where its key belongs
 *	  (pg_vector_executor.md §3.10, "Batches between processes", V7).
 *
 * A frame is one value of a bytea column: vexec's own sixteen bytes -- a
 * magic number and the hash of the schema it is in -- then one Arrow IPC
 * encapsulated message (V7_0's codec, ipc/).  A Schema message's frame
 * carries the columns' shapes (batch/batch.h), each field's metadata
 * naming its layout and its type; a RecordBatch message's frame carries a
 * batch's rows in those shapes, its buffers as they are where the rows are
 * all the batch's, else the rows' values gathered:
 *
 *	fixed, scaled	fixed-size binary of the shape's stride
 *	byte bool		fixed-size binary of one byte
 *	bit bool		bool
 *	datum			binary whose values are whole varlenas, headers and all,
 *					a 4-byte header at the type's alignment -- a TOAST
 *					pointer fetched, an expanded object flattened, as
 *					gp_core's tuples carry them -- and C strings with their
 *					NUL; the receiver's Datums point into the frame
 *	view			binary views, the values longer than a view holds
 *					copied into one buffer of the frame's own
 *	offsets			binary
 *
 * So a frame is vexec's alone, never handed to another system, as the
 * Arrow specification allows (Columnar.rst:1755-1759).  A batch goes in
 * the shapes its columns have: a conversion may fail on a value
 * (convert.c), so the shapes of one stream's batches may differ.  Each
 * batch frame names its schema by the hash of the schema message, which
 * every sender of a Motion computes alike for the same shapes, and the
 * receiver -- which takes frames from all its senders mixed, with no word
 * of which one sent each -- keeps every schema it has been sent, a sender
 * sending one to each receiver before its first batch in it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_MOTION_FRAME_H
#define VEXEC_MOTION_FRAME_H

#include "fmgr.h"
#include "nodes/execnodes.h"

#include "vexec.h"
#include "batch/batch.h"

/* A frame's first four bytes: "VXF1". */
#define VEXEC_FRAME_MAGIC		0x31465856
#define VEXEC_FRAME_HEADER		16

/* What a sender keeps: its columns, and the schemas it has sent. */
typedef struct VexecFrameWriter VexecFrameWriter;

/* What a receiver keeps: its columns, and the schemas it has been sent. */
typedef struct VexecFrameReader VexecFrameReader;

/* frame.c, the sender's side */
extern VEXEC_API VexecFrameWriter *vexec_frame_writer_create(MemoryContext parent, int ncols,
															 VexecType *const *types);

/*
 * A batch to send, in one or more frames of its rows: its columns flat, in
 * the shapes they have, and the schema of those shapes -- its hash, and the
 * frame of its Schema message, the writer's own, which a receiver is to be
 * sent before the first frame in it.  The batch's columns are only read,
 * their encodings flattened in copies, until the next batch begins.
 */
extern VEXEC_API void vexec_frame_begin(VexecFrameWriter *w, VexecBatch *batch,
										uint64 *schema, bytea **schema_frame);

/*
 * The frame of the batch's rows rows[0 .. nrows), in that order, made in
 * CurrentMemoryContext; NULL where it would be longer than a bytea may be,
 * for the caller to send the rows in parts.
 */
extern VEXEC_API bytea *vexec_frame_rows(VexecFrameWriter *w, const int *rows, int nrows);

/* frame.c, the receiver's side */
extern VEXEC_API VexecFrameReader *vexec_frame_reader_create(MemoryContext parent, int ncols,
															 VexecType *const *types);

/*
 * A frame, data[0 .. len): false for a schema's, which is checked against
 * the reader's columns and kept; true for a batch's, whose rows fill "into"
 * -- reset first, its columns pointing into the frame and into memory of
 * the batch's -- in the shapes the frame's schema gives them.  The frame's
 * bytes must stay as they are while "into" is read.  Anything malformed
 * fails with ERRCODE_INVALID_BINARY_REPRESENTATION.
 */
extern VEXEC_API bool vexec_frame_decode(VexecFrameReader *r, const char *data, size_t len,
										 VexecBatch *into);

/* cdbhash.c: the vector cdbhash (§3.10, "The keys, read from the buffers") */

/* How one key column is hashed: by a kernel of vexec's, or through fmgr. */
typedef struct VexecCdbKey
{
	Oid			hashfunc;
	int			kernel;			/* VexecCdbKernel, or -1: fmgr */
	FmgrInfo	flinfo;
} VexecCdbKey;

/*
 * Each of rows[0 .. nrows)'s segment, of "nsegs", into segs[]: the keys,
 * one column of "keys" each, hashed with their functions and folded as
 * cdbhash folds them (gp_core's gp_hash.c), then reduced by jump consistent
 * hashing.  A legacy key is never hashed here: the caller asks gp_core.
 */
extern VEXEC_API void vexec_cdbhash_prepare(VexecCdbKey *key, Oid hashfunc, const VexecType *type);
extern VEXEC_API void vexec_cdbhash(VexecBatch *work, int nkeys, VexecCdbKey *keys,
									VexecVec *const *cols, const int *rows, int nrows,
									int nsegs, int *segs);
extern VEXEC_API int vexec_jump_consistent_hash(uint64 key, int32 nsegs);

/* Whether vexec hashes a key of this hash function itself, as a batch. */
extern VEXEC_API bool vexec_cdbhash_has_kernel(Oid hashfunc, const VexecType *type);

#endif							/* VEXEC_MOTION_FRAME_H */
