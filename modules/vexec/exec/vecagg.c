/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecagg.c
 *	  VecAgg: aggregation, plain or hashed, of any split, its input a batch
 *	  at a time (pg_vector_executor.md §3.8).
 *
 * The node reads its child's batches -- or its rows, a batch at a time,
 * where the child hands up none, as an Agg reads every row of its input
 * anyway -- and the aggregates' arguments and FILTERs are compiled over them
 * as a VecResult's targets are (expr/): the eager ones a batch at a time,
 * the rest, and the rows a kernel could not compute, by PostgreSQL's
 * evaluator, a row at a time.
 *
 * Each row then finds its group and advances the aggregates' states there
 * (aggtrans.c).  What a row of nodeAgg.c runs, it runs in nodeAgg.c's
 * order (execExpr.c:3700-3960): the group looked up, then each transition
 * in turn, its FILTER, its arguments, its function.  A vectorized transition
 * takes the batch's rows at once after that, where the order cannot show:
 * where it cannot raise and its sums do not depend on the order (counts,
 * integer and numeric sums, min, max, bool_and), or, for a float's sums,
 * where no row of the batch goes to PostgreSQL's evaluator and nothing else
 * in the node can raise.  Anywhere else it is advanced in the row loop, in
 * its turn.  So an error is the one nodeAgg.c raises, at the row it raises
 * it at, and a float's sum is the same bits.
 *
 * Grouping is a hash table over the full keys (lib/simplehash.h), hashed
 * and compared as the grouping operators compare: integers and the like by
 * their bits, text of a deterministic collation and bytea by their bytes,
 * any other type through its hash and equality functions.  The keys are
 * plain columns of the input, as an Agg's are (grpColIdx).
 *
 * The table is held to get_hash_memory_limit(), measured with
 * MemoryContextMemAllocated() each time a group is made.  Past it, a row
 * whose group is not in the table goes to the partition on disk that bits
 * of its hash choose -- the input row itself, its arguments evaluated again
 * when it is read back, as nodeAgg.c's spill does (nodeAgg.c:3029-3077) --
 * and the partitions are aggregated in turn, through the same batches, once
 * the table's groups are out.  Each level of partitions takes the next bits
 * of the hash; past the last level the table grows, as nodeAgg.c's does
 * when its hash bits run out.
 *
 * Its output is a row a call: a group's keys, the columns taken from its
 * first row, and its aggregates' results, in the scan tuple that
 * custom_scan_tlist describes, through the node's HAVING qual and target
 * list, which read them as INDEX_VAR.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "storage/buffile.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ruleutils.h"
#include "varatt.h"

#include "cb_explain.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/aggtrans.h"
#include "exec/exec.h"
#include "expr/expr.h"

/* How a key column is hashed and compared. */
typedef enum VexecAggKeyKind
{
	KEY_BITS,					/* a by-value type equal where its bits are */
	KEY_BYTES,					/* text of a deterministic collation, bytea:
								 * equal where their bytes are */
	KEY_FMGR					/* the type's hash and equality functions */
} VexecAggKeyKind;

typedef struct VexecAggKeyCol
{
	int			col;			/* the input column, from 0 */
	VexecAggKeyKind kind;
	int16		typlen;
	bool		typbyval;
	Oid			collation;
	FmgrInfo	hash;
	FmgrInfo	eq;
} VexecAggKeyCol;

/* A row's key, as it is looked up. */
typedef struct VexecAggProbe
{
	Datum		value;
	const char *bytes;			/* KEY_BYTES */
	int32		len;
	bool		isnull;
} VexecAggProbe;

/* simplehash's entry: a group, found by its hash and its keys */
typedef struct VexecAggEntry
{
	char	   *group;
	uint32		hash;
	char		status;
} VexecAggEntry;

typedef struct VexecAggState VexecAggState;

static bool group_equal(VexecAggState *s, const char *group);

#define SH_PREFIX vagg
#define SH_ELEMENT_TYPE VexecAggEntry
#define SH_KEY_TYPE char *
#define SH_KEY group
#define SH_HASH_KEY(tb, key) (*(const uint32 *) (key))
#define SH_EQUAL(tb, a, b) group_equal((VexecAggState *) (tb)->private_data, (a))
#define SH_SCOPE static inline
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) ((a)->hash)
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/* A partition of spilled input rows. */
typedef struct VexecAggPartition
{
	BufFile    *file;
	int64		rows;
	int			depth;			/* the level its rows were spilled at */
} VexecAggPartition;

#define VEXEC_AGG_PARTITION_BITS	5
#define VEXEC_AGG_PARTITIONS		(1 << VEXEC_AGG_PARTITION_BITS)
#define VEXEC_AGG_MAX_DEPTH			(32 / VEXEC_AGG_PARTITION_BITS)

struct VexecAggState
{
	VexecNode	node;			/* first: the input side, the aggregates'
								 * arguments and FILTERs compiled over the
								 * input's columns */
	PlanState  *child;
	bool		child_batches;	/* the child hands up batches */
	bool		child_done;		/* the child has returned its last row: a
								 * node is not called past its end, where an
								 * index scan would begin again */
	VexecBatch *rowbatch;		/* rows into a batch: the child's, or a
								 * partition's */
	TupleDesc	indesc;

	AggStrategy strategy;
	AggSplit	split;
	double		numgroups;

	/* the keys, and the row being looked up */
	int			nkeys;
	VexecAggKeyCol *keys;
	VexecAggProbe *probe;		/* the row being looked up: its keys of
								 * rowkeys, or probe_slot's */
	VexecAggProbe *probe_slot;	/* a row PostgreSQL's evaluator has */
	VexecAggProbe *rowkeys;		/* a batch's keys, nkeys a row */
	uint32	   *rowhash;		/* and their hashes */
	int		   *rows;			/* the batch's rows to take, in order */
	char	   *probe_group;	/* stands for the probe in the table */

	/* the columns a group keeps from its first row */
	int			nextra;
	int		   *extracol;

	/* what each column of the scan tuple is */
	int			noutcols;
	int		   *outkind;		/* VEXEC_AGGCOL_* */
	int		   *outsrc;

	/* the aggregates */
	AggState   *aggstate;
	int			naggs;
	int			ntrans;
	VexecAggTrans *trans;
	int			ninputs;		/* the input targets: each transition's
								 * arguments, then its FILTER */
	bool		any_lazy;		/* an input PostgreSQL's evaluator computes */
	bool		floats_ok;		/* a float's transition may take a batch at
								 * once: nothing else here can raise */
	bool	   *batched;		/* per transition, for the current batch */
	Datum	   *argvals;
	bool	   *argnulls;

	/* a group: a header with its hash, keys, extras, states */
	Size		keyoff;
	Size		extraoff;
	Size		stateoff;
	Size		groupsize;

	/* the table */
	ExprContext *aggcontext;	/* the states' memory, as AggCheckCallContext
								 * gives it */
	ExprContext *tmpcontext;	/* a row's */
	MemoryContext groupcxt;		/* groups, their keys and extras */
	MemoryContext hashcxt;
	struct vagg_hash *table;
	char	   *plaingroup;
	Size		memlimit;
	char	  **groups;			/* per row of a batch: its group */

	/* spill */
	bool		spilling;
	VexecAggPartition *parts;	/* the level being written */
	int			depth;			/* the level of the rows being read */
	List	   *pending;		/* partitions written, to read */
	VexecAggPartition *reading;
	bool	   *spillneeded;	/* per input column: kept when spilled */
	TupleTableSlot *spillslot;
	TupleTableSlot *readslot;

