/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * ingest.c
 *	  Flight SQL's ingest -- CommandStatementIngest -- and a prepared
 *	  INSERT's parameter batches, as one INSERT ... SELECT over vexec's
 *	  ingest stream (pg_vector_executor.md §3.15, §3.16; vexec_egress.h's
 *	  minor version 1).
 *
 * The client's FlightData go to vexec as they arrive: the stream's read
 * function takes the next message off the call (h2_call_read()) while the
 * INSERT runs, and its data_header and data_body are read where they lie --
 * not through protobuf-c, whose copies would live until the call ends -- so
 * that a load holds one message at a time.  vexec writes the rows a batch of
 * columns at a time where the table allows it (VecInsert), and the rows are
 * committed, outside a Flight transaction, only once the stream has ended
 * as the client ended it: a call the client cancelled or lost fails, and
 * loads nothing.
 *
 *	CommandStatementIngest	the table is found, created, replaced or appended
 *							to, as table_definition_options say, in the
 *							statement's transaction; the stream's columns go
 *							into the table's columns of the same names.  A
 *							column whose Arrow type names a type the table's
 *							column cannot be assigned from -- utf8 for a date,
 *							say -- is read by that column type's input
 *							function, as a parameter in text format is.
 *	a prepared INSERT		"INSERT INTO t [(columns)] VALUES ($1, ...)", each
 *							value a parameter or DEFAULT, each parameter once,
 *							into a table with no trigger and no rule: its
 *							parameter batches go in as one INSERT ... SELECT,
 *							where sql.c would run the statement once a row.  A
 *							statement-level trigger, which would fire once
 *							rather than once a row, keeps the row path.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_coerce.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "flight.h"
#include "h2.h"
#include "session.h"

#include "FlightSql.pb-c.h"

/* A FlightData's fields (Flight.proto:532-557), where they lie. */
typedef struct FlightDataView
{
	const char *header;			/* data_header (2) */
	size_t		header_len;
	const char *body;			/* data_body (1000) */
	size_t		body_len;
} FlightDataView;

/* What a stream's read function keeps between calls. */
typedef struct IngestReader
{
	FlightCall *call;
	StringInfoData msg;			/* the message vexec reads now */
	bool		ended;			/* the call has no more messages */
	int64		messages;
} IngestReader;

static bool
pb_varint(const uint8 **p, const uint8 *end, uint64 *v)
{
	int			shift = 0;

	*v = 0;
	while (*p < end && shift < 64)
	{
		uint8		b = *(*p)++;

		*v |= (uint64) (b & 0x7f) << shift;
		if (!(b & 0x80))
			return true;
		shift += 7;
	}
	return false;
}

/* A FlightData message's header and body, read where they lie in data. */
static void
flight_data_view(const char *data, size_t len, FlightDataView *v)
{
	const uint8 *p = (const uint8 *) data;
	const uint8 *end = p + len;

	memset(v, 0, sizeof(FlightDataView));
	while (p < end)
	{
		uint64		key;
		uint64		n;

		if (!pb_varint(&p, end, &key))
			goto malformed;
		switch (key & 7)
		{
			case 0:
				if (!pb_varint(&p, end, &n))
					goto malformed;
				continue;
			case 1:
				if (end - p < 8)
					goto malformed;
				p += 8;
				continue;
			case 5:
				if (end - p < 4)
					goto malformed;
				p += 4;
				continue;
			case 2:
				break;
			default:
				goto malformed;
		}
		if (!pb_varint(&p, end, &n) || n > (uint64) (end - p))
			goto malformed;
		if ((key >> 3) == 2)
		{
			v->header = (const char *) p;
			v->header_len = n;
		}
		else if ((key >> 3) == 1000)
		{
			v->body = (const char *) p;
			v->body_len = n;
		}
		p += n;
	}
	return;

malformed:
	flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "a malformed FlightData");
}

/* vexec's read function: the call's next FlightData with an IPC message. */
static bool
ingest_read(void *arg, const char **metadata, size_t *metadata_len,
			const char **body, size_t *body_len)
{
	IngestReader *r = arg;
	FlightDataView v;

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (r->ended || !h2_call_read(r->call, &r->msg))
		{
			r->ended = true;
			return false;
		}
		flight_data_view(r->msg.data, r->msg.len, &v);
		if (v.header_len == 0)
			continue;			/* app_metadata alone */
		r->messages++;
		*metadata = v.header;
		*metadata_len = v.header_len;
		*body = v.body;
		*body_len = v.body_len;
		return true;
	}
}

