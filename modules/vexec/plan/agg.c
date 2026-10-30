/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * agg.c
 *	  VecAgg in the planners (pg_vector_executor.md §3.3.3, §3.3.4, §3.8):
 *	  what the oracle accepts of an aggregation, VecAgg's paths in
 *	  PostgreSQL's planner, and its plan, built from a path or from the Agg
 *	  ORCA's translator built.
 *
 * PostgreSQL's planner calls create_upper_paths_hook for UPPERREL_GROUP_AGG
 * once its own paths of the grouped relation are made, and none for the
 * partial stage (planner.c:7956-7965).  So the hook adds both of VecAgg's
 * shapes there:
 *
 *	simple		a VecAgg over the input relation's cheapest path, plain or
 *				hashed, AGGSPLIT_SIMPLE;
 *	in order	(VC4) the same over the relation's serial VecScan, where its
 *				source hands its rows in an order the keys are a prefix of:
 *				each group made as its rows come, and handed up as it ends;
 *	the pair	a partial VecAgg (AGGSPLIT_INITIAL_SERIAL) over the input's
 *				cheapest partial path, into the partially grouped relation
 *				core made, under a Gather, and a final VecAgg
 *				(AGGSPLIT_FINAL_DESERIAL) over the Gather;
 *	partitioned	(H5, §3.14) the same pair with the final VecAgg in each
 *				participant too, below the Gather, over a VecRepartition
 *				that deals the partial groups out among the participants by
 *				their keys' hash (exec/vecrepart.c): the Gather receives
 *				final groups only, and each participant finalizes its own.
 *				It is a partial path of the grouped relation as well, which
 *				create_ordered_paths() sorts in each participant under a
 *				Gather Merge, its LIMIT's bound too.
 *
 * In auto mode they compete in add_path by cost; in force mode they replace
 * the grouped relation's paths.  Explain mode records them.
 *
 * The plan's canonical form (plan/check.c): no scanrelid, the child in
 * lefttree, custom_scan_tlist the group's keys, the columns it keeps from
 * its first row, and its aggregates, which its target list and HAVING qual
 * read as INDEX_VAR -- set_customscan_references() puts them so for
 * PostgreSQL's planner, from their planner form (setrefs.c); for ORCA the
 * builder rewrites the Agg's.  The aggregates go into custom_private in
 * executor form, their arguments reading the child's output as OUTER_VAR,
 * matched against the child's target list as set_upper_references()
 * matches an Agg's, and for a final stage turned into the combining form
 * convert_combining_aggrefs() makes (setrefs.c): setrefs never looks into
 * custom_private, and custom_scan_tlist keeps the planner's form for
 * EXPLAIN.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parse_agg.h"
#include "parser/parsetree.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "vexec.h"
#include "exec/aggtrans.h"
#include "exec/device.h"
#include "exec/exec.h"
#include "plan/plan.h"
#include "source/source.h"

/* What a VecAgg path keeps for its plan: custom_private. */
typedef struct AggPathInfo
{
	AggStrategy strategy;
	AggSplit	split;
	double		numgroups;
	List	   *groupclause;
	List	   *having;
	Index		distinct_ref;	/* H3's upper stage: the sortgroupref of the
								 * DISTINCT argument in its input; else 0 */
	bool		distinct_combine;	/* X1's final stage: the DISTINCT
									 * aggregate's input is a count of the
									 * distinct values a participant was
									 * dealt, which it combines as count's */
	bool		sorted_input;	/* VC4: the input a serial VecScan whose
								 * source's order the keys are a prefix of */
	bool		device;			/* V8b: the device's strategy priced cheaper,
								 * or forced */
	bool		units;			/* V8e: and the device reads its input's units */
	bool		whole;			/* W1: H3's upper stage the device's whole,
								 * over its lower stage's scan */
	bool		device_always;	/* W7: priced as the device's wherever it may
								 * take it, cheaper or not: a variant kept
								 * aside for a bounded sort (keep_device_path()) */
	Bitmapset  *kept_attrs;		/* W11: the relation's attributes another
								 * device task of the statement reads first,
								 * which the device then keeps (W5) */
	bool		distinct_whole;	/* W19: priced as the device's whole grouping
								 * of the keys, the other aggregates and one
								 * count(DISTINCT x), a count of x's pairs */
	double		group_bytes;	/* W45 (§3.35): the device's, W42's bytes of a
								 * group on it, which its task asks admission
								 * for; 0: none */
} AggPathInfo;

/*
 * A stage that combines partial states and hands them on partial (X1):
 * nodeAgg.c names no such split, and its flags allow it.
 */
#define VEXEC_AGGSPLIT_INTERMEDIATE \
	((AggSplit) (AGGSPLITOP_COMBINE | AGGSPLITOP_SKIPFINAL | \
				 AGGSPLITOP_SERIALIZE | AGGSPLITOP_DESERIALIZE))

static Plan *plan_vecagg(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
						 List *tlist, List *clauses, List *custom_plans);

static bool contain_alternative_subplan(Node *node);
static int	list_position_int(List *list, int value);
static int64 query_topk(PlannerInfo *root, List *aggrefs, Index distinct_ref, bool distinct_combine);

static const CustomPathMethods vecagg_path_methods = {
	.CustomName = VEXEC_AGG_NAME,
	.PlanCustomPath = plan_vecagg,
};

static Plan *plan_vecrepart(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
							List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods vecrepart_path_methods = {
	.CustomName = VEXEC_REPART_NAME,
	.PlanCustomPath = plan_vecrepart,
};

static Plan *plan_vecaggjoin(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
							 List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods vecaggjoin_path_methods = {
	.CustomName = VEXEC_AGGJOIN_NAME,
	.PlanCustomPath = plan_vecaggjoin,
};

/* ---------------------------------------------------------------------
 * The oracle
 * ---------------------------------------------------------------------
 */

typedef struct AggScan
{
	List	   *groupexprs;		/* the grouping expressions */
	List	   *aggrefs;		/* each once */
	List	   *extras;			/* Vars and PlaceHolderVars outside them */
	List	   *distinct;		/* the aggregates with DISTINCT, each once */
	const char *refusal;		/* the first reason, DISTINCT's aside */
} AggScan;

/*
 * What a target list and HAVING qual read, as set_upper_references() and
 * fix_upper_expr() would match them: a grouping expression whole, an
 * aggregate whole, any other Var or PlaceHolderVar as a column of the
 * group's first row.
 */
static bool
scan_upper(Node *node, AggScan *as)
{
	if (node == NULL)
		return false;
	if (list_member(as->groupexprs, node))
		return false;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;

		if (aggref->agglevelsup != 0)
			as->refusal = "an aggregate of an outer query level";
		else if (aggref->aggdistinct != NIL)
		{
			/* H3 may take it: the caller decides */
			if (!list_member(as->distinct, aggref))
				as->distinct = lappend(as->distinct, aggref);
		}
		else if (aggref->aggorder != NIL)
			as->refusal = "ORDER BY inside an aggregate";
		else if (aggref->aggkind != AGGKIND_NORMAL)
			as->refusal = "an ordered-set aggregate";
		else if (aggref->aggdirectargs != NIL)
			as->refusal = "an aggregate's direct arguments";
		else if (contain_alternative_subplan((Node *) aggref->args) ||
				 contain_alternative_subplan((Node *) aggref->aggfilter))
			as->refusal = "a subplan of two alternatives in an aggregate's argument";
		if (!list_member(as->aggrefs, aggref))
			as->aggrefs = lappend(as->aggrefs, aggref);
		return false;
	}
	if (IsA(node, GroupingFunc))
	{
		as->refusal = "GROUPING()";
		return false;
	}
	if (IsA(node, Var) || IsA(node, PlaceHolderVar))
	{
		if (IsA(node, Var) && ((Var *) node)->varlevelsup != 0)
			return false;
		if (!list_member(as->extras, node))
			as->extras = lappend(as->extras, node);
		return false;
	}
	if (IsA(node, WindowFunc))
	{
		as->refusal = "a window function";
		return false;
	}
	return expression_tree_walker(node, scan_upper, as);
}

/* Whether an expression holds an AlternativeSubPlan. */
static bool
alt_subplan_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, AlternativeSubPlan))
		return true;
	return expression_tree_walker(node, alt_subplan_walker, context);
}

static bool
contain_alternative_subplan(Node *node)
{
	return alt_subplan_walker(node, NULL);
}

/* The grouping expressions of a target, by their sortgrouprefs. */
static List *
group_exprs(List *groupclause, PathTarget *target, bool *missing)
{
	List	   *exprs = NIL;
	ListCell   *lc;

	*missing = false;
	foreach(lc, groupclause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		Expr	   *e = NULL;
		int			i = 0;
		ListCell   *lc2;

		foreach(lc2, target->exprs)
		{
			if (get_pathtarget_sortgroupref(target, i) == sgc->tleSortGroupRef)
			{
				e = lfirst(lc2);
				break;
			}
			i++;
		}
		if (e == NULL)
			*missing = true;
		else
			exprs = lappend(exprs, e);
	}
	return exprs;
}

/*
 * What of an aggregation the oracle refuses (§3.8): grouping sets, DISTINCT
 * and ORDER BY inside an aggregate, ordered-set aggregates, GROUPING(),
 * grouping expressions that repeat; NULL where it accepts it.  as gets the
 * aggregates and the extra columns.
 */
static const char *
agg_refusal(PlannerInfo *root, List *groupclause, PathTarget *target, List *having,
			PathTarget *input_target, AggScan *as)
{
	bool		missing;
	ListCell   *lc;

	memset(as, 0, sizeof(AggScan));
	if (root->parse->groupingSets != NIL)
		return "grouping sets";
	as->groupexprs = group_exprs(groupclause, input_target, &missing);
	if (missing)
		return "a grouping column the input does not give";
	foreach(lc, as->groupexprs)
	{
		/* fix_upper_expr() would read both from the first */
		if (list_member(list_copy_head(as->groupexprs, foreach_current_index(lc)), lfirst(lc)))
			return "grouping expressions that repeat";
	}
	scan_upper((Node *) target->exprs, as);
	scan_upper((Node *) having, as);
	if (as->refusal)
		return as->refusal;
	if (as->distinct != NIL)
		return "DISTINCT inside an aggregate";
	foreach(lc, as->extras)
	{
		if (!list_member(input_target->exprs, lfirst(lc)))
			return "a column the aggregation's input does not give";
	}
	return NULL;
}

/*
 * M3 (§3.21): whether a grouping equality is the identity of its values, so
 * that a function of the column gives every row of a group the same result:
 * integers, oids, booleans, "char", dates and timestamps by their bits, and
 * text, varchar and bytea by their bytes under a deterministic collation --
 * not floats, whose -0 equals 0, nor numeric, whose 1.0 equals 1.00, nor
 * bpchar, whose trailing blanks do not count.
 */
static bool
identity_equality(Oid eqop, Oid collation)
{
	switch (get_opcode(eqop))
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
		case F_BYTEAEQ:
			return true;
		case F_TEXTEQ:
			return OidIsValid(collation) && get_collation_isdeterministic(collation);
		default:
			return false;
	}
}

/*
 * M3 (§3.21): the grouping clause without its keys that are functions of
 * the others -- an immutable expression whose every variable is another key
 * of the clause, a column compared by identity -- which ClickHouse drops too
 * (optimize_group_by_function_keys).  Equal kept keys then make equal
 * dropped ones, so the groups are the same; the input still computes them
 * for every row, as before, and an output that reads one computes it from
 * the group's keys (fix_upper_expr() finding the key's column inside it).
 * Q35's GROUP BY ClientIP, ClientIP - 1, ClientIP - 2, ClientIP - 3 hashes
 * ClientIP alone.  *ndropped says how many went.
 */
static List *
independent_keys(List *groupclause, List *groupexprs, int *ndropped, List **keptexprs)
{
	List	   *kept = NIL;
	List	   *exprs = NIL;
	ListCell   *lc1,
			   *lc2;

	*ndropped = 0;
	*keptexprs = groupexprs;
	if (!vexec_drop_implied_keys || list_length(groupclause) < 2)
		return groupclause;
	forboth(lc1, groupclause, lc2, groupexprs)
	{
		Node	   *e = lfirst(lc2);
		List	   *vars;
		bool		implied;
		ListCell   *lv;

		if (IsA(e, Var) || IsA(e, PlaceHolderVar) || contain_mutable_functions(e) ||
			contain_subplans(e))
		{
			kept = lappend(kept, lfirst(lc1));
			exprs = lappend(exprs, e);
			continue;
		}
		vars = pull_var_clause(e, PVC_RECURSE_PLACEHOLDERS);
		implied = vars != NIL;
		foreach(lv, vars)
		{
			Var		   *v = lfirst(lv);
			bool		found = false;
			ListCell   *k1,
					   *k2;

			if (v->varlevelsup != 0)
			{
				implied = false;
				break;
			}
			forboth(k1, groupclause, k2, groupexprs)
			{
				SortGroupClause *sgc = lfirst_node(SortGroupClause, k1);

				if (IsA(lfirst(k2), Var) && equal(lfirst(k2), v) &&
					identity_equality(sgc->eqop, exprCollation((Node *) v)))
				{
					found = true;
					break;
				}
			}
			if (!found)
			{
				implied = false;
				break;
			}
		}
		if (implied)
			(*ndropped)++;
		else
		{
			kept = lappend(kept, lfirst(lc1));
			exprs = lappend(exprs, e);
		}
	}
	if (*ndropped == 0)
		return groupclause;
	*keptexprs = exprs;
	return kept;
}

/*
 * M3 (§3.21): a partial stage's target without the grouping expressions its
 * keys imply, which it then neither computes nor hands on, so that its
 * groups stay as they lie (D2): the final stage computes them from its keys
 * for the groups it hands up -- with a bound above it, for those alone.
 */
static PathTarget *
without_implied(PlannerInfo *root, PathTarget *target, List *implied)
{
	PathTarget *t;
	int			i = 0;
	ListCell   *lc;

	if (implied == NIL)
		return target;
	t = create_empty_pathtarget();
	foreach(lc, target->exprs)
	{
		if (!list_member(implied, lfirst(lc)))
			add_column_to_pathtarget(t, lfirst(lc), get_pathtarget_sortgroupref(target, i));
		i++;
	}
	return set_pathtarget_cost_width(root, t);
}

/* How many of the aggregates have a vector transition, for the cost. */
static int
count_vector_aggs(List *aggrefs, AggSplit split, List **names)
{
	int			n = 0;
	ListCell   *lc;

	foreach(lc, aggrefs)
	{
		Aggref	   *aggref = lfirst_node(Aggref, lc);
		Oid			argtype = InvalidOid;
		const char *name = NULL;

		if (aggref->args != NIL)
			argtype = exprType((Node *) linitial_node(TargetEntry, aggref->args)->expr);
		if (vexec_agg_vectorized(aggref->aggfnoid, split, argtype, &name))
			n++;
		if (names)
			*names = lappend(*names, (char *) (name ? name : "fmgr"));
	}
	return n;
}

/* Grouping columns whose keys hash by their bits or bytes. */
static int
count_kernel_keys(List *groupclause, List *groupexprs)
{
	int			n = 0;
	ListCell   *lc1,
			   *lc2;

	forboth(lc1, groupclause, lc2, groupexprs)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc1);

		switch (get_opcode(sgc->eqop))
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
			case F_BYTEAEQ:
				n++;
				break;
			case F_TEXTEQ:
				{
					Oid			coll = exprCollation(lfirst(lc2));

					if (!OidIsValid(coll) || get_collation_isdeterministic(coll))
						n++;
				}
				break;
			default:
				break;
		}
	}
	return n;
}

/* ---------------------------------------------------------------------
 * Paths
 * ---------------------------------------------------------------------
 */

/*
 * A grouping key's or an aggregate argument's bytes as a chunk carries it
 * to the device, or -1 where the device has no layout of its type (V8b).
 */
static int
device_width(Oid type, int32 typmod, Oid collation, bool key)
{
	switch (type)
	{
		case NUMERICOID:
			/* an argument in a scaled layout of 18 digits or fewer: its int64 */
			if (key || typmod < (int32) VARHDRSZ || ((typmod - VARHDRSZ) >> 16) > 18 ||
				(int16) ((typmod - VARHDRSZ) & 0xffff) < 0)
				return -1;
			return 8;
		case BOOLOID:
		case CHAROID:
			return 1;
		case INT2OID:
			return 2;
		case INT4OID:
		case DATEOID:
		case OIDOID:
			return 4;
		case INT8OID:
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case CASHOID:
			return 8;
		case FLOAT4OID:
			return key ? -1 : 4;
		case FLOAT8OID:
			return key ? -1 : 8;
		case TEXTOID:
		case VARCHAROID:
		case BYTEAOID:
			if (key && OidIsValid(collation) && !get_collation_isdeterministic(collation))
				return -1;
			return 4 + 32;		/* an offset, and a value's bytes as a guess */
		default:
			return -1;
	}
}

/*
 * V8e: whether VecAgg's device strategy would read its input's units itself
 * (exec/vecagg.c, device_setup_units()): the input a VecScan of a relation
 * whose source hands units (vexec_source.h, next_unit()), every key and
 * every aggregate's argument and FILTER a column of it, and its quals none,
 * or a filter the device applies to the units' rows (W4, §3.26;
 * vexec_devfilter_build()).  The device then reads and decodes the units --
 * the quals' columns too -- in place of the scan, which the price takes as
 * the scan's run at vexec.gpu_operator_factor of its CPU cost: measured
 * (§5, V8e), 20M rows of Parquet grouped serially in 36 to 110 ms on the
 * device where the CPU took 349 to 1,444.
 */

/* W4: a filter column for each of the relation's attributes, numbered as they come */
static int
plan_filter_column(void *arg, AttrNumber attnum, Oid typid, Oid collid)
{
	(void) attnum;
	(void) typid;
	(void) collid;
	return (*(int *) arg)++;
}
/*
 * The VecScan path a path is, or reads under a projection of its output --
 * the grouping's expressions computed over its columns, as Q35's ClientIP - 1
 * is -- which the scan's plan takes as its own target list; else NULL.
 */
static Path *
scan_under(Path *path)
{
	if (IsA(path, ProjectionPath))
		path = ((ProjectionPath *) path)->subpath;
	return vexec_is_scan_path(path) ? path : NULL;
}

/*
 * W9 (§3.27): the column an aggregate of sum(x + k) sums, as H8 makes it --
 * int4_sum over an int2 x and an int4 constant, which VecAgg's sum_offset()
 * (exec/vecagg.c) makes the sum and the count of x, k times the count added
 * as it finishes -- else NULL.  ClickBench's Q29 sums ResolutionWidth plus 0
 * to 89: one column crosses to the device, or is read by it, not ninety.
 */
static Expr *
h8_base(Aggref *a)
{
	HeapTuple	tup;
	Oid			transfn;
	Expr	   *arg;
	OpExpr	   *op;
	Expr	   *x;
	Node	   *c;
	int64		k;

	if (a->aggfilter != NULL || list_length(a->args) != 1 || a->aggdistinct != NIL || a->aggorder != NIL)
		return NULL;
	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(a->aggfnoid));
	if (!HeapTupleIsValid(tup))
		return NULL;
	transfn = ((Form_pg_aggregate) GETSTRUCT(tup))->aggtransfn;
	ReleaseSysCache(tup);
	arg = linitial_node(TargetEntry, a->args)->expr;
	if (transfn != F_INT4_SUM || !IsA(arg, OpExpr) || list_length(((OpExpr *) arg)->args) != 2)
		return NULL;
	op = (OpExpr *) arg;
	switch (get_opcode(op->opno))
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
			return NULL;
	}
	if (!IsA(c, Const) || ((Const *) c)->constisnull || exprType((Node *) x) != INT2OID)
		return NULL;
	k = DatumGetInt32(((Const *) c)->constvalue);
	if (get_opcode(op->opno) == F_INT24MI)
		k = -k;
	if (k > (int64) PG_INT32_MAX - PG_INT16_MAX || k < (int64) PG_INT32_MIN - PG_INT16_MIN)
		return NULL;
	return x;
}

/* W10: whether the device's machine computes an expression over the relation's columns */
static bool
machine_makes(Node *e, Index varno)
{
	int			counter = 0;
	int			type;
	Size		len;
	const char *why;

	return vexec_devexpr_build((Expr *) e, varno, plan_filter_column, &counter, &type, &len, &why) != NULL;
}

