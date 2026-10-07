/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecinsert.c
 *	  VecInsert: an INSERT's rows written a batch at a time
 *	  (pg_vector_executor.md §3.16).
 *
 * The node takes ModifyTable's place at the top of an INSERT's plan
 * (plan/insert.c).  It opens the target as ModifyTable would, and reads its
 * child: a vector child's batches, whose redo rows it resolves in row order,
 * or a row child's rows, which it gathers into batches.  Each batch it
 * writes so:
 *
 *	checks	NOT NULL from the columns' validity, the first failing row
 *			through ExecConstraints() for PostgreSQL's own error; where the
 *			target has CHECK constraints, stored generated columns or a
 *			partition's constraint, every row through PostgreSQL's own
 *			functions -- ExecComputeStoredGenerated(), ExecConstraints(),
 *			ExecPartitionCheck() -- on a virtual slot of its values, in row
 *			order, as ModifyTable checks them;
 *	write	through the target's sink (vexec_sink.h), each column converted
 *			to the layout the sink takes; or, where there is none, the sink
 *			takes no batch of this relation, or a column will not convert, as
 *			COPY FROM writes: table_multi_insert() of up to 1,000 rows
 *			(PG19:src/backend/commands/copyfrom.c:65, 556);
 *	indexes	ExecInsertIndexTuples() for each row, on a virtual slot of its
 *			values with the TID the sink or the access method gave it, as
 *			COPY does after table_multi_insert().
 *
 * A partitioned target's rows are routed as COPY FROM routes them, with a
 * ModifyTableState of the node's own (copyfrom.c:932, 983, 1217):
 * ExecFindPartition() for each row in row order, then the partition's
 * checks on that row, then the row into the partition's pending batch.  As
 * COPY writes every partition's buffer once they hold 1,000 rows between
 * them (copyfrom.c:73, 341), the pending batches are written, each as a
 * target's batch is, in the order their partitions were first routed to,
 * once they hold a batch's rows between them, and at the end.  A batch
 * whose rows all go to one partition, of the root's row and with no
 * generated column, is written as it is, after the pending rows.
 *
 * On a cluster, ORCA's plans keep the ModifyTable gp_core dispatches as a
 * write, with VecInsert as its input (plan/insert.c): the node then takes
 * the ModifyTable's own ResultRelInfo, made before its input's, writes
 * every row and hands none up, and the ModifyTable inserts none.
 *
 * Domains are checked where the child computes the row: the planner's
 * target list for an INSERT coerces each value to its column.  The rows are
 * counted into es_processed, for the command tag.  The node returns no row:
 * it stands only where there is no RETURNING.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/heapam.h"
#include "access/tableam.h"
#include "access/tupconvert.h"
#include "catalog/pg_type.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "executor/execPartition.h"
#include "executor/executor.h"
#include "executor/nodeModifyTable.h"
#include "nodes/extensible.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/rel.h"
#include "varatt.h"

#include "vexec_sink.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "cb_explain.h"

/* Rows a table_multi_insert() call takes, as COPY's buffers hold. */
#define VEXEC_INSERT_MULTI	1000

/* A relation the node writes: the target, or a partition of it. */
typedef struct VexecInsertTarget
{
	Oid			relid;			/* the hash key, for a partition */
	ResultRelInfo *rri;
	Relation	rel;
	int			natts;
	bool		has_indexes;

	/* the checks */
	bool	   *notnull;		/* per attribute */
	bool		any_notnull;
	bool		row_checks;		/* CHECK, generated columns, a partition */
	bool		has_generated;
	TupleTableSlot *row_slot;	/* a row of the relation, virtual */
	VexecBatch *generated;		/* a batch with its generated columns */

	/* a partition's */
	TupleConversionMap *map;	/* the root's row to the partition's */
	VexecBatch *pending;		/* rows routed to it, not yet written */

	/* the sink */
	bool		started;
	const VexecSinkRoutine *sink;
	void	   *sinkstate;
	VexecSinkLayout *layouts;
	VexecShape *shapes;
	VexecColumn *columns;
	ItemPointerData *tids;

	/* table_multi_insert() */
	TupleTableSlot **slots;
	int			nslots;
	BulkInsertState bistate;

	int64		batches_sink;	/* written through the sink */
	int64		batches_multi;	/* through table_multi_insert() */
} VexecInsertTarget;

