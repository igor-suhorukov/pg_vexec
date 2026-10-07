/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecmotion.c
 *	  VecMotionSend and VecMotionRecv: a vector node's batches across one of
 *	  ORCA's Motions as Arrow IPC frames (pg_vector_executor.md §3.10, step
 *	  A; motion/motion.h).
 *
 * VecMotionSend is the top of a Motion's fragment, in the process that
 * sends the Motion's rows.  It reads its child's batches -- through the
 * batch interface where the child hands them up, else its rows a batch at
 * a time, as VecAgg's -- and gives the Motion one row for each frame: the
 * child's columns, all NULL, the segment the frame goes to, and the frame.
 * A Redistribute's keys, the plan's custom_exprs, are its node's targets,
 * which the vector cdbhash hashes a batch at a time (motion/cdbhash.c);
 * a key PostgreSQL's evaluator is to compute for a row -- a row a kernel
 * marked, a lazy key, a legacy key's -- goes row by row through gp_core's
 * own cdbhash (GpCoreApi.hash_segment).  The rows are then partitioned by
 * segment and each segment's sent as a frame, after the schema of their
 * shapes where that segment has not been sent it; a random redistribution
 * deals each batch to the next segment in turn, from one chosen at random;
 * a Broadcast and a Gather send each batch whole.  A child's row still to
 * be resolved, which the child hands up undecided, is resolved here in row
 * order, and its values sent in a batch of such rows of their own.
 *
 * VecMotionRecv is the Motion's parent, in the process that receives it.
 * Its scan tuple is the fragment's row, which a frame's batch holds; its
 * qual and target list are what the Motion's were, reading that row.  It
 * reads the Motion's rows, keeps the schemas they bring, and takes each
 * frame of a batch as its input batch, its buffers pointing into the
 * frame (motion/frame.c), which stays as it is until the Motion is read
 * again: the node reads it only for its next batch.
 *
 * Rows go across in frames whatever the format: under the Arrow format a
 * batch's buffers as they are, under the PostgreSQL format its varlena
 * values written whole.  A row of the Motion's no longer carries one row,
 * so a sorted Gather, whose merge compares rows, is never framed (plan/
 * motion.c), and a Motion's rescan, which reads again what its senders
 * sent, reads frames again.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/memutils.h"
#include "utils/ruleutils.h"
#include "varatt.h"

#include "cb_explain.h"
#include "gp_core_api.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "expr/expr.h"
#include "motion/frame.h"
#include "motion/motion.h"

/* A frame queued for the Motion: the segment it goes to, and its bytes. */
typedef struct QueuedFrame
{
	int			target;
	bytea	   *frame;
	bool		schema;
} QueuedFrame;

/* A schema the sender has sent, and the segments it has been sent to. */
typedef struct SentSchema
{
	uint64		hash;
	bool	   *to;				/* by target, nsegs + 1 of them */
} SentSchema;

typedef struct VexecMotionSendState
{
	VexecNode	node;			/* first */
	VexecMotionSendPlan plan;
	PlanState  *child;
	bool		child_batches;	/* the child's batches; else its rows */
	bool		child_done;
	TupleDesc	desc;			/* the child's rows */
	VexecBatch *rowbatch;		/* the child's rows, a batch of them */
	VexecBatch *resolved;		/* a batch's rows the child resolved */
	int		   *resolved_targets;

	int			nsegs;
	int			nkeys;
	VexecCdbKey *keys;
	void	   *row_hash;		/* gp_core's cdbhash, row by row */
	Datum	   *keyvalues;
	bool	   *keynulls;
	int			next_segment;	/* a random redistribution's next */

	VexecFrameWriter *writer;
	List	   *sent;			/* SentSchema */
	MemoryContext batchcxt;		/* a batch's frames, reset with the next */
	QueuedFrame *queue;
	int			nqueued;
	int			maxqueued;
	int			next;
	bool		done;

	VexecMotionStats mstats;
} VexecMotionSendState;

typedef struct VexecMotionRecvState
{
	VexecNode	node;			/* first */
	PlanState  *child;			/* the Motion */
	int			ncols;
	int			framecol;		/* the Motion's column of frames, from 0 */
	VexecFrameReader *reader;
	VexecBatch *frames_in;		/* the batch a frame fills */

	VexecMotionStats mstats;
} VexecMotionRecvState;

