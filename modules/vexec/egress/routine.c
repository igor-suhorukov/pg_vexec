/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * routine.c
 *	  The egress API's routine, published through the rendezvous variable
 *	  "vexec/egress_v1" (pg_vector_executor.md §3.15, V10; vexec_egress.h).
 *
 * vexec publishes the routine while the postmaster preloads it, so every
 * process forked after that finds it, as storage modules find the source
 * registry (vexec_source.h).  An extension that serves Arrow to clients
 * looks it up at run time; without vexec there is none, and vexec_flight's
 * acceptor exits with its WARNING (§3.15).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/memutils.h"

#include "vexec_egress.h"

#include "vexec.h"
#include "egress/egress.h"
#include "ipc/ipc.h"

/* The two descriptions of a piece are one layout. */
StaticAssertDecl(sizeof(VexecEgressPiece) == sizeof(VexecIpcPiece) &&
				 offsetof(VexecEgressPiece, len) == offsetof(VexecIpcPiece, len),
				 "VexecEgressPiece and VexecIpcPiece differ");
StaticAssertDecl(VEXEC_EGRESS_SCHEMA == VEXEC_IPC_SCHEMA &&
				 VEXEC_EGRESS_RECORD_BATCH == VEXEC_IPC_RECORD_BATCH,
				 "the egress's message kinds are the codec's");

static bool
egress_active(void)
{
	return vexec_mode == VEXEC_MODE_AUTO || vexec_mode == VEXEC_MODE_FORCE;
}

/* The API refuses unless the vector executor is active (§3.15). */
void
vexec_egress_check_active(void)
{
	if (!egress_active())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the vector executor is not active in this session"),
				 errdetail("\"vexec.mode\" is \"%s\".", vexec_mode_name(vexec_mode)),
				 errhint("Set \"vexec.mode\" to \"auto\" or \"force\".")));
}

/* A message of the codec's, to the caller's write function. */
void
vexec_egress_send(VexecEgressWriteFn write, void *arg, const VexecIpcMessage *msg)
{
	VexecEgressMessage out;

	out.kind = msg->kind;
	out.prefix = msg->prefix;
	out.metadata = msg->metadata;
	out.metadata_len = msg->metadata_len;
	out.pieces = (const VexecEgressPiece *) msg->pieces;
	out.npieces = msg->npieces;
	out.body_len = msg->body_len;
	out.nrows = msg->nrows;
	write(arg, &out);
}

/* Arrays a caller made: a Schema message when asked, then a RecordBatch message. */
static void
egress_arrays(const struct ArrowSchema *schema, const struct ArrowArray *array,
			  bool with_schema, VexecEgressWriteFn write, void *arg)
{
	VexecIpcWriter *writer;

	vexec_egress_check_active();
	writer = vexec_ipc_writer_create(CurrentMemoryContext);
	PG_TRY();
	{
		if (with_schema)
			vexec_egress_send(write, arg, vexec_ipc_write_schema(writer, schema));
		vexec_egress_send(write, arg, vexec_ipc_write_batch(writer, schema, array));
	}
	PG_FINALLY();
	{
		vexec_ipc_writer_free(writer);
	}
	PG_END_TRY();
}

static int
egress_message_kind(const char *metadata, size_t len)
{
	VexecIpcHeader header;

	vexec_egress_check_active();
	vexec_ipc_read_message(metadata, len, &header);
	return header.kind;
}

static DestReceiver *
egress_receiver(VexecEgressWriteFn write, void *arg,
				const VexecEgressField *fields, int nfields)
{
	return vexec_egress_receiver(write, arg, fields, nfields);
}

static void
egress_schema(TupleDesc desc, const VexecEgressField *fields, int nfields,
			  VexecEgressWriteFn write, void *arg)
{
	vexec_egress_schema(desc, fields, nfields, write, arg);
}

static const VexecEgressRoutine egress_routine = {
	.magic = VEXEC_EGRESS_MAGIC,
	.minor = VEXEC_EGRESS_MINOR,
	.size = sizeof(VexecEgressRoutine),
	.active = egress_active,
	.receiver = egress_receiver,
	.receiver_counts = vexec_egress_receiver_counts,
	.schema = egress_schema,
	.arrays = egress_arrays,
	.message_kind = egress_message_kind,
	.params_begin = vexec_egress_params_begin,
	.params_batch = vexec_egress_params_batch,
	.params_end = vexec_egress_params_end,
};

/*
 * Publish the routine, from _PG_init while the postmaster preloads vexec,
 * and take ExecutorRun for the batches (run.c).
 */
void
vexec_egress_install(void)
{
	const VexecEgressRoutine **rv;

	rv = (const VexecEgressRoutine **) find_rendezvous_variable(VEXEC_EGRESS_RENDEZVOUS);
	*rv = &egress_routine;
	vexec_egress_run_install();
}
