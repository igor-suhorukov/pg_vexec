/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * motion.c
 *	  ORCA's Motions, with frames built around them: step A of
 *	  pg_vector_executor.md §3.10, V7 (motion/motion.h).
 *
 * gp_orca offers vexec every node of a translated plan, children first, a
 * Motion among them (the port's pg19/orca/vector.c), before the Motions are
 * checked and the slice table made.  A Motion whose fragment's top is a
 * vector node gets frames: below it VecMotionSend, above it VecMotionRecv,
 * and the Motion itself rebuilt through gp_core's API for rows of the
 * fragment's columns, all NULL, a frame's segment and the frame -- a
 * Redistribute as an Explicit Redistribute keyed on the segment, a random
 * redistribution too, a Broadcast and a Gather as they were.
 *
 * The Motion is rebuilt where it stands.  ORCA's translator keeps its own
 * list of the plan's Motions, which direct dispatch and the slice table
 * read after the nodes are offered (CTranslatorDXLToPlStmt.cpp): a new
 * node in the Motion's place would leave the list pointing at the old one.
 * So gp_core makes the new Motion, whose entries for what the check of the
 * Motions sets after this are as empty as the old one's still are
 * (gp_motion.c, motion_make()), and it is copied over the old one, which
 * keeps its plan node id, its parameters and its initplans.
 *
 * Not framed:
 *	- with vexec.enable_motion_frames off, which is read here, as each
 *	  statement is planned: frames change the plan's shape, so a change of
 *	  the setting makes the session's cached plans be planned again
 *	  (vexec.c);
 *	- a sorted Gather, which merges its senders' rows; a write's Motion;
 *	  ORCA's Explicit Redistribute of a write's rows to their segments;
 *	- a parallel retrieve cursor's, whose endpoints return its top
 *	  fragment's rows to their clients (gp_core's gp_endpoint.c);
 *	- a Motion over a row node: its fragment hands up rows, which it would
 *	  turn into batches only to send them -- but for a Result that only
 *	  projects a vector node's columns, which gp_orca folds into that node
 *	  once the plan is made (projection_over_vector());
 *	- where the Motion's own target list or qual has what no vector node
 *	  evaluates, since VecMotionRecv evaluates them;
 *	- a Gather to the coordinator of a type with no binary I/O, which
 *	  gp_core's gather would carry as text, the frames with them.
 *
 * The nodes made here take their plan node ids once ORCA's plan is made
 * (vexec_orca_motion_number()): the plan's ids are unknown while its nodes
 * are offered, and M8's pass adds Gathers after.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_type.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "gp_core_api.h"

#include "vexec.h"
#include "exec/exec.h"
#include "motion/motion.h"
#include "plan/plan.h"

/* A node made here before its plan node id is given. */
#define VEXEC_NODE_ID_UNSET		(-1)

bool
vexec_is_gp_motion(Plan *plan)
{
	return plan != NULL && IsA(plan, CustomScan) &&
		strcmp(((CustomScan *) plan)->methods->CustomName, VEXEC_GP_MOTION_NAME) == 0;
}

/* Whether a type's values cross libpq in binary: send and receive functions. */
static bool
type_has_binary_io(Oid typid)
{
	HeapTuple	tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	Form_pg_type t;
	bool		binary;

	if (!HeapTupleIsValid(tp))
		return false;
	t = (Form_pg_type) GETSTRUCT(tp);
	binary = OidIsValid(t->typsend) && OidIsValid(t->typreceive);
	ReleaseSysCache(tp);
	return binary;
}

/* Column k of a plan's output as a Var of varno, from 1. */
static Var *
column_var(Plan *plan, int k, int varno)
{
	TargetEntry *tle = list_nth_node(TargetEntry, plan->targetlist, k - 1);

	return makeVar(varno, k, exprType((Node *) tle->expr), exprTypmod((Node *) tle->expr),
				   exprCollation((Node *) tle->expr), 0);
}

static char *
column_name(Plan *plan, int k)
{
	TargetEntry *tle = list_nth_node(TargetEntry, plan->targetlist, k - 1);

	return tle->resname ? pstrdup(tle->resname) : NULL;
}

/*
 * VecMotionSend over the fragment: its scan tuple the fragment's row, its
 * columns as Vars of it, then the frame's segment and the frame, which its
 * target list gives out as they are.
 */