VexecMotionStats *
vexec_motion_stats(VexecNode *node)
{
	if (node->kind == VEXEC_NODE_MOTION_SEND)
		return &((VexecMotionSendState *) node)->mstats;
	Assert(node->kind == VEXEC_NODE_MOTION_RECV);
	return &((VexecMotionRecvState *) node)->mstats;
}

/* ---------------------------------------------------------------------
 * The plan
 * ---------------------------------------------------------------------
 */

List *
vexec_motion_send_plan_encode(const VexecMotionSendPlan *plan)
{
	return list_make4(makeInteger(plan->motion), makeBoolean(plan->legacy),
					  list_copy(plan->hashfuncs), makeInteger(plan->ncols));
}

void
vexec_motion_send_plan_decode(CustomScan *cscan, VexecMotionSendPlan *plan)
{
	List	   *p = cscan->custom_private;

	if (list_length(p) != 4)
		elog(ERROR, "vexec: a VecMotionSend's plan of %d parts", list_length(p));
	plan->motion = intVal(linitial(p));
	plan->legacy = boolVal(lsecond(p));
	plan->hashfuncs = lthird(p);
	plan->ncols = intVal(lfourth(p));
	if (list_length(plan->hashfuncs) != list_length(cscan->custom_exprs))
		elog(ERROR, "vexec: a VecMotionSend of %d keys and %d hash functions",
			 list_length(cscan->custom_exprs), list_length(plan->hashfuncs));
}

/* ---------------------------------------------------------------------
 * VecMotionSend
 * ---------------------------------------------------------------------
 */

static bool send_fetch(VexecNode *node);
static void send_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *send_exec(CustomScanState *css);
static void send_end(CustomScanState *css);
static void send_rescan(CustomScanState *css);
static void send_explain(CustomScanState *css, List *ancestors, ExplainState *es);

static const CustomExecMethods send_exec_methods = {
	.CustomName = VEXEC_MOTION_SEND_NAME,
	.BeginCustomScan = send_begin,
	.ExecCustomScan = send_exec,
	.EndCustomScan = send_end,
	.ReScanCustomScan = send_rescan,
	.ExplainCustomScan = send_explain,
};