	/* output */
	bool		filled;
	bool		done;
	bool		iterating;
	vagg_iterator iter;

	/* figures */
	int64		ngroups;
	int64		total_groups;
	Size		peak_memory;
	int64		spilled_rows;
	int64		spilled_partitions;
	int			max_depth;
	int64		rows_by_postgres;
	int64		rows_batched;

	/*
	 * H2: the requests the child's source answers from its statistics, a
	 * unit of rows at a time; nstats -1 where it is asked none
	 */
	int			nstats;
	VexecSourceAgg *stats_reqs;
	VexecSourceAggAnswer *stats_answers;
	int		   *stats_first;	/* per transition: its first request */
};

static void agg_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *agg_exec(CustomScanState *css);
static void agg_end(CustomScanState *css);
static void agg_rescan(CustomScanState *css);
static void agg_explain(CustomScanState *css, List *ancestors, ExplainState *es);

static const CustomExecMethods agg_exec_methods = {
	.CustomName = VEXEC_AGG_NAME,
	.BeginCustomScan = agg_begin,
	.ExecCustomScan = agg_exec,
	.EndCustomScan = agg_end,
	.ReScanCustomScan = agg_rescan,
	.ExplainCustomScan = agg_explain,
};

Node *
vexec_create_agg_state(CustomScan *cscan)
{
	VexecAggState *s = palloc0(sizeof(VexecAggState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &agg_exec_methods;
	s->node.kind = VEXEC_NODE_AGG;
	return (Node *) s;
}

/* ---------------------------------------------------------------------
 * The plan's private list (plan/build.c writes it)
 * ---------------------------------------------------------------------
 */

/*
 * A plan travels as text, to parallel workers and to the segments; a Float
 * is written with its point, or it reads back as an Integer.
 */
List *
vexec_agg_plan_encode(const VexecAggPlan *plan)
{
	return list_make5(list_make3(makeInteger(plan->strategy), makeInteger(plan->split),
								 makeFloat(psprintf("%.1f", plan->numgroups))),
					  list_make3(plan->keycols, plan->eqops, plan->collations),
					  list_make2(plan->outkind, plan->outsrc),
					  plan->aggrefs,
					  makeInteger(plan->ntrans));
}

void
vexec_agg_plan_decode(CustomScan *cscan, VexecAggPlan *plan)
{
	List	   *priv = cscan->custom_private;
	List	   *head;
	List	   *keys;
	List	   *outs;

	if (list_length(priv) != 5)
		elog(ERROR, "vexec: a VecAgg plan of %d private items", list_length(priv));
	head = linitial(priv);
	keys = lsecond(priv);
	outs = lthird(priv);
	plan->strategy = intVal(linitial(head));
	plan->split = intVal(lsecond(head));
	plan->numgroups = floatVal(lthird(head));
	plan->keycols = linitial(keys);
	plan->eqops = lsecond(keys);
	plan->collations = lthird(keys);
	plan->outkind = linitial(outs);
	plan->outsrc = lsecond(outs);
	plan->aggrefs = lfourth(priv);
	plan->ntrans = intVal(list_nth(priv, 4));
}

/* ---------------------------------------------------------------------
 * Groups and their keys
 * ---------------------------------------------------------------------
 */

static inline Datum *
group_keys(VexecAggState *s, char *g)
{
	return (Datum *) (g + s->keyoff);
}

static inline bool *
group_keynulls(VexecAggState *s, char *g)
{
	return (bool *) (g + s->keyoff + sizeof(Datum) * s->nkeys);
}

static inline Datum *
group_extras(VexecAggState *s, char *g)
{
	return (Datum *) (g + s->extraoff);
}

static inline bool *
group_extranulls(VexecAggState *s, char *g)
{
	return (bool *) (g + s->extraoff + sizeof(Datum) * s->nextra);
}

static inline char *
group_state(VexecAggState *s, char *g, VexecAggTrans *t)
{
	return g + s->stateoff + t->stateoff;
}

/* A text or bytea Datum's bytes, detoasted into the current context. */
static void
varlena_bytes(Datum d, const char **p, int32 *len)
{
	varlena    *vl = (varlena *) DatumGetPointer(d);

	if (VARATT_IS_EXTENDED(vl))
		vl = pg_detoast_datum_packed(vl);
	*p = VARDATA_ANY(vl);
	*len = VARSIZE_ANY_EXHDR(vl);
}

static inline Datum key_bits(Datum d, int16 typlen);
static inline uint32 key_hash(VexecAggKeyCol *k, VexecAggProbe *p);

/* Whether a group's keys are the probe's: NULLs group together. */
static bool
group_equal(VexecAggState *s, const char *group)
{
	Datum	   *gk = group_keys(s, (char *) group);
	bool	   *gn = group_keynulls(s, (char *) group);
	int			i;

	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecAggProbe *p = &s->probe[i];

		if (gn[i] != p->isnull)
			return false;
		if (p->isnull)
			continue;
		switch (k->kind)
		{
			case KEY_BITS:
				if (key_bits(gk[i], k->typlen) != p->value)
					return false;
				break;
			case KEY_BYTES:
				{
					varlena    *vl = (varlena *) DatumGetPointer(gk[i]);

					if (VARSIZE(vl) - VARHDRSZ != (Size) p->len ||
						memcmp(VARDATA(vl), p->bytes, p->len) != 0)
						return false;
				}
				break;
			case KEY_FMGR:
				if (!DatumGetBool(FunctionCall2Coll(&k->eq, k->collation, gk[i], p->value)))
					return false;
				break;
		}
	}
	return true;
}

/* The probe's hash: each key's, combined as nodeAgg.c combines them. */
static uint32
probe_hash(VexecAggState *s)
{
	uint32		h = 0;
	int			i;

	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecAggProbe *p = &s->probe[i];
		uint32		kh = 0;

		h = pg_rotate_left32(h, 1);
		if (p->isnull)
			continue;
		kh = key_hash(k, p);
		h ^= kh;
	}
	return murmurhash32(h);
}

/*
 * A by-value key's bits: its type's bytes only, as a Datum built from a
 * tuple (fetch_att) and one built by a function (ObjectIdGetDatum) may
 * extend a narrower value's sign otherwise.
 */
static inline Datum
key_bits(Datum d, int16 typlen)
{
	switch (typlen)
	{
		case 1:
			return d & 0xFF;
		case 2:
			return d & 0xFFFF;
		case 4:
			return d & UINT64CONST(0xFFFFFFFF);
		default:
			return d;
	}
}

/* A key's hash, as probe_hash() takes it for one column. */
static inline uint32
key_hash(VexecAggKeyCol *k, VexecAggProbe *p)
{
	switch (k->kind)
	{
		case KEY_BITS:
			return (uint32) murmurhash64((uint64) p->value);
		case KEY_BYTES:
			return hash_bytes((const unsigned char *) p->bytes, p->len);
		default:
			return DatumGetUInt32(FunctionCall1Coll(&k->hash, k->collation, p->value));
	}
}

/* A key value into a probe, from its Datum. */
static void
probe_set(VexecAggKeyCol *k, VexecAggProbe *p, Datum d, bool isnull)
{
	p->isnull = isnull;
	p->value = isnull ? (Datum) 0 : k->kind == KEY_BITS ? key_bits(d, k->typlen) : d;
	if (!isnull && k->kind == KEY_BYTES)
		varlena_bytes(d, &p->bytes, &p->len);
}

/*
 * The keys and hashes of a batch's rows, a key column at a time: integers
 * read from their array, Arrow's bytes where they are, any other value as
 * its Datum.  The rows are those of `rows`, in the batch.
 */
