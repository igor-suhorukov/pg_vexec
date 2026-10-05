/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * compile.c
 *	  A vector node's expressions into vector programs, when the node begins
 *	  (pg_vector_executor.md §3.7; expr.h says what eager and lazy are).
 *
 * An expression compiles into a tree of steps:
 *
 *	- a call with a kernel, whose arguments compile in turn: the kernel never
 *	  raises, and defers a row PostgreSQL's function would raise on to
 *	  PostgreSQL's evaluator (expr.h);
 *	- boolean logic, NULL and boolean tests, a relabelling, a column of the
 *	  node's input, a constant, a parameter;
 *	- the fallback, for a subtree whose root has none of these forms:
 *	  PostgreSQL's evaluator, a row at a time, over the node's input row.
 *
 * A step runs ahead of PostgreSQL's order -- for every row of a batch the
 * qual or target reaches, before the node's consumer asks for any -- so a
 * fallback subtree must be pure: it can neither raise an error that depends
 * on its rows nor have a side effect.  Every function in it is not volatile
 * and is leakproof, which by PostgreSQL's definition reveals nothing of its
 * arguments through errors (CREATE FUNCTION's LEAKPROOF); and it holds
 * nothing PostgreSQL's evaluator may raise on of its own accord: a SubPlan,
 * a coercion through I/O, a domain's check, an array built of arrays.  Its
 * calls with kernels count for nothing there, since the fallback runs them
 * through PostgreSQL's evaluator.
 *
 * A qual or target that does not compile is lazy: PostgreSQL's evaluator
 * runs it a row at a time, in row order, for the rows the node's consumer
 * asks for, so its errors, call counts and order stay PostgreSQL's.
 *
 * A PARAM_EXEC compiles: an InitPlan it would run is run only where
 * PostgreSQL would run it (eval.c).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

#include "vexec.h"
#include "batch/batch.h"
#include "expr/expr.h"
#include "expr/kernel.h"

/*
 * Whether a function may be called ahead of PostgreSQL's order through
 * PostgreSQL's evaluator: not volatile, and leakproof.
 */
static bool
func_pure(Oid funcid)
{
	return func_volatile(funcid) != PROVOLATILE_VOLATILE && get_func_leakproof(funcid);
}

/* The argument types of a call, for binding. */
static int
arg_types(List *args, Oid *types)
{
	ListCell   *lc;
	int			n = 0;

	foreach(lc, args)
	{
		if (n < VEXEC_KERNEL_MAXARGS)
			types[n] = exprType(lfirst(lc));
		n++;
	}
	return n;
}

/*
 * The kernel a call can use, or NULL: one is bound to the function for
 * these inputs, and takes the call -- its constant arguments, its
 * collation.  An IN list needs its array a constant.  What the kernel
 * keeps of the call goes into *extra.
 */
static const VexecKernelEntry *
usable_kernel(Expr *expr, Oid funcid, List *args, Oid inputcollid, void **extra)
{
	Oid			types[VEXEC_KERNEL_MAXARGS];
	int			n = arg_types(args, types);
	const VexecKernelEntry *ke;
	VexecExpr	probe;

	if (n > VEXEC_KERNEL_MAXARGS)
		return NULL;
	if (IsA(expr, ScalarArrayOpExpr))
	{
		/* the comparison of x with each element of a constant array */
		if (n != 2 || !IsA(lsecond(args), Const))
			return NULL;
		types[1] = get_element_type(types[1]);
		if (!OidIsValid(types[1]))
			return NULL;
	}
	ke = vexec_kernel_find(funcid, n, types, inputcollid);
	if (ke == NULL)
		return NULL;
	memset(&probe, 0, sizeof(probe));
	probe.kind = IsA(expr, ScalarArrayOpExpr) ? VE_SAOP :
		IsA(expr, DistinctExpr) ? VE_DISTINCT : VE_CALL;
	probe.expr = expr;
	probe.funcid = funcid;
	probe.collation = inputcollid;
	probe.nargs = n;
	if (IsA(expr, ScalarArrayOpExpr))
		probe.useOr = ((ScalarArrayOpExpr *) expr)->useOr;
	probe.extra = (void *) ke->info;
	if (ke->def->bind != NULL && !ke->def->bind(&probe, ke->info))
		return NULL;
	if (extra)
		*extra = probe.extra;
	return ke;
}

/* Why a subtree cannot run ahead through PostgreSQL's evaluator. */
static bool
impurity_walker(Node *node, const char **why)
{
	if (node == NULL)
		return false;

	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
		case T_RelabelType:
		case T_CollateExpr:
		case T_BoolExpr:
		case T_BooleanTest:
		case T_NullTest:
		case T_CaseExpr:
		case T_CaseWhen:
		case T_CaseTestExpr:
		case T_CoalesceExpr:
		case T_RowExpr:
		case T_FieldSelect:
		case T_SQLValueFunction:
		case T_List:
			break;
		case T_Param:
			{
				Param	   *p = (Param *) node;

				if (p->paramkind != PARAM_EXTERN && p->paramkind != PARAM_EXEC)
				{
					*why = "a sublink's parameter";
					return true;
				}
				break;
			}
		case T_FuncExpr:
			{
				FuncExpr   *f = (FuncExpr *) node;

				if (f->funcretset)
				{
					*why = "a set-returning function";
					return true;
				}
				if (!func_pure(f->funcid))
				{
					*why = psprintf("%s(), which may raise", get_func_name(f->funcid));
					return true;
				}
				break;
			}
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			{
				OpExpr	   *op = (OpExpr *) node;

				set_opfuncid(op);
				if (op->opretset)
				{
					*why = "a set-returning operator";
					return true;
				}
				if (!func_pure(op->opfuncid))
				{
					*why = psprintf("%s(), which may raise", get_func_name(op->opfuncid));
					return true;
				}
				break;
			}
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) node;

				set_sa_opfuncid(saop);
				if (!func_pure(saop->opfuncid) ||
					(OidIsValid(saop->hashfuncid) && !func_pure(saop->hashfuncid)))
				{
					*why = psprintf("%s(), which may raise", get_func_name(saop->opfuncid));
					return true;
				}
				break;
			}
		case T_SubPlan:
		case T_AlternativeSubPlan:
			*why = "a SubPlan, run a row at a time";
			return true;
		default:
			*why = "an expression PostgreSQL's evaluator may raise on";
			return true;
	}
	return expression_tree_walker(node, impurity_walker, why);
}

