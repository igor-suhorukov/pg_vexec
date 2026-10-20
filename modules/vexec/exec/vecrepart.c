/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecrepart.c
 *	  VecRepartition: the partial groups of a parallel aggregation, dealt
 *	  out among the Gather's participants by their keys' hash, so that each
 *	  participant finalizes the groups it is dealt (pg_vector_executor.md
 *	  §3.14, H5).
 *
 * PostgreSQL's parallel aggregation finalizes in the leader alone: every
 * participant's partial groups cross the tuple queue, and the Finalize Agg
 * above the Gather combines them (PG19:src/backend/optimizer/plan/
 * planner.c, create_partial_grouping_paths()).  With millions of groups
 * the leader does most of the work.  This node sits between a partial
 * VecAgg and a final one, both in every participant, below the Gather:
 *
 *	writing		each participant reads its partial VecAgg's groups to their
 *				end, and writes each into one of the partitions by the hash
 *				of its keys -- a SharedTuplestore a partition, in a
 *				SharedFileSet, as a parallel hash join writes its batches
 *				(PG19:src/backend/executor/nodeHashjoin.c,
 *				ExecParallelHashJoinPartitionOuter());
 *	the barrier	once every participant that came has written, the barrier
 *				passes on (storage/barrier.h);
 *	reading		each participant claims partitions one at a time, with an
 *				atomic counter, and hands its final VecAgg every group of
 *				each, a batch at a time.
 *
 * A group's partial states, from whatever participant, are in one
 * partition, which one participant reads: so the final VecAgg in each
 * participant finalizes groups no other one has, and the Gather receives
 * final groups only -- and under ORDER BY ... LIMIT, a Gather Merge each
 * participant's first rows.
 *
 * The hash is VecAgg's own of the keys (vecagg.c, C4 in §3.19), which hashes
 * equal what the grouping's equality operators take for equal, so keys a
 * grouping takes for equal meet in one partition.  A partial VecAgg below
 * that groups by the same keys hands each group's hash on, and the node
 * computes none; it keeps each row's hash beside it in the partition, and
 * hands it up with the batch to the VecAgg above, which groups by the same
 * keys and takes it as the row's.  A partition is chosen by the hash mixed
 * again, so that the bits the VecAgg above takes its buckets and its spill
 * files by stay spread in each partition.
 *
 * A participant that comes once the barrier has passed on -- a worker that
 * started late, or the leader, which first reads the workers' queues --
 * finds its input read to its end by the others: every row of a parallel
 * plan comes from a parallel-aware scan, which hands out its pages, files
 * or segment files until there are none, and every participant that came
 * read until there were none.  So it writes nothing, and only reads.
 *
 * With no DSM -- a parallel plan run serially, as a Gather run outside
 * parallel mode runs its child -- the node passes its child's groups on as
 * they come: there is one participant.
 *
 * Batches and frames (X9, §3.18).  A partial VecAgg below that hands up
 * its groups as batches, with their hashes (vecagg.c), gives the node a
 * batch at a time, not a row; and the partitions then hold Arrow IPC
 * frames, V7's format (motion/frame.c), not tuples: the VecAgg deals its
 * groups into a batch a partition as they go out, their hashes a last
 * column (vexec_agg_deal()), and hands each up as it fills; the node writes
 * it as one frame -- one record of the SharedTuplestore, behind 8 bytes
 * whose first 4 are the record's length, as a MinimalTuple's t_len is,
 * which is all the store reads of a tuple -- and the participant that
 * claims the partition reads each frame back as a batch, its columns
 * pointing into the frame.  Every participant's frames are in one schema,
 * the columns' build shapes, which each one's reader knows from the start.
 * vexec.enable_motion_frames off, the partitions hold tuples.  Velox passes its batches among a pipeline's
 * drivers in memory (LocalPartition); here they go through the store's
 * files, which a parallel hash join's batches go through too.
 *
 * Groups as they lie (D2, §3.20).  Where the partial VecAgg below can make
 * its groups in vexec's own layout -- every state one without a pointer --
 * and the final VecAgg above can merge them so, the partitions hold the
 * groups themselves, not frames: the partial stage makes each group of a
 * batch's table (C2) in its partition's chunk as the batch finds it, and
 * puts its table's groups there once its input is read (vecagg.c).  A
 * chunk is in shared memory while this participant's take less than
 * vexec.repartition_shared_memory: carved from DSM segments of the
 * participant's own, which it makes as it needs them, 4 MB and doubling to
 * 64 MB, their handles in the node's chunk of the DSM -- no lock taken, as
 * the parallel query's DSA takes one for each allocation, and holds while
 * it zeroes a new segment -- and pushed onto its partition's list there.
 * Where the server has an exchange area (vexec.exchange_memory, M1 of
 * §3.21; exchange.c), a participant's first chunks are cut from its 4 MB
 * blocks instead, mapped in every process since the postmaster started and
 * neither zeroed nor given back to the system -- 48 of them at most, then
 * segments as without it -- and the last process of the parallel query to
 * let go of its DSM gives them back to the area.
 A chunk goes onto its list once the batch whose groups it may
 * hold is done with them, so that a chunk on a list is as it will stay.
 * While other participants still write, one that has written claims its
 * share of the partitions -- their number over the participants' -- and
 * merges the chunks already on their lists, mapping a segment as a chunk of
 * it first comes: the time a participant waits for the slowest writer, a
 * process sharing a core with another, is spent on the final stage's work.
 * Once every participant has written, each maps every other's segments,
 * and the barrier passes once more, so that none may go before every
 * reader has them; a participant that comes later reads nothing.  The
 * participant that claims a partition walks its list, handing the final
 * VecAgg each chunk where it lies, and once it is read gives its pages
 * back (madvise's MADV_REMOVE), so that no participant frees the
 * partitions' gigabyte at the end.  Past the budget, or with no DSM
 * segment to be had, a chunk is made in local memory and written, once the
 * batch that filled it is done with its groups, as one record of the
 * partition's SharedTuplestore, as a frame is.  No slot, no PostgreSQL's
 * form of a state, no frame and, under the budget, no file: Q32's
 * partitions were 900 MB of files.  Only with a DSM: run serially, the node
 * passes batches on as before.  vexec.enable_packed_groups off, frames.
 *
 * Its scan tuple, which custom_scan_tlist describes, is its child's row, as
 * VecSort's (vecsort.c); its keys are columns of it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/parallel.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "pgstat.h"
#include "port/atomics.h"
#include "port/pg_bitutils.h"
#include "storage/barrier.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/sharedfileset.h"
#include <sys/mman.h>

#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/sharedtuplestore.h"
#include "utils/ruleutils.h"
#include "utils/wait_event.h"

#include "cb_explain.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "exec/keyhash.h"
#include "expr/kernel.h"
#include "motion/frame.h"

#define REPART_WRITING	0		/* the barrier's phases */
#define REPART_READING	1

/* X9: a frame's record: its length, then 4 bytes, then the frame */
#define REPART_FRAME_PREFIX	8

/*
 * D2 (above): a chunk of a partition's packed groups, local or in shared
 * memory, its header first; written to a file, its first 4 bytes are the
 * record's length, as a MinimalTuple's t_len is
 */
typedef uint64 PackPointer;		/* a shared chunk: its participant, segment
								 * and offset (pack_pointer()) */

#define PACK_NONE			PG_UINT64_MAX

typedef struct PackChunk
{
	uint32		len;			/* in a file: the header and the groups' bytes */
	uint32		used;			/* the groups' bytes after the header */
	uint32		size;			/* the bytes it has room for after the header */
	int32		partition;
	PackPointer self;			/* its own place, a shared one's */
	PackPointer next;			/* the partition's next chunk in shared memory */
} PackChunk;

#define PACK_CHUNK_HEADER	MAXALIGN(sizeof(PackChunk))
#define PACK_PAGE			((Size) 4096)	/* a shared chunk's alignment */
#define PACK_CHUNK_FIRST	((Size) 16 * 1024)	/* a partition's first chunk */
#define PACK_CHUNK_MAX		((Size) 256 * 1024)	/* and its largest */
#define PACK_SEGMENTS		64	/* a participant's segments and exchange
								 * area blocks (M1), at most */
#define PACK_AREA_BLOCKS	48	/* M1: of them, the area's blocks at most --
								 * taken first, the segments after them
								 * growing as without it, past its 16's sum */
#define PACK_KIND_DSM		0	/* a segment's entry: a DSM segment's handle */
#define PACK_KIND_AREA		1	/* or an exchange area block's index (M1) */
#define PACK_SEGMENT_FIRST	((Size) 4 * 1024 * 1024)
#define PACK_SEGMENT_MAX	((Size) 64 * 1024 * 1024)

/* the wait at the barrier, as pg_stat_activity names it */
static uint32 repart_wait_event = 0;

/* The node's chunk of the DSM. */
typedef struct RepartShared
{
	SharedFileSet fileset;
	dsm_handle	seg;			/* the parallel query's segment: a worker
								 * attaches the fileset to it */
	Barrier		barrier;
	pg_atomic_uint32 next_partition;
	int			nparticipants;	/* the leader and the workers planned */
	int			npartitions;
	bool		frames;			/* the partitions hold frames (X9) */
	bool		packed;			/* or packed groups (D2) */
	Size		heads_off;		/* D2: from the struct, each partition's
								 * list of chunks in shared memory, a
								 * pg_atomic_uint64 a partition; */
	Size		segs_off;		/* each participant's segments' handles,
								 * PACK_SEGMENTS of them; */
	Size		nsegs_off;		/* and how many it made, a uint32 each; */
	Size		kinds_off;		/* each segment's kind, PACK_KIND_*, a byte */
	bool		area;			/* M1: blocks of the exchange area may be cut
								 * from, the leader's say */
	pg_atomic_uint32 area_users;	/* M1: the processes holding the DSM that
									 * may read the area's blocks */
	pg_atomic_uint32 writing;	/* D2: participants writing, which others
								 * read beside (above) */
	pg_atomic_uint32 topk_bound;	/* C5: the first stages' boundary */
	Size		sts_size;		/* each partition's SharedTuplestore */
	char		stores[FLEXIBLE_ARRAY_MEMBER];
} RepartShared;

typedef struct VexecRepartState
{
	VexecNode	node;			/* first */
	VexecRepartPlan plan;
	PlanState  *child;
	TupleDesc	desc;			/* the child's rows */
	int			nkeys;
	AttrNumber *keycols;
	VexecKeyHash *keyhash;		/* the keys' hash, VecAgg's */
	bool		hashes_from_child;	/* a partial VecAgg below hands its
									 * groups' hashes on */
	MemoryContext hashcxt;		/* a row's detoasted keys */
	uint32	   *rowhash;		/* the batch's rows' hashes */

	RepartShared *shared;		/* NULL: no DSM, one participant */
	SharedTuplestoreAccessor **parts;
	TupleTableSlot *read_slot;
	VexecBatch *own_in;			/* the input batch the node fills */
	const uint32 *hashes;		/* the hashes of the input batch's rows */

	/*
	 * X9 (above): the child hands up batches of its groups with their
	 * hashes; the partitions hold frames of them, each row's hash a last
	 * column
	 */
	bool		child_batches;
	bool		frames;
	VexecType **ftypes;			/* the frames' columns */
	VexecFrameWriter *fw;
	VexecFrameReader *fr;
	VexecBatch *frame_in;		/* a frame read back */
	int		   *allrows;		/* 0, 1, ...: a batch's rows */
	MemoryContext framecxt;		/* a frame being written */
	bool		written;		/* this participant's groups are written, or
								 * it came too late to write */
	int			reading;		/* the partition being read, or -1 */
	int			npartitions;	/* the DSM's, for EXPLAIN ANALYZE: the DSM
								 * goes before EXPLAIN prints */

	/*
	 * D2 (above): the partitions may hold packed groups -- the child and
	 * the parent both can -- and, with a DSM, do; this participant's sink,
	 * the shared chunk read last and the next, and figures
	 */
	bool		pack_ok;
	VexecPackSink *sink;
	int			me;				/* this participant: the leader 0 */
	bool		pk_reads;		/* it came before the barrier passed */
	dsm_segment **pk_maps;		/* every participant's segments, mapped */
	int			pk_nmaps;
	PackPointer pk_next;		/* the claimed partition's next shared chunk */
	PackChunk  *pk_done;		/* the one read last: its pages to give back */

	/*
	 * D2: read while others write (above): the partitions claimed then, how
	 * many it may claim, the next to look at in this pass over them, and
	 * whether the pass found a chunk; then each claimed partition read to
	 * its end in turn, pk_cur the next
	 */
	bool		pk_early;
	int		   *pk_claimed;
	int			pk_nclaimed;
	int			pk_quota;
	int			pk_scan;
	bool		pk_found;
	int			pk_cur;
	int64		pk_chunks_early;
	int64		pk_groups;		/* written by this participant */
	Size		pk_shared;		/* the bytes of its chunks in shared memory */
	int64		pk_area_blocks; /* M1: of them, the exchange area's blocks */
	Size		pk_files;		/* and in files */
	int64		pk_chunks_read;
} VexecRepartState;

/* D2: a participant's chunks as they fill (above). */
struct VexecPackSink
{
	VexecRepartState *s;
	Size		budget;			/* the bytes its segments may take */
	int			npartitions;
	PackChunk **cur;			/* per partition: the chunk being filled */
	PackPointer *cur_ptr;		/* its place; PACK_NONE: a local one */
	Size	   *next_size;		/* per partition: the next chunk's bytes */
	List	   *full;			/* chunks filled -- local ones to be written,
								 * shared ones to go onto their lists --
								 * once the batch is done with their groups */
	MemoryContext cxt;
	int			nsegs;			/* the segments made, the last being cut */
	int			nblocks;		/* M1: of them, exchange area blocks */
	Size		seg_used;
	Size		seg_size;
	bool		no_segments;	/* none to be had: local chunks from now */
};

static bool repart_fetch(VexecNode *node);
static void frames_begin(VexecRepartState *s, VexecType **types);
static void repart_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *repart_exec(CustomScanState *css);
static void repart_end(CustomScanState *css);
static void repart_rescan(CustomScanState *css);
static void repart_shutdown(CustomScanState *css);
static void repart_explain(CustomScanState *css, List *ancestors, ExplainState *es);
static Size repart_estimate_dsm(CustomScanState *css, ParallelContext *pcxt);
static void repart_initialize_dsm(CustomScanState *css, ParallelContext *pcxt, void *coordinate);
static void repart_reinitialize_dsm(CustomScanState *css, ParallelContext *pcxt, void *coordinate);
static void repart_initialize_worker(CustomScanState *css, shm_toc *toc, void *coordinate);

static const CustomExecMethods repart_exec_methods = {
	.CustomName = VEXEC_REPART_NAME,
	.BeginCustomScan = repart_begin,
	.ExecCustomScan = repart_exec,
	.EndCustomScan = repart_end,
	.ReScanCustomScan = repart_rescan,
	.EstimateDSMCustomScan = repart_estimate_dsm,
	.InitializeDSMCustomScan = repart_initialize_dsm,
	.ReInitializeDSMCustomScan = repart_reinitialize_dsm,
	.InitializeWorkerCustomScan = repart_initialize_worker,
	.ShutdownCustomScan = repart_shutdown,
	.ExplainCustomScan = repart_explain,
};

/* Its methods, for pg_otel_probe's reporter of vector nodes (node.c). */
const CustomExecMethods *
vexec_repart_exec_methods(void)
{
	return &repart_exec_methods;
}

Node *
vexec_create_repart_state(CustomScan *cscan)
{
	VexecRepartState *s = palloc0(sizeof(VexecRepartState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &repart_exec_methods;
	s->node.kind = VEXEC_NODE_REPART;
	return (Node *) s;
}

/* The plan's custom_private: the keys, columns of the child's rows from 1,
 * with the grouping's equality operators and the keys' collations. */
List *
vexec_repart_plan_encode(const VexecRepartPlan *plan)
{
	return list_make3(list_copy(plan->keycols), list_copy(plan->eqops),
					  list_copy(plan->collations));
}

void
vexec_repart_plan_decode(CustomScan *cscan, VexecRepartPlan *plan)
{
	List	   *p = cscan->custom_private;

	if (list_length(p) != 3)
		elog(ERROR, "vexec: a VecRepartition's plan of %d parts", list_length(p));
	plan->keycols = linitial(p);
	plan->eqops = lsecond(p);
	plan->collations = lthird(p);
	if (list_length(plan->eqops) != list_length(plan->keycols) ||
		list_length(plan->collations) != list_length(plan->keycols))
		elog(ERROR, "vexec: a VecRepartition of %d keys and %d operators",
			 list_length(plan->keycols), list_length(plan->eqops));
}

static void
repart_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecRepartState *s = (VexecRepartState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	TupleDesc	scandesc = css->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	VexecType **types;
	int			i;

	vexec_node_begin(node, estate);
	node->fetch = repart_fetch;
	node->label = "Vec Repartition";
	vexec_repart_plan_decode(cscan, &s->plan);

	s->child = ExecInitNode(outerPlan(cscan), estate, eflags);
	outerPlanState(css) = s->child;
	s->desc = ExecGetResultType(s->child);
	if (scandesc->natts != s->desc->natts)
		elog(ERROR, "vexec: a VecRepartition's scan tuple of %d columns, its child's %d",
			 scandesc->natts, s->desc->natts);

	/*
	 * The keys' hash: VecAgg's, taken from a partial VecAgg below that
	 * groups by the same keys where there is one (C4)
	 */
	s->nkeys = list_length(s->plan.keycols);
	s->keycols = palloc(sizeof(AttrNumber) * Max(s->nkeys, 1));
	for (i = 0; i < s->nkeys; i++)
		s->keycols[i] = (AttrNumber) list_nth_int(s->plan.keycols, i);
	s->keyhash = vexec_key_hash_create(s->desc, s->plan.keycols, s->plan.eqops,
									   s->plan.collations);
	s->hashes_from_child = vexec_is_vector_state(s->child) &&
		vexec_agg_hashes_keys((VexecNode *) s->child, s->plan.keycols, s->plan.eqops,
							  s->plan.collations);

	/*
	 * C2: the partial aggregation below may freeze its table, the stage
	 * above combining whatever states it is dealt (vecagg.c)
	 */
	if (vexec_is_vector_state(s->child))
		vexec_agg_allow_freeze((VexecNode *) s->child);

	/* X9: its groups as batches, with their hashes; the partitions as frames */
	s->child_batches = s->hashes_from_child &&
		vexec_node_batchable((VexecNode *) s->child) &&
		vexec_agg_batch_hashes((VexecNode *) s->child) != NULL;
	s->frames = s->child_batches && vexec_enable_motion_frames;
	s->hashcxt = AllocSetContextCreate(node->mcxt, "VecRepartition keys", ALLOCSET_SMALL_SIZES);
	s->rowhash = palloc0_array(uint32, VEXEC_BATCH_ROWS);

	/* the scan tuple: the child's row */
	types = palloc(sizeof(VexecType *) * Max(scandesc->natts, 1));
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(scandesc->natts, 1));
	for (i = 0; i < scandesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(scandesc, i);

		types[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		node->input_attnos[i] = i + 1;
	}
	node->input_varno = INDEX_VAR;
	node->ninput = scandesc->natts;
	node->input_slot = css->ss.ss_ScanTupleSlot;
	s->own_in = vexec_batch_create(node->mcxt, scandesc->natts, types);
	node->in = s->own_in;
	vexec_node_compile(node, cscan->scan.plan.qual, cscan->scan.plan.targetlist);

	s->read_slot = ExecInitExtraTupleSlot(estate, s->desc, &TTSOpsMinimalTuple);
	s->reading = -1;
	s->pk_next = PACK_NONE;
	if (s->frames)
		frames_begin(s, types);
}

/*
 * X9: the frames' writer and reader, over the child's columns and an int4
 * of each row's hash.  The schema is the columns' build shapes, the
 * batches' the VecAgg below deals its groups into, every participant's
 * alike: the reader is given it now, as no frame of a partition carries it.
 */
static void
frames_begin(VexecRepartState *s, VexecType **types)
{
	MemoryContext mcxt = s->node.mcxt;
	int			natts = s->desc->natts;
	VexecBatch *empty;
	uint64		schema;
	bytea	   *schema_frame;
	int			i;

	s->ftypes = MemoryContextAlloc(mcxt, sizeof(VexecType *) * (natts + 1));
	memcpy(s->ftypes, types, sizeof(VexecType *) * natts);
	s->ftypes[natts] = vexec_type_make(INT4OID, -1, InvalidOid);
	s->fw = vexec_frame_writer_create(mcxt, natts + 1, s->ftypes);
	s->fr = vexec_frame_reader_create(mcxt, natts + 1, s->ftypes);
	s->frame_in = vexec_batch_create(mcxt, natts + 1, s->ftypes);
	s->allrows = MemoryContextAlloc(mcxt, sizeof(int) * VEXEC_BATCH_ROWS);
	for (i = 0; i < VEXEC_BATCH_ROWS; i++)
		s->allrows[i] = i;
	s->framecxt = AllocSetContextCreate(mcxt, "VecRepartition frame", ALLOCSET_DEFAULT_SIZES);

	empty = vexec_batch_create(s->framecxt, natts + 1, s->ftypes);
	vexec_batch_begin_rows(empty);
	vexec_frame_begin(s->fw, empty, &schema, &schema_frame);
	if (vexec_frame_decode(s->fr, VARDATA(schema_frame), VARSIZE(schema_frame) - VARHDRSZ,
						   s->frame_in))
		elog(ERROR, "vexec: a VecRepartition's schema is no schema frame");
	MemoryContextReset(s->framecxt);
}

/*
 * A row's hash: its group's, which the partial VecAgg below handed on, or
 * VecAgg's of its keys.
 */
static uint32
row_hash(VexecRepartState *s, TupleTableSlot *slot)
{
	MemoryContext old;
	uint32		hash;

	if (s->hashes_from_child)
		return vexec_agg_last_hash((VexecNode *) s->child);
	slot_getallattrs(slot);
	MemoryContextReset(s->hashcxt);
	old = MemoryContextSwitchTo(s->hashcxt);
	hash = vexec_key_hash_slot(s->keyhash, slot);
	MemoryContextSwitchTo(old);
	return hash;
}

/* A hash's partition: by the hash mixed again (above). */
static int
hash_partition(VexecRepartState *s, uint32 hash)
{
	return (int) (murmurhash32(hash ^ 0x9E3779B9) % (uint32) s->shared->npartitions);
}

/* X9: the partition a hash's row goes to, for the VecAgg that deals them. */
static int
deal_partition(void *arg, uint32 hash)
{
	return hash_partition((VexecRepartState *) arg, hash);
}

/* ---------------------------------------------------------------------
 * D2: packed groups (above)
 * ---------------------------------------------------------------------
 */

/* Each partition's list of chunks in shared memory. */
static pg_atomic_uint64 *
pack_heads(RepartShared *sh)
{
	return (pg_atomic_uint64 *) ((char *) sh + sh->heads_off);
}

/* Each participant's segments' handles, PACK_SEGMENTS a participant. */
static dsm_handle *
pack_handles(RepartShared *sh)
{
	return (dsm_handle *) ((char *) sh + sh->segs_off);
}

/* How many segments each participant made. */
static uint32 *
pack_nsegs(RepartShared *sh)
{
	return (uint32 *) ((char *) sh + sh->nsegs_off);
}

/* Each participant's segments' kinds, PACK_KIND_*. */
static uint8 *
pack_kinds(RepartShared *sh)
{
	return (uint8 *) sh + sh->kinds_off;
}

static inline PackPointer
pack_pointer(int participant, int seg, Size offset)
{
	return ((uint64) participant << 48) | ((uint64) seg << 32) | (uint64) offset;
}

/* A shared chunk's address in this participant, whose maps hold its segment. */
static PackChunk *
pack_chunk(VexecRepartState *s, PackPointer ptr)
{
	int			participant = (int) (ptr >> 48);
	int			seg = (int) ((ptr >> 32) & 0xFFFF);
	dsm_segment *m;

	/* M1: an exchange area block, mapped since the postmaster's start */
	if (pack_kinds(s->shared)[participant * PACK_SEGMENTS + seg] == PACK_KIND_AREA)
		return (PackChunk *) (vexec_exchange_block((int32) pack_handles(s->shared)[participant * PACK_SEGMENTS + seg]) +
							  (ptr & 0xFFFFFFFF));
	m = s->pk_maps[participant * PACK_SEGMENTS + seg];

	/*
	 * mapped as a chunk of it first comes: its writer is writing, or waits
	 * at the barrier, and the segment is there; its handle went into the
	 * DSM before any chunk of it onto a list
	 */
	if (m == NULL)
	{
		m = dsm_attach(pack_handles(s->shared)[participant * PACK_SEGMENTS + seg]);
		if (m == NULL)
			elog(ERROR, "vexec: a parallel aggregation's segment is gone before it was read");
		s->pk_maps[participant * PACK_SEGMENTS + seg] = m;
	}
	return (PackChunk *) ((char *) dsm_segment_address(m) + (ptr & 0xFFFFFFFF));
}

/*
 * A shared chunk read: its pages given back to the system, so that its
 * memory goes as it is read, not as the last participant detaches.
 */
static void
pack_release(PackChunk *c)
{
	/* M1: an exchange area block's pages stay, for the next query */
	if (vexec_exchange_holds(c))
		return;
#ifdef MADV_REMOVE
	(void) madvise((void *) c, PACK_CHUNK_HEADER + c->size, MADV_REMOVE);
#else
	(void) c;
#endif
}

/* A shared chunk onto its partition's list, which other participants push onto too. */
static void
pack_push(VexecRepartState *s, int p, PackChunk *c, PackPointer ptr)
{
	pg_atomic_uint64 *head = &pack_heads(s->shared)[p];
	uint64		old = pg_atomic_read_u64(head);

	do
		c->next = (PackPointer) old;
	while (!pg_atomic_compare_exchange_u64(head, &old, (uint64) ptr));
}

/* A local chunk's groups as one record of its partition's store. */
static void
pack_write_chunk(VexecRepartState *s, PackChunk *c)
{
	uint32		used = c->used;

	c->len = (uint32) (PACK_CHUNK_HEADER + c->used);
	sts_puttuple(s->parts[c->partition], &used, (MinimalTuple) c);
	s->pk_files += c->len;
}

/*
 * A partition's chunk done with: once the batch whose groups it may hold is
 * done with them, a shared one goes onto its list, which others may read
 * while this participant writes, and a local one is written.
 */
static void
pack_retire(VexecPackSink *k, int p)
{
	/* a batch's row context is current: the list is the sink's */
	MemoryContext old = MemoryContextSwitchTo(k->cxt);

	k->cur[p]->self = k->cur_ptr[p];
	k->full = lappend(k->full, k->cur[p]);
	MemoryContextSwitchTo(old);
	k->cur[p] = NULL;
	k->cur_ptr[p] = PACK_NONE;
}

/*
 * A shared chunk of `bytes`, a multiple of PACK_PAGE, cut from this
 * participant's last segment, or from a new one -- PACK_SEGMENT_FIRST and
 * doubling to PACK_SEGMENT_MAX, within the budget -- whose handle goes into
 * the DSM for the others to map; NULL where none is to be had.
 */
static PackChunk *
pack_shared_chunk(VexecPackSink *k, Size bytes, PackPointer *ptr)
{
	VexecRepartState *s = k->s;
	RepartShared *sh = s->shared;
	int			me = s->me;

	if (k->nsegs == 0 || k->seg_size - k->seg_used < bytes)
	{
		Size		size = k->nsegs == 0 ? PACK_SEGMENT_FIRST :
			Min(k->seg_size * 2, PACK_SEGMENT_MAX);
		dsm_segment *seg;

		size = Max(size, bytes);
		/*
		 * M1: a block of the exchange area first, where the leader gave the
		 * query it and one is free; then a segment of this participant's own,
		 * and only segments after it
		 */
		if (sh->area && !k->no_segments && k->nblocks == k->nsegs && k->nsegs < PACK_AREA_BLOCKS &&
			bytes <= VEXEC_EXCHANGE_BLOCK && s->pk_shared + VEXEC_EXCHANGE_BLOCK <= k->budget)
		{
			int32		b = vexec_exchange_claim();

			if (b >= 0)
			{
				pack_kinds(sh)[me * PACK_SEGMENTS + k->nsegs] = PACK_KIND_AREA;
				pack_handles(sh)[me * PACK_SEGMENTS + k->nsegs] = (dsm_handle) b;
				pack_nsegs(sh)[me] = ++k->nsegs;
				k->nblocks++;
				k->seg_size = VEXEC_EXCHANGE_BLOCK;
				k->seg_used = 0;
				s->pk_shared += VEXEC_EXCHANGE_BLOCK;
				s->pk_area_blocks++;
				*ptr = pack_pointer(me, k->nsegs - 1, k->seg_used);
				k->seg_used += bytes;
				return pack_chunk(s, *ptr);
			}
		}
		if (k->no_segments || k->nsegs == PACK_SEGMENTS || s->pk_shared + size > k->budget)
			return NULL;
		seg = dsm_create(size, DSM_CREATE_NULL_IF_MAXSEGMENTS);
		if (seg == NULL)
		{
			k->no_segments = true;
			return NULL;
		}
		s->pk_maps[me * PACK_SEGMENTS + k->nsegs] = seg;
		pack_kinds(sh)[me * PACK_SEGMENTS + k->nsegs] = PACK_KIND_DSM;
		pack_handles(sh)[me * PACK_SEGMENTS + k->nsegs] = dsm_segment_handle(seg);
		pack_nsegs(sh)[me] = ++k->nsegs;
		k->seg_size = size;
		k->seg_used = 0;
		s->pk_shared += size;
	}
	*ptr = pack_pointer(me, k->nsegs - 1, k->seg_used);
	k->seg_used += bytes;
	return pack_chunk(s, *ptr);
}

/*
 * len bytes for a group of partition p, MAXALIGNed: in its chunk, or a new
 * one, from PACK_CHUNK_FIRST and doubling to PACK_CHUNK_MAX -- shared while
 * there is a segment to cut it from, else local.
 */
char *
vexec_pack_alloc(VexecPackSink *k, int p, Size len)
{
	PackChunk  *c = k->cur[p];
	char	   *g;

	Assert(len == MAXALIGN(len));
	if (c == NULL || c->size - c->used < len)
	{
		Size		bytes = TYPEALIGN(PACK_PAGE, Max(k->next_size[p], PACK_CHUNK_HEADER + len));
		PackPointer ptr = PACK_NONE;

		if (c != NULL)
			pack_retire(k, p);
		k->next_size[p] = Min(k->next_size[p] * 2, PACK_CHUNK_MAX);
		c = pack_shared_chunk(k, bytes, &ptr);
		if (c == NULL)
			c = (PackChunk *) MemoryContextAllocHuge(k->cxt, bytes);
		c->len = 0;
		c->used = 0;
		c->size = (uint32) (bytes - PACK_CHUNK_HEADER);
		c->partition = p;
		c->self = ptr;
		c->next = PACK_NONE;
		k->cur[p] = c;
		k->cur_ptr[p] = ptr;
	}
	g = (char *) c + PACK_CHUNK_HEADER + c->used;
	c->used += (uint32) len;
	k->s->pk_groups++;
	return g;
}

/*
 * The batch is done with its groups: the chunks it filled onto their lists,
 * the shared ones, or written, the local ones.
 */
void
vexec_pack_batch_end(VexecPackSink *k)
{
	ListCell   *lc;

	if (k->full == NIL)
		return;
	foreach(lc, k->full)
	{
		PackChunk  *c = lfirst(lc);

		if (c->self != PACK_NONE)
			pack_push(k->s, c->partition, c, c->self);
		else
		{
			pack_write_chunk(k->s, c);
			pfree(c);
		}
	}
	list_free(k->full);
	k->full = NIL;
}

/*
 * This participant's groups, made by the partial VecAgg below in their
 * partitions' chunks; then each partition's last chunk retired.
 */
static void
write_packed(VexecRepartState *s)
{
	RepartShared *sh = s->shared;
	MemoryContext mcxt = s->node.mcxt;
	VexecPackSink *k = MemoryContextAllocZero(mcxt, sizeof(VexecPackSink));
	int			p;

	k->s = s;
	k->budget = vexec_repartition_shared_memory < 0 ? get_hash_memory_limit() :
		(Size) vexec_repartition_shared_memory * 1024;
	k->npartitions = sh->npartitions;
	k->cur = MemoryContextAllocZero(mcxt, sizeof(PackChunk *) * sh->npartitions);
	k->cur_ptr = MemoryContextAlloc(mcxt, sizeof(PackPointer) * sh->npartitions);
	k->next_size = MemoryContextAlloc(mcxt, sizeof(Size) * sh->npartitions);
	for (p = 0; p < sh->npartitions; p++)
	{
		k->cur_ptr[p] = PACK_NONE;
		k->next_size[p] = PACK_CHUNK_FIRST;
	}
	k->cxt = AllocSetContextCreate(mcxt, "VecRepartition chunks", ALLOCSET_DEFAULT_SIZES);
	s->sink = k;

	vexec_agg_pack((VexecNode *) s->child, k, sh->npartitions, deal_partition, s);
	vexec_agg_pack_all((VexecNode *) s->child);

	for (p = 0; p < sh->npartitions; p++)
		if (k->cur[p] != NULL)
			pack_retire(k, p);
	vexec_pack_batch_end(k);
	MemoryContextDelete(k->cxt);
	s->sink = NULL;
}

/*
 * Every other participant's segments mapped, once every one has written:
 * none goes, then, before this participant is done with it.
 */
static void
pack_map_all(VexecRepartState *s)
{
	RepartShared *sh = s->shared;

	for (int q = 0; q < sh->nparticipants; q++)
	{
		if (q == s->me)
			continue;
		for (uint32 i = 0; i < pack_nsegs(sh)[q]; i++)
		{
			dsm_segment *m;

			if (s->pk_maps[q * PACK_SEGMENTS + i] != NULL ||
				pack_kinds(sh)[q * PACK_SEGMENTS + i] == PACK_KIND_AREA)
				continue;		/* mapped as one of its chunks came, or the area's */
			m = dsm_attach(pack_handles(sh)[q * PACK_SEGMENTS + i]);
			if (m == NULL)
				elog(ERROR, "vexec: a parallel aggregation's segment is gone before it was read");
			s->pk_maps[q * PACK_SEGMENTS + i] = m;
		}
	}
}

/* The segments this participant mapped, its own too, let go. */
static void
pack_unmap_all(VexecRepartState *s)
{
	if (s->pk_maps == NULL)
		return;
	for (int i = 0; i < s->pk_nmaps; i++)
		if (s->pk_maps[i] != NULL)
		{
			dsm_detach(s->pk_maps[i]);
			s->pk_maps[i] = NULL;
		}
}

/* The shared chunks of a list, from ptr on, given back: no one reads them now. */
static void
pack_release_list(VexecRepartState *s, PackPointer ptr)
{
	while (ptr != PACK_NONE)
	{
		PackChunk  *c = pack_chunk(s, ptr);

		ptr = c->next;
		pack_release(c);
	}
}

/*
 * X9: a batch's rows rows[first .. first + n) as one frame, a record of the
 * partition's store; in halves where the frame would be longer than an
 * allocation may be.
 */
static void
write_frame_rows(VexecRepartState *s, int p, int first, int n)
{
	Size		len;
	char	   *rec;
	uint32		nrows = (uint32) n;

	rec = vexec_frame_rows_after(s->fw, s->allrows + first, n, REPART_FRAME_PREFIX, &len);
	if (rec == NULL)
	{
		if (n == 1)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("a group too long for a parallel aggregation's partition")));
		write_frame_rows(s, p, first, n / 2);
		write_frame_rows(s, p, first + n / 2, n - n / 2);
		return;
	}
	memset(rec, 0, REPART_FRAME_PREFIX);
	*(uint32 *) rec = (uint32) (REPART_FRAME_PREFIX + len);
	sts_puttuple(s->parts[p], &nrows, (MinimalTuple) rec);
	pfree(rec);
}

/*
 * X9: this participant's groups, dealt into the partitions by the partial
 * VecAgg below as they go out, a batch of one partition's at a time, each
 * written as one frame.
 */
static void
write_frames(VexecRepartState *s)
{
	VexecNode  *child = (VexecNode *) s->child;
	VexecBatch *b;

	vexec_agg_deal(child, s->shared->npartitions, deal_partition, s);
	while ((b = vexec_next_batch(s->child)) != NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(s->framecxt);
		uint64		schema;
		bytea	   *schema_frame;

		vexec_frame_begin(s->fw, b, &schema, &schema_frame);
		write_frame_rows(s, vexec_agg_dealt_partition(child), 0, b->nrows);
		MemoryContextSwitchTo(old);
		MemoryContextReset(s->framecxt);
	}
}

/*
 * This participant's groups, each a tuple of its partition: a batch's rows
 * at a time where the child hands up batches, else a row at a time.
 */
static void
write_tuples(VexecRepartState *s)
{
	if (s->child_batches)
	{
		int			natts = s->desc->natts;
		Datum	   *values = palloc(sizeof(Datum) * Max(natts, 1));
		bool	   *isnull = palloc(sizeof(bool) * Max(natts, 1));
		VexecBatch *b;

		while ((b = vexec_next_batch(s->child)) != NULL)
		{
			const uint32 *hashes = vexec_agg_batch_hashes((VexecNode *) s->child);

			VEXEC_FOREACH_ROW(b->selection, b->nrows, r)
			{
				uint32		hash = hashes[r];
				MinimalTuple tuple;

				for (int c = 0; c < natts; c++)
					values[c] = vexec_vec_datum(b, &b->cols[c], r, &isnull[c]);
				tuple = heap_form_minimal_tuple(s->desc, values, isnull, 0);
				sts_puttuple(s->parts[hash_partition(s, hash)], &hash, tuple);
				pfree(tuple);
			}
		}
		pfree(values);
		pfree(isnull);
		return;
	}
	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(s->child);
		MinimalTuple tuple;
		bool		should_free;
		uint32		hash;

		if (TupIsNull(slot))
			break;
		hash = row_hash(s, slot);
		tuple = ExecFetchSlotMinimalTuple(slot, &should_free);
		sts_puttuple(s->parts[hash_partition(s, hash)], &hash, tuple);
		if (should_free)
			heap_free_minimal_tuple(tuple);
	}
}