static void
batch_keys(VexecAggState *s, const int *rows, int nlist)
{
	VexecBatch *in = s->node.in;
	int			nk = s->nkeys;
	int			i,
				j;

	for (j = 0; j < nlist; j++)
		s->rowhash[rows[j]] = 0;
	for (i = 0; i < nk; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecVec   *v = &in->cols[k->col];
		bool		ints = k->kind == KEY_BITS && v->encoding == VEXEC_FLAT &&
			v->shape.layout == VEXEC_FIXED && !v->shape.arrow_values &&
			(v->shape.width == 2 || v->shape.width == 4 || v->shape.width == 8) &&
			v->shape.stride == v->shape.width && v->shape.width == k->typlen;
		bool		views = k->kind == KEY_BYTES && v->encoding == VEXEC_FLAT &&
			(v->shape.layout == VEXEC_VIEW || v->shape.layout == VEXEC_OFFSETS);

		for (j = 0; j < nlist; j++)
		{
			int			row = rows[j];
			VexecAggProbe *p = &s->rowkeys[row * nk + i];
			uint32		h = pg_rotate_left32(s->rowhash[row], 1);

			if (ints)
			{
				p->isnull = v->validity != NULL && !vexec_bit(v->validity, row);
				if (!p->isnull)
				{
					if (v->shape.width == 4)
						p->value = (Datum) (uint32) ((const int32 *) v->values)[row];
					else if (v->shape.width == 8)
						p->value = (Datum) ((const int64 *) v->values)[row];
					else
						p->value = (Datum) (uint16) ((const int16 *) v->values)[row];
				}
			}
			else if (views)
			{
				p->isnull = vexec_vec_isnull(v, row);
				if (!p->isnull)
				{
					const char *bytes;
					Size		len;

					vexec_vec_value_bytes(v, row, &bytes, &len);
					p->bytes = bytes;
					p->len = (int32) len;
				}
			}
			else
			{
				bool		isnull;
				Datum		d = vexec_vec_datum(s->node.work, v, row, &isnull);

				probe_set(k, p, d, isnull);
			}
			if (!p->isnull)
				h ^= key_hash(k, p);
			s->rowhash[row] = h;
		}
	}
	for (j = 0; j < nlist; j++)
		s->rowhash[rows[j]] = murmurhash32(s->rowhash[rows[j]]);
}

static void
probe_from_slot(VexecAggState *s, TupleTableSlot *slot)
{
	int			i;

	s->probe = s->probe_slot;
	for (i = 0; i < s->nkeys; i++)
	{
		int			col = s->keys[i].col;

		probe_set(&s->keys[i], &s->probe[i], slot->tts_values[col], slot->tts_isnull[col]);
	}
}

static Size
table_memory(VexecAggState *s)
{
	Size		m = MemoryContextMemAllocated(s->aggcontext->ecxt_per_tuple_memory, true) +
		MemoryContextMemAllocated(s->groupcxt, true) +
		MemoryContextMemAllocated(s->hashcxt, true);

	s->peak_memory = Max(s->peak_memory, m);
	return m;
}

/*
 * A new group: the probe's keys and the row's extras copied, its states
 * begun.  slot is the input row where PostgreSQL's evaluator has it, else
 * the row is the input batch's `row`.
 */
static char *
group_create(VexecAggState *s, uint32 hash, TupleTableSlot *slot, int row)
{
	MemoryContext old = MemoryContextSwitchTo(s->groupcxt);
	char	   *g = palloc0(s->groupsize);
	int			i;

	*(uint32 *) g = hash;
	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		VexecAggProbe *p = &s->probe[i];

		group_keynulls(s, g)[i] = p->isnull;
		if (p->isnull)
			continue;
		if (k->kind == KEY_BYTES)
		{
			varlena    *vl = palloc(VARHDRSZ + p->len);

			SET_VARSIZE(vl, VARHDRSZ + p->len);
			memcpy(VARDATA(vl), p->bytes, p->len);
			group_keys(s, g)[i] = PointerGetDatum(vl);
		}
		else if (k->kind == KEY_BITS)
		{
			/* the Datum a tuple's value would be: fetch_att's */
			switch (k->typlen)
			{
				case 1:
					group_keys(s, g)[i] = CharGetDatum((char) p->value);
					break;
				case 2:
					group_keys(s, g)[i] = Int16GetDatum((int16) p->value);
					break;
				case 4:
					group_keys(s, g)[i] = Int32GetDatum((int32) p->value);
					break;
				default:
					group_keys(s, g)[i] = p->value;
					break;
			}
		}
		else
			group_keys(s, g)[i] = datumCopy(p->value, k->typbyval, k->typlen);
	}
	for (i = 0; i < s->nextra; i++)
	{
		int			col = s->extracol[i];
		Form_pg_attribute att = TupleDescAttr(s->indesc, col);
		bool		isnull;
		Datum		d;

		if (slot != NULL)
		{
			d = slot->tts_values[col];
			isnull = slot->tts_isnull[col];
		}
		else if (row >= 0)
			d = vexec_vec_datum(s->node.work, &s->node.in->cols[col], row, &isnull);
		else
		{
			/* a plain aggregation's group, made before any row */
			d = (Datum) 0;
			isnull = true;
		}
		group_extranulls(s, g)[i] = isnull;
		group_extras(s, g)[i] = isnull ? (Datum) 0 : datumCopy(d, att->attbyval, att->attlen);
	}
	MemoryContextSwitchTo(old);
	for (i = 0; i < s->ntrans; i++)
		vexec_aggtrans_init(&s->trans[i], s->aggstate, group_state(s, g, &s->trans[i]));
	s->ngroups++;
	if (s->strategy == AGG_HASHED)
		s->total_groups++;
	return g;
}

/* An empty table, for the input and for each partition read back. */
static void
table_reset(VexecAggState *s)
{
	ReScanExprContext(s->aggcontext);	/* AggRegisterCallback()'s run */
	MemoryContextReset(s->groupcxt);
	MemoryContextReset(s->hashcxt);
	s->table = NULL;
	s->plaingroup = NULL;
	s->iterating = false;
	s->spilling = false;
	s->ngroups = 0;
	if (s->strategy == AGG_HASHED)
	{
		double		fit = (double) s->memlimit / (double) (s->groupsize + sizeof(VexecAggEntry));
		double		size = Min(Max(s->numgroups, 64), fit);

		s->table = vagg_create(s->hashcxt, (uint32) Min(Max(size, 64), 1048576), s);
	}
	else
		s->plaingroup = group_create(s, 0, NULL, -1);
}

/* ---------------------------------------------------------------------
 * Spill
 * ---------------------------------------------------------------------
 */

/*
 * The partitions, their files and the queue outlive the row context a
 * batch is aggregated in: they are the node's.
 */
static void
spill_begin(VexecAggState *s)
{
	int			i;

	s->spilling = true;
	s->parts = MemoryContextAllocZero(s->node.mcxt,
									  sizeof(VexecAggPartition) * VEXEC_AGG_PARTITIONS);
	for (i = 0; i < VEXEC_AGG_PARTITIONS; i++)
		s->parts[i].depth = s->depth + 1;
	s->max_depth = Max(s->max_depth, s->depth + 1);
}

