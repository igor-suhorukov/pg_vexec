/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * egress.h
 *	  vexec's side of the egress API (pg_vector_executor.md §3.15, V10;
 *	  the contract is vexec_egress.h).
 *
 *	routine.c	the routine behind the rendezvous variable, the check that
 *				the vector executor is active, and the schema and arrays
 *				entries
 *	receiver.c	the receiver: a result's columns into Arrow's types, a
 *				batch at a time, out as IPC messages through ipc/
 *	run.c		ExecutorRun for a statement whose receiver is the
 *				egress's and whose plan has a vector node at the top: its
 *				batches, with no row formed
 *	params.c	a client's parameter batches, to values of the parameters'
 *				types
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_EGRESS_EGRESS_H
#define VEXEC_EGRESS_EGRESS_H

#include "tcop/dest.h"

#include "vexec_egress.h"

#include "vexec.h"
#include "exec/exec.h"
#include "ipc/ipc.h"

/* routine.c */
extern void vexec_egress_install(void);
extern void vexec_egress_check_active(void);
extern void vexec_egress_send(VexecEgressWriteFn write, void *arg,
							  const VexecIpcMessage *msg);

/* receiver.c */
extern DestReceiver *vexec_egress_receiver(VexecEgressWriteFn write, void *arg,
										   const VexecEgressField *fields, int nfields);
extern bool vexec_egress_is_receiver(DestReceiver *dest);
extern void vexec_egress_receiver_counts(DestReceiver *dest, int64 *rows,
										 int64 *batches, int64 *bytes);
extern void vexec_egress_schema(TupleDesc desc, const VexecEgressField *fields,
								int nfields, VexecEgressWriteFn write, void *arg);
extern int64 vexec_egress_send_batch(DestReceiver *dest, VexecNode *node,
									 VexecBatch *batch, const int *colmap);
extern VEXEC_API int64 vexec_egress_vector_batches(DestReceiver *dest);

/* run.c */
extern void vexec_egress_run_install(void);

/* params.c */
extern void *vexec_egress_params_begin(const char *metadata, size_t len, int nparams,
									   const Oid *types, const int32 *typmods);
extern int64 vexec_egress_params_batch(void *state, const char *metadata, size_t len,
									   const char *body, size_t body_len,
									   VexecEgressRowFn row, void *arg);
extern void vexec_egress_params_end(void *state);
extern void vexec_egress_param_natural(const struct ArrowSchema *field, int column,
									   Oid *type, int32 *typmod);
extern void *vexec_egress_stream_columns(const char *metadata, size_t len, int ncols,
										 const Oid *types, const int32 *typmods);
extern Datum vexec_egress_stream_value(void *state, int column, const struct ArrowArray *a,
									   int64 row, bool *isnull);
extern void vexec_egress_stream_row_done(void *state);

/* ingest.c: a client's stream as a source (§3.16) */
typedef struct VexecIngestCursor VexecIngestCursor;

extern void *vexec_egress_ingest_begin(const char *metadata, size_t len,
									   VexecEgressReadFn read, void *arg);
extern int	vexec_egress_ingest_columns(void *stream, const VexecIngestColumn **columns);
extern int64 vexec_egress_ingest_handle(void *stream);
extern int64 vexec_egress_ingest_rows(void *stream);
extern bool vexec_egress_ingest_finished(void *stream);
extern void vexec_egress_ingest_end(void *stream);
extern void vexec_ingest_install(void);
extern VexecIngestCursor *vexec_ingest_open(int64 handle, int ncols, const Oid *types,
											const int32 *typmods);
extern bool vexec_ingest_next_window(VexecIngestCursor *cursor, VexecBatch *batch,
									 const VexecLayoutConfig *layout);
extern bool vexec_ingest_next_row(VexecIngestCursor *cursor, Datum *values, bool *isnull);
extern void vexec_ingest_close(VexecIngestCursor *cursor);
extern bool vexec_ingest_zero_copy(VexecIngestCursor *cursor);

#endif							/* VEXEC_EGRESS_EGRESS_H */