/*
 * Writing: this participant's groups, read from its child to their end,
 * each into its partition; then the barrier.  A participant that comes once
 * the barrier has passed on writes nothing (above).
 */
static void
write_groups(VexecRepartState *s)
{
	RepartShared *sh = s->shared;
	int			p;

	if (BarrierAttach(&sh->barrier) == REPART_WRITING)
	{
		if (sh->packed)
		{
			pg_atomic_fetch_add_u32(&sh->writing, 1);
			write_packed(s);
		}
		else if (s->frames)
			write_frames(s);
		else
			write_tuples(s);
		for (p = 0; p < sh->npartitions; p++)
			sts_end_write(s->parts[p]);
		if (repart_wait_event == 0)
			repart_wait_event = WaitEventExtensionNew("VexecRepartition");
		if (sh->packed)
		{
			/*
			 * D2: its share of the partitions read while others write, the
			 * barrier after (vexec_repart_next_chunk())
			 */
			pg_atomic_fetch_sub_u32(&sh->writing, 1);
			s->pk_early = true;
			s->pk_quota = Max(sh->npartitions / sh->nparticipants, 1);
			s->pk_nclaimed = 0;
			s->pk_scan = 0;
			s->pk_found = false;
			s->pk_cur = 0;
		}
		else
			BarrierArriveAndWait(&sh->barrier, repart_wait_event);
	}
	else if (sh->packed)
	{
		/* D2: too late to be sure of the segments: the others read it all */
		BarrierDetach(&sh->barrier);
	}
	s->written = true;
}

