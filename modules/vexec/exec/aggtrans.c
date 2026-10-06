/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * aggtrans.c
 *	  VecAgg's aggregates: set up as nodeAgg.c sets them up, advanced
 *	  through fmgr or by the vectorized transitions, finalized as nodeAgg.c
 *	  finalizes them (pg_vector_executor.md §3.8; aggtrans.h).
 *
 * The vectorized transitions, and what each keeps:
 *
 *	count(*), count(x)	an int64; int8inc's overflow error, which no
 *						table reaches, is kept
 *	sum(int2/int4)		an int64 that wraps as int4_sum's does, and
 *						whether a value came
 *	avg(int2/int4)		count and sum as int64s: int4_avg_accum's int8[2]
 *	sum/avg(int8)		an int128 and a count: int8_avg_accum's
 *						Int128AggState, which is internal to numeric.c and
 *						crosses as its serialization
 *	sum/avg(numeric)	an int128 at the largest display scale seen, the
 *						counts of NaN and the infinities, and a numeric for
 *						what 38 digits cannot hold: numeric_avg_accum's
 *						NumericAggState, which crosses likewise
 *	sum(float4/8)		the running sum, in row order, float4_pl's and
 *						float8_pl's overflow errors kept
 *	avg(float4/8) ...	float8_accum's Youngs-Cramer sums, in row order:
 *						every aggregate whose transition it is (avg,
 *						variance, stddev) gets it
 *	min, max			the value, compared as the type's larger and
 *						smaller functions compare, a tie keeping the new
 *						value as they do; text by its collation (varstr_cmp)
 *	bool_and, bool_or	a bool
 *
 * Where a state leaves the node it becomes PostgreSQL's -- the int8, the
 * array, or the internal state that the aggregate's own deserialization
 * function builds from the bytes its serialization function would have
 * written -- and PostgreSQL's final or serialization function takes it from
 * there.  So every final function that shares a transition is served, and
 * a row Agg, a Motion or a Gather may follow the stage.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "executor/execExpr.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "parser/parse_agg.h"
#include "parser/parse_coerce.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
#include "utils/float.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"
#include "utils/varlena.h"
#include "varatt.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/aggtrans.h"
#include "expr/expr.h"
#include "expr/kernel.h"

/* ---------------------------------------------------------------------
 * The vectorized kinds' states
 * ---------------------------------------------------------------------
 */

typedef struct CountState
{
	int64		n;
} CountState;

typedef struct SumIntState
{
	int64		sum;
	bool		any;			/* a value came: else the state is NULL */
} SumIntState;

typedef struct AvgIntState
{
	int64		count;
	int64		sum;
} AvgIntState;

typedef struct Int128State
{
	int128		sum;
	int64		n;
	bool		called;			/* int8_avg_accum ran: else the state is NULL */
} Int128State;

typedef struct NumericState
{
	int128		sum;			/* at accscale */
	int64		n;				/* finite values */
	int64		maxscalecount;
	int64		nan;
	int64		pinf;
	int64		ninf;
	int32		accscale;
	int32		maxscale;
	bool		called;
	Datum		overflow;		/* a numeric, in the group table's memory, of
								 * the values the int128 could not take; 0
								 * when none */
} NumericState;

typedef struct FloatSumState
{
	float8		sum;			/* a float4's sum is kept as a float4 */
	bool		any;
} FloatSumState;

typedef struct FloatAccumState
{
	float8		n;
	float8		sx;
	float8		sxx;
} FloatAccumState;

typedef struct MinMaxState
{
	Datum		value;			/* by value; MINMAX_TEXT: a text in the group
								 * table's memory */
	bool		any;
} MinMaxState;

typedef struct BoolState
{
	bool		value;
	bool		any;
} BoolState;

typedef struct OffsetState
{
	int64		sum;			/* of x, wrapping as int4_sum's sum wraps */
	int64		count;
} OffsetState;

/* ---------------------------------------------------------------------
 * The AggState the functions are called with
 * ---------------------------------------------------------------------
 */

AggState *
vexec_agg_context_create(PlanState *parent, EState *estate, AggSplit split,
						 int naggs, int ntrans, ExprContext *aggcontext,
						 ExprContext *tmpcontext)
{
	AggState   *aggstate = makeNode(AggState);

	aggstate->ss.ps.plan = parent->plan;
	aggstate->ss.ps.state = estate;
	aggstate->ss.ps.ps_ExprContext = parent->ps_ExprContext;
	aggstate->aggsplit = split;
	aggstate->numaggs = naggs;
	aggstate->numtrans = ntrans;
	aggstate->maxsets = 0;
	aggstate->current_set = 0;
	aggstate->peragg = palloc0_array(AggStatePerAggData, Max(naggs, 1));
	aggstate->pertrans = palloc0_array(AggStatePerTransData, Max(ntrans, 1));
	aggstate->curaggcontext = aggcontext;
	aggstate->hashcontext = aggcontext;
	aggstate->aggcontexts = palloc0_array(ExprContext *, 1);
	aggstate->aggcontexts[0] = aggcontext;
	aggstate->tmpcontext = tmpcontext;
	aggstate->curperagg = NULL;
	aggstate->curpertrans = NULL;
	return aggstate;
}

/* An initial value from its text, as nodeAgg.c's GetAggInitVal() reads it. */
static Datum
agg_init_value(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = TextDatumGetCString(textInitVal);
	initVal = OidInputFunctionCall(typinput, strInitVal, typioparam, -1);
	pfree(strInitVal);
	return initVal;
}

static void
check_execute(Oid funcid, Oid roleid)
{
	AclResult	aclresult;

	if (!OidIsValid(funcid))
		return;
	aclresult = object_aclcheck(ProcedureRelationId, funcid, roleid, ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION, get_func_name(funcid));
	InvokeFunctionExecuteHook(funcid);
}

/*
 * A transition's working state, as build_pertrans_for_aggref() fills it for
 * an aggregate with no ORDER BY and no DISTINCT
 * (PG19:src/backend/executor/nodeAgg.c:4131-4377).
 */
static void
setup_pertrans(AggStatePerTrans pertrans, AggState *aggstate, Aggref *aggref,
			   Oid transfn_oid, Oid aggtranstype, Oid serialfn_oid, Oid deserialfn_oid,
			   Datum initValue, bool initValueIsNull, Oid *inputTypes, int numArguments)
{
	Expr	   *transfnexpr;
	Expr	   *serialfnexpr = NULL;
	Expr	   *deserialfnexpr = NULL;
	int			numTransArgs;

	pertrans->aggref = aggref;
	pertrans->aggshared = false;
	pertrans->aggCollation = aggref->inputcollid;
	pertrans->transfn_oid = transfn_oid;
	pertrans->serialfn_oid = serialfn_oid;
	pertrans->deserialfn_oid = deserialfn_oid;
	pertrans->initValue = initValue;
	pertrans->initValueIsNull = initValueIsNull;
	pertrans->numInputs = list_length(aggref->args);
	pertrans->aggtranstype = aggtranstype;
	numTransArgs = pertrans->numTransInputs + 1;

	build_aggregate_transfn_expr(inputTypes, numArguments, 0, aggref->aggvariadic,
								 aggtranstype, aggref->inputcollid, transfn_oid,
								 InvalidOid, &transfnexpr, NULL);
	fmgr_info(transfn_oid, &pertrans->transfn);
	fmgr_info_set_expr((Node *) transfnexpr, &pertrans->transfn);
	pertrans->transfn_fcinfo = (FunctionCallInfo) palloc(SizeForFunctionCallInfo(numTransArgs));
	InitFunctionCallInfoData(*pertrans->transfn_fcinfo, &pertrans->transfn, numTransArgs,
							 pertrans->aggCollation, (Node *) aggstate, NULL);
	get_typlenbyval(aggtranstype, &pertrans->transtypeLen, &pertrans->transtypeByVal);

	if (OidIsValid(serialfn_oid))
	{
		build_aggregate_serialfn_expr(serialfn_oid, &serialfnexpr);
		fmgr_info(serialfn_oid, &pertrans->serialfn);
		fmgr_info_set_expr((Node *) serialfnexpr, &pertrans->serialfn);
		pertrans->serialfn_fcinfo = (FunctionCallInfo) palloc(SizeForFunctionCallInfo(1));
		InitFunctionCallInfoData(*pertrans->serialfn_fcinfo, &pertrans->serialfn, 1,
								 InvalidOid, (Node *) aggstate, NULL);
	}
	if (OidIsValid(deserialfn_oid))
	{
		build_aggregate_deserialfn_expr(deserialfn_oid, &deserialfnexpr);
		fmgr_info(deserialfn_oid, &pertrans->deserialfn);
		fmgr_info_set_expr((Node *) deserialfnexpr, &pertrans->deserialfn);
		pertrans->deserialfn_fcinfo = (FunctionCallInfo) palloc(SizeForFunctionCallInfo(2));
		InitFunctionCallInfoData(*pertrans->deserialfn_fcinfo, &pertrans->deserialfn, 2,
								 InvalidOid, (Node *) aggstate, NULL);
	}

	pertrans->numSortCols = 0;
	pertrans->numDistinctCols = 0;
	pertrans->aggsortrequired = false;
	pertrans->sortstates = palloc0_array(Tuplesortstate *, 1);
}

