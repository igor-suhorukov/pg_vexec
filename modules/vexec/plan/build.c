/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * build.c
 *	  The node builders (pg_vector_executor.md §3.3.2): one function per
 *	  node kind builds the CustomScan in one canonical form, from a chosen
 *	  path in PostgreSQL's planner, or from the row node ORCA's translator
 *	  has just built (plan/orca.c).
 *
 * VecScan's canonical form is a SeqScan's: scanrelid set, its quals and
 * target list reading Vars of scanrelid, custom_scan_tlist empty, so that
 * the node's scan tuple is the table's row.  In PostgreSQL's planner,
 * set_plan_references() then fixes its expressions as it fixes a scan's
 * (PG19:src/backend/optimizer/plan/setrefs.c, set_customscan_references);
 * ORCA's translator builds them in executor form already.  The storage
 * modules' O15 column walkers read such a node's Vars as they read a
 * SeqScan's (pg19/modules/gp_ao/ao_am.c:894-912).  The columns the node
 * reads are found from its Vars when it begins, since the planner may give
 * a projection-capable path a new target list after its plan is built
 * (createplan.c, create_projection_plan).
 *
 * VecResult's is a Result's: no scanrelid, its child in lefttree, its quals
 * and target list reading the child's output as OUTER_VAR.  Children stay in
 * lefttree and righttree, never custom_plans (§3.6).
 *
 * The builders set the flags; core copies costs, not flags
 * ([R:route_b pg19_customscan §2.1]).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "optimizer/pathnode.h"
#include "optimizer/restrictinfo.h"

#include "vexec.h"
#include "exec/exec.h"
#include "plan/plan.h"

static Plan *plan_vecscan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
						  List *tlist, List *clauses, List *custom_plans);

static Plan *plan_vecbitmapscan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
								List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods vecscan_path_methods = {
	.CustomName = VEXEC_SCAN_NAME,
	.PlanCustomPath = plan_vecscan,
};

static const CustomPathMethods vecbitmapscan_path_methods = {
	.CustomName = VEXEC_BITMAPSCAN_NAME,
	.PlanCustomPath = plan_vecbitmapscan,
};

/*
 * A VecScan path for a relation, priced by the cost model, parameterized as
 * the row path it stands beside is: by the relation's lateral references.
 * Beside a partial row path -- a parallel sequential scan -- it is a
 * partial path, parallel-aware and of as many workers (V4).
 */
Path *
vexec_scan_path(PlannerInfo *root, RelOptInfo *rel, Path *rowpath, const VexecCost *cost)
{
	CustomPath *cp = makeNode(CustomPath);

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = rowpath->param_info;
	cp->path.parallel_aware = rowpath->parallel_aware;
	cp->path.parallel_safe = rel->consider_parallel;
	cp->path.parallel_workers = rowpath->parallel_workers;
	cp->path.rows = rowpath->rows;
	cp->path.disabled_nodes = 0;
	cp->path.startup_cost = cost->startup;
	cp->path.total_cost = cost->total;
	cp->path.pathkeys = NIL;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = NIL;
	cp->custom_private = NIL;
	cp->methods = &vecscan_path_methods;
	return &cp->path;
}

/* VecScan's plan: a SeqScan's quals and target list on a CustomScan. */
static Plan *
plan_vecscan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	(void) root;
	(void) custom_plans;
	cscan->scan.plan.targetlist = tlist;
	/* the clauses as a SeqScan's: RestrictInfos stripped, pseudoconstants gated above */
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_private = NIL;
	cscan->custom_scan_tlist = NIL;
	cscan->methods = vexec_scan_methods();
	return &cscan->scan.plan;
}

/*
 * A VecBitmapHeapScan path beside a bitmap heap path of the relation (H9):
 * the same bitmap, the same rows, parameterized as it is.  Its one custom
 * path is a copy of the bitmap heap path -- add_path() frees a bitmap heap
 * path it drops, though not the index paths of its bitmap -- which core
 * plans as it plans a BitmapHeapScan, for the builder to take its bitmap
 * and its conditions from.  Its cost is the bitmap heap path's, its rows'
 * work at the vector factors (cost.c).
 */