/* The next partition this participant claims, begun; false when none is left. */
static bool
claim_partition(VexecRepartState *s)
{
	RepartShared *sh = s->shared;
	uint32		p;

	if (sh->packed && !s->pk_reads)
		return false;
	/* D2: the partitions claimed while others wrote, first */
	if (sh->packed && s->pk_cur < s->pk_nclaimed)
		p = (uint32) s->pk_claimed[s->pk_cur++];
	else
	{
		p = pg_atomic_fetch_add_u32(&sh->next_partition, 1);
		if (p >= (uint32) sh->npartitions)
			return false;
	}
	s->reading = (int) p;
	sts_begin_parallel_scan(s->parts[p]);
	/* D2: the partition's shared chunks first -- those left -- then its records */
	if (sh->packed)
		s->pk_next = (PackPointer) pg_atomic_exchange_u64(&pack_heads(sh)[p], (uint64) PACK_NONE);
	return true;
}

/*
 * D2: while other participants write, the next chunk of the partitions this
 * one has claimed -- taken from their lists as they come -- claiming more
 * up to its share; NULL once none has a chunk now, or no participant
 * writes any more.
 */
static PackChunk *
take_early(VexecRepartState *s)
{
	RepartShared *sh = s->shared;

	for (;;)
	{
		if (s->pk_next != PACK_NONE)
		{
			PackChunk  *c = pack_chunk(s, s->pk_next);

			s->pk_next = c->next;
			return c;
		}
		if (pg_atomic_read_u32(&sh->writing) == 0)
			return NULL;		/* the rest after the barrier, at once */
		if (s->pk_scan < s->pk_nclaimed)
		{
			int			p = s->pk_claimed[s->pk_scan++];

			s->pk_next = (PackPointer) pg_atomic_exchange_u64(&pack_heads(sh)[p],
															  (uint64) PACK_NONE);
			if (s->pk_next != PACK_NONE)
				s->pk_found = true;
			continue;
		}
		if (s->pk_nclaimed < s->pk_quota)
		{
			uint32		p = pg_atomic_fetch_add_u32(&sh->next_partition, 1);

			if (p < (uint32) sh->npartitions)
			{
				s->pk_claimed[s->pk_nclaimed++] = (int) p;
				continue;
			}
			s->pk_quota = s->pk_nclaimed;	/* none left to claim */
		}
		if (!s->pk_found)
			return NULL;
		/* another pass: more chunks may have come */
		s->pk_found = false;
		s->pk_scan = 0;
	}
}

