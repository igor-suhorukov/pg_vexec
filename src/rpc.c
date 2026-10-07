/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * rpc.c
 *	  Flight's methods, with Flight SQL's commands in them
 *	  (arrow/format/Flight.proto, arrow/format/FlightSql.proto;
 *	  pg_vector_executor.md §3.15).
 *
 * Flight SQL rides on Flight: a command is a protobuf message packed in a
 * google.protobuf.Any, in a FlightDescriptor's cmd, a Ticket, or an
 * Action's body.  The mapping is the pg_arrow plan's M5:
 *
 *	GetFlightInfo	a query or a prepared query is planned (sql.c) and its
 *					result's schema returned, with a ticket naming it; a
 *					catalog command's ticket is the command itself
 *	DoGet			the ticket's statement runs, its result to vexec's
 *					receiver, each message written as FlightData
 *	DoPut			an update returns its row count; a prepared
 *					statement's parameter batches are bound, or, for a
 *					prepared INSERT of parameters, go in as one INSERT ...
 *					SELECT, as an ingest's batches do (ingest.c)
 *	DoAction		prepared statements, transactions and savepoints,
 *					session options, cancels
 *
 * FlightData is written by hand around the egress's messages: its
 * data_header the IPC message's flatbuffer, its data_body the body's
 * pieces, each sent as it lies (h2.c), so that the tags and lengths of two
 * fields are the only bytes the session adds.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "session.h"

#define FLIGHT_SERVICE			"/arrow.flight.protocol.FlightService/"

typedef Arrow__Flight__Protocol__FlightDescriptor PbDescriptor;
typedef Arrow__Flight__Protocol__FlightInfo PbFlightInfo;
typedef Arrow__Flight__Protocol__FlightEndpoint PbEndpoint;
typedef Arrow__Flight__Protocol__Ticket PbTicket;
typedef Arrow__Flight__Protocol__FlightData PbFlightData;
typedef Arrow__Flight__Protocol__Action PbAction;

/* ---------------------------------------------------------------------
 * Protobuf
 * ---------------------------------------------------------------------
 */

static void *
pb_alloc(void *data, size_t size)
{
	return MemoryContextAllocExtended((MemoryContext) data, Max(size, 1),
									  MCXT_ALLOC_NO_OOM | MCXT_ALLOC_HUGE);
}

static void
pb_free(void *data, void *ptr)
{
	(void) data;
	if (ptr)
		pfree(ptr);
}

/* protobuf-c's allocator: the call's memory. */
ProtobufCAllocator *
flight_pb_allocator(void)
{
	static ProtobufCAllocator alloc;

	alloc.alloc = pb_alloc;
	alloc.free = pb_free;
	alloc.allocator_data = flight_session->callcxt;
	return &alloc;
}

static void *
unpack(const ProtobufCMessageDescriptor *desc, const uint8_t *data, size_t len, const char *what)
{
	void	   *msg = protobuf_c_message_unpack(desc, flight_pb_allocator(), len, data);

	if (msg == NULL)
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "a malformed %s", what);
	return msg;
}

static void
pack(const ProtobufCMessage *msg, ProtobufCBinaryData *out)
{
	out->len = protobuf_c_message_get_packed_size(msg);
	out->data = palloc(Max(out->len, 1));
	protobuf_c_message_pack(msg, out->data);
}

/* One response message. */
void
flight_send_message(FlightCall *call, const ProtobufCMessage *msg)
{
	ProtobufCBinaryData bytes;
	FlightPiece piece;

	pack(msg, &bytes);
	piece.data = bytes.data;
	piece.len = bytes.len;
	if (!h2_call_send(call, &piece, 1))
		flight_error(GRPC_CANCELLED, ERRCODE_CONNECTION_FAILURE, "the client ended the call");
}

/* A Flight Result whose body is a Flight SQL message packed in an Any. */
void
flight_send_result(FlightCall *call, const char *type, const ProtobufCMessage *msg)
{
	Arrow__Flight__Protocol__Result result = ARROW__FLIGHT__PROTOCOL__RESULT__INIT;

	if (type)
		flight_any_pack(type, msg, &result.body);
	else
		pack(msg, &result.body);
	flight_send_message(call, &result.base);
}

static void
put_varint(StringInfo buf, uint64 v)
{
	while (v >= 0x80)
	{
		appendStringInfoChar(buf, (char) ((v & 0x7f) | 0x80));
		v >>= 7;
	}
	appendStringInfoChar(buf, (char) v);
}

/* google.protobuf.Any: type_url (1), value (2). */
void
flight_any_pack(const char *type, const ProtobufCMessage *msg, ProtobufCBinaryData *out)
{
	StringInfoData buf;
	ProtobufCBinaryData value;
	char	   *url = psprintf(FLIGHT_SQL_TYPE_PREFIX "%s", type);

	pack(msg, &value);
	initStringInfo(&buf);
	appendStringInfoChar(&buf, 0x0a);
	put_varint(&buf, strlen(url));
	appendStringInfoString(&buf, url);
	appendStringInfoChar(&buf, 0x12);
	put_varint(&buf, value.len);
	appendBinaryStringInfo(&buf, (const char *) value.data, (int) value.len);
	out->data = (uint8_t *) buf.data;
	out->len = buf.len;
}

static bool
get_varint(const uint8_t **p, const uint8_t *end, uint64 *v)
{
	int			shift = 0;

	*v = 0;
	while (*p < end && shift < 64)
	{
		uint8_t		b = *(*p)++;

		*v |= (uint64) (b & 0x7f) << shift;
		if (!(b & 0x80))
			return true;
		shift += 7;
	}
	return false;
}

/*
 * An Any's message name -- its type URL past Flight SQL's prefix -- and its
 * value, which points into data.
 */
