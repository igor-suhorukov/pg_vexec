/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * oracle.c
 *	  The capability oracle, for expressions (pg_vector_executor.md §3.3.2);
 *	  for types it is batch/types.c.
 *
 * Pure functions over PostgreSQL's nodes, with no error path: an expression
 * is its kernel steps, its fallback steps, or "not in a vector node".
 *
 *	- A call with a kernel runs a batch at a time.  A kernel is found by
 *	  function OID, input types, collation class and the layouts of its
 *	  inputs (§3.7), never by name or through search_path.
 *	- Any other call runs PostgreSQL's own evaluator, row by row, inside the
 *	  same vector node, on the selected rows only: the fallback, which makes
 *	  coverage total.  SubPlans and volatile functions run there, in row
 *	  order, so their call counts and order are PostgreSQL's.
 *	- Not in a vector node: a set-returning function; an Aggref, a GROUPING
 *	  or a window function anywhere but in the node that computes it, since
 *	  they compile only under their own executor states
 *	  (PG19:src/backend/executor/execExpr.c:1096, 1110, 1162); WHERE CURRENT
 *	  OF.
 *
 * The kernels are expr/'s, bound by function OID (expr/kernels.c); a call
 * without one is a fallback step, priced at PostgreSQL's full
 * cpu_operator_cost.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "funcapi.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "parser/parse_type.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "vexec.h"
#include "batch/batch.h"
#include "plan/plan.h"
#include "expr/kernel.h"

PG_FUNCTION_INFO_V1(vexec_type_layouts);

/*
 * Whether a kernel is bound to a function for these inputs (expr/kernels.c):
 * the planner's question, which the node's own binding answers again, with
 * the call's constant arguments, when it begins.
 */
bool
vexec_kernel_bound(Oid funcid, Oid inputtype, Oid collation)
{
	return vexec_kernel_find(funcid, 1, &inputtype, collation) != NULL;
}

typedef struct OracleContext
{
	PlannerInfo *root;
	VexecSteps *steps;
} OracleContext;

/*
 * A call: a kernel step where one is bound, else a fallback step.  A
 * kernel's share is priced as PostgreSQL prices the call
 * (add_function_cost), so the cost model can scale it.
 */
static void
oracle_call(OracleContext *ctx, Oid funcid, Node *node, List *args, Oid collation)
{
	Oid			inputtype = args != NIL ? exprType(linitial(args)) : InvalidOid;

	if (vexec_kernel_bound(funcid, inputtype, collation))
	{
		QualCost	qc = {0, 0};

		/* without a PlannerInfo -- ORCA's plans -- the steps are only counted */
		if (ctx->root != NULL)
			add_function_cost(ctx->root, funcid, node, &qc);
		ctx->steps->kernel++;
		ctx->steps->kernel_cost += qc.per_tuple;
	}
	else
		ctx->steps->fallback++;
}

static bool
oracle_walker(Node *node, void *context)
{
	OracleContext *ctx = (OracleContext *) context;

	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Aggref:
			ctx->steps->refusal = "an aggregate outside its aggregation";
			return true;
		case T_GroupingFunc:
			ctx->steps->refusal = "GROUPING outside its aggregation";
			return true;
		case T_WindowFunc:
			ctx->steps->refusal = "a window function";
			return true;
		case T_MergeSupportFunc:
			ctx->steps->refusal = "MERGE's merge_action()";
			return true;
		case T_CurrentOfExpr:
			ctx->steps->refusal = "WHERE CURRENT OF";
			return true;
		case T_SubLink:
			ctx->steps->refusal = "a sublink the planner has not made a SubPlan";
			return true;
		case T_FuncExpr:
			{
				FuncExpr   *f = (FuncExpr *) node;

				if (f->funcretset)
				{
					ctx->steps->refusal = "a set-returning function";
					return true;
				}
				oracle_call(ctx, f->funcid, node, f->args, f->inputcollid);
				break;
			}
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			{
				OpExpr	   *op = (OpExpr *) node;

				if (op->opretset)
				{
					ctx->steps->refusal = "a set-returning operator";
					return true;
				}
				set_opfuncid(op);
				oracle_call(ctx, op->opfuncid, node, op->args, op->inputcollid);
				break;
			}
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;

				set_sa_opfuncid(saop);
				oracle_call(ctx, saop->opfuncid, node, saop->args, saop->inputcollid);
				break;
			}
		case T_SubPlan:
		case T_AlternativeSubPlan:
			/* row by row, in row order (§3.7) */
			ctx->steps->fallback++;
			break;
		case T_CoerceViaIO:
		case T_ArrayCoerceExpr:
		case T_RowCompareExpr:
		case T_MinMaxExpr:
		case T_SQLValueFunction:
		case T_XmlExpr:
		case T_JsonConstructorExpr:
		case T_JsonIsPredicate:
		case T_JsonExpr:
		case T_NextValueExpr:
			ctx->steps->fallback++;
			break;
		default:
			break;
	}
	return expression_tree_walker(node, oracle_walker, context);
}