Node *
vexec_create_motion_send_state(CustomScan *cscan)
{
	VexecMotionSendState *s = palloc0(sizeof(VexecMotionSendState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &send_exec_methods;
	s->node.kind = VEXEC_NODE_MOTION_SEND;
	return (Node *) s;
}

static void
send_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecMotionSendState *s = (VexecMotionSendState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	const GpCoreApi *api = vexec_gp_core();
	VexecType **types;
	List	   *keytl = NIL;
	ListCell   *lc;
	int			i;

	vexec_node_begin(node, estate);
	node->fetch = send_fetch;
	node->label = "Vec Motion Send";
	vexec_motion_send_plan_decode(cscan, &s->plan);

	s->child = ExecInitNode(outerPlan(cscan), estate,
							eflags & ~(EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK));
	outerPlanState(css) = s->child;
	s->desc = ExecGetResultType(s->child);
	if (s->desc->natts != s->plan.ncols)
		elog(ERROR, "vexec: a VecMotionSend of %d columns over a child of %d",
			 s->plan.ncols, s->desc->natts);
	s->child_batches = vexec_is_vector_state(s->child) &&
		vexec_node_batchable((VexecNode *) s->child);
	if (s->child_batches)
		node->vec_child = (VexecNode *) s->child;

	/* the input, the child's row; the targets, the keys */
	node->input_varno = OUTER_VAR;
	node->ninput = s->desc->natts;
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(s->desc->natts, 1));
	types = palloc(sizeof(VexecType *) * Max(s->desc->natts, 1));
	for (i = 0; i < s->desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(s->desc, i);

		node->input_attnos[i] = i + 1;
		types[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
	}
	node->input_slot = ExecInitExtraTupleSlot(estate, s->desc, &TTSOpsVirtual);
	foreach(lc, cscan->custom_exprs)
		keytl = lappend(keytl, makeTargetEntry((Expr *) lfirst(lc), list_length(keytl) + 1,
											   NULL, false));
	vexec_node_compile(node, NIL, keytl);
	s->rowbatch = vexec_batch_create(node->mcxt, s->desc->natts, types);
	s->resolved = vexec_batch_create(node->mcxt, s->desc->natts, types);
	s->resolved_targets = palloc(sizeof(int) * VEXEC_BATCH_ROWS);

	/*
	 * Where the frames go: as many segments as gp_core hashes over, as a
	 * Redistribute's senders do (gp_motion.c, motion_begin_sending()).  Under
	 * EXPLAIN alone, on the coordinator, nothing is sent.
	 */
	s->nsegs = api != NULL ? api->get_segment_count() : 1;
	s->nkeys = list_length(cscan->custom_exprs);
	s->keys = palloc0(sizeof(VexecCdbKey) * Max(s->nkeys, 1));
	s->keyvalues = palloc(sizeof(Datum) * Max(s->nkeys, 1));
	s->keynulls = palloc(sizeof(bool) * Max(s->nkeys, 1));
	if (s->nkeys > 0 && !(eflags & EXEC_FLAG_EXPLAIN_ONLY))
	{
		Oid		   *funcs = palloc(sizeof(Oid) * s->nkeys);

		for (i = 0; i < s->nkeys; i++)
		{
			funcs[i] = list_nth_oid(s->plan.hashfuncs, i);
			vexec_cdbhash_prepare(&s->keys[i], funcs[i], node->targets[i].type);
		}
		if (api == NULL)
			elog(ERROR, "vexec: a VecMotionSend's Redistribute with no gp_core of API 1.%d",
				 VEXEC_GP_CORE_MINOR);
		s->row_hash = api->hash_make(s->nsegs, s->nkeys, funcs);
	}
	s->next_segment = (int) (pg_prng_uint32(&pg_global_prng_state) % (uint32) Max(s->nsegs, 1));
	s->writer = vexec_frame_writer_create(node->mcxt, s->desc->natts, types);
	s->batchcxt = AllocSetContextCreate(node->mcxt, "vexec frames", ALLOCSET_DEFAULT_SIZES);
}

/* The next input batch: the child's, or its rows. */
static bool
send_fetch(VexecNode *node)
{
	VexecMotionSendState *s = (VexecMotionSendState *) node;
	VexecBatch *in;

	if (s->child_batches)
	{
		VexecNode  *child = node->vec_child;
		VexecBatch *b = vexec_next_batch(s->child);

		if (b == NULL)
			return false;
		node->in = b;
		if (vexec_bits_any(child->redo, b->nrows) ||
			(child->child_redo != NULL && vexec_bits_any(child->child_redo, b->nrows)))
		{
			node->child_redo = vexec_bits_copy(node->work, child->redo, b->nrows);
			if (child->child_redo != NULL)
				vexec_bits_or_into(node->child_redo, child->child_redo, b->nrows);
		}
		return true;
	}

	in = s->rowbatch;
	vexec_batch_reset(in);
	vexec_batch_begin_rows(in);
	while (!s->child_done && in->nrows < VEXEC_BATCH_ROWS)
	{
		TupleTableSlot *slot = ExecProcNode(s->child);

		if (TupIsNull(slot))
		{
			s->child_done = true;
			break;
		}
		slot_getsomeattrs(slot, s->desc->natts);
		vexec_batch_add_values(in, slot->tts_values, slot->tts_isnull);
	}
	if (in->nrows == 0)
		return false;
	vexec_batch_apply_config(in, &node->layout);
	node->in = in;
	return true;
}

/* A frame for the Motion, to a segment: its schema first, where it is new there. */
static void
enqueue(VexecMotionSendState *s, int target, bytea *frame, bool schema)
{
	if (s->nqueued == s->maxqueued)
	{
		int			newmax = Max(16, s->maxqueued * 2);

		if (s->queue == NULL)
			s->queue = MemoryContextAlloc(s->node.mcxt, sizeof(QueuedFrame) * newmax);
		else
			s->queue = repalloc(s->queue, sizeof(QueuedFrame) * newmax);
		s->maxqueued = newmax;
	}
	s->queue[s->nqueued].target = target;
	s->queue[s->nqueued].frame = frame;
	s->queue[s->nqueued].schema = schema;
	s->nqueued++;
	if (schema)
		s->mstats.schema_frames++;
	else
		s->mstats.frames++;
	s->mstats.bytes += VARSIZE(frame) - VARHDRSZ;
}

/* Whether a segment has been sent a schema, and that it is now. */
static bool
schema_sent(VexecMotionSendState *s, uint64 hash, int target)
{
	SentSchema *found = NULL;
	int			slot = target < 0 ? s->nsegs : target;

	foreach_ptr(SentSchema, ss, s->sent)
		if (ss->hash == hash)
			found = ss;
	if (found == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		found = palloc0(sizeof(SentSchema));
		found->hash = hash;
		found->to = palloc0(sizeof(bool) * (s->nsegs + 1));
		s->sent = lappend(s->sent, found);
		MemoryContextSwitchTo(old);
	}
	if (found->to[slot])
		return true;
	found->to[slot] = true;
	return false;
}

/*
 * The frames of rows[0 .. n) of the writer's batch to a segment, halved
 * where one would be longer than a bytea may be.
 */
static void
send_rows(VexecMotionSendState *s, int target, const int *rows, int n, uint64 schema,
		  bytea *schema_frame)
{
	bytea	   *frame;

	if (n == 0)
		return;
	frame = vexec_frame_rows(s->writer, rows, n);
	if (frame == NULL)
	{
		if (n == 1)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("a row is too long for a vector Motion's frame")));
		send_rows(s, target, rows, n / 2, schema, schema_frame);
		send_rows(s, target, rows + n / 2, n - n / 2, schema, schema_frame);
		return;
	}
	if (!schema_sent(s, schema, target))
		enqueue(s, target, schema_frame, true);
	enqueue(s, target, frame, false);
	s->mstats.rows += n;
}