/*
 * The aggregates, as ExecInitAgg() sets them up (nodeAgg.c:3746-4040): the
 * permission checks, the functions, the initial values, the checks on
 * strictness, each transition once however many aggregates share it.
 */
VexecAggTrans *
vexec_aggtrans_setup(AggState *aggstate, List *aggs, EState *estate, int *ntrans)
{
	VexecAggTrans *trans = palloc0_array(VexecAggTrans, Max(aggstate->numtrans, 1));
	ListCell   *l;

	foreach(l, aggs)
	{
		Aggref	   *aggref = lfirst_node(Aggref, l);

		/*
		 * Each aggregate in its own split: one stage may run one aggregate
		 * whole and combine the states of others, as H3's upper stage does
		 * (plan/agg.c).  nodeAgg.c has one split a node.
		 */
		AggSplit	split = aggref->aggsplit;
		AggStatePerAgg peragg;
		AggStatePerTrans pertrans;
		Oid			inputTypes[FUNC_MAX_ARGS];
		int			numArguments;
		HeapTuple	aggTuple;
		Form_pg_aggregate aggform;
		AclResult	aclresult;
		Oid			finalfn_oid;
		Oid			serialfn_oid = InvalidOid;
		Oid			deserialfn_oid = InvalidOid;
		Oid			aggOwner;
		Oid			aggtranstype = aggref->aggtranstype;
		HeapTuple	procTuple;

		if (aggref->aggno < 0 || aggref->aggno >= aggstate->numaggs ||
			aggref->aggtransno < 0 || aggref->aggtransno >= aggstate->numtrans)
			elog(ERROR, "vexec: aggregate %d of transition %d out of range",
				 aggref->aggno, aggref->aggtransno);
		/* never an answer without the DISTINCT or the ORDER BY asked for */
		if (aggref->aggdistinct != NIL || aggref->aggorder != NIL ||
			aggref->aggkind != AGGKIND_NORMAL)
			elog(ERROR, "vexec: VecAgg was given an aggregate with DISTINCT, ORDER BY or WITHIN GROUP");
		peragg = &aggstate->peragg[aggref->aggno];
		if (peragg->aggref != NULL)
			continue;
		peragg->aggref = aggref;
		peragg->transno = aggref->aggtransno;

		aggTuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggref->aggfnoid));
		if (!HeapTupleIsValid(aggTuple))
			elog(ERROR, "cache lookup failed for aggregate %u", aggref->aggfnoid);
		aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

		aclresult = object_aclcheck(ProcedureRelationId, aggref->aggfnoid, GetUserId(),
									ACL_EXECUTE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error(aclresult, OBJECT_AGGREGATE, get_func_name(aggref->aggfnoid));
		InvokeFunctionExecuteHook(aggref->aggfnoid);

		if (DO_AGGSPLIT_SKIPFINAL(split))
			peragg->finalfn_oid = finalfn_oid = InvalidOid;
		else
			peragg->finalfn_oid = finalfn_oid = aggform->aggfinalfn;

		if (aggtranstype == INTERNALOID)
		{
			if (DO_AGGSPLIT_SERIALIZE(split))
			{
				if (!OidIsValid(aggform->aggserialfn))
					elog(ERROR, "serialfunc not provided for serialization aggregation");
				serialfn_oid = aggform->aggserialfn;
			}
			if (DO_AGGSPLIT_DESERIALIZE(split))
			{
				if (!OidIsValid(aggform->aggdeserialfn))
					elog(ERROR, "deserialfunc not provided for deserialization aggregation");
				deserialfn_oid = aggform->aggdeserialfn;
			}
		}

		procTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(aggref->aggfnoid));
		if (!HeapTupleIsValid(procTuple))
			elog(ERROR, "cache lookup failed for function %u", aggref->aggfnoid);
		aggOwner = ((Form_pg_proc) GETSTRUCT(procTuple))->proowner;
		ReleaseSysCache(procTuple);
		check_execute(finalfn_oid, aggOwner);
		check_execute(serialfn_oid, aggOwner);
		check_execute(deserialfn_oid, aggOwner);

		numArguments = get_aggregate_argtypes(aggref, inputTypes);
		peragg->numFinalArgs = aggform->aggfinalextra ? numArguments + 1 : 1;
		peragg->aggdirectargs = NIL;
		if (OidIsValid(finalfn_oid))
		{
			Expr	   *finalfnexpr;

			build_aggregate_finalfn_expr(inputTypes, peragg->numFinalArgs, aggtranstype,
										 aggref->aggtype, aggref->inputcollid, finalfn_oid,
										 &finalfnexpr);
			fmgr_info(finalfn_oid, &peragg->finalfn);
			fmgr_info_set_expr((Node *) finalfnexpr, &peragg->finalfn);
		}
		get_typlenbyval(aggref->aggtype, &peragg->resulttypeLen, &peragg->resulttypeByVal);

		pertrans = &aggstate->pertrans[aggref->aggtransno];
		if (pertrans->aggref == NULL)
		{
			Datum		textInitVal;
			Datum		initValue;
			bool		initValueIsNull;
			Oid			transfn_oid;
			VexecAggTrans *t = &trans[aggref->aggtransno];

			if (DO_AGGSPLIT_COMBINE(split))
			{
				transfn_oid = aggform->aggcombinefn;
				if (!OidIsValid(transfn_oid))
					elog(ERROR, "combinefn not set for aggregate function");
			}
			else
				transfn_oid = aggform->aggtransfn;
			check_execute(transfn_oid, aggOwner);

			textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple, Anum_pg_aggregate_agginitval,
										  &initValueIsNull);
			initValue = initValueIsNull ? (Datum) 0 : agg_init_value(textInitVal, aggtranstype);

			if (DO_AGGSPLIT_COMBINE(split))
			{
				Oid			combineFnInputTypes[] = {aggtranstype, aggtranstype};

				pertrans->numTransInputs = 1;
				setup_pertrans(pertrans, aggstate, aggref, transfn_oid, aggtranstype,
							   serialfn_oid, deserialfn_oid, initValue, initValueIsNull,
							   combineFnInputTypes, 2);
				if (pertrans->transfn.fn_strict && aggtranstype == INTERNALOID)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
							 errmsg("combine function with transition type %s must not be declared STRICT",
									format_type_be(aggtranstype))));
			}
			else
			{
				pertrans->numTransInputs = numArguments;
				setup_pertrans(pertrans, aggstate, aggref, transfn_oid, aggtranstype,
							   serialfn_oid, deserialfn_oid, initValue, initValueIsNull,
							   inputTypes, numArguments);
				if (pertrans->transfn.fn_strict && pertrans->initValueIsNull)
				{
					if (numArguments <= 0 ||
						!IsBinaryCoercible(inputTypes[0], aggtranstype))
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
								 errmsg("aggregate %u needs to have compatible input type and transition type",
										aggref->aggfnoid)));
				}
			}

			t->transno = aggref->aggtransno;
			t->pertrans = pertrans;
			t->aggref = aggref;
			t->share = -1;
			t->kind = VEXEC_AGG_FMGR;
			t->kindname = "fmgr";
			t->statesize = sizeof(AggStatePerGroupData);
			t->filter = -1;
			t->nargs = pertrans->numTransInputs;
		}
		else
			pertrans->aggshared = true;
		ReleaseSysCache(aggTuple);
	}
	*ntrans = aggstate->numtrans;
	return trans;
}

/* ---------------------------------------------------------------------
 * The kinds
 * ---------------------------------------------------------------------
 */

static void
set_kind(VexecAggTrans *t, VexecAggKind kind, const char *name, Size size)
{
	t->kind = kind;
	t->kindname = name;
	t->statesize = size;
}

/*
 * The vectorized transition for a transition function, where there is one
 * for its argument's type.  Only for a first stage: a combining stage reads
 * states, and calls the combine function.  An internal state whose stage
 * hands it on unserialized stays PostgreSQL's, since nothing but the
 * function's own memory can hold it.
 */