/*
 * The stream's Schema message: the first FlightData's data_header, or the
 * next message's where the first carried the descriptor alone.
 */
static void
ingest_schema(IngestReader *r, const ProtobufCBinaryData *first_header,
			  const char **schema, size_t *schema_len)
{
	const char *body;
	size_t		body_len;

	if (first_header != NULL && first_header->len > 0)
	{
		*schema = (const char *) first_header->data;
		*schema_len = first_header->len;
		return;
	}
	if (!ingest_read(r, schema, schema_len, &body, &body_len))
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION,
					 "the stream has no Schema message");
}

/* vexec's ingest, from this session's routine, or an error. */
static const VexecEgressRoutine *
ingest_routine(void)
{
	const VexecEgressRoutine *eg = flight_session->egress;

	if (!VEXEC_EGRESS_HAS(eg, ingest_end))
		flight_error(GRPC_UNIMPLEMENTED, ERRCODE_FEATURE_NOT_SUPPORTED,
					 "bulk ingestion needs vexec's ingest (its egress API at minor version 1)");
	if (!OidIsValid(get_namespace_oid("vexec", true)))
		flight_error(GRPC_FAILED_PRECONDITION, ERRCODE_UNDEFINED_FUNCTION,
					 "bulk ingestion needs vexec's extension in this database: CREATE EXTENSION vexec");
	return eg;
}

/* A statement run in the current transaction, its row count returned. */
static int64
ingest_run(const char *sql)
{
	FlightStatement *stmt = flight_statement_create(sql, false);
	int64		n = flight_statement_update(stmt);

	flight_statement_drop(stmt);
	CommandCounterIncrement();
	return n;
}

/*
 * The type a stream's column is read as, to go into a column of type
 * target: the type its Arrow type names, where it can be assigned to the
 * column, or the column's own for utf8 -- read by its input function.
 */
static char *
read_as(const VexecIngestColumn *c, Oid target, int32 target_typmod)
{
	Oid			natural = c->type;

	if (natural != target && natural == TEXTOID &&
		!can_coerce_type(1, &natural, &target, COERCION_ASSIGNMENT))
		return format_type_with_typemod(target, target_typmod);
	return format_type_with_typemod(natural, c->typmod);
}

/* The rows went in: the stream ended as the client ended it, or nothing did. */
static void
ingest_finish(FlightCall *call, const VexecEgressRoutine *eg, void *stream, IngestReader *r)
{
	if (!eg->ingest_finished(stream))
		flight_error(GRPC_INTERNAL, ERRCODE_INTERNAL_ERROR,
					 "the INSERT did not read the client's stream to its end");
	if (h2_call_gone(call))
		flight_error(GRPC_CANCELLED, ERRCODE_QUERY_CANCELED,
					 "the client ended the call before its stream's end; nothing was loaded");
	eg->ingest_end(stream);
	(void) r;
}

static void
put_update_result(FlightCall *call, int64 count)
{
	Arrow__Flight__Protocol__Sql__DoPutUpdateResult res =
		ARROW__FLIGHT__PROTOCOL__SQL__DO_PUT_UPDATE_RESULT__INIT;
	Arrow__Flight__Protocol__PutResult put = ARROW__FLIGHT__PROTOCOL__PUT_RESULT__INIT;
	ProtobufCBinaryData bytes;

	res.record_count = count;
	bytes.len = protobuf_c_message_get_packed_size(&res.base);
	bytes.data = palloc(Max(bytes.len, 1));
	protobuf_c_message_pack(&res.base, bytes.data);
	put.app_metadata = bytes;
	flight_send_message(call, &put.base);
}

/* ---------------------------------------------------------------------
 * CommandStatementIngest
 * ---------------------------------------------------------------------
 */