/*
 * A batch's rows to their segments, targets[] each's: partitioned, each
 * segment's rows a frame, in the rows' order.  With targets NULL, every row
 * to "target".
 */
static void
send_batch(VexecMotionSendState *s, VexecBatch *batch, const int *rows, const int *targets,
		   int n, int target)
{
	uint64		schema;
	bytea	   *schema_frame;
	int		   *counts;
	int		   *starts;
	int		   *sorted;
	int			t;
	int			j;

	if (n == 0)
		return;
	vexec_frame_begin(s->writer, batch, &schema, &schema_frame);
	if (targets == NULL)
	{
		send_rows(s, target, rows, n, schema, schema_frame);
		return;
	}

	/* a counting sort by segment, each segment's rows kept in order */
	counts = palloc0(sizeof(int) * (s->nsegs + 1));
	starts = palloc(sizeof(int) * (s->nsegs + 1));
	sorted = palloc(sizeof(int) * n);
	for (j = 0; j < n; j++)
		counts[targets[j]]++;
	starts[0] = 0;
	for (t = 0; t < s->nsegs; t++)
		starts[t + 1] = starts[t] + counts[t];
	for (j = 0; j < n; j++)
		sorted[starts[targets[j]]++] = rows[j];
	for (t = 0; t < s->nsegs; t++)
	{
		int			first = (t == 0 ? 0 : starts[t - 1]);

		send_rows(s, t, sorted + first, counts[t], schema, schema_frame);
	}
}

/* A row's segment by gp_core's own cdbhash, its keys PostgreSQL's evaluator's. */
static int
row_segment(VexecMotionSendState *s, TupleTableSlot *slot)
{
	const GpCoreApi *api = vexec_gp_core();
	ExprContext *econtext = s->node.css.ss.ps.ps_ExprContext;
	int			k;

	econtext->ecxt_outertuple = slot;
	for (k = 0; k < s->nkeys; k++)
		s->keyvalues[k] = ExecEvalExprSwitchContext(s->node.targets[k].state, econtext,
													&s->keynulls[k]);
	s->mstats.rows_by_postgres++;
	return api->hash_segment(s->row_hash, s->keyvalues, s->keynulls);
}

/*
 * The next input batch's frames, queued: false at the end of the input.
 * Its rows past the child's selection, less the child's undecided ones, go
 * where their keys hash to -- a batch at a time by the vector cdbhash where
 * the keys' kernels computed them, else a row at a time -- or in turn, or
 * everywhere; the undecided ones, resolved in row order, go apart.
 */