typedef struct VexecInsertState
{
	VexecNode	node;			/* first: EXPLAIN's name and figures */
	Index		rti;
	bool		can_set_tag;
	bool		explain_only;
	PlanState  *child;
	bool		child_batches;
	TupleTableSlot *child_slot; /* a vector child's row, virtual */
	ResultRelInfo *rri;
	Relation	rel;
	int			natts;
	VexecType **types;			/* the root's columns' types */

	/* the target, when it is not partitioned */
	VexecInsertTarget target;

	/* a partitioned target's routing */
	bool		partitioned;
	ModifyTableState *mtstate;
	PartitionTupleRouting *proute;
	HTAB	   *parts;			/* partition's OID -> VexecInsertTarget */
	List	   *part_list;		/* the partitions' targets, as made */
	int			npending;		/* rows pending, in every partition */
	TupleTableSlot *root_slot;	/* a row of the root, virtual */
	TupleTableSlot *copy_slot;	/* another, for rows copied to a partition */

	/* rows gathered from a row child, or a vector child's resolved rows */
	VexecBatch *gather;

	bool		done;
	int64		inserted;
	int64		rows_checked;	/* through the row-by-row checks */
} VexecInsertState;

static void insert_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *insert_exec(CustomScanState *css);
static void insert_end(CustomScanState *css);
static void insert_rescan(CustomScanState *css);
static void insert_explain(CustomScanState *css, List *ancestors, ExplainState *es);

static const CustomExecMethods insert_exec_methods = {
	.CustomName = VEXEC_INSERT_NAME,
	.BeginCustomScan = insert_begin,
	.ExecCustomScan = insert_exec,
	.EndCustomScan = insert_end,
	.ReScanCustomScan = insert_rescan,
	.ExplainCustomScan = insert_explain,
};

Node *
vexec_create_insert_state(CustomScan *cscan)
{
	VexecInsertState *s = palloc0(sizeof(VexecInsertState));

	(void) cscan;
	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &insert_exec_methods;
	s->node.kind = VEXEC_NODE_INSERT;
	return (Node *) s;
}

/* The shape a batch's column is converted to for the sink's layout. */
static void
sink_shape(const VexecSinkLayout *l, VexecShape *shape)
{
	memset(shape, 0, sizeof(VexecShape));
	shape->layout = l->layout;
	shape->arrow_values = false;
	shape->scale = l->scale;
	shape->width = l->width;
	shape->stride = l->stride;
	vexec_shape_normalize(shape);
}

/* The relation's sink, if it takes every attribute of the relation. */
static void
sink_begin(VexecInsertState *s, VexecInsertTarget *t)
{
	EState	   *estate = s->node.css.ss.ps.state;
	const VexecSinkRoutine *sink = vexec_find_sink(t->rel->rd_tableam);
	TupleDesc	desc = RelationGetDescr(t->rel);
	VexecSinkSpec spec;
	int			i;

	if (sink == NULL)
		return;
	t->layouts = palloc0(sizeof(VexecSinkLayout) * Max(t->natts, 1));
	t->shapes = palloc0(sizeof(VexecShape) * Max(t->natts, 1));
	for (i = 0; i < t->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		if (att->attisdropped)
		{
			t->layouts[i].layout = VEXEC_FIXED;
			continue;
		}
		if (!sink->supports(t->rel, att->attnum, &t->layouts[i]))
			return;
		sink_shape(&t->layouts[i], &t->shapes[i]);
	}
	memset(&spec, 0, sizeof(spec));
	spec.size = sizeof(VexecSinkSpec);
	spec.natts = t->natts;
	spec.layouts = t->layouts;
	spec.cid = estate->es_output_cid;
	spec.options = 0;
	spec.flags = t->has_indexes ? VEXEC_SINK_TIDS : 0;
	t->sink = sink;
	t->sinkstate = sink->begin(t->rel, &spec);
	t->columns = palloc0(sizeof(VexecColumn) * Max(t->natts, 1));
	t->tids = palloc(sizeof(ItemPointerData) * VEXEC_BATCH_ROWS);
}

/*
 * A relation's checks and batches, from its descriptor.  A dropped
 * attribute's column is an int4 of NULLs, as the planner's target list for
 * an INSERT has it (preptlist.c, expand_insert_targetlist()).
 */
