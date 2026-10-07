/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * h2.c
 *	  HTTP/2 on nghttp2, and gRPC on it, for one client connection that one
 *	  single-threaded process owns (pg_vector_executor.md §3.15).
 *
 * nghttp2 parses and frames; this file does the I/O, on the session's own
 * socket or its TLS, and waits in a WaitEventSet on the socket and the
 * process latch, so that signals, cancels and the postmaster's death are
 * seen as a backend sees them.
 *
 * Writing without a copy.  A response message is a list of pieces -- for a
 * record batch, the body's buffers where the egress left them -- and goes
 * out in DATA frames that nghttp2 does not copy (NGHTTP2_DATA_FLAG_NO_COPY):
 * nghttp2 decides each frame's length by the client's flow-control window,
 * and send_data_callback writes the frame's 9-byte header and its slice of
 * the pieces with one writev.  Those headers, gRPC's 5-byte prefix and the
 * protobuf tags are the only bytes the session writes itself.  The pieces
 * are valid only while h2_call_send runs, so it sends the message whole
 * before it returns, waiting for the client's window as it must: a slow
 * client holds the executor, as it holds a backend's.
 *
 * Interrupts.  nghttp2 is not re-entrant, and must never be unwound by an
 * error: the wait inside its callbacks (H2_WAIT_IO) handles no interrupt,
 * and only notes that a terminate came.  Between nghttp2's calls the waits
 * do: while idle a cancel means nothing (H2_WAIT_IDLE); while reading a
 * call's request a cancel or a terminate raises as anywhere
 * (H2_WAIT_CALL); while a message is half sent only a terminate does
 * (H2_WAIT_SEND), as PostgreSQL's ProcessClientWriteInterrupt() allows only
 * a terminate during a blocked write, and the cancel waits for the end of
 * the message (PG19:src/backend/tcop/postgres.c, ProcessClientWriteInterrupt).
 *
 * Flow control is manual (nghttp2_option_set_no_auto_window_update): a
 * request's bytes are acknowledged up to the end of its first complete
 * message, and the rest as the session takes messages, so that a client
 * streaming DoPut batches stays one message and a window ahead.
 *
 * nghttp2's memory is palloc'd, in the connection's memory context, failing
 * as NULL rather than raising inside it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <nghttp2/nghttp2.h>

#include "lib/ilist.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "storage/sinval.h"
#include "storage/waiteventset.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "flight.h"
#include "h2.h"

#define H2_READ_CHUNK		(64 * 1024)
#define H2_INBUF_MAX		(8 * 1024 * 1024)	/* read ahead while writing */
#define H2_OUTBUF_FLUSH		(64 * 1024)
#define H2_STREAM_WINDOW	(1024 * 1024)
#define H2_CONN_WINDOW		(16 * 1024 * 1024)
#define H2_MAX_FRAME		(1024 * 1024)
#define H2_MAX_HEADERS		64

#ifndef IOV_MAX
#define IOV_MAX				1024
#endif

typedef enum H2Wait
{
	H2_WAIT_IDLE,				/* between calls */
	H2_WAIT_CALL,				/* reading a call's request */
	H2_WAIT_SEND,				/* a message half sent */
	H2_WAIT_IO					/* inside nghttp2 */
} H2Wait;

typedef struct H2Header
{
	char	   *name;
	char	   *value;
} H2Header;

struct FlightCall
{
	FlightConn *conn;
	int32		stream_id;
	MemoryContext mcxt;
	char	   *path;
	H2Header	headers[H2_MAX_HEADERS];
	int			nheaders;
	bool		headers_done;	/* the request's headers are in */
	bool		request_ended;	/* and its END_STREAM */
	bool		queued;			/* in conn->ready */
	bool		dispatched;		/* the session has it */
	bool		released;		/* the session is done with it */
	bool		closed;			/* the stream is closed */
	dlist_node	node;

	/* the request: bytes not yet taken, and how many of them are acknowledged */
	StringInfoData in;
	size_t		acked;

	/* the response */
	H2Header	out_headers[8];
	int			nout_headers;
	bool		responded;		/* headers submitted */
	bool		deferred;		/* the data provider waits for a message */
	bool		finishing;		/* trailers follow the last message */
	char		status[12];
	char	   *message;		/* percent-encoded */
	char	   *sqlstate;
	char		prefix[5];		/* gRPC's: compressed flag, length */
	FlightPiece *out;			/* the message being sent */
	int			nout;
	int			maxout;
	int			out_i;
	size_t		out_off;
	size_t		out_left;
};

struct FlightConn
{
	int			sock;
	FlightTls  *tls;
	MemoryContext mcxt;
	nghttp2_session *session;
	WaitEventSet *wes;
	int			sock_pos;
	bool		dead;
	bool		in_nghttp2;
	dlist_head	ready;			/* calls whose request has arrived */
	char	   *inbuf;			/* read, not yet given to nghttp2 */
	size_t		inlen;
	size_t		incap;
	char	   *outbuf;			/* small writes, gathered */
	size_t		outlen;
	size_t		outcap;
	TimestampTz io_deadline;	/* for a write: 0 none */
};

