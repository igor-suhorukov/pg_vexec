/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * explain.c
 *	  EXPLAIN's vexec option (pg_vector_executor.md §3.3.2, Reasons).
 *
 * EXPLAIN (VEXEC) prints, after the plan, the vector alternatives the
 * planner considered and what became of each: the oracle's refusal, or the
 * two costs and why it was not chosen.  They are what the planner put into
 * PlannedStmt.extension_state; in explain mode it always records them, in
 * auto and force mode only for a statement EXPLAIN (VEXEC) plans.  Without
 * the option EXPLAIN prints nothing of vexec's, so that plans read as
 * PostgreSQL's.  COSTS OFF leaves the costs out, as it does the plan's.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#include "nodes/nodeFuncs.h"
#include "nodes/value.h"

#include "vexec.h"
#include "pg_otel_probe_api.h"
#include "exec/exec.h"
#include "plan/plan.h"

typedef struct VexecExplainOptions
{
	bool		vexec;
} VexecExplainOptions;

static int	explain_id = -1;
static explain_per_plan_hook_type prev_explain_per_plan = NULL;

static void vexec_explain_option(ExplainState *es, DefElem *opt, ParseState *pstate);
static void vexec_explain_per_plan(PlannedStmt *pstmt, IntoClause *into, ExplainState *es,
								   const char *queryString, ParamListInfo params,
								   QueryEnvironment *queryEnv);
static void vexec_statement_report(QueryDesc *queryDesc, PgOtelProbeAttrs *attrs, int when);

void
vexec_explain_install(void)
{
	explain_id = GetExplainExtensionId("vexec");
	RegisterExtensionExplainOption("vexec", vexec_explain_option,
								   GUCCheckBooleanExplainOption);
	prev_explain_per_plan = explain_per_plan_hook;
	explain_per_plan_hook = vexec_explain_per_plan;

	/* pg_otel_probe's span of a statement, where it is loaded (§4.7.4) */
	pg_otel_probe_register_statement_reporter(vexec_statement_report, PG_OTEL_PROBE_ABORT_SAFE);
}

static void
vexec_explain_option(ExplainState *es, DefElem *opt, ParseState *pstate)
{
	VexecExplainOptions *opts = GetExplainExtensionState(es, explain_id);

	if (opts == NULL)
	{
		opts = palloc0(sizeof(VexecExplainOptions));
		SetExplainExtensionState(es, explain_id, opts);
	}
	opts->vexec = defGetBoolean(opt);
}

/* Whether this EXPLAIN asked for vexec's alternatives. */
bool
vexec_explain_requested(void *esp)
{
	ExplainState *es = esp;
	VexecExplainOptions *opts;

	if (es == NULL || explain_id < 0)
		return false;
	opts = GetExplainExtensionState(es, explain_id);
	return opts != NULL && opts->vexec;
}

static Node *
item(List *items, const char *name)
{
	ListCell   *lc;

	foreach(lc, items)
	{
		DefElem    *d = lfirst_node(DefElem, lc);

		if (strcmp(d->defname, name) == 0)
			return d->arg;
	}
	return NULL;
}

static const char *
item_str(List *items, const char *name)
{
	Node	   *n = item(items, name);

	return n && IsA(n, String) ? strVal(n) : NULL;
}

static double
item_cost(List *items, const char *name)
{
	Node	   *n = item(items, name);

	return n && IsA(n, Float) ? floatVal(n) : 0;
}

