/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * reasons.c
 *	  Every vector alternative a planner did not take keeps its reason
 *	  (pg_vector_executor.md §3.3.2): the oracle's refusal, or the two costs.
 *
 * They are collected in the planner's extension state while a statement is
 * planned, and put into PlannedStmt.extension_state as a "vexec" DefElem
 * when it is done, a tree of DefElems over String, Integer, Float and
 * Boolean nodes, so that it copies, prints and reads back as any plan does.
 * EXPLAIN's vexec option prints them (explain.c).
 *
 * The join hook fires twice per pair of relations, once per orientation,
 * and for child joins too, so a join relation keeps one record: its
 * cheapest possible alternative, or its first refusal.  A statement keeps
 * at most VEXEC_MAX_ALTS records, and counts those it dropped.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "nodes/makefuncs.h"
#include "nodes/value.h"
#include "parser/parsetree.h"

#include "vexec.h"
#include "plan/plan.h"

/*
 * A record for an alternative of a node kind over some relations, in the
 * planning context.  A join relation's existing record is returned for it
 * to be updated: one with the same relations at the same query level, since
 * each subquery numbers its range table from 1.  NULL past the cap.
 */
VexecAlt *
vexec_alt_record(VexecPlanState *ps, const char *node, const char *target,
				 const void *root, Relids relids)
{
	MemoryContext old;
	VexecAlt   *alt;
	ListCell   *lc;

	if (relids != NULL)
	{
		foreach(lc, ps->alts)
		{
			alt = lfirst(lc);
			if (alt->relids != NULL && alt->root == root &&
				strcmp(alt->node, node) == 0 && bms_equal(alt->relids, relids))
				return alt;
		}
	}
	if (ps->nalts >= VEXEC_MAX_ALTS)
	{
		ps->dropped++;
		return NULL;
	}

	old = MemoryContextSwitchTo(ps->mcxt);
	alt = palloc0(sizeof(VexecAlt));
	alt->node = node;
	alt->target = pstrdup(target);
	alt->root = root;
	alt->relids = bms_copy(relids);
	alt->cost.total = -1;
	ps->alts = lappend(ps->alts, alt);
	ps->nalts++;
	MemoryContextSwitchTo(old);
	return alt;
}

/* The oracle refused it: the first reason stays, unless one was possible. */
void
vexec_alt_refuse(VexecPlanState *ps, VexecAlt *alt, const char *reason)
{
	if (alt == NULL || alt->possible || alt->reason != NULL)
		return;
	alt->reason = MemoryContextStrdup(ps->mcxt, reason);
}

/*
 * It was possible, at this cost, as the detail describes it.  A join
 * relation keeps the cheapest of its alternatives, with that one's detail.
 * What became of it follows from the mode and the node:
 *
 *	explain mode	not chosen: alternatives are costed and recorded, never
 *					chosen
 *	VecScan			added, as a path that competes in add_path by cost
 *					(auto), or that is the relation's scan (force); in
 *					ORCA's plans built, or not chosen where vexec's cost
 *					model prices it above ORCA's row scan
 *	VecResult		built, in ORCA's plans
 *	VecAgg			added, as paths of the grouped relation (auto), or as
 *					its paths (force); in ORCA's plans built, or not chosen
 *	VecHashJoin		added, as a path of the join relation (auto), or as its
 *					paths (force); in ORCA's plans built, or not chosen
 *	VecSort			added, as a path of the ordered relation (auto), or as
 *					its paths (force); in ORCA's plans built, or not chosen
 *
 * VecScan's and VecHashJoin's partial paths (V4) go with their own, into
 * the relation's partial paths.
 */
void
vexec_alt_costed(VexecPlanState *ps, VexecAlt *alt, const VexecCost *cost, const char *detail)
{
	bool		built_now;

	if (alt == NULL)
		return;
	if (alt->possible && alt->cost.total >= 0 && alt->cost.total <= cost->total)
		return;
	alt->possible = true;
	alt->cost = *cost;
	alt->detail = detail ? MemoryContextStrdup(ps->mcxt, detail) : NULL;
	built_now = strcmp(alt->node, "VecScan") == 0 || strcmp(alt->node, "VecResult") == 0 ||
		strcmp(alt->node, "VecAgg") == 0 || strcmp(alt->node, "VecHashJoin") == 0 ||
		strcmp(alt->node, "VecSort") == 0 || strcmp(alt->node, "VecBitmapHeapScan") == 0 ||
		strcmp(alt->node, "VecWindowHashAgg") == 0 || strcmp(alt->node, "VecInsert") == 0 ||
		strcmp(alt->node, "VecIngest") == 0;
	if (ps->mode == VEXEC_MODE_EXPLAIN)
	{
		alt->status = "not chosen";
		alt->reason = "explain mode";
	}
	else if (!built_now)
	{
		alt->status = "not built";
		alt->reason = "no vector node of its kind";
	}
	else if (alt->root != NULL)
	{
		/* PostgreSQL's planner: a path */
		alt->status = "added";
		if (ps->mode != VEXEC_MODE_FORCE)
			alt->reason = "to be chosen by cost";
		else if (strcmp(alt->node, "VecScan") == 0)
			alt->reason = "forced: the relation's scans, partial ones too";
		else if (strcmp(alt->node, "VecHashJoin") == 0)
			alt->reason = "forced: the join's paths, partial ones too";
		else if (strcmp(alt->node, "VecSort") == 0)
			alt->reason = "forced: the ordered relation's paths";
		else
			alt->reason = "forced: the aggregation's paths";
	}
	else if (ps->mode == VEXEC_MODE_AUTO && ps->orca_costed)
	{
		/* ORCA's plans, which ORCA's search chose with this node's price */
		alt->status = "built";
		alt->reason = "priced in ORCA's search";
	}
	else if (ps->mode == VEXEC_MODE_AUTO && cost->row_total > 0 && cost->total >= cost->row_total)
	{
		/* ORCA's plans: built or not, here */
		alt->status = "not chosen";
		alt->reason = "dearer than ORCA's row node, by vexec's cost model";
	}
	else
	{
		alt->status = "built";
		alt->reason = ps->mode == VEXEC_MODE_FORCE ? "forced" : "cheaper";
	}
}