/* ---------------------------------------------------------------------
 * Memory: nghttp2's, palloc'd, failing as NULL
 * ---------------------------------------------------------------------
 */

static void *
h2_malloc(size_t size, void *ud)
{
	return MemoryContextAllocExtended((MemoryContext) ud, Max(size, 1),
									  MCXT_ALLOC_NO_OOM | MCXT_ALLOC_HUGE);
}

static void
h2_free(void *ptr, void *ud)
{
	(void) ud;
	if (ptr)
		pfree(ptr);
}

static void *
h2_calloc(size_t nmemb, size_t size, void *ud)
{
	if (size != 0 && nmemb > SIZE_MAX / size)
		return NULL;
	return MemoryContextAllocExtended((MemoryContext) ud, Max(nmemb * size, 1),
									  MCXT_ALLOC_NO_OOM | MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
}

static void *
h2_realloc(void *ptr, size_t size, void *ud)
{
	if (ptr == NULL)
		return h2_malloc(size, ud);
	if (size == 0)
	{
		pfree(ptr);
		return NULL;
	}
	return repalloc_extended(ptr, size, MCXT_ALLOC_NO_OOM | MCXT_ALLOC_HUGE);
}

/* ---------------------------------------------------------------------
 * Waiting, and what a wait may do
 * ---------------------------------------------------------------------
 */

static void
handle_interrupts(H2Wait mode)
{
	switch (mode)
	{
		case H2_WAIT_IDLE:
			if (ConfigReloadPending)
			{
				ConfigReloadPending = false;
				ProcessConfigFile(PGC_SIGHUP);
			}
			/* no statement runs: a cancel means nothing, as for a backend */
			QueryCancelPending = false;
			if (catchupInterruptPending)
				ProcessCatchupInterrupt();
			CHECK_FOR_INTERRUPTS();
			break;
		case H2_WAIT_CALL:
			CHECK_FOR_INTERRUPTS();
			break;
		case H2_WAIT_SEND:
			/* a terminate only: a cancel waits for the end of the message */
			if (ProcDiePending)
				CHECK_FOR_INTERRUPTS();
			break;
		case H2_WAIT_IO:
			break;
	}
}

/*
 * Wait up to timeout_ms (-1: for ever) for the socket's events, or the
 * latch; the socket's events that came, 0 on a time out or the latch.
 */
static int
conn_wait(FlightConn *conn, int events, long timeout_ms, H2Wait mode)
{
	WaitEvent	ev[3];
	int			n;
	int			got = 0;
	int			i;

	ModifyWaitEvent(conn->wes, conn->sock_pos, events, NULL);
	n = WaitEventSetWait(conn->wes, timeout_ms, ev, lengthof(ev),
						 (events & WL_SOCKET_WRITEABLE) ? WAIT_EVENT_CLIENT_WRITE :
						 WAIT_EVENT_CLIENT_READ);
	for (i = 0; i < n; i++)
	{
		if (ev[i].events & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			handle_interrupts(mode);
		}
		if (ev[i].events & (WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE))
			got |= ev[i].events & (WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE);
	}
	return got;
}

/* ---------------------------------------------------------------------
 * The socket, or its TLS
 * ---------------------------------------------------------------------
 */

/* Bytes read: > 0; 0 at the end; -1 none yet, *wait what to wait for; -2 failed. */
static ssize_t
conn_recv(FlightConn *conn, char *buf, size_t len, int *wait)
{
	ssize_t		n;

	if (conn->tls)
		return flight_tls_read(conn->tls, buf, len, wait);
	n = recv(conn->sock, buf, len, 0);
	if (n >= 0)
		return n;
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
	{
		*wait = WL_SOCKET_READABLE;
		return -1;
	}
	return -2;
}

static ssize_t
conn_send(FlightConn *conn, const char *buf, size_t len, int *wait)
{
	ssize_t		n;

	if (conn->tls)
		return flight_tls_write(conn->tls, buf, len, wait);
	n = send(conn->sock, buf, len, MSG_NOSIGNAL);
	if (n >= 0)
		return n;
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
	{
		*wait = WL_SOCKET_WRITEABLE;
		return -1;
	}
	return -2;
}

/*
 * While a write waits, what the client sends is read ahead, so that it is
 * never blocked writing to a session that is blocked writing to it.
 */
static void
read_ahead(FlightConn *conn)
{
	int			wait;
	ssize_t		n;

	if (conn->inlen >= H2_INBUF_MAX)
		return;
	if (conn->incap - conn->inlen < H2_READ_CHUNK)
	{
		conn->incap = Max(conn->incap * 2, conn->inlen + H2_READ_CHUNK);
		conn->inbuf = conn->inbuf ? repalloc(conn->inbuf, conn->incap)
			: MemoryContextAlloc(conn->mcxt, conn->incap);
	}
	n = conn_recv(conn, conn->inbuf + conn->inlen, H2_READ_CHUNK, &wait);
	if (n > 0)
		conn->inlen += n;
	else if (n == 0 || n == -2)
		conn->dead = true;
}

/*
 * Write iov in full, waiting as long as it takes, or until io_deadline:
 * false when the connection failed, or a terminate came while it waited.
 */
static bool
conn_writev(FlightConn *conn, struct iovec *iov, int iovcnt)
{
	int			i = 0;

	while (i < iovcnt)
	{
		ssize_t		n;
		int			wait = WL_SOCKET_WRITEABLE;

		if (iov[i].iov_len == 0)
		{
			i++;
			continue;
		}
		if (conn->tls)
			n = conn_send(conn, iov[i].iov_base, iov[i].iov_len, &wait);
		else
		{
			struct msghdr msg;

			memset(&msg, 0, sizeof(msg));
			msg.msg_iov = &iov[i];
			msg.msg_iovlen = Min(iovcnt - i, IOV_MAX);
			n = sendmsg(conn->sock, &msg, MSG_NOSIGNAL);
			if (n < 0)
				n = (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? -1 : -2;
		}
		if (n == -2)
		{
			conn->dead = true;
			return false;
		}
		if (n >= 0)
		{
			/* advance past what was written */
			while (n > 0 && i < iovcnt)
			{
				if ((size_t) n >= iov[i].iov_len)
				{
					n -= iov[i].iov_len;
					iov[i].iov_len = 0;
					i++;
				}
				else
				{
					iov[i].iov_base = (char *) iov[i].iov_base + n;
					iov[i].iov_len -= n;
					n = 0;
				}
			}
			continue;
		}
		/* wait to write, reading ahead meanwhile */
		{
			long		timeout = -1;
			int			got;

			if (conn->io_deadline != 0)
			{
				timeout = TimestampDifferenceMilliseconds(GetCurrentTimestamp(), conn->io_deadline);
				if (timeout <= 0)
				{
					conn->dead = true;
					return false;
				}
			}
			got = conn_wait(conn, wait | (conn->inlen < H2_INBUF_MAX ? WL_SOCKET_READABLE : 0),
							timeout, H2_WAIT_IO);
			if (ProcDiePending)
			{
				conn->dead = true;
				return false;
			}
			if ((got & WL_SOCKET_READABLE) && !(wait & WL_SOCKET_READABLE))
				read_ahead(conn);
			if (conn->dead)
				return false;
		}
	}
	return true;
}

static bool
flush_outbuf(FlightConn *conn)
{
	struct iovec iov;

	if (conn->outlen == 0)
		return true;
	iov.iov_base = conn->outbuf;
	iov.iov_len = conn->outlen;
	conn->outlen = 0;
	return conn_writev(conn, &iov, 1);
}

/* ---------------------------------------------------------------------
 * nghttp2's callbacks
 * ---------------------------------------------------------------------
 */

/* Frames other than our DATA: gathered, and written with the next write. */
static nghttp2_ssize
h2_send(nghttp2_session *session, const uint8_t *data, size_t length, int flags, void *ud)
{
	FlightConn *conn = ud;

	(void) session;
	(void) flags;
	if (conn->outlen + length > conn->outcap)
	{
		if (!flush_outbuf(conn))
			return NGHTTP2_ERR_CALLBACK_FAILURE;
		if (length > conn->outcap)
		{
			conn->outcap = length;
			conn->outbuf = repalloc(conn->outbuf, conn->outcap);
		}
	}
	memcpy(conn->outbuf + conn->outlen, data, length);
	conn->outlen += length;
	if (conn->outlen >= H2_OUTBUF_FLUSH && !flush_outbuf(conn))
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	return (nghttp2_ssize) length;
}

/*
 * A DATA frame of a response message: what is gathered, the frame's
 * header, and `length` bytes of the message's pieces, in one write.
 */
static int
h2_send_data(nghttp2_session *session, nghttp2_frame *frame, const uint8_t *framehd,
			 size_t length, nghttp2_data_source *source, void *ud)
{
	FlightConn *conn = ud;
	FlightCall *call = source->ptr;
	struct iovec iov[IOV_MAX];
	int			n = 0;
	size_t		left = length;

	(void) session;
	if (frame->data.padlen > 0)
		return NGHTTP2_ERR_CALLBACK_FAILURE;	/* never asked for */

	if (conn->outlen > 0)
	{
		iov[n].iov_base = conn->outbuf;
		iov[n].iov_len = conn->outlen;
		n++;
	}
	iov[n].iov_base = (void *) framehd;
	iov[n].iov_len = 9;
	n++;
	while (left > 0)
	{
		const FlightPiece *p = &call->out[call->out_i];
		size_t		take = Min(p->len - call->out_off, left);

		if (n == IOV_MAX)
		{
			/* a frame over very many pieces: in more than one write */
			if (!conn_writev(conn, iov, n))
				return NGHTTP2_ERR_CALLBACK_FAILURE;
			conn->outlen = 0;
			n = 0;
		}
		iov[n].iov_base = (char *) p->data + call->out_off;
		iov[n].iov_len = take;
		n++;
		left -= take;
		call->out_left -= take;
		call->out_off += take;
		if (call->out_off == p->len)
		{
			call->out_i++;
			call->out_off = 0;
		}
	}
	if (!conn_writev(conn, iov, n))
		return NGHTTP2_ERR_CALLBACK_FAILURE;
	conn->outlen = 0;
	return 0;
}

static void
submit_trailers(FlightCall *call)
{
	nghttp2_nv	nva[3];
	int			n = 0;

	nva[n++] = (nghttp2_nv) {(uint8_t *) "grpc-status", (uint8_t *) call->status,
		11, strlen(call->status), NGHTTP2_NV_FLAG_NONE};
	if (call->message && call->message[0])
		nva[n++] = (nghttp2_nv) {(uint8_t *) "grpc-message", (uint8_t *) call->message,
			12, strlen(call->message), NGHTTP2_NV_FLAG_NONE};
	if (call->sqlstate)
		nva[n++] = (nghttp2_nv) {(uint8_t *) "x-postgresql-sqlstate", (uint8_t *) call->sqlstate,
			21, strlen(call->sqlstate), NGHTTP2_NV_FLAG_NONE};
	nghttp2_submit_trailer(call->conn->session, call->stream_id, nva, n);
}

/* The response's data: the message being sent, a frame's worth at a time. */
static nghttp2_ssize
h2_data_read(nghttp2_session *session, int32_t stream_id, uint8_t *buf, size_t length,
			 uint32_t *data_flags, nghttp2_data_source *source, void *ud)
{
	FlightCall *call = source->ptr;

	(void) session;
	(void) stream_id;
	(void) buf;
	(void) ud;
	if (call->out_left == 0)
	{
		if (call->finishing)
		{
			*data_flags |= NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM;
			submit_trailers(call);
			return 0;
		}
		call->deferred = true;
		return NGHTTP2_ERR_DEFERRED;
	}
	*data_flags |= NGHTTP2_DATA_FLAG_NO_COPY;
	return (nghttp2_ssize) Min(call->out_left, length);
}

static void
call_free(FlightCall *call)
{
	if (call->queued)
		dlist_delete(&call->node);
	MemoryContextDelete(call->mcxt);
}

/* The first request message's end within `in`, or 0 when it is not all in. */
static size_t
first_message_end(FlightCall *call)
{
	uint32		len;

	if (call->in.len < 5)
		return 0;
	len = pg_ntoh32(*(uint32 *) (call->in.data + 1));
	if ((size_t) call->in.len < 5 + (size_t) len)
		return 0;
	return 5 + (size_t) len;
}

/*
 * Acknowledge the request's bytes the session will hold anyway: all of an
 * incomplete first message, or the first message, and no more.
 */
static void
call_acknowledge(FlightCall *call)
{
	size_t		end = first_message_end(call);
	size_t		want = end == 0 ? (size_t) call->in.len : end;

	if (call->released)
		want = call->in.len;
	if (want > call->acked)
	{
		nghttp2_session_consume(call->conn->session, call->stream_id, want - call->acked);
		call->acked = want;
	}
}

static void
call_check_ready(FlightCall *call)
{
	if (call->queued || call->dispatched || call->closed || !call->headers_done)
		return;
	if (call->request_ended || first_message_end(call) > 0)
	{
		dlist_push_tail(&call->conn->ready, &call->node);
		call->queued = true;
	}
}

static int
h2_on_begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *ud)
{
	FlightConn *conn = ud;
	FlightCall *call;
	MemoryContext mcxt;

	if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
		return 0;
	mcxt = AllocSetContextCreate(conn->mcxt, "vexec_flight call", ALLOCSET_SMALL_SIZES);
	call = MemoryContextAllocZero(mcxt, sizeof(FlightCall));
	call->conn = conn;
	call->mcxt = mcxt;
	call->stream_id = frame->hd.stream_id;
	call->in.data = MemoryContextAlloc(mcxt, 1024);
	call->in.maxlen = 1024;
	call->in.data[0] = '\0';
	nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, call);
	return 0;
}

