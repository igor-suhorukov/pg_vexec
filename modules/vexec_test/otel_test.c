/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * otel_test.c
 *	  pg_otel_probe's side of its reporters, for the suite
 *	  (pg_vector_executor.md §3.41): a query run, and the reporters
 *	  registered with pg_otel_probe/node_reporter_v1 called for its nodes
 *	  and its statement as the probe calls them (its PLAN.md §4.7.4,
 *	  src/reporters.c) -- at ExecutorEnd before the nodes are freed, or, the
 *	  abort-safe ones alone, on its error path with the nodes as the error
 *	  left them -- each attribute a row of otel_report().
 *
 * The probe need not be loaded: vexec registers its reporters in the
 * registry whichever module made it, and this reads it as the probe does.
 * As the probe, a node's attributes are at most PG_OTEL_PROBE_ATTRS_MAX in
 * PG_OTEL_PROBE_ATTRS_BYTES of keys and strings, the rest dropped and
 * counted -- a row says so -- and nothing is allocated while they are
 * taken: they go into a fixed area, which the function makes rows of once
 * the query is done.  A reporter that raises at ExecutorEnd has its
 * attributes dropped and its error a row, as the probe counts it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"

#include "pg_otel_probe_api.h"

#include "vexec_test.h"

PG_FUNCTION_INFO_V1(vexec_test_otel_report);

#define AREA_ENTRIES	1024
#define KEY_LEN			64
#define VALUE_LEN		256
#define NAME_LEN		48

typedef struct Entry
{
	int			node;			/* the plan node's id + 1; 0 the statement */
	char		name[NAME_LEN];
	char		key[KEY_LEN];
	char		value[VALUE_LEN];
} Entry;

/* The attributes taken, as the probe takes them: a fixed area */
static Entry area[AREA_ENTRIES];
static int	nentries = 0;
static int64 dropped = 0;		/* past the area, or a node's bounds */

/* The node being reported: its id, name, and what it has used of its bounds */
typedef struct Current
{
	int			node;
	const char *name;
	int			nattrs;
	int			bytes;
	int			first;			/* its first entry, for an error's drop */
} Current;

static Current cur;

/* The query otel_report() runs, while it runs; its first run alone is taken */
static const char *capture_query = NULL;
static bool capture_done = false;

static ExecutorRun_hook_type prev_executor_run = NULL;
static ExecutorEnd_hook_type prev_executor_end = NULL;

/* ---- the attributes ---- */

static Entry *
next_entry(const char *key, const char *str)
{
	int			klen;
	int			slen;
	Entry	   *e;

	if (key == NULL || key[0] == '\0')
		return NULL;
	klen = (int) strlen(key) + 1;
	slen = str != NULL ? (int) strlen(str) + 1 : 0;
	if (cur.nattrs >= PG_OTEL_PROBE_ATTRS_MAX || cur.bytes + klen + slen > PG_OTEL_PROBE_ATTRS_BYTES ||
		nentries >= AREA_ENTRIES)
	{
		dropped++;
		return NULL;
	}
	cur.nattrs++;
	cur.bytes += klen + slen;
	e = &area[nentries++];
	e->node = cur.node;
	strlcpy(e->name, cur.name, sizeof(e->name));
	strlcpy(e->key, key, sizeof(e->key));
	e->value[0] = '\0';
	return e;
}

static void
add_int(PgOtelProbeAttrs *attrs, const char *key, int64 value)
{
	Entry	   *e = next_entry(key, NULL);

	if (e != NULL)
		snprintf(e->value, sizeof(e->value), INT64_FORMAT, value);
}

static void
add_double(PgOtelProbeAttrs *attrs, const char *key, double value)
{
	Entry	   *e = next_entry(key, NULL);

	if (e != NULL)
		snprintf(e->value, sizeof(e->value), "%g", value);
}

static void
add_string(PgOtelProbeAttrs *attrs, const char *key, const char *value)
{
	Entry	   *e = next_entry(key, value != NULL ? value : "");

	if (e != NULL)
		strlcpy(e->value, value != NULL ? value : "", sizeof(e->value));
}

static void
add_bool(PgOtelProbeAttrs *attrs, const char *key, bool value)
{
	Entry	   *e = next_entry(key, NULL);

	if (e != NULL)
		strlcpy(e->value, value ? "true" : "false", sizeof(e->value));
}

