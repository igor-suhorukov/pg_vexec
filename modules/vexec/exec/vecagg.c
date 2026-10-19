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
 * The table begins at the planner's estimate of the groups, or at the most
 * groups the same node's table held before where that is more (C6, §3.19;
 * aggsizes.c), within the memory it may hold.
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
 * A first stage that stops taking new keys (C2, §3.19).  A partial
 * aggregation under a VecRepartition -- whose final stage, in every
 * participant, combines what it is dealt -- freezes its table once it holds
 * vexec.partial_agg_limit groups, or its groups and their states take
 * VEXEC_AGG_FREEZE_BYTES -- a size that stays in the cache; its buckets,
 * made for the planner's estimate, are not counted -- as ClickHouse's
 * adaptive aggregator does
 * (enable_adaptive_aggregator, CH:src/Interpreters/AdaptiveAggregation.h):
 * a row whose key is in it updates it, and a batch's other rows aggregate
 * into a table of the batch's own, whose groups go on to the repartition as
 * partial states once the batch is done, and which then starts empty.  The
 * final stage merges a key's states, however many, as it merges those of
 * several participants (C1's kernels).  So a grouping that barely reduces
 * -- a key a row or two -- spends nothing on a table of millions, and one
 * that does keeps the reduction its frozen table holds.  A frozen table
 * thaws -- takes new keys again, and does not freeze again until it is
 * reset -- where the groups it hands on repeat their keys, as ClickHouse's
 * thaws a stream that keeps staging the same keys
 * (CH:src/Interpreters/AdaptiveAggregationImpl.h): each repeat is a row the
 * table would have merged where it came, which goes on through the
 * repartition's files as a group of its own.  A HyperLogLog counts the keys
 * handed on since the table froze; past VEXEC_AGG_THAW_EVIDENCE groups, at
 * VEXEC_AGG_THAW_REPEAT groups a key or more, the table thaws.
 *
 * The top n kept inside (C3, §3.19).  A bounded VecSort above lends the
 * node its keys and its bound (vexec_agg_set_topn()), as a bounded sort
 * lends its scan a running bound (H6).  The node then ranks every group by
 * the sort's keys, computed from only the columns they read -- a count is
 * its state -- after HAVING, keeps a heap of the n best, and finishes and
 * projects only those, which it hands up for the sort to order: so the
 * final functions, the projection and the sort run for n groups, not for
 * every one.  Under H5's Gather Merge each participant's groups are its own,
 * so each one's n best hold the n best of all.  Only where the output and
 * HAVING are free of volatile functions and subplans, which would otherwise
 * run for fewer groups than they do.
 *
 * Groups in their keys' order (VC4).  Where the planner found the input a
 * serial scan whose source hands its rows in an order the keys are a prefix
 * of (vexec_source.h's ordering(), minor 4) -- the rows of equal keys one
 * after another -- the node takes it so, if the scan's source says, as it
 * begins, that its rows come so: a row whose keys are the open group's
 * goes to it, any other closes it and opens the next, nothing hashed or
 * looked up but each group's hash once, which a stage above may read (C4).
 * The groups a batch closes are handed up once it is done, the open one
 * kept: so no table and no spill, the groups in two generations of memory,
 * the older one emptied once its groups are out and the open group is the
 * newer one's.  A scan whose rows do not come so -- its storage's units
 * changed since the plan was made, or a top n, a LIMIT's groups, a
 * dealing or a freeze asked of the node -- is hashed as any other.
 * ClickHouse's MergeTree in one part, TPC-H's lineitem grouped by its key:
 * 29-47% faster serially than hashing, 45-65% with 4 MB of work_mem
 * (ch_table_engines.md §3, "VC4, VC6 and VC16, researched").
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
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "executor/nodeAgg.h"
#include "lib/hyperloglog.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "utils/sortsupport.h"
#include "port/pg_bitutils.h"
#include "storage/buffile.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ruleutils.h"
#include "utils/typcache.h"
#include "varatt.h"

#include "cb_explain.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/aggtrans.h"
#include "exec/exec.h"
#include "exec/keyhash.h"
#include "expr/expr.h"
#include "expr/kernel.h"

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
	Oid			eqop;			/* the grouping's equality operator */
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

/* X5: the most slots an array of groups has, Velox's 2M (velox/exec/HashTable.h) */
#define VEXEC_AGG_ARRAY_SLOTS		(1 << 21)

/* C2: the bytes a first stage's table freezes at, ClickHouse's 4 MiB */
#define VEXEC_AGG_FREEZE_BYTES		((Size) 4 * 1024 * 1024)

/*
 * C2: a frozen table thaws once this many groups have gone on, or more, at
 * this many a key or more: a table taking them would have merged a third
 */
#define VEXEC_AGG_THAW_EVIDENCE		16384
#define VEXEC_AGG_THAW_REPEAT		1.5

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
	bool		hashes_from_child;	/* C4: a VecRepartition child deals out
									 * rows by these keys and hands up their
									 * hashes, which are this node's */
	const uint32 *batch_hashes; /* and those of the batch being read, or
								 * NULL */
	uint32		last_hash;		/* the hash of the group last returned */
	int		   *rows;			/* the batch's rows to take, in order */

	/*
	 * VC5: per key, the hashes of a dictionary key's values, for the
	 * dictionary of that identity (D6): a value's hash computed once for
	 * every batch that comes with it
	 */
	uint64	   *dict_hash_ids;
	uint32	  **dict_hashes;
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

	/*
	 * X9: the table's groups in the order they were made, in the table's
	 * memory, which they are handed up in: a group's memory follows the
	 * one made before it, where the buckets' order is the hashes', a cache
	 * miss a group
	 */
	char	  **made;
	int64		nmade;
	int64		made_size;
	int64		next_made;

	/*
	 * X5: the array mode (above): the table an array of groups, a slot for
	 * each combination of the keys' values in their ranges, NULL's first
	 */
	bool		array_ok;		/* every key an integer's bits */
	bool		array_decided;	/* for the current table */
	bool		array_mode;
	char	  **slots;
	int64	   *alo;			/* per key: its range */
	int64	   *ahi;
	int64	   *awidth;			/* per key: its slots, NULL's and the range's */
	int64		nslots;
	int64		array_tables;	/* EXPLAIN ANALYZE: tables made arrays, */
	int64		array_left;		/* and those hashed after all */
	int64		array_slots;	/* the most slots one had */

	/* spill */
	bool		spilling;
	VexecAggPartition *parts;	/* the level being written */
	int			depth;			/* the level of the rows being read */
	List	   *pending;		/* partitions written, to read */
	VexecAggPartition *reading;
	bool	   *spillneeded;	/* per input column: kept when spilled */
	TupleTableSlot *spillslot;
	TupleTableSlot *readslot;

	/*
	 * C2: a first stage under a VecRepartition freezes its table (above);
	 * the batch's table, its groups and their states' memory, handed on as
	 * each batch ends
	 */
	bool		may_freeze;		/* the VecRepartition above allows it */
	bool		frozen;
	bool		thawed;			/* frozen no more until the table is reset */
	hyperLogLogState passed_keys;	/* the keys handed on since it froze */
	bool		passed_keys_made;
	int64		passed_since;	/* the groups handed on since it froze */
	int64		thaw_next;		/* passed_since at the next look */
	struct vagg_hash *btable;
	MemoryContext bgroupcxt;
	MemoryContext bhashcxt;
	ExprContext *baggcontext;
	uint64	   *bmask;			/* the batch's rows whose group is the batch
								 * table's */
	bool		boutput;		/* its groups are being handed on */
	bool		biterating;
	vagg_iterator biter;

	/*
	 * C3: the top n a bounded VecSort above lends (vexec_agg_set_topn()):
	 * the sort's keys, columns of the output, their programs over the scan
	 * tuple and orders, the scan tuple's columns they and HAVING read, and a
	 * heap of the kept rows, the worst at its root
	 */
	int64		topn;
	int			ntopkeys;
	ExprState **topkey;
	SortSupport topssup;
	int16	   *topkeylen;
	bool	   *topkeybyval;
	bool	   *topneeded;
	MemoryContext topcxt;
	struct TopEntry *top;
	int			ntop;
	bool		top_filled;
	int			topnext;
	TupleTableSlot *topslot;
	int64		top_ranked;		/* groups ranked, for EXPLAIN ANALYZE */
	Datum	   *topkv;			/* a group's keys, as it is ranked */
	bool	   *topkn;
	int		   *topkeycol;		/* a key that is a column of the scan tuple:
								 * its index, read as it is; else -1 */
	int		   *topcols;		/* the scan tuple's columns ranking reads */
	int			ntopcols;

	/*
	 * C5 (§3.19): under a LIMIT without ORDER BY, the groups of the topk
	 * smallest hashes kept: a max-heap of the smallest hashes of the groups
	 * made, its root the boundary once it is full, the boundary the
	 * participants share where a VecRepartition gives one, and the stage
	 * whose boundary a final stage takes, its child's
	 */
	int64		topk;
	bool		topk_skip;		/* rows past the boundary passed over */
	uint32	   *theap;
	int			ntheap;
	uint32		tbound;			/* this participant's; PG_UINT32_MAX: none */
	pg_atomic_uint32 *tshared;	/* the participants', or NULL */
	uint32		tfinal;			/* the boundary the table's output is held */
	bool		tfinal_set;		/* to, once the table is filled */
	int64		rows_skipped;	/* EXPLAIN ANALYZE's */

	/* output */
	bool		filled;
	bool		done;
	VexecBatch *outb;			/* X9: the batch of groups a vector parent
								 * takes, the scan tuple's columns */
	uint32	   *outhash;		/* and its rows' groups' hashes */

	/*
	 * X9: the groups dealt into a VecRepartition's partitions as they go
	 * out (vexec_agg_deal()): a batch a partition, the scan tuple's columns
	 * and an int4 of each row's hash, handed up as it fills, and each one's
	 * rest at the end
	 */
	int			ndeal;			/* the partitions; 0: none */
	int			(*deal) (void *arg, uint32 hash);
	void	   *deal_arg;
	VexecType **dealtypes;
	VexecBatch **dealt;
	int			dealt_size;
	int			dealt_out;		/* the partition of the batch handed up last,
								 * or -1 */
	int			deal_rest;		/* at the end: the next partition to hand up */
	Datum	   *dealvals;
	bool	   *dealnulls;

	/* figures */
	int64		ngroups;
	int64		total_groups;
	Size		peak_memory;
	int64		spilled_rows;
	int64		spilled_partitions;
	int			max_depth;
	int64		rows_by_postgres;
	int64		rows_batched;
	uint64		size_key;		/* C6: the node's slot of sizes */
	uint64		buckets_initial;	/* the first table's buckets, and the */
	uint64		buckets_final;	/* most any table had, for EXPLAIN */
	int64		frozen_groups;	/* C2: the groups the table froze at */
	int64		passed_groups;	/* the batch tables' groups handed on */
	int64		thawed_after;	/* those handed on before the table thawed */

	/*
	 * VC4: the input in the keys' order (above): planned, decided at the
	 * first group asked for, and taken; the open group, its generation,
	 * the generation new groups go to, and the two generations' memory --
	 * their groups, and their states' as AggCheckCallContext gives it.
	 * The groups a batch closes wait in `made` to be handed up.
	 */
	bool		sorted_planned;
	bool		sorted_decided;
	bool		sorted;
	bool		sorted_eof;		/* the input is read: the open group is the last */
	char	   *cur;
	int			cur_gen;
	int			sgen;
	MemoryContext sgroupcxt[2];
	ExprContext *saggcontext[2];

	/*
	 * H2: the requests the child's source answers from its statistics, a
	 * unit of rows at a time; nstats -1 where it is asked none
	 */
	int			nstats;
	VexecSourceAgg *stats_reqs;
	VexecSourceAggAnswer *stats_answers;
	int		   *stats_first;	/* per transition: its first request */
};

static void agg_batch(VexecAggState *s);
static void agg_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *agg_exec(CustomScanState *css);
static void agg_end(CustomScanState *css);
static void agg_rescan(CustomScanState *css);
static void agg_explain(CustomScanState *css, List *ancestors, ExplainState *es);
static void table_reset(VexecAggState *s);
static void topk_note(VexecAggState *s, uint32 hash);

static const CustomExecMethods agg_exec_methods = {
	.CustomName = VEXEC_AGG_NAME,
	.BeginCustomScan = agg_begin,
	.ExecCustomScan = agg_exec,
	.EndCustomScan = agg_end,
	.ReScanCustomScan = agg_rescan,
	.ExplainCustomScan = agg_explain,
};

/* Its methods, for pg_otel_probe's reporter of vector nodes (node.c). */
const CustomExecMethods *
vexec_agg_exec_methods(void)
{
	return &agg_exec_methods;
}

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
	return list_make5(list_make5(makeInteger(plan->strategy), makeInteger(plan->split),
								 makeFloat(psprintf("%.1f", plan->numgroups)),
								 makeInteger((int) plan->topk),
								 makeInteger(plan->sorted_input ? 1 : 0)),
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
	plan->topk = list_length(head) > 3 ? intVal(lfourth(head)) : 0;
	plan->sorted_input = list_length(head) > 4 && intVal(list_nth(head, 4)) != 0;
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

/* Probes' hash: each key's, combined as nodeAgg.c combines them. */
static uint32
probes_hash(VexecAggKeyCol *keys, VexecAggProbe *probe, int nkeys)
{
	uint32		h = 0;
	int			i;

	for (i = 0; i < nkeys; i++)
	{
		h = pg_rotate_left32(h, 1);
		if (probe[i].isnull)
			continue;
		h ^= key_hash(&keys[i], &probe[i]);
	}
	return murmurhash32(h);
}

/* The probe's hash. */
static uint32
probe_hash(VexecAggState *s)
{
	return probes_hash(s->keys, s->probe, s->nkeys);
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
			return vexec_hash_bytes(p->bytes, p->len);	/* C4 */
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
 * VC5: the hashes of a dictionary key column's values, as key_hash() gives
 * them, kept for the dictionary's identity; NULL where it has none and more
 * values than the rows they answer, whose hashes are then a row's each.
 */
static const uint32 *
dict_key_hashes(VexecAggState *s, int key, const VexecVec *v, int nrows)
{
	const VexecVec *d = v->dictionary;
	uint32	   *h;
	int			i;

	if (s->dict_hashes == NULL)
	{
		s->dict_hash_ids = MemoryContextAllocZero(s->node.mcxt, sizeof(uint64) * s->nkeys);
		s->dict_hashes = MemoryContextAllocZero(s->node.mcxt, sizeof(uint32 *) * s->nkeys);
	}
	if (v->dict_id != 0 && s->dict_hash_ids[key] == v->dict_id)
		return s->dict_hashes[key];
	if (v->dict_id == 0 && d->nvalues > nrows)
		return NULL;
	if (s->dict_hashes[key])
		pfree(s->dict_hashes[key]);
	h = MemoryContextAlloc(s->node.mcxt, sizeof(uint32) * Max(d->nvalues, 1));
	for (i = 0; i < d->nvalues; i++)
	{
		const char *bytes;
		Size		len;

		vexec_vec_value_bytes(d, i, &bytes, &len);
		h[i] = vexec_hash_bytes(bytes, (int) len);
	}
	s->dict_hashes[key] = h;
	s->dict_hash_ids[key] = v->dict_id;
	return h;
}

/*
 * The keys and hashes of a batch's rows, a key column at a time: integers
 * read from their array, Arrow's bytes where they are -- a dictionary's at
 * the rows' codes, their hashes its values' (VC5) -- any other value as
 * its Datum.  The rows are those of `rows`, in the batch.
 */
static void
batch_keys(VexecAggState *s, const int *rows, int nlist)
{
	VexecBatch *in = s->node.in;
	int			nk = s->nkeys;
	const uint32 *hashes = s->batch_hashes;
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
		bool		dict = k->kind == KEY_BYTES && v->encoding == VEXEC_DICT &&
			(v->dictionary->shape.layout == VEXEC_VIEW ||
			 v->dictionary->shape.layout == VEXEC_OFFSETS);
		const uint32 *dhash = dict && hashes == NULL && !s->sorted ?
			dict_key_hashes(s, i, v, nlist) : NULL;

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
			else if (dict)
			{
				p->isnull = vexec_vec_isnull(v, row);
				if (!p->isnull)
				{
					const char *bytes;
					Size		len;

					vexec_vec_value_bytes(v->dictionary, v->codes[row], &bytes, &len);
					p->bytes = bytes;
					p->len = (int32) len;
					if (dhash != NULL)
					{
						s->rowhash[row] = h ^ dhash[v->codes[row]];
						continue;
					}
				}
			}
			else
			{
				bool		isnull;
				Datum		d = vexec_vec_datum(s->node.work, v, row, &isnull);

				probe_set(k, p, d, isnull);
			}
			/* VC4: in the keys' order, a row is compared, not hashed */
			if (!p->isnull && hashes == NULL && !s->sorted)
				h ^= key_hash(k, p);
			s->rowhash[row] = h;
		}
	}

	/* the child's hashes are this node's (C4): the keys are only compared */
	if (!s->sorted)
		for (j = 0; j < nlist; j++)
			s->rowhash[rows[j]] = hashes != NULL ? hashes[rows[j]] :
				murmurhash32(s->rowhash[rows[j]]);
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
group_create_in(VexecAggState *s, uint32 hash, TupleTableSlot *slot, int row, bool batch)
{
	MemoryContext old = MemoryContextSwitchTo(batch ? s->bgroupcxt :
											  s->sorted ? s->sgroupcxt[s->sgen] : s->groupcxt);
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
	if (batch)
	{
		/* its states in the batch table's memory, freed as it ends */
		ExprContext *saved = s->aggstate->curaggcontext;

		s->aggstate->curaggcontext = s->baggcontext;
		for (i = 0; i < s->ntrans; i++)
			vexec_aggtrans_init(&s->trans[i], s->aggstate, group_state(s, g, &s->trans[i]));
		s->aggstate->curaggcontext = saved;
		s->passed_groups++;
		s->passed_since++;
		addHyperLogLog(&s->passed_keys, murmurhash32(hash));
		return g;
	}
	for (i = 0; i < s->ntrans; i++)
		vexec_aggtrans_init(&s->trans[i], s->aggstate, group_state(s, g, &s->trans[i]));
	s->ngroups++;
	if (s->topk > 0)
		topk_note(s, hash);
	if (s->strategy == AGG_HASHED && s->sorted)
		s->total_groups++;		/* VC4: in `made` once it closes */
	else if (s->strategy == AGG_HASHED)
	{
		s->total_groups++;
		if (s->nmade == s->made_size)
		{
			s->made_size = Max(s->made_size * 2, 1024);
			s->made = s->made == NULL ?
				MemoryContextAllocHuge(s->hashcxt, sizeof(char *) * s->made_size) :
				repalloc_huge(s->made, sizeof(char *) * s->made_size);
		}
		s->made[s->nmade++] = g;
	}
	return g;
}

static char *
group_create(VexecAggState *s, uint32 hash, TupleTableSlot *slot, int row)
{
	return group_create_in(s, hash, slot, row, false);
}

/*
 * C2: the table frozen; the batch table, its memory made the first time a
 * table freezes.
 */
static void
freeze(VexecAggState *s)
{
	s->frozen = true;
	s->frozen_groups = Max(s->frozen_groups, s->ngroups);
	if (s->btable == NULL)
	{
		EState	   *estate = s->node.css.ss.ps.state;

		s->bgroupcxt = BumpContextCreate(s->node.mcxt, "VecAgg batch groups",
										 ALLOCSET_DEFAULT_SIZES);
		s->bhashcxt = AllocSetContextCreate(s->node.mcxt, "VecAgg batch table",
											ALLOCSET_DEFAULT_SIZES);
		s->baggcontext = CreateWorkExprContext(estate);
		s->btable = vagg_create(s->bhashcxt, 2 * VEXEC_BATCH_ROWS, s);
	}
	/* the keys handed on from now, counted for maybe_thaw() */
	if (!s->passed_keys_made)
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		initHyperLogLog(&s->passed_keys, 10);
		MemoryContextSwitchTo(old);
		s->passed_keys_made = true;
	}
	else
		memset(s->passed_keys.hashesArr, 0, s->passed_keys.arrSize);
	s->passed_since = 0;
	s->thaw_next = VEXEC_AGG_THAW_EVIDENCE;
}

/*
 * C2: the table thawed, where the groups handed on since it froze -- past
 * VEXEC_AGG_THAW_EVIDENCE of them -- repeat their keys
 * VEXEC_AGG_THAW_REPEAT times or more; else looked at again as many groups
 * on.
 */
static void
maybe_thaw(VexecAggState *s)
{
	s->thaw_next = s->passed_since + VEXEC_AGG_THAW_EVIDENCE;
	if ((double) s->passed_since <
		VEXEC_AGG_THAW_REPEAT * estimateHyperLogLog(&s->passed_keys))
		return;
	s->frozen = false;
	s->thawed = true;
	s->thawed_after = Max(s->thawed_after, s->passed_since);
}

/* The batch table emptied, once its groups are handed on. */
static void
batch_table_reset(VexecAggState *s)
{
	if (s->btable == NULL)
		return;
	ReScanExprContext(s->baggcontext);
	MemoryContextReset(s->bgroupcxt);
	vagg_reset(s->btable);
	s->boutput = false;
	s->biterating = false;
}

/*
 * C2: a partial aggregation under a VecRepartition may freeze its table:
 * the stage above it combines whatever states it is dealt.
 */
void
vexec_agg_allow_freeze(VexecNode *node)
{
	VexecAggState *s = (VexecAggState *) node;

	if (node->kind == VEXEC_NODE_AGG && s->strategy == AGG_HASHED &&
		DO_AGGSPLIT_SKIPFINAL(s->split) && !DO_AGGSPLIT_COMBINE(s->split) &&
		vexec_partial_agg_limit > 0 && s->topk == 0)
	{
		s->may_freeze = true;
		/* the table, still empty, made again no larger than it may grow */
		if (s->table != NULL && s->ngroups == 0)
		{
			s->buckets_initial = 0;
			table_reset(s);
		}
	}
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
	s->made = NULL;
	s->nmade = 0;
	s->made_size = 0;
	s->next_made = 0;
	s->array_decided = false;
	s->array_mode = false;
	s->slots = NULL;
	s->tfinal_set = false;
	s->spilling = false;
	s->ngroups = 0;
	s->frozen = false;
	s->thawed = false;
	batch_table_reset(s);
	if (s->strategy == AGG_HASHED)
	{
		double		fit = (double) s->memlimit / (double) (s->groupsize + sizeof(VexecAggEntry));
		double		size = Min(s->numgroups, 1048576);

		/*
		 * The planner's estimate, up to a million groups; the most a table of
		 * the node held before (C6), as far as the memory holds them, filling
		 * no more than 80% of the buckets: fuller, simplehash grows a large
		 * table early, where a key lands far from its bucket
		 * (SH_GROW_MAX_DIB).  A partition read back holds a part of the
		 * groups, no more than its rows.
		 */
		if (s->reading != NULL)
			size = Min(size, (double) s->reading->rows);
		else
			size = Max(size, (double) vexec_aggsize_get(s->size_key) * 0.9 / 0.8);
		size = Min(size, fit);
		/* a table that may freeze never holds more than it freezes at (C2) */
		if (s->may_freeze)
			size = Min(size, vexec_partial_agg_limit);

		s->table = vagg_create(s->hashcxt, (uint32) Min(Max(size, 64), 1 << 30), s);
		if (s->buckets_initial == 0)
			s->buckets_initial = s->table->size;
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
		s->batch_hashes = s->hashes_from_child ? vexec_repart_batch_hashes(child) : NULL;
		if (vexec_bits_any(child->redo, b->nrows) ||
			(child->child_redo != NULL && vexec_bits_any(child->child_redo, b->nrows)))
		{
			node->child_redo = vexec_bits_copy(node->work, child->redo, b->nrows);
			if (child->child_redo != NULL)
				vexec_bits_or_into(node->child_redo, child->child_redo, b->nrows);
		}
		return true;
	}
	s->batch_hashes = NULL;
	return fill_rows(s);
}

/* ---------------------------------------------------------------------
 * C5: the groups of the K smallest hashes (above)
 * ---------------------------------------------------------------------
 */

/*
 * The boundary now: a row or a group whose hash is past it is no one's of
 * the K groups of the smallest hashes -- K are held below it, by this
 * participant or by another, or, a final stage's, by the first stage below.
 */
static uint32
topk_bound(VexecAggState *s)
{
	uint32		b = s->tbound;

	if (s->tshared != NULL)
		b = Min(b, pg_atomic_read_u32(s->tshared));
	if (DO_AGGSPLIT_COMBINE(s->split) && s->child_batches &&
		s->node.vec_child->kind == VEXEC_NODE_REPART)
		b = Min(b, vexec_repart_topk_bound(s->node.vec_child));
	return b;
}

/* A new group's hash, into the heap of the K smallest; the boundary falls. */
static void
topk_note(VexecAggState *s, uint32 hash)
{
	int			i;

	if (s->ntheap < s->topk)
	{
		/* sift up */
		i = s->ntheap++;
		while (i > 0 && s->theap[(i - 1) / 2] < hash)
		{
			s->theap[i] = s->theap[(i - 1) / 2];
			i = (i - 1) / 2;
		}
		s->theap[i] = hash;
	}
	else if (hash < s->theap[0])
	{
		/* the largest out: sift down */
		i = 0;
		for (;;)
		{
			int			c = 2 * i + 1;

			if (c >= s->ntheap)
				break;
			if (c + 1 < s->ntheap && s->theap[c + 1] > s->theap[c])
				c++;
			if (s->theap[c] <= hash)
				break;
			s->theap[i] = s->theap[c];
			i = c;
		}
		s->theap[i] = hash;
	}
	else
		return;
	if (s->ntheap == s->topk && s->theap[0] < s->tbound)
	{
		s->tbound = s->theap[0];
		/* the participants' boundary: the least of theirs */
		if (s->tshared != NULL)
		{
			uint32		cur = pg_atomic_read_u32(s->tshared);

			while (s->tbound < cur &&
				   !pg_atomic_compare_exchange_u32(s->tshared, &cur, s->tbound))
				;
		}
	}
}

/*
 * C5: the participants' shared boundary, in the DSM of the VecRepartition
 * above this first stage, which it lends once the DSM is made.
 */
void
vexec_agg_set_topk_shared(VexecNode *node, pg_atomic_uint32 *shared)
{
	VexecAggState *s = (VexecAggState *) node;

	if (node->kind == VEXEC_NODE_AGG && s->topk > 0)
		s->tshared = shared;
}

/* C5: this stage's boundary, for the VecRepartition above it to pass on. */
uint32
vexec_agg_topk_bound(VexecNode *node)
{
	VexecAggState *s = (VexecAggState *) node;

	if (node->kind != VEXEC_NODE_AGG || s->topk == 0)
		return PG_UINT32_MAX;
	return topk_bound(s);
}

/* ---------------------------------------------------------------------
 * X5: the array mode (above)
 * ---------------------------------------------------------------------
 */

static void group_made(VexecAggState *s);

/* A key's bits as the signed integer of its width. */
static inline int64
key_int(Datum bits, int16 typlen)
{
	switch (typlen)
	{
		case 1:
			return (int8) (uint8) bits;
		case 2:
			return (int16) (uint16) bits;
		case 4:
			return (int32) (uint32) bits;
		default:
			return (int64) bits;
	}
}

/* The probe's slot in the array; false where a key is out of its range. */
static inline bool
array_slot(VexecAggState *s, const VexecAggProbe *p, int64 *slot)
{
	int64		r = 0;

	for (int k = 0; k < s->nkeys; k++)
	{
		int64		i = 0;

		if (!p[k].isnull)
		{
			int64		v = key_int(p[k].value, s->keys[k].typlen);

			if (v < s->alo[k] || v > s->ahi[k])
				return false;
			i = v - s->alo[k] + 1;
		}
		r = r * s->awidth[k] + i;
	}
	*slot = r;
	return true;
}

/*
 * Whether the empty table becomes an array, by the keys of its first batch,
 * as Velox's VectorHasher chooses (velox/exec/HashTable.cpp, decideHashMode):
 * each key's range, half its width again on either side, and a slot for
 * NULL, multiplied, no more than VEXEC_AGG_ARRAY_SLOTS -- else the range
 * alone.  The array is in the table's memory.
 */
static void
array_decide(VexecAggState *s, int nlist)
{
	int128		total = 1;
	int			k;

	s->array_decided = true;
	if (nlist == 0)
	{
		s->array_decided = false;	/* the next batch decides */
		return;
	}
	for (int pass = 0; pass < 2; pass++)
	{
		total = 1;
		for (k = 0; k < s->nkeys; k++)
		{
			int64		lo = PG_INT64_MAX;
			int64		hi = PG_INT64_MIN;
			int128		w;

			for (int j = 0; j < nlist; j++)
			{
				const VexecAggProbe *p = &s->rowkeys[s->rows[j] * s->nkeys + k];
				int64		v;

				if (p->isnull)
					continue;
				v = key_int(p->value, s->keys[k].typlen);
				lo = Min(lo, v);
				hi = Max(hi, v);
			}
			if (lo > hi)
				lo = hi = 0;	/* every value NULL */
			if (pass == 0)
			{
				/* half the width again on either side, within the type */
				int128		half = ((int128) hi - lo + 1) / 2;
				int128		min = s->keys[k].typlen == 1 ? PG_INT8_MIN :
					s->keys[k].typlen == 2 ? PG_INT16_MIN :
					s->keys[k].typlen == 4 ? PG_INT32_MIN : PG_INT64_MIN;
				int128		max = s->keys[k].typlen == 1 ? PG_INT8_MAX :
					s->keys[k].typlen == 2 ? PG_INT16_MAX :
					s->keys[k].typlen == 4 ? PG_INT32_MAX : PG_INT64_MAX;

				lo = (int64) Max((int128) lo - half, min);
				hi = (int64) Min((int128) hi + half, max);
			}
			w = (int128) hi - lo + 2;
			s->alo[k] = lo;
			s->ahi[k] = hi;
			s->awidth[k] = (int64) Min(w, (int128) PG_INT64_MAX);
			total *= w;
			if (total > VEXEC_AGG_ARRAY_SLOTS)
				break;
		}
		if (total <= VEXEC_AGG_ARRAY_SLOTS)
			break;
	}
	if (total > VEXEC_AGG_ARRAY_SLOTS)
		return;
	s->nslots = (int64) total;
	s->slots = MemoryContextAllocExtended(s->hashcxt, sizeof(char *) * s->nslots,
										  MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	s->array_mode = true;
	s->array_tables++;
	s->array_slots = Max(s->array_slots, s->nslots);
}

/*
 * The array given up for the hash table: a key out of its range, or the
 * table past its memory, about to spill.  Its groups go into the hash
 * table by the hashes they were made with, in the order they were made.
 */
static void
array_leave(VexecAggState *s)
{
	VexecAggProbe *row_probe = s->probe;
	uint32		row_hash = *(uint32 *) s->probe_group;

	/* each group's keys its probe, which the table compares an entry with */
	s->probe = s->probe_slot;
	for (int64 i = 0; i < s->nmade; i++)
	{
		char	   *g = s->made[i];
		uint32		hash = *(uint32 *) g;
		bool		found;
		VexecAggEntry *e;

		for (int k = 0; k < s->nkeys; k++)
		{
			s->probe[k].isnull = group_keynulls(s, g)[k];
			s->probe[k].value = s->probe[k].isnull ? (Datum) 0 :
				key_bits(group_keys(s, g)[k], s->keys[k].typlen);
		}
		*(uint32 *) s->probe_group = hash;
		e = vagg_insert_hash(s->table, s->probe_group, hash, &found);
		Assert(!found);
		e->group = g;
	}
	s->probe = row_probe;
	*(uint32 *) s->probe_group = row_hash;
	pfree(s->slots);
	s->slots = NULL;
	s->array_mode = false;
	s->array_left++;
}

/* ---------------------------------------------------------------------
 * VC4: groups in their keys' order (above)
 * ---------------------------------------------------------------------
 */

/* A closed group, to hand up */
static void
sorted_close(VexecAggState *s, char *g)
{
	if (s->nmade == s->made_size)
	{
		s->made_size = Max(s->made_size * 2, VEXEC_BATCH_ROWS + 1);
		s->made = s->made == NULL ?
			MemoryContextAllocHuge(s->node.mcxt, sizeof(char *) * s->made_size) :
			repalloc_huge(s->made, sizeof(char *) * s->made_size);
	}
	s->made[s->nmade++] = g;
}

/*
 * A row's group in the keys' order: the open one where the row's keys are
 * its, else a new one, the open one closed.  A row of the open group made
 * in the older generation is marked in bmask: its states are in that
 * generation's memory (agg_batch()).
 */
static char *
sorted_row_group(VexecAggState *s, TupleTableSlot *slot, int row)
{
	if (s->cur != NULL && group_equal(s, s->cur))
	{
		if (s->cur_gen != s->sgen && row >= 0)
			vexec_bit_set(s->bmask, row);
		return s->cur;
	}
	if (s->cur != NULL)
		sorted_close(s, s->cur);
	/* its hash once, for a stage above that reads it (C4) */
	s->cur = group_create(s, probe_hash(s), slot, row);
	s->cur_gen = s->sgen;
	return s->cur;
}

/*
 * The groups closed so far are out: every generation but the open group's
 * emptied, and new groups made in a freshly emptied one.
 */
static void
sorted_round_end(VexecAggState *s)
{
	int			keep = s->cur != NULL ? s->cur_gen : -1;
	Size		mem = 0;

	for (int g = 0; g < 2; g++)
		mem += MemoryContextMemAllocated(s->sgroupcxt[g], true) +
			MemoryContextMemAllocated(s->saggcontext[g]->ecxt_per_tuple_memory, true);
	s->peak_memory = Max(s->peak_memory, mem);
	for (int g = 0; g < 2; g++)
		if (g != keep)
		{
			ReScanExprContext(s->saggcontext[g]);	/* AggRegisterCallback()'s run */
			MemoryContextReset(s->sgroupcxt[g]);
		}
	s->sgen = keep >= 0 ? 1 - keep : 0;
	s->nmade = 0;
	s->next_made = 0;
}

/* Both generations emptied, no group open: the node's first, or a rescan's */
static void
sorted_reset(VexecAggState *s)
{
	s->cur = NULL;
	(void) sorted_round_end(s);
	s->sorted_eof = false;
	s->ngroups = 0;
}

/*
 * Whether the input comes in the keys' order: the child a serial VecScan
 * whose source says, as it begins, that its rows come in an order the
 * keys -- Vars of its relation, compared by their types' default equality,
 * a deterministic collation's for text -- are a prefix of, in any order.
 */
static bool
sorted_input_holds(VexecAggState *s)
{
	VexecNode  *child = s->node.vec_child;
	Plan	   *cplan = s->child->plan;
	AttrNumber	order[INDEX_MAX_KEYS];
	bool		used[INDEX_MAX_KEYS] = {false};
	int			k;

	if (!s->child_batches || child == NULL || child->kind != VEXEC_NODE_SCAN ||
		s->nkeys == 0 || s->nkeys > INDEX_MAX_KEYS)
		return false;
	k = vexec_scan_ordering(child, INDEX_MAX_KEYS, order);
	if (k < s->nkeys)
		return false;
	for (int i = 0; i < s->nkeys; i++)
	{
		VexecAggKeyCol *key = &s->keys[i];
		TargetEntry *tle;
		Var		   *v;
		int			j;

		if (key->col >= list_length(cplan->targetlist))
			return false;
		tle = list_nth_node(TargetEntry, cplan->targetlist, key->col);
		v = (Var *) tle->expr;
		if (!IsA(v, Var) || v->varno != ((Scan *) cplan)->scanrelid || v->varlevelsup != 0 ||
			v->varattno < 1)
			return false;
		if (key->eqop != lookup_type_cache(v->vartype, TYPECACHE_EQ_OPR)->eq_opr ||
			(OidIsValid(key->collation) && !get_collation_isdeterministic(key->collation)))
			return false;
		for (j = 0; j < s->nkeys; j++)
			if (order[j] == v->varattno && !used[j])
				break;
		if (j == s->nkeys)
			return false;
		used[j] = true;
	}
	return true;
}

/*
 * At the first group asked for: the input in the keys' order, where the
 * plan said it may be and nothing else asked of the node hashes; else the
 * table, as any hashed aggregation's.
 */
static void
sorted_decide(VexecAggState *s)
{
	EState	   *estate = s->node.css.ss.ps.state;

	s->sorted_decided = true;
	s->sorted = s->sorted_planned && vexec_enable_sorted_agg && s->topn == 0 && s->topk == 0 &&
		s->ndeal == 0 && !s->may_freeze && !s->hashes_from_child && sorted_input_holds(s);
	if (!s->sorted)
	{
		table_reset(s);
		return;
	}
	s->array_ok = false;
	for (int g = 0; g < 2; g++)
	{
		s->sgroupcxt[g] = BumpContextCreate(s->node.mcxt, "VecAgg groups in order",
											ALLOCSET_DEFAULT_SIZES);
		s->saggcontext[g] = CreateWorkExprContext(estate);
	}
	sorted_reset(s);
}

/*
 * Input batches until one closes a group, or the input ends: the open
 * group's generation the states' memory of its rows, the newer one the
 * others' (agg_batch()).
 */
static void
sorted_fill(VexecAggState *s)
{
	for (;;)
	{
		MemoryContext old;

		if (s->nmade > 0)
			return;
		if (!vexec_node_next_input(&s->node))
		{
			s->sorted_eof = true;
			return;
		}
		old = MemoryContextSwitchTo(s->tmpcontext->ecxt_per_tuple_memory);
		s->aggstate->curaggcontext = s->saggcontext[s->sgen];
		agg_batch(s);
		s->aggstate->curaggcontext = s->aggcontext;
		MemoryContextSwitchTo(old);
	}
}

/*
 * The next group in the keys' order, or NULL once every one is out: those
 * the last batch closed, the memory of those handed up before emptied first
 * -- their rows are their parent's until this call -- and the open one
 * last.
 */
static char *
sorted_next_group(VexecAggState *s)
{
	for (;;)
	{
		if (s->next_made < s->nmade)
			return s->made[s->next_made++];
		if (s->nmade > 0)
			sorted_round_end(s);
		if (s->sorted_eof)
		{
			if (s->cur != NULL)
			{
				sorted_close(s, s->cur);
				s->cur = NULL;
				continue;
			}
			s->done = true;
			return NULL;
		}
		sorted_fill(s);
	}
}

/*
 * The states' memory of the rows bmask marks: the batch table's (C2), made
 * as the table freezes, which it may do within the batch; the open group's
 * older generation (VC4)
 */
static inline ExprContext *
bmask_context(VexecAggState *s)
{
	return s->sorted ? s->saggcontext[1 - s->sgen] : s->baggcontext;
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
	int64		si;

	if (s->strategy == AGG_PLAIN)
		return s->plaingroup;
	if (s->sorted)
		return sorted_row_group(s, slot, row);
	*(uint32 *) s->probe_group = hash;
	if (s->spilling)
	{
		e = vagg_lookup_hash(s->table, s->probe_group, hash);
		return e ? e->group : NULL;
	}
	if (s->frozen)
	{
		/* C2: a key the table holds updates it; the batch table takes the others */
		if (s->array_mode)
		{
			if (array_slot(s, s->probe, &si) && s->slots[si] != NULL)
				return s->slots[si];
		}
		else
		{
			e = vagg_lookup_hash(s->table, s->probe_group, hash);
			if (e != NULL)
				return e->group;
		}
		e = vagg_insert_hash(s->btable, s->probe_group, hash, &found);
		if (!found)
			e->group = group_create_in(s, hash, slot, row, true);
		vexec_bit_set(s->bmask, row);
		return e->group;
	}
	if (s->array_mode)
	{
		/* X5: the key's slot, where it is in range; else the hash table */
		if (array_slot(s, s->probe, &si))
		{
			char	   *g = s->slots[si];

			if (g == NULL)
			{
				g = s->slots[si] = group_create(s, hash, slot, row);
				group_made(s);
			}
			return g;
		}
		array_leave(s);
	}
	e = vagg_insert_hash(s->table, s->probe_group, hash, &found);
	if (!found)
	{
		e->group = group_create(s, hash, slot, row);
		group_made(s);
	}
	return e->group;
}

/*
 * A group made: past the memory the table may hold, the table spills --
 * from the hash table -- and no group is made; or a first stage under a
 * VecRepartition freezes it (C2).
 */
static void
group_made(VexecAggState *s)
{
	Size		mem = table_memory(s);

	if (s->depth < VEXEC_AGG_MAX_DEPTH - 1 && mem > s->memlimit)
	{
		if (s->array_mode)
			array_leave(s);
		spill_begin(s);
	}
	else if (s->may_freeze && !s->thawed && s->depth == 0 &&
			 (s->ngroups >= vexec_partial_agg_limit ||
			  MemoryContextMemAllocated(s->groupcxt, true) +
			  MemoryContextMemAllocated(s->aggcontext->ecxt_per_tuple_memory, true) >=
			  VEXEC_AGG_FREEZE_BYTES))
		freeze(s);
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

/* The vectorized transitions marked batched, over the rows of `rows`. */
static void
batch_transitions(VexecAggState *s, const uint64 *rows, int n)
{
	VexecNode  *node = &s->node;

	if (!vexec_bits_any(rows, n))
		return;
	for (int i = 0; i < s->ntrans; i++)
	{
		VexecAggTrans *t = &s->trans[i];

		if (!s->batched[i])
			continue;
		vexec_aggtrans_batch(t, s->aggstate, node->work,
							 s->strategy == AGG_PLAIN ? NULL : s->groups,
							 s->stateoff + t->stateoff,
							 s->strategy == AGG_PLAIN ? group_state(s, s->plaingroup, t) : NULL,
							 rows, n,
							 t->nargs > 0 ? node->outputs[t->firstarg] : NULL,
							 t->filter >= 0 ? node->outputs[t->filter] : NULL);
	}
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
	ExprContext *mainctx = s->aggstate->curaggcontext;
	int			nlist;
	int			row;
	int			i,
				j;

	/*
	 * C2: the rows whose group is the batch table's, once the table froze;
	 * VC4: those of the open group of the older generation
	 */
	s->bmask = s->may_freeze || s->sorted ? vexec_bitmap_alloc(node->work, n, false) : NULL;

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

		vexec_bits_or_into(all, node->redo, n);
		if (node->child_redo != NULL)
			vexec_bits_or_into(all, node->child_redo, n);
		nlist = 0;
		VEXEC_FOREACH_ROW(all, n, r)
			s->rows[nlist++] = r;
	}
	if (s->strategy == AGG_HASHED)
	{
		batch_keys(s, s->rows, nlist);
		/*
		 * C5: a row whose key's hash is past the boundary is passed over:
		 * K groups of smaller hashes are held, so its group is no one's of
		 * those the LIMIT takes.  Not a row PostgreSQL's evaluator decides.
		 */
		if (s->topk_skip)
		{
			uint32		b = topk_bound(s);

			if (b != PG_UINT32_MAX)
			{
				int			kept = 0;

				for (j = 0; j < nlist; j++)
				{
					int			r = s->rows[j];

					if (s->rowhash[r] > b && !vexec_bit(node->redo, r) &&
						!(node->child_redo != NULL && vexec_bit(node->child_redo, r)))
						continue;
					s->rows[kept++] = r;
				}
				s->rows_skipped += nlist - kept;
				nlist = kept;
			}
		}
		/* X5: the table an array, by its first batch's keys */
		if (s->array_ok && !s->array_decided && s->ngroups == 0 && !s->frozen &&
			!s->spilling && s->reading == NULL && s->table != NULL)
			array_decide(s, nlist);
	}

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
		/* a batch table's group keeps its states in that table's memory (C2) */
		if (s->bmask != NULL && vexec_bit(s->bmask, row))
			s->aggstate->curaggcontext = bmask_context(s);
		for (i = 0; i < s->ntrans; i++)
		{
			VexecAggTrans *t = &s->trans[i];

			if (!pg && s->batched[i])
				continue;
			if (row_inputs(s, t, row, slot, pg))
				vexec_aggtrans_advance(t, s->aggstate, group_state(s, g, t),
									   s->argvals, s->argnulls);
		}
		s->aggstate->curaggcontext = mainctx;
	}

batched:
	if (!any_batched || !vexec_bits_any(taken, n))
		return;
	s->rows_batched += vexec_bits_count(taken, n);
	if (s->bmask != NULL && vexec_bits_any(s->bmask, n))
	{
		/* the table's groups, then the batch table's in its memory (C2) */
		batch_transitions(s, vexec_bits_andnot(node->work, taken, s->bmask, n), n);
		s->aggstate->curaggcontext = bmask_context(s);
		batch_transitions(s, vexec_bits_and(node->work, taken, s->bmask, n), n);
		s->aggstate->curaggcontext = mainctx;
	}
	else
		batch_transitions(s, taken, n);
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

		/*
		 * C2: the batch table's groups go on before the next batch; the
		 * table thaws where they repeat their keys
		 */
		if (s->frozen && s->btable->members > 0)
		{
			if (s->passed_since >= s->thaw_next)
				maybe_thaw(s);
			s->boutput = true;
			s->biterating = false;
			return;
		}
	}
	if (s->reading != NULL)
	{
		BufFileClose(s->reading->file);
		s->reading->file = NULL;
		s->reading = NULL;
	}
	spill_finish(s);
	s->filled = true;

	/* C6: the groups the table held, for the next run to begin with */
	if (s->strategy == AGG_HASHED)
	{
		vexec_aggsize_put(s->size_key, s->ngroups);
		s->buckets_final = Max(s->buckets_final, s->table->size);
	}
}

