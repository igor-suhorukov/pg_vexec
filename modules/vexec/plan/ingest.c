/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * ingest.c
 *	  VecIngest in PostgreSQL's planner (pg_vector_executor.md §3.16): the
 *	  function scan of vexec.ingest_stream(handle) read as a vector source.
 *
 * set_rel_pathlist_hook runs for a function's relation too
 * (PG19:src/backend/optimizer/path/allpaths.c:583-611).  For a relation
 * whose one function is vexec.ingest_stream(), its handle a constant or a
 * parameter, a CustomPath beside PostgreSQL's FunctionScan -- which runs
 * the function to its end into a tuplestore before it returns a row --
 * reads the stream's windows as batches (exec/vecingest.c), its quals and
 * target list evaluated over them.  In force mode it takes the scan's
 * place.
 *
 * Its plan has no relation (scanrelid 0), since core opens the relation of
 * a scanrelid it is given (PG19:src/backend/executor/nodeCustom.c:58-60):
 * custom_scan_tlist describes its scan tuple, the function's columns as
 * Vars of its relation, which set_customscan_references() turns the
 * target list's and the quals' into; custom_exprs holds the handle.
 *
 * In ORCA's plans VecIngest takes the place of the FunctionScan its
 * translator built (vexec_build_ingest_from_functionscan(), from orca.c):
 * the same scan tuple, the FunctionScan's target list and quals with its
 * relation's Vars made INDEX_VAR, as set_customscan_references() makes
 * them.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "vexec.h"
#include "exec/exec.h"
#include "plan/plan.h"