bool
flight_any_unpack(const uint8_t *data, size_t len, char **type, ProtobufCBinaryData *value)
{
	const uint8_t *p = data;
	const uint8_t *end = data + len;
	char	   *url = NULL;

	value->data = NULL;
	value->len = 0;
	while (p < end)
	{
		uint64		key;
		uint64		n;

		if (!get_varint(&p, end, &key))
			return false;
		if ((key & 7) != 2)
		{
			/* a field Any does not have: skipped, as protobuf does */
			if ((key & 7) == 0 && get_varint(&p, end, &n))
				continue;
			return false;
		}
		if (!get_varint(&p, end, &n) || n > (uint64) (end - p))
			return false;
		if ((key >> 3) == 1)
			url = pnstrdup((const char *) p, n);
		else if ((key >> 3) == 2)
		{
			value->data = (uint8_t *) p;
			value->len = n;
		}
		p += n;
	}
	if (url == NULL || strncmp(url, FLIGHT_SQL_TYPE_PREFIX, strlen(FLIGHT_SQL_TYPE_PREFIX)) != 0)
		return false;
	*type = url + strlen(FLIGHT_SQL_TYPE_PREFIX);
	return true;
}

/* The Any in a descriptor's cmd, or a ticket: Flight SQL's, or INVALID_ARGUMENT. */
static void
command_of(const ProtobufCBinaryData *bytes, char **type, ProtobufCBinaryData *value)
{
	if (!flight_any_unpack(bytes->data, bytes->len, type, value))
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION,
					 "not a Flight SQL command: a google.protobuf.Any of arrow.flight.protocol.sql's messages is expected");
}

/* ---------------------------------------------------------------------
 * Results as FlightData
 * ---------------------------------------------------------------------
 */

static bool serve_inline_cancel(FlightCall *call);

/*
 * The egress's write function: an IPC message as one FlightData, its
 * data_header (field 2) the message's flatbuffer and its data_body (field
 * 1000) the body's pieces (arrow/format/Flight.proto, FlightData).
 */
void
flight_egress_write(void *arg, const VexecEgressMessage *msg)
{
	FlightCall *call = arg;
	static FlightPiece *pieces = NULL;
	static int	maxpieces = 0;
	char		head[16];
	char		mid[16];
	StringInfoData b;
	int			n = 0;
	int			i;

	if (maxpieces < msg->npieces + 4)
	{
		maxpieces = msg->npieces + 64;
		pieces = pieces ? repalloc(pieces, sizeof(FlightPiece) * maxpieces)
			: MemoryContextAlloc(TopMemoryContext, sizeof(FlightPiece) * maxpieces);
	}

	/* field 2, data_header */
	b.data = head;
	b.len = 0;
	b.maxlen = sizeof(head);
	appendStringInfoChar(&b, 0x12);
	put_varint(&b, msg->metadata_len);
	pieces[n].data = head;
	pieces[n++].len = b.len;
	pieces[n].data = msg->metadata;
	pieces[n++].len = msg->metadata_len;
	if (msg->body_len > 0)
	{
		/* field 1000, data_body: key 8002 */
		b.data = mid;
		b.len = 0;
		b.maxlen = sizeof(mid);
		appendStringInfoChar(&b, (char) 0xc2);
		appendStringInfoChar(&b, 0x3e);
		put_varint(&b, msg->body_len);
		pieces[n].data = mid;
		pieces[n++].len = b.len;
		for (i = 0; i < msg->npieces; i++)
		{
			pieces[n].data = msg->pieces[i].data;
			pieces[n++].len = msg->pieces[i].len;
		}
	}
	if (!h2_call_send(call, pieces, n))
		flight_error(GRPC_CANCELLED, ERRCODE_QUERY_CANCELED, "the client ended the stream");

	/* between messages: a cancel, the client's or the server's */
	while (serve_inline_cancel(call))
		;
	CHECK_FOR_INTERRUPTS();
}

/* The schema of a result, as FlightInfo and Flight SQL's results carry it: an encapsulated IPC message. */
static void
schema_collect(void *arg, const VexecEgressMessage *msg)
{
	StringInfo	out = arg;

	appendBinaryStringInfo(out, msg->prefix, 8);
	appendBinaryStringInfo(out, msg->metadata, (int) msg->metadata_len);
}

void
flight_schema_bytes(TupleDesc desc, const VexecEgressField *fields, int nfields, StringInfo out)
{
	flight_session->egress->schema(desc, fields, nfields, schema_collect, out);
}

/* ---------------------------------------------------------------------
 * Cancels
 * ---------------------------------------------------------------------
 */

/* A ticket's statement: the session it is in, and its handle. */
static bool
ticket_statement(const ProtobufCBinaryData *ticket, uint64 *session_id, uint64 *handle)
{
	char	   *type;
	ProtobufCBinaryData value;
	ProtobufCBinaryData h = {0, NULL};

	if (!flight_any_unpack(ticket->data, ticket->len, &type, &value))
		return false;
	if (strcmp(type, "TicketStatementQuery") == 0)
	{
		Arrow__Flight__Protocol__Sql__TicketStatementQuery *t =
			(Arrow__Flight__Protocol__Sql__TicketStatementQuery *) protobuf_c_message_unpack(&arrow__flight__protocol__sql__ticket_statement_query__descriptor,
									  flight_pb_allocator(), value.len, value.data);

		if (t)
			h = t->statement_handle;
	}
	else if (strcmp(type, "CommandPreparedStatementQuery") == 0)
	{
		Arrow__Flight__Protocol__Sql__CommandPreparedStatementQuery *t =
			(Arrow__Flight__Protocol__Sql__CommandPreparedStatementQuery *) protobuf_c_message_unpack(&arrow__flight__protocol__sql__command_prepared_statement_query__descriptor,
									  flight_pb_allocator(), value.len, value.data);

		if (t)
			h = t->prepared_statement_handle;
	}
	if (h.len != 16)
		return false;
	memcpy(session_id, h.data, 8);
	memcpy(handle, h.data + 8, 8);
	return true;
}

/*
 * Cancel the statement a FlightInfo's ticket names, in whichever session
 * holds it, as pg_cancel_backend() would: the caller must have the
 * privileges of the session's role, or of pg_signal_backend.
 */
