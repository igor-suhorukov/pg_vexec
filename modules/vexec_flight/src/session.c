/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * session.c
 *	  A Flight session: one dynamic background worker with one thread, which
 *	  owns its client's socket as a backend does (pg_vector_executor.md
 *	  §3.15).
 *
 * The session takes its client's socket from the acceptor, does TLS when
 * the client starts it, serves HTTP/2 and gRPC on the socket itself (h2.c),
 * and runs one call at a time, as a backend runs one statement at a time.
 *
 * Logging in.  The client's first call carries its credentials: Flight's
 * basic authentication, "authorization: Basic ..." on a Handshake or any
 * other call, and the database in a "database" header (the user's name
 * otherwise, as libpq's default is).  The acceptor checks them (login.c) --
 * pg_hba.conf with the client's address and TLS, the password, the role's
 * and the database's limits -- and the session then connects to that role
 * and database (BackgroundWorkerInitializeConnectionByOid,
 * PG19:src/include/postmaster/bgworker.h:157), with MyProcPort describing
 * its client, so that pg_stat_activity shows the client's address and
 * pg_stat_ssl its TLS.  The answer to the login carries a bearer token,
 * which the client's later calls show.
 *
 * Statements go through portals, as a backend's do: parse, analysis,
 * planning, a portal, the executor, its result to vexec's receiver (sql.c).
 * So hooks, permissions, row-level security, settings, transactions,
 * prepared statements and pg_stat_statements behave as over PostgreSQL's
 * protocol.  Each runs in a transaction of its own, unless a Flight
 * transaction is open, as an implicit transaction does
 * (PG19:src/backend/tcop/postgres.c, start_xact_command and
 * finish_xact_command).
 *
 * Errors.  An error in a call is the call's: the loop catches it as
 * PostgresMain does (postgres.c, its sigsetjmp block), aborts the
 * transaction, and ends the call with gRPC's status for the SQLSTATE, the
 * message, and the SQLSTATE in a trailer.  A FATAL ends the session; the
 * call under way ends with UNAVAILABLE first, when its stream allows.
 *
 * Cancels.  pg_cancel_backend() and pg_terminate_backend() signal the
 * session as any worker; CancelFlightInfo from another session finds it in
 * the shared table and signals it the same way.  A cancel ends the
 * statement under way; between statements it means nothing, as for a
 * backend.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "access/xact.h"
#include "common/base64.h"
#include "common/ip.h"
#include "jit/jit.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "replication/slot.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "flight.h"
#include "h2.h"
#include "session.h"

FlightSession *flight_session = NULL;

/* The gRPC status an error sends, when its raiser named one. */
static int	error_grpc_status = -1;

/* ---------------------------------------------------------------------
 * The slot, and the acceptor's link
 * ---------------------------------------------------------------------
 */

static void
release_slot(int code, Datum arg)
{
	int			slot = DatumGetInt32(arg);

	(void) code;
	SpinLockAcquire(&flight_shared->mutex);
	if (flight_shared->slots[slot].pid == MyProcPid)
		memset(&flight_shared->slots[slot], 0, sizeof(FlightSlot));
	SpinLockRelease(&flight_shared->mutex);
}

/* Wait, with the latch, for a socket's events: false when time ran out. */
static bool
wait_socket(int fd, int events, int timeout_ms)
{
	int			rc = WaitLatchOrSocket(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH | WL_TIMEOUT | events,
									   fd, timeout_ms, WAIT_EVENT_CLIENT_READ);

	if (rc & WL_LATCH_SET)
	{
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	return (rc & events) != 0;
}

/* All of len bytes from a blocking-free socket, within timeout_ms. */
static bool
read_full(int fd, void *buf, size_t len, int timeout_ms)
{
	size_t		got = 0;
	TimestampTz deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), timeout_ms);

	while (got < len)
	{
		ssize_t		n = recv(fd, (char *) buf + got, len - got, MSG_DONTWAIT);

		if (n > 0)
		{
			got += n;
			continue;
		}
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			return false;
		if (TimestampDifferenceMilliseconds(GetCurrentTimestamp(), deadline) <= 0)
			return false;
		(void) wait_socket(fd, WL_SOCKET_READABLE,
						   (int) TimestampDifferenceMilliseconds(GetCurrentTimestamp(), deadline));
	}
	return true;
}