static void
print_alternative(List *a, ExplainState *es)
{
	const char *node = item_str(a, "node");
	const char *target = item_str(a, "target");
	const char *reason = item_str(a, "reason");
	const char *detail = item_str(a, "detail");
	const char *status = item_str(a, "status");
	Node	   *possible_node = item(a, "possible");
	bool		possible = possible_node && IsA(possible_node, Boolean) && boolVal(possible_node);
	double		row_total = item_cost(a, "row_total");
	double		total = item_cost(a, "total");

	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		ExplainIndentText(es);
		if (possible)
			appendStringInfo(es->str, "%s on %s: %s (%s)\n", node, target,
							 status ? status : "not chosen", reason);
		else
			appendStringInfo(es->str, "%s on %s: not possible (%s)\n", node, target, reason);
		es->indent++;
		if (possible && es->costs)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str,
							 "Row Cost: %.2f..%.2f  Vector Cost: %.2f..%.2f (%s)  Rows: %.0f\n",
							 item_cost(a, "row_startup"), row_total,
							 item_cost(a, "startup"), total,
							 total < row_total ? "cheaper" : "dearer",
							 item_cost(a, "rows"));
			ExplainIndentText(es);
			appendStringInfo(es->str, "Crossings: rows in %.2f, rows out %.2f\n",
							 item_cost(a, "convert_in"), item_cost(a, "rowout"));
		}
		if (detail)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "%s\n", detail);
		}
		es->indent--;
		return;
	}

	ExplainOpenGroup("Alternative", NULL, true, es);
	ExplainPropertyText("Node", node, es);
	ExplainPropertyText("Target", target, es);
	ExplainPropertyBool("Possible", possible, es);
	if (possible)
		ExplainPropertyText("Status", status ? status : "not chosen", es);
	ExplainPropertyText("Reason", reason, es);
	if (detail)
		ExplainPropertyText("Detail", detail, es);
	if (possible && es->costs)
	{
		ExplainPropertyFloat("Row Startup Cost", NULL, item_cost(a, "row_startup"), 2, es);
		ExplainPropertyFloat("Row Total Cost", NULL, row_total, 2, es);
		ExplainPropertyFloat("Startup Cost", NULL, item_cost(a, "startup"), 2, es);
		ExplainPropertyFloat("Total Cost", NULL, total, 2, es);
		ExplainPropertyFloat("Rows In Cost", NULL, item_cost(a, "convert_in"), 2, es);
		ExplainPropertyFloat("Rows Out Cost", NULL, item_cost(a, "rowout"), 2, es);
		ExplainPropertyFloat("Plan Rows", NULL, item_cost(a, "rows"), 0, es);
	}
	ExplainCloseGroup("Alternative", NULL, true, es);
}

static void
print_vexec(PlannedStmt *pstmt, ExplainState *es)
{
	List	   *items = NIL;
	ListCell   *lc;
	const char *gate;
	List	   *alts;

	foreach(lc, pstmt->extension_state)
	{
		DefElem    *d = lfirst_node(DefElem, lc);

		if (strcmp(d->defname, "vexec") == 0)
			items = (List *) d->arg;
	}

	ExplainOpenGroup("Vexec", "Vexec", true, es);
	if (items == NIL)
	{
		/* vexec.mode was off, or another planner made the plan */
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			ExplainIndentText(es);
			appendStringInfoString(es->str, "Vexec: no vector alternatives were considered\n");
		}
		else
			ExplainPropertyText("Mode", "none recorded", es);
		ExplainCloseGroup("Vexec", "Vexec", true, es);
		return;
	}

	gate = item_str(items, "gate");
	alts = (List *) item(items, "alternatives");
	if (es->format == EXPLAIN_FORMAT_TEXT)
	{
		ExplainIndentText(es);
		appendStringInfo(es->str, "Vexec: mode %s, format %s\n",
						 item_str(items, "mode"), item_str(items, "format"));
		es->indent++;
		if (gate && strcmp(gate, "open") != 0)
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Statement: not vectorized: %s\n", gate);
		}
		foreach(lc, alts)
			print_alternative(lfirst(lc), es);
		if (item(items, "dropped"))
		{
			ExplainIndentText(es);
			appendStringInfo(es->str, "Alternatives not recorded: %d\n",
							 intVal(item(items, "dropped")));
		}
		es->indent--;
	}
	else
	{
		ExplainPropertyText("Mode", item_str(items, "mode"), es);
		ExplainPropertyText("Format", item_str(items, "format"), es);
		ExplainPropertyText("Statement", gate && strcmp(gate, "open") == 0 ? "vectorizable" : gate, es);
		if (alts != NIL)
		{
			ExplainOpenGroup("Alternatives", "Alternatives", false, es);
			foreach(lc, alts)
				print_alternative(lfirst(lc), es);
			ExplainCloseGroup("Alternatives", "Alternatives", false, es);
		}
		if (item(items, "dropped"))
			ExplainPropertyInteger("Alternatives Not Recorded", NULL,
								   intVal(item(items, "dropped")), es);
	}
	ExplainCloseGroup("Vexec", "Vexec", true, es);
}