static Arrow__Flight__Protocol__CancelStatus
cancel_flight_info(const PbFlightInfo *info)
{
	uint64		session_id;
	uint64		handle;
	int			i;
	pid_t		pid = 0;
	Oid			roleid = InvalidOid;
	bool		running = false;

	if (info == NULL || info->n_endpoint == 0 || info->endpoint[0]->ticket == NULL ||
		!ticket_statement(&info->endpoint[0]->ticket->ticket, &session_id, &handle))
		return ARROW__FLIGHT__PROTOCOL__CANCEL_STATUS__CANCEL_STATUS_NOT_CANCELLABLE;

	if (session_id == flight_session->session_id)
	{
		ProtobufCBinaryData h;
		FlightStatement *stmt;
		uint8		raw[16];

		/* this session's: not running now, so dropped before it runs */
		memcpy(raw, &session_id, 8);
		memcpy(raw + 8, &handle, 8);
		h.data = raw;
		h.len = 16;
		stmt = flight_statement_find(&h, false);
		if (stmt == NULL)
			return ARROW__FLIGHT__PROTOCOL__CANCEL_STATUS__CANCEL_STATUS_NOT_CANCELLABLE;
		flight_statement_drop(stmt);
		return ARROW__FLIGHT__PROTOCOL__CANCEL_STATUS__CANCEL_STATUS_CANCELLED;
	}

	SpinLockAcquire(&flight_shared->mutex);
	for (i = 0; i < flight_shared->nslots; i++)
	{
		FlightSlot *s = &flight_shared->slots[i];

		if (s->state == FLIGHT_SLOT_ACTIVE && s->session_id == session_id)
		{
			pid = s->pid;
			roleid = s->roleid;
			running = s->running == handle;
			if (!running)
				s->cancelled = handle;	/* refused when it is asked to run */
			break;
		}
	}
	SpinLockRelease(&flight_shared->mutex);
	if (pid == 0)
		return ARROW__FLIGHT__PROTOCOL__CANCEL_STATUS__CANCEL_STATUS_NOT_CANCELLABLE;
	if (!has_privs_of_role(GetUserId(), roleid) &&
		!has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
		flight_error(GRPC_PERMISSION_DENIED, ERRCODE_INSUFFICIENT_PRIVILEGE,
					 "permission denied to cancel a statement of another Flight session");
	if (!running)
		return ARROW__FLIGHT__PROTOCOL__CANCEL_STATUS__CANCEL_STATUS_CANCELLED;
	if (kill(pid, SIGINT) != 0)
		return ARROW__FLIGHT__PROTOCOL__CANCEL_STATUS__CANCEL_STATUS_NOT_CANCELLABLE;
	return ARROW__FLIGHT__PROTOCOL__CANCEL_STATUS__CANCEL_STATUS_CANCELLING;
}

/*
 * While a statement streams, a CancelFlightInfo for it may come on the same
 * connection: served at once, between two messages, and the statement
 * cancelled as pg_cancel_backend() would.  True if one was served.
 */
/* Whether a DoAction's request is a CancelFlightInfo. */
static bool
is_cancel_action(const char *msg, size_t len)
{
	PbAction   *action = (PbAction *) protobuf_c_message_unpack(&arrow__flight__protocol__action__descriptor,
																flight_pb_allocator(), len,
																(const uint8_t *) msg);

	return action != NULL && action->type != NULL && strcmp(action->type, "CancelFlightInfo") == 0;
}

static bool
serve_inline_cancel(FlightCall *call)
{
	FlightCall *cancel = h2_take_ready_call(flight_session->conn, FLIGHT_SERVICE "DoAction",
											is_cancel_action);
	StringInfoData msg;
	PbAction   *action;
	bool		ours = false;

	(void) call;
	if (cancel == NULL)
		return false;
	initStringInfo(&msg);
	if (!flight_is_bearer(cancel) || !h2_call_read(cancel, &msg))
	{
		h2_call_finish(cancel, GRPC_UNAUTHENTICATED, "a cancel without this session's bearer token", NULL);
		return true;
	}
	action = (PbAction *) protobuf_c_message_unpack(&arrow__flight__protocol__action__descriptor,
													flight_pb_allocator(), msg.len, (uint8_t *) msg.data);
	if (action && strcmp(action->type, "CancelFlightInfo") == 0)
	{
		Arrow__Flight__Protocol__CancelFlightInfoRequest *req =
			(Arrow__Flight__Protocol__CancelFlightInfoRequest *) protobuf_c_message_unpack(&arrow__flight__protocol__cancel_flight_info_request__descriptor,
									  flight_pb_allocator(), action->body.len, action->body.data);
		Arrow__Flight__Protocol__CancelFlightInfoResult result =
			ARROW__FLIGHT__PROTOCOL__CANCEL_FLIGHT_INFO_RESULT__INIT;
		uint64		session_id;
		uint64		handle;

		if (req && req->info && req->info->n_endpoint > 0 && req->info->endpoint[0]->ticket &&
			ticket_statement(&req->info->endpoint[0]->ticket->ticket, &session_id, &handle) &&
			session_id == flight_session->session_id && handle == flight_session->running)
			ours = true;
		result.status = ours ? ARROW__FLIGHT__PROTOCOL__CANCEL_STATUS__CANCEL_STATUS_CANCELLED
			: ARROW__FLIGHT__PROTOCOL__CANCEL_STATUS__CANCEL_STATUS_NOT_CANCELLABLE;
		flight_send_result(cancel, NULL, &result.base);
		h2_call_finish(cancel, GRPC_OK, NULL, NULL);
	}
	else
		h2_call_finish(cancel, GRPC_INVALID_ARGUMENT, "a malformed CancelFlightInfo", NULL);
	if (ours)
	{
		/* as StatementCancelHandler would */
		QueryCancelPending = true;
		InterruptPending = true;
	}
	return true;
}

/* ---------------------------------------------------------------------
 * The methods
 * ---------------------------------------------------------------------
 */

/* The single request message of a unary or server-streaming call. */
static void
read_request(FlightCall *call, StringInfo msg)
{
	initStringInfo(msg);
	if (!h2_call_read(call, msg))
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "the call has no request message");
}

static void
handshake(FlightCall *call)
{
	Arrow__Flight__Protocol__HandshakeResponse resp = ARROW__FLIGHT__PROTOCOL__HANDSHAKE_RESPONSE__INIT;
	StringInfoData msg;

	initStringInfo(&msg);
	while (h2_call_read(call, &msg))
		;						/* the requests' payloads: basic auth needs none */
	h2_call_set_header(call, "authorization", flight_session->bearer);
	flight_send_message(call, &resp.base);
	h2_call_finish(call, GRPC_OK, NULL, NULL);
}