static int
h2_on_header(nghttp2_session *session, const nghttp2_frame *frame,
			 const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen,
			 uint8_t flags, void *ud)
{
	FlightCall *call;

	(void) flags;
	(void) ud;
	if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
		return 0;
	call = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
	if (call == NULL)
		return 0;
	if (namelen == 5 && memcmp(name, ":path", 5) == 0)
	{
		MemoryContext old = MemoryContextSwitchTo(call->mcxt);

		call->path = pnstrdup((const char *) value, valuelen);
		MemoryContextSwitchTo(old);
	}
	else if (call->nheaders < H2_MAX_HEADERS)
	{
		H2Header   *h = &call->headers[call->nheaders++];
		MemoryContext old = MemoryContextSwitchTo(call->mcxt);

		h->name = pnstrdup((const char *) name, namelen);
		h->value = pnstrdup((const char *) value, valuelen);
		MemoryContextSwitchTo(old);
	}
	return 0;
}

static int
h2_on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame, void *ud)
{
	FlightCall *call;

	(void) ud;
	if (frame->hd.type != NGHTTP2_HEADERS && frame->hd.type != NGHTTP2_DATA)
		return 0;
	call = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);
	if (call == NULL)
		return 0;
	if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST)
		call->headers_done = true;
	if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
		call->request_ended = true;
	call_check_ready(call);
	return 0;
}

