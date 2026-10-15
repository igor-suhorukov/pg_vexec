/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * pg_otel_probe_api.h
 *	  What other extensions use of pg_otel_probe (PLAN.md §4.7.4): reporters,
 *	  which add the counters only a node's or a statement's own extension
 *	  knows to its span, and the trace context of a request an extension
 *	  receives or sends on a statement's behalf.
 *
 * Neither side links the other.  Both rendezvous variables are reached by
 * name, and everything here is a type, a macro or an inline function, so a
 * module that includes this header needs nothing of pg_otel_probe's at load
 * time, and there is no order between the modules in
 * "shared_preload_libraries":
 *
 *	pg_otel_probe/node_reporter_v1	a registry of reporters, made by
 *		whichever module comes first while the postmaster loads libraries,
 *		as vexec_source.h's is.  A registration without the probe is an
 *		unused entry.
 *	pg_otel_probe/context_v1	the probe's own functions, published by its
 *		_PG_init and looked up when called: NULL where the probe is not
 *		loaded.
 *
 * Versions.  The major version is in the rendezvous names: a change an older
 * module cannot read makes a new name, and the old one stays empty.  Minor
 * additions go at the end of a struct whose "size" says how much of it the
 * module that filled it was built with.
 *
 * The port keeps this copy of pg_otel_probe's include/pg_otel_probe_api.h
 * (Apache-2.0) -- the same declarations, with this paragraph -- so that its
 * modules report to the probe where it is loaded, with no build dependency
 * on it (gp_core's Motions, gp_orca's statements, PAX's and gp_ao's scans).
 *
 * pg_otel_probe installs this header into the server's include directory
 * under extension/pg_otel_probe/, so that a module built elsewhere can use
 * it; a module may as well keep a copy of its own.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_OTEL_PROBE_API_H
#define PG_OTEL_PROBE_API_H

#include "fmgr.h"
#include "miscadmin.h"
#include "executor/execdesc.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "utils/memutils.h"

#define PG_OTEL_PROBE_REPORTER_RENDEZVOUS	"pg_otel_probe/node_reporter_v1"
#define PG_OTEL_PROBE_CONTEXT_RENDEZVOUS	"pg_otel_probe/context_v1"

/* ---- reporters ---- */

/*
 * What a reporter adds its attributes through, at the call: typed
 * key/values, which become OTLP attributes of the node's or the statement's
 * span, and the Parquet column extension_attributes.  A key is the
 * extension's own, under its prefix -- "vexec.batches", "cloudberry.motion.
 * bytes_sent" -- and is copied, as a string value is.  At most
 * PG_OTEL_PROBE_ATTRS_MAX of them a node, in PG_OTEL_PROBE_ATTRS_BYTES of
 * keys and strings; what does not fit is dropped and counted.  The calls
 * allocate nothing and never raise.
 */
#define PG_OTEL_PROBE_ATTRS_MAX		32
#define PG_OTEL_PROBE_ATTRS_BYTES	4096

typedef struct PgOtelProbeAttrs PgOtelProbeAttrs;

struct PgOtelProbeAttrs
{
	void		(*add_int) (PgOtelProbeAttrs *attrs, const char *key, int64 value);
	void		(*add_double) (PgOtelProbeAttrs *attrs, const char *key, double value);
	void		(*add_string) (PgOtelProbeAttrs *attrs, const char *key, const char *value);
	void		(*add_bool) (PgOtelProbeAttrs *attrs, const char *key, bool value);
	void	   *state;			/* the probe's */
};

/*
 * When a reporter is called: as the statement's executor ends, before its
 * nodes are freed (ExecutorEnd), or on the error path of a statement that
 * failed -- the nodes as the error left them, inside the error's handling,
 * so only a reporter registered PG_OTEL_PROBE_ABORT_SAFE is called there,
 * and it must neither allocate nor raise.
 */
#define PG_OTEL_PROBE_REPORT_END	0
#define PG_OTEL_PROBE_REPORT_ABORT	1

