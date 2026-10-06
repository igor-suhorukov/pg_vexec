/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * sql.c
 *	  Flight SQL's statements, as PostgreSQL's extended protocol runs them
 *	  (pg_vector_executor.md §3.15).
 *
 * A statement is parsed and analysed when the client asks for its
 * FlightInfo, or prepares it, into a saved CachedPlanSource, as
 * exec_parse_message() makes one (PG19:src/backend/tcop/postgres.c:1406);
 * its result's schema comes from the analysis.  It is planned when it runs,
 * through the plan cache, and runs in a portal, as exec_bind_message() and
 * exec_execute_message() run one (postgres.c:1640, 2122), with its result
 * going to vexec's receiver (vexec_egress.h).  So the planner's hooks --
 * vexec's own among them -- permissions, row-level security and
 * pg_stat_statements see it as they see a statement over PostgreSQL's
 * protocol.
 *
 * A query's FlightInfo keeps it in the session until its DoGet, which runs
 * it once; a ticket is good on the connection that asked for it, once.  A
 * statement that returns no rows, sent as a query, runs at its FlightInfo
 * instead, which then has no endpoint (rpc.c, command_info).  A prepared
 * statement stays until ClosePreparedStatement; DoPut binds its
 * parameters, a set a row, and a query runs once for each set, its results
 * one stream, an update once for each set, in one transaction.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/tstoreReceiver.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/analyze.h"
#include "tcop/pquery.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/snapmgr.h"
#include "utils/typcache.h"

#include "session.h"

/* ---------------------------------------------------------------------
 * The session's statements
 * ---------------------------------------------------------------------
 */