/* A descriptor's command, for GetFlightInfo, GetSchema and PollFlightInfo. */
static PbDescriptor *
read_descriptor(FlightCall *call, char **type, ProtobufCBinaryData *value)
{
	StringInfoData msg;
	PbDescriptor *desc;

	read_request(call, &msg);
	desc = unpack(&arrow__flight__protocol__flight_descriptor__descriptor,
				  (uint8_t *) msg.data, msg.len, "FlightDescriptor");
	if (desc->type != ARROW__FLIGHT__PROTOCOL__FLIGHT_DESCRIPTOR__DESCRIPTOR_TYPE__CMD)
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_FEATURE_NOT_SUPPORTED,
					 "only command descriptors are served: Flight SQL's commands");
	command_of(&desc->cmd, type, value);
	return desc;
}

/*
 * A command's FlightInfo: its result's schema, and one endpoint whose
 * ticket names it.  NULL ticket: for GetSchema, no statement is kept.
 *
 * A statement that returns no rows -- an INSERT, an UPDATE, DDL -- sent as
 * a query runs here, as GetFlightInfo's own work: its FlightInfo has no
 * endpoint, and its row count as total_records, which is what clients that
 * send every statement as a query read (adbc_driver_flightsql's
 * ExecuteQuery: a DoGet it starts and may cancel unread when its cursor
 * closes, which would leave the statement never run).  *ran is its row
 * count, -1 when it counts none; -2 when nothing ran.
 */
static void
command_info(char *type, ProtobufCBinaryData *value, const ProtobufCBinaryData *cmd,
			 StringInfo schema, ProtobufCBinaryData *ticket, int64 *ran)
{
	*ran = -2;
	if (strcmp(type, "CommandStatementQuery") == 0)
	{
		Arrow__Flight__Protocol__Sql__CommandStatementQuery *q =
			unpack(&arrow__flight__protocol__sql__command_statement_query__descriptor,
				   value->data, value->len, "CommandStatementQuery");
		FlightStatement *stmt;

		flight_check_transaction(q->_transaction_id_case != 0, &q->transaction_id);
		stmt = flight_statement_create(q->query, false);
		flight_statement_schema(stmt, schema);
		if (ticket && stmt->result == NULL)
		{
			*ran = flight_statement_update(stmt);
			flight_statement_drop(stmt);
		}
		else if (ticket)
		{
			Arrow__Flight__Protocol__Sql__TicketStatementQuery t =
				ARROW__FLIGHT__PROTOCOL__SQL__TICKET_STATEMENT_QUERY__INIT;

			flight_statement_handle(stmt, &t.statement_handle);
			flight_any_pack("TicketStatementQuery", &t.base, ticket);
		}
		else
			flight_statement_drop(stmt);
	}
	else if (strcmp(type, "CommandPreparedStatementQuery") == 0)
	{
		Arrow__Flight__Protocol__Sql__CommandPreparedStatementQuery *q =
			unpack(&arrow__flight__protocol__sql__command_prepared_statement_query__descriptor,
				   value->data, value->len, "CommandPreparedStatementQuery");
		FlightStatement *stmt = flight_statement_find(&q->prepared_statement_handle, true);

		if (stmt == NULL)
			flight_error(GRPC_NOT_FOUND, ERRCODE_UNDEFINED_PSTATEMENT, "no such prepared statement");
		flight_statement_schema(stmt, schema);
		if (ticket && stmt->result == NULL)
			*ran = flight_statement_update(stmt);
		else if (ticket)
			*ticket = *cmd;
	}
	else if (strcmp(type, "CommandStatementSubstraitPlan") == 0 ||
			 strcmp(type, "CommandStatementIngest") == 0)
		flight_error(GRPC_UNIMPLEMENTED, ERRCODE_FEATURE_NOT_SUPPORTED, "%s is not served", type);
	else if (flight_catalog_schema(type, value, schema))
	{
		if (ticket)
			*ticket = *cmd;
	}
	else
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_FEATURE_NOT_SUPPORTED,
					 "unknown Flight SQL command \"%s\"", type);
}

/* NULL ticket: a statement that ran (command_info), no endpoint. */
static PbFlightInfo *
flight_info(PbDescriptor *desc, StringInfo schema, ProtobufCBinaryData *ticket, int64 records)
{
	PbFlightInfo *info = palloc(sizeof(PbFlightInfo));

	arrow__flight__protocol__flight_info__init(info);
	info->schema.data = (uint8_t *) schema->data;
	info->schema.len = schema->len;
	info->flight_descriptor = desc;
	if (ticket)
	{
		PbEndpoint *endpoint = palloc(sizeof(PbEndpoint));
		PbTicket   *t = palloc(sizeof(PbTicket));

		arrow__flight__protocol__flight_endpoint__init(endpoint);
		arrow__flight__protocol__ticket__init(t);
		t->ticket = *ticket;
		endpoint->ticket = t;
		info->n_endpoint = 1;
		info->endpoint = palloc(sizeof(PbEndpoint *));
		info->endpoint[0] = endpoint;
	}
	info->total_records = records;
	info->total_bytes = -1;
	return info;
}

static void
get_flight_info(FlightCall *call, bool poll)
{
	char	   *type;
	ProtobufCBinaryData value;
	ProtobufCBinaryData ticket;
	StringInfoData schema;
	PbDescriptor *desc = read_descriptor(call, &type, &value);
	PbFlightInfo *info;
	int64		ran;

	flight_require_active();
	initStringInfo(&schema);
	command_info(type, &value, &desc->cmd, &schema, &ticket, &ran);
	if (ran == -2)
		info = flight_info(desc, &schema, &ticket, -1);
	else
		info = flight_info(desc, &schema, NULL, ran);
	flight_finish_xact();
	if (poll)
	{
		Arrow__Flight__Protocol__PollInfo pi = ARROW__FLIGHT__PROTOCOL__POLL_INFO__INIT;

		/* done at once: no descriptor to poll again, and all of it */
		pi.info = info;
		pi._progress_case = ARROW__FLIGHT__PROTOCOL__POLL_INFO___PROGRESS_PROGRESS;
		pi.progress = 1.0;
		flight_send_message(call, &pi.base);
	}
	else
		flight_send_message(call, &info->base);
	h2_call_finish(call, GRPC_OK, NULL, NULL);
}