static int
h2_on_data_chunk(nghttp2_session *session, uint8_t flags, int32_t stream_id,
				 const uint8_t *data, size_t len, void *ud)
{
	FlightCall *call = nghttp2_session_get_stream_user_data(session, stream_id);

	(void) flags;
	(void) ud;
	if (call == NULL || call->released)
	{
		/* nobody reads it: let the windows open again */
		nghttp2_session_consume(session, stream_id, len);
		return 0;
	}
	if ((size_t) call->in.len + len > MaxAllocSize - 1)
		return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;	/* resets the stream */
	{
		MemoryContext old = MemoryContextSwitchTo(call->mcxt);

		appendBinaryStringInfo(&call->in, (const char *) data, (int) len);
		MemoryContextSwitchTo(old);
	}
	call_acknowledge(call);
	call_check_ready(call);
	return 0;
}

static int
h2_on_stream_close(nghttp2_session *session, int32_t stream_id, uint32_t error_code, void *ud)
{
	FlightCall *call = nghttp2_session_get_stream_user_data(session, stream_id);

	(void) error_code;
	(void) ud;
	if (call == NULL)
		return 0;
	call->closed = true;
	nghttp2_session_set_stream_user_data(session, stream_id, NULL);
	if (!call->dispatched || call->released)
		call_free(call);
	return 0;
}