/* A partition's store's name, which its files' names begin with. */
static void
partition_name(VexecRepartState *s, int p, char *name)
{
	snprintf(name, MAXPGPATH, "vexec-repart-%d-%d", s->node.css.ss.ps.plan->plan_node_id, p);
}

/*
 * The partition being read, if any, closed, and its files deleted: one
 * participant reads a partition, so none reads them again.  Left to the
 * file set, they would go as the last participant detaches it, an exit the
 * Gather waits for at the end, where deleting the hundreds of megabytes a
 * grouping of millions writes takes tens of milliseconds; here they go as
 * the participants read, each its own.  The store names a participant's
 * file of it "<name>.p<participant>" (sharedtuplestore.c, sts_filename()).
 */
static void
end_reading(VexecRepartState *s)
{
	if (s->shared != NULL && s->reading >= 0)
	{
		char		name[MAXPGPATH];
		char		file[MAXPGPATH];

		sts_end_parallel_scan(s->parts[s->reading]);
		/* D2: its shared chunks given back, the one read last and any left */
		if (s->pk_done != NULL)
			pack_release(s->pk_done);
		if (s->shared->packed)
			pack_release_list(s, s->pk_next);
		s->pk_done = NULL;
		s->pk_next = PACK_NONE;
		partition_name(s, s->reading, name);
		for (int i = 0; i < s->shared->nparticipants; i++)
		{
			snprintf(file, sizeof(file), "%s.p%d", name, i);
			BufFileDeleteFileSet(&s->shared->fileset.fs, file, true);
		}
	}
	s->reading = -1;
}

