/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * check.c
 *	  The plan check (pg_vector_executor.md §3.3.2).
 *
 * Under vexec.debug_check_plans -- on where the server checks its
 * assertions -- every plan PostgreSQL's planner finishes is walked, in
 * planner_shutdown_hook: its tree, every subplan, and every node's
 * children, wherever the node keeps them.
 *
 *	- Plan node ids are unique over the tree and every subplan, which
 *	  EXPLAIN ANALYZE on a cluster and parallel query's DSM keys rely on.
 *	- A plan made in off or explain mode has no vector node.
 *	- Every vector node is in its canonical form (plan/build.c).
 *
 * It returns how many vector nodes the plan has, for
 * vexec.debug_require_vector.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/bitmapset.h"
#include "nodes/plannodes.h"

#include "vexec.h"
#include "exec/exec.h"
#include "plan/plan.h"

typedef struct CheckContext
{
	Bitmapset  *ids;
	List	   *nodes;			/* the nodes seen, to name an id's two */
	int			vector_nodes;
} CheckContext;

/* A plan node's name in the check's errors: its tag, or its custom name. */
static const char *
node_name(Plan *plan)
{
	if (IsA(plan, CustomScan))
		return ((CustomScan *) plan)->methods->CustomName;
	return psprintf("node %d", (int) nodeTag(plan));
}

/*
 * Whether a CustomScan is one of vexec's vector nodes, and that it is in
 * its canonical form (build.c, agg.c): a VecScan scans a relation with no
 * custom_scan_tlist, a VecBitmapHeapScan too with its bitmap's tree in
 * lefttree; a VecResult has its child in lefttree and no
 * relation; a VecAgg too, with a scan tuple of its plan's columns; a
 * VecHashJoin its sides in lefttree and righttree, its scan tuple their
 * columns, and its keys its operators'; a VecSort its child in lefttree,
 * its scan tuple the child's row, and its keys the child's columns; a
 * VecWindowHashAgg too, with no qual; a VecInsert its child in lefttree, its
 * scan tuple the child's row, and its target in custom_private; a VecIngest
 * no relation and no child, its scan tuple the stream's columns, and the
 * stream's handle in custom_exprs.
 */