static bool
send_next_batch(VexecMotionSendState *s)
{
	VexecNode  *node = &s->node;
	VexecBatch *in;
	int			n;
	int		   *rows;
	int		   *targets = NULL;
	int			nrows = 0;
	int			nresolved = 0;
	int			row;
	MemoryContext old;

	MemoryContextReset(s->batchcxt);
	ResetExprContext(node->css.ss.ps.ps_ExprContext);
	s->nqueued = 0;
	s->next = 0;
	if (!vexec_node_next_input(node))
		return false;
	old = MemoryContextSwitchTo(s->batchcxt);
	in = node->in;
	n = in->nrows;
	rows = palloc(sizeof(int) * Max(n, 1));
	if (s->plan.motion == VEXEC_GP_MOTION_HASH)
		targets = palloc(sizeof(int) * Max(n, 1));
	vexec_batch_reset(s->resolved);
	vexec_batch_begin_rows(s->resolved);

	for (row = 0; row < n; row++)
	{
		/*
		 * A row the child left undecided is outside its selection, which has
		 * the rows its quals passed alone (vexec_next_batch()): it is
		 * resolved here, and its quals may still pass it, or raise.
		 */
		bool		undecided = node->child_redo != NULL && vexec_bit(node->child_redo, row);

		if (!undecided && in->selection != NULL && !vexec_bit(in->selection, row))
			continue;
		CHECK_FOR_INTERRUPTS();
		if (undecided)
		{
			/* the child's row, resolved by PostgreSQL's evaluator */
			TupleTableSlot *slot = vexec_resolve_row(node->vec_child, row);

			if (slot == NULL)
				continue;
			slot_getallattrs(slot);
			if (targets != NULL)
				s->resolved_targets[nresolved] = row_segment(s, slot);
			vexec_batch_add_values(s->resolved, slot->tts_values, slot->tts_isnull);
			nresolved++;
			continue;
		}
		rows[nrows++] = row;
	}

	if (targets != NULL)
	{
		int		   *kernel_rows = palloc(sizeof(int) * Max(nrows, 1));
		int		   *kernel_at = palloc(sizeof(int) * Max(nrows, 1));
		int		   *segs = palloc(sizeof(int) * Max(nrows, 1));
		int			nkernel = 0;
		int			k;
		int			j;
		bool		all_eager = !s->plan.legacy;

		for (k = 0; k < s->nkeys && all_eager; k++)
			all_eager = node->outputs[k] != NULL;

		/*
		 * A row's keys from their columns where every key's kernel computed
		 * them; else, a legacy key's or a row PostgreSQL's evaluator is to
		 * compute, by gp_core's cdbhash, row by row, in the rows' order.
		 */
		for (j = 0; j < nrows; j++)
		{
			row = rows[j];
			if (all_eager && (node->redo == NULL || !vexec_bit(node->redo, row)))
			{
				kernel_rows[nkernel] = row;
				kernel_at[nkernel++] = j;
				continue;
			}
			vexec_node_load_input(node, row);
			targets[j] = row_segment(s, node->input_slot);
		}
		if (nkernel > 0)
		{
			vexec_cdbhash(node->work, s->nkeys, s->keys, node->outputs, kernel_rows,
						  nkernel, s->nsegs, segs);
			for (j = 0; j < nkernel; j++)
				targets[kernel_at[j]] = segs[j];
		}
		send_batch(s, in, rows, targets, nrows, -1);
		if (nresolved > 0)
		{
			int		   *rrows = palloc(sizeof(int) * nresolved);

			for (j = 0; j < nresolved; j++)
				rrows[j] = j;
			send_batch(s, s->resolved, rrows, s->resolved_targets, nresolved, -1);
		}
	}
	else
	{
		/* a random redistribution's batch to the next segment; else to all */
		int			target = -1;

		if (s->plan.motion == VEXEC_GP_MOTION_RANDOM)
		{
			target = s->next_segment;
			s->next_segment = (s->next_segment + 1) % s->nsegs;
		}
		send_batch(s, in, rows, NULL, nrows, target);
		if (nresolved > 0)
		{
			int		   *rrows = palloc(sizeof(int) * nresolved);
			int			j;

			for (j = 0; j < nresolved; j++)
				rrows[j] = j;
			send_batch(s, s->resolved, rrows, NULL, nresolved, target);
		}
	}
	MemoryContextSwitchTo(old);
	return true;
}

/*
 * The Motion's next row: a frame, after the child's columns, all NULL, and
 * the segment it goes to.
 */
