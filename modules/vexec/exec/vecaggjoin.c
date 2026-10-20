/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecaggjoin.c
 *	  VecAggJoin: the groups of two aggregations of the same rows by the
 *	  same keys, joined by their keys (pg_vector_executor.md §3.14, H3;
 *	  ch_table_engines.md's "Optimization for Clickbench", Q9).
 *
 * A grouping with one count(DISTINCT x) beside other aggregates runs X1's
 * stages, whose lower ones group by the keys and x and carry the other
 * aggregates' partial states for each pair: Q9's sum, count and avg, for
 * 4.6 million (region, user) pairs where there are 400 regions.  Planned
 * apart (plan/agg.c, split_path()), the other aggregates group by the keys
 * alone -- a second pass over the input, its groups a few hundred -- and
 * the distinct count's stages carry no state but the pairs'.  The two
 * aggregations' groups meet here, each side's gathered: both group the
 * same rows by the same keys, so that each key is one group of each.
 *
 * The inner side's groups, the distinct count's, go into a hash table by
 * their keys, hashed and compared as the grouping's operators hash and
 * compare them, NULL keys meeting as GROUP BY puts them in one group; each
 * outer group finds its key's.  The join row -- the outer side's columns,
 * then the inner side's -- is the scan tuple that custom_scan_tlist
 * describes, which the target list and the qual, the HAVING, read as
 * INDEX_VAR.  The keys are the first columns of each side.
 *
 * A row node: no batch interface, its sides' groups as rows.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "cb_explain.h"

#include "vexec.h"
#include "exec/exec.h"

typedef struct AggJoinState
{
	CustomScanState css;		/* first */
	int			nouter;			/* the scan tuple's columns from the outer side */
	int			nkeys;
	FmgrInfo   *hash;			/* per key: the operator's hash function */
	FmgrInfo   *eq;				/* and its equality */
	Oid		   *collations;
	MemoryContext tablecxt;		/* the inner groups */
	MinimalTuple *rows;			/* the inner groups, a bucket's chained */
	uint32	   *rowhash;
	int		   *next;
	int			nrows;
	int			maxrows;
	int		   *buckets;
	int			nbuckets;
	bool		built;
	TupleTableSlot *inner_slot;
	int64		joined;			/* EXPLAIN ANALYZE's */
} AggJoinState;

static void aggjoin_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *aggjoin_exec(CustomScanState *css);
static void aggjoin_end(CustomScanState *css);
static void aggjoin_rescan(CustomScanState *css);
static void aggjoin_explain(CustomScanState *css, List *ancestors, ExplainState *es);

static const CustomExecMethods aggjoin_exec_methods = {
	.CustomName = VEXEC_AGGJOIN_NAME,
	.BeginCustomScan = aggjoin_begin,
	.ExecCustomScan = aggjoin_exec,
	.EndCustomScan = aggjoin_end,
	.ReScanCustomScan = aggjoin_rescan,
	.ExplainCustomScan = aggjoin_explain,
};

Node *
vexec_create_aggjoin_state(CustomScan *cscan)
{
	AggJoinState *s = palloc0(sizeof(AggJoinState));

	(void) cscan;
	NodeSetTag(s, T_CustomScanState);
	s->css.methods = &aggjoin_exec_methods;
	return (Node *) s;
}

static void
aggjoin_begin(CustomScanState *css, EState *estate, int eflags)
{
	AggJoinState *s = (AggJoinState *) css;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	List	   *priv = cscan->custom_private;
	List	   *eqops;
	List	   *collations;

	if (list_length(priv) != 4)
		elog(ERROR, "vexec: a VecAggJoin's plan of %d parts", list_length(priv));
	s->nouter = intVal(linitial(priv));
	s->nkeys = intVal(lsecond(priv));
	eqops = lthird(priv);
	collations = lfourth(priv);
	outerPlanState(css) = ExecInitNode(outerPlan(cscan), estate, eflags);
	innerPlanState(css) = ExecInitNode(innerPlan(cscan), estate, eflags);
	s->hash = palloc0(sizeof(FmgrInfo) * Max(s->nkeys, 1));
	s->eq = palloc0(sizeof(FmgrInfo) * Max(s->nkeys, 1));
	s->collations = palloc0(sizeof(Oid) * Max(s->nkeys, 1));
	for (int i = 0; i < s->nkeys; i++)
	{
		Oid			eqop = list_nth_oid(eqops, i);
		Oid			lhash;
		Oid			rhash;

		if (!get_op_hash_functions(eqop, &lhash, &rhash))
			elog(ERROR, "vexec: a VecAggJoin key's operator %u has no hash function", eqop);
		fmgr_info(lhash, &s->hash[i]);
		fmgr_info(get_opcode(eqop), &s->eq[i]);
		s->collations[i] = list_nth_oid(collations, i);
	}
	s->tablecxt = AllocSetContextCreate(CurrentMemoryContext, "VecAggJoin groups",
										ALLOCSET_DEFAULT_SIZES);
	s->inner_slot = ExecInitExtraTupleSlot(estate, ExecGetResultType(innerPlanState(css)),
										   &TTSOpsMinimalTuple);
}

/* A group's keys' hash, the first nkeys columns of the slot: NULLs alike. */
static uint32
keys_hash(AggJoinState *s, TupleTableSlot *slot)
{
	uint32		h = 0;

	for (int i = 0; i < s->nkeys; i++)
	{
		h = pg_rotate_left32(h, 1);
		if (slot->tts_isnull[i])
			continue;
		h ^= DatumGetUInt32(FunctionCall1Coll(&s->hash[i], s->collations[i],
											  slot->tts_values[i]));
	}
	return murmurhash32(h);
}

/* Whether two groups' keys are equal, as GROUP BY takes them: NULLs equal. */
static bool
keys_equal(AggJoinState *s, TupleTableSlot *a, TupleTableSlot *b)
{
	for (int i = 0; i < s->nkeys; i++)
	{
		if (a->tts_isnull[i] != b->tts_isnull[i])
			return false;
		if (a->tts_isnull[i])
			continue;
		if (!DatumGetBool(FunctionCall2Coll(&s->eq[i], s->collations[i],
											a->tts_values[i], b->tts_values[i])))
			return false;
	}
	return true;
}

/* The inner side's groups into the table, a bucket a hash's low bits. */
static void
build_inner(AggJoinState *s)
{
	PlanState  *inner = innerPlanState(&s->css);
	MemoryContext old;
	int			b;

	MemoryContextReset(s->tablecxt);
	s->nrows = 0;
	s->maxrows = 1024;
	old = MemoryContextSwitchTo(s->tablecxt);
	s->rows = palloc(sizeof(MinimalTuple) * s->maxrows);
	s->rowhash = palloc(sizeof(uint32) * s->maxrows);
	MemoryContextSwitchTo(old);
	for (;;)
	{
		TupleTableSlot *slot = ExecProcNode(inner);

		if (TupIsNull(slot))
			break;
		slot_getallattrs(slot);
		old = MemoryContextSwitchTo(s->tablecxt);
		if (s->nrows == s->maxrows)
		{
			s->maxrows *= 2;
			s->rows = repalloc_huge(s->rows, sizeof(MinimalTuple) * s->maxrows);
			s->rowhash = repalloc_huge(s->rowhash, sizeof(uint32) * s->maxrows);
		}
		s->rowhash[s->nrows] = keys_hash(s, slot);
		s->rows[s->nrows++] = ExecCopySlotMinimalTuple(slot);
		MemoryContextSwitchTo(old);
		CHECK_FOR_INTERRUPTS();
	}
	old = MemoryContextSwitchTo(s->tablecxt);
	s->nbuckets = (int) pg_nextpower2_32((uint32) Max(s->nrows * 2, 16));
	s->buckets = palloc(sizeof(int) * s->nbuckets);
	s->next = palloc(sizeof(int) * Max(s->nrows, 1));
	MemoryContextSwitchTo(old);
	for (b = 0; b < s->nbuckets; b++)
		s->buckets[b] = -1;
	for (int i = 0; i < s->nrows; i++)
	{
		b = s->rowhash[i] & (s->nbuckets - 1);
		s->next[i] = s->buckets[b];
		s->buckets[b] = i;
	}
	s->built = true;
}

static TupleTableSlot *
aggjoin_exec(CustomScanState *css)
{
	AggJoinState *s = (AggJoinState *) css;
	ExprContext *econtext = css->ss.ps.ps_ExprContext;
	TupleTableSlot *scan = css->ss.ss_ScanTupleSlot;

	if (!s->built)
		build_inner(s);
	for (;;)
	{
		TupleTableSlot *outer;
		uint32		hash;
		int			i;
		int			ninner;

		ResetExprContext(econtext);
		outer = ExecProcNode(outerPlanState(css));
		if (TupIsNull(outer))
			return ExecClearTuple(css->ss.ps.ps_ResultTupleSlot);
		slot_getallattrs(outer);
		hash = keys_hash(s, outer);
		for (i = s->buckets[hash & (s->nbuckets - 1)]; i >= 0; i = s->next[i])
		{
			if (s->rowhash[i] != hash)
				continue;
			ExecStoreMinimalTuple(s->rows[i], s->inner_slot, false);
			slot_getallattrs(s->inner_slot);
			if (keys_equal(s, outer, s->inner_slot))
				break;
		}
		if (i < 0)
			elog(ERROR, "vexec: a group of the other aggregates with no group of the DISTINCT one");
		ninner = s->inner_slot->tts_tupleDescriptor->natts;
		ExecClearTuple(scan);
		memcpy(scan->tts_values, outer->tts_values, sizeof(Datum) * s->nouter);
		memcpy(scan->tts_isnull, outer->tts_isnull, sizeof(bool) * s->nouter);
		memcpy(scan->tts_values + s->nouter, s->inner_slot->tts_values, sizeof(Datum) * ninner);
		memcpy(scan->tts_isnull + s->nouter, s->inner_slot->tts_isnull, sizeof(bool) * ninner);
		ExecStoreVirtualTuple(scan);
		s->joined++;
		econtext->ecxt_scantuple = scan;
		if (css->ss.ps.qual != NULL && !ExecQual(css->ss.ps.qual, econtext))
		{
			InstrCountFiltered1(css, 1);
			continue;
		}
		if (css->ss.ps.ps_ProjInfo != NULL)
			return ExecProject(css->ss.ps.ps_ProjInfo);
		return ExecCopySlot(css->ss.ps.ps_ResultTupleSlot, scan);
	}
}

static void
aggjoin_rescan(CustomScanState *css)
{
	AggJoinState *s = (AggJoinState *) css;

	s->built = false;
	MemoryContextReset(s->tablecxt);
	if (outerPlanState(css)->chgParam == NULL)
		ExecReScan(outerPlanState(css));
	if (innerPlanState(css)->chgParam == NULL)
		ExecReScan(innerPlanState(css));
}

static void
aggjoin_end(CustomScanState *css)
{
	ExecEndNode(outerPlanState(css));
	ExecEndNode(innerPlanState(css));
}

static void
aggjoin_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	AggJoinState *s = (AggJoinState *) css;

	(void) ancestors;
	CbExplainRelabel(css, es, "Vec Aggregate Join", NULL);
	ExplainPropertyInteger("Join Keys", NULL, s->nkeys, es);
	if (es->analyze)
		ExplainPropertyInteger("Groups Joined", NULL, s->joined, es);
}