static bool
units_input(PlannerInfo *root, Path *input, List *keys, List *aggs, Bitmapset *kept, double *bytes,
			double *per_byte)
{
	RelOptInfo *rel = input->parent;
	ListCell   *lc;

	if (!vexec_enable_gpu_scan || scan_under(input) == NULL || rel == NULL ||
		rel->reloptkind != RELOPT_BASEREL || rel->rtekind != RTE_RELATION)
		return false;
	/* W4: the relation's quals, a filter the device applies to the units' rows */
	if (rel->baserestrictinfo != NIL)
	{
		int			counter = 0;
		Size		len;
		const char *why;

		if (vexec_devfilter_build(rel->baserestrictinfo, rel->relid, plan_filter_column, &counter,
								  &len, &why) == NULL)
			return false;
	}
	/* W10: a key, an argument, a FILTER the device's machine computes from the units' columns */
	foreach(lc, keys)
		if (!IsA(lfirst(lc), Var) && !machine_makes(lfirst(lc), rel->relid))
			return false;
	foreach(lc, aggs)
	{
		Aggref	   *aggref = lfirst(lc);
		Expr	   *arg;

		if (!IsA(aggref, Aggref))
			continue;
		/* W9: H8's sum of x + k reads x */
		arg = aggref->args != NIL ? linitial_node(TargetEntry, aggref->args)->expr : NULL;
		if ((arg != NULL && !IsA(arg, Var) && !(h8_base(aggref) != NULL && IsA(h8_base(aggref), Var)) &&
			 !machine_makes((Node *) arg, rel->relid)) ||
			(aggref->aggfilter != NULL && !IsA(aggref->aggfilter, Var) &&
			 !machine_makes((Node *) aggref->aggfilter, rel->relid)))
			return false;
	}
	{
		/*
		 * the columns the grouping reads -- its keys', its aggregates'
		 * arguments' and FILTERs' -- and the quals' (W4), their row groups the
		 * quals keep; W11, less those another device task of the statement
		 * reads first, which the device then keeps (W5): each of VecAggJoin's
		 * sides was priced reading the whole relation's columns, twice
		 */
		Bitmapset  *attrs = NULL;
		List	   *clauses = extract_actual_clauses(rel->baserestrictinfo, false);

		pull_varattnos((Node *) keys, rel->relid, &attrs);
		pull_varattnos((Node *) aggs, rel->relid, &attrs);
		pull_varattnos((Node *) clauses, rel->relid, &attrs);
		attrs = bms_del_members(attrs, kept);
		return vexec_units_bytes(root, rel, attrs, bytes, per_byte);
	}
}

/*
 * The bytes of a base relation's units the device reads for a set of its
 * attributes (pull_varattnos()'s form; its system columns not read) -- the
 * source's estimate of them (VC1), its quals' pruning in it -- and in
 * *per_byte the transfer cost's factor of its units' format; false where
 * its source hands no units.  Shared by VecAgg's device read (units_input())
 * and VecScan's (W13, paths.c).
 */
bool
vexec_units_bytes(PlannerInfo *root, RelOptInfo *rel, Bitmapset *attrs, double *bytes, double *per_byte)
{
	RangeTblEntry *rte;
	Relation	r;
	const VexecSourceRoutine *src;
	const char *how;
	bool		ok;

	rte = planner_rt_fetch(rel->relid, root);
	r = table_open(rte->relid, NoLock);
	src = vexec_source_for(r, &how);
	ok = src != NULL && VEXEC_SOURCE_HAS(src, next_unit) && VEXEC_SOURCE_HAS(src, read_unit);
	*bytes = 0;
	/*
	 * its units' format, by the source: Parquet's decoded on the device and
	 * Arrow's read as they are, at vexec.gpu_transfer_cost a byte;
	 * MergeTree's -- whose streams the source copies (read_unit_bytes()) --
	 * decompressed by nvCOMP, at three quarters of it
	 */
	if (ok && VEXEC_SOURCE_HAS(src, read_unit_bytes) && (src->name == NULL || strcmp(src->name, "arrow") != 0))
		*per_byte = 0.75;
	else
		*per_byte = 1.0;
	if (ok)
	{
		/* the needed columns' bytes in the files, as the source estimates them (VC1) */
		VexecSourceSpec spec;
		VexecPlanState *ps = vexec_plan_state(root);
		AttrNumber *attnums;
		double		rows = -1;
		double		b = -1;
		int			n = 0;
		int			i = -1;
		List	   *clauses = extract_actual_clauses(rel->baserestrictinfo, false);

		attnums = palloc(sizeof(AttrNumber) * Max(bms_num_members(attrs), 1));
		while ((i = bms_next_member(attrs, i)) >= 0)
			if (i + FirstLowInvalidHeapAttributeNumber > 0)
				attnums[n++] = i + FirstLowInvalidHeapAttributeNumber;
		memset(&spec, 0, sizeof(spec));
		spec.size = sizeof(spec);
		spec.ncolumns = n;
		spec.attnums = attnums;
		spec.quals = clauses;
		spec.max_rows = vexec_batch_rows;
		if (src->estimate != NULL)
			src->estimate(r, ActiveSnapshotSet() ? GetActiveSnapshot() : NULL, &spec, &rows, &b);
		*bytes = Max(b, 0);

		/*
		 * W7 (§3.27): the columns the device keeps between statements (W5),
		 * as its executor publishes them, priced as kept: none of their bytes
		 * read, decoded or copied -- each kept attribute's share of the
		 * relation's rows less of its bytes, as the source estimates them
		 * alone.  Which reads fill the cache stays the device's own (§8 item
		 * 21): a column is kept once the device has read it.  W20 (§3.28): a
		 * column priced as kept whole once another statement has asked the
		 * device for it, this statement's ask of it kept (exec/devasked.c);
		 * W50 (§3.36), while the statement's columns so priced fit the
		 * device's cache.
		 */
		for (int k = 0; k < n && src->estimate != NULL && *bytes > 0; k++)
		{
			double		all = Max(rel->tuples, rows);
			/* W50: the column's bytes as the device decodes them, its trial within the cache */
			double		decoded = all * vexec_devunit_row_bytes(rte->relid, 1, &attnums[k]);
			double		kept = vexec_device_priced_kept(rte->relid, attnums[k], ps != NULL ? ps->statement : 0,
														vexec_device_kept_rows(rte->relid, attnums[k]), all,
														decoded);
			VexecSourceSpec one = spec;
			double		r1 = -1;
			double		b1 = -1;

			if (kept <= 0 || all <= 0)
				continue;
			one.ncolumns = 1;
			one.attnums = &attnums[k];
			src->estimate(r, ActiveSnapshotSet() ? GetActiveSnapshot() : NULL, &one, &r1, &b1);
			*bytes = Max(*bytes - Min(kept / all, 1.0) * Max(b1, 0), 0);
		}
	}
	table_close(r, NoLock);
	return ok;
}

/*
 * W42 (§3.35): an expression's bytes on the device, its group's share of
 * them: a variable-length value's by the planner's width of its relation's
 * attribute -- its statistics' average, as set_rel_width() takes it -- an
 * offset beside it; device_width()'s w otherwise
 */
static double
device_bytes_of(PlannerInfo *root, Node *expr, int w)
{
	if (IsA(expr, Var) && get_typlen(exprType(expr)) == -1)
	{
		Var		   *v = (Var *) expr;

		if (v->varno > 0 && v->varno < root->simple_rel_array_size && v->varlevelsup == 0)
		{
			RelOptInfo *rel = root->simple_rel_array[v->varno];

			if (rel != NULL && rel->reloptkind == RELOPT_BASEREL && v->varattno >= rel->min_attr &&
				v->varattno <= rel->max_attr && rel->attr_widths[v->varattno - rel->min_attr] > 0)
				return sizeof(int32) + rel->attr_widths[v->varattno - rel->min_attr];
		}
	}
	return Max(w, 0);
}

/*
 * W17 (§3.28): whether the device applies a HAVING to its answer's groups, so
 * that the groups it drops never cross the bus nor are made on the CPU
 * (exec/vecagg.c): each aggregate in it one whose result is a device
 * aggregate's value (vexec_agg_device_result()), each grouping key a column
 * of the answer, and the whole of a form the units' filter takes
 * (vexec_devfilter_build()): comparisons with constants, IN, LIKE, IS NULL,
 * AND, OR and NOT.  The node decides again as it begins.
 */
typedef struct HavingMutate
{
	List	   *keys;
	int			next;
	bool		fast;
	bool		ok;
} HavingMutate;

static Node *
having_mutator(Node *node, HavingMutate *ctx)
{
	ListCell   *lc;

	if (node == NULL)
		return NULL;
	if (IsA(node, Aggref))
	{
		Aggref	   *a = (Aggref *) node;
		Oid			argtype = InvalidOid;
		int32		argtypmod = -1;

		if (a->args != NIL)
		{
			Expr	   *arg = linitial_node(TargetEntry, a->args)->expr;

			argtype = exprType((Node *) arg);
			argtypmod = exprTypmod((Node *) arg);
		}
		if (a->aggdistinct != NIL || a->aggorder != NIL || list_length(a->args) > 1 ||
			!vexec_agg_device_result(a->aggfnoid, a->aggsplit, argtype, argtypmod, a->inputcollid, ctx->fast))
			ctx->ok = false;
		return (Node *) makeVar(INDEX_VAR, (AttrNumber) ++ctx->next, a->aggtype, -1, a->aggcollid, 0);
	}
	foreach(lc, ctx->keys)
		if (equal(node, lfirst(lc)))
			return (Node *) makeVar(INDEX_VAR, (AttrNumber) ++ctx->next, exprType(node), exprTypmod(node),
									exprCollation(node), 0);
	return expression_tree_mutator(node, having_mutator, ctx);
}

static bool
having_device_ok(List *having, List *keys, bool fast)
{
	HavingMutate ctx;
	List	   *mutated;
	int			counter = 0;
	Size		len;
	const char *why;

	ctx.keys = keys;
	ctx.next = 0;
	ctx.fast = fast;
	ctx.ok = true;
	mutated = (List *) having_mutator((Node *) having, &ctx);
	if (!ctx.ok)
		return false;
	return vexec_devfilter_build(mutated, INDEX_VAR, plan_filter_column, &counter, &len, &why) != NULL;
}

/*
 * V8b: VecAgg's device strategy priced (cost.c, vexec_cost_agg_device())
 * where the node could take it as it begins (exec/vecagg.c): a hashed
 * first stage, or a whole aggregation, its keys of the types the device
 * groups by, its aggregates those whose states the device keeps.  It is
 * priced only where a device provider's executor serves at planning, as
 * the arena's header says; auto takes it where it is cheaper, force
 * wherever it is possible.  The node decides again as it begins, by
 * vexec.gpu then.
 *
 * W1 (§3.26): only where one task reads every row -- a serial input, not a
 * participant's share of a parallel one, whose partial groups the CPU would
 * then repartition and combine from the one participant whose task read the
 * units, millions of them (ClickBench's Q32 on mergetree: 143 ms on the
 * device, 1.2 s after it) -- so that its groups are final, and its last
 * answer is handed up from its pages, none made in the node's table.
 *
 * W3: the price adds the answer's groups as the CPU then has them, each
 * made from its row and handed up (cpu_tuple_cost at
 * vexec.cpu_tuple_factor, as the CPU strategy's own groups), beside their
 * bytes on the bus; a bounded sort above that the device ranks for gives
 * back what its cut leaves out (vexec_cost_topn()).
 */
static void
agg_device_price(PlannerInfo *root, Path *input, PathTarget *target, AggPathInfo *info,
				 VexecCost *cost)
{
	bool		fast = vexec_precision == VEXEC_PRECISION_FAST;
	double		row_bytes = 0;
	int			nkeys = 0;
	int			naggs = 0;
	List	   *aggs;
	List	   *keys;
	bool		missing;
	ListCell   *lc;
	VexecCost	dev;
	double		unit_bytes = 0;
	int64		topk = 0;		/* W14: a LIMIT's groups, without ORDER BY */
	List	   *h8_bases = NIL;	/* W9: the columns H8's sums read, each once */
	bool		numeric_keys = false;	/* W10: a key the device computes from its units alone */
	double		group_bytes = 16;	/* W42: a group's on the device -- its keys' and states' -- and its entry's */

	/* each path its own: a stage's info is copied from another's */
	info->device = false;
	info->units = false;
	info->group_bytes = 0;
	/* W9 (§3.27): a plain aggregation too, one group, the device's reductions */
	if (vexec_gpu == VEXEC_GPU_OFF || !vexec_enable_gpu_agg ||
		(info->strategy != AGG_HASHED && info->strategy != AGG_PLAIN) ||
		DO_AGGSPLIT_COMBINE(info->split) || info->sorted_input ||
		info->distinct_ref != 0 || info->distinct_combine)
		return;
	/* W1: one task reads every row, not a participant's share */
	if (input->parallel_workers > 0)
		return;
	if (vexec_device_ready() == NULL)
		return;

	/*
	 * The keys as the input computes them, by their sortgrouprefs: a stage
	 * of a distinct count groups by the count's argument too, which the
	 * query's own target list does not hold, nor a scan's -- H3's lower
	 * stage's own output does
	 */
	keys = group_exprs(info->groupclause, input->pathtarget, &missing);
	if (missing)
		keys = group_exprs(info->groupclause, target, &missing);
	if (missing)
		return;
	foreach(lc, keys)
	{
		Node	   *expr = lfirst(lc);
		int			w = device_width(exprType(expr), exprTypmod(expr), exprCollation(expr), true);

		/* W10: extract()'s numeric of an integer field, the device's INT64, from the units alone */
		if (w < 0 && exprType(expr) == NUMERICOID && vexec_devexpr_int_extract((Expr *) expr))
		{
			w = 8;
			numeric_keys = true;
		}
		if (w < 0)
			return;
		row_bytes += w;
		group_bytes += device_bytes_of(root, expr, w);
		nkeys++;
	}
	aggs = pull_var_clause((Node *) list_concat_copy(target->exprs, info->having),
						   PVC_INCLUDE_AGGREGATES | PVC_RECURSE_PLACEHOLDERS |
						   PVC_RECURSE_WINDOWFUNCS);

	/*
	 * C5: a LIMIT without ORDER BY, whose groups the node keeps on the CPU
	 * by their hashes (query_topk()).  W14 (§3.27): the device's whole
	 * grouping takes it -- one task reading every row, its groups final --
	 * the limit's groups handed up from its first page, the rest never read
	 * (exec/vecagg.c): any K groups are an answer; priced so, the limit's
	 * groups alone made and handed up.  A stage that is not the whole
	 * aggregation stays the CPU's, under which the device's never begins
	 * (device_setup()): ClickBench's Q17, priced as the device's, ran so on
	 * one CPU in V8f, 199 ms where 16 workers' took 52.
	 */
	{
		List	   *aggrefs = NIL;

		foreach(lc, aggs)
			if (IsA(lfirst(lc), Aggref))
				aggrefs = lappend(aggrefs, lfirst(lc));
		topk = query_topk(root, aggrefs, info->distinct_ref, info->distinct_combine);
		if (topk > 0 && (info->strategy != AGG_HASHED || info->split != AGGSPLIT_SIMPLE))
			return;
	}
	foreach(lc, aggs)
	{
		Aggref	   *aggref = lfirst(lc);
		Oid			argtype = InvalidOid;
		int32		argtypmod = -1;

		if (!IsA(aggref, Aggref))
			continue;
		/* W19: the mixed whole stages' one count(DISTINCT x), priced as a count of x -- the device keeps its pairs */
		if (info->distinct_whole && aggref->aggdistinct != NIL && list_length(aggref->aggdistinct) == 1 &&
			aggref->aggfnoid == F_COUNT_ANY && aggref->aggfilter == NULL && aggref->aggorder == NIL &&
			list_length(aggref->args) == 1)
			;
		else if (aggref->aggdistinct != NIL || aggref->aggorder != NIL || list_length(aggref->args) > 1)
			return;
		if (aggref->args != NIL)
		{
			Expr	   *arg = linitial_node(TargetEntry, aggref->args)->expr;
			Expr	   *base = h8_base(aggref);
			int			w;

			/*
			 * W9: H8's sum of x + k, the sum and count of x, x crossing once;
			 * the aggregate's own the argument's type, int4_sum's
			 */
			argtype = exprType((Node *) arg);
			argtypmod = exprTypmod((Node *) arg);
			if (base != NULL)
				arg = base;
			w = device_width(exprType((Node *) arg), exprTypmod((Node *) arg), InvalidOid, false);
			if (w < 0)
				return;
			if (base == NULL || !list_member(h8_bases, base))
				row_bytes += w;
			if (base != NULL)
				h8_bases = lappend(h8_bases, base);
		}
		if (aggref->aggfilter != NULL)
			row_bytes += 1;
		if (!vexec_agg_device_ok(aggref->aggfnoid, info->split, argtype, argtypmod, aggref->inputcollid, fast))
			return;
		/*
		 * W3: a stage that hands its states on, one of a reference type --
		 * avg's int8 array -- made a value for each of the device's groups,
		 * which the final stage reads again: ClickBench's Q9 with the device
		 * 1.7 s, 0.5 without
		 */
		if (DO_AGGSPLIT_SKIPFINAL(info->split) && OidIsValid(aggref->aggtranstype) &&
			!get_typbyval(aggref->aggtranstype))
			return;
		naggs++;
		group_bytes += 16;
	}
	/* a grouping of no key and no aggregate -- a GROUP BY of constants alone -- has nothing for it */
	if (nkeys == 0 && naggs == 0)
		return;
	{
		/*
		 * The device takes the node's own work, as the CPU strategy priced
		 * it -- its hashing and transitions, the path's cost past its
		 * input's -- at vexec.gpu_operator_factor: measured (§5, V8e), 20M
		 * rows grouped took 36 to 110 ms on the device serially.  Its rows'
		 * columns cross the bus, or, where the device reads its input's
		 * units itself, the scan's run is the device's read of their bytes
		 * (V8e, §3.25's N5): a Parquet file's decompressed and decoded
		 * there, and an Arrow file's copied as they are, at
		 * vexec.gpu_transfer_cost a byte -- measured again uncached (W7,
		 * §3.27), 0.2 to 0.6 ns a compressed byte of Parquet past the
		 * query's 10 ms, its own files' and an external file's (ClickBench's
		 * Q4, Q8, Q15, Q32 to Q35), where the CPU's 16 workers took 2 to 28
		 * us a cost unit: at the three times V8e priced them, the device
		 * lost Q33 by cost, 102 ms against 1,028 -- a MergeTree part's
		 * compressed streams copied from its pages and decompressed by
		 * nvCOMP at three quarters of it (W6, §3.26) -- measured, 0.3 to
		 * 0.45 ns a byte with the copy, where the CPU's scans of mergetree
		 * took 6 to 7 us a cost unit; and a column the device keeps (W5)
		 * at none (W7, units_input()).  Its
		 * answer's groups come back over the bus, and are each made from
		 * its row, finished and handed up on the CPU as the CPU strategy's
		 * own are, at its price past its start (W3, above): so the path's
		 * start is the input's that the device does not take, the task's,
		 * the bus's and the device's share of the CPU strategy's work before
		 * its first group, and its run the CPU strategy's.
		 */
		Cost		own = Max(cost->startup - input->total_cost, 0);
		Cost		out = Max(cost->total - cost->startup, 0);
		/* W14: under a LIMIT without ORDER BY, its groups alone made and handed up */
		double		groups_out = topk > 0 ? Min((double) topk, Max(info->numgroups, 1)) : info->numgroups;
		double		per_byte = 1.0;
		bool		units = units_input(root, input, keys, aggs, info->kept_attrs, &unit_bytes, &per_byte);
		Cost		saved = 0;
		Cost		extra;
		double		bytes;

		if (units)
		{
			saved = input->total_cost - input->startup_cost;
			bytes = unit_bytes * per_byte;
		}
		else if (numeric_keys)
			return;
		else
			bytes = input->rows * row_bytes;
		/* W17: a HAVING the device applies to its answer's groups: those it drops never cross */
		if (info->having != NIL && having_device_ok(info->having, keys, fast))
		{
			Selectivity sel = clauselist_selectivity(root, info->having, 0, JOIN_INNER, NULL);

			groups_out *= sel;
			out *= sel;
		}
		extra = vexec_cost_agg_device(own, bytes, groups_out * (nkeys * 8 + naggs * 16 + 16));
		if (topk > 0)
			out *= groups_out / Max(info->numgroups, 1);
		dev = *cost;
		dev.startup = Max(input->total_cost - saved, input->startup_cost) + extra;
		dev.total = dev.startup + out;

		/*
		 * W42 (§3.35): its groups on the device within the memory the
		 * device's tasks have not reserved, as the planner's estimate of them
		 * says: past it, the CPU's but where forced -- its parallel plan
		 * where it has one, not the device's serial one (W1), whose answer
		 * would find no memory and whose input the CPU would then read
		 * again in one process
		 */
		if (vexec_gpu != VEXEC_GPU_FORCE &&
			Max(info->numgroups, 1) * group_bytes > (double) vexec_device_plan_bytes())
			return;
		if (dev.total < cost->total || vexec_gpu == VEXEC_GPU_FORCE || info->device_always)
		{
			*cost = dev;
			info->device = true;
			info->units = units;
			/* W45 (§3.35): what the node's task asks admission for, as W42 held it */
			info->group_bytes = group_bytes;
		}
	}
}