/*
 * The client's socket, from the acceptor: connect to its UNIX-domain
 * socket, show the slot's token, take the socket.  The link stays open for
 * the login.
 */
static int
take_client_socket(int slot, uint64 token, int *link_out)
{
	struct sockaddr_un addr;
	FlightHello hello;
	int			link;
	struct msghdr msg;
	struct iovec iov;
	char		ok;
	union
	{
		struct cmsghdr hdr;
		char		buf[CMSG_SPACE(sizeof(int))];
	}			control;
	struct cmsghdr *cmsg;
	int			fd = -1;
	TimestampTz deadline;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), FLIGHT_SOCKET_NAME, flight_port);
	link = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (link < 0 || connect(link, (struct sockaddr *) &addr, sizeof(addr)) < 0)
		ereport(FATAL,
				(errcode_for_socket_access(),
				 errmsg("vexec_flight session could not reach the acceptor at \"%s\": %m",
						addr.sun_path)));
	hello.magic = FLIGHT_MAGIC;
	hello.slot = slot;
	hello.token = token;
	if (send(link, &hello, sizeof(hello), MSG_NOSIGNAL) != sizeof(hello))
		ereport(FATAL, (errmsg("vexec_flight session could not greet the acceptor: %m")));

	deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 10000);
	for (;;)
	{
		ssize_t		n;
		long		remaining;

		memset(&msg, 0, sizeof(msg));
		iov.iov_base = &ok;
		iov.iov_len = 1;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control.buf;
		msg.msg_controllen = sizeof(control.buf);
		n = recvmsg(link, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (n == 1)
			break;
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			ereport(FATAL, (errmsg("vexec_flight session: the acceptor closed its link")));
		remaining = TimestampDifferenceMilliseconds(GetCurrentTimestamp(), deadline);
		if (remaining <= 0)
			ereport(FATAL, (errmsg("vexec_flight session: the acceptor did not hand over its client")));
		(void) wait_socket(link, WL_SOCKET_READABLE, (int) remaining);
	}
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
			memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
	if (fd < 0)
		ereport(FATAL, (errmsg("vexec_flight session: the acceptor sent no socket")));
	*link_out = link;
	return fd;
}

/* ---------------------------------------------------------------------
 * Transactions and statements
 * ---------------------------------------------------------------------
 */

/*
 * A transaction command begun, as postgres.c's start_xact_command() begins
 * one; the memory context current before it stays current, so that what a
 * call builds for its response outlives the command's commit.
 */
void
flight_start_xact(void)
{
	if (!flight_session->xact_started)
	{
		MemoryContext old = CurrentMemoryContext;

		StartTransactionCommand();
		MemoryContextSwitchTo(old);
		flight_session->xact_started = true;
	}
	if (StatementTimeout > 0 && (StatementTimeout < TransactionTimeout || TransactionTimeout == 0))
	{
		if (!get_timeout_active(STATEMENT_TIMEOUT))
			enable_timeout_after(STATEMENT_TIMEOUT, StatementTimeout);
	}
	else if (get_timeout_active(STATEMENT_TIMEOUT))
		disable_timeout(STATEMENT_TIMEOUT, false);
}

/* The transaction command committed; the call's memory current again. */
void
flight_finish_xact(void)
{
	if (get_timeout_active(STATEMENT_TIMEOUT))
		disable_timeout(STATEMENT_TIMEOUT, false);
	if (flight_session->xact_started)
	{
		CommitTransactionCommand();
		flight_session->xact_started = false;
		MemoryContextSwitchTo(flight_session->callcxt);
	}
}