/*
 * Why an expression cannot run ahead of PostgreSQL's order through
 * PostgreSQL's evaluator, or NULL when it can.
 */
const char *
vexec_expr_impurity(Node *expr, bool kernels)
{
	const char *why = NULL;

	(void) kernels;
	(void) impurity_walker(expr, &why);
	return why;
}

/*
 * The collation class a kernel is bound by (§3.7): C, other deterministic,
 * nondeterministic.  C_COLLATION_OID stands for every collation that
 * compares bytes, DEFAULT_COLLATION_OID for the other deterministic ones,
 * InvalidOid for nondeterministic ones and for none.
 */
Oid
vexec_collation_class(Oid collation)
{
	pg_locale_t locale;

	if (!OidIsValid(collation))
		return InvalidOid;
	locale = pg_newlocale_from_collation(collation);
	if (!locale->deterministic)
		return InvalidOid;
	return locale->collate_is_c ? C_COLLATION_OID : DEFAULT_COLLATION_OID;
}

static VexecExpr *
new_expr(VexecCompileContext *cc, VexecExprKind kind, Expr *expr, int nargs)
{
	VexecExpr  *e = MemoryContextAllocZero(cc->mcxt, sizeof(VexecExpr));
	MemoryContext old = MemoryContextSwitchTo(cc->mcxt);

	e->kind = kind;
	e->expr = expr;
	e->type = vexec_type_make(exprType((Node *) expr), exprTypmod((Node *) expr),
							  exprCollation((Node *) expr));
	e->nargs = nargs;
	if (nargs > 0)
		e->args = palloc0(sizeof(VexecExpr *) * nargs);
	e->col = -1;
	e->paramid = -1;
	MemoryContextSwitchTo(old);
	return e;
}

/* A constant as a one-value batch column, in the node's memory. */
static VexecVec *
const_vec(VexecCompileContext *cc, Const *c, VexecType *type)
{
	VexecBatch *b = vexec_batch_create(cc->mcxt, 1, &type);
	Datum		value = c->constvalue;
	bool		isnull = c->constisnull;
	VexecVec   *v;

	vexec_batch_begin_rows(b);
	vexec_batch_add_values(b, &value, &isnull);
	v = MemoryContextAlloc(cc->mcxt, sizeof(VexecVec));
	*v = b->cols[0];
	v->encoding = VEXEC_CONST;
	v->nvalues = 1;
	return v;
}

/* A Var of the node's input, or NULL when the input has no column for it. */
static VexecExpr *
compile_var(VexecCompileContext *cc, Var *var)
{
	int			idx;
	VexecExpr  *e;

	if (var->varno != cc->input_varno || var->varlevelsup != 0)
		return NULL;
	idx = var->varattno + cc->attno_base;
	if (idx < 0 || var->varattno > cc->attno_max || cc->attno_col[idx] < 0)
		return NULL;
	e = new_expr(cc, VE_VAR, (Expr *) var, 0);
	e->col = cc->attno_col[idx];
	return e;
}