static void
choose_kind(VexecAggTrans *t, Oid fn, Oid transtype, Oid collation, AggSplit split,
			Oid argtype)
{
	if (DO_AGGSPLIT_COMBINE(split))
		return;
	if (transtype == INTERNALOID &&
		DO_AGGSPLIT_SKIPFINAL(split) && !DO_AGGSPLIT_SERIALIZE(split))
		return;

	switch (fn)
	{
		case F_INT8INC:
			set_kind(t, VEXEC_AGG_COUNT_STAR, "count(*)", sizeof(CountState));
			break;
		case F_INT8INC_ANY:
			set_kind(t, VEXEC_AGG_COUNT, "count", sizeof(CountState));
			break;
		case F_INT2_SUM:
		case F_INT4_SUM:
			if (argtype != (fn == F_INT2_SUM ? INT2OID : INT4OID))
				return;
			set_kind(t, VEXEC_AGG_SUM_INT, "sum", sizeof(SumIntState));
			t->width = fn == F_INT2_SUM ? 2 : 4;
			break;
		case F_INT2_AVG_ACCUM:
		case F_INT4_AVG_ACCUM:
			if (argtype != (fn == F_INT2_AVG_ACCUM ? INT2OID : INT4OID))
				return;
			set_kind(t, VEXEC_AGG_AVG_INT, "avg", sizeof(AvgIntState));
			t->width = fn == F_INT2_AVG_ACCUM ? 2 : 4;
			break;
		case F_INT8_AVG_ACCUM:
			if (argtype != INT8OID)
				return;
			set_kind(t, VEXEC_AGG_SUM_INT128, "int128 sum", sizeof(Int128State));
			break;
		case F_NUMERIC_AVG_ACCUM:
			if (argtype != NUMERICOID)
				return;
			set_kind(t, VEXEC_AGG_SUM_NUMERIC, "numeric sum", sizeof(NumericState));
			break;
		case F_FLOAT4PL:
		case F_FLOAT8PL:
			if (argtype != (fn == F_FLOAT4PL ? FLOAT4OID : FLOAT8OID))
				return;
			set_kind(t, VEXEC_AGG_SUM_FLOAT, "float sum", sizeof(FloatSumState));
			t->single = fn == F_FLOAT4PL;
			t->may_raise = true;
			t->order_dependent = true;
			break;
		case F_FLOAT4_ACCUM:
		case F_FLOAT8_ACCUM:
			if (argtype != (fn == F_FLOAT4_ACCUM ? FLOAT4OID : FLOAT8OID))
				return;
			set_kind(t, VEXEC_AGG_ACCUM_FLOAT, "float accum", sizeof(FloatAccumState));
			t->single = fn == F_FLOAT4_ACCUM;
			t->may_raise = true;
			t->order_dependent = true;
			break;
		case F_BOOLAND_STATEFUNC:
		case F_BOOLOR_STATEFUNC:
			if (argtype != BOOLOID)
				return;
			set_kind(t, VEXEC_AGG_BOOL, fn == F_BOOLAND_STATEFUNC ? "bool_and" : "bool_or",
					 sizeof(BoolState));
			t->is_and = fn == F_BOOLAND_STATEFUNC;
			break;
		case F_TEXT_LARGER:
		case F_TEXT_SMALLER:
			if (argtype != TEXTOID && argtype != VARCHAROID)
				return;
			set_kind(t, VEXEC_AGG_MINMAX_TEXT, fn == F_TEXT_LARGER ? "max" : "min",
					 sizeof(MinMaxState));
			t->larger = fn == F_TEXT_LARGER;
			t->collation = collation;
			t->c_collation = OidIsValid(collation) &&
				pg_newlocale_from_collation(collation)->collate_is_c;
			break;
		default:
			{
				/* larger and smaller over by-value types */
				static const struct
				{
					Oid			larger;
					Oid			smaller;
					Oid			type;
					VexecAggCmp cmp;
					int16		width;
				}			mm[] =
				{
					{F_INT2LARGER, F_INT2SMALLER, INT2OID, VEXEC_CMP_INT, 2},
					{F_INT4LARGER, F_INT4SMALLER, INT4OID, VEXEC_CMP_INT, 4},
					{F_INT8LARGER, F_INT8SMALLER, INT8OID, VEXEC_CMP_INT, 8},
					{F_OIDLARGER, F_OIDSMALLER, OIDOID, VEXEC_CMP_UINT, 4},
					{F_FLOAT4LARGER, F_FLOAT4SMALLER, FLOAT4OID, VEXEC_CMP_FLOAT4, 4},
					{F_FLOAT8LARGER, F_FLOAT8SMALLER, FLOAT8OID, VEXEC_CMP_FLOAT8, 8},
					{F_DATE_LARGER, F_DATE_SMALLER, DATEOID, VEXEC_CMP_INT, 4},
					{F_TIME_LARGER, F_TIME_SMALLER, TIMEOID, VEXEC_CMP_INT, 8},
					{F_TIMESTAMP_LARGER, F_TIMESTAMP_SMALLER, TIMESTAMPOID, VEXEC_CMP_INT, 8},
					{F_TIMESTAMPTZ_LARGER, F_TIMESTAMPTZ_SMALLER, TIMESTAMPTZOID, VEXEC_CMP_INT, 8},
					{F_CASHLARGER, F_CASHSMALLER, MONEYOID, VEXEC_CMP_INT, 8},
				};

				for (int i = 0; i < lengthof(mm); i++)
				{
					if ((fn == mm[i].larger || fn == mm[i].smaller) && argtype == mm[i].type)
					{
						set_kind(t, VEXEC_AGG_MINMAX, fn == mm[i].larger ? "max" : "min",
								 sizeof(MinMaxState));
						t->larger = fn == mm[i].larger;
						t->cmp = mm[i].cmp;
						t->width = mm[i].width;
						break;
					}
				}
			}
			break;
	}
}

void
vexec_aggtrans_choose(VexecAggTrans *t, AggSplit split, Oid argtype, int32 argtypmod)
{
	(void) argtypmod;
	choose_kind(t, t->pertrans->transfn_oid, t->pertrans->aggtranstype,
				t->pertrans->aggCollation, split, argtype);
}

bool
vexec_agg_vectorized(Oid aggfnoid, AggSplit split, Oid argtype, const char **name)
{
	VexecAggTrans t;
	HeapTuple	tup;
	Form_pg_aggregate aggform;

	memset(&t, 0, sizeof(t));
	t.kind = VEXEC_AGG_FMGR;
	t.kindname = "fmgr";
	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfnoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for aggregate %u", aggfnoid);
	aggform = (Form_pg_aggregate) GETSTRUCT(tup);
	choose_kind(&t, aggform->aggtransfn, aggform->aggtranstype, InvalidOid, split, argtype);
	ReleaseSysCache(tup);
	if (name)
		*name = t.kindname;
	return t.kind != VEXEC_AGG_FMGR;
}

/*
 * SUM(x + k), int4_sum over int2 x and an int4 constant k with which no x
 * overflows int4 (vecagg.c): its state is SUM(x) and COUNT(x), shared by
 * every such sum over the same x.  sum(x + k) over n values is sum(x) +
 * n * k, and the same bits where int4_sum's int64 wraps, since a sum modulo
 * 2^64 is the same however it is grouped.
 */
void
vexec_aggtrans_set_offset(VexecAggTrans *t, int64 offset, int share)
{
	t->offset = offset;
	t->share = share;
	set_kind(t, VEXEC_AGG_SUM_OFFSET, "sum of x + k", share >= 0 ? 0 : sizeof(OffsetState));
}

void
vexec_aggtrans_name(VexecAggTrans *t, StringInfo buf)
{
	appendStringInfoString(buf, t->kindname);
}

/* ---------------------------------------------------------------------
 * Groups' states: initial values
 * ---------------------------------------------------------------------
 */

void
vexec_aggtrans_init(VexecAggTrans *t, AggState *aggstate, void *state)
{
	if (t->kind == VEXEC_AGG_FMGR)
	{
		AggStatePerTrans pertrans = t->pertrans;
		AggStatePerGroup pergroup = (AggStatePerGroup) state;

		/* initialize_aggregate() (nodeAgg.c:581-660) */
		if (pertrans->initValueIsNull)
			pergroup->transValue = pertrans->initValue;
		else
		{
			MemoryContext old = MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);

			pergroup->transValue = datumCopy(pertrans->initValue, pertrans->transtypeByVal,
											 pertrans->transtypeLen);
			MemoryContextSwitchTo(old);
		}
		pergroup->transValueIsNull = pertrans->initValueIsNull;
		pergroup->noTransValue = pertrans->initValueIsNull;
		return;
	}
	memset(state, 0, t->statesize);
}

/* ---------------------------------------------------------------------
 * One row: through fmgr, or a kind's own step
 * ---------------------------------------------------------------------
 */

/*
 * A transition through fmgr, as the steps ExecBuildAggTrans() emits for an
 * aggregate without ORDER BY or DISTINCT run it (execExpr.c:3700-3960,
 * execExprInterp.c:2185-2300): the deserialization a combining stage asks
 * for, the strict input check, the first input as the state of a strict
 * function with no initial value, and the by-reference state copied into
 * the group's memory.
 */