static Path *
agg_path(PlannerInfo *root, RelOptInfo *rel, Path *input, PathTarget *target,
		 AggPathInfo *info, const VexecCost *cost_in)
{
	CustomPath *cp = makeNode(CustomPath);
	VexecCost	costs = *cost_in;
	const VexecCost *cost = &costs;

	agg_device_price(root, input, target, info, &costs);

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = target;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = rel->consider_parallel && input->parallel_safe;
	cp->path.parallel_workers = input->parallel_workers;
	cp->path.rows = info->strategy == AGG_PLAIN ? 1 : info->numgroups;
	cp->path.disabled_nodes = input->disabled_nodes;
	cp->path.startup_cost = cost->startup;
	cp->path.total_cost = cost->total;
	cp->path.pathkeys = NIL;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = list_make1(input);
	cp->custom_private = list_make4(makeInteger(info->distinct_ref),
									makeInteger(info->distinct_combine ? 1 : 0),
									makeInteger(info->sorted_input ? 1 : 0),
									makeInteger(info->device ? 1 : 0));
	cp->custom_private = list_concat(list_make5(makeInteger(info->strategy), makeInteger(info->split),
												makeFloat(psprintf("%.1f", info->numgroups)),
												info->groupclause, info->having),
									 cp->custom_private);
	cp->custom_private = lappend(cp->custom_private, makeInteger(info->whole ? 1 : 0));
	cp->custom_private = lappend(cp->custom_private, makeFloat(psprintf("%.1f", info->group_bytes)));
	cp->methods = &vecagg_path_methods;
	return &cp->path;
}

/*
 * Whether a path is a VecAgg the device's strategy takes: its own, or W1's
 * two stages of a count(DISTINCT), this the upper one.
 */
bool
vexec_agg_path_device(Path *path)
{
	CustomPath *cp = (CustomPath *) path;

	return IsA(path, CustomPath) && cp->methods == &vecagg_path_methods &&
		((list_length(cp->custom_private) > 8 && intVal(list_nth(cp->custom_private, 8)) != 0) ||
		 (list_length(cp->custom_private) > 9 && intVal(list_nth(cp->custom_private, 9)) != 0));
}

/*
 * W7 (§3.27): the grouped relation's device path, kept aside for its ORDER
 * BY ... LIMIT.  Priced with every group handed up, it may be the dearer
 * path at the grouped relation, where add_path() drops it, though a bounded
 * VecSort above gives back what the device's rank saves (vexec_cost_topn()),
 * which add_path() never sees: ClickBench's Q36 on mergetree, its device
 * VecAgg 1,394.75..1,614.30 against the CPU's 1,202.92..1,422.47, and the
 * whole plans 741.56 against 1,807.21.  A copy of the cheapest of `paths`
 * that the device takes and a bounded sort may lend its rank to -- which no
 * path list holds and so nothing frees -- stays for vexec_consider_sort(),
 * as VecScans do for H6 (sort.c, vexec_sort_keep_scan()).
 */
/*
 * W9 (§3.27): whether the device may read the input relation's units itself
 * and filter them by its quals, many rows of them -- vexec.min_rows of the
 * relation's own, where its quals leave fewer: ClickBench's Q20 counts 839
 * rows of URL LIKE '%google%' by the planner's estimate, of ten million the
 * device reads and filters in a few milliseconds, where 16 workers took 72
 */
static bool
device_reads_many(RelOptInfo *input_rel)
{
	return vexec_gpu != VEXEC_GPU_OFF && vexec_enable_gpu_agg && vexec_enable_gpu_scan &&
		input_rel->reloptkind == RELOPT_BASEREL && input_rel->rtekind == RTE_RELATION &&
		input_rel->tuples >= vexec_min_rows && vexec_device_ready() != NULL;
}

/* Whether vexec.min_rows refuses the input's rows: fewer, and the device reads no units itself (W9). */
static bool
few_rows(VexecPlanState *ps, RelOptInfo *input_rel, Path *input)
{
	return ps->mode != VEXEC_MODE_FORCE && input->rows < vexec_min_rows && !device_reads_many(input_rel);
}

/*
 * F20 (§3.38): whether a path a vector aggregation would read is a vector
 * scan's.  vexec.min_rows keeps a grouping of a few rows on PostgreSQL's
 * executor where its input is rows, which a vector node would convert; a
 * vector scan's batches are vector already, and PostgreSQL's Sort or
 * HashAggregate would read them as rows: ClickBench's Q40 and Q41 on
 * mergetree, 460 and 718 rows estimated over a parallel vector scan, 22.9
 * and 24.7 ms on PostgreSQL's grouping, 15.4 and 17.4 on VecAgg's.
 */
static bool
vector_scan_input(Path *path)
{
	if (path != NULL && IsA(path, ProjectionPath))
		path = ((ProjectionPath *) path)->subpath;
	return vexec_enable_agg_below_min_rows && path != NULL &&
		(vexec_is_scan_path(path) || vexec_is_device_scan_path(path));
}

/* The input's cheapest partial path, where a partial and final pair may read it; else NULL. */
static Path *
pair_input(RelOptInfo *input_rel, RelOptInfo *output_rel, GroupPathExtraData *extra)
{
	if ((extra->flags & GROUPING_CAN_PARTIAL_AGG) == 0 || input_rel->partial_pathlist == NIL ||
		!output_rel->consider_parallel)
		return NULL;
	return linitial(input_rel->partial_pathlist);
}

/*
 * F22 (§3.38): a vector scan a vector aggregation reads, without the
 * grouping expressions M3 left out of its keys: a copy of the path, its
 * target the scan's less them, so that the scan computes them for no row.
 * The aggregation reads none of them -- its output computes them from the
 * kept keys, whose columns stay -- nor do its aggregates' arguments, which
 * read the kept columns where no whole expression matches (exec_aggref()).
 * Q35's ClientIP - 1 to - 3: three kernel steps over every row, about 7% of
 * its cycles at 100M rows.  A projection of the scan's output -- where the
 * grouping's expressions made the scan/join target another than the
 * scan's, apply_scanjoin_target_to_paths() puts one over it -- is copied
 * so instead: the scan's plan takes its target as its own target list
 * (CUSTOMPATH_SUPPORT_PROJECTION).  Any other input as it is.
 */
static Path *
scan_without_implied(PlannerInfo *root, Path *input, List *implied)
{
	Path	   *scan = IsA(input, ProjectionPath) ? ((ProjectionPath *) input)->subpath : input;
	PathTarget *target;

	if (!vexec_drop_implied_key_inputs || implied == NIL ||
		!(vexec_is_scan_path(scan) || vexec_is_device_scan_path(scan)))
		return input;
	target = without_implied(root, input->pathtarget, implied);
	if (list_length(target->exprs) == list_length(input->pathtarget->exprs))
		return input;
	if (input != scan)
	{
		ProjectionPath *copy = palloc(sizeof(ProjectionPath));

		memcpy(copy, input, sizeof(ProjectionPath));
		copy->path.pathtarget = target;
		return &copy->path;
	}
	else
	{
		CustomPath *copy = palloc(sizeof(CustomPath));

		memcpy(copy, input, sizeof(CustomPath));
		copy->path.pathtarget = target;
		return &copy->path;
	}
}

/* W7: whether the query's ORDER BY ... LIMIT may lend a grouping its rank: a bound of constants */
static bool
ranked_above(PlannerInfo *root)
{
	return root->sort_pathkeys != NIL && vexec_gpu != VEXEC_GPU_OFF && vexec_enable_gpu_agg &&
		vexec_query_bound(root) > 0;
}

typedef struct VexecKeptAgg
{
	RelOptInfo *rel;
	Path	   *path;
} VexecKeptAgg;

static void
keep_device_path(VexecPlanState *ps, RelOptInfo *rel, List *paths)
{
	Path	   *best = NULL;
	VexecKeptAgg *kept = NULL;
	MemoryContext old;
	CustomPath *copy;
	ListCell   *lc;

	foreach(lc, paths)
	{
		Path	   *p = lfirst(lc);

		if (vexec_agg_path_device(p) && vexec_agg_path_ranks(p) &&
			(best == NULL || p->total_cost < best->total_cost))
			best = p;
	}
	if (best == NULL)
		return;
	foreach(lc, ps->kept_aggs)
		if (((VexecKeptAgg *) lfirst(lc))->rel == rel)
			kept = lfirst(lc);
	if (kept != NULL && kept->path->total_cost <= best->total_cost)
		return;
	old = MemoryContextSwitchTo(ps->mcxt);
	copy = palloc(sizeof(CustomPath));
	memcpy(copy, best, sizeof(CustomPath));
	if (kept == NULL)
	{
		kept = palloc0(sizeof(VexecKeptAgg));
		kept->rel = rel;
		ps->kept_aggs = lappend(ps->kept_aggs, kept);
	}
	kept->path = &copy->path;
	MemoryContextSwitchTo(old);
}

/* W7: a grouped relation's device VecAgg path kept aside, or NULL (sort.c) */
Path *
vexec_agg_kept_device(VexecPlanState *ps, RelOptInfo *rel)
{
	ListCell   *lc;

	foreach(lc, ps->kept_aggs)
		if (((VexecKeptAgg *) lfirst(lc))->rel == rel)
			return ((VexecKeptAgg *) lfirst(lc))->path;
	return NULL;
}

/*
 * W1 (§3.26): the input the device's whole grouping reads, where the
 * cheapest is not a serial VecScan -- a Gather of a parallel one, whose rows
 * would cross the Gather to reach the one task: the relation's cheapest
 * serial VecScan, whose rows the task takes, or whose units the device reads
 * itself (V8e).  NULL where the cheapest is one, or there is none, or no
 * device serves.
 */
static Path *
device_input(RelOptInfo *input_rel, Path *input)
{
	Path	   *best = NULL;
	ListCell   *lc;

	if (vexec_gpu == VEXEC_GPU_OFF || !vexec_enable_gpu_agg || vexec_device_ready() == NULL ||
		(scan_under(input) != NULL && input->parallel_workers == 0))
		return NULL;
	foreach(lc, input_rel->pathlist)
	{
		Path	   *p = lfirst(lc);

		if (scan_under(p) != NULL && p->parallel_workers == 0 && PATH_REQ_OUTER(p) == NULL &&
			(best == NULL || p->total_cost < best->total_cost))
			best = p;
	}
	return best;
}

/* Whether a VecAgg path, or the one it reads -- H3's lower stage -- is the device's. */
static bool
path_takes_device(Path *path)
{
	CustomPath *cp = (CustomPath *) path;

	/* W11: a VecAggJoin of two sides the device's */
	if (IsA(path, CustomPath) && cp->methods == &vecaggjoin_path_methods)
		return list_length(cp->custom_paths) == 2 && path_takes_device(linitial(cp->custom_paths)) &&
			path_takes_device(lsecond(cp->custom_paths));
	return vexec_agg_path_device(path) ||
		(IsA(path, CustomPath) && cp->methods == &vecagg_path_methods && cp->custom_paths != NIL &&
		 vexec_agg_path_device(linitial(cp->custom_paths)));
}

/*
 * W1: vexec.gpu = force takes the device wherever it can: where a grouping's
 * paths have one the device takes, the others are disabled beside it --
 * PostgreSQL's own the relation holds already too, and the grouped
 * relation's partial paths, which a Gather Merge would sort -- as the device
 * no longer takes a parallel plan's partial stages.  With vexec's alone
 * disabled, ClickBench's Q28 ran PostgreSQL's parallel aggregation, 3.1 to
 * 3.5 times the CPU's time, neither vexec's nor the device's: its
 * MIN(Referer) the device's since W12, a bounded sort over its device path
 * cost 117,200 where PostgreSQL's plan cost 117,149.
 */
static void
device_forced(RelOptInfo *output_rel, List *paths)
{
	ListCell   *lc;
	bool		any = false;

	if (vexec_gpu != VEXEC_GPU_FORCE)
		return;
	foreach(lc, paths)
		any |= path_takes_device(lfirst(lc));
	if (!any)
		return;
	foreach(lc, paths)
		if (!path_takes_device(lfirst(lc)))
			((Path *) lfirst(lc))->disabled_nodes++;
	foreach(lc, output_rel->pathlist)
		if (!path_takes_device(lfirst(lc)))
			((Path *) lfirst(lc))->disabled_nodes++;
	foreach(lc, output_rel->partial_pathlist)
		((Path *) lfirst(lc))->disabled_nodes++;
}

/*
 * W3 (§3.26), the plan's shape: what a split plan's partial groups cost as
 * they cross a VecRepartition and are combined, where repart_path() prices
 * them at a tuple's cost each way -- measured with 16 workers over
 * ClickBench's 10M rows, 0.43 to 1 us a participant's group of fixed-width
 * keys (Q4, Q15, Q16, Q35), up to 3 us with strings (Q5, Q12, Q33): 2.2 us
 * a cost unit, a group's at 0.2 and each byte of its width at 0.012.  Added
 * where the device's whole grouping is priced beside the split, which a
 * GPU's one global table has no use for; a plan without the device is
 * priced as it was.
 */
#define VEXEC_SPLIT_GROUP_COST	0.2
#define VEXEC_SPLIT_BYTE_COST	0.012

/* W3: a split path's groups dealt out by the repartition `rep` below it, where `shape` is set */
static void
split_shape_price(Path *path, Path *rep, double shape)
{
	Cost		c;

	if (shape <= 0)
		return;
	c = rep->rows * (shape + VEXEC_SPLIT_BYTE_COST * Max(rep->pathtarget->width, 8));
	path->startup_cost += c;
	path->total_cost += c;
}

/* W3: the shape's price where the device's whole grouping is among the paths, else 0 */
static double
split_shape(List *paths)
{
	ListCell   *lc;

	foreach(lc, paths)
		if (path_takes_device(lfirst(lc)))
			return VEXEC_SPLIT_GROUP_COST;
	return 0;
}

/*
 * A VecRepartition path over a partial VecAgg path (H5): parallel-aware,
 * its rows each participant's share of the partial groups.  Each group is
 * written once and read once (exec/vecrepart.c), at a tuple's cost each
 * way and the pages it takes, after the whole input.
 */
static Path *
repart_path(PlannerInfo *root, RelOptInfo *rel, Path *input, List *groupclause)
{
	CustomPath *cp = makeNode(CustomPath);
	double		pages = ceil(input->rows * Max(input->pathtarget->width, 8) / BLCKSZ);
	Cost		io = 2 * (cpu_tuple_cost * input->rows + seq_page_cost * pages);

	(void) root;
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = input->pathtarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = true;
	cp->path.parallel_safe = true;
	cp->path.parallel_workers = input->parallel_workers;
	cp->path.rows = input->rows;
	cp->path.disabled_nodes = input->disabled_nodes;
	cp->path.startup_cost = input->total_cost + io + vexec_batch_setup_cost;
	cp->path.total_cost = cp->path.startup_cost;
	cp->path.pathkeys = NIL;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = list_make1(input);
	cp->custom_private = list_make1(groupclause);
	cp->methods = &vecrepart_path_methods;
	return &cp->path;
}

/*
 * VecRepartition's plan: its keys the child's grouping columns, by their
 * sortgrouprefs, with the grouping's equality operators; its scan tuple the
 * child's row, which planner_shutdown_hook makes references to the child's
 * columns (join.c).
 */
static Plan *
plan_vecrepart(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			   List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *child = linitial(custom_plans);
	List	   *groupclause = linitial(best_path->custom_private);
	VexecRepartPlan plan;
	List	   *scan_tlist = NIL;
	ListCell   *lc;

	(void) root;
	(void) rel;
	(void) clauses;
	memset(&plan, 0, sizeof(plan));
	foreach(lc, groupclause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		TargetEntry *tle = get_sortgroupref_tle(sgc->tleSortGroupRef, child->targetlist);

		plan.keycols = lappend_int(plan.keycols, tle->resno);
		plan.eqops = lappend_oid(plan.eqops, sgc->eqop);
		plan.collations = lappend_oid(plan.collations, exprCollation((Node *) tle->expr));
	}
	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		scan_tlist = lappend(scan_tlist, makeTargetEntry(copyObject(tle->expr),
														 list_length(scan_tlist) + 1, NULL, false));
	}
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.plan.lefttree = child;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = vexec_repart_plan_encode(&plan);
	cscan->methods = vexec_repart_methods();
	return &cscan->scan.plan;
}

/*
 * VecRepartition over a partial stage gp_orca's parallel pass made on one
 * node, between it and the final stage it puts over it in each participant
 * (gp_orca_vec.h's build_repartition, plan/orca.c): its keys columns of the
 * child's row, by their resnos, with the grouping's equality operators; its
 * scan tuple the child's row, references to its columns as the planner's
 * finished plan has them (join.c), and its target list that row, as it is.
 */
Plan *
vexec_build_repart(Plan *child, int nkeys, const AttrNumber *keycols, const Oid *eqops,
				   const Oid *collations)
{
	CustomScan *cscan = makeNode(CustomScan);
	VexecRepartPlan plan;
	ListCell   *lc;

	memset(&plan, 0, sizeof(plan));
	for (int i = 0; i < nkeys; i++)
	{
		plan.keycols = lappend_int(plan.keycols, keycols[i]);
		plan.eqops = lappend_oid(plan.eqops, eqops[i]);
		plan.collations = lappend_oid(plan.collations, collations[i]);
	}
	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Var		   *outer = makeVarFromTargetEntry(OUTER_VAR, tle);
		Var		   *scan = makeVarFromTargetEntry(INDEX_VAR, tle);

		outer->varnosyn = 0;
		outer->varattnosyn = 0;
		scan->varattno = (AttrNumber) (list_length(cscan->custom_scan_tlist) + 1);
		scan->varnosyn = 0;
		scan->varattnosyn = 0;
		cscan->custom_scan_tlist = lappend(cscan->custom_scan_tlist,
										   makeTargetEntry((Expr *) outer, scan->varattno,
														   NULL, false));
		cscan->scan.plan.targetlist = lappend(cscan->scan.plan.targetlist,
											  makeTargetEntry((Expr *) scan, tle->resno,
															  tle->resname, tle->resjunk));
	}
	cscan->scan.plan.startup_cost = child->total_cost;
	cscan->scan.plan.total_cost = child->total_cost;
	cscan->scan.plan.plan_rows = child->plan_rows;
	cscan->scan.plan.plan_width = child->plan_width;
	cscan->scan.plan.parallel_aware = true;
	cscan->scan.plan.parallel_safe = true;
	cscan->scan.plan.lefttree = child;
	cscan->scan.scanrelid = 0;
	cscan->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cscan->custom_private = vexec_repart_plan_encode(&plan);
	cscan->methods = vexec_repart_methods();
	return &cscan->scan.plan;
}

/* The partially grouped relation core made for this grouped one, if any. */
static RelOptInfo *
partial_grouped_rel(PlannerInfo *root, RelOptInfo *grouped)
{
	ListCell   *lc;

	foreach(lc, root->upper_rels[UPPERREL_PARTIAL_GROUP_AGG])
	{
		RelOptInfo *rel = lfirst(lc);

		if (bms_equal(rel->relids, grouped->relids))
			return rel;
	}
	return NULL;
}

/* "1 aggregate", "2 aggregates" */
static const char *
count_of(int n, const char *one, const char *many)
{
	return psprintf("%d %s", n, n == 1 ? one : many);
}

/* ---------------------------------------------------------------------
 * H3: a DISTINCT aggregate in two VecAggs (§3.14)
 * ---------------------------------------------------------------------
 */

/*
 * Why an aggregate does not split into a partial and a final stage whose
 * answer is the same bits as one pass's, or NULL where it does.
 */