/* ---------------------------------------------------------------------
 * The connection
 * ---------------------------------------------------------------------
 */

/* nghttp2's pending frames, written; false when the connection failed. */
static bool
conn_send_session(FlightConn *conn)
{
	int			rv;

	if (conn->dead)
		return false;
	conn->in_nghttp2 = true;
	rv = nghttp2_session_send(conn->session);
	conn->in_nghttp2 = false;
	if (rv != 0 || !flush_outbuf(conn))
		conn->dead = true;
	return !conn->dead;
}

/* What was read, given to nghttp2, and what it answers, sent. */
static bool
conn_feed(FlightConn *conn)
{
	nghttp2_ssize rv;

	if (conn->inlen == 0)
		return !conn->dead;
	conn->in_nghttp2 = true;
	rv = nghttp2_session_mem_recv2(conn->session, (const uint8_t *) conn->inbuf, conn->inlen);
	conn->in_nghttp2 = false;
	conn->inlen = 0;
	if (rv < 0)
	{
		/* a protocol error: nghttp2 has queued its GOAWAY */
		conn_send_session(conn);
		conn->dead = true;
		return false;
	}
	return conn_send_session(conn);
}

/*
 * Read, if the client has sent anything -- waiting up to timeout_ms for it
 * (0: not at all, -1: for ever) -- and let nghttp2 take it.  False when the
 * connection is gone.
 */
static bool
conn_pump(FlightConn *conn, long timeout_ms, H2Wait mode)
{
	for (;;)
	{
		int			wait = WL_SOCKET_READABLE;
		ssize_t		n;
		int			got;

		if (conn->dead)
			return false;
		if (conn->inlen > 0)
			return conn_feed(conn);
		if (conn->incap < H2_READ_CHUNK)
		{
			conn->incap = H2_READ_CHUNK;
			conn->inbuf = conn->inbuf ? repalloc(conn->inbuf, conn->incap)
				: MemoryContextAlloc(conn->mcxt, conn->incap);
		}
		n = conn_recv(conn, conn->inbuf, conn->incap, &wait);
		if (n > 0)
		{
			conn->inlen = n;
			return conn_feed(conn);
		}
		if (n == 0 || n == -2)
		{
			conn->dead = true;
			return false;
		}
		if (timeout_ms == 0)
			return true;
		if (nghttp2_session_want_write(conn->session))
			wait |= WL_SOCKET_WRITEABLE;
		got = conn_wait(conn, wait, timeout_ms, mode);
		if (got == 0)
			return true;		/* the time ran out, or the latch: the caller looks again */
		if ((got & WL_SOCKET_WRITEABLE) && !conn_send_session(conn))
			return false;
	}
}

