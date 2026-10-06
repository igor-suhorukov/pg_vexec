/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * aggtrans.h
 *	  VecAgg's aggregates: PostgreSQL's transition, combine, serialization
 *	  and final functions, and the transitions vexec vectorizes
 *	  (pg_vector_executor.md §3.8).
 *
 * Every aggregate VecAgg runs is set up as nodeAgg.c sets it up
 * (PG19:src/backend/executor/nodeAgg.c:3746-4040, build_pertrans_for_aggref
 * :4131-4377): an AggStatePerTransData for each transition state, an
 * AggStatePerAggData for each aggregate, in an AggState of VecAgg's own that
 * is the functions' call context.  So AggCheckCallContext(), AggGetAggref(),
 * AggStateIsShared() and AggRegisterCallback() answer them as they answer
 * nodeAgg.c's: the group's memory, the Aggref, the callbacks of the group
 * table's context.
 *
 * A transition is either PostgreSQL's own, its function called through fmgr
 * a row at a time on the group's AggStatePerGroupData, with nodeAgg.c's
 * rules for strict functions, NULL initial values and by-reference states
 * (execExprInterp.c:2185-2300, 5690-5783); or one of the kinds below, whose
 * state VecAgg keeps in a layout of its own and advances a batch at a time.
 * A vectorized kind computes what its PostgreSQL function computes, bit for
 * bit and NULL for NULL, and hands PostgreSQL's state on where the state
 * leaves the node: to the final function, or serialized to the next stage.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_AGGTRANS_H
#define VEXEC_AGGTRANS_H

#include "executor/nodeAgg.h"
#include "lib/stringinfo.h"
#include "nodes/execnodes.h"

#include "batch/batch.h"

/*
 * The vectorized transitions, by the transition function they compute
 * (pg_aggregate.dat).  Only the first stage, which reads rows, has them;
 * a stage that combines states calls the combine function through fmgr.
 */
typedef enum VexecAggKind
{
	VEXEC_AGG_FMGR,				/* PostgreSQL's transition function */
	VEXEC_AGG_COUNT_STAR,		/* int8inc: count(*) */
	VEXEC_AGG_COUNT,			/* int8inc_any: count(x) */
	VEXEC_AGG_SUM_INT,			/* int2_sum, int4_sum: an int8, NULL until a
								 * value */
	VEXEC_AGG_AVG_INT,			/* int2_avg_accum, int4_avg_accum: int8[2]
								 * {count, sum} */
	VEXEC_AGG_SUM_INT128,		/* int8_avg_accum: Int128AggState {N, sumX} */
	VEXEC_AGG_SUM_NUMERIC,		/* numeric_avg_accum: NumericAggState, no
								 * sumX2 */
	VEXEC_AGG_SUM_FLOAT,		/* float4pl, float8pl */
	VEXEC_AGG_ACCUM_FLOAT,		/* float4_accum, float8_accum: float8[3] {N,
								 * Sx, Sxx} */
	VEXEC_AGG_MINMAX,			/* larger and smaller over a by-value type */
	VEXEC_AGG_MINMAX_TEXT,		/* text_larger, text_smaller, by the
								 * collation */
	VEXEC_AGG_BOOL,				/* booland_statefunc, boolor_statefunc */
	VEXEC_AGG_SUM_OFFSET		/* int4_sum of x + k, an int2 x: SUM(x) and
								 * COUNT(x), and k * COUNT(x) added (H8) */
} VexecAggKind;

/* MINMAX: how the values compare. */
typedef enum VexecAggCmp
{
	VEXEC_CMP_INT,				/* signed integers: int2, int4, int8, date,
								 * time, timestamp(tz), money */
	VEXEC_CMP_UINT,				/* unsigned: oid */
	VEXEC_CMP_FLOAT4,			/* NaN above everything; a tie keeps the new
								 * value */
	VEXEC_CMP_FLOAT8
} VexecAggCmp;