/* The names of a set of base relations, as EXPLAIN calls them. */
char *
vexec_relids_names(PlannerInfo *root, Relids relids)
{
	StringInfoData buf;
	int			i = -1;
	bool		first = true;

	initStringInfo(&buf);
	while ((i = bms_next_member(relids, i)) >= 0)
	{
		RangeTblEntry *rte;

		if (i == 0 || i >= root->simple_rel_array_size)
			continue;
		rte = root->simple_rte_array[i];
		if (rte == NULL || rte->rtekind == RTE_JOIN || rte->rtekind == RTE_GROUP)
			continue;
		appendStringInfo(&buf, "%s%s", first ? "" : ", ",
						 rte->eref ? rte->eref->aliasname : "?");
		first = false;
	}
	if (root->plan_name != NULL)
		appendStringInfo(&buf, " (%s)", root->plan_name);
	return buf.data;
}

static DefElem *
item_string(const char *name, const char *value)
{
	return makeDefElem(pstrdup(name), (Node *) makeString(pstrdup(value ? value : "")), -1);
}

/*
 * A cost as a Float node.  The factors take any value up to DBL_MAX, so a
 * cost may be infinite or past any meaning; a Float of "inf" would not read
 * back from the plan's text (readfuncs), so costs are clamped to +-1e18.
 */
static DefElem *
item_cost(const char *name, double value)
{
	if (isnan(value))
		value = 0;
	else if (value > 1e18)
		value = 1e18;
	else if (value < -1e18)
		value = -1e18;
	return makeDefElem(pstrdup(name), (Node *) makeFloat(psprintf("%.2f", value)), -1);
}

/* The record as a node tree, for PlannedStmt.extension_state. */
Node *
vexec_reasons_node(VexecPlanState *ps)
{
	List	   *items = NIL;
	List	   *alts = NIL;
	ListCell   *lc;

	items = lappend(items, item_string("mode", vexec_mode_name(ps->mode)));
	items = lappend(items, item_string("format",
									   ps->layout.format == VEXEC_FORMAT_ARROW ? "arrow" : "postgres"));
	items = lappend(items, item_string("gate", ps->gate_open ? "open" : ps->gate_reason));
	foreach(lc, ps->alts)
	{
		VexecAlt   *alt = lfirst(lc);
		List	   *a = NIL;

		a = lappend(a, item_string("node", alt->node));
		a = lappend(a, item_string("target", alt->target));
		a = lappend(a, makeDefElem(pstrdup("possible"), (Node *) makeBoolean(alt->possible), -1));
		if (alt->possible)
			a = lappend(a, item_string("status", alt->status ? alt->status : "?"));
		a = lappend(a, item_string("reason", alt->reason ? alt->reason : "?"));
		if (alt->detail)
			a = lappend(a, item_string("detail", alt->detail));
		if (alt->possible)
		{
			a = lappend(a, item_cost("rows", alt->cost.rows));
			a = lappend(a, item_cost("row_startup", alt->cost.row_startup));
			a = lappend(a, item_cost("row_total", alt->cost.row_total));
			a = lappend(a, item_cost("startup", alt->cost.startup));
			a = lappend(a, item_cost("total", alt->cost.total));
			a = lappend(a, item_cost("convert_in", alt->cost.convert_in));
			a = lappend(a, item_cost("rowout", alt->cost.rowout));
		}
		alts = lappend(alts, a);
	}
	items = lappend(items, makeDefElem(pstrdup("alternatives"), (Node *) alts, -1));
	if (ps->dropped > 0)
		items = lappend(items, makeDefElem(pstrdup("dropped"), (Node *) makeInteger(ps->dropped), -1));
	return (Node *) items;
}