static CustomScan *
make_send(Plan *fragment, int motion, List *hashexprs, List *hashfuncs, bool legacy)
{
	CustomScan *send = makeNode(CustomScan);
	VexecMotionSendPlan plan;
	int			n = list_length(fragment->targetlist);
	List	   *scan_tlist = NIL;
	List	   *tlist = NIL;
	int			k;

	for (k = 1; k <= n; k++)
		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) column_var(fragment, k, OUTER_VAR), k,
											 column_name(fragment, k), false));
	scan_tlist = lappend(scan_tlist,
						 makeTargetEntry((Expr *) makeNullConst(INT4OID, -1, InvalidOid),
										 n + 1, pstrdup("frame_segment"), false));
	scan_tlist = lappend(scan_tlist,
						 makeTargetEntry((Expr *) makeNullConst(BYTEAOID, -1, InvalidOid),
										 n + 2, pstrdup("frame"), false));
	foreach_node(TargetEntry, tle, scan_tlist)
		tlist = lappend(tlist,
						makeTargetEntry((Expr *) makeVar(INDEX_VAR, tle->resno,
														 exprType((Node *) tle->expr),
														 exprTypmod((Node *) tle->expr),
														 exprCollation((Node *) tle->expr), 0),
										tle->resno, tle->resname, false));

	plan.motion = motion;
	plan.legacy = legacy;
	plan.hashfuncs = hashfuncs;
	plan.ncols = n;

	send->scan.plan.targetlist = tlist;
	send->scan.plan.qual = NIL;
	send->scan.plan.lefttree = fragment;
	send->scan.plan.startup_cost = fragment->startup_cost;
	send->scan.plan.total_cost = fragment->total_cost;
	send->scan.plan.plan_rows = Max(1.0, ceil(fragment->plan_rows / VEXEC_BATCH_ROWS));
	send->scan.plan.plan_width = fragment->plan_width;
	send->scan.plan.parallel_safe = fragment->parallel_safe;
	send->scan.plan.plan_node_id = VEXEC_NODE_ID_UNSET;
	send->scan.scanrelid = 0;
	send->flags = 0;
	send->custom_plans = NIL;
	send->custom_exprs = hashexprs;
	send->custom_scan_tlist = scan_tlist;
	send->custom_private = vexec_motion_send_plan_encode(&plan);
	send->methods = vexec_motion_send_methods();
	return send;
}

/*
 * The Motion rebuilt over VecMotionSend, by gp_core, and copied over the
 * old one, which keeps what its place in the plan gave it.
 */
static void
rebuild_motion(const GpCoreApi *api, CustomScan *motion, int kind, CustomScan *send)
{
	Plan		saved = motion->scan.plan;
	List	   *tlist = NIL;
	Plan	   *built;
	int			content = api->motion_segment((Plan *) motion);
	int			slice = api->motion_slice((Plan *) motion);
	int			k;

	for (k = 1; k <= list_length(send->scan.plan.targetlist); k++)
		tlist = lappend(tlist, makeTargetEntry((Expr *) column_var(&send->scan.plan, k, OUTER_VAR),
											   k, column_name(&send->scan.plan, k), false));
	switch (kind)
	{
		case VEXEC_GP_MOTION_GATHER:
			built = api->motion_make_gather((Plan *) send, tlist, NIL, content, slice, 0,
											NULL, NULL, NULL, NULL);
			break;
		case VEXEC_GP_MOTION_BROADCAST:
			built = api->motion_make_send(VEXEC_GP_MOTION_BROADCAST, (Plan *) send, tlist, NIL,
										  content, slice, NIL, NIL);
			break;
		default:
			{
				/* each frame to the segment its row of the Motion names */
				int			segcol = list_length(tlist) - 1;

				built = api->motion_make_send(VEXEC_GP_MOTION_EXPLICIT, (Plan *) send, tlist,
											  NIL, content, slice,
											  list_make1(makeVar(OUTER_VAR, segcol, INT4OID, -1,
																 InvalidOid, 0)),
											  list_make1_oid(InvalidOid));
				break;
			}
	}
	if (built == NULL || !IsA(built, CustomScan))
		elog(ERROR, "vexec: gp_core made no Motion");
	*motion = *(CustomScan *) built;
	motion->scan.plan.plan_node_id = saved.plan_node_id;
	motion->scan.plan.initPlan = saved.initPlan;
	motion->scan.plan.extParam = saved.extParam;
	motion->scan.plan.allParam = saved.allParam;
	motion->scan.plan.parallel_aware = saved.parallel_aware;
	motion->scan.plan.parallel_safe = saved.parallel_safe;
	motion->scan.plan.async_capable = saved.async_capable;
	motion->scan.plan.startup_cost = send->scan.plan.startup_cost;
	motion->scan.plan.total_cost = send->scan.plan.total_cost;
	motion->scan.plan.plan_rows = send->scan.plan.plan_rows;
	motion->scan.plan.plan_width = send->scan.plan.plan_width;
}