void
flight_ingest(FlightCall *call, const ProtobufCBinaryData *command,
			  const ProtobufCBinaryData *first_header)
{
	Arrow__Flight__Protocol__Sql__CommandStatementIngest *cmd;
	Arrow__Flight__Protocol__Sql__CommandStatementIngest__TableDefinitionOptions *opts;
	const VexecEgressRoutine *eg;
	const VexecIngestColumn *cols;
	IngestReader r;
	const char *schema_md;
	size_t		schema_len;
	void	   *stream;
	const char *nspname = NULL;
	char	   *qname;
	Oid			relid;
	int			ncols;
	int			i;
	StringInfoData sql;
	StringInfoData targets;
	StringInfoData defs;
	int64		n;
	int			if_not_exist;
	int			if_exists;

	cmd = (Arrow__Flight__Protocol__Sql__CommandStatementIngest *)
		protobuf_c_message_unpack(&arrow__flight__protocol__sql__command_statement_ingest__descriptor,
								  flight_pb_allocator(), command->len, command->data);
	if (cmd == NULL)
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION,
					 "a malformed CommandStatementIngest");
	eg = ingest_routine();
	flight_check_transaction(cmd->_transaction_id_case != 0, &cmd->transaction_id);

	opts = cmd->table_definition_options;
	if_not_exist = opts ? opts->if_not_exist : 0;
	if_exists = opts ? opts->if_exists : 0;
	if (if_not_exist == ARROW__FLIGHT__PROTOCOL__SQL__COMMAND_STATEMENT_INGEST__TABLE_DEFINITION_OPTIONS__TABLE_NOT_EXIST_OPTION__TABLE_NOT_EXIST_OPTION_UNSPECIFIED ||
		if_exists == ARROW__FLIGHT__PROTOCOL__SQL__COMMAND_STATEMENT_INGEST__TABLE_DEFINITION_OPTIONS__TABLE_EXISTS_OPTION__TABLE_EXISTS_OPTION_UNSPECIFIED)
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_INVALID_PARAMETER_VALUE,
					 "CommandStatementIngest's table_definition_options must say what to do where the table does not exist, and where it does");
	if (cmd->table == NULL || cmd->table[0] == '\0')
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_INVALID_PARAMETER_VALUE,
					 "CommandStatementIngest names no table");
	if (cmd->_catalog_case != 0 && cmd->catalog != NULL && cmd->catalog[0] != '\0' &&
		strcmp(cmd->catalog, get_database_name(MyDatabaseId)) != 0)
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_FEATURE_NOT_SUPPORTED,
					 "catalog \"%s\" is not this session's database", cmd->catalog);
	if (cmd->_schema_case != 0 && cmd->schema != NULL && cmd->schema[0] != '\0')
		nspname = cmd->schema;
	if (cmd->temporary)
	{
		if (nspname != NULL)
			flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_INVALID_PARAMETER_VALUE,
						 "a temporary table has no schema of its own");
		nspname = "pg_temp";
	}
	qname = nspname != NULL ? quote_qualified_identifier(nspname, cmd->table) :
		pstrdup(quote_identifier(cmd->table));

	/* the stream, its columns the types their Arrow types name */
	memset(&r, 0, sizeof(r));
	r.call = call;
	initStringInfo(&r.msg);
	ingest_schema(&r, first_header, &schema_md, &schema_len);
	stream = eg->ingest_begin(schema_md, schema_len, ingest_read, &r);
	ncols = eg->ingest_columns(stream, &cols);

	/* the table: found, replaced or created */
	relid = RangeVarGetRelid(makeRangeVar(nspname ? pstrdup(nspname) : NULL, cmd->table, -1),
							 NoLock, true);
	if (OidIsValid(relid))
	{
		if (if_exists == ARROW__FLIGHT__PROTOCOL__SQL__COMMAND_STATEMENT_INGEST__TABLE_DEFINITION_OPTIONS__TABLE_EXISTS_OPTION__TABLE_EXISTS_OPTION_FAIL)
			flight_error(GRPC_ALREADY_EXISTS, ERRCODE_DUPLICATE_TABLE,
						 "relation %s already exists", qname);
		if (if_exists == ARROW__FLIGHT__PROTOCOL__SQL__COMMAND_STATEMENT_INGEST__TABLE_DEFINITION_OPTIONS__TABLE_EXISTS_OPTION__TABLE_EXISTS_OPTION_REPLACE)
		{
			(void) ingest_run(psprintf("DROP TABLE %s", qname));
			relid = InvalidOid;
		}
	}
	else if (if_not_exist == ARROW__FLIGHT__PROTOCOL__SQL__COMMAND_STATEMENT_INGEST__TABLE_DEFINITION_OPTIONS__TABLE_NOT_EXIST_OPTION__TABLE_NOT_EXIST_OPTION_FAIL)
		flight_error(GRPC_NOT_FOUND, ERRCODE_UNDEFINED_TABLE,
					 "relation %s does not exist", qname);
	if (!OidIsValid(relid))
	{
		initStringInfo(&defs);
		for (i = 0; i < ncols; i++)
			appendStringInfo(&defs, "%s%s %s", i > 0 ? ", " : "",
							 quote_identifier(cols[i].name),
							 format_type_with_typemod(cols[i].type, cols[i].typmod));
		(void) ingest_run(psprintf("CREATE %sTABLE %s (%s)",
								   cmd->temporary ? "TEMPORARY " : "",
								   cmd->temporary ? quote_identifier(cmd->table) : qname,
								   defs.data));
		relid = RangeVarGetRelid(makeRangeVar(nspname ? pstrdup(nspname) : NULL, cmd->table, -1),
								 NoLock, false);
	}

	/* the INSERT: each stream column into the table's column of its name */
	initStringInfo(&targets);
	initStringInfo(&defs);
	for (i = 0; i < ncols; i++)
	{
		AttrNumber	attno = get_attnum(relid, cols[i].name);
		Oid			atttype;
		int32		atttypmod;
		Oid			collation;

		if (attno == InvalidAttrNumber || attno < 0)
			flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_UNDEFINED_COLUMN,
						 "column \"%s\" of the stream is not a column of %s", cols[i].name, qname);
		get_atttypetypmodcoll(relid, attno, &atttype, &atttypmod, &collation);
		appendStringInfo(&targets, "%s%s", i > 0 ? ", " : "", quote_identifier(cols[i].name));
		appendStringInfo(&defs, "%s%s %s", i > 0 ? ", " : "", quote_identifier(cols[i].name),
						 read_as(&cols[i], atttype, atttypmod));
	}
	initStringInfo(&sql);
	appendStringInfo(&sql, "INSERT INTO %s (%s) SELECT %s FROM vexec.ingest_stream(" INT64_FORMAT ") AS s(%s)",
					 qname, targets.data, targets.data, eg->ingest_handle(stream), defs.data);
	n = ingest_run(sql.data);
	ingest_finish(call, eg, stream, &r);
	flight_finish_xact();
	put_update_result(call, n);
}