/* ---------------------------------------------------------------------
 * Output
 * ---------------------------------------------------------------------
 */

/* A group's column i of the scan tuple into the slot. */
static inline void
group_column(VexecAggState *s, char *g, int i, TupleTableSlot *slot)
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

/*
 * A group's columns into the scan tuple: those C3's ranking reads, the
 * others NULL, where `needed` is given; else every one.
 */
static TupleTableSlot *
group_output_cols(VexecAggState *s, char *g, const bool *needed)
{
	TupleTableSlot *slot = s->node.css.ss.ss_ScanTupleSlot;
	int			i;

	ExecClearTuple(slot);
	if (needed != NULL)
	{
		for (i = 0; i < s->noutcols; i++)
			slot->tts_isnull[i] = true;
		for (int j = 0; j < s->ntopcols; j++)
			group_column(s, g, s->topcols[j], slot);
		return ExecStoreVirtualTuple(slot);
	}
	for (i = 0; i < s->noutcols; i++)
		group_column(s, g, i, slot);
	return ExecStoreVirtualTuple(slot);
}

static TupleTableSlot *
group_output(VexecAggState *s, char *g)
{
	return group_output_cols(s, g, NULL);
}

/* ---------------------------------------------------------------------
 * C3: the top n kept inside (above)
 * ---------------------------------------------------------------------
 */