/* The input row to the partition the hash's next bits choose. */
static void
spill_row(VexecAggState *s, uint32 hash, TupleTableSlot *slot, int row)
{
	int			shift = 32 - VEXEC_AGG_PARTITION_BITS * (s->depth + 1);
	VexecAggPartition *part = &s->parts[(hash >> shift) & (VEXEC_AGG_PARTITIONS - 1)];
	TupleTableSlot *out = s->spillslot;
	MinimalTuple tup;
	bool		shouldFree;
	int			i;

	ExecClearTuple(out);
	for (i = 0; i < s->indesc->natts; i++)
	{
		if (!s->spillneeded[i])
		{
			out->tts_values[i] = (Datum) 0;
			out->tts_isnull[i] = true;
		}
		else if (slot != NULL)
		{
			out->tts_values[i] = slot->tts_values[i];
			out->tts_isnull[i] = slot->tts_isnull[i];
		}
		else
			out->tts_values[i] = vexec_vec_datum(s->node.work, &s->node.in->cols[i], row,
												 &out->tts_isnull[i]);
	}
	ExecStoreVirtualTuple(out);
	tup = ExecFetchSlotMinimalTuple(out, &shouldFree);
	if (part->file == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		part->file = BufFileCreateTemp(false);
		MemoryContextSwitchTo(old);
	}
	BufFileWrite(part->file, tup, tup->t_len);
	part->rows++;
	s->spilled_rows++;
	if (shouldFree)
		pfree(tup);
}

