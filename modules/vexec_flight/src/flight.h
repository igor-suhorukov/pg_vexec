/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * flight.h
 *	  vexec_flight: Arrow Flight SQL for PostgreSQL 19, served only while
 *	  vexec's vector executor is active (pg_vector_executor.md §3.15, V10).
 *
 * The parts:
 *
 *	vexec_flight.c	_PG_init: the settings, the shared table of sessions, the
 *					acceptor's registration
 *	acceptor.c		the acceptor, a background worker without threads: it
 *					listens on the Flight port, asks the postmaster for a
 *					session worker for each connection and hands it the
 *					socket, and checks the sessions' logins (login.c)
 *	login.c			pg_hba.conf, passwords and connection limits, for a
 *					login, with the client's real address
 *	hba.c			the acceptor's own copy of pg_hba.conf's lines, and of
 *					PostgreSQL's match against them
 *	session.c		a session: one dynamic background worker, one thread, one
 *					connection; its statement loop and its errors
 *	tls.c			the session's OpenSSL context, ALPN "h2"
 *	h2.c			HTTP/2 on nghttp2 and gRPC on it, writing a message's
 *					pieces to the socket as they lie
 *	rpc.c			Flight's methods
 *	sql.c			Flight SQL's statements, prepared statements, updates,
 *					transactions and session options
 *	catalog.c		Flight SQL's catalog commands, server information and
 *					type information
 *	arrays.c		Arrow arrays the endpoint makes itself, for results no
 *					statement makes
 *
 * Every result goes to the client through vexec's egress API
 * (vexec_egress.h): the endpoint has no row path of its own.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_FLIGHT_H
#define VEXEC_FLIGHT_H

#include <sys/socket.h>

#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "storage/spin.h"

#include "vexec_egress.h"

#define VEXEC_FLIGHT_VERSION	"0.1.0"

/* gRPC's status codes (grpc/doc/statuscodes.md). */
typedef enum GrpcStatus
{
	GRPC_OK = 0,
	GRPC_CANCELLED = 1,
	GRPC_UNKNOWN = 2,
	GRPC_INVALID_ARGUMENT = 3,
	GRPC_DEADLINE_EXCEEDED = 4,
	GRPC_NOT_FOUND = 5,
	GRPC_ALREADY_EXISTS = 6,
	GRPC_PERMISSION_DENIED = 7,
	GRPC_RESOURCE_EXHAUSTED = 8,
	GRPC_FAILED_PRECONDITION = 9,
	GRPC_ABORTED = 10,
	GRPC_OUT_OF_RANGE = 11,
	GRPC_UNIMPLEMENTED = 12,
	GRPC_INTERNAL = 13,
	GRPC_UNAVAILABLE = 14,
	GRPC_DATA_LOSS = 15,
	GRPC_UNAUTHENTICATED = 16
} GrpcStatus;

/* vexec_flight.c: the settings */
extern char *flight_listen_addresses;
extern int	flight_port;
extern char *flight_ssl_cert_file;
extern char *flight_ssl_key_file;
extern int	flight_max_sessions;
extern int	flight_idle_session_timeout;

/*
 * The sessions, in shared memory: a slot each.  The acceptor reserves one
 * before it asks for a worker, so that vexec_flight.max_sessions holds; the
 * session fills it as it logs in, and frees it when it exits.  CancelFlightInfo
 * finds a statement's session here, and logins count a role's and a
 * database's sessions here, since PostgreSQL counts backends only.
 */
typedef enum FlightSlotState
{
	FLIGHT_SLOT_FREE,
	FLIGHT_SLOT_RESERVED,		/* the acceptor asked for a worker */
	FLIGHT_SLOT_ATTACHED,		/* the worker has the socket */
	FLIGHT_SLOT_ACTIVE			/* logged in and connected */
} FlightSlotState;

typedef struct FlightSlot
{
	int			state;			/* FlightSlotState */
	pid_t		pid;
	Oid			roleid;
	Oid			dboid;
	uint64		session_id;		/* random; a ticket names it */
	uint64		token;			/* the hand-over's */
	uint64		running;		/* the statement running, or 0 */
	uint64		cancelled;		/* a statement cancelled before it ran */
	TimestampTz since;
} FlightSlot;

typedef struct FlightShared
{
	slock_t		mutex;
	int			nslots;
	pid_t		acceptor_pid;
	FlightSlot	slots[FLEXIBLE_ARRAY_MEMBER];
} FlightShared;

extern FlightShared *flight_shared;

extern int	flight_count_sessions(Oid roleid, Oid dboid);

/*
 * The acceptor's UNIX-domain socket, by which a session takes its client's
 * socket and asks for its login to be checked: in the data directory, which
 * only the server's user may enter, named by the port.
 */
#define FLIGHT_SOCKET_NAME	"vexec_flight.%d.sock"
#define FLIGHT_MAGIC		0x56584631	/* "VXF1" */
#define FLIGHT_MAX_PASSWORD	1024

typedef struct FlightHello		/* session -> acceptor, first */
{
	uint32		magic;
	int			slot;
	uint64		token;
} FlightHello;

typedef struct FlightLoginRequest	/* session -> acceptor */
{
	uint32		magic;
	int			slot;
	char		user[NAMEDATALEN];
	char		database[NAMEDATALEN];
	char		password[FLIGHT_MAX_PASSWORD];
	bool		ssl;
	struct sockaddr_storage addr;
	socklen_t	addrlen;
} FlightLoginRequest;

typedef struct FlightLoginReply /* acceptor -> session */
{
	uint32		magic;
	bool		ok;
	Oid			roleid;
	Oid			dboid;
	int			grpc_status;
	char		sqlstate[6];
	char		message[512];
} FlightLoginReply;

/* acceptor.c */
extern PGDLLEXPORT void vexec_flight_acceptor_main(Datum arg);

/* login.c */
extern void flight_login_check(const FlightLoginRequest *req, FlightLoginReply *reply);

/* hba.c */
struct Port;
extern bool flight_load_hba(void);
extern void flight_check_hba(struct Port *port);

/* session.c */
extern PGDLLEXPORT void vexec_flight_session_main(Datum arg);

/* tls.c */
typedef struct FlightTls FlightTls;

extern FlightTls *flight_tls_create(void);
extern bool flight_tls_accept(FlightTls *tls, int sock, int timeout_ms);
extern void *flight_tls_ssl(FlightTls *tls);
extern ssize_t flight_tls_read(FlightTls *tls, void *buf, size_t len, int *wait_events);
extern ssize_t flight_tls_write(FlightTls *tls, const void *buf, size_t len, int *wait_events);
extern void flight_tls_close(FlightTls *tls);

/* Text for the client: an error's message, percent-encoded for grpc-message. */
extern char *flight_percent_encode(const char *s);

#endif							/* VEXEC_FLIGHT_H */