static Plan *plan_vecingest(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
							List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods vecingest_path_methods = {
	.CustomName = VEXEC_INGEST_NAME,
	.PlanCustomPath = plan_vecingest,
};

/* vexec.ingest_stream(bigint)'s OID in this database, or InvalidOid. */
static Oid
ingest_function(void)
{
	Oid			nsp = get_namespace_oid("vexec", true);
	Oid			argtypes[1] = {INT8OID};

	if (!OidIsValid(nsp))
		return InvalidOid;
	return GetSysCacheOid3(PROCNAMEARGSNSP, Anum_pg_proc_oid,
						   CStringGetDatum("ingest_stream"),
						   PointerGetDatum(buildoidvector(argtypes, 1)),
						   ObjectIdGetDatum(nsp));
}

/* The relation's function call, if it is vexec.ingest_stream(), or NULL. */
static FuncExpr *
ingest_call(RangeTblEntry *rte)
{
	RangeTblFunction *rtfunc;
	FuncExpr   *fe;

	if (rte->rtekind != RTE_FUNCTION || list_length(rte->functions) != 1)
		return NULL;
	rtfunc = linitial(rte->functions);
	if (!IsA(rtfunc->funcexpr, FuncExpr))
		return NULL;
	fe = (FuncExpr *) rtfunc->funcexpr;
	if (list_length(fe->args) != 1 || fe->funcid != ingest_function())
		return NULL;
	return fe;
}

void
vexec_consider_ingest(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
					  VexecPlanState *ps)
{
	bool		add = ps->mode == VEXEC_MODE_AUTO || ps->mode == VEXEC_MODE_FORCE;
	FuncExpr   *fe = ingest_call(rte);
	VexecAlt   *alt = NULL;
	const char *refusal = NULL;
	Node	   *arg;
	VexecSteps	quals;
	VexecSteps	target;
	Path	   *rowpath = NULL;
	CustomPath *cp;
	VexecCost	cost;
	ListCell   *lc;

	if (fe == NULL || rel->reloptkind != RELOPT_BASEREL || IS_DUMMY_REL(rel))
		return;
	if (ps->record)
		alt = vexec_alt_record(ps, "VecIngest", "vexec.ingest_stream()", root, NULL);
	if (alt == NULL && !add)
		return;

	arg = linitial(fe->args);
	if (rte->funcordinality)
		refusal = "WITH ORDINALITY";
	else if (!IsA(arg, Const) && !IsA(arg, Param))
		refusal = "a handle that is not a constant or a parameter";
	else if (rel->lateral_relids != NULL)
		refusal = "lateral references";
	else if (!vexec_enable_scan)
		refusal = "vexec.enable_scan is off";
	if (refusal == NULL)
	{
		memset(&quals, 0, sizeof(quals));
		memset(&target, 0, sizeof(target));
		vexec_oracle_exprs(root, rel->baserestrictinfo, &quals);
		vexec_oracle_exprs(root, rel->reltarget->exprs, &target);
		refusal = quals.refusal ? quals.refusal : target.refusal;
	}
	foreach(lc, rel->pathlist)
		if (((Path *) lfirst(lc))->pathtype == T_FunctionScan &&
			((Path *) lfirst(lc))->param_info == NULL)
			rowpath = lfirst(lc);
	if (refusal == NULL && rowpath == NULL)
		refusal = "no function scan of the relation to stand beside";
	if (refusal != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return;
	}

	/*
	 * The function scan's price, but its rows a batch at a time, and no
	 * tuplestore: the work of a row at vexec.cpu_tuple_factor.
	 */
	memset(&cost, 0, sizeof(cost));
	cost.rows = rel->rows;
	cost.row_startup = rowpath->startup_cost;
	cost.row_total = rowpath->total_cost;
	cost.startup = vexec_batch_setup_cost;
	cost.total = vexec_batch_setup_cost + rel->rows * cpu_tuple_cost * vexec_cpu_tuple_factor +
		rel->rows * (quals.kernel_cost + target.kernel_cost) * vexec_cpu_operator_factor +
		rel->rows * (quals.fallback_cost + target.fallback_cost);
	vexec_alt_costed(ps, alt, &cost, "source: the client's stream");
	if (!add)
		return;

	cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = rel->rows;
	cp->path.disabled_nodes = 0;
	cp->path.startup_cost = cost.startup;
	cp->path.total_cost = cost.total;
	cp->path.pathkeys = NIL;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = NIL;
	cp->custom_private = NIL;
	cp->methods = &vecingest_path_methods;
	if (ps->mode == VEXEC_MODE_FORCE)
		rel->pathlist = NIL;
	add_path(rel, &cp->path);
	ps->npossible++;
}

/*
 * VecIngest's plan: the function's columns as its scan tuple, the handle in
 * custom_exprs, the target list and quals over the function's relation.
 */
static Plan *
plan_vecingest(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			   List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);
	RangeTblFunction *rtfunc = linitial(rte->functions);
	FuncExpr   *fe = (FuncExpr *) rtfunc->funcexpr;
	List	   *scan_tlist = NIL;
	ListCell   *ln,
			   *lt,
			   *lm,
			   *lc;
	AttrNumber	attno = 0;

	(void) custom_plans;
	forfour(ln, rtfunc->funccolnames, lt, rtfunc->funccoltypes,
			lm, rtfunc->funccoltypmods, lc, rtfunc->funccolcollations)
	{
		attno++;
		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) makeVar(rel->relid, attno, lfirst_oid(lt),
															  lfirst_int(lm), lfirst_oid(lc), 0),
											 attno, pstrdup(strVal(lfirst(ln))), false));
	}
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = list_make1(copyObject(linitial(fe->args)));
	cscan->custom_private = NIL;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->methods = vexec_ingest_methods();
	return &cscan->scan.plan;
}

/* A FunctionScan's Vars of its relation, as INDEX_VAR's of its scan tuple. */
typedef struct IndexVarContext
{
	Index		scanrelid;
	bool		whole_row;
} IndexVarContext;