static char *next_group(VexecAggState *s);

typedef struct TopEntry
{
	MinimalTuple tup;			/* the projected row */
	Datum	   *keys;
	bool	   *nulls;
} TopEntry;

/* The order of two rows' keys, the sort's: < 0 where a comes first. */
static int
top_compare(VexecAggState *s, const Datum *ak, const bool *an, const Datum *bk, const bool *bn)
{
	for (int k = 0; k < s->ntopkeys; k++)
	{
		int			c = ApplySortComparator(ak[k], an[k], bk[k], bn[k], &s->topssup[k]);

		if (c != 0)
			return c;
	}
	return 0;
}

/* The heap's order: the row that comes last at the root. */
static void
top_sift_down(VexecAggState *s, int i)
{
	TopEntry   *h = s->top;

	for (;;)
	{
		int			l = 2 * i + 1;
		int			r = l + 1;
		int			m = i;

		if (l < s->ntop && top_compare(s, h[l].keys, h[l].nulls, h[m].keys, h[m].nulls) > 0)
			m = l;
		if (r < s->ntop && top_compare(s, h[r].keys, h[r].nulls, h[m].keys, h[m].nulls) > 0)
			m = r;
		if (m == i)
			return;
		{
			TopEntry	t = h[i];

			h[i] = h[m];
			h[m] = t;
		}
		i = m;
	}
}