/* The level's partitions, queued to be read. */
static void
spill_finish(VexecAggState *s)
{
	MemoryContext old;
	int			i;

	if (s->parts == NULL)
		return;
	old = MemoryContextSwitchTo(s->node.mcxt);
	for (i = 0; i < VEXEC_AGG_PARTITIONS; i++)
	{
		VexecAggPartition *part = &s->parts[i];
		VexecAggPartition *queued;

		if (part->file == NULL)
			continue;
		if (BufFileSeek(part->file, 0, 0, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind a VecAgg spill file")));
		queued = palloc_object(VexecAggPartition);
		*queued = *part;
		s->pending = lappend(s->pending, queued);
		s->spilled_partitions++;
	}
	pfree(s->parts);
	s->parts = NULL;
	MemoryContextSwitchTo(old);
}

static void
spill_close_all(VexecAggState *s)
{
	ListCell   *lc;

	foreach(lc, s->pending)
	{
		VexecAggPartition *p = lfirst(lc);

		if (p->file)
			BufFileClose(p->file);
	}
	s->pending = NIL;
	if (s->parts != NULL)
	{
		for (int i = 0; i < VEXEC_AGG_PARTITIONS; i++)
			if (s->parts[i].file)
				BufFileClose(s->parts[i].file);
		pfree(s->parts);
		s->parts = NULL;
	}
	if (s->reading != NULL && s->reading->file != NULL)
		BufFileClose(s->reading->file);
	s->reading = NULL;
}

/* ---------------------------------------------------------------------
 * Input
 * ---------------------------------------------------------------------
 */

/* Rows into the row batch, from the child or from a partition. */
static bool
fill_rows(VexecAggState *s)
{
	VexecBatch *in = s->rowbatch;
	TupleDesc	desc = s->indesc;

	vexec_batch_reset(in);
	vexec_batch_begin_rows(in);
	while (in->nrows < VEXEC_BATCH_ROWS)
	{
		TupleTableSlot *slot;

		if (s->reading != NULL)
		{
			BufFile    *f = s->reading->file;
			uint32		len;
			MinimalTuple tup;

			if (BufFileReadMaybeEOF(f, &len, sizeof(uint32), true) == 0)
				break;
			tup = (MinimalTuple) MemoryContextAlloc(in->mcxt, len);
			tup->t_len = len;
			BufFileReadExact(f, (char *) tup + sizeof(uint32), len - sizeof(uint32));
			slot = ExecStoreMinimalTuple(tup, s->readslot, false);
		}
		else
		{
			if (s->child_done)
				break;
			slot = ExecProcNode(s->child);
			if (TupIsNull(slot))
			{
				s->child_done = true;
				break;
			}
		}
		slot_getsomeattrs(slot, desc->natts);
		vexec_batch_add_values(in, slot->tts_values, slot->tts_isnull);
	}
	if (in->nrows == 0)
		return false;
	vexec_batch_apply_config(in, &s->node.layout);
	s->node.in = in;
	return true;
}

/*
 * H2: the units of rows the child's source answers from its statistics,
 * before its next batch, into the plain group's states.
 */
static void
stats_take(VexecAggState *s)
{
	int64		nrows;

	while (vexec_scan_aggregate(s->node.vec_child, s->nstats, s->stats_reqs,
								s->stats_answers, &nrows))
	{
		for (int i = 0; i < s->ntrans; i++)
		{
			VexecAggTrans *t = &s->trans[i];

			vexec_aggtrans_stats_apply(t, s->aggstate, group_state(s, s->plaingroup, t),
									   &s->stats_answers[s->stats_first[i]], nrows);
		}
	}
}

/* The next input batch: the child's, its rows, or a partition's. */
static bool
agg_fetch(VexecNode *node)
{
	VexecAggState *s = (VexecAggState *) node;

	if (s->reading == NULL && s->child_batches)
	{
		VexecNode  *child = node->vec_child;
		VexecBatch *b;

		if (s->nstats >= 0)
			stats_take(s);
		b = vexec_next_batch(s->child);

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
	return fill_rows(s);
}

/*
 * A row's group: found, made, or NULL when the table is past its memory
 * and the row's group is not in it.
 */
static char *
row_group(VexecAggState *s, TupleTableSlot *slot, int row, uint32 hash)
{
	VexecAggEntry *e;
	bool		found;

	if (s->strategy == AGG_PLAIN)
		return s->plaingroup;
	*(uint32 *) s->probe_group = hash;
	if (s->spilling)
	{
		e = vagg_lookup_hash(s->table, s->probe_group, hash);
		return e ? e->group : NULL;
	}
	e = vagg_insert_hash(s->table, s->probe_group, hash, &found);
	if (!found)
	{
		e->group = group_create(s, hash, slot, row);
		/* past the memory the table may hold, no group is made */
		if (s->depth < VEXEC_AGG_MAX_DEPTH - 1 && table_memory(s) > s->memlimit)
			spill_begin(s);
	}
	return e->group;
}

/*
 * A transition's FILTER and arguments for one row, evaluated in nodeAgg.c's
 * order: the FILTER, and the arguments only where it passes.  `pg` asks for
 * every one of them from PostgreSQL's evaluator, over `slot`; otherwise the
 * eager ones are read from their columns.  False where the FILTER rejects
 * the row.
 */
static bool
row_inputs(VexecAggState *s, VexecAggTrans *t, int row, TupleTableSlot *slot, bool pg)
{
	VexecNode  *node = &s->node;
	ExprContext *econtext = s->tmpcontext;
	int			k;

	if (t->filter >= 0)
	{
		Datum		d;
		bool		isnull;

		if (!pg && node->targets[t->filter].eager != NULL)
			d = vexec_vec_datum(node->work, node->outputs[t->filter], row, &isnull);
		else
		{
			econtext->ecxt_outertuple = slot;
			d = ExecEvalExpr(node->targets[t->filter].state, econtext, &isnull);
		}
		if (isnull || !DatumGetBool(d))
			return false;
	}
	for (k = 0; k < t->nargs; k++)
	{
		int			target = t->firstarg + k;

		if (!pg && node->targets[target].eager != NULL)
			s->argvals[k] = vexec_vec_datum(node->work, node->outputs[target], row,
											&s->argnulls[k]);
		else
		{
			econtext->ecxt_outertuple = slot;
			s->argvals[k] = ExecEvalExpr(node->targets[target].state, econtext,
										 &s->argnulls[k]);
		}
	}
	return true;
}

/*
 * One batch, in row order.  A row a kernel computed reads its inputs from
 * the batch's columns; a row a kernel could not compute, or the child's
 * still to be resolved, is PostgreSQL's own, evaluated whole as it is
 * reached.  The vectorized transitions marked batched take their rows after
 * the loop.
 */
static void
agg_batch(VexecAggState *s)
{
	VexecNode  *node = &s->node;
	VexecBatch *in = node->in;
	int			n = in->nrows;
	bool		any_redo = vexec_bits_any(node->redo, n) ||
		(node->child_redo != NULL && vexec_bits_any(node->child_redo, n));
	uint64	   *taken;
	bool		any_batched = false;
	bool		all_batched = true;
	int			nlist;
	int			row;
	int			i,
				j;

	/* which vectorized transitions take this batch at once */
	for (i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];
		bool		ok = t->kind != VEXEC_AGG_FMGR;
		int			k;

		for (k = 0; k < t->nargs && ok; k++)
			ok = node->targets[t->firstarg + k].eager != NULL;
		if (ok && t->filter >= 0)
			ok = node->targets[t->filter].eager != NULL;
		if (ok && (t->may_raise || t->order_dependent))
			ok = !any_redo && s->floats_ok;
		s->batched[i] = ok;
		any_batched |= ok;
		all_batched &= ok;
	}

	/*
	 * A plain aggregation that takes the whole batch at once has nothing to
	 * do a row at a time: its one group takes every row the batch selects.
	 */
	if (s->strategy == AGG_PLAIN && all_batched && !any_redo)
	{
		taken = node->candidates;
		goto batched;
	}

	taken = vexec_bitmap_alloc(node->work, n, false);

	/* the rows to take, in order: the kernels', PostgreSQL's, the child's */
	{
		uint64	   *all = vexec_bits_copy(node->work, node->candidates, n);
		int			r;

		vexec_bits_or_into(all, node->redo, n);
		if (node->child_redo != NULL)
			vexec_bits_or_into(all, node->child_redo, n);
		nlist = 0;
		for (r = vexec_bits_next(all, n, 0); r >= 0; r = vexec_bits_next(all, n, r + 1))
			s->rows[nlist++] = r;
	}
	if (s->strategy == AGG_HASHED)
		batch_keys(s, s->rows, nlist);

	/*
	 * Where every transition takes the batch at once and no row needs
	 * PostgreSQL's evaluator, a row only finds its group.
	 */
	if (s->strategy == AGG_HASHED && all_batched && !any_redo && !s->any_lazy)
	{
		for (j = 0; j < nlist; j++)
		{
			char	   *g;

			row = s->rows[j];
			if (j + 8 < nlist && s->table != NULL)
				__builtin_prefetch(&s->table->data[s->rowhash[s->rows[j + 8]] & s->table->sizemask]);
			s->probe = &s->rowkeys[row * s->nkeys];
			g = row_group(s, NULL, row, s->rowhash[row]);
			if (g == NULL)
			{
				spill_row(s, s->rowhash[row], NULL, row);
				continue;
			}
			s->groups[row] = g;
			vexec_bit_set(taken, row);
		}
		CHECK_FOR_INTERRUPTS();
		goto batched;
	}

	for (j = 0; j < nlist; j++)
	{
		TupleTableSlot *slot = NULL;
		bool		pg = false;
		char	   *g;
		uint32		hash = 0;

		row = s->rows[j];
		ResetExprContext(s->tmpcontext);
		CHECK_FOR_INTERRUPTS();

		/* the bucket of a row ahead, for the table's misses to overlap */
		if (s->strategy == AGG_HASHED && j + 8 < nlist && s->table != NULL)
			__builtin_prefetch(&s->table->data[s->rowhash[s->rows[j + 8]] & s->table->sizemask]);

		if (node->child_redo != NULL && vexec_bit(node->child_redo, row))
		{
			/* the child's row, resolved by PostgreSQL's evaluator */
			slot = vexec_resolve_row(node->vec_child, row);
			if (slot == NULL)
				continue;
			slot_getallattrs(slot);
			node->loaded_row = -1;
			pg = true;
		}
		else if (vexec_bit(node->redo, row))
		{
			vexec_node_load_input(node, row);
			slot = node->input_slot;
			pg = true;
		}

		if (s->strategy == AGG_HASHED)
		{
			if (node->child_redo != NULL && vexec_bit(node->child_redo, row))
			{
				probe_from_slot(s, slot);
				hash = probe_hash(s);
			}
			else
			{
				s->probe = &s->rowkeys[row * s->nkeys];
				hash = s->rowhash[row];
			}
		}
		g = row_group(s, slot, row, hash);

		if (pg)
			s->rows_by_postgres++;
		else if (s->any_lazy)
		{
			/* the lazy inputs read the input row */
			vexec_node_load_input(node, row);
			slot = node->input_slot;
		}

		if (g == NULL)
		{
			/*
			 * Its group is not in the table: the row goes to a partition.
			 * nodeAgg.c still evaluates its FILTERs and arguments now, and
			 * again when it reads the row back (execExpr.c:3700-3960, the
			 * group's NULL check coming last); so do the lazy ones here.
			 */
			for (i = 0; i < s->ntrans && (pg || s->any_lazy); i++)
				(void) row_inputs(s, &s->trans[i], row, slot, pg);
			spill_row(s, hash, pg ? slot : NULL, row);
			continue;
		}

		if (!pg)
		{
			s->groups[row] = g;
			vexec_bit_set(taken, row);
		}
		for (i = 0; i < s->ntrans; i++)
		{
			VexecAggTrans *t = &s->trans[i];

			if (!pg && s->batched[i])
				continue;
			if (row_inputs(s, t, row, slot, pg))
				vexec_aggtrans_advance(t, s->aggstate, group_state(s, g, t),
									   s->argvals, s->argnulls);
		}
	}

batched:
	if (!any_batched || !vexec_bits_any(taken, n))
		return;
	s->rows_batched += vexec_bits_count(taken, n);
	for (i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];

		if (!s->batched[i])
			continue;
		vexec_aggtrans_batch(t, s->aggstate, node->work,
							 s->strategy == AGG_PLAIN ? NULL : s->groups,
							 s->stateoff + t->stateoff,
							 s->strategy == AGG_PLAIN ? group_state(s, s->plaingroup, t) : NULL,
							 taken, n,
							 t->nargs > 0 ? node->outputs[t->firstarg] : NULL,
							 t->filter >= 0 ? node->outputs[t->filter] : NULL);
	}
	ResetExprContext(s->tmpcontext);
}

/*
 * Every input row -- the child's, or a partition's -- into the table.  A
 * batch is aggregated in the row context, which each row resets, as
 * nodeAgg.c's tmpcontext: what a key's or an argument's detoasting, or a
 * hash, equality or transition function, allocates is freed as it goes.
 */
static void
agg_fill(VexecAggState *s)
{
	while (vexec_node_next_input(&s->node))
	{
		MemoryContext old = MemoryContextSwitchTo(s->tmpcontext->ecxt_per_tuple_memory);

		agg_batch(s);
		MemoryContextSwitchTo(old);
	}
	if (s->reading != NULL)
	{
		BufFileClose(s->reading->file);
		s->reading->file = NULL;
		s->reading = NULL;
	}
	spill_finish(s);
	s->filled = true;
}

/* ---------------------------------------------------------------------
 * Output
 * ---------------------------------------------------------------------
 */

/* A group's columns into the scan tuple. */
static TupleTableSlot *
group_output(VexecAggState *s, char *g)
{
	TupleTableSlot *slot = s->node.css.ss.ss_ScanTupleSlot;
	int			i;

	ExecClearTuple(slot);
	for (i = 0; i < s->noutcols; i++)
	{
		int			src = s->outsrc[i];

		switch (s->outkind[i])
		{
			case VEXEC_AGGCOL_KEY:
				slot->tts_values[i] = group_keys(s, g)[src];
				slot->tts_isnull[i] = group_keynulls(s, g)[src];
				break;
			case VEXEC_AGGCOL_EXTRA:
				slot->tts_values[i] = group_extras(s, g)[src];
				slot->tts_isnull[i] = group_extranulls(s, g)[src];
				break;
			case VEXEC_AGGCOL_AGG:
				{
					VexecAggTrans *t = &s->trans[s->aggstate->peragg[src].transno];
					VexecAggTrans *st = t->share >= 0 ? &s->trans[t->share] : t;

					vexec_aggtrans_result(t, s->aggstate, src, group_state(s, g, st),
										  &slot->tts_values[i], &slot->tts_isnull[i]);
				}
				break;
		}
	}
	return ExecStoreVirtualTuple(slot);
}

/* The next group of the table, or NULL once every one is out. */
static char *
next_group(VexecAggState *s)
{
	VexecAggEntry *e;

	if (s->strategy == AGG_PLAIN)
	{
		char	   *g = s->plaingroup;

		s->plaingroup = NULL;
		return g;
	}
	if (!s->iterating)
	{
		vagg_start_iterate(s->table, &s->iter);
		s->iterating = true;
	}
	e = vagg_iterate(s->table, &s->iter);
	return e != NULL ? e->group : NULL;
}

static TupleTableSlot *
agg_exec(CustomScanState *css)
{
	VexecAggState *s = (VexecAggState *) css;
	ExprContext *econtext = css->ss.ps.ps_ExprContext;

	for (;;)
	{
		char	   *g;

		if (s->done)
			return ExecClearTuple(css->ss.ps.ps_ResultTupleSlot);
		if (!s->filled)
			agg_fill(s);

		ResetExprContext(econtext);
		g = next_group(s);
		if (g == NULL)
		{
			/* the table is out: the next partition, if any */
			if (s->pending == NIL)
			{
				s->done = true;
				continue;
			}
			s->reading = linitial(s->pending);
			s->pending = list_delete_first(s->pending);
			s->depth = s->reading->depth;
			table_reset(s);
			vexec_node_rescan(&s->node);
			s->filled = false;
			continue;
		}

		econtext->ecxt_scantuple = group_output(s, g);
		if (css->ss.ps.qual != NULL && !ExecQual(css->ss.ps.qual, econtext))
		{
			InstrCountFiltered1(css, 1);
			continue;
		}
		if (css->ss.ps.ps_ProjInfo != NULL)
			return ExecProject(css->ss.ps.ps_ProjInfo);
		return ExecCopySlot(css->ss.ps.ps_ResultTupleSlot, econtext->ecxt_scantuple);
	}
}

/* ---------------------------------------------------------------------
 * Begin, rescan, end
 * ---------------------------------------------------------------------
 */

/* How a grouping column's operator lets its keys be hashed and compared. */
static VexecAggKeyKind
key_kind(Oid eqop, Oid collation, Form_pg_attribute att)
{
	RegProcedure eqfn = get_opcode(eqop);

	switch (eqfn)
	{
		case F_INT2EQ:
		case F_INT4EQ:
		case F_INT8EQ:
		case F_OIDEQ:
		case F_BOOLEQ:
		case F_CHAREQ:
		case F_DATE_EQ:
		case F_TIME_EQ:
		case F_TIMESTAMP_EQ:
		case F_TIMESTAMPTZ_EQ:
		case F_CASH_EQ:
			if (att->attbyval)
				return KEY_BITS;
			break;
		case F_BYTEAEQ:
			return KEY_BYTES;
		case F_TEXTEQ:
			if (!OidIsValid(collation) || get_collation_isdeterministic(collation))
				return KEY_BYTES;
			break;
		default:
			break;
	}
	return KEY_FMGR;
}

/*
 * H8 (§3.14): SUM(x + k) -- int4_sum over int2 x plus, or minus, an int4
 * constant with which no int2 overflows int4 -- as SUM(x) + k * COUNT(x).
 * x + k can raise nothing, so leaving it unevaluated changes no error.
 * False for any other aggregate.
 */
static bool
sum_offset(VexecAggTrans *t, Expr **xp, int64 *kp)
{
	Aggref	   *aggref = t->aggref;
	Expr	   *arg;
	OpExpr	   *op;
	Expr	   *x;
	Node	   *c;
	int64		k;

	if (DO_AGGSPLIT_COMBINE(aggref->aggsplit) || aggref->aggfilter != NULL ||
		list_length(aggref->args) != 1 || t->pertrans->transfn_oid != F_INT4_SUM)
		return false;
	arg = linitial_node(TargetEntry, aggref->args)->expr;
	if (!IsA(arg, OpExpr) || list_length(((OpExpr *) arg)->args) != 2)
		return false;
	op = (OpExpr *) arg;
	set_opfuncid(op);
	switch (op->opfuncid)
	{
		case F_INT24PL:
		case F_INT24MI:
			x = linitial(op->args);
			c = lsecond(op->args);
			break;
		case F_INT42PL:
			x = lsecond(op->args);
			c = linitial(op->args);
			break;
		default:
			return false;
	}
	if (!IsA(c, Const) || ((Const *) c)->constisnull || exprType((Node *) x) != INT2OID)
		return false;
	k = DatumGetInt32(((Const *) c)->constvalue);
	if (op->opfuncid == F_INT24MI)
		k = -k;
	if (k > (int64) PG_INT32_MAX - PG_INT16_MAX || k < (int64) PG_INT32_MIN - PG_INT16_MIN)
		return false;
	*xp = x;
	*kp = k;
	return true;
}

/* The input columns an expression reads, as OUTER_VAR Vars. */
static bool
mark_outer_vars(Node *node, bool *needed)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == OUTER_VAR && var->varattno > 0)
			needed[var->varattno - 1] = true;
		return false;
	}
	return expression_tree_walker(node, mark_outer_vars, needed);
}