static const char *
agg_split_refusal(Aggref *aggref)
{
	HeapTuple	tup;
	Form_pg_aggregate aggform;
	const char *refusal = NULL;

	if (aggref->aggdistinct != NIL || aggref->aggorder != NIL ||
		aggref->aggkind != AGGKIND_NORMAL || aggref->aggdirectargs != NIL)
		return "an aggregate beside the DISTINCT one with DISTINCT or ORDER BY of its own";
	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggref->aggfnoid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for aggregate %u", aggref->aggfnoid);
	aggform = (Form_pg_aggregate) GETSTRUCT(tup);
	if (!OidIsValid(aggform->aggcombinefn) ||
		(aggform->aggtranstype == INTERNALOID &&
		 (!OidIsValid(aggform->aggserialfn) || !OidIsValid(aggform->aggdeserialfn))))
		refusal = "an aggregate beside the DISTINCT one that has no partial state";

	/*
	 * A float's sums, combined from a state a distinct value, add in another
	 * order than PostgreSQL's one pass over the rows, and differ in their
	 * last bits.
	 */
	else if (aggform->aggtranstype == FLOAT4OID || aggform->aggtranstype == FLOAT8OID ||
			 aggform->aggtranstype == FLOAT8ARRAYOID)
		refusal = "a float aggregate beside the DISTINCT one, whose sums a second stage would add in another order";
	ReleaseSysCache(tup);
	return refusal;
}

/* The transition function of an aggregate. */
static Oid
agg_transfn(Oid aggfnoid)
{
	HeapTuple	tup = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggfnoid));
	Oid			fn;

	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for aggregate %u", aggfnoid);
	fn = ((Form_pg_aggregate) GETSTRUCT(tup))->aggtransfn;
	ReleaseSysCache(tup);
	return fn;
}

/*
 * Why a grouping with a DISTINCT aggregate does not take H3's two stages,
 * or NULL where it does: one count(DISTINCT x) over a column the input
 * gives, x's equality its bytes', every other aggregate one that splits.
 * count's answer is the number of distinct values whatever order they come
 * in; an aggregate whose answer follows the order -- a float's sum,
 * array_agg -- would see them in the hash table's order, not the sort's
 * PostgreSQL's DISTINCT gives them in, and stays the row Agg's.
 */
static const char *
distinct_refusal(AggScan *as, PathTarget *input_target, Aggref **dp)
{
	Aggref	   *d;
	SortGroupClause *dsgc;
	Expr	   *x;
	Oid			coll;
	ListCell   *lc;

	if (list_length(as->distinct) != 1)
		return "more than one DISTINCT aggregate";
	d = linitial(as->distinct);
	if (d->aggorder != NIL || d->aggfilter != NULL || list_length(d->args) != 1 ||
		list_length(d->aggdistinct) != 1)
		return "a DISTINCT aggregate with ORDER BY, FILTER or more than one argument";
	if (agg_transfn(d->aggfnoid) != F_INT8INC_ANY)
		return "a DISTINCT aggregate other than count";
	x = linitial_node(TargetEntry, d->args)->expr;
	if (!list_member(input_target->exprs, x))
		return "a DISTINCT argument the input does not give";
	dsgc = linitial_node(SortGroupClause, d->aggdistinct);
	if (!dsgc->hashable)
		return "a DISTINCT argument that cannot be hashed";
	coll = exprCollation((Node *) x);
	if (OidIsValid(coll) && !get_collation_isdeterministic(coll))
		return "a DISTINCT argument of a nondeterministic collation";
	foreach(lc, as->aggrefs)
	{
		Aggref	   *a = lfirst_node(Aggref, lc);
		const char *r;

		if (a == d || equal(a, d))
			continue;
		if ((r = agg_split_refusal(a)) != NULL)
			return r;
	}
	*dp = d;
	return NULL;
}

/*
 * X1 (§3.18): H3's stages in every participant of a parallel plan, over the
 * input's cheapest partial path:
 *
 *	1. the lower VecAgg, partial, by the keys and the argument, the other
 *	   aggregates' partial states beside;
 *	2. a VecRepartition by the keys and the argument, so that each pair's
 *	   states meet in one participant;
 *	3. there, a VecAgg by the keys and the argument that combines them and
 *	   hands them on partial: a row a distinct pair;
 *	4. the upper VecAgg by the keys, partial: a count of the participant's
 *	   distinct values a key, the other states combined;
 *	5. a Gather and the final stage, which adds the counts and finalizes the
 *	   other states; or, with keys, H5's shape: a VecRepartition by the keys
 *	   and the final stage in each participant, under a Gather.
 *
 * A pair's distinct values are disjoint between participants, so the counts
 * add up to PostgreSQL's count(DISTINCT).  The other aggregates are those
 * H3 takes, which split and combine to the bits of one pass.  The paths are
 * the grouped relation's, and the final stage in each participant one of
 * its partial paths too, which create_ordered_paths() sorts in each
 * participant under a Gather Merge, as H5's.  NIL where the input has no
 * partial path, or VecRepartition is off.
 */
static List *
distinct_parallel_paths(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
						VexecPlanState *ps, VexecAlt *alt, AggScan *as, Aggref *d,
						List *groupclause, List *having, double numgroups, PathTarget *lt,
						List *lgroupclause, List *gexprs, Index xref, bool add,
						PathTarget *ftarget, double shape)
{
	Path	   *input = input_rel->cheapest_total_path;
	Path	   *pinput;
	AggStrategy strategy = root->parse->groupClause != NIL ? AGG_HASHED : AGG_PLAIN;
	AggSplit	isplit = VEXEC_AGGSPLIT_INTERMEDIATE;
	int			nkeys_kernel = count_kernel_keys(groupclause, as->groupexprs);
	int			lkeys_kernel = count_kernel_keys(lgroupclause, gexprs);
	List	   *others = NIL;
	PathTarget *ut;
	double		divisor;
	double		lgroups_each;	/* a participant's pairs, before the
								 * repartition */
	double		pairs_each;		/* and after it */
	double		ugroups_each;	/* a participant's keys */
	double		gathered;
	AggClauseCosts pcosts;
	AggClauseCosts icosts;
	AggClauseCosts fcosts;
	AggPath    *row;
	VexecCost	cost;
	AggPathInfo info;
	Path	   *lpart;
	Path	   *lmid;
	Path	   *upart;
	Path	   *gather;
	List	   *paths = NIL;
	ListCell   *lc;

	if (!vexec_enable_repartition || input_rel->partial_pathlist == NIL ||
		!output_rel->consider_parallel)
		return NIL;
	pinput = linitial(input_rel->partial_pathlist);

	/* the stages read the columns the serial pair reads */
	if (pinput->parallel_workers <= 0 || PATH_REQ_OUTER(pinput) != NULL ||
		!equal(pinput->pathtarget->exprs, input->pathtarget->exprs))
		return NIL;

	foreach(lc, as->aggrefs)
		if (lfirst(lc) != d && !equal(lfirst(lc), d))
			others = lappend(others, lfirst(lc));
	divisor = vexec_parallel_divisor(pinput);
	lgroups_each = Min(estimate_num_groups(root, gexprs, pinput->rows, NULL, NULL), pinput->rows);
	pairs_each = clamp_row_est(Min(estimate_num_groups(root, gexprs, input->rows, NULL, NULL),
								   input->rows) / divisor);
	ugroups_each = strategy == AGG_PLAIN ? 1 : Min(numgroups, pairs_each);

	MemSet(&pcosts, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_INITIAL_SERIAL, &pcosts);
	MemSet(&icosts, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, isplit, &icosts);
	MemSet(&fcosts, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &fcosts);

	/* 1: the lower stage, partial, over each participant's share of the input */
	row = create_agg_path(root, output_rel, pinput, lt, AGG_HASHED, AGGSPLIT_INITIAL_SERIAL,
						  lgroupclause, NIL, &pcosts, lgroups_each);
	vexec_cost_agg(root, &row->path, pinput, vexec_is_vector_path(pinput),
				   list_length(lgroupclause), lkeys_kernel,
				   count_vector_aggs(others, AGGSPLIT_INITIAL_SERIAL, NULL), lgroups_each, &cost);
	memset(&info, 0, sizeof(info));
	info.strategy = AGG_HASHED;
	info.split = AGGSPLIT_INITIAL_SERIAL;
	info.numgroups = lgroups_each;
	info.groupclause = lgroupclause;
	lpart = agg_path(root, output_rel, pinput, lt, &info, &cost);

	/* 2 and 3: dealt out by the keys and the argument, and combined */
	lmid = repart_path(root, output_rel, lpart, lgroupclause);
	row = create_agg_path(root, output_rel, lmid, lt, AGG_HASHED, isplit, lgroupclause, NIL,
						  &icosts, pairs_each);
	vexec_cost_agg(root, &row->path, lmid, true, list_length(lgroupclause), lkeys_kernel, 0,
				   pairs_each, &cost);
	info.split = isplit;
	info.numgroups = pairs_each;
	{
		Path	   *lrep = lmid;

		lmid = agg_path(root, output_rel, lmid, lt, &info, &cost);
		split_shape_price(lmid, lrep, shape);	/* W3 */
	}

	/*
	 * 4: the upper stage, partial: the keys, the first row's columns, the
	 * DISTINCT aggregate's partial form -- a count of x, a value a row --
	 * and the other aggregates' states, combined.
	 */
	ut = create_empty_pathtarget();
	foreach(lc, groupclause)
		add_column_to_pathtarget(ut, (Expr *) list_nth(as->groupexprs, foreach_current_index(lc)),
								 lfirst_node(SortGroupClause, lc)->tleSortGroupRef);
	foreach(lc, as->extras)
		add_column_to_pathtarget(ut, lfirst(lc), 0);
	foreach(lc, as->aggrefs)
	{
		Aggref	   *pa = makeNode(Aggref);

		memcpy(pa, lfirst(lc), sizeof(Aggref));
		mark_partial_aggref(pa, AGGSPLIT_INITIAL_SERIAL);
		add_column_to_pathtarget(ut, (Expr *) pa, 0);
	}
	set_pathtarget_cost_width(root, ut);
	row = create_agg_path(root, output_rel, lmid, ut, strategy, isplit, groupclause, NIL,
						  &icosts, ugroups_each);
	vexec_cost_agg(root, &row->path, lmid, true, list_length(groupclause), nkeys_kernel, 1,
				   ugroups_each, &cost);
	memset(&info, 0, sizeof(info));
	info.strategy = strategy;
	info.split = isplit;
	info.numgroups = ugroups_each;
	info.groupclause = groupclause;
	info.distinct_ref = xref;
	upart = agg_path(root, output_rel, lmid, ut, &info, &cost);

	/* 5: the participants' counts gathered, and added in the final stage */
	gathered = clamp_row_est(ugroups_each * divisor);
	gather = (Path *) create_gather_path(root, output_rel, upart, ut, NULL, &gathered);
	row = create_agg_path(root, output_rel, gather, ftarget, strategy,
						  AGGSPLIT_FINAL_DESERIAL, groupclause, having, &fcosts,
						  strategy == AGG_PLAIN ? 1 : numgroups);
	vexec_cost_agg(root, &row->path, gather, false, list_length(groupclause), nkeys_kernel, 0,
				   strategy == AGG_PLAIN ? 1 : numgroups, &cost);
	memset(&info, 0, sizeof(info));
	info.strategy = strategy;
	info.split = AGGSPLIT_FINAL_DESERIAL;
	info.numgroups = strategy == AGG_PLAIN ? 1 : numgroups;
	info.groupclause = groupclause;
	info.having = having;
	info.distinct_combine = true;
	paths = lappend(paths, agg_path(root, output_rel, gather, ftarget, &info, &cost));
	vexec_alt_costed(ps, alt, &cost,
					 psprintf("count(DISTINCT) in %d workers: %s, the lower stage's pairs dealt out by them and the argument; %s",
							  upart->parallel_workers,
							  count_of(list_length(groupclause), "grouping column", "grouping columns"),
							  count_of(list_length(as->aggrefs), "aggregate", "aggregates")));

	/* 5, with keys: the final stage in each participant too (H5) */
	if (strategy == AGG_HASHED && groupclause != NIL)
	{
		Path	   *rep = repart_path(root, output_rel, upart, groupclause);
		double		each = clamp_row_est(numgroups / divisor);
		Path	   *pfinal;
		VexecCost	gcost;

		row = create_agg_path(root, output_rel, rep, ftarget, strategy,
							  AGGSPLIT_FINAL_DESERIAL, groupclause, having, &fcosts, each);
		vexec_cost_agg(root, &row->path, rep, true, list_length(groupclause), nkeys_kernel, 0,
					   each, &cost);
		info.numgroups = each;
		pfinal = agg_path(root, output_rel, rep, ftarget, &info, &cost);
		split_shape_price(pfinal, rep, shape);	/* W3 */
		pfinal->parallel_safe = true;
		pfinal->parallel_workers = upart->parallel_workers;
		if (add)
			add_partial_path(output_rel, pfinal);
		gather = (Path *) create_gather_path(root, output_rel, pfinal, ftarget,
											 NULL, &numgroups);
		paths = lappend(paths, gather);
		gcost = cost;
		gcost.startup = gather->startup_cost;
		gcost.total = gather->total_cost;
		vexec_alt_costed(ps, alt, &gcost,
						 psprintf("count(DISTINCT) in %d workers, finalized in each: %s, the pairs dealt out by them and the argument, the groups by their keys; %s",
								  upart->parallel_workers,
								  count_of(list_length(groupclause), "grouping column", "grouping columns"),
								  count_of(list_length(as->aggrefs), "aggregate", "aggregates")));
	}
	if (add)
		ps->sorts_built = true; /* VecRepartition's scan tuple finished (join.c) */
	return paths;
}

/*
 * The other aggregates of a count(DISTINCT) grouping apart (vecaggjoin.c):
 * H5's partitioned pair for them, by the keys alone, over the input's
 * partial path, their final groups gathered; X1's stages for the DISTINCT
 * aggregate alone, whose lower stages carry no other state, the final
 * groups of each participant gathered; and the two joined by their keys,
 * the join's target the grouping's, its qual the HAVING.  Q9 of ClickBench:
 * the sum, count and avg of 400 regions carried for each of 4.6 million
 * (region, user) pairs where they are planned together.  Each of its keys
 * is one group of either side, both grouping the same rows.  NULL where
 * there is no other aggregate, no keys, no partial path, or no H5's pair.
 *
 * Its price.  The cost model prices each side with every aggregate of the
 * query (get_agg_clause_costs()), and the other aggregates' second pass at
 * a scan's full price -- MergeTree's, 15 ms of Q9's 230 -- where carrying
 * their states with every pair, through the lower stages' packing,
 * repartition and merging, costs nearly nothing: so it would never be
 * chosen.  Measured, Q9 split took 181 ms where the grouping planned whole
 * took 230 (ch_table_engines.md, "Optimization for Clickbench").  So where
 * the pairs are VEXEC_SPLIT_PAIRS a group or more, the states then carried
 * many times over each group, the split is priced just under the grouping
 * planned whole (whole_cost); else at its sides' costs.
 */
#define VEXEC_SPLIT_PAIRS	64

static Path *
split_path(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
		   VexecPlanState *ps, VexecAlt *alt, AggScan *as, Aggref *d, List *groupclause,
		   List *having, double numgroups, SortGroupClause *dsgc, Expr *x, Index xref,
		   Path *whole, double shape)
{
	Path	   *pinput;
	List	   *others = NIL;
	PathTarget *lpt;			/* the other aggregates' partial target */
	PathTarget *lft;			/* and their final one */
	PathTarget *dft;			/* the DISTINCT aggregate's final one */
	PathTarget *dlt;			/* and its lower stage's */
	AggScan		das;
	List	   *lgroupclause;
	List	   *gexprs;
	List	   *dpaths;
	AggClauseCosts pcosts;
	AggClauseCosts fcosts;
	AggPathInfo info;
	AggPath    *row;
	VexecCost	cost;
	Path	   *partial;
	Path	   *rep;
	Path	   *pfinal;
	Path	   *left;
	Path	   *right;
	CustomPath *cp;
	SortGroupClause *xsgc;
	double		divisor;
	double		pgroups;
	double		each;
	int			nkeys_kernel = count_kernel_keys(groupclause, as->groupexprs);
	List	   *eqops = NIL;
	List	   *collations = NIL;
	ListCell   *lc;

	if (groupclause == NIL || root->parse->groupClause == NIL || !vexec_enable_repartition ||
		input_rel->partial_pathlist == NIL || !output_rel->consider_parallel)
		return NULL;
	pinput = linitial(input_rel->partial_pathlist);
	if (pinput->parallel_workers <= 0 || PATH_REQ_OUTER(pinput) != NULL)
		return NULL;
	foreach(lc, as->aggrefs)
		if (lfirst(lc) != d && !equal(lfirst(lc), d))
			others = lappend(others, lfirst(lc));
	if (others == NIL)
		return NULL;

	/* the targets: the keys first, then the first row's columns, then the aggregates */
	lpt = create_empty_pathtarget();
	lft = create_empty_pathtarget();
	dft = create_empty_pathtarget();
	dlt = create_empty_pathtarget();
	foreach(lc, groupclause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		Expr	   *k = (Expr *) list_nth(as->groupexprs, foreach_current_index(lc));

		add_column_to_pathtarget(lpt, k, sgc->tleSortGroupRef);
		add_column_to_pathtarget(lft, k, sgc->tleSortGroupRef);
		add_column_to_pathtarget(dft, k, sgc->tleSortGroupRef);
		add_column_to_pathtarget(dlt, k, sgc->tleSortGroupRef);
		eqops = lappend_oid(eqops, sgc->eqop);
		collations = lappend_oid(collations, exprCollation((Node *) k));
	}
	add_column_to_pathtarget(dlt, x, xref);
	foreach(lc, as->extras)
	{
		add_column_to_pathtarget(lpt, lfirst(lc), 0);
		add_column_to_pathtarget(lft, lfirst(lc), 0);
	}
	foreach(lc, others)
	{
		Aggref	   *pa = makeNode(Aggref);

		memcpy(pa, lfirst(lc), sizeof(Aggref));
		mark_partial_aggref(pa, AGGSPLIT_INITIAL_SERIAL);
		add_column_to_pathtarget(lpt, (Expr *) pa, 0);
		add_column_to_pathtarget(lft, lfirst(lc), 0);
	}
	add_column_to_pathtarget(dft, (Expr *) d, 0);
	set_pathtarget_cost_width(root, lpt);
	set_pathtarget_cost_width(root, lft);
	set_pathtarget_cost_width(root, dft);
	set_pathtarget_cost_width(root, dlt);

	/* the other aggregates: H5's pair, by the keys, gathered */
	divisor = vexec_parallel_divisor(pinput);
	pgroups = Min(estimate_num_groups(root, as->groupexprs, pinput->rows, NULL, NULL), pinput->rows);
	each = clamp_row_est(numgroups / divisor);
	MemSet(&pcosts, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_INITIAL_SERIAL, &pcosts);
	MemSet(&fcosts, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &fcosts);
	row = create_agg_path(root, output_rel, pinput, lpt, AGG_HASHED, AGGSPLIT_INITIAL_SERIAL,
						  groupclause, NIL, &pcosts, pgroups);
	vexec_cost_agg(root, &row->path, pinput, vexec_is_vector_path(pinput),
				   list_length(groupclause), nkeys_kernel,
				   count_vector_aggs(others, AGGSPLIT_INITIAL_SERIAL, NULL), pgroups, &cost);
	memset(&info, 0, sizeof(info));
	info.strategy = AGG_HASHED;
	info.split = AGGSPLIT_INITIAL_SERIAL;
	info.numgroups = pgroups;
	info.groupclause = groupclause;
	partial = agg_path(root, output_rel, pinput, lpt, &info, &cost);
	partial->parallel_safe = true;
	rep = repart_path(root, output_rel, partial, groupclause);
	row = create_agg_path(root, output_rel, rep, lft, AGG_HASHED, AGGSPLIT_FINAL_DESERIAL,
						  groupclause, NIL, &fcosts, each);
	vexec_cost_agg(root, &row->path, rep, true, list_length(groupclause), nkeys_kernel, 0,
				   each, &cost);
	info.split = AGGSPLIT_FINAL_DESERIAL;
	info.numgroups = each;
	pfinal = agg_path(root, output_rel, rep, lft, &info, &cost);
	split_shape_price(pfinal, rep, shape);	/* W3 */
	pfinal->parallel_safe = true;
	pfinal->parallel_workers = partial->parallel_workers;
	left = (Path *) create_gather_path(root, output_rel, pfinal, lft, NULL, &numgroups);

	/* the DISTINCT aggregate alone: X1's stages, finalized in each participant, gathered */
	das = *as;
	das.aggrefs = list_make1(d);
	das.extras = NIL;
	xsgc = makeNode(SortGroupClause);
	memcpy(xsgc, dsgc, sizeof(SortGroupClause));
	xsgc->tleSortGroupRef = xref;
	lgroupclause = lappend(list_copy(groupclause), xsgc);
	gexprs = lappend(list_copy(as->groupexprs), x);
	dpaths = distinct_parallel_paths(root, input_rel, output_rel, ps, alt, &das, d, groupclause,
									 NIL, numgroups, dlt, lgroupclause, gexprs, xref, false, dft,
									 shape);
	if (list_length(dpaths) < 2)
		return NULL;
	right = llast(dpaths);
	ps->sorts_built = true;		/* the VecRepartitions' scan tuples finished (join.c) */

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = output_rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = numgroups;
	cp->path.disabled_nodes = left->disabled_nodes + right->disabled_nodes;
	cp->path.startup_cost = left->total_cost + right->total_cost;
	cp->path.total_cost = cp->path.startup_cost + cpu_tuple_cost * 2 * numgroups;
	{
		double		pairs = Min(estimate_num_groups(root, gexprs, pinput->rows * divisor, NULL, NULL),
								pinput->rows * divisor);

		if (whole != NULL && pairs >= VEXEC_SPLIT_PAIRS * Max(numgroups, 1) &&
			cp->path.total_cost >= whole->total_cost)
		{
			cp->path.total_cost = whole->total_cost * 0.99;
			cp->path.startup_cost = Min(cp->path.startup_cost, cp->path.total_cost);
		}
	}
	cp->path.pathkeys = NIL;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = list_make2(left, right);
	cp->custom_private = list_make4(having, makeInteger(list_length(groupclause)), eqops,
									collations);
	cp->methods = &vecaggjoin_path_methods;
	{
		VexecCost	jcost;

		jcost.startup = cp->path.startup_cost;
		jcost.total = cp->path.total_cost;
		vexec_alt_costed(ps, alt, &jcost,
						 psprintf("count(DISTINCT) and %s apart, joined by %s",
								  count_of(list_length(others), "other aggregate", "other aggregates"),
								  count_of(list_length(groupclause), "grouping column", "grouping columns")));
	}
	return &cp->path;
}