static void
get_schema(FlightCall *call)
{
	char	   *type;
	ProtobufCBinaryData value;
	StringInfoData schema;
	Arrow__Flight__Protocol__SchemaResult result = ARROW__FLIGHT__PROTOCOL__SCHEMA_RESULT__INIT;
	PbDescriptor *desc = read_descriptor(call, &type, &value);
	int64		ran;

	flight_require_active();
	initStringInfo(&schema);
	command_info(type, &value, &desc->cmd, &schema, NULL, &ran);
	flight_finish_xact();
	result.schema.data = (uint8_t *) schema.data;
	result.schema.len = schema.len;
	flight_send_message(call, &result.base);
	h2_call_finish(call, GRPC_OK, NULL, NULL);
}

static void
do_get(FlightCall *call)
{
	StringInfoData msg;
	PbTicket   *ticket;
	char	   *type;
	ProtobufCBinaryData value;

	read_request(call, &msg);
	ticket = unpack(&arrow__flight__protocol__ticket__descriptor, (uint8_t *) msg.data, msg.len, "Ticket");
	command_of(&ticket->ticket, &type, &value);
	flight_require_active();
	if (strcmp(type, "TicketStatementQuery") == 0 || strcmp(type, "CommandPreparedStatementQuery") == 0)
	{
		bool		prepared = type[0] == 'C';
		ProtobufCBinaryData handle;
		FlightStatement *stmt;

		if (prepared)
			handle = ((Arrow__Flight__Protocol__Sql__CommandPreparedStatementQuery *)
					  unpack(&arrow__flight__protocol__sql__command_prepared_statement_query__descriptor,
							 value.data, value.len, type))->prepared_statement_handle;
		else
			handle = ((Arrow__Flight__Protocol__Sql__TicketStatementQuery *)
					  unpack(&arrow__flight__protocol__sql__ticket_statement_query__descriptor,
							 value.data, value.len, type))->statement_handle;
		stmt = flight_statement_find(&handle, prepared);
		if (stmt == NULL)
			flight_error(GRPC_NOT_FOUND, ERRCODE_UNDEFINED_PSTATEMENT,
						 "no such statement in this session: a ticket is good once, on the connection that asked for it");
		flight_statement_run(call, stmt);
		if (!prepared)
			flight_statement_drop(stmt);
	}
	else if (!flight_catalog_run(call, type, &value))
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_FEATURE_NOT_SUPPORTED,
					 "unknown Flight SQL ticket \"%s\"", type);
	flight_finish_xact();
	h2_call_finish(call, GRPC_OK, NULL, NULL);
}

static void
put_result(FlightCall *call, const char *type, const ProtobufCMessage *msg)
{
	Arrow__Flight__Protocol__PutResult result = ARROW__FLIGHT__PROTOCOL__PUT_RESULT__INIT;

	(void) type;
	pack(msg, &result.app_metadata);
	flight_send_message(call, &result.base);
}

static void
do_put(FlightCall *call)
{
	StringInfoData msg;
	PbFlightData *first;
	char	   *type;
	ProtobufCBinaryData value;

	read_request(call, &msg);
	first = unpack(&arrow__flight__protocol__flight_data__descriptor, (uint8_t *) msg.data, msg.len,
				   "FlightData");
	if (first->flight_descriptor == NULL)
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION,
					 "DoPut's first FlightData carries no descriptor");
	command_of(&first->flight_descriptor->cmd, &type, &value);
	flight_require_active();

	if (strcmp(type, "CommandStatementUpdate") == 0)
	{
		Arrow__Flight__Protocol__Sql__CommandStatementUpdate *u =
			unpack(&arrow__flight__protocol__sql__command_statement_update__descriptor,
				   value.data, value.len, type);
		Arrow__Flight__Protocol__Sql__DoPutUpdateResult r =
			ARROW__FLIGHT__PROTOCOL__SQL__DO_PUT_UPDATE_RESULT__INIT;
		FlightStatement *stmt;

		flight_check_transaction(u->_transaction_id_case != 0, &u->transaction_id);
		while (h2_call_read(call, &msg))
			;					/* an update has no batches */
		stmt = flight_statement_create(u->query, false);
		r.record_count = flight_statement_update(stmt);
		flight_statement_drop(stmt);
		flight_finish_xact();
		put_result(call, type, &r.base);
	}
	else if (strcmp(type, "CommandPreparedStatementQuery") == 0 ||
			 strcmp(type, "CommandPreparedStatementUpdate") == 0)
	{
		bool		update = strcmp(type, "CommandPreparedStatementUpdate") == 0;
		ProtobufCBinaryData handle;
		FlightStatement *stmt;

		handle = update ?
			((Arrow__Flight__Protocol__Sql__CommandPreparedStatementUpdate *)
			 unpack(&arrow__flight__protocol__sql__command_prepared_statement_update__descriptor,
					value.data, value.len, type))->prepared_statement_handle :
			((Arrow__Flight__Protocol__Sql__CommandPreparedStatementQuery *)
			 unpack(&arrow__flight__protocol__sql__command_prepared_statement_query__descriptor,
					value.data, value.len, type))->prepared_statement_handle;
		stmt = flight_statement_find(&handle, true);
		if (stmt == NULL)
			flight_error(GRPC_NOT_FOUND, ERRCODE_UNDEFINED_PSTATEMENT, "no such prepared statement");
		if (update && flight_ingest_prepared(call, stmt, &first->data_header))
			;					/* one INSERT ... SELECT over its batches (ingest.c) */
		else if (update)
		{
			Arrow__Flight__Protocol__Sql__DoPutUpdateResult r =
				ARROW__FLIGHT__PROTOCOL__SQL__DO_PUT_UPDATE_RESULT__INIT;

			flight_statement_bind(call, stmt, &first->data_header);
			r.record_count = flight_statement_update(stmt);
			flight_finish_xact();
			put_result(call, type, &r.base);
		}
		else
		{
			Arrow__Flight__Protocol__Sql__DoPutPreparedStatementResult r =
				ARROW__FLIGHT__PROTOCOL__SQL__DO_PUT_PREPARED_STATEMENT_RESULT__INIT;

			flight_statement_bind(call, stmt, &first->data_header);
			flight_finish_xact();
			r._prepared_statement_handle_case =
				ARROW__FLIGHT__PROTOCOL__SQL__DO_PUT_PREPARED_STATEMENT_RESULT___PREPARED_STATEMENT_HANDLE_PREPARED_STATEMENT_HANDLE;
			r.prepared_statement_handle = handle;
			put_result(call, type, &r.base);
		}
	}
	else if (strcmp(type, "CommandStatementIngest") == 0)
		flight_ingest(call, &value, &first->data_header);	/* ingest.c */
	else
		flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_FEATURE_NOT_SUPPORTED,
					 "unknown Flight SQL command \"%s\" for DoPut", type);
	h2_call_finish(call, GRPC_OK, NULL, NULL);
}