static void
target_init(VexecInsertState *s, VexecInsertTarget *t, ResultRelInfo *rri)
{
	EState	   *estate = s->node.css.ss.ps.state;
	TupleDesc	desc = RelationGetDescr(rri->ri_RelationDesc);
	TupleConstr *constr = desc->constr;
	VexecType **types;
	int			i;

	t->rri = rri;
	t->rel = rri->ri_RelationDesc;
	t->relid = RelationGetRelid(t->rel);
	t->natts = desc->natts;
	t->has_indexes = rri->ri_NumIndices > 0;
	t->row_slot = ExecInitExtraTupleSlot(estate, desc, &TTSOpsVirtual);
	t->notnull = palloc0(sizeof(bool) * Max(t->natts, 1));
	for (i = 0; i < t->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);

		t->notnull[i] = att->attnotnull && !att->attisdropped;
		t->any_notnull |= t->notnull[i];
	}
	t->has_generated = constr != NULL && constr->has_generated_stored;
	t->row_checks = t->has_generated || t->rel->rd_rel->relispartition ||
		(constr != NULL && (constr->num_check > 0 || constr->has_generated_virtual));
	if (t->has_generated)
	{
		types = palloc(sizeof(VexecType *) * Max(t->natts, 1));
		for (i = 0; i < t->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			types[i] = att->attisdropped ? vexec_type_make(INT4OID, -1, InvalidOid) :
				vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		}
		t->generated = vexec_batch_create(s->node.mcxt, t->natts, types);
		vexec_batch_begin_rows(t->generated);
	}
}

/* The relation's sink, or its table_multi_insert(), made at its first rows. */
static void
target_start(VexecInsertState *s, VexecInsertTarget *t)
{
	if (t->started)
		return;
	t->started = true;
	sink_begin(s, t);
}

/* What the relation holds, written and released. */
static void
target_end(VexecInsertTarget *t)
{
	if (t->sink != NULL && t->sinkstate != NULL)
		t->sink->end(t->sinkstate);
	t->sinkstate = NULL;
	if (t->bistate != NULL)
	{
		FreeBulkInsertState(t->bistate);
		table_finish_bulk_insert(t->rel, 0);
	}
	t->bistate = NULL;
}