/*
 * X9: a frame of a partition, read back: the node's input batch, its
 * columns pointing into the frame, which stays as it is until the next one
 * is read; the hashes from its last column.
 */
static void
read_frame(VexecRepartState *s, const char *rec, Size len)
{
	VexecNode  *node = &s->node;
	VexecBatch *in = s->own_in;
	int			natts = s->desc->natts;

	if (len < REPART_FRAME_PREFIX ||
		!vexec_frame_decode(s->fr, rec + REPART_FRAME_PREFIX, len - REPART_FRAME_PREFIX,
							s->frame_in))
		elog(ERROR, "vexec: a parallel aggregation's partition holds a record that is no batch's frame");
	vexec_batch_reset(in);
	for (int c = 0; c < natts; c++)
		in->cols[c] = s->frame_in->cols[c];
	in->nrows = s->frame_in->nrows;
	s->hashes = (const uint32 *) s->frame_in->cols[natts].values;
	node->in = in;
	vexec_batch_apply_config(in, &node->layout);
}

/* The next batch of groups: from the partitions this participant claims. */
static bool
repart_fetch(VexecNode *node)
{
	VexecRepartState *s = (VexecRepartState *) node;
	VexecBatch *in = s->own_in;

	if (s->child_batches && s->shared == NULL)
	{
		/* X9: one participant, the child's batches as they come */
		VexecBatch *b = vexec_next_batch(s->child);

		if (b == NULL)
			return false;
		node->in = b;
		s->hashes = vexec_agg_batch_hashes((VexecNode *) s->child);
		return true;
	}
	if (s->frames && s->shared != NULL)
	{
		if (!s->written)
			write_groups(s);
		for (;;)
		{
			MinimalTuple rec;
			uint32		nrows;

			if (s->reading < 0 && !claim_partition(s))
				return false;
			rec = sts_parallel_scan_next(s->parts[s->reading], &nrows);
			if (rec == NULL)
			{
				end_reading(s);
				continue;
			}
			read_frame(s, (const char *) rec, rec->t_len);
			return true;
		}
	}

	node->in = in;
	s->hashes = s->rowhash;
	vexec_batch_reset(in);
	vexec_batch_begin_rows(in);

	if (s->shared == NULL)
	{
		/* one participant: the child's groups as they come */
		while (in->nrows < VEXEC_BATCH_ROWS)
		{
			TupleTableSlot *slot = ExecProcNode(s->child);

			if (TupIsNull(slot))
				break;
			s->rowhash[in->nrows] = row_hash(s, slot);
			vexec_batch_add_slot(in, slot, NULL);
		}
	}
	else
	{
		if (!s->written)
			write_groups(s);
		while (in->nrows < VEXEC_BATCH_ROWS)
		{
			MinimalTuple tuple;
			uint32		hash;

			if (s->reading < 0 && !claim_partition(s))
				break;
			tuple = sts_parallel_scan_next(s->parts[s->reading], &hash);
			if (tuple == NULL)
			{
				end_reading(s);
				continue;
			}
			ExecStoreMinimalTuple(tuple, s->read_slot, false);
			s->rowhash[in->nrows] = hash;
			vexec_batch_add_slot(in, s->read_slot, NULL);
		}
		ExecClearTuple(s->read_slot);
	}
	if (in->nrows == 0)
		return false;
	vexec_batch_apply_config(in, &node->layout);
	return true;
}

