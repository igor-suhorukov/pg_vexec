/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecscan.c
 *	  VecScan: a table's sequential scan, its quals and its projection, a
 *	  batch at a time (pg_vector_executor.md §3.8, §3.5).
 *
 * Its batches come from a source (§3.5):
 *
 *	a registered one	the batch reader the table's access method
 *						registered (vexec_source.h): PAX's, gp_ao's.  It
 *						hands each column in its storage's layout, and the
 *						rows it has deleted out of the batch's selection;
 *	the slot path		any other access method, heap among them in V1
 *						(§3.5.2): table_scan_getnextslot(), the needed
 *						attributes deformed, and their values copied into
 *						the batch.
 *
 * Either way vexec begins the scan itself, through table_beginscan() with
 * the flags a SeqScan gives it (PG19:src/backend/executor/nodeSeqscan.c),
 * so the access method keeps the snapshot's registration, predicate locks,
 * rescan and end; and it begins it at the first row asked for, as a SeqScan
 * does, so that an EXPLAIN without ANALYZE begins none.  The columns are
 * read in the format in effect when the node began (§3.4.4).
 *
 * A VecAgg above a scan with no qual may have the source answer its
 * aggregates from the statistics it keeps of a unit of rows -- a file, a
 * group -- in place of the unit's batches (vexec_scan_aggregate(), H2): the
 * unit's rows are counted as the scan's, as if they had been handed up.
 *
 * The scan's tuple is the table's row: the plan's quals and target list
 * read Vars of scanrelid, as a SeqScan's do, so custom_scan_tlist is empty.
 * The node reads the attributes they name, the whole row for a whole-row
 * Var; ctid comes from each row's TID, tableoid from the relation.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/pg_type.h"
#include "commands/explain_format.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "expr/expr.h"
#include "source/source.h"

typedef struct VexecScanState
{
	VexecNode	node;			/* first */
	Relation	rel;
	const VexecSourceRoutine *src;	/* NULL: the slot path */
	TableScanDesc scan;
	void	   *srcstate;
	TupleTableSlot *amslot;		/* the slot path's */
	int			nattrs;			/* input columns that are attributes */
	AttrNumber *attrs;			/* their attnos */
	bool		need_tid;		/* the last input column is ctid */
	int			maxattr;
	bool		done;

	/* H2: the units, and their rows, a source answered from statistics */
	int64		stats_units;
	int64		stats_rows;
} VexecScanState;

static bool scan_fetch(VexecNode *node);
static void scan_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *scan_exec(CustomScanState *css);
static void scan_end(CustomScanState *css);
static void scan_rescan(CustomScanState *css);
static void scan_explain(CustomScanState *css, List *ancestors, ExplainState *es);

static const CustomExecMethods scan_exec_methods = {
	.CustomName = VEXEC_SCAN_NAME,
	.BeginCustomScan = scan_begin,
	.ExecCustomScan = scan_exec,
	.EndCustomScan = scan_end,
	.ReScanCustomScan = scan_rescan,
	.ExplainCustomScan = scan_explain,
};