FlightConn *
h2_conn_create(int sock, FlightTls *tls)
{
	MemoryContext mcxt;
	FlightConn *conn;
	nghttp2_session_callbacks *cbs;
	nghttp2_option *opt;
	nghttp2_mem *mem;
	nghttp2_settings_entry iv[4];
	int			flags;

	mcxt = AllocSetContextCreate(TopMemoryContext, "vexec_flight connection", ALLOCSET_DEFAULT_SIZES);
	conn = MemoryContextAllocZero(mcxt, sizeof(FlightConn));
	conn->sock = sock;
	conn->tls = tls;
	conn->mcxt = mcxt;
	dlist_init(&conn->ready);
	conn->outcap = H2_OUTBUF_FLUSH * 2;
	conn->outbuf = MemoryContextAlloc(mcxt, conn->outcap);

	flags = fcntl(sock, F_GETFL);
	if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
		ereport(FATAL,
				(errcode_for_socket_access(),
				 errmsg("could not set the Flight client's socket to non-blocking mode: %m")));

	conn->wes = CreateWaitEventSet(NULL, 3);
	AddWaitEventToSet(conn->wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
	AddWaitEventToSet(conn->wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
	conn->sock_pos = AddWaitEventToSet(conn->wes, WL_SOCKET_READABLE, sock, NULL, NULL);

	mem = MemoryContextAllocZero(mcxt, sizeof(nghttp2_mem));
	mem->mem_user_data = AllocSetContextCreate(mcxt, "vexec_flight nghttp2", ALLOCSET_DEFAULT_SIZES);
	mem->malloc = h2_malloc;
	mem->free = h2_free;
	mem->calloc = h2_calloc;
	mem->realloc = h2_realloc;

	if (nghttp2_session_callbacks_new(&cbs) != 0)
		elog(FATAL, "nghttp2 could not make its callbacks");
	nghttp2_session_callbacks_set_send_callback2(cbs, h2_send);
	nghttp2_session_callbacks_set_send_data_callback(cbs, h2_send_data);
	nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, h2_on_begin_headers);
	nghttp2_session_callbacks_set_on_header_callback(cbs, h2_on_header);
	nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, h2_on_frame_recv);
	nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, h2_on_data_chunk);
	nghttp2_session_callbacks_set_on_stream_close_callback(cbs, h2_on_stream_close);
	if (nghttp2_option_new(&opt) != 0)
		elog(FATAL, "nghttp2 could not make its options");
	nghttp2_option_set_no_auto_window_update(opt, 1);
	if (nghttp2_session_server_new3(&conn->session, cbs, conn, opt, mem) != 0)
		elog(FATAL, "nghttp2 could not make its session");
	nghttp2_session_callbacks_del(cbs);
	nghttp2_option_del(opt);

	iv[0] = (nghttp2_settings_entry) {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100};
	iv[1] = (nghttp2_settings_entry) {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, H2_STREAM_WINDOW};
	iv[2] = (nghttp2_settings_entry) {NGHTTP2_SETTINGS_MAX_FRAME_SIZE, H2_MAX_FRAME};
	iv[3] = (nghttp2_settings_entry) {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, 64 * 1024};
	nghttp2_submit_settings(conn->session, NGHTTP2_FLAG_NONE, iv, lengthof(iv));
	nghttp2_session_set_local_window_size(conn->session, NGHTTP2_FLAG_NONE, 0, H2_CONN_WINDOW);
	conn_send_session(conn);
	return conn;
}

bool
h2_conn_alive(FlightConn *conn)
{
	return !conn->dead;
}

FlightCall *
h2_next_call(FlightConn *conn, int timeout_ms)
{
	TimestampTz deadline = timeout_ms > 0 ?
		TimestampTzPlusMilliseconds(GetCurrentTimestamp(), timeout_ms) : 0;

	for (;;)
	{
		long		remaining = -1;

		if (!dlist_is_empty(&conn->ready))
		{
			FlightCall *call = dlist_container(FlightCall, node, dlist_pop_head_node(&conn->ready));

			call->queued = false;
			call->dispatched = true;
			return call;
		}
		if (conn->dead)
			return NULL;
		if (timeout_ms > 0)
		{
			remaining = TimestampDifferenceMilliseconds(GetCurrentTimestamp(), deadline);
			if (remaining <= 0)
				return NULL;
		}
		else if (timeout_ms == 0)
			remaining = 0;
		if (!conn_pump(conn, remaining, H2_WAIT_IDLE))
			return NULL;
		if (timeout_ms == 0 && dlist_is_empty(&conn->ready))
			return NULL;
	}
}

/*
 * A call already arrived for a path, taken out of its turn when want()
 * accepts its first request message, which stays the call's to read: what
 * the client sent meanwhile is read first, without waiting.  For a cancel
 * that must be served while a statement streams (rpc.c).  NULL when there
 * is none.
 */