/*
 * VecAggJoin's plan: its sides in lefttree, the other aggregates', and
 * righttree, the DISTINCT one's; its scan tuple their rows' columns, which
 * planner_shutdown_hook makes references to them (join.c); its qual the
 * HAVING; its keys, the first columns of each side, compared by the
 * grouping's operators.
 */
static Plan *
plan_vecaggjoin(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
				List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *left = linitial(custom_plans);
	Plan	   *right = lsecond(custom_plans);
	List	   *priv = best_path->custom_private;
	List	   *scan_tlist = NIL;
	ListCell   *lc;

	(void) root;
	(void) rel;
	(void) clauses;
	foreach(lc, left->targetlist)
		scan_tlist = lappend(scan_tlist, makeTargetEntry(copyObject(lfirst_node(TargetEntry, lc)->expr),
														 list_length(scan_tlist) + 1, NULL, false));
	foreach(lc, right->targetlist)
		scan_tlist = lappend(scan_tlist, makeTargetEntry(copyObject(lfirst_node(TargetEntry, lc)->expr),
														 list_length(scan_tlist) + 1, NULL, false));
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = (List *) copyObject(linitial(priv));
	cscan->scan.plan.lefttree = left;
	cscan->scan.plan.righttree = right;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = list_make4(makeInteger(list_length(left->targetlist)),
									   lsecond(priv), lthird(priv), lfourth(priv));
	cscan->methods = vexec_aggjoin_methods();
	return &cscan->scan.plan;
}

/*
 * W1: H3's two stages the device's both, where its lower stage reads the
 * scan's units on the device and the query's one aggregate is the distinct
 * count: the upper stage counts each key's distinct values there
 * (exec/vecagg.c), its groups final, the pairs never handed up -- priced as
 * the lower stage's path on the device, the groups it hands up the upper
 * stage's.
 */
static void
whole_pair(PlannerInfo *root, AggScan *as, Aggref *d, List *having, AggStrategy strategy, double numgroups,
		   double lgroups, const AggPathInfo *linfo, const Path *lower, AggPathInfo *uinfo, VexecCost *ucost)
{
	double		groups = strategy == AGG_PLAIN ? 1 : numgroups;
	int			nkeys = list_length(as->groupexprs);
	double		pairs;
	double		counts;

	uinfo->whole = linfo->device && linfo->units && list_length(as->aggrefs) == 1 && as->extras == NIL &&
		d->aggfnoid == F_COUNT_ANY && d->aggfilter == NULL && having == NIL;
	if (!uinfo->whole)
		return;

	/*
	 * W42 (§3.35): the pairs the device keeps -- their keys' and argument's
	 * bytes, and an entry's -- within the memory its tasks have not
	 * reserved: past it the count's two stages not the device's whole, its
	 * lower one by chunks, but where forced
	 */
	if (vexec_gpu != VEXEC_GPU_FORCE)
	{
		double		pair_bytes = 16 + device_bytes_of(root, (Node *) linitial_node(TargetEntry, d->args)->expr, 8);
		ListCell   *lc;

		foreach(lc, as->groupexprs)
			pair_bytes += device_bytes_of(root, lfirst(lc), 8);
		if (Max(lgroups, groups) * pair_bytes > (double) vexec_device_plan_bytes())
		{
			uinfo->whole = false;
			return;
		}
	}
	/* the answer the groups' -- keys and a count -- not the pairs', which never cross back */
	pairs = Max(lgroups, groups) * (8 * (nkeys + 1) + 16);
	counts = groups * (8 * nkeys + 32);
	ucost->startup = Max(lower->startup_cost - vexec_gpu_transfer_cost * (pairs - counts), 0);
	ucost->total = ucost->startup + (lower->total_cost - lower->startup_cost) * Min(groups / Max(lgroups, 1), 1.0);
}

/*
 * W19 (§3.28): the X1 stages' upper one the device's whole with the other
 * aggregates beside the distinct count -- one grouping of the keys, every
 * aggregate and the distinct pairs over the relation's serial scan's units
 * (exec/vecagg.c, ops_groupby.cpp), in place of W11's two groupings in turn:
 * priced as one device grouping of them all, where every other aggregate's
 * argument is a column of the relation and none has a FILTER; its lower
 * stage, the CPU's X1 pairs over the same scan, is read where the device
 * gives up.  ClickBench's Q9 and Q22, 7 and 13 ms of their two groupings.
 */
static bool
whole_mixed_price(PlannerInfo *root, RelOptInfo *output_rel, Path *scan, AggScan *as, Aggref *d, List *groupclause,
				  List *having, AggStrategy strategy, double numgroups, AggPathInfo *info, VexecCost *cost)
{
	AggClauseCosts costs;
	AggPath    *row;
	ListCell   *lc;

	if (as->extras != NIL)
		return false;
	foreach(lc, as->aggrefs)
	{
		Aggref	   *a = lfirst(lc);

		if (a == d || equal(a, d))
			continue;
		if (a->aggfilter != NULL || list_length(a->args) > 1 ||
			(a->args != NIL && !IsA(linitial_node(TargetEntry, a->args)->expr, Var)))
			return false;
	}
	MemSet(&costs, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &costs);
	row = create_agg_path(root, output_rel, scan, output_rel->reltarget, strategy, AGGSPLIT_SIMPLE, groupclause, having,
						  &costs, strategy == AGG_PLAIN ? 1 : numgroups);
	vexec_cost_agg(root, &row->path, scan, true, list_length(groupclause), count_kernel_keys(groupclause, as->groupexprs),
				   count_vector_aggs(as->aggrefs, AGGSPLIT_SIMPLE, NULL), numgroups, cost);
	memset(info, 0, sizeof(*info));
	info->strategy = strategy;
	info->split = AGGSPLIT_SIMPLE;
	info->numgroups = strategy == AGG_PLAIN ? 1 : numgroups;
	info->groupclause = groupclause;
	info->having = having;
	info->distinct_whole = true;
	agg_device_price(root, scan, output_rel->reltarget, info, cost);
	return info->device && info->units;
}

/*
 * W11 (§3.27): split_path()'s two sides the device's, each over the
 * relation's serial VecScan (device_input()), where a distinct count stands
 * beside other aggregates: those grouped by the keys alone, the device's
 * whole grouping (W1); the distinct count's two stages both the device's
 * (W1, whole_pair()), its lower one grouping the keys and the argument --
 * the second side's units' columns those the first kept (W5) -- the two
 * joined by VecAggJoin.  ClickBench's Q9: its distinct count Q8's grouping,
 * its other aggregates Q7's, where H3's pair handed avg's array states on
 * and the CPU took 174 ms.  NULL where either side is not the device's.
 */
static Path *
device_split_path(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel, VexecPlanState *ps,
				  VexecAlt *alt, AggScan *as, Aggref *d, List *groupclause, List *having, double numgroups,
				  Expr *x, List *lgroupclause, double lgroups, Index xref)
{
	Path	   *input = input_rel->cheapest_total_path;
	Path	   *vin = device_input(input_rel, input);
	List	   *others = NIL;
	PathTarget *lft;
	PathTarget *dft;
	PathTarget *dlt;
	AggScan		das;
	AggClauseCosts costs;
	AggPathInfo info;
	AggPathInfo uinfo;
	AggPath    *row;
	VexecCost	cost;
	VexecCost	ucost;
	Path	   *left;
	Path	   *lower;
	Path	   *right;
	CustomPath *cp;
	int			nkeys_kernel = count_kernel_keys(groupclause, as->groupexprs);
	List	   *eqops = NIL;
	List	   *collations = NIL;
	Bitmapset  *inner_attrs = NULL;
	ListCell   *lc;

	if (vin == NULL && scan_under(input) != NULL && input->parallel_workers == 0)
		vin = input;
	if (vin == NULL || groupclause == NIL || root->parse->groupClause == NIL || as->extras != NIL)
		return NULL;
	foreach(lc, as->aggrefs)
		if (lfirst(lc) != d && !equal(lfirst(lc), d))
			others = lappend(others, lfirst(lc));
	if (others == NIL)
		return NULL;

	/* the targets: the keys first, then the aggregates; the lower stage's the keys and the argument */
	lft = create_empty_pathtarget();
	dft = create_empty_pathtarget();
	dlt = create_empty_pathtarget();
	foreach(lc, groupclause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		Expr	   *k = (Expr *) list_nth(as->groupexprs, foreach_current_index(lc));

		add_column_to_pathtarget(lft, k, sgc->tleSortGroupRef);
		add_column_to_pathtarget(dft, k, sgc->tleSortGroupRef);
		add_column_to_pathtarget(dlt, k, sgc->tleSortGroupRef);
		eqops = lappend_oid(eqops, sgc->eqop);
		collations = lappend_oid(collations, exprCollation((Node *) k));
	}
	add_column_to_pathtarget(dlt, x, xref);
	foreach(lc, others)
		add_column_to_pathtarget(lft, lfirst(lc), 0);
	add_column_to_pathtarget(dft, (Expr *) d, 0);
	set_pathtarget_cost_width(root, lft);
	set_pathtarget_cost_width(root, dft);
	set_pathtarget_cost_width(root, dlt);

	/*
	 * the inner side's attributes, which VecAggJoin reads first -- the
	 * keys', the argument's and the quals' -- the outer side's read then
	 * kept (W5)
	 */
	pull_varattnos((Node *) as->groupexprs, input_rel->relid, &inner_attrs);
	pull_varattnos((Node *) x, input_rel->relid, &inner_attrs);
	pull_varattnos((Node *) extract_actual_clauses(input_rel->baserestrictinfo, false), input_rel->relid,
				   &inner_attrs);

	/* the other aggregates: the device's whole grouping by the keys */
	MemSet(&costs, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &costs);
	row = create_agg_path(root, output_rel, vin, lft, AGG_HASHED, AGGSPLIT_SIMPLE, groupclause, NIL,
						  &costs, numgroups);
	vexec_cost_agg(root, &row->path, vin, true, list_length(groupclause), nkeys_kernel,
				   count_vector_aggs(others, AGGSPLIT_SIMPLE, NULL), numgroups, &cost);
	memset(&info, 0, sizeof(info));
	info.strategy = AGG_HASHED;
	info.split = AGGSPLIT_SIMPLE;
	info.numgroups = numgroups;
	info.groupclause = groupclause;
	info.kept_attrs = inner_attrs;
	left = agg_path(root, output_rel, vin, lft, &info, &cost);
	if (!info.device)
		return NULL;

	/*
	 * the distinct count alone: its two stages both the device's, priced
	 * with no transition of the query's other aggregates -- the lower stage
	 * groups the keys and the argument, its states none, which
	 * get_agg_clause_costs() would count every aggregate's of
	 */
	das = *as;
	das.aggrefs = list_make1(d);
	das.extras = NIL;
	MemSet(&costs, 0, sizeof(AggClauseCosts));
	row = create_agg_path(root, output_rel, vin, dlt, AGG_HASHED, AGGSPLIT_INITIAL_SERIAL, lgroupclause, NIL,
						  &costs, lgroups);
	vexec_cost_agg(root, &row->path, vin, true, list_length(lgroupclause), 0, 1, lgroups, &cost);
	memset(&info, 0, sizeof(info));
	info.strategy = AGG_HASHED;
	info.split = AGGSPLIT_INITIAL_SERIAL;
	info.numgroups = lgroups;
	info.groupclause = lgroupclause;
	lower = agg_path(root, output_rel, vin, dlt, &info, &cost);
	if (!info.device || !info.units)
		return NULL;
	MemSet(&costs, 0, sizeof(AggClauseCosts));
	row = create_agg_path(root, output_rel, lower, dft, AGG_HASHED, AGGSPLIT_FINAL_DESERIAL, groupclause, NIL,
						  &costs, numgroups);
	vexec_cost_agg(root, &row->path, lower, false, list_length(groupclause), 0, 1, numgroups, &ucost);
	memset(&uinfo, 0, sizeof(uinfo));
	uinfo.strategy = AGG_HASHED;
	uinfo.split = AGGSPLIT_FINAL_DESERIAL;
	uinfo.numgroups = numgroups;
	uinfo.groupclause = groupclause;
	uinfo.distinct_ref = xref;
	whole_pair(root, &das, d, NIL, AGG_HASHED, numgroups, lgroups, &info, lower, &uinfo, &ucost);
	if (!uinfo.whole)
		return NULL;
	right = agg_path(root, output_rel, lower, dft, &uinfo, &ucost);

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = output_rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = numgroups;
	cp->path.disabled_nodes = left->disabled_nodes + right->disabled_nodes;
	cp->path.startup_cost = left->total_cost + right->total_cost;
	cp->path.total_cost = cp->path.startup_cost + cpu_tuple_cost * 2 * numgroups;
	cp->path.pathkeys = NIL;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = list_make2(left, right);
	cp->custom_private = list_make4(having, makeInteger(list_length(groupclause)), eqops, collations);
	cp->methods = &vecaggjoin_path_methods;
	{
		VexecCost	jcost;

		jcost.startup = cp->path.startup_cost;
		jcost.total = cp->path.total_cost;
		vexec_alt_costed(ps, alt, &jcost,
						 psprintf("count(DISTINCT) and %s apart, both the device's over the relation's serial scan's units",
								  count_of(list_length(others), "other aggregate", "other aggregates")));
	}
	ps->sorts_built = true;		/* the scan tuples finished (join.c) */
	return &cp->path;
}

/*
 * H3's two stages: a lower VecAgg grouping by the keys and the DISTINCT
 * argument, the other aggregates' partial states beside, and an upper one
 * grouping by the keys, which counts the argument once a distinct value and
 * combines the other states.  X1 runs them in parallel (above).
 */