/*
 * H2: whether the child's source is asked to answer the aggregates from its
 * statistics.  A plain aggregate over a VecScan with no qual whose source
 * can; and every transition, without a FILTER, one whose state statistics
 * give exactly (vexec_aggtrans_stats_requests()), over nothing or over a
 * column of the scan's table.
 */
static void
stats_setup(VexecAggState *s, List *inputs)
{
	VexecNode  *child = s->node.vec_child;
	Plan	   *cplan;
	Index		scanrelid;
	VexecSourceAgg *reqs;
	int			n = 0;

	s->nstats = -1;
	if (!vexec_aggregate_statistics || s->strategy != AGG_PLAIN ||
		DO_AGGSPLIT_COMBINE(s->split) || child == NULL ||
		child->kind != VEXEC_NODE_SCAN || !vexec_scan_can_aggregate(child))
		return;
	cplan = child->css.ss.ps.plan;
	scanrelid = ((Scan *) cplan)->scanrelid;
	reqs = palloc0_array(VexecSourceAgg, 3 * Max(s->ntrans, 1));
	s->stats_first = palloc0_array(int, Max(s->ntrans, 1));
	for (int i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];
		AttrNumber	attnum = 0;
		Oid			argtype = InvalidOid;
		int			k;

		if (t->filter >= 0 || t->kind == VEXEC_AGG_SUM_OFFSET || t->nargs > 1)
			return;
		if (t->nargs == 1)
		{
			Expr	   *arg = list_nth_node(TargetEntry, inputs, t->firstarg)->expr;
			Var		   *v;
			TargetEntry *ctle;

			if (!IsA(arg, Var) || ((Var *) arg)->varno != OUTER_VAR ||
				((Var *) arg)->varattno < 1 ||
				((Var *) arg)->varattno > list_length(cplan->targetlist))
				return;
			ctle = list_nth_node(TargetEntry, cplan->targetlist, ((Var *) arg)->varattno - 1);
			if (!IsA(ctle->expr, Var))
				return;
			v = (Var *) ctle->expr;
			if (v->varno != scanrelid || v->varattno < 1 || v->varlevelsup != 0)
				return;
			attnum = v->varattno;
			argtype = v->vartype;
		}
		k = vexec_aggtrans_stats_requests(t, argtype, attnum, &reqs[n]);
		if (k < 0)
			return;
		s->stats_first[i] = n;
		n += k;
	}
	s->nstats = n;
	s->stats_reqs = reqs;
	s->stats_answers = palloc0_array(VexecSourceAggAnswer, Max(n, 1));
}