static void
top_sift_up(VexecAggState *s, int i)
{
	TopEntry   *h = s->top;

	while (i > 0)
	{
		int			p = (i - 1) / 2;

		if (top_compare(s, h[i].keys, h[i].nulls, h[p].keys, h[p].nulls) <= 0)
			return;
		{
			TopEntry	t = h[i];

			h[i] = h[p];
			h[p] = t;
		}
		i = p;
	}
}

/*
 * A group ranked: HAVING first, over the columns it and the keys read; then
 * the keys; kept where the heap has room or it comes before the heap's
 * last, its whole row projected then.
 */
static void
top_consider(VexecAggState *s, char *g)
{
	CustomScanState *css = &s->node.css;
	ExprContext *econtext = css->ss.ps.ps_ExprContext;
	Datum	   *kv = s->topkv;
	bool	   *kn = s->topkn;
	TupleTableSlot *out;
	MemoryContext old;
	TopEntry   *e;

	ResetExprContext(econtext);
	s->top_ranked++;
	econtext->ecxt_scantuple = group_output_cols(s, g, s->topneeded);
	if (css->ss.ps.qual != NULL && !ExecQual(css->ss.ps.qual, econtext))
	{
		InstrCountFiltered1(css, 1);
		return;
	}
	for (int k = 0; k < s->ntopkeys; k++)
	{
		int			col = s->topkeycol[k];

		if (col >= 0)
		{
			kv[k] = econtext->ecxt_scantuple->tts_values[col];
			kn[k] = econtext->ecxt_scantuple->tts_isnull[col];
		}
		else
			kv[k] = ExecEvalExpr(s->topkey[k], econtext, &kn[k]);
	}
	if (s->ntop == s->topn &&
		top_compare(s, kv, kn, s->top[0].keys, s->top[0].nulls) >= 0)
		return;

	/* kept: the whole row, finished and projected, in the heap's memory */
	econtext->ecxt_scantuple = group_output(s, g);
	out = css->ss.ps.ps_ProjInfo != NULL ? ExecProject(css->ss.ps.ps_ProjInfo) :
		ExecCopySlot(css->ss.ps.ps_ResultTupleSlot, econtext->ecxt_scantuple);
	if (s->ntop == s->topn)
	{
		e = &s->top[0];
		pfree(e->tup);
		for (int k = 0; k < s->ntopkeys; k++)
			if (!s->topkeybyval[k] && !e->nulls[k])
				pfree(DatumGetPointer(e->keys[k]));
	}
	else
		e = &s->top[s->ntop++];
	old = MemoryContextSwitchTo(s->topcxt);
	e->tup = ExecCopySlotMinimalTuple(out);
	for (int k = 0; k < s->ntopkeys; k++)
	{
		e->nulls[k] = kn[k];
		e->keys[k] = kn[k] ? (Datum) 0 :
			datumCopy(kv[k], s->topkeybyval[k], s->topkeylen[k]);
	}
	MemoryContextSwitchTo(old);
	if (e == &s->top[0] && s->ntop == s->topn)
		top_sift_down(s, 0);
	else
		top_sift_up(s, s->ntop - 1);
}