/* ---------------------------------------------------------------------
 * A prepared INSERT's parameter batches
 * ---------------------------------------------------------------------
 */

/* The parameter an INSERT's value is, through implicit coercions, or 0. */
static int
value_param(Node *expr)
{
	for (;;)
	{
		if (expr == NULL)
			return 0;
		if (IsA(expr, Param))
			return ((Param *) expr)->paramkind == PARAM_EXTERN ? ((Param *) expr)->paramid : 0;
		if (IsA(expr, RelabelType))
			expr = (Node *) ((RelabelType *) expr)->arg;
		else if (IsA(expr, CoerceToDomain))
			expr = (Node *) ((CoerceToDomain *) expr)->arg;
		else if (IsA(expr, FuncExpr) &&
				 ((FuncExpr *) expr)->funcformat == COERCE_IMPLICIT_CAST)
			expr = linitial(((FuncExpr *) expr)->args);
		else
			return 0;
	}
}

/* How a prepared INSERT's parameter batches go in as one INSERT ... SELECT. */
typedef struct PreparedInsert
{
	Oid			relid;
	char	   *qname;
	AttrNumber *attnos;			/* per parameter, from 1: its column */
} PreparedInsert;

/*
 * Whether a prepared statement's parameter batches can go in as one INSERT
 * ... SELECT: "INSERT INTO t [(columns)] VALUES ($1, ...)", every value a
 * parameter or DEFAULT and every parameter once, into a table with no
 * trigger and no rule.  Decided before the stream is read.
 */