/* Level 2 of §3.15: a statement runs only while the vector executor is active. */
void
flight_require_active(void)
{
	bool		active;

	flight_start_xact();
	active = flight_session->egress->active();
	if (!active)
		flight_error(GRPC_FAILED_PRECONDITION, ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
					 "the vector executor is not active in this session: \"vexec.mode\" is \"%s\"",
					 GetConfigOption("vexec.mode", true, false));
}

/* pg_stat_activity, the shared table, and the log's statement. */
void
flight_statement_begins(const char *query, uint64 handle)
{
	debug_query_string = query;
	pgstat_report_activity(STATE_RUNNING, query);
	set_ps_display("Flight");
	flight_session->running = handle;
	SpinLockAcquire(&flight_shared->mutex);
	flight_shared->slots[flight_session->slot].running = handle;
	SpinLockRelease(&flight_shared->mutex);
}

void
flight_statement_ends(void)
{
	debug_query_string = NULL;
	flight_session->running = 0;
	SpinLockAcquire(&flight_shared->mutex);
	flight_shared->slots[flight_session->slot].running = 0;
	SpinLockRelease(&flight_shared->mutex);
}

/* An error with a gRPC status of its own, beside its SQLSTATE. */
void
flight_error(int grpc_status, int sqlerrcode, const char *fmt,...)
{
	char		buf[1024];
	va_list		args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	error_grpc_status = grpc_status;
	ereport(ERROR, (errcode(sqlerrcode), errmsg_internal("%s", buf)));
	pg_unreachable();
}

/* gRPC's status for a SQLSTATE. */
static int
grpc_status_for(int sqlerrcode)
{
	switch (sqlerrcode)
	{
		case ERRCODE_QUERY_CANCELED:
			return GRPC_CANCELLED;
		case ERRCODE_INSUFFICIENT_PRIVILEGE:
			return GRPC_PERMISSION_DENIED;
		case ERRCODE_UNIQUE_VIOLATION:
		case ERRCODE_DUPLICATE_OBJECT:
		case ERRCODE_DUPLICATE_TABLE:
		case ERRCODE_DUPLICATE_DATABASE:
		case ERRCODE_DUPLICATE_SCHEMA:
		case ERRCODE_DUPLICATE_FUNCTION:
		case ERRCODE_DUPLICATE_PSTATEMENT:
			return GRPC_ALREADY_EXISTS;
		case ERRCODE_UNDEFINED_TABLE:
		case ERRCODE_UNDEFINED_OBJECT:
		case ERRCODE_UNDEFINED_SCHEMA:
		case ERRCODE_UNDEFINED_FUNCTION:
		case ERRCODE_UNDEFINED_DATABASE:
		case ERRCODE_UNDEFINED_PSTATEMENT:
			return GRPC_NOT_FOUND;
		case ERRCODE_FEATURE_NOT_SUPPORTED:
			return GRPC_UNIMPLEMENTED;
		case ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE:
			return GRPC_FAILED_PRECONDITION;
		case ERRCODE_T_R_SERIALIZATION_FAILURE:
		case ERRCODE_T_R_DEADLOCK_DETECTED:
			return GRPC_ABORTED;
		case ERRCODE_ADMIN_SHUTDOWN:
		case ERRCODE_CRASH_SHUTDOWN:
		case ERRCODE_CANNOT_CONNECT_NOW:
			return GRPC_UNAVAILABLE;
	}
	switch (ERRCODE_TO_CATEGORY(sqlerrcode))
	{
		case ERRCODE_TO_CATEGORY(ERRCODE_DATA_EXCEPTION):
		case ERRCODE_TO_CATEGORY(ERRCODE_SYNTAX_ERROR_OR_ACCESS_RULE_VIOLATION):
			return GRPC_INVALID_ARGUMENT;
		case ERRCODE_TO_CATEGORY(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION):
		case ERRCODE_TO_CATEGORY(ERRCODE_INVALID_TRANSACTION_STATE):
			return GRPC_FAILED_PRECONDITION;
		case ERRCODE_TO_CATEGORY(ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION):
			return GRPC_UNAUTHENTICATED;
		case ERRCODE_TO_CATEGORY(ERRCODE_INSUFFICIENT_RESOURCES):
		case ERRCODE_TO_CATEGORY(ERRCODE_PROGRAM_LIMIT_EXCEEDED):
			return GRPC_RESOURCE_EXHAUSTED;
		case ERRCODE_TO_CATEGORY(ERRCODE_TRANSACTION_ROLLBACK):
			return GRPC_ABORTED;
		case ERRCODE_TO_CATEGORY(ERRCODE_CONNECTION_EXCEPTION):
		case ERRCODE_TO_CATEGORY(ERRCODE_OPERATOR_INTERVENTION):
			return GRPC_UNAVAILABLE;
		case ERRCODE_TO_CATEGORY(ERRCODE_INTERNAL_ERROR):
			return GRPC_INTERNAL;
	}
	return GRPC_UNKNOWN;
}