static HTAB *
statements(void)
{
	if (flight_session->statements == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(uint64);
		ctl.entrysize = sizeof(FlightStatement);
		ctl.hcxt = flight_session->mcxt;
		flight_session->statements = hash_create("vexec_flight statements", 64, &ctl,
												 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	return flight_session->statements;
}

/* A handle: the session's id, and the statement's. */
void
flight_statement_handle(FlightStatement *stmt, ProtobufCBinaryData *out)
{
	out->data = palloc(16);
	out->len = 16;
	memcpy(out->data, &flight_session->session_id, 8);
	memcpy(out->data + 8, &stmt->handle, 8);
}

FlightStatement *
flight_statement_find(const ProtobufCBinaryData *handle, bool prepared)
{
	uint64		session_id;
	uint64		h;
	FlightStatement *stmt;

	if (handle->len != 16)
		return NULL;
	memcpy(&session_id, handle->data, 8);
	memcpy(&h, handle->data + 8, 8);
	if (session_id != flight_session->session_id)
		return NULL;
	stmt = hash_search(statements(), &h, HASH_FIND, NULL);
	if (stmt == NULL || stmt->prepared != prepared)
		return NULL;
	return stmt;
}

void
flight_statement_drop(FlightStatement *stmt)
{
	uint64		h = stmt->handle;

	if (stmt->plansource)
		DropCachedPlan(stmt->plansource);
	MemoryContextDelete(stmt->mcxt);
	(void) hash_search(statements(), &h, HASH_REMOVE, NULL);
}

/* At most this many queries wait for their DoGet: the oldest go first. */
#define FLIGHT_MAX_PENDING	64

static void
forget_old_queries(void)
{
	HASH_SEQ_STATUS seq;
	FlightStatement *stmt;
	FlightStatement *oldest = NULL;
	int			n = 0;

	hash_seq_init(&seq, statements());
	while ((stmt = hash_seq_search(&seq)) != NULL)
		if (!stmt->prepared)
		{
			n++;
			if (oldest == NULL || stmt->handle < oldest->handle)
				oldest = stmt;
		}
	if (n >= FLIGHT_MAX_PENDING && oldest)
		flight_statement_drop(oldest);
}

/*
 * A statement, parsed and analysed into a saved plan source: one SQL
 * statement; a prepared one's parameter types inferred, as Parse infers
 * them (postgres.c:1406, exec_parse_message).
 */
FlightStatement *
flight_statement_create(const char *query, bool prepared)
{
	List	   *raw;
	RawStmt    *rs;
	bool		snapshot = false;
	CachedPlanSource *psrc;
	List	   *querytrees;
	Oid		   *paramtypes = NULL;
	int			nparams = 0;
	uint64		handle;
	FlightStatement *stmt;
	bool		found;
	MemoryContext mcxt;
	int			i;

	if (query == NULL)
		query = "";
	raw = pg_parse_query(query);
	if (list_length(raw) != 1)
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_SYNTAX_ERROR,
					 "a Flight SQL statement is one SQL statement, and this is %d", list_length(raw));
	rs = linitial_node(RawStmt, raw);
	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, commands ignored until end of transaction block")));
	if (!prepared)
		forget_old_queries();

	if (analyze_requires_snapshot(rs))
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot = true;
	}
	psrc = CreateCachedPlan(rs, query, CreateCommandTag(rs->stmt));
	if (prepared)
	{
		querytrees = pg_analyze_and_rewrite_varparams(rs, query, &paramtypes, &nparams, NULL);
		for (i = 0; i < nparams; i++)
			if (paramtypes[i] == InvalidOid || paramtypes[i] == UNKNOWNOID)
				ereport(ERROR,
						(errcode(ERRCODE_INDETERMINATE_DATATYPE),
						 errmsg("could not determine data type of parameter $%d", i + 1)));
	}
	else
		querytrees = pg_analyze_and_rewrite_fixedparams(rs, query, NULL, 0, NULL);
	if (snapshot)
		PopActiveSnapshot();
	CompleteCachedPlan(psrc, querytrees, NULL, paramtypes, nparams, NULL, NULL,
					   CURSOR_OPT_PARALLEL_OK, true);
	SaveCachedPlan(psrc);

	handle = ++flight_session->next_handle;
	stmt = hash_search(statements(), &handle, HASH_ENTER, &found);
	mcxt = AllocSetContextCreate(flight_session->mcxt, "vexec_flight statement", ALLOCSET_SMALL_SIZES);
	stmt->handle = handle;
	stmt->prepared = prepared;
	stmt->mcxt = mcxt;
	stmt->query = MemoryContextStrdup(mcxt, query);
	stmt->plansource = psrc;
	stmt->result = NULL;
	if (psrc->resultDesc)
	{
		MemoryContext old = MemoryContextSwitchTo(mcxt);

		stmt->result = CreateTupleDescCopy(psrc->resultDesc);
		MemoryContextSwitchTo(old);
	}
	stmt->nparams = nparams;
	stmt->paramtypes = MemoryContextAlloc(mcxt, sizeof(Oid) * Max(nparams, 1));
	if (nparams > 0)
		memcpy(stmt->paramtypes, paramtypes, sizeof(Oid) * nparams);
	stmt->paramsets = NIL;
	return stmt;
}

/* ---------------------------------------------------------------------
 * Schemas, with Flight SQL's column metadata
 * ---------------------------------------------------------------------
 */

/*
 * Flight SQL's metadata of a result's columns (arrow/format/FlightSql.proto,
 * CommandGetTables): the type's name, a numeric's precision and scale,
 * whether the column is case sensitive, and the table and its schema a
 * column comes from where the statement's target list says.
 */