Node *
vexec_create_scan_state(CustomScan *cscan)
{
	VexecScanState *s = palloc0(sizeof(VexecScanState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &scan_exec_methods;
	s->node.kind = VEXEC_NODE_SCAN;
	return (Node *) s;
}

/*
 * The attributes the plan's quals and target list read: every attribute
 * for a whole-row Var; and whether ctid is read.
 */
static Bitmapset *
needed_attrs(CustomScan *cscan, bool *need_tid)
{
	Index		relid = cscan->scan.scanrelid;
	Bitmapset  *attrs = NULL;

	pull_varattnos((Node *) cscan->scan.plan.targetlist, relid, &attrs);
	pull_varattnos((Node *) cscan->scan.plan.qual, relid, &attrs);
	*need_tid = bms_is_member(SelfItemPointerAttributeNumber - FirstLowInvalidHeapAttributeNumber,
							  attrs);
	return attrs;
}

static void
scan_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecScanState *s = (VexecScanState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	TupleDesc	desc;
	Bitmapset  *attrs;
	VexecType **types;
	bool		whole_row;
	int			col;
	int			i;
	const char *how;

	vexec_node_begin(node, estate);
	node->fetch = scan_fetch;
	s->rel = css->ss.ss_currentRelation;
	desc = RelationGetDescr(s->rel);

	attrs = needed_attrs(cscan, &s->need_tid);
	whole_row = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs);

	/* the input columns: the attributes read, in order, then ctid */
	s->attrs = palloc(sizeof(AttrNumber) * (desc->natts + 1));
	for (i = 1; i <= desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i - 1);

		if (att->attisdropped)
			continue;
		if (whole_row || bms_is_member(i - FirstLowInvalidHeapAttributeNumber, attrs))
			s->attrs[s->nattrs++] = i;
	}
	s->maxattr = s->nattrs > 0 ? s->attrs[s->nattrs - 1] : 0;

	node->ninput = s->nattrs + (s->need_tid ? 1 : 0);
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(node->ninput, 1));
	types = palloc(sizeof(VexecType *) * Max(node->ninput, 1));
	for (col = 0; col < s->nattrs; col++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, s->attrs[col] - 1);

		node->input_attnos[col] = s->attrs[col];
		types[col] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
	}
	if (s->need_tid)
	{
		node->input_attnos[col] = SelfItemPointerAttributeNumber;
		types[col] = vexec_type_make(TIDOID, -1, InvalidOid);
	}
	node->in = vexec_batch_create(node->mcxt, node->ninput, types);
	node->input_varno = cscan->scan.scanrelid;
	node->input_slot = css->ss.ss_ScanTupleSlot;
	node->input_slot->tts_tableOid = RelationGetRelid(s->rel);

	/* a registered source serves only the columns it supports */
	s->src = vexec_source_for(s->rel, &how);
	if (s->src != NULL && s->src->supports != NULL)
	{
		for (i = 0; i < s->nattrs; i++)
			if (!s->src->supports(s->rel, s->attrs[i]))
			{
				s->src = NULL;
				break;
			}
	}

	vexec_node_compile(node, cscan->scan.plan.qual, cscan->scan.plan.targetlist);
}

/* The quals a source may prune by: no parameter, no SubPlan, nothing volatile. */
static bool
prunable_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param) || IsA(node, SubPlan) || IsA(node, AlternativeSubPlan))
		return true;
	return expression_tree_walker(node, prunable_walker, context);
}

static List *
pruning_quals(CustomScan *cscan)
{
	List	   *quals = NIL;
	ListCell   *lc;

	foreach(lc, cscan->scan.plan.qual)
	{
		Node	   *q = lfirst(lc);

		if (!prunable_walker(q, NULL) && !contain_volatile_functions(q))
			quals = lappend(quals, q);
	}
	return quals;
}

/* Begin the table's scan, and the source's, at the first row asked for. */
static void
begin_scan(VexecScanState *s)
{
	EState	   *estate = s->node.css.ss.ps.state;
	uint32		flags = SO_NONE;

	if (ScanRelIsReadOnly(&s->node.css.ss))
		flags |= SO_HINT_REL_READ_ONLY;
	if (estate->es_instrument & INSTRUMENT_IO)
		flags |= SO_SCAN_INSTRUMENT;
	s->scan = table_beginscan(s->rel, estate->es_snapshot, 0, NULL, flags);
	s->node.css.ss.ss_currentScanDesc = s->scan;

	if (s->src != NULL)
	{
		VexecSourceSpec *spec = palloc0(sizeof(VexecSourceSpec));
		uint8	   *layouts = palloc(Max(s->nattrs, 1));
		int			i;

		for (i = 0; i < s->nattrs; i++)
		{
			VexecShape	shape;

			vexec_type_shape(s->node.in->types[i], &s->node.layout, &shape);
			layouts[i] = shape.layout;
		}
		spec->size = sizeof(VexecSourceSpec);
		spec->ncolumns = s->nattrs;
		spec->attnums = s->attrs;
		spec->layouts = layouts;
		spec->quals = pruning_quals((CustomScan *) s->node.css.ss.ps.plan);
		spec->max_rows = VEXEC_BATCH_ROWS;
		spec->flags = s->need_tid ? VEXEC_SRC_TIDS : 0;
		s->srcstate = s->src->begin(s->scan, spec);
	}
	else
		s->amslot = table_slot_create(s->rel, NULL);
}