static void
insert_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecInsertState *s = (VexecInsertState *) css;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	TupleDesc	desc;
	TupleDesc	childdesc;
	int			i;

	vexec_node_begin(&s->node, estate);
	s->node.label = "Vec Insert";
	s->rti = linitial_int(cscan->custom_private);
	s->can_set_tag = lsecond_int(cscan->custom_private) != 0;
	s->explain_only = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;

	/*
	 * The target, as ExecInitModifyTable() opens it; or the ModifyTable's
	 * own, which made it before its input's, where the node is that input.
	 */
	if (estate->es_result_relations != NULL &&
		estate->es_result_relations[s->rti - 1] != NULL)
		s->rri = estate->es_result_relations[s->rti - 1];
	else
	{
		s->rri = makeNode(ResultRelInfo);
		ExecInitResultRelation(estate, s->rri, s->rti);
	}
	CheckValidResultRel(s->rri, CMD_INSERT, ONCONFLICT_NONE, NIL);
	s->rel = s->rri->ri_RelationDesc;
	s->partitioned = s->rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE;
	if (!s->partitioned && s->rel->rd_rel->relhasindex &&
		s->rri->ri_IndexRelationDescs == NULL)
		ExecOpenIndices(s->rri, false);

	s->child = ExecInitNode(outerPlan(cscan), estate, eflags);
	outerPlanState(css) = s->child;
	s->child_batches = vexec_is_vector_state(s->child) &&
		vexec_node_batchable((VexecNode *) s->child);

	desc = RelationGetDescr(s->rel);
	childdesc = ExecGetResultType(s->child);
	s->natts = desc->natts;
	if (childdesc->natts != s->natts)
		elog(ERROR, "vexec: a VecInsert's child gives %d columns, its target has %d",
			 childdesc->natts, s->natts);
	s->child_slot = ExecInitExtraTupleSlot(estate, childdesc, &TTSOpsVirtual);

	/* a dropped attribute's column is the child's NULL of its own type */
	s->types = palloc(sizeof(VexecType *) * Max(s->natts, 1));
	for (i = 0; i < s->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(childdesc, i);

		s->types[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
	}
	s->gather = vexec_batch_create(s->node.mcxt, s->natts, s->types);
	vexec_batch_begin_rows(s->gather);

	if (s->partitioned)
	{
		HASHCTL		ctl;

		/* COPY FROM's ModifyTableState, for the routing (copyfrom.c:932) */
		s->mtstate = makeNode(ModifyTableState);
		s->mtstate->ps.plan = NULL;
		s->mtstate->ps.state = estate;
		s->mtstate->operation = CMD_INSERT;
		s->mtstate->mt_nrels = 1;
		s->mtstate->resultRelInfo = s->rri;
		s->mtstate->rootResultRelInfo = s->rri;
		s->root_slot = ExecInitExtraTupleSlot(estate, desc, &TTSOpsVirtual);
		s->copy_slot = ExecInitExtraTupleSlot(estate, desc, &TTSOpsVirtual);
		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(VexecInsertTarget);
		ctl.hcxt = s->node.mcxt;
		s->parts = hash_create("vexec VecInsert partitions", 16, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		if (!s->explain_only)
			s->proute = ExecSetupPartitionTupleRouting(estate, s->rel);
		return;
	}

	/* its sink, or table_multi_insert()'s slots, at the first rows written */
	target_init(s, &s->target, s->rri);
}

/* Raise PostgreSQL's error for a row of the batch that fails a check. */
pg_noreturn static void
fail_row(VexecInsertState *s, VexecInsertTarget *t, VexecBatch *b, int row)
{
	EState	   *estate = s->node.css.ss.ps.state;

	vexec_batch_store_row(b, row, t->row_slot);
	t->row_slot->tts_tableOid = t->relid;
	ExecConstraints(t->rri, t->row_slot, estate);
	elog(ERROR, "vexec: a VecInsert row failed NOT NULL, and ExecConstraints() passed it");
}

/* NOT NULL, from the columns' validity: the first failing row raises. */
static void
check_notnull(VexecInsertState *s, VexecInsertTarget *t, VexecBatch *b)
{
	int			first = -1;
	int			i;

	for (i = 0; i < t->natts; i++)
	{
		VexecVec   *v = &b->cols[i];
		int			w;

		if (!t->notnull[i] || v->validity == NULL)
			continue;
		if (v->encoding == VEXEC_CONST)
		{
			if (!vexec_bit(v->validity, 0))
				first = 0;
			continue;
		}
		for (w = 0; w < VEXEC_WORDS(b->nrows); w++)
		{
			int			base = w * 64;
			int			n = Min(64, b->nrows - base);
			uint64		mask = n == 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << n) - 1;
			uint64		nulls = ~v->validity[w] & mask;

			if (nulls != 0)
			{
				int			row = base + pg_rightmost_one_pos64(nulls);

				if (first < 0 || row < first)
					first = row;
				break;
			}
		}
	}
	if (first >= 0)
		fail_row(s, t, b, first);
}

/*
 * The checks PostgreSQL's own functions make, a row at a time in row order:
 * stored generated columns, constraints, a partition's constraint.  With
 * generated columns the rows are the generated batch's, which it returns.
 */
static VexecBatch *
check_rows(VexecInsertState *s, VexecInsertTarget *t, VexecBatch *b)
{
	EState	   *estate = s->node.css.ss.ps.state;
	TupleTableSlot *slot = t->row_slot;
	int			row;

	if (t->has_generated)
	{
		vexec_batch_reset(t->generated);
		vexec_batch_begin_rows(t->generated);
	}
	for (row = 0; row < b->nrows; row++)
	{
		ResetPerTupleExprContext(estate);
		vexec_batch_store_row(b, row, slot);
		slot->tts_tableOid = t->relid;
		if (t->has_generated)
			ExecComputeStoredGenerated(t->rri, estate, slot, CMD_INSERT);
		if (t->rel->rd_att->constr)
			ExecConstraints(t->rri, slot, estate);
		if (t->rel->rd_rel->relispartition)
			ExecPartitionCheck(t->rri, slot, estate, true);
		if (t->has_generated)
			vexec_batch_add_slot(t->generated, slot, NULL);
	}
	s->rows_checked += b->nrows;
	ResetPerTupleExprContext(estate);
	return t->has_generated ? t->generated : b;
}

/* Each row's index entries, with its TID. */
static void
insert_index_entries(VexecInsertState *s, VexecInsertTarget *t, VexecBatch *b,
					 const ItemPointerData *tids)
{
	EState	   *estate = s->node.css.ss.ps.state;
	TupleTableSlot *slot = t->row_slot;
	int			row;

	for (row = 0; row < b->nrows; row++)
	{
		List	   *recheck;

		ResetPerTupleExprContext(estate);
		vexec_batch_store_row(b, row, slot);
		slot->tts_tableOid = t->relid;
		slot->tts_tid = tids[row];
		recheck = ExecInsertIndexTuples(t->rri, estate, 0, slot, NIL, NULL);
		list_free(recheck);
	}
	ResetPerTupleExprContext(estate);
}