/* A registration's flags. */
#define PG_OTEL_PROBE_ABORT_SAFE	0x0001

/*
 * A node's reporter: "ps" is a node of the kind it registered for, a
 * CustomScanState of its CustomExecMethods, or a scan of a relation of its
 * table access method.  It reads the node and calls attrs' functions.  An
 * error it raises drops what it added, is counted, and never reaches the
 * statement; it runs under PG_TRY for that, not in a subtransaction, so it
 * takes no locks and changes nothing.
 */
typedef void (*PgOtelProbeNodeReporter) (PlanState *ps, PgOtelProbeAttrs *attrs, int when);

/* A statement's reporter, with its QueryDesc: the statement's own figures. */
typedef void (*PgOtelProbeStatementReporter) (QueryDesc *queryDesc, PgOtelProbeAttrs *attrs,
											  int when);

#define PG_OTEL_PROBE_REPORTER_MAGIC	0x504F5231	/* "POR1" */
#define PG_OTEL_PROBE_REPORTERS_MAX		64

typedef struct PgOtelProbeNodeReporterEntry
{
	const CustomExecMethods *methods;	/* a custom node's, or NULL */
	const void *table_am;		/* a TableAmRoutine's, or NULL */
	PgOtelProbeNodeReporter reporter;
	int			flags;
} PgOtelProbeNodeReporterEntry;

typedef struct PgOtelProbeStatementReporterEntry
{
	PgOtelProbeStatementReporter reporter;
	int			flags;
} PgOtelProbeStatementReporterEntry;

/*
 * The registry the rendezvous variable points at.  Whichever module comes
 * first while the postmaster loads libraries makes it; it is never freed,
 * and every process inherits it.  Its layout is fixed for the major version.
 */
typedef struct PgOtelProbeReporterRegistry
{
	uint32		magic;
	int			nnodes;
	PgOtelProbeNodeReporterEntry nodes[PG_OTEL_PROBE_REPORTERS_MAX];
	int			nstatements;
	PgOtelProbeStatementReporterEntry statements[PG_OTEL_PROBE_REPORTERS_MAX];
} PgOtelProbeReporterRegistry;

/* The registry, made if no module has made it yet. */
static inline PgOtelProbeReporterRegistry *
pg_otel_probe_reporter_registry(void)
{
	PgOtelProbeReporterRegistry **rv;

	rv = (PgOtelProbeReporterRegistry **) find_rendezvous_variable(PG_OTEL_PROBE_REPORTER_RENDEZVOUS);
	if (*rv == NULL)
	{
		PgOtelProbeReporterRegistry *reg;

		reg = (PgOtelProbeReporterRegistry *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(PgOtelProbeReporterRegistry));
		reg->magic = PG_OTEL_PROBE_REPORTER_MAGIC;
		*rv = reg;
	}
	else if ((*rv)->magic != PG_OTEL_PROBE_REPORTER_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("the rendezvous variable \"%s\" does not hold pg_otel_probe's reporter registry",
						PG_OTEL_PROBE_REPORTER_RENDEZVOUS)));
	return *rv;
}

static inline void
pg_otel_probe_register_check(void)
{
	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a pg_otel_probe reporter can only be registered while \"shared_preload_libraries\" are loaded")));
}

static inline void
pg_otel_probe_add_node_entry(const CustomExecMethods *methods, const void *table_am,
							 PgOtelProbeNodeReporter reporter, int flags)
{
	PgOtelProbeReporterRegistry *reg;

	pg_otel_probe_register_check();
	reg = pg_otel_probe_reporter_registry();
	if (reg->nnodes >= PG_OTEL_PROBE_REPORTERS_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many pg_otel_probe node reporters"),
				 errdetail("At most %d can be registered.", PG_OTEL_PROBE_REPORTERS_MAX)));
	reg->nodes[reg->nnodes].methods = methods;
	reg->nodes[reg->nnodes].table_am = table_am;
	reg->nodes[reg->nnodes].reporter = reporter;
	reg->nodes[reg->nnodes].flags = flags;
	reg->nnodes++;
}