static void
consider_distinct_pair(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
					   VexecPlanState *ps, VexecAlt *alt, AggScan *as, List *groupclause,
					   List *having, double numgroups, bool add)
{
	Path	   *input = input_rel->cheapest_total_path;
	Aggref	   *d = NULL;
	const char *refusal;
	PathTarget *lt;
	List	   *lgroupclause;
	SortGroupClause *dsgc;
	SortGroupClause *xsgc;
	Expr	   *x;
	Index		xref = 0;
	ListCell   *lc;
	List	   *gexprs;
	double		lgroups;
	AggClauseCosts pcosts;
	AggClauseCosts fcosts;
	AggPath    *lrow;
	AggPath    *urow;
	VexecCost	lcost;
	VexecCost	ucost;
	AggPathInfo linfo;
	AggPathInfo uinfo;
	Path	   *lower;
	Path	   *dpair = NULL;	/* W1: the pair with the device's lower stage */
	Path	   *mixed = NULL;	/* W19: the pair as one device grouping, the other aggregates in it */
	List	   *variants = NIL; /* W7: device paths kept aside, not added */
	double		shape;
	List	   *newpaths;
	AggStrategy strategy = root->parse->groupClause != NIL ? AGG_HASHED : AGG_PLAIN;

	if ((refusal = distinct_refusal(as, input->pathtarget, &d)) != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return;
	}
	if (groupclause != NIL && !grouping_is_hashable(groupclause))
	{
		vexec_alt_refuse(ps, alt, "a grouping that cannot be hashed");
		return;
	}
	if (!enable_hashagg)
	{
		vexec_alt_refuse(ps, alt, "hash aggregation is disabled");
		return;
	}
	/*
	 * W9: where the device reads the relation's units whole, not refused for
	 * the rows its quals leave; F20: nor over a vector scan's batches
	 */
	if (few_rows(ps, input_rel, input) && !vector_scan_input(input))
	{
		vexec_alt_refuse(ps, alt, psprintf("%.0f input rows, fewer than vexec.min_rows", input->rows));
		return;
	}
	x = linitial_node(TargetEntry, d->args)->expr;
	dsgc = linitial_node(SortGroupClause, d->aggdistinct);

	/* a sortgroupref of its own for the argument, past every other */
	foreach(lc, root->processed_tlist)
		xref = Max(xref, lfirst_node(TargetEntry, lc)->ressortgroupref);
	foreach(lc, groupclause)
		xref = Max(xref, lfirst_node(SortGroupClause, lc)->tleSortGroupRef);
	xref++;

	/* the lower stage's output: the keys, the argument, the first row's, the partial states */
	lt = create_empty_pathtarget();
	foreach(lc, groupclause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);

		add_column_to_pathtarget(lt, (Expr *) list_nth(as->groupexprs, foreach_current_index(lc)),
								 sgc->tleSortGroupRef);
	}
	add_column_to_pathtarget(lt, x, xref);
	foreach(lc, as->extras)
		add_column_to_pathtarget(lt, lfirst(lc), 0);
	foreach(lc, as->aggrefs)
	{
		Aggref	   *a = lfirst_node(Aggref, lc);
		Aggref	   *pa;

		if (a->aggdistinct != NIL)
			continue;
		pa = makeNode(Aggref);
		memcpy(pa, a, sizeof(Aggref));
		mark_partial_aggref(pa, AGGSPLIT_INITIAL_SERIAL);
		add_column_to_pathtarget(lt, (Expr *) pa, 0);
	}
	set_pathtarget_cost_width(root, lt);

	xsgc = makeNode(SortGroupClause);
	memcpy(xsgc, dsgc, sizeof(SortGroupClause));
	xsgc->tleSortGroupRef = xref;
	lgroupclause = lappend(list_copy(groupclause), xsgc);
	gexprs = lappend(list_copy(as->groupexprs), x);
	lgroups = Min(estimate_num_groups(root, gexprs, input->rows, NULL, NULL), input->rows);

	MemSet(&pcosts, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_INITIAL_SERIAL, &pcosts);
	MemSet(&fcosts, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &fcosts);
	lrow = create_agg_path(root, output_rel, input, lt, AGG_HASHED, AGGSPLIT_INITIAL_SERIAL,
						   lgroupclause, NIL, &pcosts, lgroups);
	vexec_cost_agg(root, &lrow->path, input, vexec_is_vector_path(input),
				   list_length(lgroupclause), 0, list_length(as->aggrefs), lgroups, &lcost);
	memset(&linfo, 0, sizeof(linfo));
	linfo.strategy = AGG_HASHED;
	linfo.split = AGGSPLIT_INITIAL_SERIAL;
	linfo.numgroups = lgroups;
	linfo.groupclause = lgroupclause;
	lower = agg_path(root, output_rel, input, lt, &linfo, &lcost);

	urow = create_agg_path(root, output_rel, lower, output_rel->reltarget, strategy,
						   AGGSPLIT_FINAL_DESERIAL, groupclause, having, &fcosts,
						   strategy == AGG_PLAIN ? 1 : numgroups);
	vexec_cost_agg(root, &urow->path, lower, false, list_length(groupclause), 0,
				   list_length(as->aggrefs), strategy == AGG_PLAIN ? 1 : numgroups, &ucost);
	memset(&uinfo, 0, sizeof(uinfo));
	uinfo.strategy = strategy;
	uinfo.split = AGGSPLIT_FINAL_DESERIAL;
	uinfo.numgroups = numgroups;
	uinfo.groupclause = groupclause;
	uinfo.having = having;
	uinfo.distinct_ref = xref;
	{
		/* the stages as they are, or W1's both the device's where the lower one is */
		AggPathInfo winfo = uinfo;
		VexecCost	wcost = ucost;

		whole_pair(root, as, d, having, strategy, numgroups, lgroups, &linfo, lower, &winfo, &wcost);
		vexec_alt_costed(ps, alt, &wcost, winfo.whole ?
						 "two stages for count(DISTINCT), both the device's over the relation's serial scan's units" :
						 psprintf("two stages for count(DISTINCT): %s, the argument a key of the lower one; %s",
								  count_of(list_length(groupclause), "grouping column", "grouping columns"),
								  count_of(list_length(as->aggrefs), "aggregate", "aggregates")));
		newpaths = list_make1(agg_path(root, output_rel, lower, output_rel->reltarget, &winfo, &wcost));
	}

	/*
	 * W1: the lower stage on the device over the relation's serial VecScan,
	 * where the cheapest input is not one: one task groups every pair --
	 * made first, for the parallel stages' price beside it (W3), and added
	 * last, out of the split's baseline
	 */
	{
		Path	   *dinput = device_input(input_rel, input);

		if (dinput != NULL)
		{
			AggPathInfo dlinfo = linfo;
			AggPathInfo duinfo = uinfo;
			VexecCost	dlcost;
			VexecCost	ducost;
			Path	   *dlower;

			lrow = create_agg_path(root, output_rel, dinput, lt, AGG_HASHED, AGGSPLIT_INITIAL_SERIAL,
								   lgroupclause, NIL, &pcosts, lgroups);
			vexec_cost_agg(root, &lrow->path, dinput, true, list_length(lgroupclause), 0,
						   list_length(as->aggrefs), lgroups, &dlcost);
			dlower = agg_path(root, output_rel, dinput, lt, &dlinfo, &dlcost);
			if (dlinfo.device)
			{
				urow = create_agg_path(root, output_rel, dlower, output_rel->reltarget, strategy,
									   AGGSPLIT_FINAL_DESERIAL, groupclause, having, &fcosts,
									   strategy == AGG_PLAIN ? 1 : numgroups);
				vexec_cost_agg(root, &urow->path, dlower, false, list_length(groupclause), 0,
							   list_length(as->aggrefs), strategy == AGG_PLAIN ? 1 : numgroups,
							   &ducost);

				whole_pair(root, as, d, having, strategy, numgroups, lgroups, &dlinfo, dlower, &duinfo, &ducost);
				dpair = agg_path(root, output_rel, dlower, output_rel->reltarget, &duinfo, &ducost);
				vexec_alt_costed(ps, alt, &ducost, duinfo.whole ?
								 "two stages for count(DISTINCT), both the device's over the relation's serial scan's units" :
								 "two stages for count(DISTINCT), the lower one the device's over the relation's serial scan");
			}
		}
	}

	/*
	 * W19: the pair as one device grouping with the other aggregates beside
	 * the distinct count, over the relation's serial scan's units; its lower
	 * stage the CPU's X1 pairs over the same scan, read where the device gives
	 * up.  Priced as the whole grouping, in place of W11's two.
	 */
	if (list_length(as->aggrefs) > 1)
	{
		Path	   *min = device_input(input_rel, input);

		if (min == NULL && scan_under(input) != NULL && input->parallel_workers == 0)
			min = input;
		if (min != NULL)
		{
			AggPathInfo minfo;
			VexecCost	mcost;

			if (whole_mixed_price(root, output_rel, min, as, d, groupclause, having, strategy, numgroups, &minfo, &mcost))
			{
				AggPathInfo mlinfo = linfo;
				AggPathInfo muinfo = uinfo;
				VexecCost	mlcost;
				Path	   *mlower;

				lrow = create_agg_path(root, output_rel, min, lt, AGG_HASHED, AGGSPLIT_INITIAL_SERIAL, lgroupclause, NIL,
									   &pcosts, lgroups);
				vexec_cost_agg(root, &lrow->path, min, true, list_length(lgroupclause), 0, list_length(as->aggrefs),
							   lgroups, &mlcost);
				mlower = agg_path(root, output_rel, min, lt, &mlinfo, &mlcost);
				muinfo.whole = true;
				mixed = agg_path(root, output_rel, mlower, output_rel->reltarget, &muinfo, &mcost);
				vexec_alt_costed(ps, alt, &mcost,
								 "two stages for count(DISTINCT), one grouping the device's with the other aggregates, over the relation's serial scan's units");
			}
		}
	}

	/*
	 * W7: where a bounded sort above may lend the grouping its rank and no
	 * pair is the device's, the device's variant of both stages over the
	 * relation's serial VecScan, priced as the device's -- kept aside for
	 * the sort, never added
	 */
	if (dpair == NULL && !linfo.device && ranked_above(root))
	{
		Path	   *vin = device_input(input_rel, input);

		if (vin == NULL && scan_under(input) != NULL && input->parallel_workers == 0)
			vin = input;
		if (vin != NULL)
		{
			AggPathInfo vlinfo = linfo;
			AggPathInfo vuinfo = uinfo;
			VexecCost	vlcost;
			VexecCost	vucost;
			Path	   *vlower;

			lrow = create_agg_path(root, output_rel, vin, lt, AGG_HASHED, AGGSPLIT_INITIAL_SERIAL,
								   lgroupclause, NIL, &pcosts, lgroups);
			vexec_cost_agg(root, &lrow->path, vin, true, list_length(lgroupclause), 0,
						   list_length(as->aggrefs), lgroups, &vlcost);
			vlinfo.device_always = true;
			vlower = agg_path(root, output_rel, vin, lt, &vlinfo, &vlcost);
			if (vlinfo.device)
			{
				urow = create_agg_path(root, output_rel, vlower, output_rel->reltarget, strategy,
									   AGGSPLIT_FINAL_DESERIAL, groupclause, having, &fcosts,
									   strategy == AGG_PLAIN ? 1 : numgroups);
				vexec_cost_agg(root, &urow->path, vlower, false, list_length(groupclause), 0,
							   list_length(as->aggrefs), strategy == AGG_PLAIN ? 1 : numgroups,
							   &vucost);
				whole_pair(root, as, d, having, strategy, numgroups, lgroups, &vlinfo, vlower, &vuinfo, &vucost);
				if (vuinfo.whole)
					variants = lappend(variants,
									   agg_path(root, output_rel, vlower, output_rel->reltarget, &vuinfo, &vucost));
			}
		}
	}
	shape = split_shape(dpair != NULL ? lappend(list_copy(newpaths), dpair) : newpaths);

	/* X1: the same stages in every participant of a parallel plan */
	newpaths = list_concat(newpaths,
						   distinct_parallel_paths(root, input_rel, output_rel, ps, alt, as, d,
												   groupclause, having, numgroups, lt,
												   lgroupclause, gexprs, xref, add,
												   output_rel->reltarget, shape));

	/* the other aggregates apart, by the keys alone, the two joined (vecaggjoin.c) */
	if (vexec_enable_agg_split)
	{
		Path	   *split = split_path(root, input_rel, output_rel, ps, alt, as, d, groupclause,
									   having, numgroups, dsgc, x, xref,
									   newpaths != NIL ? (Path *) llast(newpaths) : NULL, shape);

		if (split != NULL)
			newpaths = lappend(newpaths, split);
		/* W11: both sides the device's */
		if (vexec_gpu != VEXEC_GPU_OFF && vexec_enable_gpu_agg && vexec_device_ready() != NULL)
		{
			split = device_split_path(root, input_rel, output_rel, ps, alt, as, d, groupclause, having,
									  numgroups, x, lgroupclause, lgroups, xref);
			if (split != NULL)
				newpaths = lappend(newpaths, split);
		}
	}
	if (dpair != NULL)
		newpaths = lappend(newpaths, dpair);
	if (mixed != NULL)
		newpaths = lappend(newpaths, mixed);

	if (!add)
		return;
	keep_device_path(ps, output_rel, list_concat_copy(newpaths, variants));	/* W7 */
	device_forced(output_rel, newpaths);
	if (ps->mode == VEXEC_MODE_FORCE)
		output_rel->pathlist = NIL;
	foreach(lc, newpaths)
		add_path(output_rel, lfirst(lc));
	ps->npossible++;
}

/*
 * VC13: whether a plain aggregation's every aggregate is one the source of
 * its input's serial VecScan answers from its statistics, unit by unit
 * (H2, VEXEC_EST_AGG_EXACT): count(*), or count, min or max of a column of
 * the scanned relation, with no FILTER, DISTINCT or ORDER BY -- what
 * VecAgg's stats_setup() asks of the source (exec/vecagg.c), a minimum or
 * maximum of the types it asks them of (aggtrans.c's
 * vexec_aggtrans_stats_requests()): integers, dates, times and timestamps,
 * and a text under C's collation.  *units is the scan's.  §3.36
 * ("Universal?"): a text's minimum under another priced so read every unit
 * on the CPU, serially
 */
static bool
stats_answer(RelOptInfo *input_rel, Path *input, List *aggrefs, double *units)
{
	ListCell   *lc;

	if (!vexec_aggregate_statistics || aggrefs == NIL || !vexec_scan_path_answers(input, units))
		return false;
	foreach(lc, aggrefs)
	{
		Aggref	   *a = lfirst(lc);
		char	   *name;

		if (a->aggfilter != NULL || a->aggdistinct != NIL || a->aggorder != NIL ||
			get_func_namespace(a->aggfnoid) != PG_CATALOG_NAMESPACE)
			return false;
		name = get_func_name(a->aggfnoid);
		if (a->aggstar)
		{
			if (strcmp(name, "count") != 0)
				return false;
			continue;
		}
		if (vexec_scan_path_counts_only(input))
			return false;
		if (list_length(a->args) != 1 ||
			(strcmp(name, "count") != 0 && strcmp(name, "min") != 0 && strcmp(name, "max") != 0))
			return false;
		{
			Expr	   *arg = linitial_node(TargetEntry, a->args)->expr;
			Oid			type = exprType((Node *) arg);

			if (!IsA(arg, Var) || ((Var *) arg)->varno != input_rel->relid ||
				((Var *) arg)->varattno < 1 || ((Var *) arg)->varlevelsup != 0)
				return false;
			if (strcmp(name, "count") != 0 &&
				!(type == INT2OID || type == INT4OID || type == INT8OID || type == DATEOID || type == TIMEOID ||
				  type == TIMESTAMPOID || type == TIMESTAMPTZOID ||
				  ((type == TEXTOID || type == VARCHAROID) && OidIsValid(a->inputcollid) &&
				   pg_newlocale_from_collation(a->inputcollid)->collate_is_c)))
				return false;
		}
	}
	return true;
}

/*
 * VC4: the input relation's serial VecScan -- among its paths, or kept
 * aside where a cheaper one dropped it (sort.c), with the input's target --
 * where its source hands its rows in an order the grouping's keys are a
 * prefix of (vexec_source.h's ordering(), minor 4): each key a column of
 * the relation compared by its type's default equality, a deterministic
 * collation's for text, as the source's order compares it.  NULL where
 * there is none.
 */
static Path *
sorted_input_path(PlannerInfo *root, RelOptInfo *input_rel, Path *input, List *groupclause,
				  VexecPlanState *ps)
{
	AttrNumber	order[INDEX_MAX_KEYS];
	AttrNumber	keys[INDEX_MAX_KEYS];
	bool		used[INDEX_MAX_KEYS] = {false};
	Path	   *scan = NULL;
	int			nkeys = list_length(groupclause);
	int			k;
	ListCell   *lc;

	if (!vexec_enable_sorted_agg || input_rel->reloptkind != RELOPT_BASEREL ||
		input_rel->rtekind != RTE_RELATION || nkeys == 0 || nkeys > INDEX_MAX_KEYS)
		return NULL;
	foreach(lc, groupclause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		Var		   *v = (Var *) get_sortgroupclause_expr(sgc, root->processed_tlist);
		Oid			coll;

		if (!IsA(v, Var) || v->varno != input_rel->relid || v->varlevelsup != 0 || v->varattno < 1)
			return NULL;
		coll = exprCollation((Node *) v);
		if (sgc->eqop != lookup_type_cache(v->vartype, TYPECACHE_EQ_OPR)->eq_opr ||
			(OidIsValid(coll) && !get_collation_isdeterministic(coll)))
			return NULL;
		keys[foreach_current_index(lc)] = v->varattno;
	}
	foreach(lc, input_rel->pathlist)
	{
		Path	   *p = lfirst(lc);

		if (vexec_is_scan_path(p) && !p->parallel_aware && p->param_info == NULL)
		{
			scan = p;
			break;
		}
	}
	if (scan == NULL)
	{
		Path	   *kept = vexec_sort_kept_scan(ps, input_rel);
		CustomPath *copy;

		if (kept == NULL)
			return NULL;
		copy = palloc(sizeof(CustomPath));
		memcpy(copy, kept, sizeof(CustomPath));
		copy->path.pathtarget = input->pathtarget;
		scan = &copy->path;
	}
	k = vexec_scan_path_ordering(root, scan, nkeys, order);
	if (k < nkeys)
		return NULL;
	for (int i = 0; i < nkeys; i++)
	{
		int			j;

		for (j = 0; j < nkeys; j++)
			if (order[j] == keys[i] && !used[j])
				break;
		if (j == nkeys)
			return NULL;
		used[j] = true;
	}
	return scan;
}

/*
 * VecAgg's alternatives for an aggregation (§3.3.3): recorded for EXPLAIN,
 * and in auto and force mode added.
 */