FlightCall *
h2_take_ready_call(FlightConn *conn, const char *path,
				   bool (*want) (const char *msg, size_t len))
{
	dlist_mutable_iter iter;

	if (conn->dead || conn->in_nghttp2)
		return NULL;
	(void) conn_pump(conn, 0, H2_WAIT_CALL);
	dlist_foreach_modify(iter, &conn->ready)
	{
		FlightCall *call = dlist_container(FlightCall, node, iter.cur);
		size_t		end = first_message_end(call);

		if (call->path && strcmp(call->path, path) == 0 && end > 0 &&
			call->in.data[0] == 0 && want(call->in.data + 5, end - 5))
		{
			dlist_delete(&call->node);
			call->queued = false;
			call->dispatched = true;
			return call;
		}
	}
	return NULL;
}

void
h2_conn_shutdown(FlightConn *conn, int timeout_ms)
{
	if (!conn->dead && !conn->in_nghttp2)
	{
		conn->io_deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), timeout_ms);
		nghttp2_session_terminate_session(conn->session, NGHTTP2_NO_ERROR);
		conn_send_session(conn);
	}
	conn->dead = true;
	if (conn->tls)
		flight_tls_close(conn->tls);
	closesocket(conn->sock);
}

/* ---------------------------------------------------------------------
 * Calls
 * ---------------------------------------------------------------------
 */

const char *
h2_call_path(FlightCall *call)
{
	return call->path ? call->path : "";
}

const char *
h2_call_header(FlightCall *call, const char *name)
{
	int			i;

	for (i = 0; i < call->nheaders; i++)
		if (strcmp(call->headers[i].name, name) == 0)
			return call->headers[i].value;
	return NULL;
}

bool
h2_call_gone(FlightCall *call)
{
	return call->closed || call->conn->dead;
}

bool
h2_call_responded(FlightCall *call)
{
	return call->responded;
}

bool
h2_call_read(FlightCall *call, StringInfo msg)
{
	resetStringInfo(msg);
	for (;;)
	{
		size_t		end = first_message_end(call);

		if (end > 0)
		{
			if (call->in.data[0] != 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("a compressed gRPC message: vexec_flight takes identity encoding only")));
			appendBinaryStringInfo(msg, call->in.data + 5, (int) (end - 5));
			memmove(call->in.data, call->in.data + end, call->in.len - end);
			call->in.len -= (int) end;
			call->in.data[call->in.len] = '\0';
			call->acked -= Min(call->acked, end);
			call_acknowledge(call);
			if (!conn_send_session(call->conn))
				return false;
			return true;
		}
		if (call->in.len >= 5 &&
			pg_ntoh32(*(uint32 *) (call->in.data + 1)) > MaxAllocSize - 1)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("a gRPC message of %u bytes is too long",
							pg_ntoh32(*(uint32 *) (call->in.data + 1)))));
		if (h2_call_gone(call))
			return false;
		if (call->request_ended)
		{
			if (call->in.len > 0)
				ereport(ERROR,
						(errcode(ERRCODE_PROTOCOL_VIOLATION),
						 errmsg("the client ended a call in the middle of a gRPC message")));
			return false;
		}
		if (!conn_pump(call->conn, -1, H2_WAIT_CALL))
			return false;
	}
}

void
h2_call_set_header(FlightCall *call, const char *name, const char *value)
{
	if (call->responded || call->nout_headers >= lengthof(call->out_headers))
		elog(ERROR, "vexec_flight: a response header after the response began");
	call->out_headers[call->nout_headers].name = MemoryContextStrdup(call->mcxt, name);
	call->out_headers[call->nout_headers].value = MemoryContextStrdup(call->mcxt, value);
	call->nout_headers++;
}

/* The response's headers, with its data to follow. */
static void
respond(FlightCall *call)
{
	nghttp2_nv	nva[2 + lengthof(call->out_headers)];
	nghttp2_data_provider2 prd;
	int			n = 0;
	int			i;

	nva[n++] = (nghttp2_nv) {(uint8_t *) ":status", (uint8_t *) "200", 7, 3, NGHTTP2_NV_FLAG_NONE};
	nva[n++] = (nghttp2_nv) {(uint8_t *) "content-type", (uint8_t *) "application/grpc", 12, 16,
		NGHTTP2_NV_FLAG_NONE};
	for (i = 0; i < call->nout_headers; i++)
		nva[n++] = (nghttp2_nv) {(uint8_t *) call->out_headers[i].name,
			(uint8_t *) call->out_headers[i].value, strlen(call->out_headers[i].name),
			strlen(call->out_headers[i].value), NGHTTP2_NV_FLAG_NONE};
	prd.source.ptr = call;
	prd.read_callback = h2_data_read;
	nghttp2_submit_response2(call->conn->session, call->stream_id, nva, n, &prd);
	call->responded = true;
}