typedef struct VexecAggTrans
{
	int			transno;		/* PostgreSQL's aggtransno */
	AggStatePerTrans pertrans;	/* in the AggState's array */
	Aggref	   *aggref;			/* the first aggregate of this state */
	VexecAggKind kind;
	const char *kindname;		/* for EXPLAIN */

	/* the kind's parameters */
	VexecAggCmp cmp;			/* MINMAX */
	bool		larger;			/* MINMAX, MINMAX_TEXT: max */
	bool		is_and;			/* BOOL: bool_and */
	bool		single;			/* SUM_FLOAT, ACCUM_FLOAT: a float4 input */
	int16		width;			/* the input's bytes: SUM_INT, AVG_INT,
								 * MINMAX */
	Oid			collation;		/* MINMAX_TEXT */
	int64		offset;			/* SUM_OFFSET: k */
	int			share;			/* SUM_OFFSET: an earlier transition whose
								 * state, over the same x, it reads; else -1 */
	bool		c_collation;	/* MINMAX_TEXT: the collation is C's */

	/*
	 * Whether it may raise (a float's overflow), and whether its result
	 * depends on the rows' order (a float's sums): such a kind is advanced
	 * in row order, interleaved with the other aggregates as nodeAgg.c
	 * would, wherever an error elsewhere could come first (vecagg.c).
	 */
	bool		may_raise;
	bool		order_dependent;

	/* its state in a group */
	Size		stateoff;
	Size		statesize;

	/* its inputs among the node's input targets */
	int			firstarg;
	int			nargs;
	int			filter;			/* its FILTER, or -1 */
} VexecAggTrans;

/*
 * The AggState the functions are called with: nodeAgg.c's fields that the
 * call-context functions and ExecAggCopyTransValue() read, and the arrays.
 */
extern AggState *vexec_agg_context_create(PlanState *parent, EState *estate,
										  AggSplit split, int naggs, int ntrans,
										  ExprContext *aggcontext,
										  ExprContext *tmpcontext);

/*
 * Set up the aggregates, as ExecInitAgg does: aggs are the node's Aggrefs in
 * executor form, each once, their aggno and aggtransno PostgreSQL's.  Fills
 * the AggState's peragg and pertrans arrays and returns the transitions,
 * *ntrans of them, in transno order, their kinds chosen for argtypes (the
 * input targets' types, from the arguments' first).
 */
extern VexecAggTrans *vexec_aggtrans_setup(AggState *aggstate, List *aggs,
										   EState *estate, int *ntrans);

/* A kind for the transition, from its function and its argument's type. */
extern void vexec_aggtrans_choose(VexecAggTrans *t, AggSplit split, Oid argtype,
								  int32 argtypmod);

/* A group's state for a transition: its initial value. */
extern void vexec_aggtrans_init(VexecAggTrans *t, AggState *aggstate, void *state);

/*
 * One row through a transition, its arguments' values given, its FILTER
 * already passed: the transition function, or the kind's own step.
 */
extern void vexec_aggtrans_advance(VexecAggTrans *t, AggState *aggstate, void *state,
								   const Datum *args, const bool *nulls);

/*
 * The rows of a batch through a vectorized transition, in row order: each
 * row of `rows` whose FILTER passed into groups[row]'s state at `off`, or
 * into `plain` when groups is NULL.  arg is the argument's column (NULL for
 * count(*)), filter the FILTER's, or NULL.
 */
extern void vexec_aggtrans_batch(VexecAggTrans *t, AggState *aggstate, VexecBatch *work,
								 char **groups, Size off, char *plain,
								 const uint64 *rows, int nrows,
								 VexecVec *arg, VexecVec *filter);

/*
 * An aggregate's result for a group, into the output tuple's memory: its
 * final function's, or, past a stage that skips it, its state, serialized
 * where the split asks.
 */
extern void vexec_aggtrans_result(VexecAggTrans *t, AggState *aggstate, int aggno,
								  void *state, Datum *value, bool *isnull);

/*
 * H2: the requests a source answers from the statistics of a unit of its
 * rows (vexec_source.h) for the transition, over the table's column attnum
 * of type argtype, into reqs[0..2]; how many, or -1 where its state cannot
 * be had exactly from them.  count(*) needs none: the unit's rows.
 */
extern int	vexec_aggtrans_stats_requests(const VexecAggTrans *t, Oid argtype,
										  AttrNumber attnum, VexecSourceAgg *reqs);

/*
 * H2: a unit's answers to those requests into a state, as the unit's rows,
 * nrows of them, would have advanced it one by one.
 */
extern void vexec_aggtrans_stats_apply(VexecAggTrans *t, AggState *aggstate, void *state,
									   const VexecSourceAggAnswer *answers, int64 nrows);

/* H8's SUM(x + k): over x, from the state of `share` where it is >= 0. */
extern void vexec_aggtrans_set_offset(VexecAggTrans *t, int64 offset, int share);

/* The transition's kind, for EXPLAIN. */
extern void vexec_aggtrans_name(VexecAggTrans *t, StringInfo buf);

/*
 * For the planners: whether an aggregate of this split over an argument of
 * this type has a vectorized transition, and its name.
 */
extern bool vexec_agg_vectorized(Oid aggfnoid, AggSplit split, Oid argtype,
								 const char **name);

#endif							/* VEXEC_AGGTRANS_H */