static bool
is_vector_node(CustomScan *cscan)
{
	if (!vexec_is_vector_node((Plan *) cscan))
		return false;
	if (cscan->methods == vexec_scan_methods())
	{
		if (cscan->scan.scanrelid == 0 || cscan->custom_scan_tlist != NIL ||
			cscan->scan.plan.lefttree != NULL)
			elog(ERROR, "vexec plan check: a VecScan not in its canonical form");
	}
	else if (cscan->methods == vexec_bitmapscan_methods())
	{
		Plan	   *bitmap = cscan->scan.plan.lefttree;

		if (cscan->scan.scanrelid == 0 || cscan->custom_scan_tlist != NIL || bitmap == NULL ||
			cscan->scan.plan.righttree != NULL ||
			(!IsA(bitmap, BitmapIndexScan) && !IsA(bitmap, BitmapAnd) && !IsA(bitmap, BitmapOr)))
			elog(ERROR, "vexec plan check: a VecBitmapHeapScan not in its canonical form");
	}
	else if (cscan->methods == vexec_agg_methods())
	{
		VexecAggPlan plan;

		if (cscan->scan.scanrelid != 0 || cscan->scan.plan.lefttree == NULL ||
			cscan->custom_plans != NIL)
			elog(ERROR, "vexec plan check: a VecAgg not in its canonical form");
		vexec_agg_plan_decode(cscan, &plan);
		if (list_length(plan.outkind) != list_length(cscan->custom_scan_tlist) ||
			list_length(plan.keycols) != list_length(plan.eqops))
			elog(ERROR, "vexec plan check: a VecAgg whose scan tuple is not its plan's");
	}
	else if (cscan->methods == vexec_hashjoin_methods())
	{
		VexecJoinPlan plan;
		Plan	   *outer = cscan->scan.plan.lefttree;
		Plan	   *inner = cscan->scan.plan.righttree;

		if (cscan->scan.scanrelid != 0 || outer == NULL || inner == NULL ||
			cscan->custom_plans != NIL || list_length(cscan->custom_exprs) != 4)
			elog(ERROR, "vexec plan check: a VecHashJoin not in its canonical form");
		vexec_join_plan_decode(cscan, &plan);
		if (plan.nouter != list_length(outer->targetlist) ||
			plan.ninner != list_length(inner->targetlist) ||
			list_length(cscan->custom_scan_tlist) != plan.nouter + plan.ninner)
			elog(ERROR, "vexec plan check: a VecHashJoin whose scan tuple is not its sides' columns");
		if (list_length(plan.hashoperators) == 0 ||
			list_length(plan.hashoperators) !=
			list_length(list_nth(cscan->custom_exprs, VEXEC_JOIN_OUTERKEYS)) ||
			list_length(plan.hashoperators) !=
			list_length(list_nth(cscan->custom_exprs, VEXEC_JOIN_INNERKEYS)))
			elog(ERROR, "vexec plan check: a VecHashJoin whose keys are not its operators'");
	}
	else if (cscan->methods == vexec_sort_methods())
	{
		VexecSortPlan plan;
		Plan	   *child = cscan->scan.plan.lefttree;
		ListCell   *lc;

		if (cscan->scan.scanrelid != 0 || child == NULL || cscan->custom_plans != NIL ||
			cscan->scan.plan.righttree != NULL)
			elog(ERROR, "vexec plan check: a VecSort not in its canonical form");
		vexec_sort_plan_decode(cscan, &plan);
		if (plan.late_tidcol > 0)
		{
			/* late columns: the child, a VecScan, gives the keys and the TID */
			if (!vexec_is_vector_node(child) || ((Scan *) child)->scanrelid == 0 ||
				plan.late_tidcol != list_length(child->targetlist))
				elog(ERROR, "vexec plan check: a VecSort of late columns not over its scan's keys and TIDs");
		}
		else if (list_length(cscan->custom_scan_tlist) != list_length(child->targetlist))
			elog(ERROR, "vexec plan check: a VecSort whose scan tuple is not its child's row");
		foreach(lc, plan.keycols)
			if (lfirst_int(lc) < 1 || lfirst_int(lc) > list_length(child->targetlist))
				elog(ERROR, "vexec plan check: a VecSort's key is not a column of its child");
	}
	else if (cscan->methods == vexec_window_methods())
	{
		VexecWindowPlan plan;
		Plan	   *child = cscan->scan.plan.lefttree;
		ListCell   *lc;

		if (cscan->scan.scanrelid != 0 || child == NULL || cscan->custom_plans != NIL ||
			cscan->scan.plan.righttree != NULL || cscan->scan.plan.qual != NIL)
			elog(ERROR, "vexec plan check: a VecWindowHashAgg not in its canonical form");
		vexec_window_plan_decode(cscan, &plan);
		if (list_length(cscan->custom_scan_tlist) != list_length(child->targetlist))
			elog(ERROR, "vexec plan check: a VecWindowHashAgg whose scan tuple is not its child's row");
		foreach(lc, list_concat_copy(list_concat_copy(plan.partcols, plan.ordcols), plan.sortcols))
			if (lfirst_int(lc) < 1 || lfirst_int(lc) > list_length(child->targetlist))
				elog(ERROR, "vexec plan check: a VecWindowHashAgg's key is not a column of its child");
	}
	else if (cscan->methods == vexec_repart_methods())
	{
		VexecRepartPlan plan;
		Plan	   *child = cscan->scan.plan.lefttree;

		if (cscan->scan.scanrelid != 0 || child == NULL || cscan->custom_plans != NIL ||
			!cscan->scan.plan.parallel_aware)
			elog(ERROR, "vexec plan check: a VecRepartition not in its canonical form");
		vexec_repart_plan_decode(cscan, &plan);
		if (list_length(cscan->custom_scan_tlist) != list_length(child->targetlist))
			elog(ERROR, "vexec plan check: a VecRepartition whose scan tuple is not its child's row");
	}
	else if (cscan->methods == vexec_insert_methods())
	{
		Plan	   *child = cscan->scan.plan.lefttree;

		if (cscan->scan.scanrelid != 0 || child == NULL || cscan->custom_plans != NIL ||
			cscan->scan.plan.qual != NIL || list_length(cscan->custom_private) != 2 ||
			list_length(cscan->custom_scan_tlist) != list_length(child->targetlist))
			elog(ERROR, "vexec plan check: a VecInsert not in its canonical form");
	}
	else if (cscan->methods == vexec_ingest_methods())
	{
		if (cscan->scan.scanrelid != 0 || cscan->scan.plan.lefttree != NULL ||
			cscan->custom_plans != NIL || cscan->custom_scan_tlist == NIL ||
			list_length(cscan->custom_exprs) != 1)
			elog(ERROR, "vexec plan check: a VecIngest not in its canonical form");
	}
	else if (cscan->scan.scanrelid != 0 || cscan->scan.plan.lefttree == NULL)
		elog(ERROR, "vexec plan check: a VecResult not in its canonical form");
	return true;
}

