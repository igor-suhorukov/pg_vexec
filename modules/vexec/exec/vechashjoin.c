/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vechashjoin.c
 *	  VecHashJoin: a hash join, PostgreSQL's HashJoin and its Hash in one
 *	  node, its build side read into a table of columns and its probe side a
 *	  batch at a time (pg_vector_executor.md §3.8).
 *
 * The node is written from PostgreSQL's own (nodeHashjoin.c, nodeHash.c),
 * and keeps its order of reads and of evaluation:
 *
 *	- the outer side's first row is fetched before the table is built where
 *	  PostgreSQL fetches it (nodeHashjoin.c:288-332): for a left or anti
 *	  join, or where the outer side starts cheaper than the inner side costs;
 *	  an outer side with no row then leaves the inner side unread;
 *	- the inner side is read whole into the table; one with no row to match
 *	  leaves the outer side unread but for that first row, unless the join
 *	  keeps its outer rows (nodeHashjoin.c:350-375);
 *	- for each outer row in order, the inner rows of its hash value in the
 *	  order PostgreSQL's bucket holds them, the last inserted first
 *	  (nodeHash.c, ExecHashTableInsert): the hash clauses, then the join
 *	  quals, decide a pair, and a semi join, an anti join and an inner-unique
 *	  join stop at an outer row's first match (nodeHashjoin.c:540-605);
 *	- a left or anti join's outer row that met no inner row is null-extended
 *	  after its pairs; a right join's inner rows that met none come after
 *	  the outer side; then a left or anti join's outer rows of a NULL key,
 *	  then a right join's inner rows of a NULL key, which PostgreSQL 19
 *	  keeps aside as it meets them (nodeHashjoin.c:609-760, 1135-1160).
 *
 * Two levels of evaluation, as PostgreSQL's:
 *
 *	1. the hash clauses and the join quals decide whether a pair of rows
 *	   matches.  The keys are hashed and compared a batch at a time: as
 *	   integers, bits or bytes where the operator compares them so, else by
 *	   its own functions.  Where the hash clauses are not the keys'
 *	   equalities, or join quals remain, they are compiled over the
 *	   candidate pairs as a vector node's quals are (expr/), in their order;
 *	2. the join's other quals and its target list, over the rows the join
 *	   gives -- the pairs that matched, and null-extended rows -- are the
 *	   node's own quals and targets (node.c), over join rows: the scan
 *	   tuple of the outer side's columns and then the inner side's.
 *
 * What PostgreSQL would evaluate for a pair in its order, and what may
 * raise or has side effects, runs in that order.  A pair the first level
 * could not decide ahead -- a kernel could not compute it, a row-by-row qual
 * decides it -- is a join row still to be decided, and so is every row of
 * the same outer row whose fate depends on it: a semi join's later pairs, an
 * outer join's null-extended row.  Such rows reach the consumer, in row
 * order, as rows still to be resolved (child_redo), and resolve_input
 * decides each as it is reached, through PostgreSQL's evaluator.  An outer
 * row the outer side still resolves, or whose keys only PostgreSQL's
 * evaluator computes, is taken only once every row before it has been
 * consumed: a batch of join rows never reaches past it.  A row child is read
 * a batch ahead only where reading early costs nothing but time
 * (vexec_plan_read_ahead_safe()), else a row at a time.
 *
 * The table is held to get_hash_memory_limit().  Past it the join is a
 * grace hash join: the inner rows go to partitions on disk by bits of their
 * hash, the outer rows follow them, and each partition is joined in turn,
 * split again by the next bits where it still does not fit, as far as the
 * bits go.  Rows then come out in partition order, as PostgreSQL's come out
 * in its batches' order; and the outer side is read whole before the first
 * row comes out, where PostgreSQL's first batch would interleave.
 *
 * Where PostgreSQL leaves a side unread and the side receives a Motion,
 * gp_core's executor would squelch it (pg19/modules/gp_core/gp_motion.c:
 * 1869-1896, 2062-2079), or its senders wait.  A VecHashJoin is no node
 * gp_core's wrappers squelch: a vector parent takes its batches without
 * ExecProcNode.  From V7 it asks gp_core to squelch its subtree
 * (GpCoreApi.squelch_subtree, §3.10), which gp_core does in a fragment
 * whose slices stream, where the join is never run again; elsewhere -- on
 * the coordinator, below a nested loop's inner side -- it reads such a
 * side's Motions to their end, and EXPLAIN ANALYZE counts them.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "access/tupmacs.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "port/pg_bitutils.h"
#include "storage/buffile.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ruleutils.h"
#include "utils/tuplestore.h"
#include "varatt.h"

#include "cb_explain.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "expr/expr.h"
#include "motion/motion.h"

/* the grace join's partitions: five bits of the hash a level */
#define HJ_PARTITION_BITS	5
#define HJ_PARTITIONS		(1 << HJ_PARTITION_BITS)
#define HJ_MAX_DEPTH		(32 / HJ_PARTITION_BITS)

/* rows of the table a chunk holds */
#define HJ_CHUNK_ROWS		VEXEC_BATCH_ROWS
#define HJ_CHUNK_SHIFT		10

/* gp_core's Motion (pg19/include/gp_motion.h) */
#define HJ_MOTION_NAME		"GpMotion"

/* How a key is hashed and compared. */
typedef enum HjKeyKind
{
	HJ_KEY_INT,					/* integers of any width, as int64 */
	HJ_KEY_BITS,				/* one by-value type, equal where its bits are */
	HJ_KEY_BYTES,				/* text of a deterministic collation, bytea */
	HJ_KEY_FMGR					/* the operator's own hash and equality */
} HjKeyKind;

typedef struct HjKey
{
	HjKeyKind	kind;
	Oid			hashop;
	Oid			collation;
	Oid			type[2];		/* the outer side's key type, the inner's */
	int16		typlen[2];
	bool		typbyval[2];
	bool		eq_pure;		/* its equality may run ahead of PostgreSQL's
								 * order */
	FmgrInfo	hash[2];		/* FMGR: each side's hash function */
	FmgrInfo	eq;				/* FMGR: the operator's function */
} HjKey;

/* A row's key, normalized. */
typedef struct HjKeyVal
{
	union
	{
		int64		i;			/* INT, BITS */
		Datum		d;			/* FMGR */
		const char *p;			/* BYTES */
	}			u;
	int32		len;			/* BYTES */
} HjKeyVal;

/*
 * A batch of one side's rows, with their keys: a vector child's own batch, a
 * row child's rows, a row resolved alone, or rows read back from a file.
 */
typedef struct HjBatch
{
	VexecBatch *batch;			/* the side's columns */
	int			nrows;
	uint64	   *rows;			/* the rows there are */
	uint64	   *child_dirty;	/* rows the vector child still resolves, or
								 * NULL */
	uint64	   *key_dirty;		/* rows whose keys PostgreSQL's evaluator
								 * computes, as they are reached */
	uint64	   *nullkey;		/* rows with a NULL key: they meet no row */
	uint32	   *hashes;			/* per row */
	HjKeyVal   *keys;			/* nkeys a row */
	uint8	   *matched;		/* an outer batch's: per row, it met a row */
	MemoryContext keycxt;		/* what the keys point at, beyond the batch */
} HjBatch;

/* One side: its child, its keys, its batches. */
typedef struct HjSide
{
	PlanState  *ps;
	bool		inner;
	int			sidx;			/* 0 outer, 1 inner */
	int			ncols;
	int			first_attno;	/* the scan tuple's attno of its column 1 */
	TupleDesc	desc;			/* the child's result type */
	VexecType **types;
	bool		batches;		/* a vector child that hands up batches */
	bool		read_ahead;		/* a row child read a batch ahead */
	bool		done;			/* the child gave its last row */
	bool		touched;		/* the child was read, or drained */
	VexecBatch *rowbatch;		/* a row child's rows, a file's, a store's */
	VexecNode  *eval;			/* the keys' programs over the side's batch */
	VexecTop   *keys;			/* per key */
	bool		any_lazy_key;
	HjBatch		cur;			/* the child's batch, or rows read */
	HjBatch		one;			/* a row resolved alone */
	VexecBatch *onebatch;
	TupleDesc	spilldesc;		/* the side's columns, its keys, the hash */
	TupleTableSlot *spillslot;	/* virtual */
	TupleTableSlot *readslot;	/* minimal */
	TupleTableSlot *rowslot;	/* the child's row type, virtual */
	TupleTableSlot *nullslot;	/* the child's row type, minimal */
	Tuplestorestate *nullrows;	/* rows of a NULL key a join keeps */
} HjSide;

/* A chunk of the table: inner rows, their hashes, keys and matches. */
typedef struct HjChunk
{
	VexecBatch *batch;			/* the inner side's columns, in store shapes */
	int			nrows;
	uint32		hashes[HJ_CHUNK_ROWS];
	uint32		next[HJ_CHUNK_ROWS];	/* the bucket's next row: rid + 1 */
	uint64		matched[VEXEC_WORDS(HJ_CHUNK_ROWS)];
	HjKeyVal   *keys;			/* nkeys a row */
} HjChunk;

/* A partition of the grace join: its inner rows' file and its outer rows'. */
typedef struct HjPart
{
	BufFile    *inner;
	BufFile    *outer;
	int			depth;			/* the level its rows were split at */
} HjPart;

typedef enum HjPhase
{
	HJ_BUILD,					/* the table to build */
	HJ_PROBE,					/* outer rows: the child's, or a partition's */
	HJ_FILL_INNER,				/* right: the table's rows that met none */
	HJ_NEXT_PART,				/* grace: the next partition */
	HJ_NULL_OUTER,				/* left, anti: outer rows of a NULL key */
	HJ_NULL_INNER,				/* right: inner rows of a NULL key */
	HJ_DONE
} HjPhase;

/* a candidate pair's first level */
#define HJ_C_EVAL		0		/* to be evaluated */
#define HJ_C_MATCH		1
#define HJ_C_NO			2
#define HJ_C_UNDECIDED	3

/* a join row */
#define HJ_JR_PAIR		0		/* an outer row and an inner row */
#define HJ_JR_FILL		1		/* an outer row, the inner side NULL */
#define HJ_JR_INNER		2		/* an inner row, the outer side NULL */

#define HJ_JR_UND		0x01	/* still to be decided, as it is reached */
#define HJ_JR_KNOWN		0x02	/* its first level known to match */

/* where a round's inner rows come from */
#define HJ_SRC_NONE		0
#define HJ_SRC_TABLE	1
#define HJ_SRC_BATCH	2

typedef struct HjState
{
	VexecNode	node;			/* first: the join rows, the other quals, the
								 * target list */
	VexecJoinPlan plan;
	JoinType	jointype;
	bool		fill_outer;		/* left, anti */
	bool		fill_inner;		/* right */
	bool		single_match;	/* semi, or inner_unique */
	int			nscan;			/* the scan tuple's columns */
	VexecType **scantypes;
	TupleDesc	scandesc;

	HjSide		side[2];		/* outer, inner */

	/* the keys */
	int			nkeys;
	HjKey	   *keys;
	bool		simple;			/* the hash clauses are the keys' equalities */

	/* the first level: the hash clauses, unless simple, then the join quals */
	List	   *hashclauses;
	List	   *joinqual;
	ExprState  *hashclauses_state;
	ExprState  *joinqual_state;
	VexecNode  *match;			/* their columns and programs */
	int			nl1;
	VexecTop   *l1;
	int			l1_first_lazy;

	/* the table */
	MemoryContext tablecxt;
	Size		memlimit;
	VexecShape *store_shape;	/* per inner column */
	bool	   *store_uniform;	/* every chunk holds it in its store shape */
	HjChunk   **chunks;
	int			nchunks;
	int			maxchunks;
	int64		nrows;			/* rows in the table */
	uint32	   *heads;			/* per bucket: its first row, rid + 1 */
	uint32		mask;
	bool		built;			/* the table is made: PostgreSQL's
								 * hj_HashTable */
	Bitmapset  *inner_key_params;	/* PARAM_EXEC ids the inner keys read: a
									 * Hash node's own chgParam */
	bool		outer_not_empty;	/* hj_OuterNotEmpty */
	Size		peak_memory;

	/* the grace join */
	bool		spilled;
	int			level;			/* of the files being written */
	BufFile    *parts[2][HJ_PARTITIONS];
	List	   *pending;		/* HjPart, to join, in order */
	HjPart	   *curpart;
	MemoryContext readcxt;		/* a file's rows, read back */
	int			max_depth;
	int64		spilled_parts;
	int64		spilled_rows;

	/* where the join is */
	HjPhase		phase;
	HjBatch    *ob;				/* the outer batch probed, or NULL */
	int			opos;			/* its next row */
	bool		in_one;			/* ob is a resolved row's: cur resumes at
								 * cur_resume */
	int			cur_resume;
	int			inprog;			/* the row whose bucket a round left part
								 * way, or -1 */
	uint32		inprog_next;	/* its bucket's next row, rid + 1 */
	int64		fill_rid;		/* right: the next table row to look at */

	/* a round: its candidates, its outer rows, its join rows */
	int			c_n;
	int		   *c_outer;
	uint32	   *c_rid;
	uint8	   *c_status;
	int		   *c_idx;
	int			r_n;
	int		   *r_row;
	int		   *r_first;
	int		   *r_count;
	bool	   *r_complete;
	int			jr_n;
	int		   *jr_outer;		/* row of jr_ob, or -1 */
	int		   *jr_inner;		/* rid + 1, or row + 1 of jr_ib, or 0 */
	uint8	   *jr_kind;
	uint8	   *jr_flags;
	int		   *jr_tmp;
	HjBatch    *jr_ob;
	int			jr_src;			/* HJ_SRC_* */
	HjBatch    *jr_ib;
	VexecBatch *candbatch;

	/* figures */
	int64		inner_rows;		/* put in the table */
	int64		join_filtered;	/* pairs the first level turned down */
	bool		squelched;		/* gp_core stopped its sides' Motions; the
								 * counts are the node's stats, which the
								 * coordinator's EXPLAIN prints from the
								 * segments' */
	int64		outer_rows_one; /* outer rows read a row at a time */
	int64		rounds;
} HjState;

static void hj_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *hj_exec(CustomScanState *css);
static void hj_end(CustomScanState *css);
static void hj_rescan(CustomScanState *css);
static void hj_explain(CustomScanState *css, List *ancestors, ExplainState *es);

static const CustomExecMethods hj_exec_methods = {
	.CustomName = VEXEC_HASHJOIN_NAME,
	.BeginCustomScan = hj_begin,
	.ExecCustomScan = hj_exec,
	.EndCustomScan = hj_end,
	.ReScanCustomScan = hj_rescan,
	.ExplainCustomScan = hj_explain,
};