/*
 * VecMotionRecv over the rebuilt Motion: its scan tuple the fragment's row,
 * the Motion's first columns, its target list and qual what the Motion's
 * were, which read that row as INDEX_VAR.
 */
static CustomScan *
make_recv(CustomScan *motion, int n, List *tlist, List *qual, const Plan *was)
{
	CustomScan *recv = makeNode(CustomScan);
	List	   *scan_tlist = NIL;
	int			k;

	for (k = 1; k <= n; k++)
		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry((Expr *) column_var(&motion->scan.plan, k, OUTER_VAR),
											 k, column_name(&motion->scan.plan, k), false));
	recv->scan.plan.targetlist = tlist;
	recv->scan.plan.qual = qual;
	recv->scan.plan.lefttree = (Plan *) motion;
	recv->scan.plan.startup_cost = was->startup_cost;
	recv->scan.plan.total_cost = was->total_cost;
	recv->scan.plan.plan_rows = was->plan_rows;
	recv->scan.plan.plan_width = was->plan_width;
	recv->scan.plan.parallel_safe = was->parallel_safe;
	recv->scan.plan.plan_node_id = VEXEC_NODE_ID_UNSET;
	recv->scan.scanrelid = 0;
	recv->flags = 0;
	recv->custom_plans = NIL;
	recv->custom_exprs = NIL;
	recv->custom_scan_tlist = scan_tlist;
	recv->custom_private = list_make1(makeInteger(n));
	recv->methods = vexec_motion_recv_methods();
	return recv;
}

/*
 * A Result that only projects, over a vector node: ORCA's translator puts
 * one above a node whose columns it would rearrange, and gp_orca folds it
 * into the node below once the plan is made, where that node projects
 * (the port's pg19/orca/orca.c, remove_redundant_results()) -- as VecAgg
 * does, whose rows VecMotionSend reads in either case.  The Motion's
 * fragment is then the vector node's.
 */
static bool
projection_over_vector(Plan *plan)
{
	return IsA(plan, Result) && ((Result *) plan)->resconstantqual == NULL &&
		plan->qual == NIL && plan->initPlan == NIL && plan->lefttree != NULL &&
		vexec_is_vector_node(plan->lefttree) &&
		!expression_returns_set((Node *) plan->targetlist);
}

/* What the Motion's rows are, and why it is not framed, or NULL. */
static const char *
motion_refusal(const GpCoreApi *api, CustomScan *motion, int cursorOptions, int *kind)
{
	Plan	   *fragment = motion->scan.plan.lefttree;

	if (!vexec_enable_motion_frames)
		return "vexec.enable_motion_frames is off";
	if (api == NULL)
		return psprintf("no gp_core of API 1.%d", VEXEC_GP_CORE_MINOR);
	if (cursorOptions & GP_CURSOR_OPT_PARALLEL_RETRIEVE)
		return "a parallel retrieve cursor, whose endpoints return the fragment's rows";
	*kind = api->motion_type((Plan *) motion);
	switch (*kind)
	{
		case VEXEC_GP_MOTION_GATHER:
			if (api->motion_merge_keys((Plan *) motion) > 0)
				return "a sorted Gather, which merges its senders' rows";
			break;
		case VEXEC_GP_MOTION_HASH:
		case VEXEC_GP_MOTION_BROADCAST:
		case VEXEC_GP_MOTION_RANDOM:
			break;
		case VEXEC_GP_MOTION_DML:
			return "a write's Motion";
		default:
			return "an Explicit Redistribute of a write's rows";
	}
	if (fragment == NULL ||
		!(vexec_is_vector_node(fragment) || projection_over_vector(fragment)))
		return "its fragment's top is a row node";
	if (*kind == VEXEC_GP_MOTION_GATHER)
	{
		foreach_node(TargetEntry, tle, fragment->targetlist)
			if (!type_has_binary_io(exprType((Node *) tle->expr)))
				return "a Gather of a type with no binary send and receive functions";
	}
	return NULL;
}