/* SetSessionOptions: each option as SET, errors per option. */
static void
set_session_options(FlightCall *call, const ProtobufCBinaryData *body)
{
	Arrow__Flight__Protocol__SetSessionOptionsRequest *req =
		unpack(&arrow__flight__protocol__set_session_options_request__descriptor,
			   body->data, body->len, "SetSessionOptionsRequest");
	Arrow__Flight__Protocol__SetSessionOptionsResult result =
		ARROW__FLIGHT__PROTOCOL__SET_SESSION_OPTIONS_RESULT__INIT;
	size_t		i;

	result.errors = palloc0(sizeof(void *) * Max(req->n_session_options, 1));
	for (i = 0; i < req->n_session_options; i++)
	{
		const char *key = req->session_options[i]->key;
		Arrow__Flight__Protocol__SessionOptionValue *v = req->session_options[i]->value;
		StringInfoData value;
		MemoryContext oldcxt = CurrentMemoryContext;
		int			err = 0;

		initStringInfo(&value);
		if (v == NULL)
			err = ARROW__FLIGHT__PROTOCOL__SET_SESSION_OPTIONS_RESULT__ERROR_VALUE__INVALID_VALUE;
		else
			switch (v->option_value_case)
			{
				case ARROW__FLIGHT__PROTOCOL__SESSION_OPTION_VALUE__OPTION_VALUE_STRING_VALUE:
					appendStringInfoString(&value, v->string_value);
					break;
				case ARROW__FLIGHT__PROTOCOL__SESSION_OPTION_VALUE__OPTION_VALUE_BOOL_VALUE:
					appendStringInfoString(&value, v->bool_value ? "on" : "off");
					break;
				case ARROW__FLIGHT__PROTOCOL__SESSION_OPTION_VALUE__OPTION_VALUE_INT64_VALUE:
					appendStringInfo(&value, INT64_FORMAT, (int64) v->int64_value);
					break;
				case ARROW__FLIGHT__PROTOCOL__SESSION_OPTION_VALUE__OPTION_VALUE_DOUBLE_VALUE:
					appendStringInfo(&value, "%.17g", v->double_value);
					break;
				case ARROW__FLIGHT__PROTOCOL__SESSION_OPTION_VALUE__OPTION_VALUE_STRING_LIST_VALUE:
					{
						size_t		j;

						for (j = 0; v->string_list_value && j < v->string_list_value->n_values; j++)
							appendStringInfo(&value, "%s%s", j ? ", " : "",
											 v->string_list_value->values[j]);
						break;
					}
				default:
					err = ARROW__FLIGHT__PROTOCOL__SET_SESSION_OPTIONS_RESULT__ERROR_VALUE__INVALID_VALUE;
			}
		if (err == 0)
		{
			/* SET, in a subtransaction, so that one bad option leaves the others */
			flight_start_xact();
			BeginInternalSubTransaction(NULL);
			PG_TRY();
			{
				if (GetConfigOption(key, true, false) == NULL)
					err = ARROW__FLIGHT__PROTOCOL__SET_SESSION_OPTIONS_RESULT__ERROR_VALUE__INVALID_NAME;
				else
					(void) set_config_option(key, value.data, PGC_USERSET, PGC_S_SESSION,
											 GUC_ACTION_SET, true, ERROR, false);
				ReleaseCurrentSubTransaction();
			}
			PG_CATCH();
			{
				MemoryContextSwitchTo(oldcxt);
				FlushErrorState();
				RollbackAndReleaseCurrentSubTransaction();
				err = ARROW__FLIGHT__PROTOCOL__SET_SESSION_OPTIONS_RESULT__ERROR_VALUE__INVALID_VALUE;
			}
			PG_END_TRY();
			MemoryContextSwitchTo(oldcxt);
		}
		if (err != 0)
		{
			Arrow__Flight__Protocol__SetSessionOptionsResult__ErrorsEntry *e = palloc(sizeof(*e));
			Arrow__Flight__Protocol__SetSessionOptionsResult__Error *ev = palloc(sizeof(*ev));

			arrow__flight__protocol__set_session_options_result__errors_entry__init(e);
			arrow__flight__protocol__set_session_options_result__error__init(ev);
			ev->value = err;
			e->key = (char *) key;
			e->value = ev;
			result.errors[result.n_errors++] = e;
		}
	}
	flight_finish_xact();
	flight_send_result(call, NULL, &result.base);
}

static void
get_session_options(FlightCall *call)
{
	static const char *const names[] = {"vexec.mode", "vexec.batch_format", "search_path",
	"TimeZone", "application_name", "statement_timeout"};
	Arrow__Flight__Protocol__GetSessionOptionsResult result =
		ARROW__FLIGHT__PROTOCOL__GET_SESSION_OPTIONS_RESULT__INIT;
	int			i;

	result.session_options = palloc0(sizeof(void *) * lengthof(names));
	for (i = 0; i < (int) lengthof(names); i++)
	{
		const char *v = GetConfigOption(names[i], true, false);
		Arrow__Flight__Protocol__GetSessionOptionsResult__SessionOptionsEntry *e;
		Arrow__Flight__Protocol__SessionOptionValue *val;

		if (v == NULL)
			continue;
		e = palloc(sizeof(*e));
		val = palloc(sizeof(*val));
		arrow__flight__protocol__get_session_options_result__session_options_entry__init(e);
		arrow__flight__protocol__session_option_value__init(val);
		val->option_value_case = ARROW__FLIGHT__PROTOCOL__SESSION_OPTION_VALUE__OPTION_VALUE_STRING_VALUE;
		val->string_value = pstrdup(v);
		e->key = (char *) names[i];
		e->value = val;
		result.session_options[result.n_session_options++] = e;
	}
	flight_send_result(call, NULL, &result.base);
}