/* The slot path: up to a batch of rows, their values copied in. */
static bool
fetch_slots(VexecScanState *s)
{
	VexecBatch *in = s->node.in;
	Datum	   *values = palloc(sizeof(Datum) * Max(s->node.ninput, 1));
	bool	   *isnull = palloc(sizeof(bool) * Max(s->node.ninput, 1));
	int			i;

	vexec_batch_reset(in);
	vexec_batch_begin_rows(in);
	while (in->nrows < VEXEC_BATCH_ROWS)
	{
		if (!table_scan_getnextslot(s->scan, ForwardScanDirection, s->amslot))
		{
			s->done = true;
			break;
		}
		slot_getsomeattrs(s->amslot, s->maxattr);
		for (i = 0; i < s->nattrs; i++)
		{
			values[i] = s->amslot->tts_values[s->attrs[i] - 1];
			isnull[i] = s->amslot->tts_isnull[s->attrs[i] - 1];
		}
		if (s->need_tid)
		{
			values[i] = PointerGetDatum(&s->amslot->tts_tid);
			isnull[i] = false;
		}
		vexec_batch_add_values(in, values, isnull);
	}
	pfree(values);
	pfree(isnull);
	if (in->nrows > 0)
		vexec_batch_apply_config(in, &s->node.layout);
	return in->nrows > 0;
}

/* A registered source's next batch, its columns as the source holds them. */
static bool
fetch_source(VexecScanState *s)
{
	VexecBatch *in = s->node.in;
	VexecSourceBatch sb;
	int			i;

	memset(&sb, 0, sizeof(sb));
	vexec_batch_reset(in);
	if (!s->src->next(s->srcstate, &sb))
	{
		s->done = true;
		return false;
	}
	in->nrows = sb.nrows;
	in->selection = (uint64 *) sb.visible;
	for (i = 0; i < s->nattrs; i++)
	{
		const VexecColumn *c = &sb.columns[i];
		VexecVec   *v = &in->cols[i];

		v->shape.layout = c->layout;
		v->shape.arrow_values = c->arrow_values;
		v->shape.scale = c->scale;
		v->shape.width = c->width;
		v->shape.stride = c->stride;
		vexec_shape_normalize(&v->shape);
		v->encoding = c->encoding;
		v->nvalues = c->encoding == VEXEC_CONST ? 1 : sb.nrows;
		v->validity = (uint64 *) c->validity;
		v->values = (void *) c->values;
		v->buffers = (char **) c->buffers;
		v->buffer_sizes = (int64 *) c->buffer_sizes;
		v->nbuffers = c->nbuffers;
		v->datums = (Datum *) c->datums;
		if (c->encoding == VEXEC_DICT)
		{
			VexecVec   *d = vexec_batch_alloc0(in, sizeof(VexecVec));
			const VexecColumn *dc = c->dictionary;

			v->codes = (int32 *) c->codes;
			d->type = v->type;
			d->shape.layout = dc->layout;
			d->shape.arrow_values = dc->arrow_values;
			d->shape.scale = dc->scale;
			d->shape.width = dc->width;
			d->shape.stride = dc->stride;
			vexec_shape_normalize(&d->shape);
			d->encoding = VEXEC_FLAT;
			d->nvalues = c->ndictionary;
			d->validity = (uint64 *) dc->validity;
			d->values = (void *) dc->values;
			d->buffers = (char **) dc->buffers;
			d->buffer_sizes = (int64 *) dc->buffer_sizes;
			d->nbuffers = dc->nbuffers;
			d->datums = (Datum *) dc->datums;
			v->dictionary = d;
		}
	}
	if (s->need_tid)
	{
		VexecVec   *v = &in->cols[i];

		if (sb.tids == NULL)
			elog(ERROR, "vexec: the batch source \"%s\" gave no TIDs", s->src->name);

		memset(&v->shape, 0, sizeof(VexecShape));
		v->shape.layout = VEXEC_FIXED;
		v->shape.width = sizeof(ItemPointerData);
		v->shape.stride = sizeof(ItemPointerData);
		v->encoding = VEXEC_FLAT;
		v->nvalues = sb.nrows;
		v->validity = NULL;
		v->values = (void *) sb.tids;
	}
	vexec_batch_apply_config(in, &s->node.layout);
	return true;
}