Plan *
vexec_orca_motion(VexecPlanState *ps, CustomScan *motion, int cursorOptions)
{
	const GpCoreApi *api = vexec_gp_core();
	Plan	   *fragment = motion->scan.plan.lefttree;
	VexecAlt   *alt = NULL;
	VexecSteps	steps;
	const char *refusal;
	int			kind = -1;
	List	   *hashexprs = NIL;
	List	   *hashfuncs = NIL;
	bool		legacy = false;
	Plan		was;
	List	   *tlist;
	List	   *qual;
	CustomScan *send;

	if (ps->record)
		alt = vexec_alt_record(ps, "VecMotionSend", "ORCA's Motion", NULL, NULL);
	refusal = motion_refusal(api, motion, cursorOptions, &kind);
	if (refusal == NULL)
	{
		/* what VecMotionRecv evaluates, and a Redistribute's keys */
		memset(&steps, 0, sizeof(steps));
		vexec_oracle_exprs(NULL, motion->scan.plan.targetlist, &steps);
		vexec_oracle_exprs(NULL, motion->scan.plan.qual, &steps);
		if (kind == VEXEC_GP_MOTION_HASH)
		{
			hashexprs = motion->custom_exprs;
			hashfuncs = api->motion_hash_functions((Plan *) motion, &legacy);
			vexec_oracle_exprs(NULL, hashexprs, &steps);
			if (list_length(hashfuncs) != list_length(hashexprs))
				steps.refusal = "a Redistribute whose keys are not its hash functions'";
		}
		refusal = steps.refusal;
	}
	if (refusal != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return NULL;
	}
	if (alt != NULL)
	{
		VexecCost	cost;

		memset(&cost, 0, sizeof(cost));
		cost.rows = motion->scan.plan.plan_rows;
		vexec_alt_costed(ps, alt, &cost,
						 psprintf("frames over a %s, over a vector node",
								  kind == VEXEC_GP_MOTION_HASH ? "Redistribute" :
								  kind == VEXEC_GP_MOTION_RANDOM ? "random redistribution" :
								  kind == VEXEC_GP_MOTION_BROADCAST ? "Broadcast" : "Gather"));
	}
	ps->npossible++;
	if (ps->mode != VEXEC_MODE_AUTO && ps->mode != VEXEC_MODE_FORCE)
		return NULL;

	/* the Motion's own columns and qual move up to VecMotionRecv */
	was = motion->scan.plan;
	tlist = motion->scan.plan.targetlist;
	qual = motion->scan.plan.qual;
	send = make_send(fragment, kind, copyObject(hashexprs), hashfuncs, legacy);
	rebuild_motion(api, motion, kind, send);
	ps->motions_framed++;
	return (Plan *) make_recv(motion, list_length(fragment->targetlist), tlist, qual, &was);
}

/* ---------------------------------------------------------------------
 * Plan node ids, once ORCA's plan is made
 * ---------------------------------------------------------------------
 */

typedef struct NumberWalk
{
	int			max;
	bool		assign;
} NumberWalk;

/* Every node of a tree, wherever the node keeps its children. */
static void
number_walk(Plan *plan, NumberWalk *w)
{
	if (plan == NULL)
		return;
	check_stack_depth();
	if (w->assign)
	{
		if (plan->plan_node_id == VEXEC_NODE_ID_UNSET && IsA(plan, CustomScan) &&
			(((CustomScan *) plan)->methods == vexec_motion_send_methods() ||
			 ((CustomScan *) plan)->methods == vexec_motion_recv_methods()))
			plan->plan_node_id = ++w->max;
	}
	else
		w->max = Max(w->max, plan->plan_node_id);
	switch (nodeTag(plan))
	{
		case T_CustomScan:
			foreach_ptr(Plan, child, ((CustomScan *) plan)->custom_plans)
				number_walk(child, w);
			break;
		case T_Append:
			foreach_ptr(Plan, child, ((Append *) plan)->appendplans)
				number_walk(child, w);
			break;
		case T_MergeAppend:
			foreach_ptr(Plan, child, ((MergeAppend *) plan)->mergeplans)
				number_walk(child, w);
			break;
		case T_BitmapAnd:
			foreach_ptr(Plan, child, ((BitmapAnd *) plan)->bitmapplans)
				number_walk(child, w);
			break;
		case T_BitmapOr:
			foreach_ptr(Plan, child, ((BitmapOr *) plan)->bitmapplans)
				number_walk(child, w);
			break;
		case T_SubqueryScan:
			number_walk(((SubqueryScan *) plan)->subplan, w);
			break;
		default:
			break;
	}
	number_walk(plan->lefttree, w);
	number_walk(plan->righttree, w);
}

/*
 * The nodes made around Motions given plan node ids past every other node's
 * of the plan and its subplans, as the plan's own passes give the nodes
 * they add (the port's lockrows.c, parallel.c).
 */
void
vexec_orca_motion_number(VexecPlanState *ps, PlannedStmt *stmt)
{
	NumberWalk	w = {0, false};

	if (ps == NULL || ps->motions_framed == 0)
		return;
	number_walk(stmt->planTree, &w);
	foreach_ptr(Plan, sub, stmt->subplans)
		number_walk(sub, &w);
	w.assign = true;
	number_walk(stmt->planTree, &w);
	foreach_ptr(Plan, sub, stmt->subplans)
		number_walk(sub, &w);
}