static TupleTableSlot *
send_exec(CustomScanState *css)
{
	VexecMotionSendState *s = (VexecMotionSendState *) css;
	TupleTableSlot *slot = css->ss.ss_ScanTupleSlot;
	int			natts = slot->tts_tupleDescriptor->natts;
	int			i;

	for (;;)
	{
		if (s->next < s->nqueued)
		{
			QueuedFrame *q = &s->queue[s->next++];

			ExecClearTuple(slot);
			for (i = 0; i < natts - 2; i++)
			{
				slot->tts_values[i] = (Datum) 0;
				slot->tts_isnull[i] = true;
			}
			slot->tts_values[natts - 2] = Int32GetDatum(Max(q->target, 0));
			slot->tts_isnull[natts - 2] = false;
			slot->tts_values[natts - 1] = PointerGetDatum(q->frame);
			slot->tts_isnull[natts - 1] = false;
			return ExecStoreVirtualTuple(slot);
		}
		if (s->done || !send_next_batch(s))
		{
			s->done = true;
			return ExecClearTuple(slot);
		}
	}
}

static void
send_rescan(CustomScanState *css)
{
	VexecMotionSendState *s = (VexecMotionSendState *) css;

	if (s->child->chgParam == NULL)
		ExecReScan(s->child);
	MemoryContextReset(s->batchcxt);
	s->nqueued = 0;
	s->next = 0;
	s->done = false;
	s->child_done = false;
	vexec_node_rescan(&s->node);
}

static void
send_end(CustomScanState *css)
{
	VexecMotionSendState *s = (VexecMotionSendState *) css;

	vexec_node_end(&s->node);
	ExecEndNode(s->child);
}

/* What the Motion's frames are, and what went: the node's, or the segments'. */
static void
explain_motion_stats(VexecNode *node, const VexecMotionStats *m, ExplainState *es)
{
	if (!es->analyze || (!node->ran && node->segments_seen == 0))
		return;
	ExplainPropertyInteger("Frames", NULL, m->frames, es);
	if (es->verbose)
	{
		ExplainPropertyInteger("Schema Frames", NULL, m->schema_frames, es);
		ExplainPropertyInteger("Frame Bytes", "bytes", m->bytes, es);
		if (m->rows_by_postgres > 0)
			ExplainPropertyInteger("Keys Hashed Row by Row", NULL, m->rows_by_postgres, es);
	}
}

static void
send_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecMotionSendState *s = (VexecMotionSendState *) css;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	const char *to;

	vexec_node_relabel(&s->node, es);
	switch (s->plan.motion)
	{
		case VEXEC_GP_MOTION_HASH:
			to = "the segments their keys hash to";
			break;
		case VEXEC_GP_MOTION_RANDOM:
			to = "each segment in turn";
			break;
		case VEXEC_GP_MOTION_BROADCAST:
			to = "every segment";
			break;
		default:
			to = "the one gathering";
			break;
	}
	ExplainPropertyText("Frames To", to, es);
	if (cscan->custom_exprs != NIL)
	{
		List	   *context = set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan,
													   ancestors);
		List	   *keys = NIL;
		ListCell   *lc;

		foreach(lc, cscan->custom_exprs)
			keys = lappend(keys, deparse_expression(lfirst(lc), context, es->verbose, false));
		ExplainPropertyList("Hash Key", keys, es);
		if (es->verbose && s->plan.legacy)
			ExplainPropertyText("Hash", "legacy, row by row", es);
	}
	if (es->verbose)
	{
		ExplainPropertyText("Batch Format",
							s->node.layout.format == VEXEC_FORMAT_ARROW ? "arrow" : "postgres", es);
		ExplainPropertyText("Input", s->child_batches ? "batches" : "rows", es);
	}
	explain_motion_stats(&s->node, &s->mstats, es);
}

/* ---------------------------------------------------------------------
 * VecMotionRecv
 * ---------------------------------------------------------------------
 */

static bool recv_fetch(VexecNode *node);
static void recv_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *recv_exec(CustomScanState *css);
static void recv_end(CustomScanState *css);
static void recv_rescan(CustomScanState *css);
static void recv_explain(CustomScanState *css, List *ancestors, ExplainState *es);

static const CustomExecMethods recv_exec_methods = {
	.CustomName = VEXEC_MOTION_RECV_NAME,
	.BeginCustomScan = recv_begin,
	.ExecCustomScan = recv_exec,
	.EndCustomScan = recv_end,
	.ReScanCustomScan = recv_rescan,
	.ExplainCustomScan = recv_explain,
};