/*
 * A DATUM column's out-of-line and compressed values made plain, as a sink
 * takes them (vexec_sink.h).
 */
static void
fetch_external(VexecBatch *b, VexecVec *v)
{
	Datum	   *values = v->values;
	Datum	   *copy = NULL;
	int			i;

	if (v->type->typlen != -1)
		return;
	for (i = 0; i < v->nvalues; i++)
	{
		varlena    *vl;

		if (v->validity != NULL && !vexec_bit(v->validity, i))
			continue;
		vl = (varlena *) DatumGetPointer(values[i]);
		if (!VARATT_IS_EXTERNAL(vl) && !VARATT_IS_COMPRESSED(vl))
			continue;
		if (copy == NULL)
		{
			copy = vexec_batch_alloc(b, sizeof(Datum) * Max(v->nvalues, 1));
			memcpy(copy, values, sizeof(Datum) * v->nvalues);
		}
		{
			MemoryContext old = MemoryContextSwitchTo(b->mcxt);

			copy[i] = PointerGetDatum(detoast_attr(vl));
			MemoryContextSwitchTo(old);
		}
	}
	if (copy != NULL)
		v->values = copy;
}

/*
 * The batch through the sink: false when a column will not convert to the
 * layout the sink takes, and table_multi_insert() writes it instead.
 */
static bool
write_sink(VexecInsertState *s, VexecInsertTarget *t, VexecBatch *b)
{
	TupleDesc	desc = RelationGetDescr(t->rel);
	VexecSinkBatch sb;
	int			i;

	for (i = 0; i < t->natts; i++)
	{
		VexecColumn *c = &t->columns[i];
		VexecVec	v;

		memset(c, 0, sizeof(VexecColumn));
		c->encoding = VEXEC_FLAT;
		if (TupleDescAttr(desc, i)->attisdropped)
		{
			/* every row NULL */
			c->layout = VEXEC_FIXED;
			c->validity = vexec_bitmap_alloc(b, b->nrows, false);
			continue;
		}
		v = b->cols[i];
		if (v.encoding != VEXEC_FLAT)
			vexec_vec_flatten(b, &v);
		if (!vexec_shape_equal(&v.shape, &t->shapes[i]) &&
			!vexec_vec_convert(b, &v, &t->shapes[i]))
			return false;
		if (v.shape.layout == VEXEC_DATUM)
			fetch_external(b, &v);
		c->layout = v.shape.layout;
		c->arrow_values = v.shape.arrow_values;
		c->scale = v.shape.scale;
		c->width = v.shape.width;
		c->stride = v.shape.stride;
		c->validity = v.validity;
		c->values = v.values;
		c->buffers = (const void *const *) v.buffers;
		c->buffer_sizes = v.buffer_sizes;
		c->nbuffers = v.nbuffers;
	}
	memset(&sb, 0, sizeof(sb));
	sb.nrows = b->nrows;
	sb.columns = t->columns;
	sb.tids = t->has_indexes ? t->tids : NULL;
	t->sink->put(t->sinkstate, &sb);
	if (t->has_indexes)
		insert_index_entries(s, t, b, t->tids);
	t->batches_sink++;
	return true;
}

/* The batch as COPY writes its rows: table_multi_insert() and the indexes. */
static void
write_multi(VexecInsertState *s, VexecInsertTarget *t, VexecBatch *b)
{
	EState	   *estate = s->node.css.ss.ps.state;
	int			start;

	if (t->bistate == NULL)
		t->bistate = GetBulkInsertState();
	if (t->slots == NULL)
		t->slots = palloc0(sizeof(TupleTableSlot *) * VEXEC_INSERT_MULTI);
	for (start = 0; start < b->nrows; start += VEXEC_INSERT_MULTI)
	{
		int			n = Min(VEXEC_INSERT_MULTI, b->nrows - start);
		int			k;

		/* the slots, as many as the rows have needed */
		for (; t->nslots < n; t->nslots++)
			t->slots[t->nslots] = ExecInitExtraTupleSlot(estate, RelationGetDescr(t->rel),
														 table_slot_callbacks(t->rel));
		for (k = 0; k < n; k++)
		{
			vexec_batch_store_row(b, start + k, t->slots[k]);
			t->slots[k]->tts_tableOid = t->relid;
		}
		table_multi_insert(t->rel, t->slots, n, estate->es_output_cid, 0, t->bistate);
		for (k = 0; k < n; k++)
		{
			if (t->has_indexes)
			{
				List	   *recheck;

				ResetPerTupleExprContext(estate);
				recheck = ExecInsertIndexTuples(t->rri, estate, 0, t->slots[k], NIL, NULL);
				list_free(recheck);
			}
			ExecClearTuple(t->slots[k]);
		}
		ResetPerTupleExprContext(estate);
	}
	t->batches_multi++;
}