/* The Var attnos of the scan tuple an expression reads. */
static bool
scan_cols_walker(Node *node, bool *needed)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var) && ((Var *) node)->varno == INDEX_VAR && ((Var *) node)->varattno > 0)
	{
		needed[((Var *) node)->varattno - 1] = true;
		return false;
	}
	return expression_tree_walker(node, scan_cols_walker, needed);
}

/*
 * C3: a bounded VecSort above lends its keys -- columns of the node's
 * output, with their ordering operators, collations and NULLS FIRST flags
 * -- and its bound: the node keeps its n best groups (above).  False where
 * it does not take them: no hashed final aggregation, or an output or
 * HAVING that a volatile function or a subplan would run for fewer groups.
 */
bool
vexec_agg_set_topn(VexecNode *node, int64 bound, List *keycols, List *sortops,
				   List *collations, List *nullsfirst)
{
	VexecAggState *s = (VexecAggState *) node;
	Plan	   *plan = node->css.ss.ps.plan;
	ListCell   *lc;

	if (node->kind != VEXEC_NODE_AGG || s->strategy != AGG_HASHED ||
		DO_AGGSPLIT_SKIPFINAL(s->split) || bound <= 0 || bound > VEXEC_BATCH_ROWS * 64 ||
		contain_volatile_functions((Node *) plan->targetlist) ||
		contain_volatile_functions((Node *) plan->qual) ||
		contain_subplans((Node *) plan->targetlist) || contain_subplans((Node *) plan->qual))
		return false;
	s->topn = bound;
	s->ntopkeys = list_length(keycols);
	s->topkey = palloc(sizeof(ExprState *) * s->ntopkeys);
	s->topssup = palloc0(sizeof(SortSupportData) * s->ntopkeys);
	s->topkeylen = palloc(sizeof(int16) * s->ntopkeys);
	s->topkeybyval = palloc(sizeof(bool) * s->ntopkeys);
	s->topneeded = palloc0(sizeof(bool) * Max(s->noutcols, 1));
	s->topkeycol = palloc(sizeof(int) * s->ntopkeys);
	foreach(lc, keycols)
	{
		int			k = foreach_current_index(lc);
		TargetEntry *tle = list_nth_node(TargetEntry, plan->targetlist, lfirst_int(lc) - 1);
		SortSupport ssup = &s->topssup[k];

		s->topkey[k] = ExecInitExpr(tle->expr, &node->css.ss.ps);
		s->topkeycol[k] = IsA(tle->expr, Var) && ((Var *) tle->expr)->varno == INDEX_VAR &&
			((Var *) tle->expr)->varattno > 0 ? ((Var *) tle->expr)->varattno - 1 : -1;
		get_typlenbyval(exprType((Node *) tle->expr), &s->topkeylen[k], &s->topkeybyval[k]);
		scan_cols_walker((Node *) tle->expr, s->topneeded);
		ssup->ssup_cxt = node->mcxt;
		ssup->ssup_collation = list_nth_oid(collations, k);
		ssup->ssup_nulls_first = list_nth_int(nullsfirst, k) != 0;
		ssup->ssup_attno = k + 1;
		ssup->abbreviate = false;
		PrepareSortSupportFromOrderingOp(list_nth_oid(sortops, k), ssup);
	}
	scan_cols_walker((Node *) plan->qual, s->topneeded);
	s->topcols = palloc(sizeof(int) * Max(s->noutcols, 1));
	for (int i = 0; i < s->noutcols; i++)
		if (s->topneeded[i])
			s->topcols[s->ntopcols++] = i;
	s->topcxt = AllocSetContextCreate(node->mcxt, "VecAgg top n", ALLOCSET_DEFAULT_SIZES);
	s->top = MemoryContextAllocZero(node->mcxt, sizeof(TopEntry) * bound);
	for (int i = 0; i < bound; i++)
	{
		s->top[i].keys = MemoryContextAlloc(node->mcxt, sizeof(Datum) * s->ntopkeys);
		s->top[i].nulls = MemoryContextAlloc(node->mcxt, sizeof(bool) * s->ntopkeys);
	}
	s->topkv = palloc(sizeof(Datum) * s->ntopkeys);
	s->topkn = palloc(sizeof(bool) * s->ntopkeys);
	s->topslot = ExecInitExtraTupleSlot(node->css.ss.ps.state,
										node->css.ss.ps.ps_ResultTupleDesc,
										&TTSOpsMinimalTuple);
	return true;
}