/* An error's text for grpc-message: its message, detail and hint. */
static char *
error_text(ErrorData *edata)
{
	StringInfoData s;

	initStringInfo(&s);
	appendStringInfoString(&s, edata->message ? edata->message : "an error");
	if (edata->detail)
		appendStringInfo(&s, "\nDETAIL: %s", edata->detail);
	if (edata->hint)
		appendStringInfo(&s, "\nHINT: %s", edata->hint);
	appendStringInfo(&s, "\nSQLSTATE: %s", unpack_sql_state(edata->sqlerrcode));
	return s.data;
}

/* ---------------------------------------------------------------------
 * Logging in
 * ---------------------------------------------------------------------
 */

static int	acceptor_link = -1;

/* The user and password of "authorization: Basic <base64>", or false. */
static bool
basic_credentials(const char *header, char *user, char *password)
{
	char	   *b64;
	char	   *decoded;
	int			len;
	char	   *colon;

	if (header == NULL || pg_strncasecmp(header, "Basic ", 6) != 0)
		return false;
	/* padded to a multiple of 4: some clients send base64 without its "=" */
	b64 = psprintf("%s%.*s", header + 6, (int) ((4 - strlen(header + 6) % 4) % 4), "===");
	decoded = palloc(pg_b64_dec_len(strlen(b64)) + 1);
	len = pg_b64_decode(b64, strlen(b64), (uint8 *) decoded, pg_b64_dec_len(strlen(b64)));
	if (len < 0)
		return false;
	decoded[len] = '\0';
	colon = strchr(decoded, ':');
	if (colon == NULL || colon - decoded >= NAMEDATALEN ||
		strlen(colon + 1) >= FLIGHT_MAX_PASSWORD || memchr(decoded, '\0', len) != NULL)
		return false;
	memcpy(user, decoded, colon - decoded);
	user[colon - decoded] = '\0';
	strlcpy(password, colon + 1, FLIGHT_MAX_PASSWORD);
	explicit_bzero(decoded, len);
	return true;
}

/* A Port describing the client, for hba's lines and pg_stat_activity. */
static Port *
client_port(void)
{
	Port	   *port = MemoryContextAllocZero(TopMemoryContext, sizeof(Port));
	char		host[NI_MAXHOST];
	char		service[NI_MAXSERV];

	port->sock = flight_session->sock;
	port->raddr.salen = sizeof(port->raddr.addr);
	(void) getpeername(port->sock, (struct sockaddr *) &port->raddr.addr, &port->raddr.salen);
	port->laddr.salen = sizeof(port->laddr.addr);
	(void) getsockname(port->sock, (struct sockaddr *) &port->laddr.addr, &port->laddr.salen);
	host[0] = service[0] = '\0';
	(void) pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen, host, sizeof(host),
							  service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV);
	port->remote_host = MemoryContextStrdup(TopMemoryContext, host);
	port->remote_port = MemoryContextStrdup(TopMemoryContext, service);
	port->user_name = flight_session->user;
	port->database_name = flight_session->database;
	if (flight_session->tls)
	{
		/* pg_stat_ssl reads the connection's TLS through it */
		port->ssl_in_use = true;
		port->ssl = flight_tls_ssl(flight_session->tls);
	}
	return port;
}