/*
 * A batch of the relation's rows, every one of them selected, written; its
 * rows checked first unless routing checked them.
 */
static void
write_target(VexecInsertState *s, VexecInsertTarget *t, VexecBatch *b, bool checked)
{
	EState	   *estate = s->node.css.ss.ps.state;

	if (b->nrows == 0)
		return;
	if (!checked)
	{
		if (t->row_checks)
			b = check_rows(s, t, b);
		else if (t->any_notnull)
			check_notnull(s, t, b);
	}
	target_start(s, t);
	if (t->sink == NULL || !write_sink(s, t, b))
		write_multi(s, t, b);
	s->inserted += b->nrows;
	if (s->can_set_tag)
		estate->es_processed += b->nrows;
}

/* A partition's pending rows, written. */
static void
flush_pending(VexecInsertState *s, VexecInsertTarget *t)
{
	if (t->pending == NULL || t->pending->nrows == 0)
		return;
	s->npending -= t->pending->nrows;
	write_target(s, t, t->pending, true);
	vexec_batch_reset(t->pending);
	vexec_batch_begin_rows(t->pending);
}

static void
flush_all_pending(VexecInsertState *s)
{
	ListCell   *lc;

	foreach(lc, s->part_list)
		flush_pending(s, lfirst(lc));
}

/* A routed row, of the partition's own row, into its pending batch. */
static void
add_pending(VexecInsertState *s, VexecInsertTarget *t, TupleTableSlot *slot)
{
	if (t->pending == NULL)
	{
		TupleDesc	desc = RelationGetDescr(t->rel);
		VexecType **types = palloc(sizeof(VexecType *) * Max(t->natts, 1));
		int			i;

		for (i = 0; i < t->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			types[i] = att->attisdropped ? vexec_type_make(INT4OID, -1, InvalidOid) :
				vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		}
		t->pending = vexec_batch_create(s->node.mcxt, t->natts, types);
		vexec_batch_begin_rows(t->pending);
	}
	vexec_batch_add_slot(t->pending, slot, NULL);
	if (++s->npending >= VEXEC_BATCH_ROWS)
		flush_all_pending(s);
}

/*
 * The partition a row was routed to.  The planner offered the node only for
 * partitions with no trigger on INSERT, no foreign table among them and no
 * deferrable constraint, and the plan depends on each of them
 * (plan/insert.c); one that has gained such a thing since, between the
 * plan's check and the partition's lock, fails the statement rather than go
 * without it.
 */
static VexecInsertTarget *
route_target(VexecInsertState *s, ResultRelInfo *prri)
{
	Oid			relid = RelationGetRelid(prri->ri_RelationDesc);
	VexecInsertTarget *t;
	TriggerDesc *trig = prri->ri_TrigDesc;
	bool		found;
	int			i;

	t = hash_search(s->parts, &relid, HASH_ENTER, &found);
	if (found)
		return t;
	memset(t, 0, sizeof(VexecInsertTarget));
	t->relid = relid;
	if (prri->ri_FdwRoutine != NULL ||
		(trig != NULL &&
		 (trig->trig_insert_before_row || trig->trig_insert_after_row ||
		  trig->trig_insert_instead_row || trig->trig_insert_new_table)))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("partition \"%s\" has gained a trigger or become a foreign table since the INSERT was planned",
						RelationGetRelationName(prri->ri_RelationDesc)),
				 errhint("Run the statement again.")));
	for (i = 0; i < prri->ri_NumIndices; i++)
		if (!prri->ri_IndexRelationDescs[i]->rd_index->indimmediate)
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("partition \"%s\" has gained a deferrable constraint since the INSERT was planned",
							RelationGetRelationName(prri->ri_RelationDesc)),
					 errhint("Run the statement again.")));
	target_init(s, t, prri);
	t->map = ExecGetRootToChildMap(prri, s->node.css.ss.ps.state);
	s->part_list = lappend(s->part_list, t);
	return t;
}