Node *
vexec_create_hashjoin_state(CustomScan *cscan)
{
	HjState    *s = palloc0(sizeof(HjState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &hj_exec_methods;
	s->node.kind = VEXEC_NODE_HASHJOIN;
	return (Node *) s;
}

/* ---------------------------------------------------------------------
 * The plan's private list (plan/join.c writes it)
 * ---------------------------------------------------------------------
 */

/*
 * A plan travels as text, to parallel workers and to the segments; a Float
 * is written with its point, or it reads back as an Integer.
 */
List *
vexec_join_plan_encode(const VexecJoinPlan *plan)
{
	return list_make4(list_make4(makeInteger(plan->jointype), makeBoolean(plan->inner_unique),
								 makeInteger(plan->nouter), makeInteger(plan->ninner)),
					  plan->hashoperators,
					  plan->hashcollations,
					  makeFloat(psprintf("%.1f", plan->inner_rows)));
}

void
vexec_join_plan_decode(CustomScan *cscan, VexecJoinPlan *plan)
{
	List	   *priv = cscan->custom_private;
	List	   *head;

	if (list_length(priv) != 4)
		elog(ERROR, "vexec: a VecHashJoin plan of %d private items", list_length(priv));
	head = linitial(priv);
	plan->jointype = intVal(linitial(head));
	plan->inner_unique = boolVal(lsecond(head));
	plan->nouter = intVal(lthird(head));
	plan->ninner = intVal(lfourth(head));
	plan->hashoperators = lsecond(priv);
	plan->hashcollations = lthird(priv);
	plan->inner_rows = floatVal(lfourth(priv));
}

/* ---------------------------------------------------------------------
 * Reading ahead
 * ---------------------------------------------------------------------
 */

static bool
exprs_pure(Node *node)
{
	return vexec_expr_impurity(node, false) == NULL;
}

static bool
plans_read_ahead_safe(List *plans)
{
	ListCell   *lc;

	foreach(lc, plans)
		if (!vexec_plan_read_ahead_safe(lfirst(lc)))
			return false;
	return true;
}

/*
 * Whether a plan's rows may be read a batch ahead of its parent's asking:
 * reading them early can raise no error and have no side effect that
 * reading them as asked would not, so that nothing but time tells.  Scans,
 * sorts, joins and appends whose every expression is pure (expr/compile.c),
 * and gp_core's Motions, which compute nothing here; vexec's own nodes read
 * as rows by the same rule.  Anything else is read a row at a time.
 */
bool
vexec_plan_read_ahead_safe(Plan *plan)
{
	if (plan == NULL)
		return true;
	check_stack_depth();
	if (!exprs_pure((Node *) plan->targetlist) || !exprs_pure((Node *) plan->qual))
		return false;

	switch (nodeTag(plan))
	{
		case T_SeqScan:
		case T_Material:
		case T_Sort:
		case T_IncrementalSort:
		case T_Unique:
		case T_Hash:
		case T_Gather:
		case T_GatherMerge:
			break;
		case T_IndexScan:
			if (!exprs_pure((Node *) ((IndexScan *) plan)->indexqual) ||
				!exprs_pure((Node *) ((IndexScan *) plan)->indexorderby))
				return false;
			break;
		case T_IndexOnlyScan:
			if (!exprs_pure((Node *) ((IndexOnlyScan *) plan)->indexqual) ||
				!exprs_pure((Node *) ((IndexOnlyScan *) plan)->recheckqual) ||
				!exprs_pure((Node *) ((IndexOnlyScan *) plan)->indexorderby))
				return false;
			break;
		case T_BitmapHeapScan:
			if (!exprs_pure((Node *) ((BitmapHeapScan *) plan)->bitmapqualorig))
				return false;
			break;
		case T_BitmapIndexScan:
			if (!exprs_pure((Node *) ((BitmapIndexScan *) plan)->indexqual))
				return false;
			break;
		case T_BitmapAnd:
			if (!plans_read_ahead_safe(((BitmapAnd *) plan)->bitmapplans))
				return false;
			break;
		case T_BitmapOr:
			if (!plans_read_ahead_safe(((BitmapOr *) plan)->bitmapplans))
				return false;
			break;
		case T_Result:
			if (!exprs_pure(((Result *) plan)->resconstantqual))
				return false;
			break;
		case T_Append:
			if (!plans_read_ahead_safe(((Append *) plan)->appendplans))
				return false;
			break;
		case T_MergeAppend:
			if (!plans_read_ahead_safe(((MergeAppend *) plan)->mergeplans))
				return false;
			break;
		case T_SubqueryScan:
			if (!vexec_plan_read_ahead_safe(((SubqueryScan *) plan)->subplan))
				return false;
			break;
		case T_NestLoop:
			if (!exprs_pure((Node *) ((Join *) plan)->joinqual))
				return false;
			break;
		case T_MergeJoin:
			if (!exprs_pure((Node *) ((Join *) plan)->joinqual) ||
				!exprs_pure((Node *) ((MergeJoin *) plan)->mergeclauses))
				return false;
			break;
		case T_HashJoin:
			if (!exprs_pure((Node *) ((Join *) plan)->joinqual) ||
				!exprs_pure((Node *) ((HashJoin *) plan)->hashclauses) ||
				!exprs_pure((Node *) ((HashJoin *) plan)->hashkeys))
				return false;
			break;
		case T_Limit:
			if (!exprs_pure(((Limit *) plan)->limitOffset) ||
				!exprs_pure(((Limit *) plan)->limitCount))
				return false;
			break;
		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;

				if (strcmp(cscan->methods->CustomName, HJ_MOTION_NAME) == 0)
					return true;	/* a receiving Motion computes nothing here */
				if (!vexec_is_vector_node(plan) ||
					cscan->methods == vexec_agg_methods())
					return false;	/* VecAgg's output runs final functions */
				if (!exprs_pure((Node *) cscan->custom_exprs))
					return false;
				break;
			}
		default:
			return false;
	}
	return vexec_plan_read_ahead_safe(plan->lefttree) &&
		vexec_plan_read_ahead_safe(plan->righttree);
}

/* ---------------------------------------------------------------------
 * Keys
 * ---------------------------------------------------------------------
 */

/* How an equality operator lets its keys be hashed and compared. */
static HjKeyKind
key_kind(HjKey *k)
{
	switch (get_opcode(k->hashop))
	{
		case F_INT2EQ:
		case F_INT4EQ:
		case F_INT8EQ:
		case F_INT24EQ:
		case F_INT42EQ:
		case F_INT28EQ:
		case F_INT82EQ:
		case F_INT48EQ:
		case F_INT84EQ:
			if (k->typbyval[0] && k->typbyval[1] &&
				(k->typlen[0] == 2 || k->typlen[0] == 4 || k->typlen[0] == 8) &&
				(k->typlen[1] == 2 || k->typlen[1] == 4 || k->typlen[1] == 8))
				return HJ_KEY_INT;
			break;
		case F_OIDEQ:
		case F_BOOLEQ:
		case F_CHAREQ:
		case F_DATE_EQ:
		case F_TIME_EQ:
		case F_TIMESTAMP_EQ:
		case F_TIMESTAMPTZ_EQ:
		case F_CASH_EQ:
			if (getBaseType(k->type[0]) == getBaseType(k->type[1]) && k->typbyval[0] &&
				k->typlen[0] == k->typlen[1])
				return HJ_KEY_BITS;
			break;
		case F_BYTEAEQ:
			return HJ_KEY_BYTES;
		case F_TEXTEQ:
			if (!OidIsValid(k->collation) || get_collation_isdeterministic(k->collation))
				return HJ_KEY_BYTES;
			break;
		default:
			break;
	}
	return HJ_KEY_FMGR;
}

/* A by-value key's bits: its type's bytes only (vecagg.c's key_bits()). */
static inline int64
key_bits(Datum d, int16 typlen)
{
	switch (typlen)
	{
		case 1:
			return (int64) (d & 0xFF);
		case 2:
			return (int64) (d & 0xFFFF);
		case 4:
			return (int64) (d & UINT64CONST(0xFFFFFFFF));
		default:
			return (int64) d;
	}
}

static inline int64
key_int(Datum d, int16 typlen)
{
	switch (typlen)
	{
		case 2:
			return DatumGetInt16(d);
		case 4:
			return DatumGetInt32(d);
		default:
			return DatumGetInt64(d);
	}
}

/* One key's hash: both sides hash alike what they compare alike. */
static inline uint32
key_hash(HjKey *k, int sidx, const HjKeyVal *v)
{
	switch (k->kind)
	{
		case HJ_KEY_INT:
		case HJ_KEY_BITS:
			return (uint32) murmurhash64((uint64) v->u.i);
		case HJ_KEY_BYTES:
			return hash_bytes((const unsigned char *) v->u.p, v->len);
		default:
			return DatumGetUInt32(FunctionCall1Coll(&k->hash[sidx], k->collation, v->u.d));
	}
}

/* Whether an outer key and an inner key are equal by the operator. */
static inline bool
key_equal(HjKey *k, const HjKeyVal *o, const HjKeyVal *i)
{
	switch (k->kind)
	{
		case HJ_KEY_INT:
		case HJ_KEY_BITS:
			return o->u.i == i->u.i;
		case HJ_KEY_BYTES:
			return o->len == i->len && memcmp(o->u.p, i->u.p, o->len) == 0;
		default:
			return DatumGetBool(FunctionCall2Coll(&k->eq, k->collation, o->u.d, i->u.d));
	}
}

static inline bool
keys_equal(HjState *s, const HjKeyVal *o, const HjKeyVal *i)
{
	for (int k = 0; k < s->nkeys; k++)
		if (!key_equal(&s->keys[k], &o[k], &i[k]))
			return false;
	return true;
}

/*
 * A key's Datum into its normalized value.  With copy, what it points at is
 * copied into cxt; else it points where the Datum does, a detoasted value
 * into cxt.
 */
static void
key_from_datum(HjKey *k, int sidx, Datum d, HjKeyVal *v, MemoryContext cxt, bool copy)
{
	switch (k->kind)
	{
		case HJ_KEY_INT:
			v->u.i = key_int(d, k->typlen[sidx]);
			break;
		case HJ_KEY_BITS:
			v->u.i = key_bits(d, k->typlen[sidx]);
			break;
		case HJ_KEY_BYTES:
			{
				MemoryContext old = MemoryContextSwitchTo(cxt);
				varlena    *vl = (varlena *) DatumGetPointer(d);
				bool		toasted = VARATT_IS_EXTENDED(vl);

				if (toasted)
					vl = pg_detoast_datum_packed(vl);
				v->len = VARSIZE_ANY_EXHDR(vl);
				if (copy && !toasted)
				{
					char	   *p = palloc(Max(v->len, 1));

					memcpy(p, VARDATA_ANY(vl), v->len);
					v->u.p = p;
				}
				else
					v->u.p = VARDATA_ANY(vl);
				MemoryContextSwitchTo(old);
				break;
			}
		default:
			if (copy && !k->typbyval[sidx])
			{
				MemoryContext old = MemoryContextSwitchTo(cxt);

				v->u.d = datumCopy(d, false, k->typlen[sidx]);
				MemoryContextSwitchTo(old);
			}
			else
				v->u.d = d;
			break;
	}
}

/* ---------------------------------------------------------------------
 * Side batches
 * ---------------------------------------------------------------------
 */

static void
hjbatch_init(HjState *s, HjBatch *hb)
{
	MemoryContext mcxt = s->node.mcxt;

	hb->hashes = MemoryContextAllocZero(mcxt, sizeof(uint32) * VEXEC_BATCH_ROWS);
	hb->keys = MemoryContextAllocZero(mcxt, sizeof(HjKeyVal) * Max(s->nkeys, 1) * VEXEC_BATCH_ROWS);
	hb->matched = MemoryContextAllocZero(mcxt, VEXEC_BATCH_ROWS);
	hb->keycxt = AllocSetContextCreate(mcxt, "VecHashJoin keys", ALLOCSET_DEFAULT_SIZES);
}

/*
 * A side batch begun over a batch of nrows rows, every row there and none
 * keyed yet.  Its bitmaps are in the side's work batch, reset here: a side
 * has one batch of its own at a time, and a resolved row's beside it.
 */
static void
hjbatch_begin(HjSide *side, HjBatch *hb, VexecBatch *batch, int nrows, bool reset_work)
{
	VexecBatch *w = side->eval->work;

	if (reset_work)
		vexec_batch_reset(w);
	MemoryContextReset(hb->keycxt);
	hb->batch = batch;
	hb->nrows = nrows;
	hb->rows = vexec_bitmap_alloc(w, Max(nrows, 1), true);
	if (nrows % 64 != 0)
		hb->rows[nrows / 64] &= (UINT64CONST(1) << (nrows % 64)) - 1;
	hb->child_dirty = NULL;
	hb->key_dirty = vexec_bitmap_alloc(w, Max(nrows, 1), false);
	hb->nullkey = vexec_bitmap_alloc(w, Max(nrows, 1), false);
	memset(hb->matched, 0, Max(nrows, 1));
}

/*
 * The keys of a side batch's rows that need nothing resolved first: their
 * programs a batch at a time, then each row's values normalized and hashed.
 * A row a kernel could not compute, or whose keys have a row-by-row
 * program, is key_dirty: PostgreSQL's evaluator computes its keys when the
 * row is reached.  The programs run ahead of PostgreSQL's order -- not
 * exactly: an InitPlan behind a key runs where a row reaches it (eval.c).
 */
static void
batch_keys(HjState *s, HjSide *side, HjBatch *hb)
{
	VexecNode  *ev = side->eval;
	int			n = hb->nrows;
	int			sidx = side->sidx;
	VexecVec  **cols = palloc0(sizeof(VexecVec *) * Max(s->nkeys, 1));
	VexecEval	e;
	uint64	   *active;
	uint64	   *clean;
	int			k;

	ev->in = hb->batch;
	ev->loaded_row = -1;
	active = hb->child_dirty != NULL ?
		vexec_bits_andnot(ev->work, hb->rows, hb->child_dirty, n) :
		vexec_bits_copy(ev->work, hb->rows, n);
	memset(&e, 0, sizeof(e));
	e.node = ev;
	e.in = hb->batch;
	e.work = ev->work;
	e.nrows = n;
	e.redo = vexec_bitmap_alloc(ev->work, n, false);
	e.exact = false;
	e.econtext = ev->eager_econtext;
	if (vexec_bits_any(active, n))
		for (k = 0; k < s->nkeys; k++)
			if (side->keys[k].eager != NULL)
				cols[k] = vexec_eval(&e, side->keys[k].eager, active);
	ResetExprContext(ev->eager_econtext);

	vexec_bits_or_into(hb->key_dirty, e.redo, n);
	if (side->any_lazy_key)
		vexec_bits_or_into(hb->key_dirty, active, n);
	clean = vexec_bits_andnot(ev->work, active, hb->key_dirty, n);

	for (int r = vexec_bits_next(clean, n, 0); r >= 0; r = vexec_bits_next(clean, n, r + 1))
		hb->hashes[r] = 0;
	for (k = 0; k < s->nkeys && vexec_bits_any(clean, n); k++)
	{
		HjKey	   *key = &s->keys[k];
		VexecVec   *v = cols[k];
		bool		ints;

		ints = (key->kind == HJ_KEY_INT || key->kind == HJ_KEY_BITS) &&
			v->encoding == VEXEC_FLAT && v->shape.layout == VEXEC_FIXED &&
			!v->shape.arrow_values && v->shape.width == v->shape.stride &&
			v->shape.width == key->typlen[sidx];
		for (int r = vexec_bits_next(clean, n, 0); r >= 0; r = vexec_bits_next(clean, n, r + 1))
		{
			HjKeyVal   *kv = &hb->keys[r * s->nkeys + k];

			if (vexec_bit(hb->nullkey, r))
				continue;
			if (vexec_vec_isnull(v, r))
			{
				vexec_bit_set(hb->nullkey, r);
				continue;
			}
			if (ints)
			{
				switch (v->shape.width)
				{
					case 1:
						kv->u.i = ((const uint8 *) v->values)[r];
						break;
					case 2:
						kv->u.i = key->kind == HJ_KEY_INT ? (int64) ((const int16 *) v->values)[r] :
							(int64) (uint16) ((const int16 *) v->values)[r];
						break;
					case 4:
						kv->u.i = key->kind == HJ_KEY_INT ? (int64) ((const int32 *) v->values)[r] :
							(int64) (uint32) ((const int32 *) v->values)[r];
						break;
					default:
						kv->u.i = ((const int64 *) v->values)[r];
						break;
				}
			}
			else if (key->kind == HJ_KEY_BYTES && v->encoding == VEXEC_FLAT &&
					 (v->shape.layout == VEXEC_VIEW || v->shape.layout == VEXEC_OFFSETS))
			{
				const char *p;
				Size		len;

				vexec_vec_value_bytes(v, r, &p, &len);
				kv->u.p = p;
				kv->len = (int32) len;
			}
			else
			{
				bool		isnull;
				Datum		d = vexec_vec_datum(ev->work, v, r, &isnull);

				key_from_datum(key, sidx, d, kv, hb->keycxt, false);
			}
			hb->hashes[r] = pg_rotate_left32(hb->hashes[r], 1) ^ key_hash(key, sidx, kv);
		}
	}
	for (int r = vexec_bits_next(clean, n, 0); r >= 0; r = vexec_bits_next(clean, n, r + 1))
		if (!vexec_bit(hb->nullkey, r))
			hb->hashes[r] = murmurhash32(hb->hashes[r]);
	pfree(cols);
}

/* A row of a side batch into the side's slot, as scan tuple columns. */
static void
side_load_row(HjSide *side, HjBatch *hb, int row)
{
	side->eval->in = hb->batch;
	side->eval->loaded_row = -1;
	vexec_node_load_input(side->eval, row);
}

/*
 * A row's keys by PostgreSQL's evaluator, in their order, over the side's
 * slot: false at the first NULL key, after which a strict hash evaluates
 * none (execExpr.c, ExecBuildHash32Expr).
 */
static bool
row_keys(HjState *s, HjSide *side, HjKeyVal *kv, uint32 *hash, MemoryContext cxt)
{
	ExprContext *econtext = side->eval->eager_econtext;
	uint32		h = 0;

	for (int k = 0; k < s->nkeys; k++)
	{
		bool		isnull;
		Datum		d = ExecEvalExprSwitchContext(side->keys[k].state, econtext, &isnull);

		if (isnull)
		{
			ResetExprContext(econtext);
			return false;
		}
		key_from_datum(&s->keys[k], side->sidx, d, &kv[k], cxt, true);
		h = pg_rotate_left32(h, 1) ^ key_hash(&s->keys[k], side->sidx, &kv[k]);
	}
	ResetExprContext(econtext);
	*hash = murmurhash32(h);
	return true;
}

/* A row the vector child resolved, into the side's one-row batch. */
static void
side_one_from_slot(HjState *s, HjSide *side, TupleTableSlot *slot)
{
	VexecBatch *b = side->onebatch;

	slot_getsomeattrs(slot, side->ncols);
	vexec_batch_reset(b);
	vexec_batch_begin_rows(b);
	vexec_batch_add_values(b, slot->tts_values, slot->tts_isnull);
	if (!side->inner)
		vexec_batch_apply_config(b, &s->node.layout);
	hjbatch_begin(side, &side->one, b, 1, false);
}

/*
 * The side's next batch from its child: a vector child's batch as it is,
 * the rows it still resolves marked; a row child's rows, a batch of them
 * where they may be read ahead, else one.  False at the child's end.
 */
static bool
side_next(HjState *s, HjSide *side)
{
	HjBatch    *hb = &side->cur;

	if (side->done)
		return false;
	side->touched = true;
	if (side->batches)
	{
		VexecNode  *child = (VexecNode *) side->ps;
		VexecBatch *b;
		int			n;

		for (;;)
		{
			b = vexec_next_batch(side->ps);
			if (b == NULL)
			{
				side->done = true;
				return false;
			}
			if (b->nrows > 0)
				break;
		}
		n = b->nrows;
		hjbatch_begin(side, hb, b, n, true);
		if (b->selection != NULL)
			memcpy(hb->rows, b->selection, sizeof(uint64) * VEXEC_WORDS(n));
		if (vexec_bits_any(child->redo, n) ||
			(child->child_redo != NULL && vexec_bits_any(child->child_redo, n)))
		{
			hb->child_dirty = vexec_bits_copy(side->eval->work, child->redo, n);
			if (child->child_redo != NULL)
				vexec_bits_or_into(hb->child_dirty, child->child_redo, n);
			vexec_bits_or_into(hb->rows, hb->child_dirty, n);
		}
	}
	else
	{
		VexecBatch *b = side->rowbatch;
		int			max = side->read_ahead ? VEXEC_BATCH_ROWS : 1;

		vexec_batch_reset(b);
		vexec_batch_begin_rows(b);
		while (b->nrows < max)
		{
			TupleTableSlot *slot = ExecProcNode(side->ps);

			if (TupIsNull(slot))
			{
				side->done = true;
				break;
			}
			slot_getsomeattrs(slot, side->ncols);
			vexec_batch_add_values(b, slot->tts_values, slot->tts_isnull);
		}
		if (b->nrows == 0)
			return false;
		if (!side->inner)
		{
			vexec_batch_apply_config(b, &s->node.layout);
			if (!side->read_ahead)
				s->outer_rows_one++;
		}
		hjbatch_begin(side, hb, b, b->nrows, true);
	}
	batch_keys(s, side, hb);
	return true;
}

/* A side batch's row as the side's child gave it, in the side's row slot. */
static TupleTableSlot *
batch_row_slot(HjSide *side, HjBatch *hb, int row)
{
	TupleTableSlot *rs = side->rowslot;

	ExecClearTuple(rs);
	for (int i = 0; i < side->ncols; i++)
		rs->tts_values[i] = vexec_vec_datum(side->eval->work, &hb->batch->cols[i], row,
											&rs->tts_isnull[i]);
	return ExecStoreVirtualTuple(rs);
}

/* A row of a NULL key a join keeps for its end, in its side's store. */
static void
keep_null_row(HjState *s, HjSide *side, TupleTableSlot *row)
{
	if (side->nullrows == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		side->nullrows = tuplestore_begin_heap(true, false, work_mem);
		MemoryContextSwitchTo(old);
	}
	tuplestore_puttupleslot(side->nullrows, row);
}

/* ---------------------------------------------------------------------
 * Columns of join rows
 * ---------------------------------------------------------------------
 */

/* An all-NULL column, in its type's build shape. */
static void
vec_nulls(VexecBatch *db, VexecVec *dst, int n)
{
	VexecShape	shape;

	vexec_type_build_shape(dst->type, &shape);
	vexec_vec_init(db, dst, &shape, n);
	dst->validity = vexec_bitmap_alloc(db, n, false);
}

/* A Datum at row j of a column in its type's build shape. */
static void
vec_put_datum(VexecBatch *db, VexecVec *dst, int j, Datum d)
{
	const VexecType *type = dst->type;

	switch (dst->shape.layout)
	{
		case VEXEC_FIXED:
			{
				char	   *p = (char *) dst->values + (Size) j * dst->shape.stride;

				if (type->typbyval)
					store_att_byval(p, d, type->typlen);
				else
					memcpy(p, DatumGetPointer(d), type->typlen);
				break;
			}
		case VEXEC_BYTE_BOOL:
			((uint8 *) dst->values)[j] = DatumGetBool(d) ? 1 : 0;
			break;
		case VEXEC_DATUM:
			((Datum *) dst->values)[j] = d;
			break;
		default:
			elog(ERROR, "vexec: a join row column of layout %d", dst->shape.layout);
	}
}

/* Rows of a column into another by index, through their Datums. */
static void
vec_take_datums(VexecBatch *db, VexecVec *dst, const VexecVec *src, const int *idx, int n)
{
	VexecShape	shape;

	vexec_type_build_shape(dst->type, &shape);
	vexec_vec_init(db, dst, &shape, n);
	dst->validity = vexec_bitmap_alloc(db, n, true);
	for (int j = 0; j < n; j++)
	{
		bool		isnull;
		Datum		d;

		if (idx[j] < 0)
		{
			vexec_bit_clear(dst->validity, j);
			continue;
		}
		d = vexec_vec_datum(db, src, idx[j], &isnull);
		if (isnull)
			vexec_bit_clear(dst->validity, j);
		else
			vec_put_datum(db, dst, j, d);
	}
}

/*
 * Rows of a column into another, by index: dst's row j is src's row
 * idx[j], or NULL where idx[j] < 0.  Fixed-width values are copied;
 * varlena values are pointed at where they lie, which outlives the join
 * rows: the side's batch, or the table.
 */
static void
vec_take(VexecBatch *db, VexecVec *dst, const VexecVec *src, const int *idx, int n)
{
	const VexecType *type = dst->type;
	bool		anynull = false;
	bool		need_validity;
	int			j;

	for (j = 0; j < n && !anynull; j++)
		anynull = idx[j] < 0;

	if (src->encoding == VEXEC_CONST)
	{
		if (!anynull)
		{
			*dst = *src;
			dst->type = type;
			return;
		}
		vec_take_datums(db, dst, src, idx, n);
		return;
	}
	need_validity = src->validity != NULL || anynull;
	if (src->encoding == VEXEC_DICT)
	{
		*dst = *src;
		dst->type = type;
		dst->nvalues = n;
		dst->codes = vexec_batch_alloc0(db, sizeof(int32) * Max(n, 1));
		dst->validity = need_validity ? vexec_bitmap_alloc(db, n, false) : NULL;
		for (j = 0; j < n; j++)
		{
			if (idx[j] < 0)
				continue;
			dst->codes[j] = src->codes[idx[j]];
			if (dst->validity != NULL && !vexec_vec_isnull(src, idx[j]))
				vexec_bit_set(dst->validity, j);
		}
		return;
	}

	switch (src->shape.layout)
	{
		case VEXEC_FIXED:
		case VEXEC_SCALED:
			{
				Size		stride = src->shape.stride;
				Size		width = src->shape.width;
				const char *sv = src->values;
				char	   *dv;

				vexec_vec_init(db, dst, &src->shape, n);
				dv = dst->values;
				for (j = 0; j < n; j++)
					if (idx[j] >= 0)
						memcpy(dv + (Size) j * stride, sv + (Size) idx[j] * stride, width);
				break;
			}
		case VEXEC_BYTE_BOOL:
			vexec_vec_init(db, dst, &src->shape, n);
			for (j = 0; j < n; j++)
				if (idx[j] >= 0)
					((uint8 *) dst->values)[j] = ((const uint8 *) src->values)[idx[j]];
			break;
		case VEXEC_BIT_BOOL:
			vexec_vec_init(db, dst, &src->shape, n);
			for (j = 0; j < n; j++)
				if (idx[j] >= 0 && vexec_bit((const uint64 *) src->values, idx[j]))
					vexec_bit_set((uint64 *) dst->values, j);
			break;
		case VEXEC_DATUM:
			vexec_vec_init(db, dst, &src->shape, n);
			for (j = 0; j < n; j++)
				if (idx[j] >= 0)
					((Datum *) dst->values)[j] = ((const Datum *) src->values)[idx[j]];
			break;
		case VEXEC_VIEW:
			vexec_vec_init(db, dst, &src->shape, n);
			dst->buffers = src->buffers;
			dst->buffer_sizes = src->buffer_sizes;
			dst->nbuffers = src->nbuffers;
			for (j = 0; j < n; j++)
				if (idx[j] >= 0)
					((VexecView *) dst->values)[j] = ((const VexecView *) src->values)[idx[j]];
			if (src->datums != NULL)
			{
				dst->datums = vexec_batch_alloc0(db, sizeof(Datum) * Max(n, 1));
				for (j = 0; j < n; j++)
					if (idx[j] >= 0)
						dst->datums[j] = src->datums[idx[j]];
			}
			break;
		case VEXEC_OFFSETS:
			{
				/* views over the offsets' one buffer, shared */
				VexecShape	view = {VEXEC_VIEW, false, 0, 0, 0};
				const int32 *off = (const int32 *) src->values;

				vexec_vec_init(db, dst, &view, n);
				dst->buffers = src->buffers;
				dst->buffer_sizes = src->buffer_sizes;
				dst->nbuffers = 1;
				for (j = 0; j < n; j++)
				{
					VexecView  *vw = &((VexecView *) dst->values)[j];
					int32		len;
					const char *p;

					if (idx[j] < 0 || vexec_vec_isnull(src, idx[j]))
						continue;
					len = off[idx[j] + 1] - off[idx[j]];
					p = src->buffers[0] + off[idx[j]];
					vw->inlined.size = len;
					if (len <= VEXEC_VIEW_INLINE)
						memcpy(vw->inlined.data, p, len);
					else
					{
						memcpy(vw->ref.prefix, p, 4);
						vw->ref.buffer_index = 0;
						vw->ref.offset = off[idx[j]];
					}
				}
				if (src->datums != NULL)
				{
					dst->datums = vexec_batch_alloc0(db, sizeof(Datum) * Max(n, 1));
					for (j = 0; j < n; j++)
						if (idx[j] >= 0)
							dst->datums[j] = src->datums[idx[j]];
				}
				break;
			}
	}
	dst->type = type;
	if (need_validity)
	{
		dst->validity = vexec_bitmap_alloc(db, n, false);
		for (j = 0; j < n; j++)
			if (idx[j] >= 0 && !vexec_vec_isnull(src, idx[j]))
				vexec_bit_set(dst->validity, j);
	}
}

/*
 * An inner column of join rows, from the table: rid1[j] is row j's table
 * row plus one, or 0 for NULL.  Fixed-width values are copied, varlena ones
 * pointed at in the table; a column some chunk holds as Datums comes out as
 * Datums.
 */
static void
gather_table(HjState *s, VexecBatch *db, VexecVec *dst, int col, const int *rid1, int n)
{
	VexecShape	shape = s->store_shape[col];
	const VexecType *type = dst->type;
	bool		any_null = false;

	if (!s->store_uniform[col])
	{
		VexecShape	datum = {VEXEC_DATUM, false, 0, 0, 0};

		shape = datum;
	}
	vexec_vec_init(db, dst, &shape, n);
	dst->type = type;
	for (int j = 0; j < n; j++)
	{
		uint32		rid;
		HjChunk    *ch;
		int			r;
		const VexecVec *v;

		if (rid1[j] <= 0)
		{
			any_null = true;
			continue;
		}
		rid = (uint32) (rid1[j] - 1);
		ch = s->chunks[rid >> HJ_CHUNK_SHIFT];
		r = rid & (HJ_CHUNK_ROWS - 1);
		v = &ch->batch->cols[col];
		if (vexec_vec_isnull(v, r))
		{
			any_null = true;
			continue;
		}
		if (v->shape.layout != shape.layout)
		{
			bool		isnull;

			((Datum *) dst->values)[j] = vexec_vec_datum(db, v, r, &isnull);
			continue;
		}
		switch (shape.layout)
		{
			case VEXEC_FIXED:
			case VEXEC_SCALED:
				memcpy((char *) dst->values + (Size) j * shape.stride,
					   (const char *) v->values + (Size) r * shape.stride, shape.width);
				break;
			case VEXEC_BYTE_BOOL:
				((uint8 *) dst->values)[j] = ((const uint8 *) v->values)[r];
				break;
			default:
				((Datum *) dst->values)[j] = ((const Datum *) v->values)[r];
				break;
		}
	}
	if (any_null)
	{
		dst->validity = vexec_bitmap_alloc(db, n, false);
		for (int j = 0; j < n; j++)
		{
			uint32		rid;

			if (rid1[j] <= 0)
				continue;
			rid = (uint32) (rid1[j] - 1);
			if (!vexec_vec_isnull(&s->chunks[rid >> HJ_CHUNK_SHIFT]->batch->cols[col],
								  rid & (HJ_CHUNK_ROWS - 1)))
				vexec_bit_set(dst->validity, j);
		}
	}
}

/* ---------------------------------------------------------------------
 * The table
 * ---------------------------------------------------------------------
 */

static Size
table_memory(HjState *s)
{
	Size		m = MemoryContextMemAllocated(s->tablecxt, true);

	s->peak_memory = Max(s->peak_memory, m);
	return m;
}

/* The buckets of a table of n rows: a power of two, a row a bucket. */
static uint64
table_buckets(int64 n)
{
	uint64		b = 1024;

	while (b < (uint64) n && b < (UINT64CONST(1) << 31))
		b <<= 1;
	return b;
}

/* Past the memory the table may hold, the buckets it will need counted. */
static bool
table_full(HjState *s)
{
	return table_memory(s) + table_buckets(s->nrows) * sizeof(uint32) > s->memlimit;
}

/* A new chunk, its columns empty in their store shapes. */
static HjChunk *
chunk_new(HjState *s)
{
	HjSide	   *in = &s->side[1];
	MemoryContext old = MemoryContextSwitchTo(s->tablecxt);
	HjChunk    *ch = palloc0(sizeof(HjChunk));

	ch->batch = vexec_batch_create(s->tablecxt, in->ncols, in->types);
	for (int i = 0; i < in->ncols; i++)
	{
		VexecVec   *v = &ch->batch->cols[i];

		vexec_vec_init(ch->batch, v, &s->store_shape[i], HJ_CHUNK_ROWS);
		v->nvalues = 0;
	}
	ch->keys = palloc0(sizeof(HjKeyVal) * Max(s->nkeys, 1) * HJ_CHUNK_ROWS);
	if (s->nchunks >= s->maxchunks)
	{
		s->maxchunks = Max(s->maxchunks * 2, 64);
		s->chunks = s->chunks ? repalloc(s->chunks, sizeof(HjChunk *) * s->maxchunks) :
			palloc(sizeof(HjChunk *) * s->maxchunks);
	}
	s->chunks[s->nchunks++] = ch;
	MemoryContextSwitchTo(old);
	return ch;
}

/* Row j of a chunk's column NULL; its validity made on the first. */
static void
chunk_set_null(HjChunk *ch, VexecVec *v, int j)
{
	if (v->validity == NULL)
	{
		v->validity = vexec_bitmap_alloc(ch->batch, HJ_CHUNK_ROWS, true);
		for (int r = j; r < HJ_CHUNK_ROWS; r++)
			vexec_bit_clear(v->validity, r);
	}
	vexec_bit_clear(v->validity, j);
	if (v->shape.layout == VEXEC_DATUM)
		((Datum *) v->values)[j] = (Datum) 0;
}

/*
 * A chunk's scaled numeric column as Datums, for a value its scale cannot
 * hold -- NaN, an infinity, more digits: such a column comes out as Datums.
 */
static void
chunk_col_to_datum(HjState *s, HjChunk *ch, int col, int nrows)
{
	VexecVec   *v = &ch->batch->cols[col];
	Datum	   *vals = vexec_batch_alloc0(ch->batch, sizeof(Datum) * HJ_CHUNK_ROWS);
	VexecShape	shape = {VEXEC_DATUM, false, 0, 0, 0};

	for (int r = 0; r < nrows; r++)
	{
		bool		isnull;

		if (!vexec_vec_isnull(v, r))
			vals[r] = vexec_vec_datum(ch->batch, v, r, &isnull);
	}
	v->shape = shape;
	v->values = vals;
	s->store_uniform[col] = false;
}

/* A Datum into row j of a chunk's column, in the column's shape. */
static void
chunk_put_datum(HjState *s, HjChunk *ch, int col, int j, Datum d)
{
	VexecVec   *v = &ch->batch->cols[col];
	const VexecType *type = v->type;

	switch (v->shape.layout)
	{
		case VEXEC_FIXED:
			{
				char	   *p = (char *) v->values + (Size) j * v->shape.stride;

				if (type->typbyval)
					store_att_byval(p, d, type->typlen);
				else
					memcpy(p, DatumGetPointer(d), type->typlen);
				break;
			}
		case VEXEC_BYTE_BOOL:
			((uint8 *) v->values)[j] = DatumGetBool(d) ? 1 : 0;
			break;
		case VEXEC_SCALED:
			{
				int128		value;

				if (vexec_numeric_to_scaled(d, v->shape.scale, type->digits, v->shape.width, &value))
				{
					if (v->shape.width == 8)
					{
						int64		v64 = (int64) value;

						memcpy((char *) v->values + (Size) j * 8, &v64, sizeof(int64));
					}
					else
						memcpy((char *) v->values + (Size) j * 16, &value, sizeof(int128));
					break;
				}
				chunk_col_to_datum(s, ch, col, j);
				((Datum *) v->values)[j] = vexec_varlena_copy(ch->batch, type, d);
				break;
			}
		case VEXEC_DATUM:
			((Datum *) v->values)[j] = vexec_varlena_copy(ch->batch, type, d);
			break;
		default:
			elog(ERROR, "vexec: a VecHashJoin table column of layout %d", v->shape.layout);
	}
}

/*
 * Row i of a side batch's column into row j of a chunk's: its bytes where
 * the shapes are one, else through its Datum.
 */
static void
chunk_put(HjState *s, HjChunk *ch, int col, int j, const VexecVec *src, int i, VexecBatch *work)
{
	VexecVec   *v = &ch->batch->cols[col];

	if (vexec_vec_isnull(src, i))
	{
		chunk_set_null(ch, v, j);
		return;
	}
	if (v->validity != NULL)
		vexec_bit_set(v->validity, j);
	if (src->encoding == VEXEC_FLAT && v->shape.layout != VEXEC_DATUM &&
		vexec_shape_equal(&src->shape, &v->shape))
		vexec_vec_copy_value(ch->batch, v, j, src, i);
	else
	{
		bool		isnull;
		Datum		d = vexec_vec_datum(work, src, i, &isnull);

		chunk_put_datum(s, ch, col, j, d);
	}
}

/* A row's keys into a chunk: what they point at copied in. */
static void
chunk_put_keys(HjState *s, HjChunk *ch, int j, const HjKeyVal *from)
{
	for (int k = 0; k < s->nkeys; k++)
	{
		HjKey	   *key = &s->keys[k];
		HjKeyVal   *to = &ch->keys[j * s->nkeys + k];

		*to = from[k];
		if (key->kind == HJ_KEY_BYTES)
		{
			char	   *p = vexec_arena_alloc(&ch->batch->arena, Max(from[k].len, 1), 1, NULL, NULL);

			memcpy(p, from[k].u.p, from[k].len);
			to->u.p = p;
		}
		else if (key->kind == HJ_KEY_FMGR && !key->typbyval[1])
		{
			MemoryContext old = MemoryContextSwitchTo(ch->batch->mcxt);

			to->u.d = datumCopy(from[k].u.d, false, key->typlen[1]);
			MemoryContextSwitchTo(old);
		}
	}
}

/* Rows of a side batch into the table, in their order. */
static void
table_add(HjState *s, HjBatch *hb, const int *rows, int n)
{
	HjSide	   *in = &s->side[1];
	int			done = 0;

	while (done < n)
	{
		HjChunk    *ch = s->nchunks > 0 ? s->chunks[s->nchunks - 1] : NULL;
		int			seg;
		int			base;

		if (ch == NULL || ch->nrows >= HJ_CHUNK_ROWS)
			ch = chunk_new(s);
		base = ch->nrows;
		seg = Min(n - done, HJ_CHUNK_ROWS - base);
		for (int c = 0; c < in->ncols; c++)
		{
			const VexecVec *src = &hb->batch->cols[c];

			for (int j = 0; j < seg; j++)
				chunk_put(s, ch, c, base + j, src, rows[done + j], in->eval->work);
			ch->batch->cols[c].nvalues = base + seg;
		}
		for (int j = 0; j < seg; j++)
		{
			int			r = rows[done + j];

			ch->hashes[base + j] = hb->hashes[r];
			chunk_put_keys(s, ch, base + j, &hb->keys[r * s->nkeys]);
		}
		ch->nrows = base + seg;
		s->nrows += seg;
		done += seg;
	}
}

/* The buckets over the table's rows: each one's rows, the last first. */
static void
table_link(HjState *s)
{
	uint64		nb = table_buckets(s->nrows);

	s->heads = MemoryContextAllocExtended(s->tablecxt, sizeof(uint32) * nb,
										  MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
	s->mask = (uint32) (nb - 1);
	for (int c = 0; c < s->nchunks; c++)
	{
		HjChunk    *ch = s->chunks[c];

		for (int r = 0; r < ch->nrows; r++)
		{
			uint32		rid = ((uint32) c << HJ_CHUNK_SHIFT) | (uint32) r;
			uint32		b = ch->hashes[r] & s->mask;

			ch->next[r] = s->heads[b];
			s->heads[b] = rid + 1;
		}
	}
	(void) table_memory(s);
}

/* The table empty. */
static void
table_reset(HjState *s)
{
	MemoryContextReset(s->tablecxt);
	s->chunks = NULL;
	s->nchunks = 0;
	s->maxchunks = 0;
	s->nrows = 0;
	s->heads = NULL;
	s->mask = 0;
	for (int i = 0; i < s->side[1].ncols; i++)
		s->store_uniform[i] = true;
}

/* ---------------------------------------------------------------------
 * The grace join's files
 * ---------------------------------------------------------------------
 */

/* The partition a hash's bits at a level choose. */
static inline int
part_of(uint32 hash, int level)
{
	return (hash >> (32 - HJ_PARTITION_BITS * level)) & (HJ_PARTITIONS - 1);
}

static BufFile *
part_file(HjState *s, int sidx, int p)
{
	if (s->parts[sidx][p] == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		s->parts[sidx][p] = BufFileCreateTemp(false);
		MemoryContextSwitchTo(old);
	}
	return s->parts[sidx][p];
}

/* A side's row, its keys and its hash, to the level's file its hash picks. */
static void
spill_write(HjState *s, HjSide *side, TupleTableSlot *row, const HjKeyVal *kv, uint32 hash)
{
	TupleTableSlot *out = side->spillslot;
	MinimalTuple tup;
	bool		shouldFree;

	slot_getallattrs(row);
	ExecClearTuple(out);
	memcpy(out->tts_values, row->tts_values, sizeof(Datum) * side->ncols);
	memcpy(out->tts_isnull, row->tts_isnull, sizeof(bool) * side->ncols);
	for (int k = 0; k < s->nkeys; k++)
	{
		HjKey	   *key = &s->keys[k];
		Datum		d;

		switch (key->kind)
		{
			case HJ_KEY_INT:
			case HJ_KEY_BITS:
				d = Int64GetDatum(kv[k].u.i);
				break;
			case HJ_KEY_BYTES:
				{
					bytea	   *b = palloc(VARHDRSZ + kv[k].len);

					SET_VARSIZE(b, VARHDRSZ + kv[k].len);
					memcpy(VARDATA(b), kv[k].u.p, kv[k].len);
					d = PointerGetDatum(b);
					break;
				}
			default:
				d = kv[k].u.d;
				break;
		}
		out->tts_values[side->ncols + k] = d;
		out->tts_isnull[side->ncols + k] = false;
	}
	out->tts_values[side->ncols + s->nkeys] = UInt32GetDatum(hash);
	out->tts_isnull[side->ncols + s->nkeys] = false;
	ExecStoreVirtualTuple(out);
	tup = ExecFetchSlotMinimalTuple(out, &shouldFree);
	BufFileWrite(part_file(s, side->sidx, part_of(hash, s->level)), tup, tup->t_len);
	s->spilled_rows++;
	if (shouldFree)
		pfree(tup);
}

/*
 * The next row of a file into the side's read slot, its tuple in cxt, which
 * the caller resets; NULL at the file's end.
 */
static MinimalTuple
spill_read(HjSide *side, BufFile *file, MemoryContext cxt)
{
	uint32		len;
	MinimalTuple tup;

	if (BufFileReadMaybeEOF(file, &len, sizeof(uint32), true) == 0)
		return NULL;
	tup = (MinimalTuple) MemoryContextAlloc(cxt, len);
	tup->t_len = len;
	BufFileReadExact(file, (char *) tup + sizeof(uint32), len - sizeof(uint32));
	ExecStoreMinimalTuple(tup, side->readslot, false);
	slot_getallattrs(side->readslot);
	return tup;
}

/* A row read back: its keys and its hash, out of the read slot. */
static void
spill_keys(HjState *s, HjSide *side, HjKeyVal *kv, uint32 *hash, MemoryContext cxt)
{
	TupleTableSlot *rs = side->readslot;

	for (int k = 0; k < s->nkeys; k++)
	{
		HjKey	   *key = &s->keys[k];
		Datum		d = rs->tts_values[side->ncols + k];

		if (key->kind == HJ_KEY_INT || key->kind == HJ_KEY_BITS)
			kv[k].u.i = DatumGetInt64(d);
		else if (key->kind == HJ_KEY_BYTES)
		{
			bytea	   *b = DatumGetByteaPP(d);
			char	   *p = MemoryContextAlloc(cxt, Max(VARSIZE_ANY_EXHDR(b), 1));

			memcpy(p, VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
			kv[k].u.p = p;
			kv[k].len = VARSIZE_ANY_EXHDR(b);
		}
		else
			key_from_datum(key, side->sidx, d, &kv[k], cxt, true);
	}
	*hash = DatumGetUInt32(rs->tts_values[side->ncols + s->nkeys]);
}

static void
spill_rewind(BufFile *file)
{
	if (file != NULL && BufFileSeek(file, 0, 0, SEEK_SET) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind a VecHashJoin spill file")));
}

/*
 * Rows of a file into a side batch, up to a batch of them, their keys and
 * hashes beside: how many.
 */
static int
file_batch(HjState *s, HjSide *side, BufFile *file, HjBatch *hb, VexecBatch *b)
{
	int			n;

	vexec_batch_reset(b);
	vexec_batch_begin_rows(b);
	hjbatch_begin(side, hb, b, VEXEC_BATCH_ROWS, true);
	MemoryContextReset(s->readcxt);
	while (b->nrows < VEXEC_BATCH_ROWS && spill_read(side, file, s->readcxt) != NULL)
	{
		int			r = b->nrows;

		vexec_batch_add_values(b, side->readslot->tts_values, side->readslot->tts_isnull);
		spill_keys(s, side, &hb->keys[r * s->nkeys], &hb->hashes[r], hb->keycxt);
	}
	n = b->nrows;
	if (n == 0)
		return 0;
	if (!side->inner)
		vexec_batch_apply_config(b, &s->node.layout);
	hb->nrows = n;
	if (n % 64 != 0)
		hb->rows[n / 64] &= (UINT64CONST(1) << (n % 64)) - 1;
	for (int w = VEXEC_WORDS(n); w < VEXEC_WORDS(VEXEC_BATCH_ROWS); w++)
		hb->rows[w] = 0;
	return n;
}

/* The table's rows to the level's inner files, and the table emptied. */
static void
spill_table(HjState *s)
{
	HjSide	   *in = &s->side[1];
	TupleTableSlot *rs = in->rowslot;

	for (int c = 0; c < s->nchunks; c++)
	{
		HjChunk    *ch = s->chunks[c];

		for (int r = 0; r < ch->nrows; r++)
		{
			ExecClearTuple(rs);
			for (int i = 0; i < in->ncols; i++)
				rs->tts_values[i] = vexec_vec_datum(ch->batch, &ch->batch->cols[i], r,
													&rs->tts_isnull[i]);
			ExecStoreVirtualTuple(rs);
			spill_write(s, in, rs, &ch->keys[r * s->nkeys], ch->hashes[r]);
		}
		CHECK_FOR_INTERRUPTS();
	}
	table_reset(s);
}

/* The level's files, queued in order, ahead of what is queued. */
static void
spill_queue(HjState *s)
{
	List	   *parts = NIL;
	MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

	for (int p = 0; p < HJ_PARTITIONS; p++)
	{
		HjPart	   *part;

		if (s->parts[0][p] == NULL && s->parts[1][p] == NULL)
			continue;
		part = palloc0(sizeof(HjPart));
		part->outer = s->parts[0][p];
		part->inner = s->parts[1][p];
		part->depth = s->level;
		spill_rewind(part->inner);
		spill_rewind(part->outer);
		parts = lappend(parts, part);
		s->parts[0][p] = s->parts[1][p] = NULL;
		s->spilled_parts++;
	}
	s->pending = list_concat(parts, s->pending);
	s->max_depth = Max(s->max_depth, s->level);
	MemoryContextSwitchTo(old);
}

static void
part_close(HjPart *p)
{
	if (p->inner)
		BufFileClose(p->inner);
	if (p->outer)
		BufFileClose(p->outer);
	p->inner = p->outer = NULL;
}

static void
spill_close_all(HjState *s)
{
	ListCell   *lc;

	foreach(lc, s->pending)
		part_close(lfirst(lc));
	s->pending = NIL;
	if (s->curpart != NULL)
		part_close(s->curpart);
	s->curpart = NULL;
	for (int i = 0; i < 2; i++)
		for (int p = 0; p < HJ_PARTITIONS; p++)
		{
			if (s->parts[i][p])
				BufFileClose(s->parts[i][p]);
			s->parts[i][p] = NULL;
		}
}

/*
 * A partition that does not fit, split by the next level's bits: both its
 * files read through and written again, its parts queued in its place.
 */
static void
split_partition(HjState *s, HjPart *part)
{
	BufFile    *files[2] = {part->outer, part->inner};

	s->level = part->depth + 1;
	for (int i = 0; i < 2; i++)
	{
		HjSide	   *side = &s->side[i];
		MinimalTuple tup;

		if (files[i] == NULL)
			continue;
		spill_rewind(files[i]);
		MemoryContextReset(s->readcxt);
		while ((tup = spill_read(side, files[i], s->readcxt)) != NULL)
		{
			uint32		hash = DatumGetUInt32(side->readslot->tts_values[side->ncols + s->nkeys]);

			BufFileWrite(part_file(s, i, part_of(hash, s->level)), tup, tup->t_len);
			MemoryContextReset(s->readcxt);
			CHECK_FOR_INTERRUPTS();
		}
	}
	part_close(part);
	spill_queue(s);
}

/*
 * A partition's inner rows into the table, its buckets made: true.  False
 * where they do not fit and the bits go further; the partition is then
 * split and its parts queued.
 */
static bool
load_partition(HjState *s, HjPart *part)
{
	HjSide	   *in = &s->side[1];
	int			rows[VEXEC_BATCH_ROWS];

	for (int r = 0; r < VEXEC_BATCH_ROWS; r++)
		rows[r] = r;
	table_reset(s);
	if (part->inner != NULL)
	{
		int			n;

		while ((n = file_batch(s, in, part->inner, &in->cur, in->rowbatch)) > 0)
		{
			table_add(s, &in->cur, rows, n);
			if (part->depth < HJ_MAX_DEPTH && table_full(s))
			{
				table_reset(s);
				split_partition(s, part);
				return false;
			}
			CHECK_FOR_INTERRUPTS();
		}
	}
	s->inner_rows += s->nrows;
	table_link(s);
	return true;
}

/* ---------------------------------------------------------------------
 * Draining a side PostgreSQL leaves unread
 * ---------------------------------------------------------------------
 */

static bool
is_receiving_motion(PlanState *ps)
{
	return IsA(ps, CustomScanState) &&
		strcmp(((CustomScanState *) ps)->methods->CustomName, HJ_MOTION_NAME) == 0 &&
		outerPlanState(ps) == NULL && innerPlanState(ps) == NULL;
}

/*
 * Every Motion a side receives, read to its end, so that its senders stop
 * waiting for it, as gp_core's squelch would stop them; but for a
 * subplan's, which gp_core leaves to the run's end too (gp_motion.c).
 */
static void
drain(HjState *s, PlanState *ps)
{
	if (ps == NULL)
		return;
	check_stack_depth();
	if (is_receiving_motion(ps))
	{
		for (;;)
		{
			TupleTableSlot *slot = ExecProcNode(ps);

			if (TupIsNull(slot))
				break;
			s->node.stats.drained_rows++;
			CHECK_FOR_INTERRUPTS();
		}
		s->node.stats.drained_motions++;
		return;
	}
	drain(s, outerPlanState(ps));
	drain(s, innerPlanState(ps));
	switch (nodeTag(ps))
	{
		case T_AppendState:
			for (int i = 0; i < ((AppendState *) ps)->as_nplans; i++)
				drain(s, ((AppendState *) ps)->appendplans[i]);
			break;
		case T_MergeAppendState:
			for (int i = 0; i < ((MergeAppendState *) ps)->ms_nplans; i++)
				drain(s, ((MergeAppendState *) ps)->mergeplans[i]);
			break;
		case T_SubqueryScanState:
			drain(s, ((SubqueryScanState *) ps)->subplan);
			break;
		case T_CustomScanState:
			foreach_ptr(PlanState, child, ((CustomScanState *) ps)->custom_ps)
				drain(s, child);
			break;
		default:
			break;
	}
}

/*
 * A side the join reads no more: its Motions' senders stopped, where
 * gp_core squelches the join's subtree -- a fragment whose slices stream,
 * the join never run again (GpCoreApi.squelch_subtree, V7) -- else the
 * side's Motions read to their end.  The join reads neither side again
 * once it stops reading one: it gives no row.
 */
static void
drain_side(HjState *s, HjSide *side)
{
	side->touched = true;
	if (!s->squelched && vexec_gp_core_squelch(&s->node.css.ss.ps))
	{
		s->squelched = true;
		s->node.stats.squelched++;
		return;
	}
	if (!s->squelched)
		drain(s, side->ps);
}

/* ---------------------------------------------------------------------
 * Building
 * ---------------------------------------------------------------------
 */

/* Past the memory the table may hold: the grace join, from the first level. */
static void
maybe_spill(HjState *s)
{
	if (s->spilled || !table_full(s))
		return;
	s->spilled = true;
	s->level = 1;
	spill_table(s);
}

/* Rows of an inner batch into the table, or past its memory, to the files. */
static void
inner_keep(HjState *s, HjBatch *hb, const int *rows, int n)
{
	HjSide	   *in = &s->side[1];

	if (n == 0)
		return;
	if (!s->spilled)
	{
		table_add(s, hb, rows, n);
		return;
	}
	for (int j = 0; j < n; j++)
		spill_write(s, in, batch_row_slot(in, hb, rows[j]), &hb->keys[rows[j] * s->nkeys],
					hb->hashes[rows[j]]);
}

/*
 * The inner side, read whole (MultiExecPrivateHash): each row in order --
 * resolved first where the vector child still resolves it, its keys by
 * PostgreSQL's evaluator where they are dirty -- into the table; one of a
 * NULL key kept for a right join's end, else dropped.
 */
static void
build(HjState *s)
{
	HjSide	   *in = &s->side[1];
	int		   *run = palloc(sizeof(int) * VEXEC_BATCH_ROWS);

	table_reset(s);
	while (side_next(s, in))
	{
		HjBatch    *hb = &in->cur;
		int			n = hb->nrows;
		int			nrun = 0;

		for (int r = vexec_bits_next(hb->rows, n, 0); r >= 0; r = vexec_bits_next(hb->rows, n, r + 1))
		{
			bool		dirty_child = hb->child_dirty != NULL && vexec_bit(hb->child_dirty, r);
			bool		dirty_key = vexec_bit(hb->key_dirty, r);

			if (!dirty_child && !dirty_key && !vexec_bit(hb->nullkey, r))
			{
				run[nrun++] = r;
				continue;
			}

			/* the rows before it go first, in order */
			inner_keep(s, hb, run, nrun);
			nrun = 0;
			if (dirty_child)
			{
				TupleTableSlot *slot = vexec_resolve_row((VexecNode *) in->ps, r);
				int			zero = 0;

				if (slot == NULL)
					continue;
				side_one_from_slot(s, in, slot);
				side_load_row(in, &in->one, 0);
				if (!row_keys(s, in, in->one.keys, &in->one.hashes[0], in->one.keycxt))
				{
					if (s->fill_inner)
						keep_null_row(s, in, batch_row_slot(in, &in->one, 0));
					continue;
				}
				inner_keep(s, &in->one, &zero, 1);
				continue;
			}
			if (dirty_key)
			{
				side_load_row(in, hb, r);
				if (!row_keys(s, in, &hb->keys[r * s->nkeys], &hb->hashes[r], hb->keycxt))
				{
					if (s->fill_inner)
						keep_null_row(s, in, batch_row_slot(in, hb, r));
					continue;
				}
				inner_keep(s, hb, &r, 1);
				continue;
			}
			/* a NULL key */
			if (s->fill_inner)
				keep_null_row(s, in, batch_row_slot(in, hb, r));
		}
		inner_keep(s, hb, run, nrun);
		maybe_spill(s);
		CHECK_FOR_INTERRUPTS();
	}
	pfree(run);
	s->built = true;
	if (!s->spilled)
	{
		s->inner_rows += s->nrows;
		table_link(s);
	}
}

/*
 * The outer side's first row, before the table is built, as PostgreSQL
 * fetches it (nodeHashjoin.c:316-332): false where it has none.  A row the
 * vector child still resolves is resolved now, as PostgreSQL's fetch would
 * compute it; its keys wait, as PostgreSQL computes them after the build.
 */
static bool
prefetch_outer(HjState *s)
{
	HjSide	   *out = &s->side[0];

	for (;;)
	{
		HjBatch    *ob = s->ob;
		int			row;

		if (ob == NULL)
		{
			if (!side_next(s, out))
				return false;
			s->ob = &out->cur;
			s->opos = 0;
			continue;
		}
		row = vexec_bits_next(ob->rows, ob->nrows, s->opos);
		if (row < 0)
		{
			s->ob = NULL;
			continue;
		}
		if (ob->child_dirty != NULL && vexec_bit(ob->child_dirty, row))
		{
			TupleTableSlot *slot = vexec_resolve_row((VexecNode *) out->ps, row);

			vexec_bit_clear(ob->child_dirty, row);
			if (slot == NULL)
			{
				vexec_bit_clear(ob->rows, row);
				s->opos = row + 1;
				continue;
			}
			side_one_from_slot(s, out, slot);
			vexec_bit_set(out->one.key_dirty, 0);
			s->cur_resume = row + 1;
			s->in_one = true;
			s->ob = &out->one;
			s->opos = 0;
		}
		return true;
	}
}

/*
 * The outer side, past the table's memory, to the files: every row, in
 * order, resolved and keyed now, since nothing comes out before the
 * partitions are joined; one of a NULL key kept for a left or anti join's
 * end.
 */
static void
spill_outer(HjState *s)
{
	HjSide	   *out = &s->side[0];

	for (;;)
	{
		HjBatch    *ob = s->ob;
		int			row;

		CHECK_FOR_INTERRUPTS();
		if (ob == NULL)
		{
			if (!side_next(s, out))
				break;
			s->ob = &out->cur;
			s->opos = 0;
			continue;
		}
		row = vexec_bits_next(ob->rows, ob->nrows, s->opos);
		if (row < 0)
		{
			if (s->in_one)
			{
				s->in_one = false;
				s->ob = &out->cur;
				s->opos = s->cur_resume;
			}
			else
				s->ob = NULL;
			continue;
		}
		s->opos = row + 1;
		if (ob->child_dirty != NULL && vexec_bit(ob->child_dirty, row))
		{
			TupleTableSlot *slot = vexec_resolve_row((VexecNode *) out->ps, row);

			vexec_bit_clear(ob->child_dirty, row);
			if (slot == NULL)
				continue;
			side_one_from_slot(s, out, slot);
			vexec_bit_set(out->one.key_dirty, 0);
			s->cur_resume = row + 1;
			s->in_one = true;
			s->ob = &out->one;
			s->opos = 0;
			continue;
		}
		if (vexec_bit(ob->key_dirty, row))
		{
			vexec_bit_clear(ob->key_dirty, row);
			side_load_row(out, ob, row);
			if (!row_keys(s, out, &ob->keys[row * s->nkeys], &ob->hashes[row], ob->keycxt))
				vexec_bit_set(ob->nullkey, row);
		}
		if (vexec_bit(ob->nullkey, row))
		{
			if (s->fill_outer)
				keep_null_row(s, out, batch_row_slot(out, ob, row));
			continue;
		}
		s->outer_not_empty = true;
		spill_write(s, out, batch_row_slot(out, ob, row), &ob->keys[row * s->nkeys],
					ob->hashes[row]);
	}
	spill_queue(s);
}

/*
 * The join's first steps (nodeHashjoin.c, HJ_BUILD_HASHTABLE): the outer
 * side's first row where PostgreSQL fetches one, then the table.  False
 * where the join gives no row; the side it leaves unread is drained.
 */
static bool
build_phase(HjState *s)
{
	Plan	   *outer = s->side[0].ps->plan;
	Plan	   *inner = s->side[1].ps->plan;

	if (!s->fill_inner &&
		(s->fill_outer ||
		 (outer->startup_cost < inner->total_cost && !s->outer_not_empty)))
	{
		if (!prefetch_outer(s))
		{
			s->outer_not_empty = false;
			drain_side(s, &s->side[1]);
			return false;
		}
		s->outer_not_empty = true;
	}

	build(s);

	/* an inner side with nothing to match: the outer side is not read */
	if (s->nrows == 0 && !s->spilled && s->side[1].nullrows == NULL && !s->fill_outer)
	{
		drain_side(s, &s->side[0]);
		return false;
	}
	s->outer_not_empty = false;
	return true;
}

/* ---------------------------------------------------------------------
 * Probing: rounds of join rows
 * ---------------------------------------------------------------------
 */

/* The outer side's next batch: the child's, or the partition's. */
static bool
outer_next(HjState *s)
{
	HjSide	   *out = &s->side[0];

	if (s->spilled)
	{
		if (s->curpart == NULL || s->curpart->outer == NULL ||
			file_batch(s, out, s->curpart->outer, &out->cur, out->rowbatch) == 0)
			return false;
	}
	else if (!side_next(s, out))
		return false;
	s->ob = &out->cur;
	s->opos = 0;
	s->inprog = -1;
	return true;
}

static void
mark_matched(HjState *s, uint32 rid)
{
	HjChunk    *ch = s->chunks[rid >> HJ_CHUNK_SHIFT];

	vexec_bit_set(ch->matched, rid & (HJ_CHUNK_ROWS - 1));
}

static void
add_join_row(HjState *s, int kind, int outer, int inner, uint8 flags)
{
	int			j = s->jr_n++;

	Assert(j < VEXEC_BATCH_ROWS);
	s->jr_kind[j] = kind;
	s->jr_outer[j] = outer;
	s->jr_inner[j] = inner;
	s->jr_flags[j] = flags;
}

/*
 * The first level of the round's candidates still to be evaluated: their
 * columns gathered, the hash clauses -- unless the keys' equality decided
 * them -- and the join quals in their order, each over the pairs the ones
 * before it passed.  A pair a kernel could not decide, or that a row-by-row
 * qual must decide, is undecided.  They run ahead of PostgreSQL's order,
 * so not exactly.
 */
static void
first_level(HjState *s)
{
	VexecNode  *m = s->match;
	VexecBatch *cb = s->candbatch;
	HjBatch    *ob = s->jr_ob;
	int			n = 0;
	int		   *oidx;
	int		   *rid1;
	VexecEval	e;
	uint64	   *active;
	bool		lazy = s->l1_first_lazy < s->nl1;

	for (int i = 0; i < s->c_n; i++)
		if (s->c_status[i] == HJ_C_EVAL)
			s->c_idx[n++] = i;
	if (n == 0)
		return;

	vexec_batch_reset(m->work);
	oidx = vexec_batch_alloc(m->work, sizeof(int) * n);
	rid1 = vexec_batch_alloc(m->work, sizeof(int) * n);
	for (int j = 0; j < n; j++)
	{
		oidx[j] = s->c_outer[s->c_idx[j]];
		rid1[j] = (int) s->c_rid[s->c_idx[j]] + 1;
	}
	vexec_batch_reset(cb);
	cb->nrows = n;
	for (int c = 0; c < m->ninput; c++)
	{
		int			attno = m->input_attnos[c];

		if (attno <= s->plan.nouter)
			vec_take(cb, &cb->cols[c], &ob->batch->cols[attno - 1], oidx, n);
		else
			gather_table(s, cb, &cb->cols[c], attno - s->plan.nouter - 1, rid1, n);
	}

	m->in = cb;
	m->loaded_row = -1;
	memset(&e, 0, sizeof(e));
	e.node = m;
	e.in = cb;
	e.work = m->work;
	e.nrows = n;
	e.redo = vexec_bitmap_alloc(m->work, n, false);
	e.exact = false;
	e.econtext = m->eager_econtext;
	active = vexec_bitmap_alloc(m->work, n, true);
	for (int q = 0; q < s->l1_first_lazy && vexec_bits_any(active, n); q++)
		active = vexec_eval_qual(&e, s->l1[q].eager, active);
	ResetExprContext(m->eager_econtext);

	for (int j = 0; j < n; j++)
	{
		int			i = s->c_idx[j];

		if (vexec_bit(e.redo, j))
			s->c_status[i] = HJ_C_UNDECIDED;
		else if (!vexec_bit(active, j))
		{
			s->c_status[i] = HJ_C_NO;
			s->join_filtered++;
		}
		else
			s->c_status[i] = lazy ? HJ_C_UNDECIDED : HJ_C_MATCH;
	}
}

/*
 * The round's join rows, from its outer rows and their candidates, as
 * PostgreSQL would come to them (nodeHashjoin.c:540-640): a pair that
 * matched comes out; a semi, anti or inner-unique join's outer row stops at
 * its first match; a left or anti join's outer row that met none comes
 * null-extended after its pairs.  A row whose fate hangs on an undecided
 * pair before it is undecided too.
 */
static void
assign(HjState *s)
{
	HjBatch    *ob = s->jr_ob;
	bool		stop_at_match = s->single_match || s->jointype == JOIN_ANTI;

	for (int ri = 0; ri < s->r_n; ri++)
	{
		int			oi = s->r_row[ri];
		bool		matched = ob->matched[oi] != 0;
		bool		pending = false;

		for (int ci = s->r_first[ri]; ci < s->r_first[ri] + s->r_count[ri]; ci++)
		{
			uint8		st = s->c_status[ci];
			int			rid1 = (int) s->c_rid[ci] + 1;

			if (stop_at_match && matched)
				break;
			if (st == HJ_C_NO)
				continue;
			if (st == HJ_C_UNDECIDED)
			{
				add_join_row(s, HJ_JR_PAIR, oi, rid1, HJ_JR_UND);
				pending = true;
				continue;
			}
			/* a match */
			if (s->jointype == JOIN_ANTI)
			{
				/* never a row of its own; with none undecided before, decided */
				if (!pending)
					ob->matched[oi] = 1;
				matched = true;
				break;
			}
			if (pending && stop_at_match)
			{
				/* it comes out where the undecided ones before it did not match */
				add_join_row(s, HJ_JR_PAIR, oi, rid1, HJ_JR_UND | HJ_JR_KNOWN);
				matched = true;
				break;
			}
			add_join_row(s, HJ_JR_PAIR, oi, rid1, 0);
			ob->matched[oi] = 1;
			matched = true;
			if (s->fill_inner)
				mark_matched(s, s->c_rid[ci]);
		}
		if (s->r_complete[ri] && s->fill_outer && !matched)
			add_join_row(s, HJ_JR_FILL, oi, 0, pending ? HJ_JR_UND : 0);
	}
}

/* The round's join rows, gathered: node->in, its rows still to be decided. */
static void
make_join_rows(HjState *s)
{
	VexecBatch *jb = s->node.in;
	int			n = s->jr_n;
	uint64	   *und = NULL;
	int			c;

	vexec_batch_reset(jb);
	jb->nrows = n;
	jb->selection = NULL;
	for (c = 0; c < s->plan.nouter; c++)
	{
		if (s->jr_ob != NULL)
			vec_take(jb, &jb->cols[c], &s->jr_ob->batch->cols[c], s->jr_outer, n);
		else
			vec_nulls(jb, &jb->cols[c], n);
	}
	if (s->jr_src == HJ_SRC_BATCH)
		for (int j = 0; j < n; j++)
			s->jr_tmp[j] = s->jr_inner[j] - 1;
	for (c = 0; c < s->plan.ninner; c++)
	{
		VexecVec   *dst = &jb->cols[s->plan.nouter + c];

		switch (s->jr_src)
		{
			case HJ_SRC_TABLE:
				gather_table(s, jb, dst, c, s->jr_inner, n);
				break;
			case HJ_SRC_BATCH:
				vec_take(jb, dst, &s->jr_ib->batch->cols[c], s->jr_tmp, n);
				break;
			default:
				vec_nulls(jb, dst, n);
				break;
		}
	}
	for (int j = 0; j < n; j++)
		if (s->jr_flags[j] & HJ_JR_UND)
		{
			if (und == NULL)
				und = vexec_bitmap_alloc(s->node.work, n, false);
			vexec_bit_set(und, j);
		}
	s->node.child_redo = und;
	s->node.loaded_row = -1;
	s->rounds++;
}

/*
 * A round of join rows from the outer side: its rows in order, from one
 * outer batch, until the join rows would pass a batch, or an outer row must
 * wait for every row before it to be consumed -- one the vector child
 * still resolves, or whose keys PostgreSQL's evaluator computes.  False at
 * the outer side's end.
 */
static bool
probe_round(HjState *s)
{
	HjSide	   *out = &s->side[0];
	const int	limit = VEXEC_BATCH_ROWS;
	bool		stop_at_match = s->single_match || s->jointype == JOIN_ANTI;
	bool		full = false;

	s->c_n = 0;
	s->r_n = 0;
	s->jr_n = 0;

	while (!full)
	{
		HjBatch    *ob = s->ob;
		int			row;
		HjKeyVal   *okv;
		uint32		h;

		CHECK_FOR_INTERRUPTS();
		if (ob == NULL)
		{
			if (s->r_n > 0)
				break;			/* never two outer batches in a round */
			if (!outer_next(s))
				return false;
			continue;
		}

		if (s->inprog >= 0)
		{
			/* a row a round before left part way: a round begins with it */
			Assert(s->r_n == 0);
			row = s->inprog;
			s->jr_ob = ob;
			s->r_row[0] = row;
			s->r_first[0] = 0;
			s->r_count[0] = 0;
			s->r_complete[0] = false;
			s->r_n = 1;
			if (stop_at_match && ob->matched[row])
				s->inprog_next = 0;
		}
		else
		{
			row = vexec_bits_next(ob->rows, ob->nrows, s->opos);
			if (row < 0)
			{
				if (s->in_one)
				{
					s->in_one = false;
					s->ob = &out->cur;
					s->opos = s->cur_resume;
					if (s->r_n > 0)
						break;
				}
				else
					s->ob = NULL;
				continue;
			}
			if ((ob->child_dirty != NULL && vexec_bit(ob->child_dirty, row)) ||
				vexec_bit(ob->key_dirty, row))
			{
				/* it waits for every row before it to be consumed */
				if (s->r_n > 0)
					break;
				if (ob->child_dirty != NULL && vexec_bit(ob->child_dirty, row))
				{
					TupleTableSlot *slot = vexec_resolve_row((VexecNode *) out->ps, row);

					vexec_bit_clear(ob->child_dirty, row);
					if (slot == NULL)
					{
						vexec_bit_clear(ob->rows, row);
						s->opos = row + 1;
						continue;
					}
					side_one_from_slot(s, out, slot);
					vexec_bit_set(out->one.key_dirty, 0);
					s->cur_resume = row + 1;
					s->in_one = true;
					s->ob = &out->one;
					s->opos = 0;
					continue;
				}
				vexec_bit_clear(ob->key_dirty, row);
				side_load_row(out, ob, row);
				if (!row_keys(s, out, &ob->keys[row * s->nkeys], &ob->hashes[row], ob->keycxt))
					vexec_bit_set(ob->nullkey, row);
			}
			if (vexec_bit(ob->nullkey, row))
			{
				/* a NULL key meets no row: a left or anti join's, for its end */
				s->opos = row + 1;
				if (s->fill_outer)
					keep_null_row(s, out, batch_row_slot(out, ob, row));
				continue;
			}
			/* room for the row's own join rows: a null-extended one at least */
			if (s->r_n + 1 > limit || s->c_n + (s->fill_outer ? s->r_n + 1 : 0) > limit)
				break;
			s->opos = row + 1;
			s->outer_not_empty = true;
			s->inprog = row;
			s->inprog_next = s->heads[ob->hashes[row] & s->mask];
			if (s->r_n == 0)
				s->jr_ob = ob;	/* the round's rows are this batch's */
			s->r_row[s->r_n] = row;
			s->r_first[s->r_n] = s->c_n;
			s->r_count[s->r_n] = 0;
			s->r_complete[s->r_n] = false;
			s->r_n++;
		}

		/* the row's bucket: the rows of its hash, compared as far as can be */
		okv = &ob->keys[row * s->nkeys];
		h = ob->hashes[row];
		while (s->inprog_next != 0)
		{
			uint32		rid = s->inprog_next - 1;
			HjChunk    *ch = s->chunks[rid >> HJ_CHUNK_SHIFT];
			int			cr = rid & (HJ_CHUNK_ROWS - 1);
			uint8		st;

			if (s->c_n + 1 + (s->fill_outer ? s->r_n : 0) > limit)
			{
				full = true;
				break;
			}
			s->inprog_next = ch->next[cr];
			if (ch->hashes[cr] != h)
				continue;
			if (s->simple)
			{
				if (!keys_equal(s, okv, &ch->keys[cr * s->nkeys]))
					continue;
				st = s->nl1 == 0 ? HJ_C_MATCH : HJ_C_EVAL;
			}
			else
				st = HJ_C_EVAL;
			s->c_outer[s->c_n] = row;
			s->c_rid[s->c_n] = rid;
			s->c_status[s->c_n] = st;
			s->c_n++;
			s->r_count[s->r_n - 1]++;
			if (st == HJ_C_MATCH && stop_at_match)
				s->inprog_next = 0;
		}
		if (full)
			break;
		s->r_complete[s->r_n - 1] = true;
		s->inprog = -1;
	}

	Assert(s->r_n > 0);
	s->jr_src = HJ_SRC_TABLE;
	first_level(s);
	assign(s);
	if (s->jr_n > 0)
		make_join_rows(s);
	return true;
}

/* A right join's table rows that met no row, null-extended. */
static bool
fill_inner_round(HjState *s)
{
	s->jr_n = 0;
	s->jr_ob = NULL;
	s->jr_src = HJ_SRC_TABLE;
	while (s->jr_n < VEXEC_BATCH_ROWS && s->fill_rid < s->nrows)
	{
		uint32		rid = (uint32) s->fill_rid++;
		HjChunk    *ch = s->chunks[rid >> HJ_CHUNK_SHIFT];

		if (!vexec_bit(ch->matched, rid & (HJ_CHUNK_ROWS - 1)))
			add_join_row(s, HJ_JR_INNER, -1, (int) rid + 1, 0);
	}
	if (s->jr_n == 0)
		return false;
	make_join_rows(s);
	return true;
}

/* A side's rows of a NULL key, kept for the end, null-extended. */
static bool
null_round(HjState *s, HjSide *side)
{
	VexecBatch *b = side->rowbatch;
	HjBatch    *hb = &side->cur;

	if (side->nullrows == NULL)
		return false;
	vexec_batch_reset(b);
	vexec_batch_begin_rows(b);
	while (b->nrows < VEXEC_BATCH_ROWS &&
		   tuplestore_gettupleslot(side->nullrows, true, false, side->nullslot))
	{
		slot_getallattrs(side->nullslot);
		vexec_batch_add_values(b, side->nullslot->tts_values, side->nullslot->tts_isnull);
	}
	if (b->nrows == 0)
		return false;
	if (!side->inner)
		vexec_batch_apply_config(b, &s->node.layout);
	hjbatch_begin(side, hb, b, b->nrows, true);
	s->jr_n = 0;
	for (int r = 0; r < b->nrows; r++)
	{
		if (side->inner)
			add_join_row(s, HJ_JR_INNER, -1, r + 1, 0);
		else
			add_join_row(s, HJ_JR_FILL, r, 0, 0);
	}
	if (side->inner)
	{
		s->jr_ob = NULL;
		s->jr_src = HJ_SRC_BATCH;
		s->jr_ib = hb;
	}
	else
	{
		s->jr_ob = hb;
		s->jr_src = HJ_SRC_NONE;
	}
	make_join_rows(s);
	return true;
}

/* The next partition of the grace join, its table made: false at the end. */
static bool
next_partition(HjState *s)
{
	for (;;)
	{
		HjPart	   *part;

		if (s->curpart != NULL)
		{
			part_close(s->curpart);
			pfree(s->curpart);
			s->curpart = NULL;
		}
		if (s->pending == NIL)
			return false;
		part = linitial(s->pending);
		s->pending = list_delete_first(s->pending);
		s->curpart = part;

		/* a side with no rows: nothing to join, nothing to keep */
		if (part->inner == NULL && !s->fill_outer)
			continue;
		if (part->outer == NULL && !s->fill_inner)
			continue;
		if (!load_partition(s, part))
		{
			/* split: its parts are queued, it is gone */
			pfree(s->curpart);
			s->curpart = NULL;
			continue;
		}
		s->ob = NULL;
		s->opos = 0;
		s->in_one = false;
		s->inprog = -1;
		s->fill_rid = 0;
		return true;
	}
}

/*
 * A batch of the join ends -- its outer rows read, a right join's
 * unmatched inner rows given -- where PostgreSQL's ExecHashJoinNewBatch()
 * begins.  The port's tests interrupt a hash join there: Cloudberry's
 * exec_hashjoin_new_batch fault, at an injection point of the port's test
 * builds (pg19/docker/patches/test-workfile-points.patch).  Without one
 * attached it does nothing.
 */
static void
batch_ends(void)
{
	INJECTION_POINT("exec-hashjoin-new-batch", NULL);
}

/* What follows the outer side's rows, or a partition's. */
static HjPhase
after_probe(HjState *s)
{
	if (s->fill_inner)
		return HJ_FILL_INNER;
	batch_ends();
	if (s->spilled)
		return HJ_NEXT_PART;
	return s->fill_outer ? HJ_NULL_OUTER : HJ_DONE;
}

static HjPhase
after_fill(HjState *s)
{
	batch_ends();
	if (s->spilled)
		return HJ_NEXT_PART;
	return s->fill_inner ? HJ_NULL_INNER : HJ_DONE;
}

/*
 * The node's fetch: the next batch of join rows into node->in, the rows
 * still to be decided in node->child_redo.  False at the join's end.
 */
static bool
hj_fetch(VexecNode *node)
{
	HjState    *s = (HjState *) node;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		switch (s->phase)
		{
			case HJ_BUILD:
				if (!build_phase(s))
				{
					s->phase = HJ_DONE;
					return false;
				}
				if (s->spilled)
				{
					spill_outer(s);
					s->phase = HJ_NEXT_PART;
				}
				else
					s->phase = HJ_PROBE;
				continue;
			case HJ_PROBE:
				if (probe_round(s))
				{
					if (s->jr_n > 0)
						return true;
					continue;
				}
				s->phase = after_probe(s);
				s->fill_rid = 0;
				continue;
			case HJ_FILL_INNER:
				if (fill_inner_round(s))
					return true;
				s->phase = after_fill(s);
				continue;
			case HJ_NEXT_PART:
				if (next_partition(s))
				{
					s->phase = HJ_PROBE;
					continue;
				}
				s->phase = s->fill_outer ? HJ_NULL_OUTER :
					s->fill_inner ? HJ_NULL_INNER : HJ_DONE;
				continue;
			case HJ_NULL_OUTER:
				if (null_round(s, &s->side[0]))
					return true;
				if (s->side[0].nullrows != NULL)
				{
					/* PostgreSQL drops these once emitted (nodeHashjoin.c:719) */
					tuplestore_end(s->side[0].nullrows);
					s->side[0].nullrows = NULL;
				}
				s->phase = s->fill_inner ? HJ_NULL_INNER : HJ_DONE;
				continue;
			case HJ_NULL_INNER:
				if (null_round(s, &s->side[1]))
					return true;
				s->phase = HJ_DONE;
				continue;
			case HJ_DONE:
				return false;
		}
	}
}

/*
 * A join row still to be decided, reached by its consumer: the first level
 * by PostgreSQL's evaluator where it is a pair not yet known, in the order
 * the rows before it left -- a semi, anti or inner-unique join's outer row
 * that has matched takes no more pairs, and a null-extended row comes out
 * only where no pair of its row matched.  The join row, in the scan slot,
 * or NULL.
 */
static TupleTableSlot *
hj_resolve(VexecNode *node, int row)
{
	HjState    *s = (HjState *) node;
	HjBatch    *ob = s->jr_ob;
	int			oi = s->jr_outer[row];
	ExprContext *econtext = node->css.ss.ps.ps_ExprContext;

	vexec_node_load_input(node, row);
	switch (s->jr_kind[row])
	{
		case HJ_JR_PAIR:
			if ((s->single_match || s->jointype == JOIN_ANTI) && ob->matched[oi])
				return NULL;
			if (!(s->jr_flags[row] & HJ_JR_KNOWN))
			{
				econtext->ecxt_scantuple = node->input_slot;
				if ((!s->simple && !ExecQual(s->hashclauses_state, econtext)) ||
					!ExecQual(s->joinqual_state, econtext))
				{
					s->join_filtered++;
					return NULL;
				}
			}
			ob->matched[oi] = 1;
			if (s->fill_inner)
				mark_matched(s, (uint32) (s->jr_inner[row] - 1));
			if (s->jointype == JOIN_ANTI)
				return NULL;
			return node->input_slot;
		case HJ_JR_FILL:
			return ob->matched[oi] ? NULL : node->input_slot;
		default:
			return node->input_slot;
	}
}

static TupleTableSlot *
hj_exec(CustomScanState *css)
{
	return vexec_node_exec(&((HjState *) css)->node);
}

/* ---------------------------------------------------------------------
 * Begin, rescan, end
 * ---------------------------------------------------------------------
 */

/* Programs over some scan tuple columns, which a helper node's batch holds. */
static void
compile_over(HjState *s, VexecNode *helper, List *exprs, bool is_qual, VexecTop *tops)
{
	VexecCompileContext cc;
	int			base = -FirstLowInvalidHeapAttributeNumber;
	int			i;
	ListCell   *lc;

	memset(&cc, 0, sizeof(cc));
	cc.parent = &s->node.css.ss.ps;
	cc.input_varno = INDEX_VAR;
	cc.attno_base = base;
	cc.attno_max = s->nscan;
	cc.mcxt = s->node.mcxt;
	cc.attno_col = palloc(sizeof(int) * (base + s->nscan + 1));
	for (i = 0; i < base + s->nscan + 1; i++)
		cc.attno_col[i] = -1;
	for (i = 0; i < helper->ninput; i++)
		cc.attno_col[helper->input_attnos[i] + base] = i;
	i = 0;
	foreach(lc, exprs)
		vexec_compile_top(&cc, lfirst(lc), is_qual, &tops[i++]);
	helper->nkernels += cc.nkernels;
	helper->nfallbacks += cc.nfallbacks;
	helper->declared = list_concat(helper->declared, cc.declared);
}

/* A node of the helper kind: a batch's columns, and a slot of the scan tuple. */
static VexecNode *
helper_node(HjState *s, EState *estate, int ncols, const AttrNumber *attnos)
{
	VexecNode  *h = MemoryContextAllocZero(s->node.mcxt, sizeof(VexecNode));

	h->kind = VEXEC_NODE_HASHJOIN;
	h->mcxt = s->node.mcxt;
	h->layout = s->node.layout;
	h->work = vexec_batch_create(s->node.mcxt, 0, NULL);
	h->input_varno = INDEX_VAR;
	h->ninput = ncols;
	h->input_attnos = palloc(sizeof(AttrNumber) * Max(ncols, 1));
	memcpy(h->input_attnos, attnos, sizeof(AttrNumber) * ncols);
	h->input_slot = ExecInitExtraTupleSlot(estate, s->scandesc, &TTSOpsVirtual);
	h->loaded_row = -1;
	h->eager_econtext = CreateExprContext(estate);
	h->eager_econtext->ecxt_scantuple = h->input_slot;
	return h;
}

/* The PARAM_EXEC ids an expression reads. */
static bool
param_ids_walker(Node *node, Bitmapset **ids)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param) && ((Param *) node)->paramkind == PARAM_EXEC)
		*ids = bms_add_member(*ids, ((Param *) node)->paramid);
	return expression_tree_walker(node, param_ids_walker, ids);
}