Path *
vexec_bitmapscan_path(PlannerInfo *root, RelOptInfo *rel, BitmapHeapPath *bp,
					  const VexecSteps *quals, const VexecSteps *target)
{
	CustomPath *cp = makeNode(CustomPath);
	BitmapHeapPath *copy = makeNode(BitmapHeapPath);
	VexecCost	cost;

	memcpy(copy, bp, sizeof(BitmapHeapPath));
	vexec_cost_bitmapscan(root, rel, &bp->path, quals, target, &cost);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = rel->reltarget;
	cp->path.param_info = bp->path.param_info;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = bp->path.parallel_safe;
	cp->path.parallel_workers = 0;
	cp->path.rows = bp->path.rows;
	cp->path.disabled_nodes = bp->path.disabled_nodes;
	cp->path.startup_cost = cost.startup;
	cp->path.total_cost = cost.total;
	cp->path.pathkeys = NIL;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = list_make1(copy);
	cp->custom_private = NIL;
	cp->methods = &vecbitmapscan_path_methods;
	return &cp->path;
}

/*
 * VecBitmapHeapScan's plan, from the BitmapHeapScan core made of its custom
 * path: the bitmap's tree, its conditions to recheck (custom_exprs) and its
 * filter, as a scan of the relation; the BitmapHeapScan set aside.
 */
static Plan *
plan_vecbitmapscan(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
				   List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	BitmapHeapScan *bhs = list_length(custom_plans) == 1 ? linitial(custom_plans) : NULL;

	(void) root;
	(void) clauses;

	/*
	 * Pseudoconstant clauses: core gates the BitmapHeapScan with a Result
	 * of them, as it gates this node above (create_scan_plan()), and the
	 * inner gate is set aside with the BitmapHeapScan.
	 */
	if (bhs != NULL && IsA(bhs, Result) && ((Result *) bhs)->resconstantqual != NULL &&
		((Result *) bhs)->plan.qual == NIL && outerPlan(bhs) != NULL &&
		IsA(outerPlan(bhs), BitmapHeapScan))
		bhs = (BitmapHeapScan *) outerPlan(bhs);
	if (bhs == NULL || !IsA(bhs, BitmapHeapScan))
		elog(ERROR, "vexec: a VecBitmapHeapScan's path planned without its BitmapHeapScan");
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = bhs->scan.plan.qual;
	cscan->scan.plan.lefttree = bhs->scan.plan.lefttree;
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = bhs->bitmapqualorig;
	cscan->custom_private = NIL;
	cscan->custom_scan_tlist = NIL;
	cscan->methods = vexec_bitmapscan_methods();
	return &cscan->scan.plan;
}

/*
 * VecBitmapHeapScan in place of a BitmapHeapScan ORCA's translator built:
 * the same bitmap and conditions, its plan node id, costs and parameters
 * kept.
 */
Plan *
vexec_build_bitmapscan_from_bitmapscan(BitmapHeapScan *bhs)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan = bhs->scan;
	cscan->scan.plan.type = T_CustomScan;
	cscan->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cscan->custom_exprs = bhs->bitmapqualorig;
	cscan->custom_relids = bms_make_singleton(bhs->scan.scanrelid);
	cscan->methods = vexec_bitmapscan_methods();
	return &cscan->scan.plan;
}

/*
 * VecScan in place of a SeqScan ORCA's translator built: the same scan,
 * its plan node id, costs and parameters kept.
 */
Plan *
vexec_build_scan_from_seqscan(SeqScan *seqscan)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan = seqscan->scan;
	cscan->scan.plan.type = T_CustomScan;
	cscan->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cscan->methods = vexec_scan_methods();
	/*
	 * The relation it scans, as PostgreSQL's planner gives a custom path's
	 * (create_customscan_plan): EXPLAIN names a CustomScan's relations from
	 * these, and gives each the name it gives the SeqScan's -- t1_2 for a
	 * partition's RTE among several aliased t1.
	 */
	cscan->custom_relids = bms_make_singleton(seqscan->scan.scanrelid);
	return &cscan->scan.plan;
}

/*
 * VecResult in place of a Result ORCA's translator built: the same
 * projection and filter over the same child.
 */
Plan *
vexec_build_result_from_result(Result *result)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan = result->plan;
	cscan->scan.plan.type = T_CustomScan;
	cscan->scan.scanrelid = 0;
	cscan->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cscan->methods = vexec_result_methods();
	return &cscan->scan.plan;
}