void
vexec_consider_agg(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
				   GroupPathExtraData *extra, VexecPlanState *ps, const char *target_name)
{
	VexecAlt   *alt = NULL;
	Path	   *input = input_rel->cheapest_total_path;
	List	   *groupclause = root->processed_groupClause;
	List	   *having = (List *) extra->havingQual;
	bool		add = ps->mode == VEXEC_MODE_AUTO || ps->mode == VEXEC_MODE_FORCE;

	/*
	 * A GROUP BY whose every key the planner proved constant has no keys
	 * left, and is still a grouping: one group where there are rows, none
	 * where there are none, as PostgreSQL's AGG_SORTED of no columns
	 * (planner.c, "parse->groupClause ? AGG_SORTED : AGG_PLAIN").  VecAgg
	 * hashes it, on no columns.
	 */
	AggStrategy strategy = root->parse->groupClause != NIL ? AGG_HASHED : AGG_PLAIN;
	AggClauseCosts agg_costs;
	AggPathInfo info;
	AggPath    *rowpath;
	VexecCost	cost;
	AggScan		as;
	const char *refusal = NULL;
	double		numgroups;
	int			nvec;
	int			nkeys_kernel;
	bool		answered = false;
	double		units = 0;
	List	   *names = NIL;
	List	   *newpaths = NIL;
	List	   *vgroupclause;	/* M3: VecAgg's keys, the implied ones out */
	List	   *vgroupexprs;
	List	   *implied = NIL;
	int			nimplied;
	List	   *variants = NIL;	/* W7: device paths kept aside, not added */
	bool		few;			/* F20: vexec.min_rows' few rows, a vector scan's */
	ListCell   *lc;

	if (ps->record)
		alt = vexec_alt_record(ps, "VecAgg", target_name, root, NULL);
	if (alt == NULL && !add)
		return;

	/* the groups core estimated: its paths of the grouped relation carry them */
	if (output_rel->pathlist != NIL)
		numgroups = ((Path *) linitial(output_rel->pathlist))->rows;
	else
		numgroups = Max(output_rel->rows, 1);

	if (!vexec_enable_agg)
		refusal = "vexec.enable_agg is off";
	else if (output_rel->reloptkind != RELOPT_UPPER_REL)
		refusal = "a partition's aggregation";
	else if (extra->patype != PARTITIONWISE_AGGREGATE_NONE)
		refusal = "partitionwise aggregation";
	else if (input == NULL || PATH_REQ_OUTER(input) != NULL)
		refusal = "a parameterized input";
	else if ((refusal = agg_refusal(root, groupclause, output_rel->reltarget, having,
									input->pathtarget, &as)) != NULL)
	{
		/* a DISTINCT aggregate, and nothing else in the way: H3 */
		if (as.distinct != NIL && as.refusal == NULL)
		{
			consider_distinct_pair(root, input_rel, output_rel, ps, alt, &as, groupclause,
								   having, numgroups, add);
			return;
		}
	}
	else if (strategy == AGG_HASHED && !grouping_is_hashable(groupclause))
		refusal = "a grouping that cannot be hashed";
	else if (strategy == AGG_HASHED && (extra->flags & GROUPING_CAN_USE_HASH) == 0)
		refusal = "a grouping PostgreSQL's planner would not hash";
	else if (strategy == AGG_HASHED && !enable_hashagg)
		refusal = "hash aggregation is disabled";
	else if (few_rows(ps, input_rel, input) && !vector_scan_input(input) &&
			 !vector_scan_input(pair_input(input_rel, output_rel, extra)))
		refusal = psprintf("%.0f input rows, fewer than vexec.min_rows", input->rows);
	if (refusal != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return;
	}
	{
		VexecSteps	steps;

		memset(&steps, 0, sizeof(steps));
		foreach(lc, as.aggrefs)
		{
			Aggref	   *aggref = lfirst(lc);

			vexec_oracle_exprs(root, aggref->args, &steps);
			if (aggref->aggfilter)
				vexec_oracle_expr(root, (Node *) aggref->aggfilter, &steps);
		}
		if (steps.refusal)
		{
			vexec_alt_refuse(ps, alt, steps.refusal);
			return;
		}
	}

	/*
	 * VC13: where the input relation has a serial VecScan whose source's
	 * statistics answer every aggregate unit by unit, the aggregation reads
	 * it, whichever scan is cheapest by rows (below)
	 */
	if (strategy == AGG_PLAIN)
	{
		foreach(lc, input_rel->pathlist)
		{
			Path	   *p = lfirst(lc);

			if (vexec_scan_path_answers(p, &units) && stats_answer(input_rel, p, as.aggrefs, &units))
			{
				input = p;
				answered = true;
				break;
			}
		}
	}

	/* M3: the keys the others imply, out of VecAgg's; core's paths keep them all */
	vgroupclause = independent_keys(groupclause, as.groupexprs, &nimplied, &vgroupexprs);
	foreach(lc, as.groupexprs)
		if (!list_member_ptr(vgroupexprs, lfirst(lc)))
			implied = lappend(implied, lfirst(lc));

	/* PostgreSQL's own Agg over the same input, which the cost derives from */
	MemSet(&agg_costs, 0, sizeof(AggClauseCosts));
	get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &agg_costs);
	rowpath = create_agg_path(root, output_rel, input, output_rel->reltarget, strategy,
							  AGGSPLIT_SIMPLE, groupclause, having, &agg_costs,
							  strategy == AGG_PLAIN ? 1 : numgroups);
	nvec = count_vector_aggs(as.aggrefs, AGGSPLIT_SIMPLE, &names);
	nkeys_kernel = count_kernel_keys(vgroupclause, vgroupexprs);
	vexec_cost_agg(root, &rowpath->path, input, vexec_is_vector_path(input),
				   list_length(vgroupclause), nkeys_kernel, nvec,
				   strategy == AGG_PLAIN ? 1 : numgroups, &cost);

	/*
	 * VC13: an aggregation the source's statistics answer reads its units'
	 * statistics, not its rows: priced at a tuple a unit, and given no
	 * partial pair, whose participants' units -- runs of a part's granules,
	 * in MergeTree -- no statistics answer (Q0 and Q6 of ClickBench, 13 and
	 * 17 ms with 16 workers, 0.04 and 0.1 in one process).
	 */
	if (answered)
	{
		cost.startup = input->startup_cost + units * cpu_tuple_cost;
		cost.total = cost.startup + cpu_tuple_cost;
	}
	{
		StringInfoData buf;

		initStringInfo(&buf);
		foreach(lc, names)
			appendStringInfo(&buf, "%s%s", foreach_current_index(lc) > 0 ? ", " : "",
							 (char *) lfirst(lc));
		vexec_alt_costed(ps, alt, &cost,
						 psprintf("%s, %s%s, %s, %d with vector transitions (%s)",
								  strategy == AGG_HASHED ? "hashed" : "plain",
								  count_of(list_length(vgroupclause), "grouping column", "grouping columns"),
								  nimplied > 0 ? psprintf(" (%d more implied by them)", nimplied) : "",
								  count_of(list_length(as.aggrefs), "aggregate", "aggregates"),
								  nvec, buf.data));
	}
	if (!add)
		return;

	memset(&info, 0, sizeof(info));
	info.strategy = strategy;
	info.split = AGGSPLIT_SIMPLE;
	info.numgroups = numgroups;
	info.groupclause = vgroupclause;
	info.having = having;

	/*
	 * F20: where vexec.min_rows refuses the input's rows, VecAgg reads a
	 * vector scan's batches alone -- this path's input, or the pair's
	 * partial one's (below).  F22: the scan without the implied keys.
	 */
	few = few_rows(ps, input_rel, input);
	if (!few || vector_scan_input(input))
		newpaths = lappend(newpaths, agg_path(root, output_rel,
											  scan_without_implied(root, input, implied),
											  output_rel->reltarget, &info, &cost));

	/*
	 * W7: where a bounded sort above may lend the grouping its rank, the
	 * device's variant of the path over the cheapest input, priced as the
	 * device's where the CPU's is the cheaper here -- kept aside for the
	 * sort, never added, which add_path() would drop
	 */
	if (strategy == AGG_HASHED && !answered && !info.device && ranked_above(root) &&
		(!few || vector_scan_input(input)))
	{
		AggPathInfo vinfo = info;
		Path	   *vp;

		vinfo.device_always = true;
		vp = agg_path(root, output_rel, scan_without_implied(root, input, implied),
					  output_rel->reltarget, &vinfo, &cost);
		if (vinfo.device)
			variants = lappend(variants, vp);
	}

	/*
	 * W1: the device's whole grouping over the relation's serial VecScan,
	 * where the cheapest input is not one: the device then reads every row
	 * as one task, or the scan's units itself -- W9, a plain aggregation's
	 * too, its one group
	 */
	if (!answered)
	{
		Path	   *dinput = device_input(input_rel, input);

		if (dinput != NULL)
		{
			AggPath    *drow = create_agg_path(root, output_rel, dinput, output_rel->reltarget,
											   strategy, AGGSPLIT_SIMPLE, groupclause, having,
											   &agg_costs, strategy == AGG_PLAIN ? 1 : numgroups);
			AggPathInfo dinfo = info;
			VexecCost	dcost;
			Path	   *dp;

			vexec_cost_agg(root, &drow->path, dinput, true, list_length(vgroupclause),
						   nkeys_kernel, nvec, strategy == AGG_PLAIN ? 1 : numgroups, &dcost);
			dp = agg_path(root, output_rel, dinput, output_rel->reltarget, &dinfo, &dcost);
			if (dinfo.device)
			{
				newpaths = lappend(newpaths, dp);
				vexec_alt_costed(ps, alt, &dcost, "the device's whole grouping, over the relation's serial scan");
			}
			else if (ranked_above(root))
			{
				/* W7: its device variant, for the sort */
				dinfo = info;
				dinfo.device_always = true;
				dp = agg_path(root, output_rel, dinput, output_rel->reltarget, &dinfo, &dcost);
				if (dinfo.device)
					variants = lappend(variants, dp);
			}
		}
	}

	/*
	 * VC4: over a serial VecScan whose source hands its rows in an order the
	 * keys are a prefix of, the groups made as they come -- priced as
	 * PostgreSQL's sorted Agg of that input, which holds no table and so
	 * never spills, and begins handing groups up at once; hashed where the
	 * scan's rows do not come so when it runs (exec/vecagg.c).
	 */
	if (strategy == AGG_HASHED && !answered)
	{
		Path	   *sinput = sorted_input_path(root, input_rel, input, vgroupclause, ps);

		if (sinput != NULL)
		{
			AggPath    *srow = create_agg_path(root, output_rel, sinput, output_rel->reltarget,
											   AGG_SORTED, AGGSPLIT_SIMPLE, groupclause, having,
											   &agg_costs, numgroups);
			VexecCost	scost;
			AggPathInfo sinfo = info;

			vexec_cost_agg(root, &srow->path, sinput, true, list_length(vgroupclause),
						   nkeys_kernel, nvec, numgroups, &scost);
			/* the savings are the rows', not the start's: the first group closes after a few */
			scost.startup = sinput->startup_cost + vexec_batch_setup_cost;
			scost.total = Max(scost.total, scost.startup);
			sinfo.sorted_input = true;
			newpaths = lappend(newpaths, agg_path(root, output_rel, sinput, output_rel->reltarget,
												  &sinfo, &scost));
		}
	}

	/*
	 * The partial and final pair, over the input's cheapest partial path,
	 * where core made the partially grouped relation and every aggregate
	 * can be split.
	 */
	if (!answered && (extra->flags & GROUPING_CAN_PARTIAL_AGG) && input_rel->partial_pathlist != NIL &&
		output_rel->consider_parallel)
	{
		RelOptInfo *prel = partial_grouped_rel(root, output_rel);
		Path	   *pinput = linitial(input_rel->partial_pathlist);
		AggScan		pas;

		if (prel != NULL && pinput->parallel_workers > 0 && (!few || vector_scan_input(pinput)) &&
			agg_refusal(root, groupclause, prel->reltarget, NIL, pinput->pathtarget, &pas) == NULL)
		{
			/* M3: the partial groups without the expressions their keys imply */
			PathTarget *ptarget = without_implied(root, prel->reltarget, implied);
			AggPathInfo pinfo = info;
			AggPathInfo finfo = info;
			AggPath    *prow;
			AggPath    *frow;
			VexecCost	pcost;
			VexecCost	fcost;
			Path	   *partial;
			Path	   *gather;
			double		pgroups;
			double		total_groups;
			List	   *gexprs = get_sortgrouplist_exprs(groupclause, root->processed_tlist);
			AggClauseCosts pcosts;
			AggClauseCosts fcosts;

			pgroups = strategy == AGG_PLAIN ? 1 :
				Min(estimate_num_groups(root, gexprs, pinput->rows, NULL, NULL), pinput->rows);
			MemSet(&pcosts, 0, sizeof(AggClauseCosts));
			get_agg_clause_costs(root, AGGSPLIT_INITIAL_SERIAL, &pcosts);
			MemSet(&fcosts, 0, sizeof(AggClauseCosts));
			get_agg_clause_costs(root, AGGSPLIT_FINAL_DESERIAL, &fcosts);

			prow = create_agg_path(root, prel, pinput, prel->reltarget, strategy,
								   AGGSPLIT_INITIAL_SERIAL, groupclause, NIL, &pcosts, pgroups);
			vexec_cost_agg(root, &prow->path, pinput, vexec_is_vector_path(pinput),
						   list_length(vgroupclause), nkeys_kernel,
						   count_vector_aggs(pas.aggrefs, AGGSPLIT_INITIAL_SERIAL, NULL),
						   pgroups, &pcost);
			pinfo.split = AGGSPLIT_INITIAL_SERIAL;
			pinfo.numgroups = pgroups;
			pinfo.having = NIL;
			partial = agg_path(root, prel, scan_without_implied(root, pinput, implied), ptarget,
							   &pinfo, &pcost);
			partial->parallel_safe = true;

			total_groups = partial->rows * partial->parallel_workers;
			gather = (Path *) create_gather_path(root, prel, partial, ptarget, NULL,
												 &total_groups);

			frow = create_agg_path(root, output_rel, gather, output_rel->reltarget, strategy,
								   AGGSPLIT_FINAL_DESERIAL, groupclause, having, &fcosts,
								   strategy == AGG_PLAIN ? 1 : numgroups);
			vexec_cost_agg(root, &frow->path, gather, false, list_length(vgroupclause),
						   nkeys_kernel, 0, strategy == AGG_PLAIN ? 1 : numgroups, &fcost);
			finfo.split = AGGSPLIT_FINAL_DESERIAL;
			newpaths = lappend(newpaths, agg_path(root, output_rel, gather,
												  output_rel->reltarget, &finfo, &fcost));

			/*
			 * H5: the final stage in each participant, over the partial
			 * groups dealt out by their keys (above): a partial path of the
			 * grouped relation, and the Gather of its final groups.
			 */
			if (strategy == AGG_HASHED && vexec_enable_repartition && groupclause != NIL)
			{
				Path	   *repart = repart_path(root, prel, partial, vgroupclause);
				double		each = clamp_row_est(numgroups / Max(partial->parallel_workers, 1));
				AggPath    *erow;
				VexecCost	ecost;
				Path	   *pfinal;
				Path	   *gather2;
				AggPathInfo einfo = finfo;

				erow = create_agg_path(root, output_rel, repart, output_rel->reltarget, strategy,
									   AGGSPLIT_FINAL_DESERIAL, groupclause, having, &fcosts, each);
				vexec_cost_agg(root, &erow->path, repart, true, list_length(vgroupclause),
							   nkeys_kernel, 0, each, &ecost);
				einfo.numgroups = each;
				pfinal = agg_path(root, output_rel, repart, output_rel->reltarget, &einfo, &ecost);
				split_shape_price(pfinal, repart, split_shape(newpaths));	/* W3 */
				pfinal->parallel_safe = true;
				pfinal->parallel_workers = partial->parallel_workers;
				add_partial_path(output_rel, pfinal);
				gather2 = (Path *) create_gather_path(root, output_rel, pfinal, output_rel->reltarget,
													  NULL, &numgroups);
				newpaths = lappend(newpaths, gather2);
				ps->sorts_built = true; /* the scan tuple finished (join.c) */
			}
		}
	}

	/*
	 * Force mode: wherever the oracle accepts it, the aggregation is
	 * VecAgg's, the simple one or the pair as their costs choose; and the
	 * device's, where vexec.gpu forces it (W1).  The device's path kept
	 * aside for a bounded sort above, which add_path() may drop (W7).
	 */
	keep_device_path(ps, output_rel, list_concat_copy(newpaths, variants));
	device_forced(output_rel, newpaths);
	if (ps->mode == VEXEC_MODE_FORCE)
		output_rel->pathlist = NIL;
	foreach(lc, newpaths)
		add_path(output_rel, lfirst(lc));
	ps->npossible++;
}

/*
 * C3 (§3.19): whether a path is a VecAgg that a bounded VecSort above may
 * lend its bound to, which then finishes and hands up only the bound's
 * groups (exec/vecagg.c): a hashed aggregation that finalizes.
 */
bool
vexec_agg_path_ranks(Path *path)
{
	CustomPath *cp;
	AggStrategy strategy;
	AggSplit	split;

	if (!IsA(path, CustomPath) || ((CustomPath *) path)->methods != &vecagg_path_methods)
		return false;
	cp = (CustomPath *) path;
	strategy = intVal(linitial(cp->custom_private));
	split = intVal(lsecond(cp->custom_private));
	return strategy == AGG_HASHED && !DO_AGGSPLIT_SKIPFINAL(split);
}

/* ---------------------------------------------------------------------
 * Plans
 * ---------------------------------------------------------------------
 */

/*
 * An expression in executor form over the child's output: what matches an
 * entry of the child's target list as a whole becomes an OUTER_VAR Var of
 * it, as fix_upper_expr() makes it (setrefs.c); a Var that matches none is
 * an error, as it is there.
 */
typedef struct OuterContext
{
	List	   *child_tlist;
} OuterContext;

static Node *
to_outer_mutator(Node *node, OuterContext *ctx)
{
	TargetEntry *tle;

	if (node == NULL)
		return NULL;
	if (!IsA(node, Const) && !IsA(node, Param))
	{
		tle = tlist_member((Expr *) node, ctx->child_tlist);
		if (tle != NULL)
		{
			Var		   *v = makeVarFromTargetEntry(OUTER_VAR, tle);

			v->varnosyn = 0;
			v->varattnosyn = 0;
			return (Node *) v;
		}
	}
	if (IsA(node, Var))
		elog(ERROR, "vexec: an aggregate's variable is not in its input's target list");
	if (IsA(node, PlaceHolderVar))
		return to_outer_mutator((Node *) ((PlaceHolderVar *) node)->phexpr, ctx);
	return expression_tree_mutator(node, to_outer_mutator, ctx);
}

static Node *
to_outer(Node *node, List *child_tlist)
{
	OuterContext ctx = {child_tlist};

	node = to_outer_mutator(node, &ctx);
	fix_opfuncids(node);
	return node;
}

/*
 * The executor's Aggref of a planner's one: its arguments and FILTER over
 * the child, or, combining, its one argument the partial state the child
 * gives (convert_combining_aggrefs(), setrefs.c).  An Aggref of a stage
 * between two others (X1) is already the partial one the child gives, and
 * its output keeps the partial type: the state, serialized where it is
 * internal, as the child hands it on.
 */
static Aggref *
exec_aggref(Aggref *aggref, AggSplit split, List *child_tlist)
{
	Aggref	   *a = makeNode(Aggref);

	memcpy(a, aggref, sizeof(Aggref));
	if (DO_AGGSPLIT_COMBINE(split))
	{
		Aggref	   *child = aggref;

		if (aggref->aggsplit == AGGSPLIT_SIMPLE)
		{
			child = makeNode(Aggref);
			memcpy(child, aggref, sizeof(Aggref));
			mark_partial_aggref(child, AGGSPLIT_INITIAL_SERIAL);
		}
		a->args = list_make1(makeTargetEntry((Expr *) to_outer((Node *) child, child_tlist),
											 1, NULL, false));
		a->aggfilter = NULL;
		if (aggref->aggsplit == AGGSPLIT_SIMPLE)
			mark_partial_aggref(a, split);
		else
		{
			Assert(DO_AGGSPLIT_SKIPFINAL(split) && DO_AGGSPLIT_SKIPFINAL(aggref->aggsplit));
			a->aggsplit = split;
		}
	}
	else
	{
		a->args = (List *) to_outer((Node *) aggref->args, child_tlist);
		a->aggfilter = (Expr *) to_outer((Node *) aggref->aggfilter, child_tlist);
		if (a->aggsplit != split)
			mark_partial_aggref(a, split);
	}
	return a;
}

/*
 * Each aggregate once, numbered as VecAgg sets them up: aggno its place,
 * aggtransno shared where PostgreSQL's planner shared it.
 */
static void
number_aggrefs(List *aggrefs, List *orig_transnos, int *ntrans)
{
	List	   *seen = NIL;
	ListCell   *lc1,
			   *lc2;
	int			aggno = 0;

	forboth(lc1, aggrefs, lc2, orig_transnos)
	{
		Aggref	   *a = lfirst_node(Aggref, lc1);
		int			orig = lfirst_int(lc2);
		int			t = orig >= 0 ? list_position_int(seen, orig) : -1;

		a->aggno = aggno++;
		if (t < 0)
		{
			seen = lappend_int(seen, orig >= 0 ? orig : -1 - a->aggno);
			t = list_length(seen) - 1;
		}
		a->aggtransno = t;
	}
	*ntrans = list_length(seen);
}

static int
list_position_int(List *list, int value)
{
	ListCell   *lc;

	foreach(lc, list)
		if (lfirst_int(lc) == value)
			return foreach_current_index(lc);
	return -1;
}

/*
 * A grouping column of the child: the one its sortgroupref marks, or, for
 * one the stage added -- H3's DISTINCT argument -- the child's column of the
 * expression the path's target marks with it.
 */
static TargetEntry *
get_sortgroupref_tle_or_expr(Index ref, PathTarget *target, List *child_tlist)
{
	ListCell   *lc;
	int			i = 0;

	foreach(lc, child_tlist)
		if (lfirst_node(TargetEntry, lc)->ressortgroupref == ref)
			return lfirst(lc);
	foreach(lc, target->exprs)
	{
		if (get_pathtarget_sortgroupref(target, i) == ref)
		{
			TargetEntry *tle = tlist_member(lfirst(lc), child_tlist);

			if (tle != NULL)
				return tle;
		}
		i++;
	}
	elog(ERROR, "vexec: grouping column %u is not in the aggregation's input", ref);
	return NULL;
}

/*
 * C5 (§3.19): the groups a LIMIT without ORDER BY takes from the query's
 * aggregation -- its count and offset, both constants -- which the VecAgg
 * then keeps by the K smallest hashes of their keys (vecagg.c): any K
 * groups are an answer.  0 where it may not: an ORDER BY, a HAVING,
 * DISTINCT, window functions, grouping sets or set-returning functions
 * after the aggregation; a DISTINCT aggregate's stage, which groups by more
 * than the query's keys; an aggregate with an ORDER BY; or an aggregate's
 * argument or FILTER that may raise, which a skipped row would leave
 * unevaluated where PostgreSQL evaluates it.  As ClickHouse turns its top-K
 * off (enable_group_by_top_k_optimization, CH:src/Processors/QueryPlan/
 * Optimizations/optimizeGroupByTopK.cpp).
 */
#define VEXEC_AGG_TOPK_MAX	100000

static int64
query_topk(PlannerInfo *root, List *aggrefs, Index distinct_ref, bool distinct_combine)
{
	Query	   *parse = root->parse;
	Const	   *count = (Const *) parse->limitCount;
	int64		n;
	int64		offset = 0;
	ListCell   *lc;

	if (!vexec_enable_group_topk || count == NULL || !IsA(count, Const) ||
		count->constisnull || parse->limitOption != LIMIT_OPTION_COUNT ||
		parse->sortClause != NIL || parse->havingQual != NULL ||
		parse->distinctClause != NIL || parse->hasWindowFuncs ||
		parse->groupingSets != NIL || parse->hasTargetSRFs || parse->groupClause == NIL ||
		distinct_ref > 0 || distinct_combine)
		return 0;
	n = DatumGetInt64(count->constvalue);
	if (n <= 0)
		return 0;
	if (parse->limitOffset != NULL)
	{
		Const	   *off = (Const *) parse->limitOffset;

		if (!IsA(off, Const))
			return 0;
		if (!off->constisnull)
			offset = Max(DatumGetInt64(off->constvalue), 0);
	}
	if (n > VEXEC_AGG_TOPK_MAX || offset > VEXEC_AGG_TOPK_MAX - n)
		return 0;
	foreach(lc, aggrefs)
	{
		Aggref	   *aggref = lfirst_node(Aggref, lc);

		if (aggref->aggdistinct != NIL || aggref->aggorder != NIL ||
			vexec_expr_may_raise((Node *) aggref->args) ||
			vexec_expr_may_raise((Node *) aggref->aggfilter))
			return 0;
	}
	return n + offset;
}