/* An IN list's array as constant registers, one per element. */
static VexecSaop *
compile_saop_array(VexecCompileContext *cc, Const *arr)
{
	VexecSaop  *s = MemoryContextAllocZero(cc->mcxt, sizeof(VexecSaop));
	MemoryContext old = MemoryContextSwitchTo(cc->mcxt);

	if (arr->constisnull)
		s->array_null = true;
	else
	{
		ArrayType  *a = DatumGetArrayTypeP(arr->constvalue);
		Oid			elemtype = ARR_ELEMTYPE(a);
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		Datum	   *values;
		bool	   *nulls;
		int			i;
		VexecType  *type = vexec_type_make(elemtype, exprTypmod((Node *) arr), arr->constcollid);

		get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
		deconstruct_array(a, elemtype, elmlen, elmbyval, elmalign, &values, &nulls, &s->nelems);
		s->elems = palloc0(sizeof(VexecVec *) * Max(s->nelems, 1));
		for (i = 0; i < s->nelems; i++)
		{
			Const	   *c;

			if (nulls[i])
				continue;
			c = makeConst(elemtype, -1, arr->constcollid, elmlen, values[i], false, elmbyval);
			s->elems[i] = const_vec(cc, c, type);
		}
	}
	MemoryContextSwitchTo(old);
	return s;
}

/*
 * A call with a kernel, its arguments compiled; NULL when it has no kernel
 * or an argument does not compile.
 */
static VexecExpr *
compile_call(VexecCompileContext *cc, Expr *expr, VexecExprKind kind, Oid funcid,
			 List *args, Oid inputcollid)
{
	void	   *extra = NULL;
	const VexecKernelEntry *ke = usable_kernel(expr, funcid, args, inputcollid, &extra);
	VexecExpr  *e;
	ListCell   *lc;
	int			i = 0;

	if (ke == NULL)
		return NULL;
	e = new_expr(cc, kind, expr, list_length(args));
	e->kernel = ke->def;
	e->funcid = funcid;
	e->collation = inputcollid;
	e->strict = func_strict(funcid);
	e->extra = extra;
	if (IsA(expr, ScalarArrayOpExpr))
	{
		e->useOr = ((ScalarArrayOpExpr *) expr)->useOr;
		e->saop = compile_saop_array(cc, lsecond_node(Const, args));
	}
	foreach(lc, args)
	{
		e->args[i] = vexec_compile_expr(cc, lfirst(lc));
		if (e->args[i] == NULL)
			return NULL;
		i++;
	}
	{
		MemoryContext old = MemoryContextSwitchTo(cc->mcxt);

		e->finfo = palloc0(sizeof(FmgrInfo));
		fmgr_info(funcid, e->finfo);
		fmgr_info_set_expr((Node *) expr, e->finfo);
		MemoryContextSwitchTo(old);
	}
	cc->nkernels++;
	return e;
}

/*
 * The fallback for a subtree with no vector form at its root: PostgreSQL's
 * evaluator, a row at a time, ahead of PostgreSQL's order, so only for a
 * pure one.
 */
static VexecExpr *
compile_fallback(VexecCompileContext *cc, Expr *expr)
{
	const char *why = vexec_expr_impurity((Node *) expr, false);
	VexecExpr  *e;
	MemoryContext old;

	if (why != NULL)
	{
		if (cc->why == NULL)
			cc->why = why;
		return NULL;
	}
	e = new_expr(cc, VE_FALLBACK, expr, 0);
	old = MemoryContextSwitchTo(cc->mcxt);
	e->state = ExecInitExpr(expr, cc->parent);
	MemoryContextSwitchTo(old);
	cc->nfallbacks++;
	return e;
}