static void
fmgr_advance(VexecAggTrans *t, AggState *aggstate, AggStatePerGroup pergroup,
			 const Datum *args, const bool *nulls)
{
	AggStatePerTrans pertrans = t->pertrans;
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	int			n = pertrans->numTransInputs;
	MemoryContext old;
	Datum		newVal;
	int			i;

	for (i = 0; i < n; i++)
	{
		fcinfo->args[i + 1].value = args[i];
		fcinfo->args[i + 1].isnull = nulls[i];
	}

	if (OidIsValid(pertrans->deserialfn_oid))
	{
		FunctionCallInfo ds = pertrans->deserialfn_fcinfo;

		if (pertrans->deserialfn.fn_strict && nulls[0])
			return;
		ds->args[0].value = args[0];
		ds->args[0].isnull = nulls[0];
		ds->args[1].value = PointerGetDatum(NULL);
		ds->args[1].isnull = false;
		ds->isnull = false;
		old = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
		fcinfo->args[1].value = FunctionCallInvoke(ds);
		fcinfo->args[1].isnull = ds->isnull;
		MemoryContextSwitchTo(old);
	}

	if (pertrans->transfn.fn_strict)
	{
		for (i = 0; i < n; i++)
			if (fcinfo->args[i + 1].isnull)
				return;
		if (pergroup->noTransValue)
		{
			/* ExecAggInitGroup(): the input becomes the state */
			old = MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);
			pergroup->transValue = datumCopy(fcinfo->args[1].value,
											 pertrans->transtypeByVal,
											 pertrans->transtypeLen);
			pergroup->transValueIsNull = false;
			pergroup->noTransValue = false;
			MemoryContextSwitchTo(old);
			return;
		}
		if (pergroup->transValueIsNull)
			return;
	}

	aggstate->curpertrans = pertrans;
	aggstate->current_set = 0;
	old = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
	fcinfo->args[0].value = pergroup->transValue;
	fcinfo->args[0].isnull = pergroup->transValueIsNull;
	fcinfo->isnull = false;
	newVal = FunctionCallInvoke(fcinfo);
	if (!pertrans->transtypeByVal &&
		DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
		newVal = ExecAggCopyTransValue(aggstate, pertrans, newVal, fcinfo->isnull,
									   pergroup->transValue, pergroup->transValueIsNull);
	pergroup->transValue = newVal;
	pergroup->transValueIsNull = fcinfo->isnull;
	MemoryContextSwitchTo(old);
	aggstate->curpertrans = NULL;
}

static inline int64
wrap_add64(int64 a, int64 b)
{
	return (int64) ((uint64) a + (uint64) b);
}

static void
count_one(CountState *s)
{
	if (unlikely(pg_add_s64_overflow(s->n, 1, &s->n)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
}

/* float8_accum()'s step (PG19:src/backend/utils/adt/float.c:3090-3168). */
static inline void
accum_float(FloatAccumState *s, float8 newval)
{
	float8		N = s->n;
	float8		Sx = s->sx;
	float8		Sxx = s->sxx;
	float8		tmp;

	N += 1.0;
	Sx += newval;
	if (s->n > 0.0)
	{
		tmp = newval * N - Sx;
		Sxx += tmp * tmp / (N * s->n);
		if (isinf(Sx) || isinf(Sxx))
		{
			if (!isinf(s->sx) && !isinf(newval))
				float_overflow_error();
			Sxx = get_float8_nan();
		}
	}
	else
	{
		if (isnan(newval) || isinf(newval))
			Sxx = get_float8_nan();
	}
	s->n = N;
	s->sx = Sx;
	s->sxx = Sxx;
}

static inline void
sum_float(VexecAggTrans *t, FloatSumState *s, float8 v)
{
	if (!s->any)
	{
		s->sum = v;
		s->any = true;
	}
	else if (t->single)
		s->sum = (float8) float4_pl((float4) s->sum, (float4) v);
	else
		s->sum = float8_pl(s->sum, v);
}

/* Whether the new value replaces the state: the type's larger or smaller. */
static inline bool
minmax_takes(const VexecAggTrans *t, Datum state, Datum value)
{
	switch (t->cmp)
	{
		case VEXEC_CMP_INT:
			{
				int64		a,
							b;

				if (t->width == 2)
				{
					a = DatumGetInt16(state);
					b = DatumGetInt16(value);
				}
				else if (t->width == 4)
				{
					a = DatumGetInt32(state);
					b = DatumGetInt32(value);
				}
				else
				{
					a = DatumGetInt64(state);
					b = DatumGetInt64(value);
				}
				/* larger keeps the state where state > new; smaller where < */
				return t->larger ? !(a > b) : !(a < b);
			}
		case VEXEC_CMP_UINT:
			{
				Oid			a = DatumGetObjectId(state);
				Oid			b = DatumGetObjectId(value);

				return t->larger ? !(a > b) : !(a < b);
			}
		case VEXEC_CMP_FLOAT4:
			{
				float4		a = DatumGetFloat4(state);
				float4		b = DatumGetFloat4(value);

				return t->larger ? !float4_gt(a, b) : !float4_lt(a, b);
			}
		case VEXEC_CMP_FLOAT8:
			{
				float8		a = DatumGetFloat8(state);
				float8		b = DatumGetFloat8(value);

				return t->larger ? !float8_gt(a, b) : !float8_lt(a, b);
			}
	}
	return false;
}

/* A text value's bytes, detoasted into the current context when it must be. */
static void
text_bytes(Datum d, const char **p, int *len)
{
	varlena    *vl = (varlena *) DatumGetPointer(d);

	if (VARATT_IS_EXTENDED(vl))
		vl = pg_detoast_datum_packed(vl);
	*p = VARDATA_ANY(vl);
	*len = VARSIZE_ANY_EXHDR(vl);
}

/*
 * text_larger() and text_smaller(): the state stays where it compares
 * above (below) the new value, which takes ties.  A tie between the same
 * bytes changes nothing to keep.
 */
static void
minmax_text(VexecAggTrans *t, AggState *aggstate, MinMaxState *s, const char *p, int len)
{
	const char *sp;
	int			slen;
	int			c;
	MemoryContext old;
	text	   *copy;

	if (s->any)
	{
		text_bytes(s->value, &sp, &slen);
		if (t->c_collation)
		{
			/* varstr_cmp() under C: the bytes, then the length */
			c = memcmp(sp, p, Min(slen, len));
			if (c == 0 && slen != len)
				c = slen < len ? -1 : 1;
		}
		else
			c = varstr_cmp(sp, slen, p, len, t->collation);
		if (t->larger ? c > 0 : c < 0)
			return;
		if (c == 0 && slen == len && memcmp(sp, p, len) == 0)
			return;
	}
	old = MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);
	copy = (text *) palloc(VARHDRSZ + len);
	SET_VARSIZE(copy, VARHDRSZ + len);
	memcpy(VARDATA(copy), p, len);
	MemoryContextSwitchTo(old);
	if (s->any)
		pfree(DatumGetPointer(s->value));
	s->value = PointerGetDatum(copy);
	s->any = true;
}

/* ---- numeric ---- */

static const int128 pow10_tab[] = {
	1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000,
	(int128) 10000000000LL, (int128) 100000000000LL, (int128) 1000000000000LL,
	(int128) 10000000000000LL, (int128) 100000000000000LL, (int128) 1000000000000000LL,
	(int128) 10000000000000000LL, (int128) 100000000000000000LL,
	(int128) 1000000000000000000LL,
};

/* 10^n, 0 <= n <= 38 */
static int128
pow10_128(int n)
{
	int128		r = 1;

	while (n > 18)
	{
		r *= pow10_tab[18];
		n -= 18;
	}
	return r * pow10_tab[n];
}

/* What the int128 cannot hold goes to the overflow numeric. */
static void
numeric_overflow_add(AggState *aggstate, NumericState *s, Datum num)
{
	MemoryContext old;
	Datum		sum;

	if (s->overflow == (Datum) 0)
		sum = num;
	else
		sum = DirectFunctionCall2(numeric_add, s->overflow, num);
	old = MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);
	sum = datumCopy(sum, false, -1);
	MemoryContextSwitchTo(old);
	if (s->overflow != (Datum) 0)
		pfree(DatumGetPointer(s->overflow));
	s->overflow = sum;
}

/* The int128 accumulator into the overflow numeric, emptied. */
static void
numeric_spill_acc(AggState *aggstate, NumericState *s)
{
	if (s->sum != 0)
		numeric_overflow_add(aggstate, s,
							 vexec_scaled_to_numeric(NULL, s->sum, s->accscale));
	s->sum = 0;
}

/* A finite value x / 10^scale into the accumulator. */
static void
numeric_add_scaled(AggState *aggstate, NumericState *s, int128 x, int scale)
{
	int128		r;

	if (scale > s->accscale)
	{
		/* the accumulator moves to the larger scale */
		if (s->sum != 0 &&
			__builtin_mul_overflow(s->sum, pow10_128(scale - s->accscale), &r))
			numeric_spill_acc(aggstate, s);
		else
			s->sum = s->sum != 0 ? r : 0;
		s->accscale = scale;
	}
	else if (scale < s->accscale)
	{
		if (__builtin_mul_overflow(x, pow10_128(s->accscale - scale), &r))
		{
			numeric_overflow_add(aggstate, s, vexec_scaled_to_numeric(NULL, x, scale));
			return;
		}
		x = r;
	}
	if (__builtin_add_overflow(s->sum, x, &r))
	{
		numeric_spill_acc(aggstate, s);
		r = x;
	}
	s->sum = r;
}