/* The plan of a VecAgg path. */
static Plan *
plan_vecagg(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *child = linitial(custom_plans);
	List	   *priv = best_path->custom_private;
	VexecAggPlan plan;
	List	   *groupclause = lfourth(priv);
	List	   *having = list_nth(priv, 4);
	Index		distinct_ref = list_length(priv) > 5 ? intVal(list_nth(priv, 5)) : 0;
	bool		distinct_combine = list_length(priv) > 6 && intVal(list_nth(priv, 6)) != 0;
	bool		sorted_input = list_length(priv) > 7 && intVal(list_nth(priv, 7)) != 0;
	bool		device = list_length(priv) > 8 && intVal(list_nth(priv, 8)) != 0;
	bool		whole = list_length(priv) > 9 && intVal(list_nth(priv, 9)) != 0;
	PathTarget *target = best_path->path.pathtarget;
	List	   *scan_tlist = NIL;
	List	   *transnos = NIL;
	AggScan		as;
	ListCell   *lc;
	int			resno = 1;

	(void) clauses;
	memset(&plan, 0, sizeof(plan));
	plan.strategy = intVal(linitial(priv));
	plan.split = intVal(lsecond(priv));
	plan.numgroups = floatVal(lthird(priv));
	plan.sorted_input = sorted_input;
	plan.device = device;
	plan.group_bytes = list_length(priv) > 10 ? floatVal(list_nth(priv, 10)) : 0;

	/* the keys: the child's grouping columns, by sortgroupref */
	memset(&as, 0, sizeof(as));
	foreach(lc, groupclause)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		TargetEntry *tle = get_sortgroupref_tle_or_expr(sgc->tleSortGroupRef, target,
														child->targetlist);

		plan.keycols = lappend_int(plan.keycols, tle->resno);
		plan.eqops = lappend_oid(plan.eqops, sgc->eqop);
		plan.collations = lappend_oid(plan.collations, exprCollation((Node *) tle->expr));
		as.groupexprs = lappend(as.groupexprs, tle->expr);
		scan_tlist = lappend(scan_tlist, makeTargetEntry(copyObject(tle->expr), resno++,
														 NULL, false));
		plan.outkind = lappend_int(plan.outkind, VEXEC_AGGCOL_KEY);
		plan.outsrc = lappend_int(plan.outsrc, foreach_current_index(lc));
	}

	/* the aggregates and the first row's columns the output reads */
	scan_upper((Node *) target->exprs, &as);
	scan_upper((Node *) having, &as);
	foreach(lc, as.extras)
	{
		TargetEntry *tle = tlist_member(lfirst(lc), child->targetlist);

		if (tle == NULL)
			elog(ERROR, "vexec: a column of the aggregation's output is not in its input");
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) copyObject(lfirst(lc)), resno++,
														 NULL, false));
		plan.outkind = lappend_int(plan.outkind, VEXEC_AGGCOL_EXTRA);
		plan.outsrc = lappend_int(plan.outsrc, tle->resno);
	}
	foreach(lc, as.aggrefs)
	{
		Aggref	   *aggref = lfirst_node(Aggref, lc);

		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) copyObject(aggref), resno++,
														 NULL, false));
		plan.outkind = lappend_int(plan.outkind, VEXEC_AGGCOL_AGG);
		plan.outsrc = lappend_int(plan.outsrc, foreach_current_index(lc));
		if (distinct_ref > 0 && aggref->aggdistinct != NIL)
		{
			/*
			 * H3's upper stage: the DISTINCT aggregate whole, its argument
			 * the lower stage's key, a value a group.
			 */
			Aggref	   *a = makeNode(Aggref);
			TargetEntry *xtle = get_sortgroupref_tle(distinct_ref, child->targetlist);
			Var		   *xv = makeVarFromTargetEntry(OUTER_VAR, xtle);

			memcpy(a, aggref, sizeof(Aggref));
			xv->varnosyn = 0;
			xv->varattnosyn = 0;
			a->args = list_make1(makeTargetEntry((Expr *) xv, 1, NULL, false));
			a->aggdistinct = NIL;
			a->aggpresorted = false;
			plan.aggrefs = lappend(plan.aggrefs, a);
		}
		else
		{
			Aggref	   *a = exec_aggref(aggref, plan.split, child->targetlist);

			if (distinct_combine && a->aggdistinct != NIL)
			{
				/*
				 * X1's final stage: count(DISTINCT x)'s input is a count of
				 * the distinct values each participant was dealt -- disjoint
				 * sets, the repartition by the keys and x saw to it --
				 * combined as count's are.
				 */
				a->aggdistinct = NIL;
				a->aggpresorted = false;
			}
			plan.aggrefs = lappend(plan.aggrefs, a);
		}
		transnos = lappend_int(transnos, aggref->aggtransno);
	}
	number_aggrefs(plan.aggrefs, transnos, &plan.ntrans);
	if (plan.strategy == AGG_HASHED)
		plan.topk = query_topk(root, as.aggrefs, distinct_ref, distinct_combine);

	/*
	 * W1: H3's two stages the device's whole (consider_distinct_pair()): the
	 * keys and the distinct count's argument as the lower stage's scan's
	 * columns, which this node then reads itself; the node the device's,
	 * decided again as it begins.  None where the scan does not hand them as
	 * they are.
	 */
	if (whole && distinct_ref > 0 && IsA(child, CustomScan) && outerPlan(child) != NULL)
	{
		Plan	   *scan = outerPlan(child);
		TargetEntry *xtle = get_sortgroupref_tle(distinct_ref, child->targetlist);
		List	   *cols = NIL;

		foreach(lc, as.groupexprs)
		{
			TargetEntry *stle = tlist_member(lfirst(lc), scan->targetlist);

			if (stle == NULL)
				break;
			cols = lappend_int(cols, stle->resno);
		}
		if (list_length(cols) == list_length(as.groupexprs))
		{
			TargetEntry *stle = tlist_member(xtle->expr, scan->targetlist);

			if (stle != NULL)
			{
				plan.whole_cols = lappend_int(cols, stle->resno);
				plan.device = true;
				/*
				 * W19: the other aggregates with it, each in its order: the scan's
				 * column of its argument -- the lower stage's partial transition's,
				 * whose state the combining one here reads -- 0 for none, its
				 * FILTER's (none), and whether it is the distinct count; none of
				 * it where an argument is not a column, which keeps the pair W1's
				 */
				if (list_length(as.aggrefs) > 1)
				{
					List	   *more = NIL;
					bool		ok = as.extras == NIL;

					foreach(lc, as.aggrefs)
					{
						Aggref	   *orig = lfirst_node(Aggref, lc);
						Aggref	   *pa = NULL;
						Expr	   *parg;
						TargetEntry *atle;
						ListCell   *lt;

						if (orig->aggdistinct != NIL)
						{
							more = lappend_int(more, stle->resno);
							more = lappend_int(more, 0);
							more = lappend_int(more, 1);
							continue;
						}
						/* the lower stage's partial aggregate of it */
						foreach(lt, child->targetlist)
						{
							Expr	   *e = lfirst_node(TargetEntry, lt)->expr;

							if (IsA(e, Aggref) && ((Aggref *) e)->aggfnoid == orig->aggfnoid &&
								equal(((Aggref *) e)->args, orig->args) && equal(((Aggref *) e)->aggfilter, orig->aggfilter))
							{
								pa = (Aggref *) e;
								break;
							}
						}
						if (pa == NULL || pa->aggfilter != NULL || list_length(pa->args) > 1)
						{
							ok = false;
							break;
						}
						parg = pa->args != NIL ? linitial_node(TargetEntry, pa->args)->expr : NULL;
						atle = parg != NULL ? tlist_member(parg, scan->targetlist) : NULL;
						if (parg != NULL && atle == NULL && IsA(parg, Var) && ((Var *) parg)->varno == OUTER_VAR)
							atle = get_tle_by_resno(scan->targetlist, ((Var *) parg)->varattno);
						if (parg != NULL && atle == NULL)
						{
							ok = false;
							break;
						}
						more = lappend_int(more, atle != NULL ? atle->resno : 0);
						more = lappend_int(more, 0);
						more = lappend_int(more, 0);
					}
					if (ok)
						plan.whole_cols = list_concat(plan.whole_cols, more);
				}
			}
		}
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = having;
	cscan->scan.plan.lefttree = child;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = vexec_agg_plan_encode(&plan);
	cscan->methods = vexec_agg_methods();
	(void) root;
	(void) rel;
	return &cscan->scan.plan;
}

/* ---------------------------------------------------------------------
 * ORCA's Agg
 * ---------------------------------------------------------------------
 */

typedef struct OrcaAggContext
{
	Agg		   *agg;
	List	   *scan_tlist;
	VexecAggPlan *plan;
} OrcaAggContext;

/* A scan tuple column for an expression, made where it is new. */
static int
scan_column(OrcaAggContext *ctx, Expr *expr, int kind, int src)
{
	ListCell   *lc;

	foreach(lc, ctx->scan_tlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (equal(tle->expr, expr))
			return tle->resno;
	}
	ctx->scan_tlist = lappend(ctx->scan_tlist,
							  makeTargetEntry(copyObject(expr), list_length(ctx->scan_tlist) + 1,
											  NULL, false));
	ctx->plan->outkind = lappend_int(ctx->plan->outkind, kind);
	ctx->plan->outsrc = lappend_int(ctx->plan->outsrc, src);
	return list_length(ctx->scan_tlist);
}

/*
 * The Agg's target list and qual over the scan tuple: an aggregate, a
 * grouping column and any other column of the child read as INDEX_VAR.
 */
static Node *
orca_index_mutator(Node *node, OrcaAggContext *ctx)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;
		int			before = list_length(ctx->scan_tlist);
		int			col;

		col = scan_column(ctx, (Expr *) aggref, VEXEC_AGGCOL_AGG, list_length(ctx->plan->aggrefs));
		if (list_length(ctx->scan_tlist) > before)
			ctx->plan->aggrefs = lappend(ctx->plan->aggrefs, copyObject(aggref));
		return (Node *) makeVar(INDEX_VAR, col, exprType(node), exprTypmod(node),
								exprCollation(node), 0);
	}
	if (IsA(node, Var) && ((Var *) node)->varno == OUTER_VAR)
	{
		Var		   *var = (Var *) node;
		int			col = -1;

		for (int i = 0; i < ctx->agg->numCols; i++)
			if (ctx->agg->grpColIdx[i] == var->varattno)
				col = i + 1;
		if (col < 0)
			col = scan_column(ctx, (Expr *) var, VEXEC_AGGCOL_EXTRA, var->varattno);
		return (Node *) makeVar(INDEX_VAR, col, var->vartype, var->vartypmod,
								var->varcollid, 0);
	}
	return expression_tree_mutator(node, orca_index_mutator, ctx);
}

/* Whether ORCA's Agg can be a VecAgg, and why not. */
const char *
vexec_orca_agg_refusal(Agg *agg)
{
	AggScan		as;

	if (agg->aggstrategy != AGG_PLAIN && agg->aggstrategy != AGG_HASHED)
		return agg->aggstrategy == AGG_SORTED ? "a sorted aggregation, whose order its parent may read" :
			"a mixed aggregation";
	if (agg->groupingSets != NIL || agg->chain != NIL)
		return "grouping sets";
	if (agg->plan.lefttree == NULL)
		return "no input";
	memset(&as, 0, sizeof(as));
	scan_upper((Node *) agg->plan.targetlist, &as);
	scan_upper((Node *) agg->plan.qual, &as);
	if (as.refusal == NULL && as.distinct != NIL)
		return "DISTINCT inside an aggregate";	/* ORCA splits its own, or keeps one Agg */
	return as.refusal;
}

/*
 * W1: whether a plan reads a participant's share of its rows: a
 * parallel-aware node in it, short of a Gather's.
 */
static bool
plan_parallel_aware(Plan *p)
{
	ListCell   *lc;

	if (p == NULL)
		return false;
	if (p->parallel_aware)
		return true;
	if (IsA(p, Gather) || IsA(p, GatherMerge))
		return false;
	if (IsA(p, CustomScan))
		foreach(lc, ((CustomScan *) p)->custom_plans)
			if (plan_parallel_aware(lfirst(lc)))
				return true;
	if (IsA(p, Append))
		foreach(lc, ((Append *) p)->appendplans)
			if (plan_parallel_aware(lfirst(lc)))
				return true;
	if (IsA(p, SubqueryScan) && plan_parallel_aware(((SubqueryScan *) p)->subplan))
		return true;
	return plan_parallel_aware(p->lefttree) || plan_parallel_aware(p->righttree);
}

/*
 * V8b: whether a VecAgg built from ORCA's Agg takes the device's strategy,
 * by vexec's prices: as agg_device_price() has it for PostgreSQL's planner,
 * from the plan's rows and the Agg's keys and aggregates; and, as there,
 * only where one task reads every row (W1).
 */
static bool
orca_agg_device(Agg *agg, VexecAggPlan *plan)
{
	bool		fast = vexec_precision == VEXEC_PRECISION_FAST;
	Plan	   *input = agg->plan.lefttree;
	double		row_bytes = 0;
	int			nkeys = 0;
	int			naggs = 0;
	Cost		own;
	ListCell   *lc;

	/* W9: a plain aggregation too, one group */
	if (vexec_gpu == VEXEC_GPU_OFF || !vexec_enable_gpu_agg ||
		(agg->aggstrategy != AGG_HASHED && agg->aggstrategy != AGG_PLAIN) ||
		DO_AGGSPLIT_COMBINE(agg->aggsplit) || input == NULL ||
		vexec_device_ready() == NULL || plan_parallel_aware(input))
		return false;
	for (int i = 0; i < agg->numCols; i++)
	{
		TargetEntry *tle = get_tle_by_resno(input->targetlist, agg->grpColIdx[i]);
		int			w;

		if (tle == NULL)
			return false;
		w = device_width(exprType((Node *) tle->expr), exprTypmod((Node *) tle->expr), agg->grpCollations[i], true);
		if (w < 0)
			return false;
		row_bytes += w;
		nkeys++;
	}
	foreach(lc, plan->aggrefs)
	{
		Aggref	   *aggref = lfirst_node(Aggref, lc);
		Oid			argtype = InvalidOid;
		int32		argtypmod = -1;

		if (aggref->aggdistinct != NIL || aggref->aggorder != NIL || list_length(aggref->args) > 1)
			return false;
		if (aggref->args != NIL)
		{
			Expr	   *arg = linitial_node(TargetEntry, aggref->args)->expr;
			int			w = device_width(exprType((Node *) arg), exprTypmod((Node *) arg), InvalidOid, false);

			if (w < 0)
				return false;
			argtype = exprType((Node *) arg);
			argtypmod = exprTypmod((Node *) arg);
			row_bytes += w;
		}
		if (aggref->aggfilter != NULL)
			row_bytes += 1;
		if (!vexec_agg_device_ok(aggref->aggfnoid, agg->aggsplit, argtype, argtypmod, aggref->inputcollid, fast))
			return false;
		naggs++;
	}
	/*
	 * ORCA's costs are its own units: the node's own work in PostgreSQL's,
	 * as the CPU strategy's operator terms over the input's rows
	 */
	own = cpu_operator_cost * vexec_cpu_operator_factor * (nkeys + naggs) * Max(input->plan_rows, 1);
	return vexec_gpu == VEXEC_GPU_FORCE ||
		vexec_cost_agg_device(own, Max(input->plan_rows, 1) * row_bytes,
							  Max(agg->plan.plan_rows, 1) * (nkeys * 8 + naggs * 16 + 16)) < own;
}

/* VecAgg in place of the Agg ORCA's translator built. */
Plan *
vexec_build_agg_from_agg(Agg *agg)
{
	CustomScan *cscan = makeNode(CustomScan);
	VexecAggPlan plan;
	OrcaAggContext ctx;
	List	   *transnos = NIL;
	ListCell   *lc;
	int			i;

	memset(&plan, 0, sizeof(plan));
	plan.strategy = agg->aggstrategy;
	plan.split = agg->aggsplit;
	plan.numgroups = agg->numGroups;
	ctx.agg = agg;
	ctx.scan_tlist = NIL;
	ctx.plan = &plan;

	/* the keys first: scan columns 1 to numCols */
	for (i = 0; i < agg->numCols; i++)
	{
		TargetEntry *tle = get_tle_by_resno(agg->plan.lefttree->targetlist, agg->grpColIdx[i]);
		Var		   *var;

		if (tle == NULL)
			elog(ERROR, "vexec: ORCA's Agg groups by column %d of its input, which has none",
				 agg->grpColIdx[i]);
		var = makeVarFromTargetEntry(OUTER_VAR, tle);
		ctx.scan_tlist = lappend(ctx.scan_tlist, makeTargetEntry((Expr *) var, i + 1, NULL, false));
		plan.outkind = lappend_int(plan.outkind, VEXEC_AGGCOL_KEY);
		plan.outsrc = lappend_int(plan.outsrc, i);
		plan.keycols = lappend_int(plan.keycols, agg->grpColIdx[i]);
		plan.eqops = lappend_oid(plan.eqops, agg->grpOperators[i]);
		plan.collations = lappend_oid(plan.collations, agg->grpCollations[i]);
	}

	cscan->scan.plan = agg->plan;
	cscan->scan.plan.type = T_CustomScan;
	cscan->scan.plan.targetlist = (List *) orca_index_mutator((Node *) agg->plan.targetlist, &ctx);
	cscan->scan.plan.qual = vexec_flat_quals((List *) orca_index_mutator((Node *) agg->plan.qual, &ctx));
	cscan->scan.scanrelid = 0;
	cscan->flags = CUSTOMPATH_SUPPORT_PROJECTION;

	/*
	 * The states ORCA's translator shared, as PostgreSQL's planner shares
	 * them (TranslateAggFillInfo(): find_compatible_agg() and _trans()), so
	 * that a shared transition runs once a row, as in the Agg it replaces.
	 */
	foreach(lc, plan.aggrefs)
		transnos = lappend_int(transnos, lfirst_node(Aggref, lc)->aggtransno);
	number_aggrefs(plan.aggrefs, transnos, &plan.ntrans);

	/*
	 * V8b: the device's strategy by vexec's own prices, after ORCA's search,
	 * which priced the CPU's (§3.11, "ORCA's plans get their strategy as
	 * vexec builds each node")
	 */
	plan.device = orca_agg_device(agg, &plan);

	cscan->custom_scan_tlist = ctx.scan_tlist;
	/* extParam and allParam are the Agg's, which ORCA's translator set */
	cscan->custom_exprs = NIL;
	cscan->custom_private = vexec_agg_plan_encode(&plan);
	cscan->custom_plans = NIL;
	cscan->methods = vexec_agg_methods();
	return &cscan->scan.plan;
}

/*
 * The Agg a VecAgg of ORCA's plan stands for, for a pass over the finished
 * plan that reads it (orca.c, describe_node): its strategy, split, keys and
 * groups, and its target list and qual with each column of the scan tuple
 * made what it is -- a key or a column the group keeps, of the child's
 * output as OUTER_VAR, or an aggregate -- so that each entry keeps the
 * VecAgg's resno and type.  A copy, never put in the plan.
 */
typedef struct DescribeContext
{
	VexecAggPlan *plan;
	List	   *scan_tlist;
} DescribeContext;

static Node *
describe_mutator(Node *node, DescribeContext *ctx)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == INDEX_VAR)
	{
		Var		   *var = (Var *) node;
		int			col = var->varattno - 1;
		int			kind;
		int			src;

		if (col < 0 || col >= list_length(ctx->plan->outkind))
			elog(ERROR, "vexec: a VecAgg's column %d, of %d", var->varattno,
				 list_length(ctx->plan->outkind));
		kind = list_nth_int(ctx->plan->outkind, col);
		src = list_nth_int(ctx->plan->outsrc, col);
		if (kind == VEXEC_AGGCOL_AGG)
			return copyObject(list_nth(ctx->plan->aggrefs, src));
		else
		{
			Var		   *v = copyObject(var);

			v->varno = OUTER_VAR;
			v->varattno = kind == VEXEC_AGGCOL_KEY ?
				list_nth_int(ctx->plan->keycols, src) : src;
			return (Node *) v;
		}
	}
	return expression_tree_mutator(node, describe_mutator, ctx);
}

Agg *
vexec_agg_describe(CustomScan *cscan)
{
	Agg		   *agg = makeNode(Agg);
	VexecAggPlan plan;
	DescribeContext ctx;
	int			i;

	vexec_agg_plan_decode(cscan, &plan);
	ctx.plan = &plan;
	ctx.scan_tlist = cscan->custom_scan_tlist;

	agg->plan = cscan->scan.plan;
	agg->plan.type = T_Agg;
	agg->plan.targetlist = (List *) describe_mutator((Node *) cscan->scan.plan.targetlist, &ctx);
	agg->plan.qual = (List *) describe_mutator((Node *) cscan->scan.plan.qual, &ctx);
	agg->aggstrategy = plan.strategy;
	agg->aggsplit = plan.split;
	agg->numCols = list_length(plan.keycols);
	agg->grpColIdx = palloc(sizeof(AttrNumber) * Max(agg->numCols, 1));
	agg->grpOperators = palloc(sizeof(Oid) * Max(agg->numCols, 1));
	agg->grpCollations = palloc(sizeof(Oid) * Max(agg->numCols, 1));
	for (i = 0; i < agg->numCols; i++)
	{
		agg->grpColIdx[i] = (AttrNumber) list_nth_int(plan.keycols, i);
		agg->grpOperators[i] = list_nth_oid(plan.eqops, i);
		agg->grpCollations[i] = list_nth_oid(plan.collations, i);
	}
	agg->numGroups = (long) plan.numgroups;
	agg->transitionSpace = 0;
	agg->aggParams = NULL;
	agg->groupingSets = NIL;
	agg->chain = NIL;
	return agg;
}