/*
 * A partition's checks on a routed row, of the partition's own row, as
 * ExecInsert() makes them after routing: its stored generated columns and
 * its constraints; not its partition constraint, which routing proved.
 */
static void
check_routed_row(VexecInsertState *s, VexecInsertTarget *t, TupleTableSlot *slot)
{
	EState	   *estate = s->node.css.ss.ps.state;
	TupleConstr *constr = t->rel->rd_att->constr;
	int			i;

	if (t->has_generated)
		ExecComputeStoredGenerated(t->rri, estate, slot, CMD_INSERT);
	if (constr != NULL &&
		(constr->num_check > 0 || constr->has_generated_stored || constr->has_generated_virtual))
	{
		ExecConstraints(t->rri, slot, estate);
		s->rows_checked++;
		return;
	}
	if (!t->any_notnull)
		return;
	slot_getallattrs(slot);
	for (i = 0; i < t->natts; i++)
		if (t->notnull[i] && slot->tts_isnull[i])
		{
			ExecConstraints(t->rri, slot, estate);
			elog(ERROR, "vexec: a VecInsert row failed NOT NULL, and ExecConstraints() passed it");
		}
}

/*
 * A batch of the partitioned root's rows, every one of them selected:
 * each row routed and checked in row order, as ModifyTable routes and checks
 * them, and into its partition's pending batch; unless every row goes to one
 * partition as it is, when the batch is written whole, after the pending
 * rows.
 */
static void
route_batch(VexecInsertState *s, VexecBatch *b)
{
	EState	   *estate = s->node.css.ss.ps.state;
	VexecInsertTarget *first = NULL;
	bool		whole = true;
	int			row;

	for (row = 0; row < b->nrows; row++)
	{
		ResultRelInfo *prri;
		VexecInsertTarget *t;
		TupleTableSlot *slot;

		ResetPerTupleExprContext(estate);
		vexec_batch_store_row(b, row, s->root_slot);
		prri = ExecFindPartition(s->mtstate, s->rri, s->proute, s->root_slot, estate);
		t = route_target(s, prri);
		if (whole)
		{
			if (first == NULL)
				first = t;
			if (t != first || t->map != NULL || t->has_generated)
			{
				int			r;

				/*
				 * The rows before this one, checked, into the first
				 * partition's batch: writing a full one may raise for one of
				 * them, before this row's checks, as ModifyTable would.
				 */
				whole = false;
				for (r = 0; r < row; r++)
				{
					vexec_batch_store_row(b, r, s->copy_slot);
					add_pending(s, first, s->copy_slot);
				}
			}
		}
		slot = s->root_slot;
		if (t->map != NULL)
			slot = execute_attr_map_slot(t->map->attrMap, slot, t->row_slot);
		slot->tts_tableOid = t->relid;
		check_routed_row(s, t, slot);
		if (!whole)
			add_pending(s, t, slot);
	}
	ResetPerTupleExprContext(estate);
	if (whole && first != NULL)
	{
		flush_all_pending(s);
		write_target(s, first, b, true);
	}
}

/* A batch of the target's rows, every one of them selected, written. */
static void
write_batch(VexecInsertState *s, VexecBatch *b)
{
	if (b->nrows == 0)
		return;
	if (s->partitioned)
		route_batch(s, b);
	else
		write_target(s, &s->target, b, false);
}

/* The rows gathered, written. */
static void
flush_gather(VexecInsertState *s)
{
	if (s->gather->nrows == 0)
		return;
	write_batch(s, s->gather);
	vexec_batch_reset(s->gather);
	vexec_batch_begin_rows(s->gather);
}

static void
gather_slot(VexecInsertState *s, TupleTableSlot *slot)
{
	vexec_batch_add_slot(s->gather, slot, NULL);
	if (s->gather->nrows >= VEXEC_BATCH_ROWS)
		flush_gather(s);
}

static bool
any_bit(const uint64 *bits, int nrows)
{
	int			w;

	if (bits == NULL)
		return false;
	for (w = 0; w < VEXEC_WORDS(nrows); w++)
		if (bits[w] != 0)
			return true;
	return false;
}