/*
 * An expression's steps.  Its costs are PostgreSQL's per-row cost of it,
 * split between the kernels' share and the fallback's.
 */
void
vexec_oracle_expr(PlannerInfo *root, Node *expr, VexecSteps *steps)
{
	OracleContext ctx;
	QualCost	qc;
	VexecSteps	one;

	memset(&one, 0, sizeof(one));
	ctx.root = root;
	ctx.steps = &one;
	(void) oracle_walker(expr, &ctx);

	if (root != NULL)
	{
		cost_qual_eval_node(&qc, expr, root);
		one.fallback_cost = Max(qc.per_tuple - one.kernel_cost, 0);
		one.startup = qc.startup;
	}

	steps->kernel += one.kernel;
	steps->fallback += one.fallback;
	steps->kernel_cost += one.kernel_cost;
	steps->fallback_cost += one.fallback_cost;
	steps->startup += one.startup;
	if (steps->refusal == NULL)
		steps->refusal = one.refusal;
}

/* A list of expressions (quals, a target): their steps together. */
void
vexec_oracle_exprs(PlannerInfo *root, List *exprs, VexecSteps *steps)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		Node	   *e = lfirst(lc);

		if (IsA(e, RestrictInfo))
			e = (Node *) ((RestrictInfo *) e)->clause;
		vexec_oracle_expr(root, e, steps);
	}
}

/*
 * vexec.type_layouts(type text): what the oracle holds a type as -- its
 * class, its layout in the PostgreSQL format and in the Arrow format, and
 * every layout it can be held in.  The type is named as SQL names it, with
 * its typmod: 'numeric(10,2)'.
 */
Datum
vexec_type_layouts(PG_FUNCTION_ARGS)
{
	char	   *typname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Oid			typid;
	int32		typmod;
	VexecType  *type;
	VexecLayoutConfig pg = {VEXEC_FORMAT_POSTGRES, VEXEC_VARLENA_FORMAT, VEXEC_BOOL_FORMAT,
	VEXEC_TEMPORAL_FORMAT, VEXEC_NUMERIC_FORMAT};
	VexecLayoutConfig arrow = {VEXEC_FORMAT_ARROW, VEXEC_VARLENA_FORMAT, VEXEC_BOOL_FORMAT,
	VEXEC_TEMPORAL_FORMAT, VEXEC_NUMERIC_FORMAT};
	VexecShape	shape;
	VexecShape	shapes[8];
	int			nshapes;
	Datum	   *names;
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};
	int			i;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	parseTypeString(typname, &typid, &typmod, NULL);
	type = vexec_type_make(typid, typmod, InvalidOid);

	values[0] = CStringGetTextDatum(vexec_type_class_name(type->tclass));
	vexec_type_shape(type, &pg, &shape);
	values[1] = CStringGetTextDatum(vexec_shape_name(type, &shape));
	vexec_type_shape(type, &arrow, &shape);
	values[2] = CStringGetTextDatum(vexec_shape_name(type, &shape));
	nshapes = vexec_type_shapes(type, shapes, lengthof(shapes));
	names = palloc(sizeof(Datum) * nshapes);
	for (i = 0; i < nshapes; i++)
		names[i] = CStringGetTextDatum(vexec_shape_name(type, &shapes[i]));
	values[3] = PointerGetDatum(construct_array_builtin(names, nshapes, TEXTOID));

	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}