/*
 * C3's output: every group ranked -- the table's, then each spilled
 * partition's -- and the kept rows handed up, in the heap's order, for the
 * sort above to order.
 */
static TupleTableSlot *
top_exec(VexecAggState *s)
{
	CustomScanState *css = &s->node.css;

	if (!s->top_filled)
	{
		for (;;)
		{
			char	   *g;

			if (!s->filled)
				agg_fill(s);
			while ((g = next_group(s)) != NULL)
				top_consider(s, g);
			if (s->pending == NIL)
				break;
			/* the table is out: the next partition */
			s->reading = linitial(s->pending);
			s->pending = list_delete_first(s->pending);
			s->depth = s->reading->depth;
			table_reset(s);
			vexec_node_rescan(&s->node);
			s->filled = false;
		}
		s->top_filled = true;
		s->topnext = 0;
	}
	if (s->topnext >= s->ntop)
		return ExecClearTuple(css->ss.ps.ps_ResultTupleSlot);
	return ExecStoreMinimalTuple(s->top[s->topnext++].tup, s->topslot, false);
}

/*
 * The next group of the table, or NULL once every one is out: in the order
 * they were made (X9, above), the ones a few places on fetched ahead.
 */
static char *
next_group(VexecAggState *s)
{
	char	   *g;

	if (s->strategy == AGG_PLAIN)
	{
		g = s->plaingroup;
		s->plaingroup = NULL;
		return g;
	}
	if (s->next_made >= s->nmade)
		return NULL;
	if (s->next_made + 8 < s->nmade)
	{
		char	   *ahead = s->made[s->next_made + 8];

		__builtin_prefetch(ahead);
		__builtin_prefetch(ahead + s->stateoff);
	}
	return s->made[s->next_made++];
}

/* C2: the batch table's next group, or NULL once every one is out. */
static char *
next_bgroup(VexecAggState *s)
{
	VexecAggEntry *e;

	if (!s->biterating)
	{
		vagg_start_iterate(s->btable, &s->biter);
		s->biterating = true;
	}
	e = vagg_iterate(s->btable, &s->biter);
	return e != NULL ? e->group : NULL;
}

