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
 * participant's first rows.  The hash is the keys' hash functions of the
 * grouping's equality operators (get_op_hash_functions()), so keys a
 * grouping takes for equal meet in one partition.
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
 * Its scan tuple, which custom_scan_tlist describes, is its child's row, as
 * VecSort's (vecsort.c); its keys are columns of it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
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
#include "storage/dsm.h"
#include "storage/sharedfileset.h"
#include "utils/lsyscache.h"
#include "utils/sharedtuplestore.h"
#include "utils/ruleutils.h"
#include "utils/wait_event.h"

#include "cb_explain.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"

#define REPART_WRITING	0		/* the barrier's phases */
#define REPART_READING	1

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
	FmgrInfo   *hashfns;
	Oid		   *collations;

	RepartShared *shared;		/* NULL: no DSM, one participant */
	SharedTuplestoreAccessor **parts;
	TupleTableSlot *read_slot;
	bool		written;		/* this participant's groups are written, or
								 * it came too late to write */
	int			reading;		/* the partition being read, or -1 */
	int			npartitions;	/* the DSM's, for EXPLAIN ANALYZE: the DSM
								 * goes before EXPLAIN prints */
} VexecRepartState;

static bool repart_fetch(VexecNode *node);
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

	/* the keys' hash functions, those of the grouping's equality operators */
	s->nkeys = list_length(s->plan.keycols);
	s->keycols = palloc(sizeof(AttrNumber) * Max(s->nkeys, 1));
	s->hashfns = palloc(sizeof(FmgrInfo) * Max(s->nkeys, 1));
	s->collations = palloc(sizeof(Oid) * Max(s->nkeys, 1));
	for (i = 0; i < s->nkeys; i++)
	{
		RegProcedure lhs;
		RegProcedure rhs;

		s->keycols[i] = (AttrNumber) list_nth_int(s->plan.keycols, i);
		if (!get_op_hash_functions(list_nth_oid(s->plan.eqops, i), &lhs, &rhs))
			elog(ERROR, "vexec: no hash function for operator %u", list_nth_oid(s->plan.eqops, i));
		fmgr_info(lhs, &s->hashfns[i]);
		s->collations[i] = list_nth_oid(s->plan.collations, i);
	}

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
	node->in = vexec_batch_create(node->mcxt, scandesc->natts, types);
	vexec_node_compile(node, cscan->scan.plan.qual, cscan->scan.plan.targetlist);

	s->read_slot = ExecInitExtraTupleSlot(estate, s->desc, &TTSOpsMinimalTuple);
	s->reading = -1;
}

/* A row's partition: the hash of its keys, as a grouping hashes them. */
static int
row_partition(VexecRepartState *s, TupleTableSlot *slot)
{
	uint32		hash = 0;
	int			i;

	slot_getallattrs(slot);
	for (i = 0; i < s->nkeys; i++)
	{
		int			a = s->keycols[i] - 1;

		hash = pg_rotate_left32(hash, 1);
		if (!slot->tts_isnull[a])
			hash ^= DatumGetUInt32(FunctionCall1Coll(&s->hashfns[i], s->collations[i],
													 slot->tts_values[a]));
	}
	hash = murmurhash32(hash);
	return (int) (hash % (uint32) s->shared->npartitions);
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
		for (;;)
		{
			TupleTableSlot *slot = ExecProcNode(s->child);
			MinimalTuple tuple;
			bool		should_free;

			if (TupIsNull(slot))
				break;
			p = row_partition(s, slot);
			tuple = ExecFetchSlotMinimalTuple(slot, &should_free);
			sts_puttuple(s->parts[p], NULL, tuple);
			if (should_free)
				heap_free_minimal_tuple(tuple);
		}
		for (p = 0; p < sh->npartitions; p++)
			sts_end_write(s->parts[p]);
		if (repart_wait_event == 0)
			repart_wait_event = WaitEventExtensionNew("VexecRepartition");
		BarrierArriveAndWait(&sh->barrier, repart_wait_event);
	}
	s->written = true;
}

/* The next partition this participant claims, begun; false when none is left. */
static bool
claim_partition(VexecRepartState *s)
{
	RepartShared *sh = s->shared;
	uint32		p = pg_atomic_fetch_add_u32(&sh->next_partition, 1);

	if (p >= (uint32) sh->npartitions)
		return false;
	s->reading = (int) p;
	sts_begin_parallel_scan(s->parts[p]);
	return true;
}

/* The partition being read, if any, closed: this participant's own file. */
static void
end_reading(VexecRepartState *s)
{
	if (s->shared != NULL && s->reading >= 0)
		sts_end_parallel_scan(s->parts[s->reading]);
	s->reading = -1;
}

/* The next batch of groups: from the partitions this participant claims. */
static bool
repart_fetch(VexecNode *node)
{
	VexecRepartState *s = (VexecRepartState *) node;
	VexecBatch *in = node->in;

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

			if (s->reading < 0 && !claim_partition(s))
				break;
			tuple = sts_parallel_scan_next(s->parts[s->reading], NULL);
			if (tuple == NULL)
			{
				end_reading(s);
				continue;
			}
			ExecStoreMinimalTuple(tuple, s->read_slot, false);
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

	return add_size(offsetof(RepartShared, stores),
					mul_size(repart_partitions(nparticipants),
							 MAXALIGN(sts_estimate(nparticipants))));
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

		snprintf(name, sizeof(name), "vexec-repart-%d-%d", s->node.css.ss.ps.plan->plan_node_id, p);
		s->parts[p] = sts_initialize((SharedTuplestore *) (sh->stores + p * sh->sts_size),
									 sh->nparticipants, 0, 0, SHARED_TUPLESTORE_SINGLE_PASS,
									 &sh->fileset, name);
	}
}

static void
repart_initialize_dsm(CustomScanState *css, ParallelContext *pcxt, void *coordinate)
{
	VexecRepartState *s = (VexecRepartState *) css;
	RepartShared *sh = (RepartShared *) coordinate;

	sh->nparticipants = pcxt->nworkers + 1;
	sh->npartitions = repart_partitions(sh->nparticipants);
	sh->sts_size = MAXALIGN(sts_estimate(sh->nparticipants));
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
	SharedFileSetDeleteAll(&sh->fileset);
	BarrierInit(&sh->barrier, 0);
	pg_atomic_write_u32(&sh->next_partition, 0);
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
	SharedFileSetAttach(&sh->fileset, seg);
	s->shared = sh;
	s->npartitions = sh->npartitions;
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
	vexec_node_explain_properties(&s->node, ancestors, es);
	if (es->analyze && s->npartitions > 0)
		ExplainPropertyInteger("Partitions", NULL, s->npartitions, es);
}