static PgOtelProbeAttrs attrs = {
	.add_int = add_int,
	.add_double = add_double,
	.add_string = add_string,
	.add_bool = add_bool,
	.state = NULL,
};

/* ---- the reporters ---- */

static const PgOtelProbeReporterRegistry *
registry(void)
{
	PgOtelProbeReporterRegistry **rv;

	rv = (PgOtelProbeReporterRegistry **) find_rendezvous_variable(PG_OTEL_PROBE_REPORTER_RENDEZVOUS);
	return (*rv != NULL && (*rv)->magic == PG_OTEL_PROBE_REPORTER_MAGIC) ? *rv : NULL;
}

/* The table access method a scan node reads, as the probe finds it, or NULL */
static const void *
scan_am(PlanState *ps)
{
	Relation	rel;

	switch (nodeTag(ps))
	{
		case T_SeqScanState:
		case T_SampleScanState:
		case T_BitmapHeapScanState:
		case T_TidScanState:
		case T_TidRangeScanState:
		case T_IndexScanState:
		case T_IndexOnlyScanState:
			rel = ((ScanState *) ps)->ss_currentRelation;
			return rel != NULL ? (const void *) rel->rd_tableam : NULL;
		default:
			return NULL;
	}
}

static const char *
node_name(PlanState *ps)
{
	switch (nodeTag(ps))
	{
		case T_CustomScanState:
			return ((CustomScanState *) ps)->methods->CustomName;
		case T_SeqScanState:
			return "Seq Scan";
		case T_IndexScanState:
			return "Index Scan";
		case T_BitmapHeapScanState:
			return "Bitmap Heap Scan";
		default:
			return "node";
	}
}

/* Each reporter of the node's kind, as the probe calls them */
static void
report_node(PlanState *ps, int when)
{
	const PgOtelProbeReporterRegistry *reg = registry();
	const void *am = scan_am(ps);

	if (reg == NULL)
		return;
	for (int i = 0; i < reg->nnodes; i++)
	{
		const PgOtelProbeNodeReporterEntry *e = &reg->nodes[i];
		bool		matches;

		if (e->methods != NULL)
			matches = IsA(ps, CustomScanState) && ((CustomScanState *) ps)->methods == e->methods;
		else
			matches = am != NULL && e->table_am == am;
		if (!matches)
			continue;
		if (when == PG_OTEL_PROBE_REPORT_ABORT)
		{
			if (e->flags & PG_OTEL_PROBE_ABORT_SAFE)
				e->reporter(ps, &attrs, when);
			continue;
		}
		cur.first = nentries;
		PG_TRY();
		{
			e->reporter(ps, &attrs, when);
		}
		PG_CATCH();
		{
			/* its attributes dropped, its error a row: as the probe counts it */
			ErrorData  *edata;

			nentries = cur.first;
			edata = CopyErrorData();
			FlushErrorState();
			add_string(&attrs, "pg_otel_probe.reporter_error", edata->message);
		}
		PG_END_TRY();
	}
}

static bool
report_walker(PlanState *ps, void *context)
{
	int			when = *(int *) context;

	if (ps == NULL)
		return false;
	cur.node = ps->plan->plan_node_id + 1;
	cur.name = node_name(ps);
	cur.nattrs = 0;
	cur.bytes = 0;
	report_node(ps, when);
	return planstate_tree_walker(ps, report_walker, context);
}

/* The statement's reporters, then each node's */
static void
report_all(QueryDesc *queryDesc, int when)
{
	const PgOtelProbeReporterRegistry *reg = registry();

	if (reg == NULL)
		return;
	cur.node = 0;
	cur.name = "statement";
	cur.nattrs = 0;
	cur.bytes = 0;
	for (int i = 0; i < reg->nstatements; i++)
	{
		const PgOtelProbeStatementReporterEntry *e = &reg->statements[i];

		if (when == PG_OTEL_PROBE_REPORT_ABORT && !(e->flags & PG_OTEL_PROBE_ABORT_SAFE))
			continue;
		e->reporter(queryDesc, &attrs, when);
	}
	if (queryDesc->planstate != NULL)
		(void) report_walker(queryDesc->planstate, &when);
}

static bool
capturing(QueryDesc *queryDesc)
{
	return capture_query != NULL && !capture_done && queryDesc->sourceText != NULL &&
		strcmp(queryDesc->sourceText, capture_query) == 0;
}

