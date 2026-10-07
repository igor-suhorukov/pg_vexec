/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * session.h
 *	  What a Flight session's parts share: session.c's state and loop,
 *	  rpc.c's methods, sql.c's statements, catalog.c's commands.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_FLIGHT_SESSION_H
#define VEXEC_FLIGHT_SESSION_H

#include "nodes/params.h"
#include "utils/hsearch.h"
#include "utils/plancache.h"

#include <protobuf-c/protobuf-c.h>

#include "Flight.pb-c.h"
#include "FlightSql.pb-c.h"

#include "flight.h"
#include "h2.h"

/* Flight SQL's message names, as an Any's type URL ends. */
#define FLIGHT_SQL_TYPE_PREFIX	"type.googleapis.com/arrow.flight.protocol.sql."

/*
 * A statement the session holds: a query planned at GetFlightInfo and run at
 * DoGet, or a prepared statement, with the parameter sets DoPut bound.
 */
typedef struct FlightStatement
{
	uint64		handle;			/* the hash key */
	bool		prepared;
	MemoryContext mcxt;
	char	   *query;
	CachedPlanSource *plansource;
	TupleDesc	result;			/* NULL: the statement returns no rows */
	int			nparams;
	Oid		   *paramtypes;
	List	   *paramsets;		/* ParamListInfo, one a set DoPut bound */
} FlightStatement;

typedef struct FlightSession
{
	FlightConn *conn;
	FlightTls  *tls;
	int			sock;
	int			slot;
	uint64		session_id;
	const VexecEgressRoutine *egress;
	bool		logged_in;
	char		bearer[80];		/* "Bearer <hex>" */
	char		user[NAMEDATALEN];
	char		database[NAMEDATALEN];
	MemoryContext mcxt;			/* the session's */
	MemoryContext callcxt;		/* a call's, reset after it */
	FlightCall *call;			/* the call being served */
	HTAB	   *statements;
	uint64		next_handle;
	bool		xact_started;	/* a transaction command is open */
	bool		in_transaction; /* a Flight transaction is open */
	uint8		transaction_id[16];
	List	   *savepoints;		/* char *, the names, newest first */
	uint64		running;		/* the statement handle being run */
	bool		closing;		/* CloseSession: end after this call */
} FlightSession;

extern FlightSession *flight_session;

/* session.c */
extern void flight_start_xact(void);
extern void flight_finish_xact(void);
extern void flight_require_active(void);
extern void flight_statement_begins(const char *query, uint64 handle);
extern void flight_statement_ends(void);
extern bool flight_is_bearer(FlightCall *call);
pg_noreturn extern void flight_error(int grpc_status, int sqlerrcode, const char *fmt,...)
			pg_attribute_printf(3, 4);

/* rpc.c */
extern void flight_dispatch(FlightCall *call);
extern void flight_send_message(FlightCall *call, const ProtobufCMessage *msg);
extern void flight_send_result(FlightCall *call, const char *type, const ProtobufCMessage *msg);
extern void flight_egress_write(void *arg, const VexecEgressMessage *msg);
extern void flight_any_pack(const char *type, const ProtobufCMessage *msg, ProtobufCBinaryData *out);
extern bool flight_any_unpack(const uint8_t *data, size_t len, char **type, ProtobufCBinaryData *value);
extern ProtobufCAllocator *flight_pb_allocator(void);
extern void flight_schema_bytes(TupleDesc desc, const VexecEgressField *fields, int nfields,
								StringInfo out);

/* sql.c */
extern FlightStatement *flight_statement_create(const char *query, bool prepared);
extern FlightStatement *flight_statement_find(const ProtobufCBinaryData *handle, bool prepared);
extern void flight_statement_drop(FlightStatement *stmt);
extern void flight_statement_handle(FlightStatement *stmt, ProtobufCBinaryData *out);
extern void flight_statement_schema(FlightStatement *stmt, StringInfo out);
extern void flight_statement_parameter_schema(FlightStatement *stmt, StringInfo out);
extern void flight_statement_run(FlightCall *call, FlightStatement *stmt);
extern int64 flight_statement_update(FlightStatement *stmt);
extern void flight_statement_bind(FlightCall *call, FlightStatement *stmt,
								  const ProtobufCBinaryData *schema_header);
extern void flight_check_transaction(bool has_id, const ProtobufCBinaryData *id);
extern void flight_sql_command(const char *sql);

/* ingest.c */
extern void flight_ingest(FlightCall *call, const ProtobufCBinaryData *command,
						  const ProtobufCBinaryData *first_header);
extern bool flight_ingest_prepared(FlightCall *call, FlightStatement *stmt,
								   const ProtobufCBinaryData *first_header);
extern void flight_run_sql(FlightCall *call, const char *sql, int nargs, const Oid *argtypes,
						   const Datum *args, const bool *nulls,
						   const VexecEgressField *fields, int nfields,
						   DestReceiver *(*wrap) (DestReceiver *egress, void *arg), void *wrap_arg);
extern VexecEgressField *flight_sql_fields(TupleDesc desc);

/* catalog.c */
extern bool flight_catalog_schema(const char *type, ProtobufCBinaryData *value, StringInfo out);
extern bool flight_catalog_run(FlightCall *call, const char *type, ProtobufCBinaryData *value);

#endif							/* VEXEC_FLIGHT_SESSION_H */