/* The vector forms; NULL for an expression that has none. */
static VexecExpr *
compile_vector(VexecCompileContext *cc, Expr *expr)
{
	VexecExpr  *e;

	switch (nodeTag(expr))
	{
		case T_Var:
			return compile_var(cc, (Var *) expr);
		case T_Const:
			e = new_expr(cc, VE_CONST, expr, 0);
			e->constvec = const_vec(cc, (Const *) expr, e->type);
			return e;
		case T_Param:
			{
				Param	   *p = (Param *) expr;
				MemoryContext old;

				if (p->paramkind != PARAM_EXTERN && p->paramkind != PARAM_EXEC)
					return NULL;
				e = new_expr(cc, VE_PARAM, expr, 0);
				old = MemoryContextSwitchTo(cc->mcxt);
				e->param_state = ExecInitExpr(expr, cc->parent);
				MemoryContextSwitchTo(old);
				if (p->paramkind == PARAM_EXEC)
					e->paramid = p->paramid;
				return e;
			}
		case T_RelabelType:
		case T_CollateExpr:
			{
				Expr	   *arg = IsA(expr, RelabelType) ? ((RelabelType *) expr)->arg
					: ((CollateExpr *) expr)->arg;
				VexecExpr  *a = vexec_compile_expr(cc, arg);

				if (a == NULL)
					return NULL;
				e = new_expr(cc, VE_RELABEL, expr, 1);
				e->args[0] = a;
				return e;
			}
		case T_BoolExpr:
			{
				BoolExpr   *b = (BoolExpr *) expr;
				ListCell   *lc;
				int			i = 0;

				e = new_expr(cc, b->boolop == AND_EXPR ? VE_AND :
							 b->boolop == OR_EXPR ? VE_OR : VE_NOT,
							 expr, list_length(b->args));
				foreach(lc, b->args)
				{
					e->args[i] = vexec_compile_expr(cc, lfirst(lc));
					if (e->args[i] == NULL)
						return NULL;
					i++;
				}
				return e;
			}
		case T_NullTest:
			{
				NullTest   *nt = (NullTest *) expr;

				/* a row's NULL test looks into its fields: the fallback */
				if (nt->argisrow)
					return NULL;
				e = new_expr(cc, VE_NULLTEST, expr, 1);
				e->testtype = nt->nulltesttype;
				e->args[0] = vexec_compile_expr(cc, nt->arg);
				return e->args[0] ? e : NULL;
			}
		case T_BooleanTest:
			{
				BooleanTest *bt = (BooleanTest *) expr;

				e = new_expr(cc, VE_BOOLTEST, expr, 1);
				e->testtype = bt->booltesttype;
				e->args[0] = vexec_compile_expr(cc, bt->arg);
				return e->args[0] ? e : NULL;
			}
		case T_FuncExpr:
			{
				FuncExpr   *f = (FuncExpr *) expr;

				if (f->funcretset)
					return NULL;
				return compile_call(cc, expr, VE_CALL, f->funcid, f->args, f->inputcollid);
			}
		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) expr;

				set_opfuncid(op);
				if (op->opretset)
					return NULL;
				return compile_call(cc, expr, VE_CALL, op->opfuncid, op->args, op->inputcollid);
			}
		case T_DistinctExpr:
			{
				DistinctExpr *d = (DistinctExpr *) expr;

				set_opfuncid((OpExpr *) d);
				return compile_call(cc, expr, VE_DISTINCT, d->opfuncid, d->args, d->inputcollid);
			}
		case T_ScalarArrayOpExpr:
			{
				ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) expr;

				set_sa_opfuncid(saop);
				return compile_call(cc, expr, VE_SAOP, saop->opfuncid, saop->args,
									saop->inputcollid);
			}
		default:
			return NULL;
	}
}

/*
 * Compile an expression: its vector form where it has one, else a fallback
 * step for the whole subtree where that is pure.  NULL when neither: the
 * qual or target it is part of is lazy (cc->why says why).
 */
VexecExpr *
vexec_compile_expr(VexecCompileContext *cc, Expr *expr)
{
	VexecExpr  *e;
	int			nkernels = cc->nkernels;
	int			nfallbacks = cc->nfallbacks;

	e = compile_vector(cc, expr);
	if (e != NULL)
		return e;
	/* what a failed vector form counted is not in the program */
	cc->nkernels = nkernels;
	cc->nfallbacks = nfallbacks;
	return compile_fallback(cc, expr);
}

/*
 * A top-level qual or target entry: eager when it compiles, lazy otherwise.
 * Its PostgreSQL state is made in both cases: the lazy one runs on it, and
 * the row path uses it for a qual.
 */
void
vexec_compile_top(VexecCompileContext *cc, Expr *expr, bool is_qual, VexecTop *top)
{
	MemoryContext old = MemoryContextSwitchTo(cc->mcxt);
	int			nkernels = cc->nkernels;
	int			nfallbacks = cc->nfallbacks;

	memset(top, 0, sizeof(VexecTop));
	top->expr = expr;
	top->type = vexec_type_make(exprType((Node *) expr), exprTypmod((Node *) expr),
								exprCollation((Node *) expr));
	if (is_qual)
		top->state = ExecInitQual(list_make1(expr), cc->parent);
	else
		top->state = ExecInitExpr(expr, cc->parent);
	MemoryContextSwitchTo(old);

	cc->why = NULL;
	top->eager = vexec_compile_expr(cc, expr);
	if (top->eager == NULL)
	{
		top->why_lazy = cc->why ? cc->why : "no vector form";
		cc->nkernels = nkernels;
		cc->nfallbacks = nfallbacks;
	}
}