static void
check_plan_node(Plan *plan, CheckContext *ctx)
{
	ListCell   *lc;

	if (plan == NULL)
		return;
	check_stack_depth();

	if (bms_is_member(plan->plan_node_id, ctx->ids))
	{
		ListCell   *ln;
		Plan	   *other = NULL;

		foreach(ln, ctx->nodes)
			if (((Plan *) lfirst(ln))->plan_node_id == plan->plan_node_id)
				other = lfirst(ln);
		elog(ERROR, "vexec plan check: plan node id %d is used twice, by a %s and a %s",
			 plan->plan_node_id, other ? node_name(other) : "?", node_name(plan));
	}
	ctx->ids = bms_add_member(ctx->ids, plan->plan_node_id);
	ctx->nodes = lappend(ctx->nodes, plan);

	switch (nodeTag(plan))
	{
		case T_CustomScan:
			{
				CustomScan *cscan = (CustomScan *) plan;

				if (is_vector_node(cscan))
					ctx->vector_nodes++;
				foreach(lc, cscan->custom_plans)
					check_plan_node(lfirst(lc), ctx);
				break;
			}
		case T_Append:
			foreach(lc, ((Append *) plan)->appendplans)
				check_plan_node(lfirst(lc), ctx);
			break;
		case T_MergeAppend:
			foreach(lc, ((MergeAppend *) plan)->mergeplans)
				check_plan_node(lfirst(lc), ctx);
			break;
		case T_BitmapAnd:
			foreach(lc, ((BitmapAnd *) plan)->bitmapplans)
				check_plan_node(lfirst(lc), ctx);
			break;
		case T_BitmapOr:
			foreach(lc, ((BitmapOr *) plan)->bitmapplans)
				check_plan_node(lfirst(lc), ctx);
			break;
		case T_SubqueryScan:
			check_plan_node(((SubqueryScan *) plan)->subplan, ctx);
			break;
		default:
			break;
	}
	check_plan_node(plan->lefttree, ctx);
	check_plan_node(plan->righttree, ctx);
}

