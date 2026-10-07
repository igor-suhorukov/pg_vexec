/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * h2.h
 *	  One client connection's HTTP/2, and gRPC on it (h2.c).
 *
 * A session serves one call at a time: it takes the next call whose
 * request has arrived, reads its request messages, sends its response
 * messages, and finishes it with gRPC's status.  Meanwhile the connection
 * keeps reading: flow control, pings, the client's resets and its next
 * calls go on underneath, without a thread.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_FLIGHT_H2_H
#define VEXEC_FLIGHT_H2_H

#include "flight.h"

typedef struct FlightConn FlightConn;
typedef struct FlightCall FlightCall;

/* A piece of a message: bytes written as they lie. */
typedef struct FlightPiece
{
	const void *data;
	size_t		len;
} FlightPiece;

extern FlightConn *h2_conn_create(int sock, FlightTls *tls);
extern bool h2_conn_alive(FlightConn *conn);

/*
 * The next call whose request has arrived -- its headers, and its first
 * message or its end -- waiting up to timeout_ms (-1: for ever) while idle,
 * when a cancel means nothing.  NULL when the time ran out or the
 * connection closed.
 */
extern FlightCall *h2_next_call(FlightConn *conn, int timeout_ms);

/*
 * A call already arrived for a path, out of its turn -- a CancelFlightInfo
 * while a statement streams -- when want() accepts its first request
 * message; or NULL.
 */
extern FlightCall *h2_take_ready_call(FlightConn *conn, const char *path,
									  bool (*want) (const char *msg, size_t len));

/* A GOAWAY, what is queued flushed for up to timeout_ms, and the socket closed. */
extern void h2_conn_shutdown(FlightConn *conn, int timeout_ms);

extern const char *h2_call_path(FlightCall *call);
extern const char *h2_call_header(FlightCall *call, const char *name);

/*
 * The call's next request message into msg (reset first); false at the
 * request's end.  It waits for the client, and a cancel or a terminate may
 * raise meanwhile.
 */
extern bool h2_call_read(FlightCall *call, StringInfo msg);

/* A response header, before the call's first message. */
extern void h2_call_set_header(FlightCall *call, const char *name, const char *value);

/*
 * One response message, its pieces written as they lie, sent before it
 * returns.  False when the client has reset the call, or the connection
 * failed: the message is then not sent.  While the client's window is
 * closed it waits, and a cancel waits with it, as a backend's write to a
 * slow client does; a terminate ends the connection.
 */
extern bool h2_call_send(FlightCall *call, const FlightPiece *pieces, int npieces);

/*
 * The call's end: gRPC's status and message, and a trailer of the
 * statement's SQLSTATE when there is one.  The call is released.
 */
extern void h2_call_finish(FlightCall *call, int status, const char *message,
						   const char *sqlstate);

/* Whether the client has reset the call, or the connection is gone. */
extern bool h2_call_gone(FlightCall *call);

/* Whether a response was begun: headers or a message sent. */
extern bool h2_call_responded(FlightCall *call);

#endif							/* VEXEC_FLIGHT_H2_H */