static bool
scan_fetch(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;

	if (s->scan == NULL)
		begin_scan(s);
	if (s->done)
		return false;
	return s->src != NULL ? fetch_source(s) : fetch_slots(s);
}

/*
 * Whether the scan's source may answer aggregates from its statistics: it
 * has the contract's aggregate(), and the scan no qual and no TIDs to read.
 */
bool
vexec_scan_can_aggregate(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;
	Plan	   *plan = node->css.ss.ps.plan;

	Assert(node->kind == VEXEC_NODE_SCAN);
	return s->src != NULL && VEXEC_SOURCE_HAS(s->src, aggregate) &&
		plan->qual == NIL && !s->need_tid;
}

/*
 * The source's next unit answered from its statistics, between batches:
 * true, and its rows counted as handed up; false where the source leaves
 * the unit to its batches, or the scan is done.
 */
bool
vexec_scan_aggregate(VexecNode *node, int nreqs, const VexecSourceAgg *reqs,
					 VexecSourceAggAnswer *answers, int64 *nrows)
{
	VexecScanState *s = (VexecScanState *) node;
	PlanState  *ps = &node->css.ss.ps;
	bool		ok;

	Assert(vexec_scan_can_aggregate(node));
	if (ps->chgParam != NULL)
		ExecReScan(ps);
	if (s->scan == NULL)
		begin_scan(s);
	if (s->done || node->finished)
		return false;
	CHECK_FOR_INTERRUPTS();
	if (ps->instrument)
		InstrStartNode(ps->instrument);
	*nrows = 0;
	ok = s->src->aggregate(s->srcstate, nreqs, reqs, answers, nrows);
	if (ps->instrument)
		InstrStopNode(ps->instrument, ok ? (double) *nrows : 0);
	if (ok)
	{
		s->stats_units++;
		s->stats_rows += *nrows;
	}
	return ok;
}

/* The scan's source's name, for EXPLAIN. */
const char *
vexec_scan_source_name(VexecNode *node)
{
	VexecScanState *s = (VexecScanState *) node;

	return s->src != NULL && s->src->name != NULL ? s->src->name : "a registered source";
}

static TupleTableSlot *
scan_exec(CustomScanState *css)
{
	return vexec_node_exec(&((VexecScanState *) css)->node);
}

static void
scan_rescan(CustomScanState *css)
{
	VexecScanState *s = (VexecScanState *) css;

	if (s->scan != NULL)
	{
		if (s->src != NULL)
		{
			table_rescan(s->scan, NULL);
			s->src->rescan(s->srcstate);
		}
		else
			table_rescan(s->scan, NULL);
	}
	s->done = false;
	vexec_node_rescan(&s->node);
}

static void
scan_end(CustomScanState *css)
{
	VexecScanState *s = (VexecScanState *) css;

	if (s->srcstate != NULL)
		s->src->end(s->srcstate);
	s->srcstate = NULL;
	if (s->amslot != NULL)
		ExecDropSingleTupleTableSlot(s->amslot);
	s->amslot = NULL;
	if (s->scan != NULL)
		table_endscan(s->scan);
	s->scan = NULL;
	vexec_node_end(&s->node);
}

static void
scan_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecScanState *s = (VexecScanState *) css;

	vexec_node_explain(&s->node, ancestors, es);
	if (es->verbose)
	{
		const char *how;

		(void) vexec_source_for(s->rel, &how);
		ExplainPropertyText("Source", s->src != NULL ? how : "the slot path", es);
	}
	if (es->analyze && s->stats_units > 0)
	{
		ExplainPropertyInteger("Units From Statistics", NULL, s->stats_units, es);
		ExplainPropertyInteger("Rows From Statistics", NULL, s->stats_rows, es);
	}
}