/*
 * A vector child's batch.  Without redo rows it is written as it is, its
 * selected rows compacted; with them, its rows go one by one in row order
 * into the gathered batch, the redo rows through the child's evaluator.
 */
static void
put_child_batch(VexecInsertState *s, VexecNode *child, VexecBatch *b)
{
	int			row;

	s->node.stats.batches++;
	s->node.stats.rows_in += b->nrows;
	if (!any_bit(child->redo, b->nrows) && !any_bit(child->child_redo, b->nrows))
	{
		flush_gather(s);
		vexec_batch_compact(b);
		write_batch(s, b);
		return;
	}
	for (row = 0; row < b->nrows; row++)
	{
		TupleTableSlot *slot;

		if ((child->redo != NULL && vexec_bit(child->redo, row)) ||
			(child->child_redo != NULL && vexec_bit(child->child_redo, row)))
		{
			slot = vexec_resolve_row(child, row);
			if (slot == NULL)
				continue;
		}
		else if (b->selection == NULL || vexec_bit(b->selection, row))
		{
			vexec_batch_store_row(b, row, s->child_slot);
			slot = s->child_slot;
		}
		else
			continue;
		gather_slot(s, slot);
	}
}

static TupleTableSlot *
insert_exec(CustomScanState *css)
{
	VexecInsertState *s = (VexecInsertState *) css;

	if (s->done)
		return NULL;
	s->node.ran = true;
	if (s->child_batches)
	{
		VexecNode  *child = (VexecNode *) s->child;
		VexecBatch *b;

		while ((b = vexec_next_batch(s->child)) != NULL)
			put_child_batch(s, child, b);
	}
	else
	{
		for (;;)
		{
			TupleTableSlot *slot = ExecProcNode(s->child);

			if (TupIsNull(slot))
				break;
			s->node.stats.rows_in++;
			gather_slot(s, slot);
		}
	}
	flush_gather(s);
	if (s->partitioned)
		flush_all_pending(s);
	s->done = true;
	return NULL;
}

static void
insert_rescan(CustomScanState *css)
{
	(void) css;
	elog(ERROR, "vexec: a VecInsert cannot be rescanned");
}

static void
insert_end(CustomScanState *css)
{
	VexecInsertState *s = (VexecInsertState *) css;
	ListCell   *lc;

	if (s->partitioned)
	{
		foreach(lc, s->part_list)
			target_end(lfirst(lc));
		if (s->proute != NULL)
			ExecCleanupTupleRouting(s->mtstate, s->proute);
		s->proute = NULL;
	}
	else
		target_end(&s->target);
	vexec_node_end(&s->node);
	ExecEndNode(s->child);
}

static void
insert_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecInsertState *s = (VexecInsertState *) css;
	const char *relname = quote_identifier(RelationGetRelationName(s->rel));
	const VexecSinkRoutine *sink = vexec_find_sink(s->rel->rd_tableam);
	int64		batches_sink = s->target.batches_sink;
	int64		batches_multi = s->target.batches_multi;
	ListCell   *lc;

	(void) ancestors;
	CbExplainRelabel(css, es, "Vec Insert", psprintf(" on %s", relname));
	if (es->format != EXPLAIN_FORMAT_TEXT)
		ExplainPropertyText("Relation Name", RelationGetRelationName(s->rel), es);
	ExplainPropertyText("Write", s->partitioned ? "each partition's" :
						sink != NULL ? psprintf("sink %s", sink->name) : "table_multi_insert", es);
	if (es->verbose)
		ExplainPropertyText("Input", s->child_batches ? "batches" : "rows", es);
	if (es->analyze && s->node.ran)
	{
		foreach(lc, s->part_list)
		{
			VexecInsertTarget *t = lfirst(lc);

			batches_sink += t->batches_sink;
			batches_multi += t->batches_multi;
		}
		ExplainPropertyInteger("Rows Inserted", NULL, s->inserted, es);
		if (s->partitioned)
			ExplainPropertyInteger("Partitions Written", NULL, list_length(s->part_list), es);
		if (batches_sink > 0)
			ExplainPropertyInteger("Batches Through the Sink", NULL, batches_sink, es);
		if (batches_multi > 0)
			ExplainPropertyInteger("Batches Through table_multi_insert", NULL,
								   batches_multi, es);
		if (s->rows_checked > 0)
			ExplainPropertyInteger("Rows Checked Row by Row", NULL, s->rows_checked, es);
	}
}