static TupleTableSlot *
repart_exec(CustomScanState *css)
{
	return vexec_node_exec(&((VexecRepartState *) css)->node);
}

/*
 * C4: whether the node's batches carry the hashes a VecAgg grouping by these
 * keys -- columns of the node's output from 1, with these equality
 * operators and collations -- would compute: its own keys, in their order,
 * each a column of its scan tuple as it is.  vexec_repart_batch_hashes()
 * then gives each row's of the batch last handed up.
 */
bool
vexec_repart_hashes_keys(VexecNode *node, List *keycols, List *eqops, List *collations)
{
	VexecRepartState *s = (VexecRepartState *) node;
	List	   *tlist = node->css.ss.ps.plan->targetlist;

	if (node->kind != VEXEC_NODE_REPART || list_length(keycols) != s->nkeys)
		return false;
	for (int i = 0; i < s->nkeys; i++)
	{
		int			col = list_nth_int(keycols, i);
		TargetEntry *tle;
		Var		   *v;

		if (col < 1 || col > list_length(tlist))
			return false;
		tle = list_nth_node(TargetEntry, tlist, col - 1);
		if (!IsA(tle->expr, Var))
			return false;
		v = (Var *) tle->expr;
		if (v->varno != INDEX_VAR || v->varattno != s->keycols[i] ||
			list_nth_oid(s->plan.eqops, i) != list_nth_oid(eqops, i) ||
			list_nth_oid(s->plan.collations, i) != list_nth_oid(collations, i))
			return false;
	}
	return true;
}

/*
 * C5: the first stages' boundary, which the final stage above takes: the
 * participants', in the DSM, or, with no DSM, the one first stage's.
 */
uint32
vexec_repart_topk_bound(VexecNode *node)
{
	VexecRepartState *s = (VexecRepartState *) node;

	if (s->shared != NULL)
		return pg_atomic_read_u32(&s->shared->topk_bound);
	if (vexec_is_vector_state(s->child))
		return vexec_agg_topk_bound((VexecNode *) s->child);
	return PG_UINT32_MAX;
}

const uint32 *
vexec_repart_batch_hashes(VexecNode *node)
{
	return ((VexecRepartState *) node)->hashes;
}