bool
h2_call_send(FlightCall *call, const FlightPiece *pieces, int npieces)
{
	FlightConn *conn = call->conn;
	size_t		total = 0;
	int			i;

	if (h2_call_gone(call))
		return false;
	for (i = 0; i < npieces; i++)
		total += pieces[i].len;
	if (total > PG_UINT32_MAX)
		elog(ERROR, "vexec_flight: a gRPC message of %zu bytes", total);

	/* the message: gRPC's prefix, then the pieces as they lie */
	if (call->maxout < npieces + 1)
	{
		call->maxout = npieces + 1;
		call->out = call->out ? repalloc(call->out, sizeof(FlightPiece) * call->maxout)
			: MemoryContextAlloc(call->mcxt, sizeof(FlightPiece) * call->maxout);
	}
	call->prefix[0] = 0;
	*(uint32 *) (call->prefix + 1) = pg_hton32((uint32) total);
	call->out[0].data = call->prefix;
	call->out[0].len = 5;
	memcpy(call->out + 1, pieces, sizeof(FlightPiece) * npieces);
	call->nout = npieces + 1;
	call->out_i = 0;
	call->out_off = 0;
	call->out_left = total + 5;

	if (!call->responded)
		respond(call);
	else if (call->deferred)
	{
		call->deferred = false;
		nghttp2_session_resume_data(conn->session, call->stream_id);
	}

	for (;;)
	{
		if (!conn_send_session(conn))
			break;
		if (call->out_left == 0)
			return true;
		if (call->closed)
			break;
		/* the client's window is shut: wait for it to open */
		if (!conn_pump(conn, -1, H2_WAIT_SEND))
			break;
		if (call->deferred && call->out_left > 0)
		{
			call->deferred = false;
			nghttp2_session_resume_data(conn->session, call->stream_id);
		}
	}
	/* the call or the connection is gone: nothing more of the message */
	call->out_left = 0;
	call->nout = 0;
	return false;
}

void
h2_call_finish(FlightCall *call, int status, const char *message, const char *sqlstate)
{
	FlightConn *conn = call->conn;

	/* an exit from inside nghttp2 -- the postmaster's death -- may not use it */
	if (conn->in_nghttp2)
		conn->dead = true;
	if (!h2_call_gone(call))
	{
		snprintf(call->status, sizeof(call->status), "%d", status);
		call->message = message ? MemoryContextStrdup(call->mcxt, flight_percent_encode(message)) : NULL;
		call->sqlstate = sqlstate ? MemoryContextStrdup(call->mcxt, sqlstate) : NULL;
		if (call->out_left > 0)
		{
			/* a message half sent cannot be ended by trailers */
			nghttp2_submit_rst_stream(conn->session, NGHTTP2_FLAG_NONE, call->stream_id,
									  NGHTTP2_INTERNAL_ERROR);
			call->out_left = 0;
			call->request_ended = true;	/* the reset ends both sides */
		}
		else if (!call->responded)
		{
			/* trailers only: one HEADERS frame that ends the stream */
			nghttp2_nv	nva[5];
			int			n = 0;

			nva[n++] = (nghttp2_nv) {(uint8_t *) ":status", (uint8_t *) "200", 7, 3, NGHTTP2_NV_FLAG_NONE};
			nva[n++] = (nghttp2_nv) {(uint8_t *) "content-type", (uint8_t *) "application/grpc", 12, 16,
				NGHTTP2_NV_FLAG_NONE};
			nva[n++] = (nghttp2_nv) {(uint8_t *) "grpc-status", (uint8_t *) call->status, 11,
				strlen(call->status), NGHTTP2_NV_FLAG_NONE};
			if (call->message && call->message[0])
				nva[n++] = (nghttp2_nv) {(uint8_t *) "grpc-message", (uint8_t *) call->message, 12,
					strlen(call->message), NGHTTP2_NV_FLAG_NONE};
			if (call->sqlstate)
				nva[n++] = (nghttp2_nv) {(uint8_t *) "x-postgresql-sqlstate", (uint8_t *) call->sqlstate,
					21, strlen(call->sqlstate), NGHTTP2_NV_FLAG_NONE};
			nghttp2_submit_response2(conn->session, call->stream_id, nva, n, NULL);
			call->responded = true;
		}
		else
		{
			call->finishing = true;
			if (call->deferred)
			{
				call->deferred = false;
				nghttp2_session_resume_data(conn->session, call->stream_id);
			}
		}
		conn_send_session(conn);
		/* a client still sending is told the call is over, after the trailers */
		if (!call->request_ended && !h2_call_gone(call))
		{
			nghttp2_submit_rst_stream(conn->session, NGHTTP2_FLAG_NONE, call->stream_id,
									  NGHTTP2_NO_ERROR);
			conn_send_session(conn);
		}
	}
	call->released = true;
	if (call->closed)
		call_free(call);
	else
		call_acknowledge(call);	/* what remains of the request, let go */
}
