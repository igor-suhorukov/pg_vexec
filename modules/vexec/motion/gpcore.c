/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * gpcore.c
 *	  What vexec asks of the port's gp_core, through its API (the port's
 *	  pg19/include/gp_core_api.h, of which pgxs/include holds a copy), from
 *	  its minor version 15 (pg_vector_executor.md §3.10, V7).
 *
 * vexec builds and loads with no gp_core -- on vanilla PostgreSQL 19, and
 * on one node of the port without it -- so it finds gp_core only through
 * the rendezvous variable gp_core publishes (cb_module.h), never by a
 * symbol, and asks nothing of a gp_core older than the API's minor version
 * 15, which has what the frames across Motions need:
 *
 *	a Redistribute's hash functions, a Gather's merge keys, and cdbhash a
 *		row at a time, for the senders' vector cdbhash and its fallback;
 *	squelch_subtree(), by which a VecHashJoin that reads no more of its
 *		sides stops their Motions' senders, where gp_core's executor would
 *		squelch a HashJoin's, in place of reading them to their end;
 *	explain_register(), by which a segment keeps vexec's own figures of
 *		the vector nodes it ran -- their batches, their frames -- and the
 *		coordinator's EXPLAIN ANALYZE prints them (§3.10, "EXPLAIN
 *		ANALYZE").
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"

#include "cb_module.h"
#include "gp_core_api.h"

#include "vexec.h"
#include "exec/exec.h"
#include "motion/motion.h"

/* gp_core's API of minor version 15 or later, once found; NULL before. */
static const GpCoreApi *gp_core = NULL;
static bool gp_core_looked = false;

const GpCoreApi *
vexec_gp_core(void)
{
	void	  **rv;
	const GpCoreApi *api;

	if (gp_core != NULL || gp_core_looked)
		return gp_core;
	rv = find_rendezvous_variable(CB_CORE_RENDEZVOUS);
	api = (const GpCoreApi *) *rv;

	/*
	 * Looked up again until gp_core is there: a library listed before
	 * gp_core in shared_preload_libraries finds nothing as it loads.
	 */
	if (api == NULL)
		return NULL;
	gp_core_looked = true;
	if (api->version_major == GP_CORE_API_VERSION_MAJOR &&
		api->version_minor >= VEXEC_GP_CORE_MINOR)
		gp_core = api;
	return gp_core;
}

/* ---------------------------------------------------------------------
 * EXPLAIN ANALYZE's figures of the vector nodes the segments ran
 * ---------------------------------------------------------------------
 */

/* What a segment keeps of a vector node it ran: a VexecExplainBlob. */
#define VEXEC_BLOB_MAGIC	0x56584231	/* "VXB1" */

typedef struct VexecExplainBlob
{
	uint32		magic;
	int32		kind;			/* VexecNodeKind */
	VexecNodeStats stats;
	VexecMotionStats motion;
} VexecExplainBlob;

/* On a segment: a vector node's figures, for the coordinator. */
static bool
explain_collect(PlanState *ps, StringInfoData *buf)
{
	VexecNode  *node;
	VexecExplainBlob b;

	if (!vexec_is_vector_state(ps))
		return false;
	node = (VexecNode *) ps;
	if (!node->ran)
		return false;
	memset(&b, 0, sizeof(b));
	b.magic = VEXEC_BLOB_MAGIC;
	b.kind = node->kind;
	b.stats = node->stats;
	if (node->kind == VEXEC_NODE_MOTION_SEND || node->kind == VEXEC_NODE_MOTION_RECV)
		b.motion = *vexec_motion_stats(node);
	appendBinaryStringInfo(buf, &b, sizeof(b));
	return true;
}

/*
 * On the coordinator: a segment's figures of the node, added to what the
 * node's EXPLAIN prints for the segments, which ran it where the
 * coordinator did not.
 */
static void
explain_deposit(PlanState *ps, int content, const char *data, int len)
{
	VexecNode  *node;
	VexecExplainBlob b;

	if (!vexec_is_vector_state(ps) || len != (int) sizeof(b))
		return;
	memcpy(&b, data, sizeof(b));
	node = (VexecNode *) ps;
	if (b.magic != VEXEC_BLOB_MAGIC || b.kind != node->kind)
		return;
	node->segments_seen++;
	node->segment_stats.batches += b.stats.batches;
	node->segment_stats.rows_in += b.stats.rows_in;
	node->segment_stats.kernel_steps += b.stats.kernel_steps;
	node->segment_stats.fallback_rows += b.stats.fallback_rows;
	node->segment_stats.lazy_rows += b.stats.lazy_rows;
	node->segment_stats.redo_rows += b.stats.redo_rows;
	node->segment_stats.declared_rows += b.stats.declared_rows;
	node->segment_stats.batches_out += b.stats.batches_out;
	node->segment_stats.drained_motions += b.stats.drained_motions;
	node->segment_stats.drained_rows += b.stats.drained_rows;
	node->segment_stats.squelched += b.stats.squelched;
	if (node->kind == VEXEC_NODE_MOTION_SEND || node->kind == VEXEC_NODE_MOTION_RECV)
	{
		VexecMotionStats *m = vexec_motion_stats(node);

		m->frames += b.motion.frames;
		m->schema_frames += b.motion.schema_frames;
		m->bytes += b.motion.bytes;
		m->rows += b.motion.rows;
		m->rows_by_postgres += b.motion.rows_by_postgres;
	}
}

/*
 * Registered once, by the first vector node a backend begins: gp_core's
 * report of a segment's part, and its taking on the coordinator, both run
 * in a backend that has begun a vector node of the plan they are of.
 */
void
vexec_gp_core_explain_register(void)
{
	static bool registered = false;
	const GpCoreApi *api;

	if (registered)
		return;
	api = vexec_gp_core();
	if (api == NULL)
		return;
	api->explain_register(explain_collect, explain_deposit);
	registered = true;
}

/*
 * The subtree a node reads no more, its Motions' senders stopped, where
 * gp_core squelches there (GpCoreApi.squelch_subtree); false where it does
 * not, and the caller reads what it must to its end.
 */
bool
vexec_gp_core_squelch(PlanState *ps)
{
	const GpCoreApi *api = vexec_gp_core();

	return api != NULL && api->squelch_subtree(ps);
}