/* do_numeric_accum()'s counts (numeric.c): the display scales, N. */
static inline void
numeric_count(NumericState *s, int dscale, int64 k)
{
	if (dscale > s->maxscale)
	{
		s->maxscale = dscale;
		s->maxscalecount = k;
	}
	else if (dscale == s->maxscale)
		s->maxscalecount += k;
	s->n += k;
}

/* One numeric value, as do_numeric_accum() takes it. */
static void
numeric_accum_datum(AggState *aggstate, NumericState *s, Datum d)
{
	VexecNumericParts parts;
	int128		x;

	if (!vexec_numeric_parts(d, &parts))
	{
		if (parts.special == VEXEC_NUMERIC_PINF)
			s->pinf++;
		else if (parts.special == VEXEC_NUMERIC_NINF)
			s->ninf++;
		else
			s->nan++;
		return;
	}
	numeric_count(s, parts.dscale, 1);
	if (parts.dscale <= 38 && vexec_numeric_to_scaled(d, parts.dscale, 38, 16, &x))
		numeric_add_scaled(aggstate, s, x, parts.dscale);
	else
		numeric_overflow_add(aggstate, s, d);
}

void
vexec_aggtrans_advance(VexecAggTrans *t, AggState *aggstate, void *state,
					   const Datum *args, const bool *nulls)
{
	switch (t->kind)
	{
		case VEXEC_AGG_FMGR:
			fmgr_advance(t, aggstate, (AggStatePerGroup) state, args, nulls);
			break;
		case VEXEC_AGG_COUNT_STAR:
			count_one((CountState *) state);
			break;
		case VEXEC_AGG_COUNT:
			if (!nulls[0])
				count_one((CountState *) state);
			break;
		case VEXEC_AGG_SUM_INT:
			if (!nulls[0])
			{
				SumIntState *s = state;
				int64		v = t->width == 2 ? DatumGetInt16(args[0]) : DatumGetInt32(args[0]);

				s->sum = s->any ? wrap_add64(s->sum, v) : v;
				s->any = true;
			}
			break;
		case VEXEC_AGG_AVG_INT:
			if (!nulls[0])
			{
				AvgIntState *s = state;
				int64		v = t->width == 2 ? DatumGetInt16(args[0]) : DatumGetInt32(args[0]);

				s->count++;
				s->sum = wrap_add64(s->sum, v);
			}
			break;
		case VEXEC_AGG_SUM_INT128:
			{
				Int128State *s = state;

				s->called = true;
				if (!nulls[0])
				{
					s->n++;
					s->sum += DatumGetInt64(args[0]);
				}
			}
			break;
		case VEXEC_AGG_SUM_NUMERIC:
			{
				NumericState *s = state;

				s->called = true;
				if (!nulls[0])
					numeric_accum_datum(aggstate, s, args[0]);
			}
			break;
		case VEXEC_AGG_SUM_FLOAT:
			if (!nulls[0])
				sum_float(t, (FloatSumState *) state,
						  t->single ? (float8) DatumGetFloat4(args[0]) : DatumGetFloat8(args[0]));
			break;
		case VEXEC_AGG_ACCUM_FLOAT:
			if (!nulls[0])
				accum_float((FloatAccumState *) state,
							t->single ? (float8) DatumGetFloat4(args[0]) : DatumGetFloat8(args[0]));
			break;
		case VEXEC_AGG_MINMAX:
			if (!nulls[0])
			{
				MinMaxState *s = state;

				if (!s->any || minmax_takes(t, s->value, args[0]))
				{
					s->value = args[0];
					s->any = true;
				}
			}
			break;
		case VEXEC_AGG_MINMAX_TEXT:
			if (!nulls[0])
			{
				const char *p;
				int			len;

				text_bytes(args[0], &p, &len);
				minmax_text(t, aggstate, (MinMaxState *) state, p, len);
			}
			break;
		case VEXEC_AGG_BOOL:
			if (!nulls[0])
			{
				BoolState *s = state;
				bool		v = DatumGetBool(args[0]);

				if (!s->any)
					s->value = v;
				else
					s->value = t->is_and ? (s->value && v) : (s->value || v);
				s->any = true;
			}
			break;
		case VEXEC_AGG_SUM_OFFSET:
			if (!nulls[0] && t->share < 0)
			{
				OffsetState *s = state;

				s->sum = wrap_add64(s->sum, DatumGetInt16(args[0]));
				s->count++;
			}
			break;
	}
}

/* ---------------------------------------------------------------------
 * A batch
 * ---------------------------------------------------------------------
 */

/*
 * The rows a transition takes: those asked for whose FILTER is true.  A
 * NULL filter is no FILTER.
 */
static uint64 *
filtered_rows(VexecBatch *work, const uint64 *rows, int nrows, VexecVec *filter)
{
	uint64	   *out = vexec_bitmap_alloc(work, nrows, false);

	memcpy(out, rows, sizeof(uint64) * VEXEC_WORDS(nrows));
	if (filter == NULL)
		return out;
	VEXEC_FOREACH_ROW(rows, nrows, i)
	{
		bool		isnull;
		Datum		d = vexec_vec_datum(work, filter, i, &isnull);

		if (isnull || !DatumGetBool(d))
			vexec_bit_clear(out, i);
	}
	return out;
}

/* Rows whose argument is not NULL, for a strict transition. */
static void
drop_nulls(uint64 *rows, int nrows, const VexecVec *arg)
{
	int			w;

	if (arg->encoding == VEXEC_CONST)
	{
		if (vexec_vec_isnull(arg, 0))
			memset(rows, 0, sizeof(uint64) * VEXEC_WORDS(nrows));
		return;
	}
	if (arg->validity == NULL)
		return;
	for (w = 0; w < VEXEC_WORDS(nrows); w++)
		rows[w] &= arg->validity[w];
}

/*
 * The argument flat, in the shape the loops read: the type's PostgreSQL
 * shape, a dictionary expanded and Arrow's epochs moved to PostgreSQL's.
 * NULL where it cannot be: the loop reads Datums then.
 */
static VexecVec *
flat_arg(VexecBatch *work, VexecVec *arg, bool scaled_ok)
{
	VexecVec   *c;
	VexecShape	want;

	if (arg->encoding != VEXEC_FLAT)
		return NULL;			/* a constant or a dictionary: by Datum */
	c = vexec_batch_alloc(work, sizeof(VexecVec));
	*c = *arg;
	if (scaled_ok && c->shape.layout == VEXEC_SCALED)
		return c;
	vexec_type_build_shape(c->type, &want);
	if (!vexec_shape_equal(&c->shape, &want) && !vexec_vec_convert(work, c, &want))
		return NULL;
	return c;
}

#define STATE(T)	((T *) (groups ? groups[i] + off : plain))

/* Through the kind's one-row step, a Datum at a time. */
static void
batch_by_datum(VexecAggTrans *t, AggState *aggstate, VexecBatch *work, char **groups,
			   Size off, char *plain, const uint64 *rows, int nrows, VexecVec *arg)
{
	VEXEC_FOREACH_ROW(rows, nrows, i)
	{
		Datum		d = (Datum) 0;
		bool		isnull = true;

		if (arg != NULL)
			d = vexec_vec_datum(work, arg, i, &isnull);
		vexec_aggtrans_advance(t, aggstate, STATE(void), &d, &isnull);
	}
}

static void
batch_count(char **groups, Size off, char *plain, const uint64 *rows, int nrows)
{
	if (groups == NULL)
	{
		CountState *s = (CountState *) plain;
		int64		k = vexec_bits_count(rows, nrows);

		if (unlikely(pg_add_s64_overflow(s->n, k, &s->n)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("bigint out of range")));
		return;
	}
	VEXEC_FOREACH_ROW(rows, nrows, i)
		count_one(STATE(CountState));
}

static inline int64
int_at(const VexecVec *v, int width, int i)
{
	if (width == 2)
		return ((const int16 *) v->values)[i];
	if (width == 4)
		return ((const int32 *) v->values)[i];
	return ((const int64 *) v->values)[i];
}

static void
batch_sum_int(VexecAggTrans *t, char **groups, Size off, char *plain,
			  const uint64 *rows, int nrows, const VexecVec *v)
{
	int			width = t->width;

	if (groups == NULL)
	{
		SumIntState *s = (SumIntState *) plain;
		uint64		sum = (uint64) s->sum;
		bool		any = s->any;

		VEXEC_FOREACH_ROW(rows, nrows, i)
		{
			sum += (uint64) int_at(v, width, i);
			any = true;
		}
		s->sum = s->any || any ? (int64) sum : 0;
		s->any = any;
		return;
	}
	VEXEC_FOREACH_ROW(rows, nrows, i)
	{
		SumIntState *s = STATE(SumIntState);

		s->sum = s->any ? wrap_add64(s->sum, int_at(v, width, i)) : int_at(v, width, i);
		s->any = true;
	}
}

