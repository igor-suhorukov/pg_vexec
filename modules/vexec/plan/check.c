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
	int			vector_nodes;
} CheckContext;

/*
 * Whether a CustomScan is one of vexec's vector nodes, and that it is in
 * its canonical form (build.c): a VecScan scans a relation with no
 * custom_scan_tlist; a VecResult has its child in lefttree and no
 * relation.
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
		elog(ERROR, "vexec plan check: plan node id %d is used twice", plan->plan_node_id);
	ctx->ids = bms_add_member(ctx->ids, plan->plan_node_id);

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

int
vexec_check_plan(PlannedStmt *pstmt, int mode)
{
	CheckContext ctx = {NULL, 0};
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