/*
 * Log in with a call's credentials: the acceptor checks them, and the
 * session connects.  False, the call finished, when they are refused or
 * missing.
 */
static bool
log_in(FlightCall *call)
{
	FlightLoginRequest req;
	FlightLoginReply reply;
	const char *database = h2_call_header(call, "database");
	const char *appname = h2_call_header(call, "application_name");
	Port	   *port;
	uint8		token[32];
	char		hex[65];
	int			i;

	memset(&req, 0, sizeof(req));
	if (!basic_credentials(h2_call_header(call, "authorization"), req.user, req.password))
	{
		h2_call_finish(call, GRPC_UNAUTHENTICATED,
					   "log in with Flight's basic authentication: \"authorization: Basic ...\"", NULL);
		return false;
	}
	strlcpy(req.database, database ? database : req.user, sizeof(req.database));
	req.magic = FLIGHT_MAGIC;
	req.slot = flight_session->slot;
	req.ssl = flight_session->tls != NULL;
	{
		socklen_t	len = sizeof(req.addr);

		(void) getpeername(flight_session->sock, (struct sockaddr *) &req.addr, &len);
		req.addrlen = len;
	}
	if (send(acceptor_link, &req, sizeof(req), MSG_NOSIGNAL) != sizeof(req) ||
		!read_full(acceptor_link, &reply, sizeof(reply), 60000) ||
		reply.magic != FLIGHT_MAGIC)
		ereport(FATAL, (errmsg("vexec_flight session lost its link to the acceptor")));
	explicit_bzero(req.password, sizeof(req.password));
	if (!reply.ok)
	{
		reply.message[sizeof(reply.message) - 1] = '\0';
		reply.sqlstate[5] = '\0';
		h2_call_finish(call, reply.grpc_status, reply.message, reply.sqlstate);
		return false;
	}

	strlcpy(flight_session->user, req.user, NAMEDATALEN);
	strlcpy(flight_session->database, req.database, NAMEDATALEN);
	port = client_port();
	MyProcPort = port;
	SpinLockAcquire(&flight_shared->mutex);
	flight_shared->slots[flight_session->slot].roleid = reply.roleid;
	flight_shared->slots[flight_session->slot].dboid = reply.dboid;
	flight_shared->slots[flight_session->slot].state = FLIGHT_SLOT_ACTIVE;
	SpinLockRelease(&flight_shared->mutex);
	close(acceptor_link);
	acceptor_link = -1;

	BackgroundWorkerInitializeConnectionByOid(reply.dboid, reply.roleid, 0);
	if (appname)
		SetConfigOption("application_name", appname, PGC_USERSET, PGC_S_CLIENT);
	pgstat_report_activity(STATE_IDLE, NULL);
	set_ps_display(psprintf("%s %s %s", req.user, req.database, port->remote_host));

	if (!pg_strong_random(token, sizeof(token)))
		ereport(FATAL, (errmsg("vexec_flight could not make a session token")));
	for (i = 0; i < 32; i++)
		snprintf(hex + i * 2, 3, "%02x", token[i]);
	snprintf(flight_session->bearer, sizeof(flight_session->bearer), "Bearer %s", hex);
	flight_session->logged_in = true;
	return true;
}

/* Whether a call shows the session's bearer token, or its user's basic credentials. */
bool
flight_is_bearer(FlightCall *call)
{
	const char *auth = h2_call_header(call, "authorization");
	char		user[NAMEDATALEN];
	char		password[FLIGHT_MAX_PASSWORD];

	if (auth == NULL)
		return false;
	if (strcmp(auth, flight_session->bearer) == 0)
		return true;
	/* a client that sends its credentials with every call: the same user */
	if (basic_credentials(auth, user, password))
	{
		explicit_bzero(password, sizeof(password));
		return strcmp(user, flight_session->user) == 0;
	}
	return false;
}