static void
batch_avg_int(VexecAggTrans *t, char **groups, Size off, char *plain,
			  const uint64 *rows, int nrows, const VexecVec *v)
{
	int			width = t->width;

	if (groups == NULL)
	{
		AvgIntState *s = (AvgIntState *) plain;
		uint64		sum = (uint64) s->sum;
		int64		count = 0;

		VEXEC_FOREACH_ROW(rows, nrows, i)
		{
			sum += (uint64) int_at(v, width, i);
			count++;
		}
		s->sum = (int64) sum;
		s->count += count;
		return;
	}
	VEXEC_FOREACH_ROW(rows, nrows, i)
	{
		AvgIntState *s = STATE(AvgIntState);

		s->count++;
		s->sum = wrap_add64(s->sum, int_at(v, width, i));
	}
}

static void
batch_int128(char **groups, Size off, char *plain, const uint64 *called,
			 const uint64 *rows, int nrows, const VexecVec *v)
{
	const int64 *vals = (const int64 *) v->values;

	if (groups == NULL)
	{
		Int128State *s = (Int128State *) plain;
		int128		sum = s->sum;
		int64		n = 0;

		VEXEC_FOREACH_ROW(rows, nrows, i)
		{
			sum += vals[i];
			n++;
		}
		s->sum = sum;
		s->n += n;
		if (vexec_bits_any(called, nrows))
			s->called = true;
		return;
	}
	VEXEC_FOREACH_ROW(called, nrows, i)
		STATE(Int128State)->called = true;
	VEXEC_FOREACH_ROW(rows, nrows, i)
	{
		Int128State *s = STATE(Int128State);

		s->sum += vals[i];
		s->n++;
	}
}

static inline int128
scaled_value(const VexecVec *v, int i)
{
	if (v->shape.width == 8)
		return ((const int64 *) v->values)[i];
	else
	{
		int128		x;

		memcpy(&x, (const char *) v->values + (Size) i * 16, sizeof(int128));
		return x;
	}
}

static void
batch_numeric_scaled(AggState *aggstate, char **groups, Size off, char *plain,
					 const uint64 *called, const uint64 *rows, int nrows, const VexecVec *v)
{
	int			scale = v->shape.scale;

	if (groups == NULL)
	{
		NumericState *s = (NumericState *) plain;
		int128		sum = 0;
		int64		k = 0;
		bool		over = false;

		/* the batch's sum first, at its own scale, then into the state */
		VEXEC_FOREACH_ROW(rows, nrows, i)
		{
			if (!over && __builtin_add_overflow(sum, scaled_value(v, i), &sum))
				over = true;
			k++;
		}
		if (vexec_bits_any(called, nrows))
			s->called = true;
		if (k == 0)
			return;
		numeric_count(s, scale, k);
		if (!over)
			numeric_add_scaled(aggstate, s, sum, scale);
		else
		{
			VEXEC_FOREACH_ROW(rows, nrows, i)
				numeric_add_scaled(aggstate, s, scaled_value(v, i), scale);
		}
		return;
	}
	VEXEC_FOREACH_ROW(called, nrows, i)
		STATE(NumericState)->called = true;
	VEXEC_FOREACH_ROW(rows, nrows, i)
	{
		NumericState *s = STATE(NumericState);
		int128		x = scaled_value(v, i);
		int128		r;

		numeric_count(s, scale, 1);
		/* the common case inline: the accumulator at the batch's scale */
		if (s->accscale == scale && !__builtin_add_overflow(s->sum, x, &r))
			s->sum = r;
		else
			numeric_add_scaled(aggstate, s, x, scale);
	}
}

static void
batch_float(VexecAggTrans *t, char **groups, Size off, char *plain,
			const uint64 *rows, int nrows, const VexecVec *v)
{
	/* a plain sum of float8s in a register, in row order */
	if (groups == NULL && t->kind == VEXEC_AGG_SUM_FLOAT && !t->single)
	{
		FloatSumState *s = (FloatSumState *) plain;
		float8		sum = s->sum;
		bool		any = s->any;

		VEXEC_FOREACH_ROW(rows, nrows, i)
		{
			float8		x = ((const float8 *) v->values)[i];

			sum = any ? float8_pl(sum, x) : x;
			any = true;
		}
		s->sum = sum;
		s->any = any;
		return;
	}
	VEXEC_FOREACH_ROW(rows, nrows, i)
	{
		float8		x = t->single ? (float8) ((const float4 *) v->values)[i] :
			((const float8 *) v->values)[i];

		if (t->kind == VEXEC_AGG_SUM_FLOAT)
			sum_float(t, STATE(FloatSumState), x);
		else
			accum_float(STATE(FloatAccumState), x);
	}
}

static inline Datum
fixed_datum(const VexecAggTrans *t, const VexecVec *v, int i)
{
	switch (t->cmp)
	{
		case VEXEC_CMP_FLOAT4:
			return Float4GetDatum(((const float4 *) v->values)[i]);
		case VEXEC_CMP_FLOAT8:
			return Float8GetDatum(((const float8 *) v->values)[i]);
		case VEXEC_CMP_UINT:
			return ObjectIdGetDatum(((const Oid *) v->values)[i]);
		default:
			if (t->width == 2)
				return Int16GetDatum(((const int16 *) v->values)[i]);
			if (t->width == 4)
				return Int32GetDatum(((const int32 *) v->values)[i]);
			return Int64GetDatum(((const int64 *) v->values)[i]);
	}
}

static void
batch_minmax(VexecAggTrans *t, char **groups, Size off, char *plain,
			 const uint64 *rows, int nrows, const VexecVec *v)
{
	/* the common case of a plain integer max or min, in a register */
	if (groups == NULL && t->cmp == VEXEC_CMP_INT && t->width >= 4)
	{
		MinMaxState *s = (MinMaxState *) plain;
		int64		best = 0;
		bool		any = s->any;

		if (any)
			best = t->width == 4 ? DatumGetInt32(s->value) : DatumGetInt64(s->value);
		VEXEC_FOREACH_ROW(rows, nrows, i)
		{
			int64		x = int_at(v, t->width, i);

			if (!any || (t->larger ? x >= best : x <= best))
				best = x;
			any = true;
		}
		if (any)
			s->value = t->width == 4 ? Int32GetDatum((int32) best) : Int64GetDatum(best);
		s->any = any;
		return;
	}
	VEXEC_FOREACH_ROW(rows, nrows, i)
	{
		MinMaxState *s = STATE(MinMaxState);
		Datum		x = fixed_datum(t, v, i);

		if (!s->any || minmax_takes(t, s->value, x))
		{
			s->value = x;
			s->any = true;
		}
	}
}

static void
batch_text(VexecAggTrans *t, AggState *aggstate, char **groups, Size off, char *plain,
		   const uint64 *rows, int nrows, const VexecVec *v)
{
	VEXEC_FOREACH_ROW(rows, nrows, i)
	{
		const char *p;
		Size		len;

		if (v->shape.layout == VEXEC_DATUM)
		{
			int			l;

			/* a Datum's bytes, detoasted where it is stored compressed */
			text_bytes(((const Datum *) v->values)[i], &p, &l);
			len = l;
		}
		else
			vexec_vec_value_bytes(v, i, &p, &len);
		minmax_text(t, aggstate, STATE(MinMaxState), p, (int) len);
	}
}

static void
batch_offset(char **groups, Size off, char *plain, const uint64 *rows, int nrows,
			 const VexecVec *v)
{
	if (groups == NULL)
	{
		OffsetState *s = (OffsetState *) plain;
		uint64		sum = (uint64) s->sum;
		int64		count = 0;

		VEXEC_FOREACH_ROW(rows, nrows, i)
		{
			sum += (uint64) (int64) ((const int16 *) v->values)[i];
			count++;
		}
		s->sum = (int64) sum;
		s->count += count;
		return;
	}
	VEXEC_FOREACH_ROW(rows, nrows, i)
	{
		OffsetState *s = STATE(OffsetState);

		s->sum = wrap_add64(s->sum, ((const int16 *) v->values)[i]);
		s->count++;
	}
}

static void
batch_bool(VexecAggTrans *t, char **groups, Size off, char *plain,
		   const uint64 *rows, int nrows, const VexecVec *v)
{
	VEXEC_FOREACH_ROW(rows, nrows, i)
	{
		BoolState  *s = STATE(BoolState);
		bool		x = v->shape.layout == VEXEC_BIT_BOOL ?
			vexec_bit((const uint64 *) v->values, i) : ((const uint8 *) v->values)[i] != 0;

		if (!s->any)
			s->value = x;
		else
			s->value = t->is_and ? (s->value && x) : (s->value || x);
		s->any = true;
	}
}