/*
 * D2: the partial VecAgg below, whose groups go into the partitions by its
 * own keys' hash: the one that may pack them; else NULL.
 */
VexecNode *
vexec_repart_child_agg(VexecNode *node)
{
	VexecRepartState *s = (VexecRepartState *) node;

	if (node->kind != VEXEC_NODE_REPART || !s->hashes_from_child)
		return NULL;
	return (VexecNode *) s->child;
}

/*
 * D2: the final VecAgg above can merge the child's groups as they lie --
 * before the DSM is made, so that every participant's plan says the same;
 * whether the partitions will hold them so where there is a DSM.
 */
bool
vexec_repart_offer_unpack(VexecNode *node)
{
	VexecRepartState *s = (VexecRepartState *) node;

	s->pack_ok = vexec_enable_packed_groups && s->hashes_from_child &&
		vexec_agg_pack_ok((VexecNode *) s->child);
	return s->pack_ok;
}

/* D2: whether this run's partitions hold packed groups: with a DSM only. */
bool
vexec_repart_packed(VexecNode *node)
{
	VexecRepartState *s = (VexecRepartState *) node;

	return s->shared != NULL && s->shared->packed;
}

/*
 * D2: the next chunk of packed groups of the partitions this participant
 * claims, once its own are written: its groups, *len bytes of them, where
 * they lie -- in shared memory, or the record of a file -- until the next
 * call, which frees a shared one; false once none is left.  The groups'
 * count is the parent's to add to the node's rows.
 */
bool
vexec_repart_next_chunk(VexecNode *node, const char **groups, Size *len)
{
	VexecRepartState *s = (VexecRepartState *) node;
	PlanState  *ps = &node->css.ss.ps;

	Assert(vexec_repart_packed(node));
	/* a Gather run again rescans its subtree here, as ExecProcNode would */
	if (ps->chgParam != NULL)
		ExecReScan(ps);
	CHECK_FOR_INTERRUPTS();
	if (ps->instrument)
		InstrStartNode(ps->instrument);
	if (!s->written)
		write_groups(s);
	for (;;)
	{
		if (s->pk_done != NULL)
		{
			pack_release(s->pk_done);
			s->pk_done = NULL;
		}
		if (s->pk_early)
		{
			PackChunk  *c = take_early(s);

			if (c != NULL)
			{
				s->pk_done = c;
				s->pk_chunks_early++;
				*groups = (const char *) c + PACK_CHUNK_HEADER;
				*len = c->used;
				break;
			}
			/* the writers done, every segment mapped before any may go */
			BarrierArriveAndWait(&s->shared->barrier, repart_wait_event);
			pack_map_all(s);
			BarrierArriveAndWait(&s->shared->barrier, repart_wait_event);
			s->pk_early = false;
			s->pk_reads = true;
			continue;
		}
		if (s->reading < 0 && !claim_partition(s))
		{
			if (ps->instrument)
				InstrStopNode(ps->instrument, 0);
			return false;
		}
		if (s->pk_next != PACK_NONE)
		{
			PackChunk  *c = pack_chunk(s, s->pk_next);

			s->pk_next = c->next;
			s->pk_done = c;
			*groups = (const char *) c + PACK_CHUNK_HEADER;
			*len = c->used;
			break;
		}
		else
		{
			uint32		used;
			MinimalTuple rec = sts_parallel_scan_next(s->parts[s->reading], &used);

			if (rec == NULL)
			{
				end_reading(s);
				continue;
			}
			if (rec->t_len < PACK_CHUNK_HEADER ||
				((PackChunk *) rec)->used != rec->t_len - PACK_CHUNK_HEADER)
				elog(ERROR, "vexec: a parallel aggregation's partition holds a record that is no chunk of groups");
			*groups = (const char *) rec + PACK_CHUNK_HEADER;
			*len = ((PackChunk *) rec)->used;
			break;
		}
	}
	s->pk_chunks_read++;
	if (ps->instrument)
		InstrStopNode(ps->instrument, 0);
	return true;
}

/* ---------------------------------------------------------------------
 * The DSM: the shared file set, the barrier, a SharedTuplestore a partition
 * ---------------------------------------------------------------------
 */

static int
repart_partitions(int nparticipants)
{
	return Min(Max(8, 4 * nparticipants), 64);
}

static Size
repart_estimate_dsm(CustomScanState *css, ParallelContext *pcxt)
{
	int			nparticipants = pcxt->nworkers + 1;

	Size		size = add_size(offsetof(RepartShared, stores),
								mul_size(repart_partitions(nparticipants),
										 MAXALIGN(sts_estimate(nparticipants))));

	/* D2: the lists, the segments' handles and their counts */
	size = add_size(size, mul_size(repart_partitions(nparticipants), sizeof(pg_atomic_uint64)));
	size = add_size(size, mul_size(nparticipants, PACK_SEGMENTS * sizeof(dsm_handle)));
	size = add_size(size, mul_size(nparticipants, sizeof(uint32)));
	/* M1: the segments' kinds */
	return add_size(size, mul_size(nparticipants, PACK_SEGMENTS * sizeof(uint8)));
}

/*
 * M1: every participant's exchange area blocks given back, once no process
 * holding the parallel query's DSM reads them -- by the last to let go of
 * it: the leader at the query's end, or a worker that outlived the leader's
 * part in an error's cleanup.  The DSM holds the lists, and is still mapped
 * while its detach callbacks run.
 */
static void
area_release(RepartShared *sh)
{
	for (int q = 0; q < sh->nparticipants; q++)
	{
		uint32		n = pack_nsegs(sh)[q];

		for (uint32 i = 0; i < n; i++)
		{
			int			k = q * PACK_SEGMENTS + (int) i;

			if (pack_kinds(sh)[k] != PACK_KIND_AREA)
				continue;
			pack_kinds(sh)[k] = PACK_KIND_DSM;	/* given back once */
			vexec_exchange_release((int32) pack_handles(sh)[k]);
		}
	}
}

static void
area_detach(dsm_segment *seg, Datum arg)
{
	RepartShared *sh = (RepartShared *) DatumGetPointer(arg);

	(void) seg;
	if (pg_atomic_sub_fetch_u32(&sh->area_users, 1) == 0)
		area_release(sh);
}

/*
 * M1: a worker counted among the area's users, where the leader still is
 * one: a worker that comes once the others let go -- the query ending in an
 * error -- finds blocks given back, which it must not read.
 */
static void
area_join(RepartShared *sh, dsm_segment *seg)
{
	uint32		n = pg_atomic_read_u32(&sh->area_users);

	while (n > 0)
		if (pg_atomic_compare_exchange_u32(&sh->area_users, &n, n + 1))
		{
			on_dsm_detach(seg, area_detach, PointerGetDatum(sh));
			return;
		}
	elog(ERROR, "vexec: a parallel aggregation's partitions are gone before a worker came to read them");
}

/* Each partition's store, made: the leader's, participant 0. */
static void
make_stores(VexecRepartState *s)
{
	RepartShared *sh = s->shared;
	int			p;

	for (p = 0; p < sh->npartitions; p++)
	{
		char		name[MAXPGPATH];

		partition_name(s, p, name);
		s->parts[p] = sts_initialize((SharedTuplestore *) (sh->stores + p * sh->sts_size),
									 sh->nparticipants, 0, sizeof(uint32),
									 SHARED_TUPLESTORE_SINGLE_PASS, &sh->fileset, name);
	}
}