static bool
prepared_insert(FlightStatement *stmt, PreparedInsert *pi)
{
	RawStmt    *raw = stmt->plansource->raw_parse_tree;
	InsertStmt *ins;
	SelectStmt *sel;
	Query	   *q;
	Relation	rel;
	bool		plain;
	ListCell   *lc;
	int			i;

	if (raw == NULL || !IsA(raw->stmt, InsertStmt) || stmt->nparams == 0)
		return false;
	ins = (InsertStmt *) raw->stmt;
	if (ins->onConflictClause != NULL || ins->withClause != NULL ||
		ins->returningClause != NULL || ins->override != OVERRIDING_NOT_SET ||
		ins->selectStmt == NULL || !IsA(ins->selectStmt, SelectStmt))
		return false;
	sel = (SelectStmt *) ins->selectStmt;
	if (list_length(sel->valuesLists) != 1)
		return false;
	foreach(lc, (List *) linitial(sel->valuesLists))
		if (!IsA(lfirst(lc), ParamRef) && !IsA(lfirst(lc), SetToDefault))
			return false;
	if (list_length(stmt->plansource->query_list) != 1)
		return false;
	q = linitial_node(Query, stmt->plansource->query_list);
	if (q->commandType != CMD_INSERT || q->resultRelation <= 0 || q->returningList != NIL)
		return false;

	pi->relid = rt_fetch(q->resultRelation, q->rtable)->relid;
	rel = table_open(pi->relid, AccessShareLock);
	plain = rel->rd_rel->relkind == RELKIND_RELATION && rel->trigdesc == NULL &&
		rel->rd_rules == NULL;
	pi->qname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
										   RelationGetRelationName(rel));
	table_close(rel, AccessShareLock);
	if (!plain)
		return false;

	/* each parameter's column, once */
	pi->attnos = palloc0(sizeof(AttrNumber) * (stmt->nparams + 1));
	foreach(lc, q->targetList)
	{
		TargetEntry *tle = lfirst(lc);
		int			p = value_param((Node *) tle->expr);

		if (p == 0)
			continue;			/* a default */
		if (p > stmt->nparams || pi->attnos[p] != 0)
			return false;
		pi->attnos[p] = tle->resno;
	}
	for (i = 1; i <= stmt->nparams; i++)
		if (pi->attnos[i] == 0)
			return false;
	return true;
}

/* The INSERT ... SELECT itself, the stream's columns the parameters. */
static char *
prepared_insert_sql(const PreparedInsert *pi, int nparams, int64 handle,
					const VexecIngestColumn *cols, int ncols)
{
	StringInfoData targets;
	StringInfoData values;
	StringInfoData defs;
	int			i;

	if (ncols != nparams)
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION,
					 "the parameters' batches supply %d columns, but the statement requires %d",
					 ncols, nparams);
	initStringInfo(&targets);
	initStringInfo(&values);
	initStringInfo(&defs);
	for (i = 1; i <= nparams; i++)
	{
		Oid			atttype;
		int32		atttypmod;
		Oid			collation;

		get_atttypetypmodcoll(pi->relid, pi->attnos[i], &atttype, &atttypmod, &collation);
		appendStringInfo(&targets, "%s%s", i > 1 ? ", " : "",
						 quote_identifier(get_attname(pi->relid, pi->attnos[i], false)));
		appendStringInfo(&values, "%sp%d", i > 1 ? ", " : "", i);
		appendStringInfo(&defs, "%sp%d %s", i > 1 ? ", " : "", i,
						 read_as(&cols[i - 1], atttype, atttypmod));
	}
	return psprintf("INSERT INTO %s (%s) SELECT %s FROM vexec.ingest_stream(" INT64_FORMAT ") AS s(%s)",
					pi->qname, targets.data, values.data, handle, defs.data);
}

/*
 * A prepared update's DoPut, where its parameter batches can go in as one
 * INSERT ... SELECT: true, and the call answered; false, with nothing read,
 * where the statement runs once a row (sql.c).
 */
bool
flight_ingest_prepared(FlightCall *call, FlightStatement *stmt,
					   const ProtobufCBinaryData *first_header)
{
	const VexecEgressRoutine *eg = flight_session->egress;
	const VexecIngestColumn *cols;
	PreparedInsert pi;
	IngestReader r;
	const char *schema_md;
	size_t		schema_len;
	void	   *stream;
	int			ncols;
	int64		n;

	if (stmt->plansource == NULL || !VEXEC_EGRESS_HAS(eg, ingest_end) ||
		!OidIsValid(get_namespace_oid("vexec", true)) || !prepared_insert(stmt, &pi))
		return false;

	memset(&r, 0, sizeof(r));
	r.call = call;
	initStringInfo(&r.msg);
	ingest_schema(&r, first_header, &schema_md, &schema_len);
	stream = eg->ingest_begin(schema_md, schema_len, ingest_read, &r);
	ncols = eg->ingest_columns(stream, &cols);
	n = ingest_run(prepared_insert_sql(&pi, stmt->nparams, eg->ingest_handle(stream),
									   cols, ncols));
	ingest_finish(call, eg, stream, &r);
	flight_finish_xact();
	put_update_result(call, n);
	return true;
}
