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

/* OT7: what the statement's vector nodes with a device strategy did */
typedef struct DeviceSums
{
	bool		on_error;
	int			device_nodes;	/* ran on the device, all or in part */
	int			cpu_nodes;		/* a device strategy, run on the CPU: none, or
								 * given up */
	double		device_ms;
	double		gpu_ms;			/* of it, the GPU's, by CUDA events (OT10) */
} DeviceSums;

static bool
sum_device(PlanState *ps, void *context)
{
	DeviceSums *sums = (DeviceSums *) context;

	if (ps == NULL)
		return false;
	if (vexec_is_vector_state(ps) && ((VexecNode *) ps)->ran)
	{
		VexecNodeFigures f;

		vexec_node_figures((VexecNode *) ps, &f, sums->on_error);
		if (f.device != NULL && strcmp(f.device, "none") != 0)
			sums->device_nodes++;
		if (f.device != NULL && (strcmp(f.device, "none") == 0 || f.device_why != NULL))
			sums->cpu_nodes++;
		if (f.device != NULL && f.device_ms > 0)
			sums->device_ms += f.device_ms;
		if (f.device != NULL && f.device_gpu_ms > 0)
			sums->gpu_ms += f.device_gpu_ms;
	}
	return planstate_tree_walker(ps, sum_device, context);
}

static const char *
gpu_name(int gpu)
{
	switch (gpu)
	{
		case VEXEC_GPU_AUTO:
			return "auto";
		case VEXEC_GPU_FORCE:
			return "force";
		default:
			return "off";
	}
}

/*
 * What a statement's span carries in pg_otel_probe's traces (its PLAN.md
 * §4.7.4, pg_otel_probe_api.h): the mode and the batch format it was
 * planned in, whether the gate let the vectorized planner in and why not,
 * and the vector nodes its plan has.  In every mode, its planning's summary
 * (pg_vector_executor.md §3.41, OT6, plan/reasons.c): the alternatives the
 * oracle accepted and those it refused, with the first refusal's reason, and
 * what the sources' planning read (OT5); in explain mode, the number of
 * alternatives the planners priced and did not take -- the record EXPLAIN
 * (VEXEC) prints.  And what the device did (OT7): vexec.gpu, the nodes the
 * device took, those whose device strategy ran on the CPU, and the device's
 * time.  A segment's fragment, a part of a plan made on the coordinator,
 * carries no record: its span has its vector nodes and its device alone.  It
 * reads the plan and the nodes' states, and so is safe on a failed
 * statement's error path.
 */
static void
vexec_statement_report(QueryDesc *queryDesc, PgOtelProbeAttrs *attrs, int when)
{
	PlannedStmt *pstmt = queryDesc->plannedstmt;
	List	   *items = NIL;
	List	   *summary = NIL;
	ListCell   *lc;
	int			nodes = 0;
	const char *gate;
	const char *refusal;
	Node	   *dropped;
	Node	   *n;
	DeviceSums	sums;

	if (pstmt == NULL || pstmt->commandType == CMD_UTILITY)
		return;
	foreach(lc, pstmt->extension_state)
	{
		DefElem    *d = lfirst_node(DefElem, lc);

		if (strcmp(d->defname, "vexec") == 0)
			items = (List *) d->arg;
		else if (strcmp(d->defname, "vexec_summary") == 0)
			summary = (List *) d->arg;
	}
	nodes = count_vector_nodes(pstmt->planTree);
	foreach(lc, pstmt->subplans)
		nodes += count_vector_nodes(lfirst(lc));
	if (items == NIL && summary == NIL && nodes == 0 && vexec_mode == VEXEC_MODE_OFF)
		return;
	/* the mode and format it was planned in; else the mode the session runs in */
	if (summary != NIL || items != NIL)
	{
		List	   *planned = summary != NIL ? summary : items;

		attrs->add_string(attrs, "vexec.mode", item_str(planned, "mode"));
		attrs->add_string(attrs, "vexec.batch_format", item_str(planned, "format"));
		gate = item_str(planned, "gate");
		if (gate != NULL && strcmp(gate, "open") != 0)
			attrs->add_string(attrs, "vexec.gate", gate);
	}
	else
		attrs->add_string(attrs, "vexec.mode", vexec_mode_name(vexec_mode));
	if (summary != NIL)
	{
		n = item(summary, "possible");
		if (n != NULL && IsA(n, Integer))
			attrs->add_int(attrs, "vexec.alternatives.possible", intVal(n));
		n = item(summary, "refused");
		if (n != NULL && IsA(n, Integer))
			attrs->add_int(attrs, "vexec.alternatives.refused", intVal(n));
		refusal = item_str(summary, "refusal");
		if (refusal != NULL)
			attrs->add_string(attrs, "vexec.refusal", refusal);
		n = item(summary, "planning");
		if (n != NULL && IsA(n, List))
		{
			foreach(lc, (List *) n)
			{
				DefElem    *p = lfirst_node(DefElem, lc);

				if (p->arg != NULL && IsA(p->arg, Integer))
					attrs->add_int(attrs, p->defname, intVal(p->arg));
			}
		}
	}
	if (items != NIL)
	{
		dropped = item(items, "dropped");
		attrs->add_int(attrs, "vexec.alternatives_not_taken",
					   list_length((List *) item(items, "alternatives")) +
					   (dropped != NULL && IsA(dropped, Integer) ? intVal(dropped) : 0));
	}
	attrs->add_int(attrs, "vexec.vector_nodes", nodes);

	/* OT7: the device, where a node had its strategy or vexec.gpu is on */
	memset(&sums, 0, sizeof(sums));
	sums.on_error = when == PG_OTEL_PROBE_REPORT_ABORT;
	if (queryDesc->planstate != NULL)
		(void) sum_device(queryDesc->planstate, &sums);
	if (vexec_gpu != VEXEC_GPU_OFF || sums.device_nodes > 0 || sums.cpu_nodes > 0)
		attrs->add_string(attrs, "vexec.gpu", gpu_name(vexec_gpu));
	if (sums.device_nodes > 0 || sums.cpu_nodes > 0)
	{
		attrs->add_int(attrs, "vexec.device_nodes", sums.device_nodes);
		attrs->add_int(attrs, "vexec.device_nodes_cpu", sums.cpu_nodes);
		attrs->add_double(attrs, "vexec.device.ms", sums.device_ms);
		attrs->add_double(attrs, "vexec.device.gpu_ms", sums.gpu_ms);
	}
}