static void
vexec_explain_per_plan(PlannedStmt *pstmt, IntoClause *into, ExplainState *es,
					   const char *queryString, ParamListInfo params,
					   QueryEnvironment *queryEnv)
{
	if (vexec_explain_requested(es))
		print_vexec(pstmt, es);

	if (prev_explain_per_plan)
		prev_explain_per_plan(pstmt, into, es, queryString, params, queryEnv);
}

/* ---------------------------------------------------------------------
 * pg_otel_probe's span of a statement
 * ---------------------------------------------------------------------
 */

/*
 * The vector nodes of a plan tree, the fragments below a Motion included:
 * on the port's coordinator those are only described, never initialised.
 */
static int
count_vector_nodes(Plan *plan)
{
	ListCell   *lc;
	int			n;

	if (plan == NULL)
		return 0;
	check_stack_depth();
	n = vexec_is_vector_node(plan) ? 1 : 0;
	n += count_vector_nodes(plan->lefttree) + count_vector_nodes(plan->righttree);
	switch (nodeTag(plan))
	{
		case T_Append:
			foreach(lc, ((Append *) plan)->appendplans)
				n += count_vector_nodes(lfirst(lc));
			break;
		case T_MergeAppend:
			foreach(lc, ((MergeAppend *) plan)->mergeplans)
				n += count_vector_nodes(lfirst(lc));
			break;
		case T_BitmapAnd:
			foreach(lc, ((BitmapAnd *) plan)->bitmapplans)
				n += count_vector_nodes(lfirst(lc));
			break;
		case T_BitmapOr:
			foreach(lc, ((BitmapOr *) plan)->bitmapplans)
				n += count_vector_nodes(lfirst(lc));
			break;
		case T_SubqueryScan:
			n += count_vector_nodes(((SubqueryScan *) plan)->subplan);
			break;
		case T_CustomScan:
			foreach(lc, ((CustomScan *) plan)->custom_plans)
				n += count_vector_nodes(lfirst(lc));
			break;
		default:
			break;
	}
	return n;
}

/*
 * What a statement's span carries in pg_otel_probe's traces (its PLAN.md
 * §4.7.4, pg_otel_probe_api.h): the mode and the batch format it was
 * planned in, whether the gate let the vectorized planner in and why not,
 * the vector nodes its plan has, and the alternatives the planners priced
 * and did not take -- the record EXPLAIN (VEXEC) prints.  A segment's
 * fragment, a part of a plan made on the coordinator, carries no record:
 * its span has its vector nodes alone.  It reads the plan, and so is safe on
 * a failed statement's error path.
 */
static void
vexec_statement_report(QueryDesc *queryDesc, PgOtelProbeAttrs *attrs, int when)
{
	PlannedStmt *pstmt = queryDesc->plannedstmt;
	List	   *items = NIL;
	ListCell   *lc;
	int			nodes = 0;
	const char *gate;
	Node	   *dropped;

	if (pstmt == NULL || pstmt->commandType == CMD_UTILITY)
		return;
	foreach(lc, pstmt->extension_state)
	{
		DefElem    *d = lfirst_node(DefElem, lc);

		if (strcmp(d->defname, "vexec") == 0)
			items = (List *) d->arg;
	}
	nodes = count_vector_nodes(pstmt->planTree);
	foreach(lc, pstmt->subplans)
		nodes += count_vector_nodes(lfirst(lc));
	if (items == NIL && nodes == 0 && vexec_mode == VEXEC_MODE_OFF)
		return;
	/* the record is kept in explain mode; else the mode the session runs in */
	if (items == NIL)
		attrs->add_string(attrs, "vexec.mode", vexec_mode_name(vexec_mode));
	if (items != NIL)
	{
		attrs->add_string(attrs, "vexec.mode", item_str(items, "mode"));
		attrs->add_string(attrs, "vexec.batch_format", item_str(items, "format"));
		gate = item_str(items, "gate");
		if (gate != NULL && strcmp(gate, "open") != 0)
			attrs->add_string(attrs, "vexec.gate", gate);
		dropped = item(items, "dropped");
		attrs->add_int(attrs, "vexec.alternatives_not_taken",
					   list_length((List *) item(items, "alternatives")) +
					   (dropped != NULL && IsA(dropped, Integer) ? intVal(dropped) : 0));
	}
	attrs->add_int(attrs, "vexec.vector_nodes", nodes);
}