static VexecEgressField *
result_fields(TupleDesc desc, List *tlist)
{
	VexecEgressField *fields = palloc0(sizeof(VexecEgressField) * Max(desc->natts, 1));
	ListCell   *lc = tlist ? list_head(tlist) : NULL;
	int			i;

	for (i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		const char **keys = palloc(sizeof(char *) * 8);
		const char **values = palloc(sizeof(char *) * 8);
		int			n = 0;
		TargetEntry *tle = NULL;

		while (lc != NULL)
		{
			tle = lfirst_node(TargetEntry, lc);
			lc = lnext(tlist, lc);
			if (!tle->resjunk)
				break;
			tle = NULL;
		}
		keys[n] = "ARROW:FLIGHT:SQL:TYPE_NAME";
		values[n++] = format_type_be(att->atttypid);
		if (getBaseType(att->atttypid) == NUMERICOID && att->atttypmod >= (int32) VARHDRSZ)
		{
			keys[n] = "ARROW:FLIGHT:SQL:PRECISION";
			values[n++] = psprintf("%d", ((att->atttypmod - VARHDRSZ) >> 16) & 0xffff);
			keys[n] = "ARROW:FLIGHT:SQL:SCALE";
			values[n++] = psprintf("%d", (((att->atttypmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024);
		}
		keys[n] = "ARROW:FLIGHT:SQL:IS_CASE_SENSITIVE";
		values[n++] = OidIsValid(att->attcollation) ? "1" : "0";
		if (tle && OidIsValid(tle->resorigtbl))
		{
			char	   *rel = get_rel_name(tle->resorigtbl);
			Oid			nsp = get_rel_namespace(tle->resorigtbl);

			if (rel)
			{
				keys[n] = "ARROW:FLIGHT:SQL:CATALOG_NAME";
				values[n++] = get_database_name(MyDatabaseId);
				keys[n] = "ARROW:FLIGHT:SQL:DB_SCHEMA_NAME";
				values[n++] = get_namespace_name(nsp);
				keys[n] = "ARROW:FLIGHT:SQL:TABLE_NAME";
				values[n++] = rel;
			}
		}
		fields[i].nmetadata = n;
		fields[i].keys = keys;
		fields[i].values = values;
	}
	return fields;
}

/* A table's or a result's columns, with Flight SQL's metadata and no origin. */
VexecEgressField *
flight_sql_fields(TupleDesc desc)
{
	return result_fields(desc, NIL);
}

/* The statement's result schema: empty for a statement that returns no rows. */
void
flight_statement_schema(FlightStatement *stmt, StringInfo out)
{
	TupleDesc	desc = stmt->result ? stmt->result : CreateTemplateTupleDesc(0);
	List	   *tlist = stmt->result ? CachedPlanGetTargetList(stmt->plansource, NULL) : NIL;

	flight_schema_bytes(desc, result_fields(desc, tlist), desc->natts, out);
}

/* The parameters' schema: a field a parameter, "$1", "$2" ... */
void
flight_statement_parameter_schema(FlightStatement *stmt, StringInfo out)
{
	TupleDesc	desc = CreateTemplateTupleDesc(stmt->nparams);
	int			i;

	for (i = 0; i < stmt->nparams; i++)
		TupleDescInitEntry(desc, (AttrNumber) (i + 1), psprintf("$%d", i + 1),
						   stmt->paramtypes[i], -1, 0);
	flight_schema_bytes(desc, flight_sql_fields(desc), desc->natts, out);
}

/* ---------------------------------------------------------------------
 * Running
 * ---------------------------------------------------------------------
 */

/* A transaction id, as the statement names it, against the session's. */
void
flight_check_transaction(bool has_id, const ProtobufCBinaryData *id)
{
	if (has_id)
	{
		if (!flight_session->in_transaction || id->len != sizeof(flight_session->transaction_id) ||
			memcmp(id->data, flight_session->transaction_id, id->len) != 0)
			flight_error(GRPC_NOT_FOUND, ERRCODE_INVALID_TRANSACTION_STATE,
						 "no such transaction in this session");
	}
	else if (flight_session->in_transaction)
		flight_error(GRPC_FAILED_PRECONDITION, ERRCODE_ACTIVE_SQL_TRANSACTION,
					 "a Flight transaction is open in this session: the statement must name it");
}

static void
refuse_if_cancelled(FlightStatement *stmt)
{
	bool		cancelled;

	SpinLockAcquire(&flight_shared->mutex);
	cancelled = flight_shared->slots[flight_session->slot].cancelled == stmt->handle;
	if (cancelled)
		flight_shared->slots[flight_session->slot].cancelled = 0;
	SpinLockRelease(&flight_shared->mutex);
	if (cancelled)
		flight_error(GRPC_CANCELLED, ERRCODE_QUERY_CANCELED,
					 "canceling statement due to CancelFlightInfo");
}

/* One execution of a statement, with one set of parameters, into a receiver. */
static uint64
run_once(FlightStatement *stmt, ParamListInfo params, DestReceiver *dest)
{
	CachedPlan *cplan;
	Portal		portal;
	QueryCompletion qc;
	bool		snapshot = false;

	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, commands ignored until end of transaction block")));
	if (stmt->nparams > 0 || analyze_requires_snapshot(stmt->plansource->raw_parse_tree))
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		snapshot = true;
	}
	cplan = GetCachedPlan(stmt->plansource, params, NULL, NULL);
	portal = CreatePortal("", true, true);
	portal->visible = false;
	PortalDefineQuery(portal, NULL, stmt->query, stmt->plansource->commandTag,
					  cplan->stmt_list, cplan);
	PortalStart(portal, params, 0, InvalidSnapshot);
	if (snapshot)
		PopActiveSnapshot();
	InitializeQueryCompletion(&qc);
	(void) PortalRun(portal, FETCH_ALL, true, dest, dest, &qc);
	PortalDrop(portal, false);
	return command_tag_display_rowcount(qc.commandTag) ? qc.nprocessed : PG_UINT64_MAX;
}

/* What a call's egress writes: counted, so that a stream with no rows still has its schema. */
typedef struct CountingWrite
{
	FlightCall *call;
	int64		messages;
} CountingWrite;

static void
counting_write(void *arg, const VexecEgressMessage *msg)
{
	CountingWrite *cw = arg;

	cw->messages++;
	flight_egress_write(cw->call, msg);
}

/*
 * A statement's result to the client, once for each bound set of
 * parameters -- once when it has none -- as one stream whose schema the
 * first execution sends.
 */
void
flight_statement_run(FlightCall *call, FlightStatement *stmt)
{
	CountingWrite cw = {call, 0};
	DestReceiver *dest;
	VexecEgressField *fields = NULL;
	int			nfields = 0;
	ListCell   *lc;

	if (stmt->nparams > 0 && stmt->paramsets == NIL)
		flight_error(GRPC_FAILED_PRECONDITION, ERRCODE_UNDEFINED_PARAMETER,
					 "the prepared statement has %d parameters, and none is bound: bind them with DoPut",
					 stmt->nparams);
	refuse_if_cancelled(stmt);
	if (stmt->result)
	{
		fields = result_fields(stmt->result, CachedPlanGetTargetList(stmt->plansource, NULL));
		nfields = stmt->result->natts;
	}
	flight_statement_begins(stmt->query, stmt->handle);
	dest = flight_session->egress->receiver(counting_write, &cw, fields, nfields);
	if (stmt->paramsets == NIL)
		(void) run_once(stmt, NULL, dest);
	else
		foreach(lc, stmt->paramsets)
		{
			(void) run_once(stmt, lfirst(lc), dest);
			CommandCounterIncrement();
		}
	dest->rDestroy(dest);
	if (cw.messages == 0)
	{
		/* a statement that returns no rows: its stream's schema, empty */
		flight_session->egress->schema(CreateTemplateTupleDesc(0), NULL, 0, counting_write, &cw);
	}
	flight_statement_ends();
}

/*
 * A statement for its row count: once for each bound set of parameters,
 * all in one transaction.  -1 when the statement counts no rows, as Flight
 * SQL has it (DoPutUpdateResult).
 */
int64
flight_statement_update(FlightStatement *stmt)
{
	uint64		total = 0;
	bool		counted = false;
	ListCell   *lc;

	if (stmt->nparams > 0 && stmt->paramsets == NIL)
		flight_error(GRPC_FAILED_PRECONDITION, ERRCODE_UNDEFINED_PARAMETER,
					 "the prepared statement has %d parameters, and none is bound", stmt->nparams);
	refuse_if_cancelled(stmt);
	flight_statement_begins(stmt->query, stmt->handle);
	if (stmt->paramsets == NIL)
	{
		uint64		n = run_once(stmt, NULL, None_Receiver);

		counted = n != PG_UINT64_MAX;
		total = counted ? n : 0;
	}
	else
		foreach(lc, stmt->paramsets)
		{
			uint64		n = run_once(stmt, lfirst(lc), None_Receiver);

			if (n != PG_UINT64_MAX)
			{
				counted = true;
				total += n;
			}
			CommandCounterIncrement();
		}
	flight_statement_ends();
	return counted ? (int64) total : -1;
}

/* A transaction command -- BEGIN, COMMIT, SAVEPOINT ... -- as a statement of its own. */
void
flight_sql_command(const char *sql)
{
	List	   *raw = pg_parse_query(sql);
	RawStmt    *rs = linitial_node(RawStmt, raw);
	List	   *plans;
	Portal		portal;
	QueryCompletion qc;

	flight_start_xact();
	if (IsAbortedTransactionBlockState() && !IsA(rs->stmt, TransactionStmt))
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, commands ignored until end of transaction block")));
	flight_statement_begins(sql, 0);
	plans = pg_plan_queries(pg_analyze_and_rewrite_fixedparams(rs, sql, NULL, 0, NULL),
							sql, CURSOR_OPT_PARALLEL_OK, NULL);
	portal = CreatePortal("", true, true);
	portal->visible = false;
	PortalDefineQuery(portal, NULL, sql, CreateCommandTag(rs->stmt), plans, NULL);
	PortalStart(portal, NULL, 0, InvalidSnapshot);
	InitializeQueryCompletion(&qc);
	(void) PortalRun(portal, FETCH_ALL, true, None_Receiver, None_Receiver, &qc);
	PortalDrop(portal, false);
	flight_statement_ends();
	flight_finish_xact();
}

/*
 * SQL of the endpoint's own -- a catalog command's -- with parameters, its
 * result to the client through vexec's receiver, wrapped when the command
 * adds a column (catalog.c).
 */
void
flight_run_sql(FlightCall *call, const char *sql, int nargs, const Oid *argtypes,
			   const Datum *args, const bool *nulls,
			   const VexecEgressField *fields, int nfields,
			   DestReceiver *(*wrap) (DestReceiver *egress, void *arg), void *wrap_arg)
{
	List	   *raw = pg_parse_query(sql);
	RawStmt    *rs = linitial_node(RawStmt, raw);
	List	   *plans;
	Portal		portal;
	QueryCompletion qc;
	ParamListInfo params = NULL;
	CountingWrite cw = {call, 0};
	DestReceiver *egress;
	DestReceiver *dest;
	int			i;

	if (IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, commands ignored until end of transaction block")));
	if (nargs > 0)
	{
		params = makeParamList(nargs);
		for (i = 0; i < nargs; i++)
		{
			params->params[i].value = args[i];
			params->params[i].isnull = nulls ? nulls[i] : false;
			params->params[i].pflags = PARAM_FLAG_CONST;
			params->params[i].ptype = argtypes[i];
		}
	}
	flight_statement_begins(sql, 0);
	PushActiveSnapshot(GetTransactionSnapshot());
	plans = pg_plan_queries(pg_analyze_and_rewrite_fixedparams(rs, sql, argtypes, nargs, NULL),
							sql, CURSOR_OPT_PARALLEL_OK, params);
	PopActiveSnapshot();
	portal = CreatePortal("", true, true);
	portal->visible = false;
	PortalDefineQuery(portal, NULL, sql, CreateCommandTag(rs->stmt), plans, NULL);
	PortalStart(portal, params, 0, InvalidSnapshot);
	egress = flight_session->egress->receiver(counting_write, &cw, fields, nfields);
	dest = wrap ? wrap(egress, wrap_arg) : egress;
	InitializeQueryCompletion(&qc);
	(void) PortalRun(portal, FETCH_ALL, true, dest, dest, &qc);
	PortalDrop(portal, false);
	if (dest != egress)
		dest->rDestroy(dest);
	egress->rDestroy(egress);
	flight_statement_ends();
}

/* ---------------------------------------------------------------------
 * Binding parameters
 * ---------------------------------------------------------------------
 */

typedef struct BindState
{
	FlightStatement *stmt;
	MemoryContext mcxt;
	List	   *sets;
} BindState;

static void
bind_row(void *arg, int64 rownum, const Datum *values, const bool *isnull)
{
	BindState  *bs = arg;
	FlightStatement *stmt = bs->stmt;
	MemoryContext old = MemoryContextSwitchTo(bs->mcxt);
	ParamListInfo params = makeParamList(stmt->nparams);
	int			i;

	(void) rownum;
	for (i = 0; i < stmt->nparams; i++)
	{
		int16		typlen;
		bool		typbyval;

		get_typlenbyval(stmt->paramtypes[i], &typlen, &typbyval);
		params->params[i].isnull = isnull[i];
		params->params[i].value = isnull[i] ? (Datum) 0 : datumCopy(values[i], typbyval, typlen);
		params->params[i].pflags = PARAM_FLAG_CONST;
		params->params[i].ptype = stmt->paramtypes[i];
	}
	bs->sets = lappend(bs->sets, params);
	MemoryContextSwitchTo(old);
}

/*
 * A prepared statement's parameters from DoPut's stream: the first
 * FlightData's schema -- or the next one's, when the first carries the
 * descriptor alone -- then a set of parameters a row of each batch.  They
 * replace what an earlier DoPut bound.
 */
void
flight_statement_bind(FlightCall *call, FlightStatement *stmt, const ProtobufCBinaryData *schema_header)
{
	const VexecEgressRoutine *eg = flight_session->egress;
	BindState	bs;
	void	   *state = NULL;
	StringInfoData msg;
	MemoryContext setcxt;

	setcxt = AllocSetContextCreate(stmt->mcxt, "vexec_flight parameters", ALLOCSET_DEFAULT_SIZES);
	bs.stmt = stmt;
	bs.mcxt = setcxt;
	bs.sets = NIL;
	if (schema_header->len > 0)
		state = eg->params_begin((const char *) schema_header->data, schema_header->len,
								 stmt->nparams, stmt->paramtypes, NULL);
	initStringInfo(&msg);
	while (h2_call_read(call, &msg))
	{
		Arrow__Flight__Protocol__FlightData *fd =
			(Arrow__Flight__Protocol__FlightData *) protobuf_c_message_unpack(&arrow__flight__protocol__flight_data__descriptor,
									  flight_pb_allocator(), msg.len, (uint8_t *) msg.data);

		if (fd == NULL)
			flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "a malformed FlightData");
		if (fd->data_header.len == 0)
			continue;			/* app_metadata alone */
		if (state == NULL)
		{
			state = eg->params_begin((const char *) fd->data_header.data, fd->data_header.len,
									 stmt->nparams, stmt->paramtypes, NULL);
			continue;
		}
		if (eg->message_kind((const char *) fd->data_header.data, fd->data_header.len) !=
			VEXEC_EGRESS_RECORD_BATCH)
			flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_FEATURE_NOT_SUPPORTED,
						 "a parameter stream holds record batches only");
		(void) eg->params_batch(state, (const char *) fd->data_header.data, fd->data_header.len,
								(const char *) fd->data_body.data, fd->data_body.len, bind_row, &bs);
	}
	if (state)
		eg->params_end(state);
	if (stmt->paramsets != NIL)
		MemoryContextDelete(GetMemoryChunkContext(linitial(stmt->paramsets)));
	stmt->paramsets = bs.sets;
	if (bs.sets == NIL)
		MemoryContextDelete(setcxt);
}