void
vexec_aggtrans_batch(VexecAggTrans *t, AggState *aggstate, VexecBatch *work,
					 char **groups, Size off, char *plain,
					 const uint64 *rows, int nrows, VexecVec *arg, VexecVec *filter)
{
	uint64	   *take = filtered_rows(work, rows, nrows, filter);
	uint64	   *called;
	VexecVec   *v;

	if (!vexec_bits_any(take, nrows))
		return;
	if (t->kind == VEXEC_AGG_COUNT_STAR)
	{
		batch_count(groups, off, plain, take, nrows);
		return;
	}
	if (arg == NULL)
		elog(ERROR, "vexec: an aggregate's argument was not computed");
	if (t->kind == VEXEC_AGG_COUNT)
	{
		drop_nulls(take, nrows, arg);
		batch_count(groups, off, plain, take, nrows);
		return;
	}

	/* the non-strict ones run for NULLs too: their state is made */
	called = take;
	if (t->kind == VEXEC_AGG_SUM_INT128 || t->kind == VEXEC_AGG_SUM_NUMERIC)
		take = vexec_bits_copy(work, called, nrows);
	drop_nulls(take, nrows, arg);

	if (t->kind == VEXEC_AGG_MINMAX_TEXT)
	{
		/* a constant or a dictionary: its values a row at a time */
		if (arg->encoding == VEXEC_FLAT &&
			(arg->shape.layout == VEXEC_DATUM || arg->shape.layout == VEXEC_VIEW ||
			 arg->shape.layout == VEXEC_OFFSETS))
			batch_text(t, aggstate, groups, off, plain, take, nrows, arg);
		else
			batch_by_datum(t, aggstate, work, groups, off, plain, take, nrows, arg);
		return;
	}

	v = flat_arg(work, arg, t->kind == VEXEC_AGG_SUM_NUMERIC);
	if (v == NULL || (t->kind == VEXEC_AGG_SUM_NUMERIC && v->shape.layout != VEXEC_SCALED))
	{
		if (t->kind == VEXEC_AGG_SUM_INT128 || t->kind == VEXEC_AGG_SUM_NUMERIC)
			batch_by_datum(t, aggstate, work, groups, off, plain, called, nrows, arg);
		else
			batch_by_datum(t, aggstate, work, groups, off, plain, take, nrows, arg);
		return;
	}

	switch (t->kind)
	{
		case VEXEC_AGG_SUM_INT:
			batch_sum_int(t, groups, off, plain, take, nrows, v);
			break;
		case VEXEC_AGG_AVG_INT:
			batch_avg_int(t, groups, off, plain, take, nrows, v);
			break;
		case VEXEC_AGG_SUM_INT128:
			batch_int128(groups, off, plain, called, take, nrows, v);
			break;
		case VEXEC_AGG_SUM_NUMERIC:
			batch_numeric_scaled(aggstate, groups, off, plain, called, take, nrows, v);
			break;
		case VEXEC_AGG_SUM_FLOAT:
		case VEXEC_AGG_ACCUM_FLOAT:
			batch_float(t, groups, off, plain, take, nrows, v);
			break;
		case VEXEC_AGG_MINMAX:
			batch_minmax(t, groups, off, plain, take, nrows, v);
			break;
		case VEXEC_AGG_BOOL:
			batch_bool(t, groups, off, plain, take, nrows, v);
			break;
		case VEXEC_AGG_SUM_OFFSET:
			if (t->share < 0)
				batch_offset(groups, off, plain, take, nrows, v);
			break;
		default:
			batch_by_datum(t, aggstate, work, groups, off, plain, take, nrows, arg);
			break;
	}
}

/* ---------------------------------------------------------------------
 * A unit of rows from a source's statistics (H2)
 * ---------------------------------------------------------------------
 */

int
vexec_aggtrans_stats_requests(const VexecAggTrans *t, Oid argtype, AttrNumber attnum,
							  VexecSourceAgg *reqs)
{
	int			n = 0;

#define STATS_REQUEST(k, ty) \
	(reqs[n].kind = (k), reqs[n].attnum = attnum, reqs[n].type = (ty), \
	 reqs[n].collation = InvalidOid, n++)

	switch (t->kind)
	{
		case VEXEC_AGG_COUNT_STAR:
			break;
		case VEXEC_AGG_COUNT:
			STATS_REQUEST(VEXEC_SRC_AGG_COUNT, INT8OID);
			break;
		case VEXEC_AGG_MINMAX:

			/*
			 * Integers, dates, times and timestamps, which every btree
			 * ordering of theirs orders as their larger and smaller
			 * functions do; not oid, nor a float, whose NaN a source's
			 * ordering may place elsewhere.
			 */
			if (t->cmp != VEXEC_CMP_INT || argtype == MONEYOID)
				return -1;
			STATS_REQUEST(t->larger ? VEXEC_SRC_AGG_MAX : VEXEC_SRC_AGG_MIN, argtype);
			break;
		case VEXEC_AGG_SUM_INT:
			STATS_REQUEST(VEXEC_SRC_AGG_SUM, INT8OID);
			break;
		case VEXEC_AGG_AVG_INT:
			STATS_REQUEST(VEXEC_SRC_AGG_COUNT, INT8OID);
			STATS_REQUEST(VEXEC_SRC_AGG_SUM, INT8OID);
			break;
		case VEXEC_AGG_SUM_INT128:
			STATS_REQUEST(VEXEC_SRC_AGG_COUNT, INT8OID);
			STATS_REQUEST(VEXEC_SRC_AGG_SUM, NUMERICOID);
			break;
		default:
			return -1;
	}
#undef STATS_REQUEST
	return n;
}

static void
count_add(CountState *s, int64 k)
{
	if (unlikely(pg_add_s64_overflow(s->n, k, &s->n)))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));
}

/*
 * Each kind as its batch step takes the unit's rows: count(*) counts them,
 * count(x) its values; min and max take the unit's least or greatest value
 * as one more input; sum and avg of int2 and int4 add the unit's sum, which
 * wraps as int4_sum's does, being the same sum; sum and avg of int8 take
 * the numeric sum as an int128, int8_avg_accum having been called for each
 * row, NULL or not.
 */
void
vexec_aggtrans_stats_apply(VexecAggTrans *t, AggState *aggstate, void *state,
						   const VexecSourceAggAnswer *answers, int64 nrows)
{
	switch (t->kind)
	{
		case VEXEC_AGG_COUNT_STAR:
			count_add((CountState *) state, nrows);
			break;
		case VEXEC_AGG_COUNT:
			count_add((CountState *) state, DatumGetInt64(answers[0].value));
			break;
		case VEXEC_AGG_MINMAX:
			if (!answers[0].isnull)
			{
				Datum		d = answers[0].value;
				bool		isnull = false;

				vexec_aggtrans_advance(t, aggstate, state, &d, &isnull);
			}
			break;
		case VEXEC_AGG_SUM_INT:
			if (!answers[0].isnull)
			{
				SumIntState *s = (SumIntState *) state;
				int64		sum = DatumGetInt64(answers[0].value);

				s->sum = s->any ? wrap_add64(s->sum, sum) : sum;
				s->any = true;
			}
			break;
		case VEXEC_AGG_AVG_INT:
			{
				AvgIntState *s = (AvgIntState *) state;

				s->count += DatumGetInt64(answers[0].value);
				if (!answers[1].isnull)
					s->sum = wrap_add64(s->sum, DatumGetInt64(answers[1].value));
			}
			break;
		case VEXEC_AGG_SUM_INT128:
			{
				Int128State *s = (Int128State *) state;
				int64		n = DatumGetInt64(answers[0].value);
				int128		sum;

				if (nrows > 0)
					s->called = true;
				if (n == 0)
					break;
				if (answers[1].isnull ||
					!vexec_numeric_to_scaled(answers[1].value, 0, 38, 16, &sum))
					elog(ERROR, "vexec: a batch source's sum of %lld bigint values is not an integer of 38 digits",
						 (long long) n);
				s->sum += sum;
				s->n += n;
			}
			break;
		default:
			elog(ERROR, "vexec: no statistics give the state of the %s transition",
				 t->kindname);
	}
}

/* ---------------------------------------------------------------------
 * Results
 * ---------------------------------------------------------------------
 */

/* int8_avg_serialize()'s bytes (numeric.c:5797-5826) of N and sumX. */
static bytea *
int128_serialized(const Int128State *s)
{
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, s->n);
	pq_sendint64(&buf, (int64) (s->sum >> 64));
	pq_sendint64(&buf, (uint64) s->sum);
	return pq_endtypsend(&buf);
}

/* numeric_avg_serialize()'s bytes (numeric.c:5184-5232). */
static bytea *
numeric_serialized(const NumericState *s)
{
	StringInfoData buf;
	Datum		sum;
	VexecNumericParts parts;
	int			i;

	/* sumX, at the largest display scale of the values summed */
	sum = vexec_scaled_to_numeric(NULL, s->sum, s->accscale);
	if (s->overflow != (Datum) 0)
		sum = DirectFunctionCall2(numeric_add, sum, s->overflow);
	if (!vexec_numeric_parts(sum, &parts))
		elog(ERROR, "vexec: a numeric sum of finite values is not finite");

	pq_begintypsend(&buf);
	pq_sendint64(&buf, s->n);
	pq_sendint32(&buf, parts.ndigits);
	pq_sendint32(&buf, parts.weight);
	pq_sendint32(&buf, parts.sign);
	pq_sendint32(&buf, parts.dscale);
	for (i = 0; i < parts.ndigits; i++)
	{
		int16		d;

		memcpy(&d, parts.digits + i * sizeof(int16), sizeof(int16));
		pq_sendint16(&buf, d);
	}
	pq_sendint32(&buf, s->maxscale);
	pq_sendint64(&buf, s->maxscalecount);
	pq_sendint64(&buf, s->nan);
	pq_sendint64(&buf, s->pinf);
	pq_sendint64(&buf, s->ninf);
	return pq_endtypsend(&buf);
}