static Node *
index_var_mutator(Node *node, IndexVarContext *ctx)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == (int) ctx->scanrelid &&
		((Var *) node)->varlevelsup == 0)
	{
		Var		   *var = copyObject((Var *) node);

		if (var->varattno <= 0)
			ctx->whole_row = true;
		var->varno = INDEX_VAR;
		return (Node *) var;
	}
	return expression_tree_mutator(node, index_var_mutator, ctx);
}

/*
 * VecIngest in place of a FunctionScan ORCA's translator built (orca.c), in
 * executor form.  NULL, with *refusal where the FunctionScan is
 * vexec.ingest_stream()'s and must stay; NULL alone for any other function.
 */
Plan *
vexec_build_ingest_from_functionscan(FunctionScan *fs, List *rtable, const char **refusal)
{
	Index		scanrelid = fs->scan.scanrelid;
	RangeTblFunction *rtfunc;
	RangeTblEntry *rte;
	FuncExpr   *fe;
	Node	   *arg;
	CustomScan *cscan;
	List	   *scan_tlist = NIL;
	IndexVarContext ctx;
	ListCell   *ln,
			   *lt,
			   *lm,
			   *lc;
	AttrNumber	attno = 0;
	VexecSteps	steps;

	*refusal = NULL;
	if (list_length(fs->functions) != 1 || scanrelid < 1 || scanrelid > list_length(rtable))
		return NULL;
	rtfunc = linitial(fs->functions);
	if (!IsA(rtfunc->funcexpr, FuncExpr))
		return NULL;
	fe = (FuncExpr *) rtfunc->funcexpr;
	if (list_length(fe->args) != 1 || fe->funcid != ingest_function())
		return NULL;

	/* the column definition list, where the plan's own copy lacks it */
	rte = rt_fetch(scanrelid, rtable);
	if (rtfunc->funccolnames == NIL && rte->rtekind == RTE_FUNCTION &&
		list_length(rte->functions) == 1)
		rtfunc = linitial(rte->functions);
	arg = linitial(fe->args);
	if (fs->funcordinality)
		*refusal = "WITH ORDINALITY";
	else if (!IsA(arg, Const) && !IsA(arg, Param))
		*refusal = "a handle that is not a constant or a parameter";
	else if (!vexec_enable_scan)
		*refusal = "vexec.enable_scan is off";
	else if (rtfunc->funccolnames == NIL)
		*refusal = "no column definition list";
	if (*refusal == NULL)
	{
		memset(&steps, 0, sizeof(steps));
		vexec_oracle_exprs(NULL, fs->scan.plan.qual, &steps);
		vexec_oracle_exprs(NULL, fs->scan.plan.targetlist, &steps);
		*refusal = steps.refusal;
	}
	if (*refusal != NULL)
		return NULL;

	forfour(ln, rtfunc->funccolnames, lt, rtfunc->funccoltypes,
			lm, rtfunc->funccoltypmods, lc, rtfunc->funccolcollations)
	{
		attno++;
		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) makeVar(scanrelid, attno, lfirst_oid(lt),
															  lfirst_int(lm), lfirst_oid(lc), 0),
											 attno, pstrdup(strVal(lfirst(ln))), false));
	}

	cscan = makeNode(CustomScan);
	cscan->scan.plan = fs->scan.plan;
	cscan->scan.plan.type = T_CustomScan;
	ctx.scanrelid = scanrelid;
	ctx.whole_row = false;
	cscan->scan.plan.targetlist = (List *)
		index_var_mutator((Node *) fs->scan.plan.targetlist, &ctx);
	cscan->scan.plan.qual = (List *) index_var_mutator((Node *) fs->scan.plan.qual, &ctx);
	if (ctx.whole_row)
	{
		*refusal = "a whole-row reference";
		return NULL;
	}
	cscan->scan.scanrelid = 0;
	cscan->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = list_make1(copyObject(arg));
	cscan->custom_private = NIL;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_relids = bms_make_singleton(scanrelid);
	cscan->methods = vexec_ingest_methods();
	return &cscan->scan.plan;
}