Node *
vexec_create_motion_recv_state(CustomScan *cscan)
{
	VexecMotionRecvState *s = palloc0(sizeof(VexecMotionRecvState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &recv_exec_methods;
	s->node.kind = VEXEC_NODE_MOTION_RECV;
	return (Node *) s;
}

static void
recv_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecMotionRecvState *s = (VexecMotionRecvState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	TupleDesc	scandesc = css->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	TupleDesc	motiondesc;
	VexecType **types;
	int			i;

	vexec_node_begin(node, estate);
	node->fetch = recv_fetch;
	node->label = "Vec Motion Receive";
	if (list_length(cscan->custom_private) != 1)
		elog(ERROR, "vexec: a VecMotionRecv's plan of %d parts",
			 list_length(cscan->custom_private));
	s->ncols = intVal(linitial(cscan->custom_private));

	s->child = ExecInitNode(outerPlan(cscan), estate, eflags);
	outerPlanState(css) = s->child;
	motiondesc = ExecGetResultType(s->child);
	if (scandesc->natts != s->ncols || motiondesc->natts != s->ncols + 2 ||
		TupleDescAttr(motiondesc, s->ncols + 1)->atttypid != BYTEAOID)
		elog(ERROR, "vexec: a VecMotionRecv of %d columns over a Motion of %d",
			 s->ncols, motiondesc->natts);
	s->framecol = s->ncols + 1;

	/* the scan tuple: the fragment's row, which a frame's batch holds */
	types = palloc(sizeof(VexecType *) * Max(s->ncols, 1));
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(s->ncols, 1));
	for (i = 0; i < s->ncols; i++)
	{
		Form_pg_attribute att = TupleDescAttr(scandesc, i);

		types[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		node->input_attnos[i] = i + 1;
	}
	node->input_varno = INDEX_VAR;
	node->ninput = s->ncols;
	node->input_slot = css->ss.ss_ScanTupleSlot;
	s->frames_in = vexec_batch_create(node->mcxt, s->ncols, types);
	s->reader = vexec_frame_reader_create(node->mcxt, s->ncols, types);
	vexec_node_compile(node, cscan->scan.plan.qual, cscan->scan.plan.targetlist);
}

/*
 * The next batch a frame holds: the Motion's rows read until one is a
 * batch's frame, the schemas before it kept; false at the Motion's end.
 */
static bool
recv_fetch(VexecNode *node)
{
	VexecMotionRecvState *s = (VexecMotionRecvState *) node;

	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(s->child);
		bytea	   *frame;

		if (TupIsNull(slot))
			return false;
		slot_getsomeattrs(slot, s->framecol + 1);
		if (slot->tts_isnull[s->framecol])
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("a vector Motion's row without its frame")));
		frame = (bytea *) DatumGetPointer(slot->tts_values[s->framecol]);
		if (VARATT_IS_EXTERNAL(frame) || VARATT_IS_COMPRESSED(frame))
			frame = (bytea *) PG_DETOAST_DATUM(PointerGetDatum(frame));
		s->mstats.bytes += VARSIZE_ANY_EXHDR(frame);
		if (!vexec_frame_decode(s->reader, VARDATA_ANY(frame), VARSIZE_ANY_EXHDR(frame),
								s->frames_in))
		{
			s->mstats.schema_frames++;
			continue;
		}
		s->mstats.frames++;
		s->mstats.rows += s->frames_in->nrows;
		if (s->frames_in->nrows == 0)
			continue;
		vexec_batch_apply_config(s->frames_in, &node->layout);
		node->in = s->frames_in;
		return true;
	}
}

static TupleTableSlot *
recv_exec(CustomScanState *css)
{
	return vexec_node_exec(&((VexecMotionRecvState *) css)->node);
}

static void
recv_rescan(CustomScanState *css)
{
	VexecMotionRecvState *s = (VexecMotionRecvState *) css;

	if (s->child->chgParam == NULL)
		ExecReScan(s->child);
	vexec_node_rescan(&s->node);
}

static void
recv_end(CustomScanState *css)
{
	VexecMotionRecvState *s = (VexecMotionRecvState *) css;

	vexec_node_end(&s->node);
	ExecEndNode(s->child);
}

static void
recv_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecMotionRecvState *s = (VexecMotionRecvState *) css;

	vexec_node_explain(&s->node, ancestors, es);
	explain_motion_stats(&s->node, &s->mstats, es);
}