static void
agg_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecAggState *s = (VexecAggState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	VexecAggPlan plan;
	List	   *inputs = NIL;
	bool	   *inexact;
	Expr	  **offset_x;
	ListCell   *lc;
	Size		off;
	int			i;

	vexec_node_begin(node, estate);
	node->fetch = agg_fetch;
	vexec_agg_plan_decode(cscan, &plan);
	s->strategy = plan.strategy;
	s->split = plan.split;
	s->numgroups = plan.numgroups;

	/* the child: its batches where it hands them up, else its rows */
	s->child = ExecInitNode(outerPlan(cscan), estate,
							eflags & ~(EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK));
	outerPlanState(css) = s->child;
	s->indesc = ExecGetResultType(s->child);
	s->child_batches = vexec_is_vector_state(s->child) &&
		vexec_node_batchable((VexecNode *) s->child);
	if (s->child_batches)
		node->vec_child = (VexecNode *) s->child;

	node->input_varno = OUTER_VAR;
	node->ninput = s->indesc->natts;
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(s->indesc->natts, 1));
	for (i = 0; i < s->indesc->natts; i++)
		node->input_attnos[i] = i + 1;
	node->input_slot = ExecInitExtraTupleSlot(estate, s->indesc, &TTSOpsVirtual);
	{
		VexecType **types = palloc(sizeof(VexecType *) * Max(s->indesc->natts, 1));

		for (i = 0; i < s->indesc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(s->indesc, i);

			types[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		}
		s->rowbatch = vexec_batch_create(node->mcxt, s->indesc->natts, types);
	}

	/* the keys */
	s->nkeys = list_length(plan.keycols);
	s->keys = palloc0_array(VexecAggKeyCol, Max(s->nkeys, 1));
	s->probe_slot = palloc0_array(VexecAggProbe, Max(s->nkeys, 1));
	s->probe = s->probe_slot;
	s->rowkeys = palloc0_array(VexecAggProbe, (Size) Max(s->nkeys, 1) * VEXEC_BATCH_ROWS);
	s->rowhash = palloc0_array(uint32, VEXEC_BATCH_ROWS);
	s->rows = palloc0_array(int, VEXEC_BATCH_ROWS);
	for (i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *k = &s->keys[i];
		Oid			eqop = list_nth_oid(plan.eqops, i);
		Oid			lhash;
		Oid			rhash;
		Form_pg_attribute att;

		k->col = list_nth_int(plan.keycols, i) - 1;
		if (k->col < 0 || k->col >= s->indesc->natts)
			elog(ERROR, "vexec: grouping column %d of %d", k->col + 1, s->indesc->natts);
		att = TupleDescAttr(s->indesc, k->col);
		k->collation = list_nth_oid(plan.collations, i);
		k->typlen = att->attlen;
		k->typbyval = att->attbyval;
		k->kind = key_kind(eqop, k->collation, att);
		if (!get_op_hash_functions(eqop, &lhash, &rhash))
			elog(ERROR, "vexec: no hash function for the grouping operator %u", eqop);
		fmgr_info(lhash, &k->hash);
		fmgr_info(get_opcode(eqop), &k->eq);
	}

	/* the scan tuple's columns, and the extras */
	s->noutcols = list_length(plan.outkind);
	s->outkind = palloc(sizeof(int) * Max(s->noutcols, 1));
	s->outsrc = palloc(sizeof(int) * Max(s->noutcols, 1));
	s->extracol = palloc(sizeof(int) * Max(s->noutcols, 1));
	for (i = 0; i < s->noutcols; i++)
	{
		s->outkind[i] = list_nth_int(plan.outkind, i);
		s->outsrc[i] = list_nth_int(plan.outsrc, i);
		if (s->outkind[i] == VEXEC_AGGCOL_EXTRA)
		{
			s->extracol[s->nextra] = s->outsrc[i] - 1;
			s->outsrc[i] = s->nextra++;
		}
	}

	/* the aggregates, set up as nodeAgg.c sets them up */
	s->aggcontext = CreateWorkExprContext(estate);
	s->tmpcontext = CreateExprContext(estate);
	s->naggs = list_length(plan.aggrefs);
	s->aggstate = vexec_agg_context_create(&css->ss.ps, estate, s->split, s->naggs,
										   plan.ntrans, s->aggcontext, s->tmpcontext);
	s->trans = vexec_aggtrans_setup(s->aggstate, plan.aggrefs, estate, &s->ntrans);

	/* each transition's arguments, then its FILTER, as the input's targets */
	offset_x = palloc0_array(Expr *, Max(s->ntrans, 1));
	for (i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];
		Aggref	   *aggref = t->aggref;
		int			k = 0;
		Expr	   *x;
		int64		offset;

		if (aggref == NULL)
			elog(ERROR, "vexec: transition %d has no aggregate", i);
		if (sum_offset(t, &x, &offset))
		{
			int			share = -1;

			for (int j = 0; j < i && share < 0; j++)
				if (offset_x[j] != NULL && s->trans[j].share < 0 && equal(offset_x[j], x))
					share = j;
			vexec_aggtrans_set_offset(t, offset, share);
			t->filter = -1;
			if (share >= 0)
			{
				t->firstarg = s->trans[share].firstarg;
				continue;
			}
			offset_x[i] = x;
			t->firstarg = list_length(inputs);
			inputs = lappend(inputs, makeTargetEntry(x, list_length(inputs) + 1, NULL, false));
			continue;
		}
		t->firstarg = list_length(inputs);
		foreach(lc, aggref->args)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			if (k++ >= t->nargs)
				break;
			inputs = lappend(inputs, makeTargetEntry(tle->expr, list_length(inputs) + 1,
													 NULL, false));
		}
		t->filter = -1;
		if (aggref->aggfilter != NULL && !DO_AGGSPLIT_COMBINE(aggref->aggsplit))
		{
			t->filter = list_length(inputs);
			inputs = lappend(inputs, makeTargetEntry(aggref->aggfilter, list_length(inputs) + 1,
													 NULL, false));
		}
	}
	s->ninputs = list_length(inputs);

	/*
	 * An argument under a FILTER is evaluated for rows nodeAgg.c would not
	 * evaluate it for: not exactly its rows, for an InitPlan behind it
	 * (expr/eval.c).
	 */
	inexact = palloc0_array(bool, Max(s->ninputs, 1));
	for (i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];

		if (t->filter >= 0)
			for (int k = 0; k < t->nargs; k++)
				inexact[t->firstarg + k] = true;
	}
	node->target_inexact = inexact;
	vexec_node_compile(node, NIL, inputs);

	/* the kinds: vectorized where the argument's type has one */
	s->floats_ok = true;
	for (i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];
		Oid			argtype = InvalidOid;
		int32		argtypmod = -1;

		if (t->kind == VEXEC_AGG_SUM_OFFSET)
			continue;			/* H8's, set above */
		if (t->nargs > 0)
		{
			argtype = node->targets[t->firstarg].type->typid;
			argtypmod = node->targets[t->firstarg].type->typmod;
		}
		vexec_aggtrans_choose(t, t->aggref->aggsplit, argtype, argtypmod);
		if (t->kind == VEXEC_AGG_FMGR)
			s->floats_ok = false;
	}
	for (i = 0; i < s->ninputs; i++)
		if (node->targets[i].eager == NULL)
		{
			s->any_lazy = true;
			s->floats_ok = false;
		}
	s->batched = palloc0_array(bool, Max(s->ntrans, 1));
	stats_setup(s, inputs);
	s->argvals = palloc0_array(Datum, FUNC_MAX_ARGS);
	s->argnulls = palloc0_array(bool, FUNC_MAX_ARGS);

	/* a group: header, keys and their NULLs, extras and theirs, states */
	off = 0;
	for (i = 0; i < s->ntrans; i++)
	{
		s->trans[i].stateoff = off;
		off += MAXALIGN(s->trans[i].statesize);
	}
	s->keyoff = MAXALIGN(sizeof(uint32));
	s->extraoff = s->keyoff + MAXALIGN((sizeof(Datum) + sizeof(bool)) * s->nkeys);
	s->stateoff = s->extraoff + MAXALIGN((sizeof(Datum) + sizeof(bool)) * s->nextra);
	s->groupsize = s->stateoff + off;
	s->probe_group = palloc0(MAXALIGN(sizeof(uint32)));
	s->groups = palloc0_array(char *, VEXEC_BATCH_ROWS);

	/* spill: the input columns the keys, extras and inputs read */
	s->spillneeded = palloc0_array(bool, Max(s->indesc->natts, 1));
	for (i = 0; i < s->nkeys; i++)
		s->spillneeded[s->keys[i].col] = true;
	for (i = 0; i < s->nextra; i++)
		s->spillneeded[s->extracol[i]] = true;
	mark_outer_vars((Node *) inputs, s->spillneeded);
	s->spillslot = MakeSingleTupleTableSlot(s->indesc, &TTSOpsVirtual);
	s->readslot = MakeSingleTupleTableSlot(s->indesc, &TTSOpsMinimalTuple);

	s->memlimit = get_hash_memory_limit();
	s->groupcxt = BumpContextCreate(node->mcxt, "VecAgg groups", ALLOCSET_DEFAULT_SIZES);
	s->hashcxt = AllocSetContextCreate(node->mcxt, "VecAgg hash table", ALLOCSET_DEFAULT_SIZES);
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY))
		table_reset(s);
}