static void
do_action(FlightCall *call)
{
	StringInfoData msg;
	PbAction   *action;
	const char *t;
	char	   *type;
	ProtobufCBinaryData value;

	read_request(call, &msg);
	action = unpack(&arrow__flight__protocol__action__descriptor, (uint8_t *) msg.data, msg.len, "Action");
	t = action->type ? action->type : "";

	/* Flight's own actions: their bodies are not packed in an Any */
	if (strcmp(t, "CancelFlightInfo") == 0)
	{
		Arrow__Flight__Protocol__CancelFlightInfoRequest *req =
			unpack(&arrow__flight__protocol__cancel_flight_info_request__descriptor,
				   action->body.data, action->body.len, "CancelFlightInfoRequest");
		Arrow__Flight__Protocol__CancelFlightInfoResult result =
			ARROW__FLIGHT__PROTOCOL__CANCEL_FLIGHT_INFO_RESULT__INIT;

		flight_start_xact();
		result.status = cancel_flight_info(req->info);
		flight_finish_xact();
		flight_send_result(call, NULL, &result.base);
	}
	else if (strcmp(t, "SetSessionOptions") == 0)
		set_session_options(call, &action->body);
	else if (strcmp(t, "GetSessionOptions") == 0)
		get_session_options(call);
	else if (strcmp(t, "CloseSession") == 0)
	{
		Arrow__Flight__Protocol__CloseSessionResult result =
			ARROW__FLIGHT__PROTOCOL__CLOSE_SESSION_RESULT__INIT;

		result.status = ARROW__FLIGHT__PROTOCOL__CLOSE_SESSION_RESULT__STATUS__CLOSED;
		flight_send_result(call, NULL, &result.base);
		flight_session->closing = true;
	}
	else
	{
		/* Flight SQL's: an Any each */
		command_of(&action->body, &type, &value);
		if (strcmp(t, "CreatePreparedStatement") == 0)
		{
			Arrow__Flight__Protocol__Sql__ActionCreatePreparedStatementRequest *req =
				unpack(&arrow__flight__protocol__sql__action_create_prepared_statement_request__descriptor,
					   value.data, value.len, t);
			Arrow__Flight__Protocol__Sql__ActionCreatePreparedStatementResult result =
				ARROW__FLIGHT__PROTOCOL__SQL__ACTION_CREATE_PREPARED_STATEMENT_RESULT__INIT;
			FlightStatement *stmt;
			StringInfoData ds;
			StringInfoData ps;

			flight_require_active();
			flight_check_transaction(req->_transaction_id_case != 0, &req->transaction_id);
			stmt = flight_statement_create(req->query, true);
			initStringInfo(&ds);
			initStringInfo(&ps);
			flight_statement_schema(stmt, &ds);
			flight_statement_parameter_schema(stmt, &ps);
			flight_finish_xact();
			flight_statement_handle(stmt, &result.prepared_statement_handle);
			result.dataset_schema.data = (uint8_t *) ds.data;
			result.dataset_schema.len = ds.len;
			result.parameter_schema.data = (uint8_t *) ps.data;
			result.parameter_schema.len = ps.len;
			result._is_update_case =
				ARROW__FLIGHT__PROTOCOL__SQL__ACTION_CREATE_PREPARED_STATEMENT_RESULT___IS_UPDATE_IS_UPDATE;
			result.is_update = stmt->result == NULL;
			flight_send_result(call, "ActionCreatePreparedStatementResult", &result.base);
		}
		else if (strcmp(t, "ClosePreparedStatement") == 0)
		{
			Arrow__Flight__Protocol__Sql__ActionClosePreparedStatementRequest *req =
				unpack(&arrow__flight__protocol__sql__action_close_prepared_statement_request__descriptor,
					   value.data, value.len, t);
			FlightStatement *stmt = flight_statement_find(&req->prepared_statement_handle, true);

			if (stmt)
				flight_statement_drop(stmt);
		}
		else if (strcmp(t, "BeginTransaction") == 0)
		{
			Arrow__Flight__Protocol__Sql__ActionBeginTransactionResult result =
				ARROW__FLIGHT__PROTOCOL__SQL__ACTION_BEGIN_TRANSACTION_RESULT__INIT;

			flight_require_active();
			if (flight_session->in_transaction)
				flight_error(GRPC_FAILED_PRECONDITION, ERRCODE_ACTIVE_SQL_TRANSACTION,
							 "a Flight transaction is open in this session already");
			flight_finish_xact();
			flight_sql_command("BEGIN");
			if (!pg_strong_random(flight_session->transaction_id, sizeof(flight_session->transaction_id)))
				elog(ERROR, "vexec_flight could not make a transaction id");
			flight_session->in_transaction = true;
			flight_session->savepoints = NIL;
			result.transaction_id.data = flight_session->transaction_id;
			result.transaction_id.len = sizeof(flight_session->transaction_id);
			flight_send_result(call, "ActionBeginTransactionResult", &result.base);
		}
		else if (strcmp(t, "EndTransaction") == 0)
		{
			Arrow__Flight__Protocol__Sql__ActionEndTransactionRequest *req =
				unpack(&arrow__flight__protocol__sql__action_end_transaction_request__descriptor,
					   value.data, value.len, t);

			flight_check_transaction(true, &req->transaction_id);
			if (req->action == ARROW__FLIGHT__PROTOCOL__SQL__ACTION_END_TRANSACTION_REQUEST__END_TRANSACTION__END_TRANSACTION_COMMIT)
				flight_sql_command("COMMIT");
			else if (req->action == ARROW__FLIGHT__PROTOCOL__SQL__ACTION_END_TRANSACTION_REQUEST__END_TRANSACTION__END_TRANSACTION_ROLLBACK)
				flight_sql_command("ROLLBACK");
			else
				flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_INVALID_PARAMETER_VALUE,
							 "EndTransaction names neither COMMIT nor ROLLBACK");
			flight_session->in_transaction = false;
			flight_session->savepoints = NIL;
		}
		else if (strcmp(t, "BeginSavepoint") == 0)
		{
			Arrow__Flight__Protocol__Sql__ActionBeginSavepointRequest *req =
				unpack(&arrow__flight__protocol__sql__action_begin_savepoint_request__descriptor,
					   value.data, value.len, t);
			Arrow__Flight__Protocol__Sql__ActionBeginSavepointResult result =
				ARROW__FLIGHT__PROTOCOL__SQL__ACTION_BEGIN_SAVEPOINT_RESULT__INIT;
			MemoryContext old;

			flight_check_transaction(true, &req->transaction_id);
			flight_sql_command(psprintf("SAVEPOINT %s", quote_identifier(req->name)));
			old = MemoryContextSwitchTo(flight_session->mcxt);
			flight_session->savepoints = lcons(pstrdup(req->name), flight_session->savepoints);
			MemoryContextSwitchTo(old);
			result.savepoint_id.data = (uint8_t *) req->name;
			result.savepoint_id.len = strlen(req->name);
			flight_send_result(call, "ActionBeginSavepointResult", &result.base);
		}
		else if (strcmp(t, "EndSavepoint") == 0)
		{
			Arrow__Flight__Protocol__Sql__ActionEndSavepointRequest *req =
				unpack(&arrow__flight__protocol__sql__action_end_savepoint_request__descriptor,
					   value.data, value.len, t);
			char	   *name = pnstrdup((const char *) req->savepoint_id.data, req->savepoint_id.len);

			if (!flight_session->in_transaction)
				flight_error(GRPC_FAILED_PRECONDITION, ERRCODE_NO_ACTIVE_SQL_TRANSACTION,
							 "no Flight transaction is open in this session");
			if (req->action == ARROW__FLIGHT__PROTOCOL__SQL__ACTION_END_SAVEPOINT_REQUEST__END_SAVEPOINT__END_SAVEPOINT_RELEASE)
				flight_sql_command(psprintf("RELEASE SAVEPOINT %s", quote_identifier(name)));
			else if (req->action == ARROW__FLIGHT__PROTOCOL__SQL__ACTION_END_SAVEPOINT_REQUEST__END_SAVEPOINT__END_SAVEPOINT_ROLLBACK)
				flight_sql_command(psprintf("ROLLBACK TO SAVEPOINT %s", quote_identifier(name)));
			else
				flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_INVALID_PARAMETER_VALUE,
							 "EndSavepoint names neither RELEASE nor ROLLBACK");
		}
		else if (strcmp(t, "CancelQuery") == 0)
		{
			Arrow__Flight__Protocol__Sql__ActionCancelQueryRequest *req =
				unpack(&arrow__flight__protocol__sql__action_cancel_query_request__descriptor,
					   value.data, value.len, t);
			PbFlightInfo *info = unpack(&arrow__flight__protocol__flight_info__descriptor,
										req->info.data, req->info.len, "FlightInfo");
			Arrow__Flight__Protocol__Sql__ActionCancelQueryResult result =
				ARROW__FLIGHT__PROTOCOL__SQL__ACTION_CANCEL_QUERY_RESULT__INIT;
			int			status;

			flight_start_xact();
			status = cancel_flight_info(info);
			flight_finish_xact();
			/* CancelResult's values follow CancelStatus's */
			result.result = status;
			flight_send_result(call, "ActionCancelQueryResult", &result.base);
		}
		else
			flight_error(GRPC_UNIMPLEMENTED, ERRCODE_FEATURE_NOT_SUPPORTED,
						 "the action \"%s\" is not served", t);
	}
	h2_call_finish(call, GRPC_OK, NULL, NULL);
}