/* ---------------------------------------------------------------------
 * The session's end
 * ---------------------------------------------------------------------
 */

static void
session_exit(int code, Datum arg)
{
	FlightSession *s = flight_session;

	(void) code;
	(void) arg;
	if (s == NULL || s->conn == NULL)
		return;
	/*
	 * A FATAL -- a terminate, the postmaster's shutdown -- ends the call under
	 * way with UNAVAILABLE, when its stream can still carry trailers.
	 */
	if (s->call && !h2_call_gone(s->call))
		h2_call_finish(s->call, GRPC_UNAVAILABLE,
					   "terminating connection due to administrator command", "57P01");
	h2_conn_shutdown(s->conn, 1000);
	s->conn = NULL;
}

/* ---------------------------------------------------------------------
 * The session
 * ---------------------------------------------------------------------
 */

/* TLS or plaintext, by the client's first byte: 0x16 begins TLS's handshake. */
static bool
start_tls(int sock)
{
	char		first;
	TimestampTz deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
													   AuthenticationTimeout * 1000);

	for (;;)
	{
		ssize_t		n = recv(sock, &first, 1, MSG_PEEK | MSG_DONTWAIT);
		long		remaining;

		if (n == 1)
			break;
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			proc_exit(0);
		remaining = TimestampDifferenceMilliseconds(GetCurrentTimestamp(), deadline);
		if (remaining <= 0)
			proc_exit(0);
		(void) wait_socket(sock, WL_SOCKET_READABLE, (int) remaining);
	}
	if (first != 0x16)
		return true;
	flight_session->tls = flight_tls_create();
	if (flight_session->tls == NULL)
	{
		ereport(COMMERROR,
				(errcode(ERRCODE_PROTOCOL_VIOLATION),
				 errmsg("vexec_flight: a client began TLS, and the endpoint has no certificate")));
		return false;
	}
	return flight_tls_accept(flight_session->tls, sock, AuthenticationTimeout * 1000);
}

/* The idle wait: the session's timeout, or idle_in_transaction_session_timeout's. */
static int
idle_timeout(void)
{
	if (IsTransactionOrTransactionBlock() && IdleInTransactionSessionTimeout > 0)
		return IdleInTransactionSessionTimeout;
	return flight_idle_session_timeout > 0 ? flight_idle_session_timeout : -1;
}