static void
repart_initialize_dsm(CustomScanState *css, ParallelContext *pcxt, void *coordinate)
{
	VexecRepartState *s = (VexecRepartState *) css;
	RepartShared *sh = (RepartShared *) coordinate;

	sh->nparticipants = pcxt->nworkers + 1;
	sh->npartitions = repart_partitions(sh->nparticipants);
	sh->frames = s->frames;
	sh->packed = s->pack_ok;
	pg_atomic_init_u32(&sh->topk_bound, PG_UINT32_MAX);
	if (vexec_is_vector_state(s->child))
		vexec_agg_set_topk_shared((VexecNode *) s->child, &sh->topk_bound);
	sh->sts_size = MAXALIGN(sts_estimate(sh->nparticipants));
	sh->heads_off = offsetof(RepartShared, stores) + sh->npartitions * sh->sts_size;
	sh->segs_off = sh->heads_off + sh->npartitions * sizeof(pg_atomic_uint64);
	sh->nsegs_off = sh->segs_off + sh->nparticipants * PACK_SEGMENTS * sizeof(dsm_handle);
	sh->kinds_off = sh->nsegs_off + sh->nparticipants * sizeof(uint32);
	for (int p = 0; p < sh->npartitions; p++)
		pg_atomic_init_u64(&pack_heads(sh)[p], (uint64) PACK_NONE);
	memset(pack_nsegs(sh), 0, sizeof(uint32) * sh->nparticipants);
	memset(pack_kinds(sh), PACK_KIND_DSM, sh->nparticipants * PACK_SEGMENTS);
	/* M1: the area, where the server has one and the session takes it; the leader its first user */
	sh->area = sh->packed && vexec_exchange_enabled();
	pg_atomic_init_u32(&sh->area_users, sh->area ? 1 : 0);
	if (sh->area)
		on_dsm_detach(pcxt->seg, area_detach, PointerGetDatum(sh));
	pg_atomic_init_u32(&sh->writing, 0);
	s->me = 0;
	s->pk_nmaps = sh->nparticipants * PACK_SEGMENTS;
	s->pk_maps = MemoryContextAllocZero(s->node.mcxt, sizeof(dsm_segment *) * s->pk_nmaps);
	s->pk_claimed = MemoryContextAlloc(s->node.mcxt, sizeof(int) * sh->npartitions);
	sh->seg = dsm_segment_handle(pcxt->seg);
	SharedFileSetInit(&sh->fileset, pcxt->seg);
	BarrierInit(&sh->barrier, 0);
	pg_atomic_init_u32(&sh->next_partition, 0);
	s->shared = sh;
	s->npartitions = sh->npartitions;
	s->parts = MemoryContextAllocZero(s->node.mcxt,
									  sizeof(SharedTuplestoreAccessor *) * sh->npartitions);
	make_stores(s);
	s->written = false;
	s->reading = -1;
}

/*
 * A Gather run again: its participants are new processes, and the
 * partitions are made again, their files gone.
 */
static void
repart_reinitialize_dsm(CustomScanState *css, ParallelContext *pcxt, void *coordinate)
{
	VexecRepartState *s = (VexecRepartState *) css;
	RepartShared *sh = (RepartShared *) coordinate;

	(void) pcxt;
	end_reading(s);
	/*
	 * D2: the segments let go, the lists empty: the participants are new.
	 * M1: the last run's area blocks given back -- its workers have exited,
	 * and the leader alone holds the DSM (ReinitializeParallelDSM())
	 */
	pack_unmap_all(s);
	if (sh->area)
		area_release(sh);
	for (int p = 0; p < sh->npartitions; p++)
		pg_atomic_write_u64(&pack_heads(sh)[p], (uint64) PACK_NONE);
	memset(pack_nsegs(sh), 0, sizeof(uint32) * sh->nparticipants);
	memset(pack_kinds(sh), PACK_KIND_DSM, sh->nparticipants * PACK_SEGMENTS);
	pg_atomic_write_u32(&sh->writing, 0);
	s->pk_reads = false;
	s->pk_early = false;
	s->pk_nclaimed = 0;
	s->pk_cur = 0;
	SharedFileSetDeleteAll(&sh->fileset);
	BarrierInit(&sh->barrier, 0);
	pg_atomic_write_u32(&sh->next_partition, 0);
	pg_atomic_write_u32(&sh->topk_bound, PG_UINT32_MAX);
	make_stores(s);
	s->written = false;
}

static void
repart_initialize_worker(CustomScanState *css, shm_toc *toc, void *coordinate)
{
	VexecRepartState *s = (VexecRepartState *) css;
	RepartShared *sh = (RepartShared *) coordinate;
	dsm_segment *seg = dsm_find_mapping(sh->seg);
	int			p;

	(void) toc;
	if (seg == NULL)
		elog(ERROR, "vexec: a worker has no mapping of its parallel query's segment");
	if (sh->frames != s->frames || sh->packed != s->pack_ok)
		elog(ERROR, "vexec: a VecRepartition's participants differ on its partitions' format");
	if (vexec_is_vector_state(s->child))
		vexec_agg_set_topk_shared((VexecNode *) s->child, &sh->topk_bound);
	SharedFileSetAttach(&sh->fileset, seg);
	if (sh->area)
		area_join(sh, seg);
	s->shared = sh;
	s->npartitions = sh->npartitions;
	s->me = ParallelWorkerNumber + 1;
	s->pk_nmaps = sh->nparticipants * PACK_SEGMENTS;
	s->pk_maps = MemoryContextAllocZero(s->node.mcxt, sizeof(dsm_segment *) * s->pk_nmaps);
	s->pk_claimed = MemoryContextAlloc(s->node.mcxt, sizeof(int) * sh->npartitions);
	s->parts = MemoryContextAllocZero(s->node.mcxt,
									  sizeof(SharedTuplestoreAccessor *) * sh->npartitions);
	for (p = 0; p < sh->npartitions; p++)
		s->parts[p] = sts_attach((SharedTuplestore *) (sh->stores + p * sh->sts_size),
								 ParallelWorkerNumber + 1, &sh->fileset);
}

/*
 * A rescan resets this participant's state; the shared state is made again
 * with the DSM, by the Gather's ReInitializeDSM, before the next fetch.
 */
static void
repart_rescan(CustomScanState *css)
{
	VexecRepartState *s = (VexecRepartState *) css;

	end_reading(s);
	if (s->child->chgParam == NULL)
		ExecReScan(s->child);
	s->written = false;
	vexec_node_rescan(&s->node);
}

/*
 * Shutdown: the Gather's comes after its children's and detaches the DSM,
 * so the node lets go of it now -- EXPLAIN ANALYZE, and the node's end,
 * come after.  A Gather run again makes a new DSM, and InitializeDSM gives
 * the node it.
 */
static void
repart_shutdown(CustomScanState *css)
{
	VexecRepartState *s = (VexecRepartState *) css;

	end_reading(s);
	pack_unmap_all(s);
	s->shared = NULL;
	s->parts = NULL;
}

static void
repart_end(CustomScanState *css)
{
	VexecRepartState *s = (VexecRepartState *) css;

	end_reading(s);
	vexec_node_end(&s->node);
	ExecEndNode(s->child);
}

static void
repart_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecRepartState *s = (VexecRepartState *) css;
	List	   *context = set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan, ancestors);
	List	   *keys = NIL;
	int			i;

	vexec_node_relabel(&s->node, es);
	for (i = 0; i < list_length(s->plan.keycols); i++)
	{
		int			col = list_nth_int(s->plan.keycols, i);
		Form_pg_attribute att = TupleDescAttr(s->desc, col - 1);
		Var		   *v = makeVar(OUTER_VAR, col, att->atttypid, att->atttypmod,
								att->attcollation, 0);

		keys = lappend(keys, deparse_expression((Node *) v, context, es->verbose, false));
	}
	ExplainPropertyList("Partition Key", keys, es);
	if (es->verbose)
	{
		ExplainPropertyText("Key Hashes", s->hashes_from_child ? "the child's" : "computed", es);
		ExplainPropertyText("Input", s->child_batches ? "batches" : "rows", es);
		ExplainPropertyText("Partition Format", s->pack_ok ? "packed groups" :
							s->frames ? "frames" : "tuples", es);
	}
	vexec_node_explain_properties(&s->node, ancestors, es);
	if (es->analyze && s->npartitions > 0)
		ExplainPropertyInteger("Partitions", NULL, s->npartitions, es);
	if (es->analyze && (s->pk_groups > 0 || s->pk_chunks_read > 0))
	{
		ExplainPropertyInteger("Packed Groups Written", NULL, s->pk_groups, es);
		ExplainPropertyInteger("Shared Memory", "kB", (int64) ((s->pk_shared + 1023) / 1024), es);
		if (s->pk_area_blocks > 0)
			ExplainPropertyInteger("Exchange Area Blocks", NULL, s->pk_area_blocks, es);
		if (s->pk_files > 0)
			ExplainPropertyInteger("Files", "kB", (int64) ((s->pk_files + 1023) / 1024), es);
		ExplainPropertyInteger("Chunks Read", NULL, s->pk_chunks_read, es);
		if (s->pk_chunks_early > 0)
			ExplainPropertyInteger("Chunks Read Beside Writers", NULL, s->pk_chunks_early, es);
	}
}