static void
list_actions(FlightCall *call)
{
	static const char *const actions[][2] = {
		{"CreatePreparedStatement", "Flight SQL: prepare a statement"},
		{"ClosePreparedStatement", "Flight SQL: close a prepared statement"},
		{"BeginTransaction", "Flight SQL: begin a transaction"},
		{"EndTransaction", "Flight SQL: commit or roll back the transaction"},
		{"BeginSavepoint", "Flight SQL: make a savepoint"},
		{"EndSavepoint", "Flight SQL: release or roll back to a savepoint"},
		{"CancelQuery", "Flight SQL: cancel a query (deprecated: CancelFlightInfo)"},
		{"CancelFlightInfo", "cancel the statement a FlightInfo names"},
		{"SetSessionOptions", "set the session's settings"},
		{"GetSessionOptions", "the session's settings"},
		{"CloseSession", "end the session"},
	};
	int			i;

	for (i = 0; i < (int) lengthof(actions); i++)
	{
		Arrow__Flight__Protocol__ActionType at = ARROW__FLIGHT__PROTOCOL__ACTION_TYPE__INIT;

		at.type = (char *) actions[i][0];
		at.description = (char *) actions[i][1];
		flight_send_message(call, &at.base);
	}
	h2_call_finish(call, GRPC_OK, NULL, NULL);
}

/* A call, by its method. */
void
flight_dispatch(FlightCall *call)
{
	const char *path = h2_call_path(call);
	const char *ct = h2_call_header(call, "content-type");
	const char *method;

	if (ct == NULL || strncmp(ct, "application/grpc", 16) != 0)
	{
		h2_call_finish(call, GRPC_UNIMPLEMENTED, "the endpoint serves gRPC only", NULL);
		return;
	}
	if (strncmp(path, FLIGHT_SERVICE, strlen(FLIGHT_SERVICE)) != 0)
	{
		h2_call_finish(call, GRPC_UNIMPLEMENTED, "the endpoint serves Arrow Flight's service only", NULL);
		return;
	}
	method = path + strlen(FLIGHT_SERVICE);
	if (strcmp(method, "Handshake") == 0)
		handshake(call);
	else if (strcmp(method, "GetFlightInfo") == 0)
		get_flight_info(call, false);
	else if (strcmp(method, "PollFlightInfo") == 0)
		get_flight_info(call, true);
	else if (strcmp(method, "GetSchema") == 0)
		get_schema(call);
	else if (strcmp(method, "DoGet") == 0)
		do_get(call);
	else if (strcmp(method, "DoPut") == 0)
		do_put(call);
	else if (strcmp(method, "DoAction") == 0)
		do_action(call);
	else if (strcmp(method, "ListActions") == 0)
		list_actions(call);
	else if (strcmp(method, "ListFlights") == 0)
		h2_call_finish(call, GRPC_OK, NULL, NULL);	/* no flights to list */
	else
		h2_call_finish(call, GRPC_UNIMPLEMENTED,
					   psprintf("the method \"%s\" is not served", method), NULL);
}