/* The largest plan_node_id below plan, the walk check_plan_node() makes. */
static int
max_plan_node_id(Plan *plan)
{
	ListCell   *lc;
	List	   *children = NIL;
	int			max;

	if (plan == NULL)
		return -1;
	check_stack_depth();
	max = plan->plan_node_id;
	switch (nodeTag(plan))
	{
		case T_CustomScan:
			children = ((CustomScan *) plan)->custom_plans;
			break;
		case T_Append:
			children = ((Append *) plan)->appendplans;
			break;
		case T_MergeAppend:
			children = ((MergeAppend *) plan)->mergeplans;
			break;
		case T_BitmapAnd:
			children = ((BitmapAnd *) plan)->bitmapplans;
			break;
		case T_BitmapOr:
			children = ((BitmapOr *) plan)->bitmapplans;
			break;
		case T_SubqueryScan:
			max = Max(max, max_plan_node_id(((SubqueryScan *) plan)->subplan));
			break;
		default:
			break;
	}
	foreach(lc, children)
		max = Max(max, max_plan_node_id(lfirst(lc)));
	max = Max(max, max_plan_node_id(plan->lefttree));
	return Max(max, max_plan_node_id(plan->righttree));
}

/* The largest plan_node_id of a statement's plan, its subplans' included. */
int
vexec_max_plan_node_id(PlannedStmt *pstmt)
{
	int			max = max_plan_node_id(pstmt->planTree);
	ListCell   *lc;

	foreach(lc, pstmt->subplans)
		max = Max(max, max_plan_node_id(lfirst(lc)));
	return max;
}

/*
 * Each VecInsert that is a ModifyTable's input and has its id -- the copy
 * of the ModifyTable's plan it was made from, on a cluster's coordinator
 * (plan/insert.c) -- given an id past *next, found wherever the finished
 * plan has it: the port's dispatcher may copy a writing fragment after
 * its nodes were built.
 */
static void
number_insert_inputs(Plan *plan, int *next)
{
	ListCell   *lc;
	List	   *children = NIL;

	if (plan == NULL)
		return;
	check_stack_depth();
	if (IsA(plan, ModifyTable) && plan->lefttree != NULL &&
		IsA(plan->lefttree, CustomScan) &&
		((CustomScan *) plan->lefttree)->methods == vexec_insert_methods() &&
		plan->lefttree->plan_node_id == plan->plan_node_id)
		plan->lefttree->plan_node_id = ++(*next);
	switch (nodeTag(plan))
	{
		case T_CustomScan:
			children = ((CustomScan *) plan)->custom_plans;
			break;
		case T_Append:
			children = ((Append *) plan)->appendplans;
			break;
		case T_MergeAppend:
			children = ((MergeAppend *) plan)->mergeplans;
			break;
		case T_SubqueryScan:
			number_insert_inputs(((SubqueryScan *) plan)->subplan, next);
			break;
		default:
			break;
	}
	foreach(lc, children)
		number_insert_inputs(lfirst(lc), next);
	number_insert_inputs(plan->lefttree, next);
	number_insert_inputs(plan->righttree, next);
}

void
vexec_number_insert_inputs(PlannedStmt *pstmt)
{
	int			next = vexec_max_plan_node_id(pstmt);
	ListCell   *lc;

	number_insert_inputs(pstmt->planTree, &next);
	foreach(lc, pstmt->subplans)
		number_insert_inputs(lfirst(lc), &next);
}

int
vexec_check_plan(PlannedStmt *pstmt, int mode)
{
	CheckContext ctx = {NULL, NIL, 0};
	ListCell   *lc;

	check_plan_node(pstmt->planTree, &ctx);
	foreach(lc, pstmt->subplans)
	{
		Plan	   *sub = lfirst(lc);

		/* each subplan's tree numbers its nodes on from the main tree's */
		check_plan_node(sub, &ctx);
	}
	bms_free(ctx.ids);

	if (ctx.vector_nodes > 0 &&
		(mode == VEXEC_MODE_OFF || mode == VEXEC_MODE_EXPLAIN))
		elog(ERROR, "vexec plan check: %d vector nodes in a plan made with vexec.mode = %s",
			 ctx.vector_nodes, vexec_mode_name(mode));
	return ctx.vector_nodes;
}