/*
 * A reporter of a custom node: every CustomScanState whose methods are
 * these.  From the module's _PG_init; the methods and the function must
 * stay valid for the life of the process.  Several reporters of one kind of
 * node are each called, in the order they registered.
 */
static inline void
pg_otel_probe_register_node_reporter(const CustomExecMethods *methods,
									 PgOtelProbeNodeReporter reporter, int flags)
{
	if (methods == NULL || reporter == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a pg_otel_probe node reporter needs its node's methods and a function")));
	pg_otel_probe_add_node_entry(methods, NULL, reporter, flags);
}

/*
 * A reporter of scans of a table access method's relations -- a Seq Scan or
 * a Bitmap Heap Scan of a PAX table -- given the scan node; its
 * ss_currentScanDesc is the access method's own.  An addition to §4.7.4's
 * API: storage is not a custom node.
 */
static inline void
pg_otel_probe_register_scan_reporter(const void *table_am,
									 PgOtelProbeNodeReporter reporter, int flags)
{
	if (table_am == NULL || reporter == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a pg_otel_probe scan reporter needs its access method and a function")));
	pg_otel_probe_add_node_entry(NULL, table_am, reporter, flags);
}

/* A reporter of every statement the probe traces. */
static inline void
pg_otel_probe_register_statement_reporter(PgOtelProbeStatementReporter reporter, int flags)
{
	PgOtelProbeReporterRegistry *reg;

	pg_otel_probe_register_check();
	if (reporter == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a pg_otel_probe statement reporter needs a function")));
	reg = pg_otel_probe_reporter_registry();
	if (reg->nstatements >= PG_OTEL_PROBE_REPORTERS_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many pg_otel_probe statement reporters"),
				 errdetail("At most %d can be registered.", PG_OTEL_PROBE_REPORTERS_MAX)));
	reg->statements[reg->nstatements].reporter = reporter;
	reg->statements[reg->nstatements].flags = flags;
	reg->nstatements++;
}

/* ---- trace context ---- */

/* A traceparent's length, without its NUL: "00-" 32 "-" 16 "-" 2. */
#define PG_OTEL_PROBE_TRACEPARENT_LEN	55
#define PG_OTEL_PROBE_TRACESTATE_MAX	512

typedef struct PgOtelProbeContextApi
{
	int			version;		/* 1 */
	int			size;			/* sizeof what the probe filled */

	/*
	 * A request this extension received names its caller's span: a W3C
	 * traceparent and tracestate (NULL or "" for none), as the
	 * pg_otel_probe.trace_context setting takes them (§4.3.3) -- for the
	 * transaction with is_local, else for the session.  The next statement
	 * becomes a child of that span.  False, and nothing set, where the
	 * value is malformed or the probe is off; it never raises.
	 */
	bool		(*set_parent) (const char *traceparent, const char *tracestate, bool is_local);

	/*
	 * The context to put on a request this extension sends on the running
	 * statement's behalf: its traceparent into a buffer of at least
	 * PG_OTEL_PROBE_TRACEPARENT_LEN + 1 bytes, and its tracestate, cut to
	 * state_size.  False where no span runs.
	 */
	bool		(*outgoing) (char *traceparent, char *tracestate, size_t state_size);
} PgOtelProbeContextApi;

/* The probe's context functions, or NULL where it is not loaded. */
static inline const PgOtelProbeContextApi *
pg_otel_probe_context_api(void)
{
	const PgOtelProbeContextApi **rv;

	rv = (const PgOtelProbeContextApi **) find_rendezvous_variable(PG_OTEL_PROBE_CONTEXT_RENDEZVOUS);
	if (*rv == NULL || (*rv)->version != 1)
		return NULL;
	return *rv;
}

#endif							/* PG_OTEL_PROBE_API_H */