/* The attnos of the scan tuple an expression reads. */
static bool
index_attnos_walker(Node *node, Bitmapset **attnos)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == INDEX_VAR && var->varattno > 0)
			*attnos = bms_add_member(*attnos, var->varattno);
		return false;
	}
	return expression_tree_walker(node, index_attnos_walker, attnos);
}

static void
side_init(HjState *s, HjSide *side, PlanState *ps, int sidx, int first_attno, int ncols,
		  List *keyexprs, EState *estate)
{
	MemoryContext mcxt = s->node.mcxt;
	AttrNumber *attnos = palloc(sizeof(AttrNumber) * Max(ncols, 1));
	int			i;

	side->ps = ps;
	side->sidx = sidx;
	side->inner = sidx == 1;
	side->first_attno = first_attno;
	side->ncols = ncols;
	side->desc = ExecGetResultType(ps);
	if (side->desc->natts != ncols)
		elog(ERROR, "vexec: a VecHashJoin's %s side gives %d columns, its scan tuple %d",
			 side->inner ? "inner" : "outer", side->desc->natts, ncols);
	side->types = palloc(sizeof(VexecType *) * Max(ncols, 1));
	for (i = 0; i < ncols; i++)
	{
		Form_pg_attribute att = TupleDescAttr(side->desc, i);

		side->types[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		attnos[i] = first_attno + i;
	}
	side->batches = vexec_is_vector_state(ps) && vexec_node_batchable((VexecNode *) ps);
	side->read_ahead = side->inner || side->batches || vexec_plan_read_ahead_safe(ps->plan);
	side->rowbatch = vexec_batch_create(mcxt, ncols, side->types);
	side->onebatch = vexec_batch_create(mcxt, ncols, side->types);

	/* the keys' programs, over the side's columns as the scan tuple's */
	side->eval = helper_node(s, estate, ncols, attnos);
	side->keys = palloc0(sizeof(VexecTop) * Max(s->nkeys, 1));
	compile_over(s, side->eval, keyexprs, false, side->keys);
	for (i = 0; i < s->nkeys; i++)
		if (side->keys[i].eager == NULL)
			side->any_lazy_key = true;
	hjbatch_init(s, &side->cur);
	hjbatch_init(s, &side->one);

	/* the grace join's rows: the side's columns, its keys, the hash */
	side->spilldesc = CreateTemplateTupleDesc(ncols + s->nkeys + 1);
	for (i = 0; i < ncols; i++)
		TupleDescCopyEntry(side->spilldesc, i + 1, side->desc, i + 1);
	for (i = 0; i < s->nkeys; i++)
	{
		HjKey	   *k = &s->keys[i];
		Oid			typid = k->kind == HJ_KEY_INT || k->kind == HJ_KEY_BITS ? INT8OID :
			k->kind == HJ_KEY_BYTES ? BYTEAOID : k->type[sidx];

		TupleDescInitEntry(side->spilldesc, ncols + i + 1, NULL, typid, -1, 0);
	}
	TupleDescInitEntry(side->spilldesc, ncols + s->nkeys + 1, NULL, INT4OID, -1, 0);
	TupleDescFinalize(side->spilldesc);
	side->spillslot = ExecInitExtraTupleSlot(estate, side->spilldesc, &TTSOpsVirtual);
	side->readslot = ExecInitExtraTupleSlot(estate, side->spilldesc, &TTSOpsMinimalTuple);
	side->rowslot = ExecInitExtraTupleSlot(estate, side->desc, &TTSOpsVirtual);
	side->nullslot = ExecInitExtraTupleSlot(estate, side->desc, &TTSOpsMinimalTuple);
}

/*
 * Whether each hash clause is its keys' equality, "outer key op inner key"
 * with the key's own operator, so that comparing the keys decides it.
 */
static bool
clauses_are_keys(HjState *s, List *okeys, List *ikeys)
{
	int			k = 0;
	ListCell   *lc;

	if (list_length(s->hashclauses) != s->nkeys)
		return false;
	foreach(lc, s->hashclauses)
	{
		OpExpr	   *op = (OpExpr *) lfirst(lc);

		if (!IsA(op, OpExpr) || list_length(op->args) != 2 ||
			op->opno != s->keys[k].hashop || !s->keys[k].eq_pure ||
			!equal(linitial(op->args), list_nth(okeys, k)) ||
			!equal(lsecond(op->args), list_nth(ikeys, k)))
			return false;
		k++;
	}
	return true;
}

static void
hj_begin(CustomScanState *css, EState *estate, int eflags)
{
	HjState    *s = (HjState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	List	   *okeys;
	List	   *ikeys;
	List	   *l1exprs;
	Bitmapset  *l1attnos = NULL;
	AttrNumber *attnos;
	int			i;
	int			x;

	vexec_node_begin(node, estate);
	node->fetch = hj_fetch;
	node->resolve_input = hj_resolve;
	vexec_join_plan_decode(cscan, &s->plan);
	s->jointype = s->plan.jointype;
	switch (s->jointype)
	{
		case JOIN_INNER:
			node->label = "Vec Hash Join";
			break;
		case JOIN_LEFT:
			node->label = "Vec Hash Left Join";
			break;
		case JOIN_SEMI:
			node->label = "Vec Hash Semi Join";
			break;
		case JOIN_ANTI:
			node->label = "Vec Hash Anti Join";
			break;
		case JOIN_RIGHT:
			node->label = "Vec Hash Right Join";
			break;
		default:
			elog(ERROR, "vexec: a VecHashJoin of join type %d", (int) s->jointype);
	}
	s->fill_outer = s->jointype == JOIN_LEFT || s->jointype == JOIN_ANTI;
	s->fill_inner = s->jointype == JOIN_RIGHT;
	s->single_match = s->plan.inner_unique || s->jointype == JOIN_SEMI;

	/* the children: neither runs backward or marks (nodeHashjoin.c) */
	eflags &= ~(EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK);
	outerPlanState(css) = ExecInitNode(outerPlan(cscan), estate, eflags);
	innerPlanState(css) = ExecInitNode(innerPlan(cscan), estate, eflags);

	/* the scan tuple: a join row */
	s->scandesc = css->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	s->nscan = s->scandesc->natts;
	if (s->nscan != s->plan.nouter + s->plan.ninner)
		elog(ERROR, "vexec: a VecHashJoin's scan tuple of %d columns, its sides' %d and %d",
			 s->nscan, s->plan.nouter, s->plan.ninner);
	s->scantypes = palloc(sizeof(VexecType *) * Max(s->nscan, 1));
	node->input_attnos = palloc(sizeof(AttrNumber) * Max(s->nscan, 1));
	for (i = 0; i < s->nscan; i++)
	{
		Form_pg_attribute att = TupleDescAttr(s->scandesc, i);

		s->scantypes[i] = vexec_type_make(att->atttypid, att->atttypmod, att->attcollation);
		node->input_attnos[i] = i + 1;
	}
	node->input_varno = INDEX_VAR;
	node->ninput = s->nscan;
	node->input_slot = css->ss.ss_ScanTupleSlot;
	node->in = vexec_batch_create(node->mcxt, s->nscan, s->scantypes);
	vexec_node_compile(node, cscan->scan.plan.qual, cscan->scan.plan.targetlist);

	/* the hash clauses, the join quals, the keys */
	if (list_length(cscan->custom_exprs) != 4)
		elog(ERROR, "vexec: a VecHashJoin of %d custom expressions", list_length(cscan->custom_exprs));
	s->hashclauses = list_nth(cscan->custom_exprs, VEXEC_JOIN_HASHCLAUSES);
	s->joinqual = list_nth(cscan->custom_exprs, VEXEC_JOIN_JOINQUAL);
	okeys = list_nth(cscan->custom_exprs, VEXEC_JOIN_OUTERKEYS);
	ikeys = list_nth(cscan->custom_exprs, VEXEC_JOIN_INNERKEYS);
	s->nkeys = list_length(okeys);
	if (s->nkeys == 0 || list_length(ikeys) != s->nkeys ||
		list_length(s->plan.hashoperators) != s->nkeys ||
		list_length(s->plan.hashcollations) != s->nkeys)
		elog(ERROR, "vexec: a VecHashJoin of %d outer keys, %d inner keys, %d operators",
			 s->nkeys, list_length(ikeys), list_length(s->plan.hashoperators));
	s->keys = palloc0(sizeof(HjKey) * s->nkeys);
	for (i = 0; i < s->nkeys; i++)
	{
		HjKey	   *k = &s->keys[i];
		Oid			eqfn;

		k->hashop = list_nth_oid(s->plan.hashoperators, i);
		k->collation = list_nth_oid(s->plan.hashcollations, i);
		k->type[0] = exprType(list_nth(okeys, i));
		k->type[1] = exprType(list_nth(ikeys, i));
		get_typlenbyval(k->type[0], &k->typlen[0], &k->typbyval[0]);
		get_typlenbyval(k->type[1], &k->typlen[1], &k->typbyval[1]);
		if (!op_strict(k->hashop))
			elog(ERROR, "vexec: a VecHashJoin on hash operator %u, which is not strict", k->hashop);
		k->kind = key_kind(k);
		eqfn = get_opcode(k->hashop);
		k->eq_pure = k->kind != HJ_KEY_FMGR ||
			(func_volatile(eqfn) != PROVOLATILE_VOLATILE && get_func_leakproof(eqfn));
		if (k->kind == HJ_KEY_FMGR)
		{
			Oid			lhash;
			Oid			rhash;

			if (!get_op_hash_functions(k->hashop, &lhash, &rhash))
				elog(ERROR, "vexec: no hash functions for hash operator %u", k->hashop);
			fmgr_info(lhash, &k->hash[0]);
			fmgr_info(rhash, &k->hash[1]);
			fmgr_info(eqfn, &k->eq);
		}
	}
	s->simple = clauses_are_keys(s, okeys, ikeys);
	(void) param_ids_walker((Node *) ikeys, &s->inner_key_params);
	s->hashclauses_state = ExecInitQual(s->hashclauses, &css->ss.ps);
	s->joinqual_state = ExecInitQual(s->joinqual, &css->ss.ps);

	/* the sides */
	side_init(s, &s->side[0], outerPlanState(css), 0, 1, s->plan.nouter, okeys, estate);
	side_init(s, &s->side[1], innerPlanState(css), 1, s->plan.nouter + 1, s->plan.ninner,
			  ikeys, estate);

	/* the first level: the hash clauses the keys do not decide, the join quals */
	l1exprs = list_concat(s->simple ? NIL : list_copy(s->hashclauses), list_copy(s->joinqual));
	s->nl1 = list_length(l1exprs);
	(void) index_attnos_walker((Node *) l1exprs, &l1attnos);
	attnos = palloc(sizeof(AttrNumber) * Max(bms_num_members(l1attnos), 1));
	i = 0;
	x = -1;
	while ((x = bms_next_member(l1attnos, x)) >= 0)
		attnos[i++] = x;
	s->match = helper_node(s, estate, i, attnos);
	s->l1 = palloc0(sizeof(VexecTop) * Max(s->nl1, 1));
	compile_over(s, s->match, l1exprs, true, s->l1);
	s->l1_first_lazy = s->nl1;
	for (i = 0; i < s->nl1; i++)
		if (s->l1[i].eager == NULL)
		{
			s->l1_first_lazy = i;
			break;
		}
	/* EXPLAIN's steps: the keys' and the join quals' programs too */
	node->nkernels += s->match->nkernels + s->side[0].eval->nkernels + s->side[1].eval->nkernels;
	node->nfallbacks += s->match->nfallbacks + s->side[0].eval->nfallbacks +
		s->side[1].eval->nfallbacks;
	node->declared = list_concat(list_concat(list_concat(node->declared, s->match->declared),
											 s->side[0].eval->declared),
								 s->side[1].eval->declared);
	{
		VexecType **types = palloc(sizeof(VexecType *) * Max(s->match->ninput, 1));

		for (i = 0; i < s->match->ninput; i++)
			types[i] = s->scantypes[s->match->input_attnos[i] - 1];
		s->candbatch = vexec_batch_create(node->mcxt, s->match->ninput, types);
	}

	/* the table, its columns in the PostgreSQL format, numerics as set */
	s->tablecxt = AllocSetContextCreate(node->mcxt, "VecHashJoin table", ALLOCSET_DEFAULT_SIZES);
	s->readcxt = AllocSetContextCreate(node->mcxt, "VecHashJoin files", ALLOCSET_DEFAULT_SIZES);
	s->memlimit = get_hash_memory_limit();
	s->store_shape = palloc(sizeof(VexecShape) * Max(s->plan.ninner, 1));
	s->store_uniform = palloc(sizeof(bool) * Max(s->plan.ninner, 1));
	for (i = 0; i < s->plan.ninner; i++)
	{
		VexecLayoutConfig cfg = {VEXEC_FORMAT_POSTGRES, VEXEC_VARLENA_DATUM, VEXEC_BOOL_BYTE,
		VEXEC_TEMPORAL_POSTGRES, node->layout.numeric};

		vexec_type_shape(s->side[1].types[i], &cfg, &s->store_shape[i]);
		s->store_uniform[i] = true;
	}

	/* a round's arrays */
	s->c_outer = palloc(sizeof(int) * VEXEC_BATCH_ROWS);
	s->c_rid = palloc(sizeof(uint32) * VEXEC_BATCH_ROWS);
	s->c_status = palloc(sizeof(uint8) * VEXEC_BATCH_ROWS);
	s->c_idx = palloc(sizeof(int) * VEXEC_BATCH_ROWS);
	s->r_row = palloc(sizeof(int) * VEXEC_BATCH_ROWS);
	s->r_first = palloc(sizeof(int) * VEXEC_BATCH_ROWS);
	s->r_count = palloc(sizeof(int) * VEXEC_BATCH_ROWS);
	s->r_complete = palloc(sizeof(bool) * VEXEC_BATCH_ROWS);
	s->jr_outer = palloc(sizeof(int) * VEXEC_BATCH_ROWS);
	s->jr_inner = palloc(sizeof(int) * VEXEC_BATCH_ROWS);
	s->jr_kind = palloc(sizeof(uint8) * VEXEC_BATCH_ROWS);
	s->jr_flags = palloc(sizeof(uint8) * VEXEC_BATCH_ROWS);
	s->jr_tmp = palloc(sizeof(int) * VEXEC_BATCH_ROWS);

	s->phase = HJ_BUILD;
	s->inprog = -1;
}

static void
hj_rescan(CustomScanState *css)
{
	HjState    *s = (HjState *) css;
	PlanState  *outer = outerPlanState(css);
	PlanState  *inner = innerPlanState(css);

	/*
	 * The table again where it is all in memory and neither the inner side's
	 * parameters nor its keys' changed, its rows' matches forgotten
	 * (ExecReScanHashJoin, whose Hash node's chgParam holds both); else made
	 * again.
	 */
	if (s->built && !s->spilled && inner->chgParam == NULL &&
		!bms_overlap(css->ss.ps.chgParam, s->inner_key_params))
	{
		if (s->fill_inner)
			for (int c = 0; c < s->nchunks; c++)
				memset(s->chunks[c]->matched, 0, sizeof(s->chunks[c]->matched));
		if (s->side[1].nullrows != NULL)
			tuplestore_rescan(s->side[1].nullrows);
		s->outer_not_empty = false;
		s->phase = HJ_PROBE;
	}
	else
	{
		table_reset(s);
		spill_close_all(s);
		s->spilled = false;
		s->built = false;
		if (s->side[1].nullrows != NULL)
		{
			tuplestore_end(s->side[1].nullrows);
			s->side[1].nullrows = NULL;
		}
		s->phase = HJ_BUILD;
		if (s->side[1].touched && inner->chgParam == NULL)
			ExecReScan(inner);
		s->side[1].done = false;
		s->side[1].touched = false;
	}
	if (s->side[0].nullrows != NULL)
	{
		tuplestore_end(s->side[0].nullrows);
		s->side[0].nullrows = NULL;
	}
	s->ob = NULL;
	s->opos = 0;
	s->in_one = false;
	s->inprog = -1;
	s->fill_rid = 0;
	s->jr_n = 0;
	s->side[0].done = false;
	if (outer->chgParam == NULL)
		ExecReScan(outer);
	vexec_node_rescan(&s->node);
}

static void
hj_end(CustomScanState *css)
{
	HjState    *s = (HjState *) css;

	spill_close_all(s);
	for (int i = 0; i < 2; i++)
	{
		if (s->side[i].nullrows != NULL)
			tuplestore_end(s->side[i].nullrows);
		s->side[i].nullrows = NULL;
		if (s->side[i].eval != NULL && s->side[i].eval->eager_econtext != NULL)
			FreeExprContext(s->side[i].eval->eager_econtext, true);
	}
	if (s->match != NULL && s->match->eager_econtext != NULL)
		FreeExprContext(s->match->eager_econtext, true);
	ExecEndNode(outerPlanState(css));
	ExecEndNode(innerPlanState(css));
	vexec_node_end(&s->node);
}

/* ---------------------------------------------------------------------
 * EXPLAIN
 * ---------------------------------------------------------------------
 */

static const char *
side_input(HjSide *side)
{
	if (side->batches)
		return "batches";
	return side->read_ahead ? "rows" : "rows, one at a time";
}

static const char *
key_kind_name(HjKeyKind kind)
{
	switch (kind)
	{
		case HJ_KEY_INT:
			return "integers";
		case HJ_KEY_BITS:
			return "bits";
		case HJ_KEY_BYTES:
			return "bytes";
		default:
			return "the operator's functions";
	}
}

static void
hj_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	HjState    *s = (HjState *) css;
	List	   *context;
	bool		useprefix = list_length(es->rtable) > 1 || es->verbose;

	vexec_node_relabel(&s->node, es);
	context = set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan, ancestors);
	if (s->hashclauses != NIL)
		ExplainPropertyText("Hash Cond",
							deparse_expression((Node *) make_ands_explicit(s->hashclauses),
											   context, useprefix, false), es);
	if (s->joinqual != NIL)
		ExplainPropertyText("Join Filter",
							deparse_expression((Node *) make_ands_explicit(s->joinqual),
											   context, useprefix, false), es);
	vexec_node_explain_properties(&s->node, ancestors, es);
	if (es->verbose)
	{
		StringInfoData buf;

		initStringInfo(&buf);
		for (int k = 0; k < s->nkeys; k++)
			appendStringInfo(&buf, "%s%s", k > 0 ? ", " : "", key_kind_name(s->keys[k].kind));
		ExplainPropertyText("Hash Keys Compared As", buf.data, es);
		if (s->nl1 > 0)
			ExplainPropertyText("Vector Join Quals",
								psprintf("%d of %d", s->l1_first_lazy, s->nl1), es);
		ExplainPropertyText("Outer Input", side_input(&s->side[0]), es);
		ExplainPropertyText("Inner Input", side_input(&s->side[1]), es);
	}
	/*
	 * the sides it left unread: its own, or where the segments ran it, theirs
	 * (motion/gpcore.c)
	 */
	if (es->analyze && (s->node.ran || s->node.segments_seen > 0))
	{
		const VexecNodeStats *st = s->node.ran ? &s->node.stats : &s->node.segment_stats;

		if (st->drained_motions > 0)
		{
			ExplainPropertyInteger("Drained Motions", NULL, st->drained_motions, es);
			ExplainPropertyInteger("Drained Rows", NULL, st->drained_rows, es);
		}
		if (st->squelched > 0)
			ExplainPropertyInteger("Squelched", NULL, st->squelched, es);
	}
	if (es->analyze && s->node.ran)
	{
		if (s->nl1 > 0)
			ExplainPropertyInteger("Rows Removed by Join Filter", NULL, s->join_filtered, es);
		ExplainPropertyInteger("Inner Rows", NULL, s->inner_rows, es);
		ExplainPropertyInteger("Memory Usage", "kB", (int64) ((s->peak_memory + 1023) / 1024), es);
		if (s->spilled_parts > 0)
		{
			ExplainPropertyInteger("Spilled Partitions", NULL, s->spilled_parts, es);
			ExplainPropertyInteger("Spilled Rows", NULL, s->spilled_rows, es);
			ExplainPropertyInteger("Spill Depth", NULL, s->max_depth, es);
		}
		if (es->verbose)
		{
			ExplainPropertyInteger("Rounds", NULL, s->rounds, es);
			if (s->outer_rows_one > 0)
				ExplainPropertyInteger("Outer Rows One at a Time", NULL, s->outer_rows_one, es);
		}
	}
}