/* An internal state, from its bytes, by the aggregate's deserialization. */
static Datum
internal_state(AggState *aggstate, Oid deserialfn, bytea *bytes)
{
	LOCAL_FCINFO(fcinfo, 2);
	FmgrInfo	flinfo;
	Datum		result;

	fmgr_info(deserialfn, &flinfo);
	InitFunctionCallInfoData(*fcinfo, &flinfo, 2, InvalidOid, (Node *) aggstate, NULL);
	fcinfo->args[0].value = PointerGetDatum(bytes);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = PointerGetDatum(NULL);
	fcinfo->args[1].isnull = false;
	result = FunctionCallInvoke(fcinfo);
	if (fcinfo->isnull)
		elog(ERROR, "vexec: deserialization function %u returned NULL", deserialfn);
	return result;
}

/*
 * A vectorized kind's state as PostgreSQL's transition function would
 * have left it, in the current memory context.  *bytes, for an internal
 * state, its serialization.
 */
static void
pg_state(VexecAggTrans *t, AggState *aggstate, void *state, bool internal,
		 Datum *value, bool *isnull, bytea **bytes)
{
	*value = (Datum) 0;
	*isnull = true;
	*bytes = NULL;
	switch (t->kind)
	{
		case VEXEC_AGG_FMGR:
			{
				AggStatePerGroup pergroup = (AggStatePerGroup) state;

				*value = pergroup->transValue;
				*isnull = pergroup->transValueIsNull;
			}
			break;
		case VEXEC_AGG_COUNT_STAR:
		case VEXEC_AGG_COUNT:
			*value = Int64GetDatum(((CountState *) state)->n);
			*isnull = false;
			break;
		case VEXEC_AGG_SUM_INT:
			if (((SumIntState *) state)->any)
			{
				*value = Int64GetDatum(((SumIntState *) state)->sum);
				*isnull = false;
			}
			break;
		case VEXEC_AGG_AVG_INT:
			{
				Datum		d[2];

				d[0] = Int64GetDatum(((AvgIntState *) state)->count);
				d[1] = Int64GetDatum(((AvgIntState *) state)->sum);
				*value = PointerGetDatum(construct_array_builtin(d, 2, INT8OID));
				*isnull = false;
			}
			break;
		case VEXEC_AGG_SUM_INT128:
			if (((Int128State *) state)->called)
			{
				*bytes = int128_serialized((Int128State *) state);
				if (internal)
					*value = internal_state(aggstate, F_INT8_AVG_DESERIALIZE, *bytes);
				*isnull = false;
			}
			break;
		case VEXEC_AGG_SUM_NUMERIC:
			if (((NumericState *) state)->called)
			{
				*bytes = numeric_serialized((NumericState *) state);
				if (internal)
					*value = internal_state(aggstate, F_NUMERIC_AVG_DESERIALIZE, *bytes);
				*isnull = false;
			}
			break;
		case VEXEC_AGG_SUM_FLOAT:
			if (((FloatSumState *) state)->any)
			{
				float8		sum = ((FloatSumState *) state)->sum;

				*value = t->single ? Float4GetDatum((float4) sum) : Float8GetDatum(sum);
				*isnull = false;
			}
			break;
		case VEXEC_AGG_ACCUM_FLOAT:
			{
				FloatAccumState *s = state;
				Datum		d[3];

				d[0] = Float8GetDatum(s->n);
				d[1] = Float8GetDatum(s->sx);
				d[2] = Float8GetDatum(s->sxx);
				*value = PointerGetDatum(construct_array_builtin(d, 3, FLOAT8OID));
				*isnull = false;
			}
			break;
		case VEXEC_AGG_MINMAX:
		case VEXEC_AGG_MINMAX_TEXT:
			if (((MinMaxState *) state)->any)
			{
				*value = ((MinMaxState *) state)->value;
				*isnull = false;
			}
			break;
		case VEXEC_AGG_BOOL:
			if (((BoolState *) state)->any)
			{
				*value = BoolGetDatum(((BoolState *) state)->value);
				*isnull = false;
			}
			break;
		case VEXEC_AGG_SUM_OFFSET:
			if (((OffsetState *) state)->count > 0)
			{
				OffsetState *s = state;

				*value = Int64GetDatum(wrap_add64(s->sum, (int64) ((uint64) t->offset * (uint64) s->count)));
				*isnull = false;
			}
			break;
	}
}

/*
 * An aggregate's result for a group, as finalize_aggregate() and
 * finalize_partialaggregate() make it (nodeAgg.c:1046-1195), in the output
 * tuple's memory.
 */
void
vexec_aggtrans_result(VexecAggTrans *t, AggState *aggstate, int aggno, void *state,
					  Datum *value, bool *isnull)
{
	AggStatePerAgg peragg = &aggstate->peragg[aggno];
	AggStatePerTrans pertrans = t->pertrans;
	AggSplit	split = peragg->aggref->aggsplit;
	MemoryContext old;
	Datum		sv;
	bool		snull;
	bytea	   *bytes;

	bool		serialized = DO_AGGSPLIT_SKIPFINAL(split) &&
		OidIsValid(pertrans->serialfn_oid);

	old = MemoryContextSwitchTo(aggstate->ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
	pg_state(t, aggstate, state, !serialized, &sv, &snull, &bytes);

	if (DO_AGGSPLIT_SKIPFINAL(split))
	{
		if (serialized)
		{
			if (pertrans->serialfn.fn_strict && snull)
			{
				*value = (Datum) 0;
				*isnull = true;
			}
			else if (bytes != NULL)
			{
				/* the serialization function's bytes, written here */
#ifdef USE_ASSERT_CHECKING
				{
					FunctionCallInfo fcinfo = pertrans->serialfn_fcinfo;
					bytea	   *theirs;

					fcinfo->args[0].value =
						internal_state(aggstate, t->kind == VEXEC_AGG_SUM_INT128 ?
									   F_INT8_AVG_DESERIALIZE : F_NUMERIC_AVG_DESERIALIZE,
									   bytes);
					fcinfo->args[0].isnull = false;
					fcinfo->isnull = false;
					theirs = DatumGetByteaPP(FunctionCallInvoke(fcinfo));
					Assert(VARSIZE_ANY_EXHDR(theirs) == VARSIZE_ANY_EXHDR(bytes) &&
						   memcmp(VARDATA_ANY(theirs), VARDATA_ANY(bytes),
								  VARSIZE_ANY_EXHDR(bytes)) == 0);
				}
#endif
				*value = PointerGetDatum(bytes);
				*isnull = false;
			}
			else
			{
				FunctionCallInfo fcinfo = pertrans->serialfn_fcinfo;
				Datum		result;

				fcinfo->args[0].value = MakeExpandedObjectReadOnly(sv, snull, pertrans->transtypeLen);
				fcinfo->args[0].isnull = snull;
				fcinfo->isnull = false;
				result = FunctionCallInvoke(fcinfo);
				*isnull = fcinfo->isnull;
				*value = MakeExpandedObjectReadOnly(result, fcinfo->isnull, peragg->resulttypeLen);
			}
		}
		else
		{
			*value = MakeExpandedObjectReadOnly(sv, snull, pertrans->transtypeLen);
			*isnull = snull;
		}
		MemoryContextSwitchTo(old);
		return;
	}

	if (OidIsValid(peragg->finalfn_oid))
	{
		LOCAL_FCINFO(fcinfo, FUNC_MAX_ARGS);
		int			numFinalArgs = peragg->numFinalArgs;
		bool		anynull = snull;
		int			i;

		aggstate->curperagg = peragg;
		InitFunctionCallInfoData(*fcinfo, &peragg->finalfn, numFinalArgs,
								 pertrans->aggCollation, (Node *) aggstate, NULL);
		fcinfo->args[0].value = MakeExpandedObjectReadOnly(sv, snull, pertrans->transtypeLen);
		fcinfo->args[0].isnull = snull;
		for (i = 1; i < numFinalArgs; i++)
		{
			fcinfo->args[i].value = (Datum) 0;
			fcinfo->args[i].isnull = true;
			anynull = true;
		}
		if (fcinfo->flinfo->fn_strict && anynull)
		{
			*value = (Datum) 0;
			*isnull = true;
		}
		else
		{
			Datum		result = FunctionCallInvoke(fcinfo);

			*isnull = fcinfo->isnull;
			*value = MakeExpandedObjectReadOnly(result, fcinfo->isnull, peragg->resulttypeLen);
		}
		aggstate->curperagg = NULL;
	}
	else
	{
		*value = MakeExpandedObjectReadOnly(sv, snull, pertrans->transtypeLen);
		*isnull = snull;
	}
	MemoryContextSwitchTo(old);
}
