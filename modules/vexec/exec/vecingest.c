/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecingest.c
 *	  VecIngest: a client's stream of Arrow record batches, read as a vector
 *	  source (pg_vector_executor.md §3.16).
 *
 * The function scan of vexec.ingest_stream(handle), as plan/ingest.c makes
 * it: no relation (scanrelid 0), its scan tuple the column definition
 * list's columns (custom_scan_tlist), the handle's expression in
 * custom_exprs.  The node opens the stream when it first reads, takes each
 * window of its rows as its input batch (egress/ingest.c), and evaluates
 * its quals and target list over it as any vector node does: a VecInsert
 * above it reads its batches, a row parent its rows.
 *
 * A stream is read once: the node cannot be rescanned once it has begun.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/executor.h"
#include "nodes/extensible.h"

#include "vexec.h"
#include "batch/batch.h"
#include "egress/egress.h"
#include "exec/exec.h"
#include "cb_explain.h"

typedef struct VexecIngestState
{
	VexecNode	node;			/* first */
	ExprState  *handle_expr;
	int64		handle;
	int			ncols;
	Oid		   *types;
	int32	   *typmods;
	VexecIngestCursor *cursor;
	bool		zero_copy;		/* every column read without a copy */
	bool		done;
} VexecIngestState;

static bool ingest_fetch(VexecNode *node);
static void ingest_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *ingest_exec(CustomScanState *css);
static void ingest_end(CustomScanState *css);
static void ingest_rescan(CustomScanState *css);
static void ingest_explain(CustomScanState *css, List *ancestors, ExplainState *es);

static const CustomExecMethods ingest_exec_methods = {
	.CustomName = VEXEC_INGEST_NAME,
	.BeginCustomScan = ingest_begin,
	.ExecCustomScan = ingest_exec,
	.EndCustomScan = ingest_end,
	.ReScanCustomScan = ingest_rescan,
	.ExplainCustomScan = ingest_explain,
};

Node *
vexec_create_ingest_state(CustomScan *cscan)
{
	VexecIngestState *s = palloc0(sizeof(VexecIngestState));

	(void) cscan;
	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &ingest_exec_methods;
	s->node.kind = VEXEC_NODE_INGEST;
	return (Node *) s;
}

static void
ingest_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecIngestState *s = (VexecIngestState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	TupleDesc	scandesc = css->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	VexecType **types;
	int			i;

	(void) eflags;
	vexec_node_begin(node, estate);
	node->fetch = ingest_fetch;
	node->label = "Vec Ingest";

	s->ncols = scandesc->natts;
	s->types = palloc(sizeof(Oid) * Max(s->ncols, 1));
	s->typmods = palloc(sizeof(int32) * Max(s->ncols, 1));
	types = palloc(sizeof(VexecType *) * Max(s->ncols, 1));
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(s->ncols, 1));
	for (i = 0; i < s->ncols; i++)
	{
		Form_pg_attribute att = TupleDescAttr(scandesc, i);

		s->types[i] = att->atttypid;
		s->typmods[i] = att->atttypmod;
		types[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		node->input_attnos[i] = i + 1;
	}
	node->input_varno = INDEX_VAR;
	node->ninput = s->ncols;
	node->input_slot = css->ss.ss_ScanTupleSlot;
	node->in = vexec_batch_create(node->mcxt, s->ncols, types);
	vexec_node_compile(node, cscan->scan.plan.qual, cscan->scan.plan.targetlist);
	s->handle_expr = ExecInitExpr(linitial(cscan->custom_exprs), &css->ss.ps);
}

/* The stream's next window into the input batch; false at its end. */
static bool
ingest_fetch(VexecNode *node)
{
	VexecIngestState *s = (VexecIngestState *) node;

	if (s->done)
		return false;
	node->ran = true;
	if (s->cursor == NULL)
	{
		bool		isnull;
		Datum		d;
		MemoryContext old;

		d = ExecEvalExprSwitchContext(s->handle_expr, node->css.ss.ps.ps_ExprContext, &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("vexec.ingest_stream() needs a stream's handle, not NULL")));
		s->handle = DatumGetInt64(d);
		old = MemoryContextSwitchTo(node->mcxt);
		s->cursor = vexec_ingest_open(s->handle, s->ncols, s->types, s->typmods);
		MemoryContextSwitchTo(old);
		s->zero_copy = vexec_ingest_zero_copy(s->cursor);
	}
	if (!vexec_ingest_next_window(s->cursor, node->in, &node->layout))
	{
		s->done = true;
		return false;
	}
	node->stats.batches++;
	node->stats.rows_in += node->in->nrows;
	return true;
}

static TupleTableSlot *
ingest_exec(CustomScanState *css)
{
	return vexec_node_exec(&((VexecIngestState *) css)->node);
}

static void
ingest_rescan(CustomScanState *css)
{
	VexecIngestState *s = (VexecIngestState *) css;

	if (s->cursor != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a client's stream is read once, and cannot be rescanned")));
	vexec_node_rescan(&s->node);
}

static void
ingest_end(CustomScanState *css)
{
	VexecIngestState *s = (VexecIngestState *) css;

	vexec_ingest_close(s->cursor);
	s->cursor = NULL;
	vexec_node_end(&s->node);
}

static void
ingest_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecIngestState *s = (VexecIngestState *) css;

	CbExplainRelabel(css, es, "Vec Ingest", " on ingest_stream");
	vexec_node_explain_properties(&s->node, ancestors, es);
	if (es->analyze && s->node.ran)
		ExplainPropertyText("Columns Read", s->zero_copy ? "without a copy" :
							"value by value", es);
}