/*
 * The next group to hand up, or NULL once every one is out: a batch
 * table's groups as each batch ends (C2), the table's once the input is
 * read, then each partition's.
 */
static char *
next_out_group(VexecAggState *s)
{
	if (!s->sorted_decided)
		sorted_decide(s);
	for (;;)
	{
		char	   *g;

		if (s->done)
			return NULL;
		if (s->sorted)
			return sorted_next_group(s);
		if (s->boutput)
		{
			/*
			 * C2: the groups of the batch just aggregated, handed on; the
			 * table emptied once the last is out, the row handed up before
			 * it consumed
			 */
			g = next_bgroup(s);
			if (g == NULL)
			{
				batch_table_reset(s);
				continue;
			}
			return g;
		}
		if (!s->filled)
		{
			agg_fill(s);
			if (s->boutput)
				continue;
		}

		g = next_group(s);
		/*
		 * C5: a group past the boundary may lack rows passed over; one
		 * within it lacks none.  A first stage hands every group on, for
		 * the final stage to judge.
		 */
		if (g != NULL && s->topk > 0 && !DO_AGGSPLIT_SKIPFINAL(s->split))
		{
			/*
			 * the boundary once the table is filled: a later partition's
			 * rows may lower it, past groups of this table that lack none
			 */
			if (!s->tfinal_set)
			{
				s->tfinal = topk_bound(s);
				s->tfinal_set = true;
			}
			if (*(uint32 *) g > s->tfinal)
				continue;
		}
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
		return g;
	}
}

/*
 * X9: the next batch of the groups dealt into the partitions: the one a
 * group fills, or, once every group is out, each partition's rest.  The
 * batch handed up before is emptied first: its parent is done with it.
 */
static VexecBatch *
next_dealt_batch(VexecAggState *s)
{
	ExprContext *econtext = s->node.css.ss.ps.ps_ExprContext;
	int			ncols = s->noutcols;

	if (s->dealt_out >= 0)
		vexec_batch_reset(s->dealt[s->dealt_out]);
	s->dealt_out = -1;
	for (;;)
	{
		char	   *g;
		uint32		hash;
		int			p;
		VexecBatch *b;
		TupleTableSlot *slot;

		ResetExprContext(econtext);
		g = next_out_group(s);
		if (g == NULL)
		{
			while (s->deal_rest < s->ndeal)
			{
				p = s->deal_rest++;
				if (s->dealt[p] != NULL && s->dealt[p]->nrows > 0)
				{
					s->dealt_out = p;
					return s->dealt[p];
				}
			}
			return NULL;
		}
		hash = *(uint32 *) g;
		p = s->deal(s->deal_arg, hash);
		b = s->dealt[p];
		if (b == NULL)
			b = s->dealt[p] = vexec_batch_create(s->node.mcxt, ncols + 1, s->dealtypes);
		if (b->nrows == 0 && !b->filling)
			vexec_batch_begin_rows(b);
		slot = group_output(s, g);
		memcpy(s->dealvals, slot->tts_values, sizeof(Datum) * ncols);
		memcpy(s->dealnulls, slot->tts_isnull, sizeof(bool) * ncols);
		s->dealvals[ncols] = Int32GetDatum((int32) hash);
		s->dealnulls[ncols] = false;
		vexec_batch_add_values(b, s->dealvals, s->dealnulls);
		if (b->nrows == VEXEC_BATCH_ROWS)
		{
			s->dealt_out = p;
			return b;
		}
	}
}

/*
 * X9: the groups to be dealt into a VecRepartition's npartitions
 * partitions by deal(arg, hash) as they go out: each batch the node hands
 * up from now on holds one partition's groups -- the scan tuple's columns,
 * in their build shapes, and an int4 of each group's hash -- whose number
 * vexec_agg_dealt_partition() then gives.  Before the node's first batch.
 */
void
vexec_agg_deal(VexecNode *node, int npartitions, int (*deal) (void *arg, uint32 hash),
			   void *arg)
{
	VexecAggState *s = (VexecAggState *) node;
	MemoryContext old = MemoryContextSwitchTo(node->mcxt);

	Assert(node->next_out != NULL && s->topn == 0);
	if (s->dealtypes == NULL)
	{
		s->dealtypes = palloc(sizeof(VexecType *) * (s->noutcols + 1));
		memcpy(s->dealtypes, s->outb->types, sizeof(VexecType *) * s->noutcols);
		s->dealtypes[s->noutcols] = vexec_type_make(INT4OID, -1, InvalidOid);
		s->dealvals = palloc(sizeof(Datum) * (s->noutcols + 1));
		s->dealnulls = palloc(sizeof(bool) * (s->noutcols + 1));
	}
	if (s->dealt_size != npartitions)
	{
		s->dealt = palloc0(sizeof(VexecBatch *) * npartitions);
		s->dealt_size = npartitions;
	}
	for (int p = 0; p < npartitions; p++)
		if (s->dealt[p] != NULL)
			vexec_batch_reset(s->dealt[p]);
	s->ndeal = npartitions;
	s->deal = deal;
	s->deal_arg = arg;
	s->dealt_out = -1;
	s->deal_rest = 0;
	MemoryContextSwitchTo(old);
}

/* X9: the partition whose groups the batch handed up last holds. */
int
vexec_agg_dealt_partition(VexecNode *node)
{
	return ((VexecAggState *) node)->dealt_out;
}

/*
 * X9: the next batch of groups, for a vector parent -- the node's output
 * where it is the scan tuple as it is, with no HAVING (agg_begin()) -- and
 * each row's group's hash (vexec_agg_batch_hashes()), a batch of a
 * group a row, as Velox's aggregation extracts its output's columns from
 * its groups' rows (RowContainer::extractColumn(), velox/exec/RowContainer.h).
 * The values are copied into the batch, so a group's memory may go before
 * the batch does: a batch table's goes as its last group is out (C2).  The
 * columns keep their build shapes, which every kernel reads, and a frame
 * takes as they are.
 */
static VexecBatch *
agg_next_batch(VexecNode *node)
{
	VexecAggState *s = (VexecAggState *) node;
	VexecBatch *out = s->outb;
	TupleTableSlot *slot;

	if (s->ndeal > 0)
		return next_dealt_batch(s);
	vexec_batch_reset(out);
	vexec_batch_begin_rows(out);
	/* the results' memory, of the batch before: copied into it */
	ResetExprContext(node->css.ss.ps.ps_ExprContext);
	while (out->nrows < VEXEC_BATCH_ROWS)
	{
		if (s->topn > 0)
		{
			/* C3's n best, as rows */
			slot = top_exec(s);
			if (TupIsNull(slot))
				break;
			slot_getallattrs(slot);
			s->outhash[out->nrows] = 0;
		}
		else
		{
			char	   *g = next_out_group(s);

			if (g == NULL)
				break;
			s->outhash[out->nrows] = *(uint32 *) g;
			slot = group_output(s, g);
		}
		vexec_batch_add_values(out, slot->tts_values, slot->tts_isnull);
	}
	if (out->nrows == 0)
		return NULL;
	return out;
}