/* ---- the hooks ---- */

static void
otel_executor_run(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	if (!capturing(queryDesc))
	{
		if (prev_executor_run)
			prev_executor_run(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
		return;
	}
	PG_TRY();
	{
		if (prev_executor_run)
			prev_executor_run(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
	}
	PG_CATCH();
	{
		/* the probe's error path: the abort-safe reporters, nothing allocated */
		report_all(queryDesc, PG_OTEL_PROBE_REPORT_ABORT);
		capture_done = true;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
otel_executor_end(QueryDesc *queryDesc)
{
	if (capturing(queryDesc))
	{
		report_all(queryDesc, PG_OTEL_PROBE_REPORT_END);
		capture_done = true;
	}
	if (prev_executor_end)
		prev_executor_end(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}

/* From _PG_init: the hooks, which do nothing but while otel_report() runs */
void
vexec_test_otel_init(void)
{
	prev_executor_run = ExecutorRun_hook;
	ExecutorRun_hook = otel_executor_run;
	prev_executor_end = ExecutorEnd_hook;
	ExecutorEnd_hook = otel_executor_end;
}

/* ---- SQL ---- */

/*
 * otel_report(query): the query run in a subtransaction, and the
 * attributes its statement's and its nodes' reporters gave -- node 0 the
 * statement, else its plan node's id + 1 -- then a row of node -2 with the
 * attributes dropped past a node's bounds, where any were, and a row of
 * node -1 with the error, where the query failed.
 */
Datum
vexec_test_otel_report(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *query = text_to_cstring(PG_GETARG_TEXT_PP(0));
	MemoryContext oldcxt;
	ResourceOwner oldowner = CurrentResourceOwner;
	char	   *error = NULL;
	Datum		values[4];
	bool		nulls[4] = {0};

	InitMaterializedSRF(fcinfo, 0);
	oldcxt = CurrentMemoryContext;
	nentries = 0;
	dropped = 0;
	capture_query = query;
	capture_done = false;
	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "vexec_test: SPI_connect failed");
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcxt);
	PG_TRY();
	{
		int			rc = SPI_execute(query, false, 0);

		if (rc < 0)
			elog(ERROR, "vexec_test: SPI_execute failed: %s", SPI_result_code_string(rc));
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcxt);
		CurrentResourceOwner = oldowner;
		error = edata->message;
	}
	PG_END_TRY();
	capture_query = NULL;
	SPI_finish();
	MemoryContextSwitchTo(oldcxt);

	for (int i = 0; i < nentries; i++)
	{
		values[0] = Int32GetDatum(area[i].node);
		values[1] = CStringGetTextDatum(area[i].name);
		values[2] = CStringGetTextDatum(area[i].key);
		values[3] = CStringGetTextDatum(area[i].value);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	if (dropped > 0)
	{
		values[0] = Int32GetDatum(-2);
		values[1] = CStringGetTextDatum("pg_otel_probe");
		values[2] = CStringGetTextDatum("dropped");
		values[3] = CStringGetTextDatum(psprintf(INT64_FORMAT, dropped));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	if (error != NULL)
	{
		values[0] = Int32GetDatum(-1);
		values[1] = CStringGetTextDatum("error");
		values[2] = CStringGetTextDatum("error");
		values[3] = CStringGetTextDatum(error);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	return (Datum) 0;
}

/* ---- metric reporters ---- */

PG_FUNCTION_INFO_V1(vexec_test_otel_metrics);

/* A reporter's points as the probe's collector takes them, into rows */
typedef struct MetricRows
{
	PgOtelProbeMetrics api;		/* first */
	Tuplestorestate *store;
	TupleDesc	desc;
	const PgOtelProbeMetricReporterEntry *entry;
	int			npoints;
	int			nattrs;
	int64		dropped;
	bool		open;
	const PgOtelProbeMetricDef *def;
	double		value;
	StringInfoData attrs;
} MetricRows;

static void
metric_close(MetricRows *r)
{
	Datum		values[6];
	bool		nulls[6] = {0};

	if (!r->open)
		return;
	r->open = false;
	appendStringInfoChar(&r->attrs, '}');
	values[0] = CStringGetTextDatum(r->entry->prefix);
	values[1] = CStringGetTextDatum(r->def->name);
	values[2] = CStringGetTextDatum(r->def->unit != NULL ? r->def->unit : "");
	values[3] = CStringGetTextDatum(r->def->kind == PG_OTEL_PROBE_COUNTER ? "counter" :
									r->def->kind == PG_OTEL_PROBE_UPDOWN ? "updown" : "gauge");
	values[4] = Float8GetDatum(r->value);
	values[5] = DirectFunctionCall1(jsonb_in, CStringGetDatum(r->attrs.data));
	tuplestore_putvalues(r->store, r->desc, values, nulls);
}

/* As the probe admits a point: a name under the reporter's prefix, a kind, its bound */
static bool
metric_point(MetricRows *r, const PgOtelProbeMetricDef *def, double value)
{
	size_t		len = strlen(r->entry->prefix);

	metric_close(r);
	if (def == NULL || def->name == NULL || r->npoints >= PG_OTEL_PROBE_METRIC_POINTS_MAX ||
		strncmp(def->name, r->entry->prefix, len) != 0 || def->name[len] != '.' ||
		(def->kind != PG_OTEL_PROBE_GAUGE && def->kind != PG_OTEL_PROBE_COUNTER &&
		 def->kind != PG_OTEL_PROBE_UPDOWN))
	{
		r->dropped++;
		return false;
	}
	r->npoints++;
	r->open = true;
	r->def = def;
	r->value = value;
	r->nattrs = 0;
	resetStringInfo(&r->attrs);
	appendStringInfoChar(&r->attrs, '{');
	return true;
}

static bool
metric_point_int(PgOtelProbeMetrics *m, const PgOtelProbeMetricDef *def, TimestampTz start, int64 value)
{
	return metric_point((MetricRows *) m, def, (double) value);
}

static bool
metric_point_double(PgOtelProbeMetrics *m, const PgOtelProbeMetricDef *def, TimestampTz start, double value)
{
	return metric_point((MetricRows *) m, def, value);
}

static void
metric_attr(PgOtelProbeMetrics *m, const char *key, const char *value)
{
	MetricRows *r = (MetricRows *) m;

	if (!r->open || key == NULL || value == NULL)
		return;
	if (r->nattrs >= PG_OTEL_PROBE_METRIC_ATTRS_MAX)
	{
		r->dropped++;
		return;
	}
	if (r->nattrs++ > 0)
		appendStringInfoString(&r->attrs, ", ");
	escape_json(&r->attrs, key);
	appendStringInfoString(&r->attrs, ": ");
	escape_json(&r->attrs, value);
}

/*
 * otel_metrics(): every metric reporter of pg_otel_probe's registry called
 * as the probe's collector calls it, each point a row -- the reporter's
 * prefix, the name, unit, kind, value and attributes -- and, where any were
 * dropped, a last row of the reporter "pg_otel_probe", name "dropped"
 */
Datum
vexec_test_otel_metrics(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	PgOtelProbeMetricRegistry **rv;
	const PgOtelProbeMetricRegistry *reg;
	MetricRows	r;

	InitMaterializedSRF(fcinfo, 0);
	memset(&r, 0, sizeof(r));
	r.api.point_int = metric_point_int;
	r.api.point_double = metric_point_double;
	r.api.attr = metric_attr;
	r.api.state = &r;
	r.store = rsinfo->setResult;
	r.desc = rsinfo->setDesc;
	initStringInfo(&r.attrs);
	rv = (PgOtelProbeMetricRegistry **) find_rendezvous_variable(PG_OTEL_PROBE_METRICS_RENDEZVOUS);
	reg = (*rv != NULL && (*rv)->magic == PG_OTEL_PROBE_METRICS_MAGIC) ? *rv : NULL;
	for (int i = 0; reg != NULL && i < reg->nreporters; i++)
	{
		r.entry = &reg->reporters[i];
		r.npoints = 0;
		r.entry->reporter(&r.api);
		metric_close(&r);
	}
	if (r.dropped > 0)
	{
		Datum		values[6];
		bool		nulls[6] = {0};

		values[0] = CStringGetTextDatum("pg_otel_probe");
		values[1] = CStringGetTextDatum("dropped");
		values[2] = CStringGetTextDatum("1");
		values[3] = CStringGetTextDatum("counter");
		values[4] = Float8GetDatum((double) r.dropped);
		values[5] = DirectFunctionCall1(jsonb_in, CStringGetDatum("{}"));
		tuplestore_putvalues(r.store, r.desc, values, nulls);
	}
	return (Datum) 0;
}