void
vexec_flight_session_main(Datum arg)
{
	sigjmp_buf	local_sigjmp_buf;
	int			slot = DatumGetInt32(arg);
	uint64		token;
	FlightSession *s;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();

	memcpy(&token, MyBgworkerEntry->bgw_extra, sizeof(token));
	s = MemoryContextAllocZero(TopMemoryContext, sizeof(FlightSession));
	flight_session = s;
	s->slot = slot;
	s->mcxt = AllocSetContextCreate(TopMemoryContext, "vexec_flight session", ALLOCSET_DEFAULT_SIZES);
	s->callcxt = AllocSetContextCreate(TopMemoryContext, "vexec_flight call", ALLOCSET_DEFAULT_SIZES);
	s->egress = vexec_egress_lookup();
	if (s->egress == NULL)
		ereport(FATAL, (errmsg("vexec_flight session: vexec's egress API is not loaded")));

	/* the slot: the acceptor reserved it with this token */
	SpinLockAcquire(&flight_shared->mutex);
	if (slot < 0 || slot >= flight_shared->nslots ||
		flight_shared->slots[slot].state != FLIGHT_SLOT_RESERVED ||
		flight_shared->slots[slot].token != token)
	{
		SpinLockRelease(&flight_shared->mutex);
		ereport(FATAL, (errmsg("vexec_flight session: slot %d is not reserved for it", slot)));
	}
	flight_shared->slots[slot].state = FLIGHT_SLOT_ATTACHED;
	flight_shared->slots[slot].pid = MyProcPid;
	SpinLockRelease(&flight_shared->mutex);
	before_shmem_exit(release_slot, Int32GetDatum(slot));
	if (!pg_strong_random(&s->session_id, sizeof(s->session_id)))
		ereport(FATAL, (errmsg("vexec_flight could not make a session id")));
	SpinLockAcquire(&flight_shared->mutex);
	flight_shared->slots[slot].session_id = s->session_id;
	SpinLockRelease(&flight_shared->mutex);

	s->sock = take_client_socket(slot, token, &acceptor_link);
	if (!start_tls(s->sock))
		proc_exit(0);
	s->conn = h2_conn_create(s->sock, s->tls);
	before_shmem_exit(session_exit, 0);

	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		ErrorData  *edata;
		int			status;

		error_context_stack = NULL;
		HOLD_INTERRUPTS();
		disable_all_timeouts(false);
		QueryCancelPending = false;
		MemoryContextSwitchTo(s->mcxt);
		edata = CopyErrorData();
		status = error_grpc_status >= 0 ? error_grpc_status : grpc_status_for(edata->sqlerrcode);
		error_grpc_status = -1;
		EmitErrorReport();
		debug_query_string = NULL;
		if (s->logged_in)
		{
			AbortCurrentTransaction();
			PortalErrorCleanup();
			if (MyReplicationSlot != NULL)
				ReplicationSlotRelease();
			ReplicationSlotCleanup(false);
			jit_reset_after_error();
			flight_statement_ends();
		}
		s->xact_started = false;
		MemoryContextSwitchTo(s->callcxt);
		FlushErrorState();
		if (s->call)
		{
			h2_call_finish(s->call, status, error_text(edata), unpack_sql_state(edata->sqlerrcode));
			s->call = NULL;
		}
		FreeErrorData(edata);
		MemoryContextSwitchTo(TopMemoryContext);
		MemoryContextReset(s->callcxt);
		if (s->logged_in)
			pgstat_report_activity(IsTransactionBlock() ? STATE_IDLEINTRANSACTION_ABORTED : STATE_IDLE,
								   NULL);
		RESUME_INTERRUPTS();
	}
	PG_exception_stack = &local_sigjmp_buf;

	for (;;)
	{
		FlightCall *call;

		if (s->closing || !h2_conn_alive(s->conn))
			break;
		call = h2_next_call(s->conn, s->logged_in ? idle_timeout() :
							AuthenticationTimeout * 1000);
		if (call == NULL)
		{
			if (!h2_conn_alive(s->conn))
				break;
			if (!s->logged_in)
				break;			/* no login within authentication_timeout */
			if (IsTransactionOrTransactionBlock() && IdleInTransactionSessionTimeout > 0)
				ereport(FATAL,
						(errcode(ERRCODE_IDLE_IN_TRANSACTION_SESSION_TIMEOUT),
						 errmsg("terminating connection due to idle-in-transaction timeout")));
			ereport(LOG, (errmsg("vexec_flight: the session was idle for \"vexec_flight.idle_session_timeout\"")));
			break;
		}
		s->call = call;
		MemoryContextSwitchTo(s->callcxt);
		if (!s->logged_in)
		{
			if (!log_in(call))
			{
				s->call = NULL;
				continue;
			}
			/* connecting ran transactions of its own */
			MemoryContextSwitchTo(s->callcxt);
		}
		if (!flight_is_bearer(call))
			h2_call_finish(call, GRPC_UNAUTHENTICATED,
						   "the call shows neither this session's bearer token nor its user's credentials",
						   NULL);
		else
			flight_dispatch(call);
		s->call = NULL;
		MemoryContextSwitchTo(TopMemoryContext);
		MemoryContextReset(s->callcxt);
		if (s->logged_in)
			pgstat_report_activity(IsTransactionBlock() ? STATE_IDLEINTRANSACTION : STATE_IDLE, NULL);
	}

	if (s->conn)
	{
		h2_conn_shutdown(s->conn, 1000);
		s->conn = NULL;
	}
	proc_exit(0);
}