static void
agg_rescan(CustomScanState *css)
{
	VexecAggState *s = (VexecAggState *) css;

	spill_close_all(s);
	s->depth = 0;
	s->filled = false;
	s->done = false;
	s->child_done = false;
	table_reset(s);
	/* the child rescans on its first call when its parameters changed */
	if (s->child->chgParam == NULL)
		ExecReScan(s->child);
	vexec_node_rescan(&s->node);
}

static void
agg_end(CustomScanState *css)
{
	VexecAggState *s = (VexecAggState *) css;

	spill_close_all(s);
	if (s->aggcontext)
		ReScanExprContext(s->aggcontext);
	if (s->spillslot)
		ExecDropSingleTupleTableSlot(s->spillslot);
	if (s->readslot)
		ExecDropSingleTupleTableSlot(s->readslot);
	ExecEndNode(s->child);
	vexec_node_end(&s->node);
}

/* ---------------------------------------------------------------------
 * EXPLAIN
 * ---------------------------------------------------------------------
 */

static const char *
agg_label(VexecAggState *s)
{
	const char *split = "";

	if (DO_AGGSPLIT_SKIPFINAL(s->split) && DO_AGGSPLIT_COMBINE(s->split))
		split = "Combine ";
	else if (DO_AGGSPLIT_SKIPFINAL(s->split))
		split = "Partial ";
	else if (DO_AGGSPLIT_COMBINE(s->split))
		split = "Finalize ";
	return psprintf("Vec %s%s", split, s->strategy == AGG_HASHED ? "HashAggregate" : "Aggregate");
}

static void
agg_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecAggState *s = (VexecAggState *) css;
	VexecNode  *node = &s->node;
	int			i;

	CbExplainRelabel(css, es, agg_label(s), NULL);
	if (s->nkeys > 0)
	{
		List	   *context = set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan, ancestors);
		List	   *keys = NIL;

		for (i = 0; i < s->nkeys; i++)
		{
			Form_pg_attribute att = TupleDescAttr(s->indesc, s->keys[i].col);
			Var		   *v = makeVar(OUTER_VAR, s->keys[i].col + 1, att->atttypid,
									att->atttypmod, att->attcollation, 0);

			keys = lappend(keys, deparse_expression((Node *) v, context, es->verbose, false));
		}
		ExplainPropertyList("Group Key", keys, es);
	}
	if (es->verbose)
	{
		StringInfoData buf;
		int			fast = 0;

		initStringInfo(&buf);
		for (i = 0; i < s->ntrans; i++)
		{
			if (s->trans[i].kind != VEXEC_AGG_FMGR)
				fast++;
			if (i > 0)
				appendStringInfoString(&buf, ", ");
			vexec_aggtrans_name(&s->trans[i], &buf);
		}
		ExplainPropertyText("Batch Format",
							node->layout.format == VEXEC_FORMAT_ARROW ? "arrow" : "postgres", es);
		ExplainPropertyText("Vector Transitions", psprintf("%d of %d", fast, s->ntrans), es);
		if (s->ntrans > 0)
			ExplainPropertyText("Transitions", buf.data, es);
		ExplainPropertyInteger("Kernel Steps", NULL, node->nkernels, es);
		ExplainPropertyInteger("Fallback Steps", NULL, node->nfallbacks, es);
		ExplainPropertyText("Input", s->child_batches ? "batches" : "rows", es);
		if (s->nstats >= 0)
			ExplainPropertyText("From Statistics",
								vexec_scan_source_name(node->vec_child), es);
	}
	if (es->analyze && node->ran)
	{
		if (s->strategy == AGG_HASHED)
		{
			ExplainPropertyInteger("Groups", NULL, s->total_groups, es);
			ExplainPropertyInteger("Memory Usage", "kB",
								   (int64) ((s->peak_memory + 1023) / 1024), es);
		}
		if (s->spilled_partitions > 0)
		{
			ExplainPropertyInteger("Spilled Partitions", NULL, s->spilled_partitions, es);
			ExplainPropertyInteger("Spilled Rows", NULL, s->spilled_rows, es);
			ExplainPropertyInteger("Spill Depth", NULL, s->max_depth, es);
		}
		if (es->verbose)
		{
			ExplainPropertyInteger("Batches", NULL, node->stats.batches, es);
			if (s->rows_by_postgres > 0)
				ExplainPropertyInteger("Rows Evaluated Row by Row", NULL, s->rows_by_postgres, es);
		}
	}
}