static TupleTableSlot *
agg_exec(CustomScanState *css)
{
	VexecAggState *s = (VexecAggState *) css;
	ExprContext *econtext = css->ss.ps.ps_ExprContext;

	if (s->topn > 0)
		return top_exec(s);
	for (;;)
	{
		char	   *g;

		ResetExprContext(econtext);
		g = next_out_group(s);
		if (g == NULL)
			return ExecClearTuple(css->ss.ps.ps_ResultTupleSlot);
		s->last_hash = *(uint32 *) g;
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

/* A grouping column, set up: how it is hashed and compared. */
static void
key_setup(VexecAggKeyCol *k, TupleDesc desc, int col, Oid eqop, Oid collation)
{
	Oid			lhash;
	Oid			rhash;
	Form_pg_attribute att;

	k->col = col;
	if (k->col < 0 || k->col >= desc->natts)
		elog(ERROR, "vexec: grouping column %d of %d", k->col + 1, desc->natts);
	att = TupleDescAttr(desc, k->col);
	k->eqop = eqop;
	k->collation = collation;
	k->typlen = att->attlen;
	k->typbyval = att->attbyval;
	k->kind = key_kind(eqop, k->collation, att);
	if (!get_op_hash_functions(eqop, &lhash, &rhash))
		elog(ERROR, "vexec: no hash function for the grouping operator %u", eqop);
	fmgr_info(lhash, &k->hash);
	fmgr_info(get_opcode(eqop), &k->eq);
}

/*
 * C4 (§3.19): VecAgg's hash of a row's keys, for VecRepartition, which deals
 * rows out by it: so that the VecAgg above takes each row's hash from it,
 * and the partial VecAgg below hands its groups' on, and no node hashes a
 * key twice.  The hash is the one a VecAgg grouping by the same keys --
 * the same columns, equality operators and collations, in the same order --
 * computes for them.
 */
struct VexecKeyHash
{
	int			nkeys;
	VexecAggKeyCol *keys;
	VexecAggProbe *probe;
};

VexecKeyHash *
vexec_key_hash_create(TupleDesc desc, List *keycols, List *eqops, List *collations)
{
	VexecKeyHash *kh = palloc0(sizeof(VexecKeyHash));

	kh->nkeys = list_length(keycols);
	kh->keys = palloc0_array(VexecAggKeyCol, Max(kh->nkeys, 1));
	kh->probe = palloc0_array(VexecAggProbe, Max(kh->nkeys, 1));
	for (int i = 0; i < kh->nkeys; i++)
		key_setup(&kh->keys[i], desc, list_nth_int(keycols, i) - 1,
				  list_nth_oid(eqops, i), list_nth_oid(collations, i));
	return kh;
}

/*
 * The hash of a row's keys; the slot's columns deformed.  A key's
 * detoasting allocates in the current memory context.
 */
uint32
vexec_key_hash_slot(VexecKeyHash *kh, TupleTableSlot *slot)
{
	for (int i = 0; i < kh->nkeys; i++)
	{
		int			col = kh->keys[i].col;

		probe_set(&kh->keys[i], &kh->probe[i], slot->tts_values[col], slot->tts_isnull[col]);
	}
	return probes_hash(kh->keys, kh->probe, kh->nkeys);
}

/*
 * Whether a VecAgg's output rows carry their groups' hashes for these keys
 * -- columns of its output from 1, with these equality operators and
 * collations: its own keys, in their order, each a column of its scan tuple
 * as it is.  vexec_agg_last_hash() then gives the hash of the row it last
 * returned, which is vexec_key_hash_slot()'s for that row.
 */
bool
vexec_agg_hashes_keys(VexecNode *node, List *keycols, List *eqops, List *collations)
{
	VexecAggState *s = (VexecAggState *) node;
	List	   *tlist = node->css.ss.ps.plan->targetlist;

	if (node->kind != VEXEC_NODE_AGG || s->strategy != AGG_HASHED ||
		list_length(keycols) != s->nkeys)
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
		if (v->varno != INDEX_VAR || v->varattno < 1 || v->varattno > s->noutcols ||
			s->outkind[v->varattno - 1] != VEXEC_AGGCOL_KEY || s->outsrc[v->varattno - 1] != i ||
			s->keys[i].eqop != list_nth_oid(eqops, i) ||
			s->keys[i].collation != list_nth_oid(collations, i))
			return false;
	}
	return true;
}

uint32
vexec_agg_last_hash(VexecNode *node)
{
	return ((VexecAggState *) node)->last_hash;
}

/*
 * X9: the hashes of the groups of the batch the node last handed up, a
 * row's each; NULL where it keeps none, its n best (C3).
 */
const uint32 *
vexec_agg_batch_hashes(VexecNode *node)
{
	VexecAggState *s = (VexecAggState *) node;

	return s->topn > 0 ? NULL : s->outhash;
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
	/* VC4: in the keys' order, or hashed, decided at the first group asked for */
	s->sorted_planned = plan.sorted_input && plan.strategy == AGG_HASHED && plan.keycols != NIL;
	s->sorted_decided = !s->sorted_planned;

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
		key_setup(&s->keys[i], s->indesc, list_nth_int(plan.keycols, i) - 1,
				  list_nth_oid(plan.eqops, i), list_nth_oid(plan.collations, i));
	/* X5: an array of groups where every key is an integer's bits */
	s->array_ok = s->strategy == AGG_HASHED && s->nkeys > 0;
	for (i = 0; i < s->nkeys; i++)
		s->array_ok &= s->keys[i].kind == KEY_BITS &&
			(s->keys[i].typlen == 1 || s->keys[i].typlen == 2 ||
			 s->keys[i].typlen == 4 || s->keys[i].typlen == 8);
	s->alo = palloc0_array(int64, Max(s->nkeys, 1));
	s->ahi = palloc0_array(int64, Max(s->nkeys, 1));
	s->awidth = palloc0_array(int64, Max(s->nkeys, 1));
	s->topk = s->strategy == AGG_HASHED && s->nkeys > 0 ? plan.topk : 0;
	s->tbound = PG_UINT32_MAX;
	s->hashes_from_child = s->child_batches && s->strategy == AGG_HASHED &&
		node->vec_child->kind == VEXEC_NODE_REPART &&
		vexec_repart_hashes_keys(node->vec_child, plan.keycols, plan.eqops, plan.collations);

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

	/*
	 * C5: rows passed over only where no transition may raise -- each one
	 * vexec's own, not through fmgr -- for such a row to be one PostgreSQL's
	 * would not have raised on.  The groups of the smallest hashes are kept,
	 * and a final or a single stage returns only those, either way: a stage
	 * below or above may pass rows over
	 */
	s->topk_skip = s->topk > 0;
	for (i = 0; i < s->ntrans && s->topk_skip; i++)
		if (s->trans[i].kind == VEXEC_AGG_FMGR || s->trans[i].may_raise)
			s->topk_skip = false;
	if (s->topk > 0)
		s->theap = palloc(sizeof(uint32) * s->topk);
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
	if (s->strategy == AGG_HASHED)
		s->size_key = vexec_aggsize_key(estate, cscan->scan.plan.plan_node_id);

	/*
	 * X9: batches for a vector parent where the output is the scan tuple as
	 * it is -- no HAVING, no projection -- and no column of it is a state
	 * kept in memory, an internal one
	 */
	if (css->ss.ps.qual == NULL && css->ss.ps.ps_ProjInfo == NULL)
	{
		TupleDesc	scandesc = css->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
		VexecType **types = palloc(sizeof(VexecType *) * Max(scandesc->natts, 1));
		bool		ok = scandesc->natts > 0;

		for (i = 0; i < scandesc->natts && ok; i++)
		{
			Form_pg_attribute att = TupleDescAttr(scandesc, i);

			if (att->atttypid == INTERNALOID || get_typtype(att->atttypid) == TYPTYPE_PSEUDO)
				ok = false;
			else
				types[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		}
		if (ok)
		{
			s->outb = vexec_batch_create(node->mcxt, scandesc->natts, types);
			s->outhash = palloc0_array(uint32, VEXEC_BATCH_ROWS);
			node->next_out = agg_next_batch;
		}
	}
	s->groupcxt = BumpContextCreate(node->mcxt, "VecAgg groups", ALLOCSET_DEFAULT_SIZES);
	s->hashcxt = AllocSetContextCreate(node->mcxt, "VecAgg hash table", ALLOCSET_DEFAULT_SIZES);
	if (!(eflags & EXEC_FLAG_EXPLAIN_ONLY) && s->sorted_decided)
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
	if (s->topn > 0)
	{
		MemoryContextReset(s->topcxt);
		s->ntop = 0;
		s->top_filled = false;
		s->topnext = 0;
	}
	/* C5: the heap of hashes again, from none */
	s->ntheap = 0;
	s->tbound = PG_UINT32_MAX;
	if (s->sorted)
		sorted_reset(s);		/* VC4: the child's order is the same scan's */
	else if (s->sorted_decided)
		table_reset(s);
	/*
	 * X9: the groups dealt as before, the parent's choice, each partition's
	 * batch empty -- a child rescans lazily, at the first batch its parent
	 * asks of it, after the parent has asked for the dealing again
	 */
	for (int p = 0; p < s->ndeal; p++)
		if (s->dealt[p] != NULL)
			vexec_batch_reset(s->dealt[p]);
	s->dealt_out = -1;
	s->deal_rest = 0;
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
	for (int g = 0; g < 2; g++)
		if (s->saggcontext[g])
			ReScanExprContext(s->saggcontext[g]);
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
	return psprintf("Vec %s%s", split, s->sorted_planned ? "GroupAggregate" :
					s->strategy == AGG_HASHED ? "HashAggregate" : "Aggregate");
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
	/* VC4: the input in the keys' order, its source's, where the scan says so as it begins */
	if (s->sorted_planned)
		ExplainPropertyText("Group Order", "the scan's", es);
	if (s->sorted_planned && es->analyze && node->ran && s->sorted_decided)
		ExplainPropertyText("Groups In Order", s->sorted ? "yes" : "no: hashed", es);
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
		if (node->nshared > 0)
			ExplainPropertyInteger("Shared Steps", NULL, node->nshared, es);
		vexec_node_explain_declared(node, es);
		ExplainPropertyText("Input", s->child_batches ? "batches" : "rows", es);
		if (es->analyze && node->stats.batches_out > 0)
			ExplainPropertyText("Output", "batches", es);
		if (s->hashes_from_child)
			ExplainPropertyText("Key Hashes", "the child's", es);
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
			/* C6: whether a table grew past the size it began with */
			if (es->verbose && s->buckets_initial > 0)
			{
				if (es->format != EXPLAIN_FORMAT_TEXT)
				{
					ExplainPropertyUInteger("Buckets", NULL, s->buckets_final, es);
					ExplainPropertyUInteger("Original Buckets", NULL, s->buckets_initial, es);
				}
				else if (s->buckets_final > s->buckets_initial)
					ExplainPropertyText("Buckets", psprintf(UINT64_FORMAT " (originally " UINT64_FORMAT ")",
															s->buckets_final, s->buckets_initial), es);
				else
					ExplainPropertyUInteger("Buckets", NULL, s->buckets_initial, es);
			}
		}
		/* C5: the groups kept by their hashes, and the rows passed over */
		if (s->topk > 0)
		{
			ExplainPropertyInteger("Groups Kept by Hash", NULL, s->topk, es);
			ExplainPropertyInteger("Rows Passed Over", NULL, s->rows_skipped, es);
		}
		/* X5: tables made arrays of groups, and those hashed after all */
		if (es->verbose && s->array_tables > 0)
			ExplainPropertyText("Group Table",
								psprintf("array of " INT64_FORMAT " slots%s", s->array_slots,
										 s->array_left > 0 ? ", then hashed" : ""), es);
		if (s->frozen_groups > 0)
		{
			ExplainPropertyInteger("Table Frozen At", "groups", s->frozen_groups, es);
			ExplainPropertyInteger("Groups Handed On", NULL, s->passed_groups, es);
			if (s->thawed_after > 0)
				ExplainPropertyInteger("Table Thawed After", "groups handed on",
									   s->thawed_after, es);
		}
		if (s->topn > 0)
		{
			ExplainPropertyInteger("Top N Kept", NULL, s->ntop, es);
			ExplainPropertyInteger("Groups Ranked", NULL, s->top_ranked, es);
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
