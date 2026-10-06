/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * tls.c
 *	  A session's TLS, with an OpenSSL context of its own
 *	  (pg_vector_executor.md §3.15).
 *
 * PostgreSQL's own context cannot serve gRPC: it rejects a client that does
 * not offer PostgreSQL's ALPN name (PG19:src/backend/libpq/
 * be-secure-openssl.c:1795-1811), and gRPC offers "h2".  So a session makes
 * a context of its own, from vexec_flight.ssl_cert_file and
 * vexec_flight.ssl_key_file -- the server's ssl_cert_file and ssl_key_file
 * when those are empty -- relative to the data directory as the server's
 * are, and selects "h2".  Without them, or when they do not load, there is
 * no TLS, and a "hostssl" line of pg_hba.conf matches no Flight login.
 *
 * The socket is non-blocking: a read or a write that must wait says on
 * what, and h2.c waits for it in its WaitEventSet.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "libpq/libpq.h"
#include "miscadmin.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#include "flight.h"

struct FlightTls
{
	SSL_CTX    *ctx;
	SSL		   *ssl;
};

static const char *
tls_error(void)
{
	unsigned long e = ERR_get_error();
	const char *reason;

	if (e == 0)
		return "no SSL error reported";
	reason = ERR_reason_error_string(e);
	return reason ? reason : psprintf("SSL error code %lu", e);
}

/* "h2" from the client's ALPN list, or the handshake goes on without ALPN. */
static int
alpn_select(SSL *ssl, const unsigned char **out, unsigned char *outlen,
			const unsigned char *in, unsigned int inlen, void *arg)
{
	unsigned int i = 0;

	(void) ssl;
	(void) arg;
	while (i < inlen)
	{
		unsigned int len = in[i];

		if (i + 1 + len > inlen)
			break;
		if (len == 2 && memcmp(in + i + 1, "h2", 2) == 0)
		{
			*out = in + i + 1;
			*outlen = 2;
			return SSL_TLSEXT_ERR_OK;
		}
		i += 1 + len;
	}
	return SSL_TLSEXT_ERR_NOACK;
}

/*
 * The session's TLS context, or NULL when it has no certificate: the
 * extension's settings, or the server's.
 */
FlightTls *
flight_tls_create(void)
{
	const char *cert = flight_ssl_cert_file && flight_ssl_cert_file[0] ?
		flight_ssl_cert_file : ssl_cert_file;
	const char *key = flight_ssl_key_file && flight_ssl_key_file[0] ?
		flight_ssl_key_file : ssl_key_file;
	FlightTls  *tls;
	SSL_CTX    *ctx;

	if (cert == NULL || cert[0] == '\0' || key == NULL || key[0] == '\0' ||
		access(cert, R_OK) != 0 || access(key, R_OK) != 0)
		return NULL;
	ctx = SSL_CTX_new(TLS_server_method());
	if (ctx == NULL)
	{
		ereport(LOG, (errmsg("vexec_flight could not make a TLS context: %s", tls_error())));
		return NULL;
	}
	SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
	SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION);
	SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	if (SSL_CTX_use_certificate_chain_file(ctx, cert) != 1 ||
		SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
		SSL_CTX_check_private_key(ctx) != 1)
	{
		ereport(LOG,
				(errmsg("vexec_flight could not load its TLS certificate \"%s\" and key \"%s\": %s",
						cert, key, tls_error())));
		SSL_CTX_free(ctx);
		return NULL;
	}
	SSL_CTX_set_alpn_select_cb(ctx, alpn_select, NULL);
	tls = MemoryContextAllocZero(TopMemoryContext, sizeof(FlightTls));
	tls->ctx = ctx;
	return tls;
}

/* The TLS handshake on a socket, within timeout_ms: false if it failed. */
bool
flight_tls_accept(FlightTls *tls, int sock, int timeout_ms)
{
	TimestampTz deadline = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), timeout_ms);

	tls->ssl = SSL_new(tls->ctx);
	if (tls->ssl == NULL || SSL_set_fd(tls->ssl, sock) != 1)
	{
		ereport(LOG, (errmsg("vexec_flight could not begin TLS: %s", tls_error())));
		return false;
	}
	for (;;)
	{
		int			rc = SSL_accept(tls->ssl);
		int			err;
		int			events;
		long		remaining;

		if (rc == 1)
			return true;
		err = SSL_get_error(tls->ssl, rc);
		if (err == SSL_ERROR_WANT_READ)
			events = WL_SOCKET_READABLE;
		else if (err == SSL_ERROR_WANT_WRITE)
			events = WL_SOCKET_WRITEABLE;
		else
		{
			ereport(COMMERROR,
					(errcode(ERRCODE_PROTOCOL_VIOLATION),
					 errmsg("vexec_flight: TLS handshake failed: %s", tls_error())));
			return false;
		}
		remaining = TimestampDifferenceMilliseconds(GetCurrentTimestamp(), deadline);
		if (remaining <= 0)
			return false;
		(void) WaitLatchOrSocket(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH | WL_TIMEOUT | events,
								 sock, remaining, WAIT_EVENT_SSL_OPEN_SERVER);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
}

void *
flight_tls_ssl(FlightTls *tls)
{
	return tls->ssl;
}

/* What an SSL call that returned rc means: -1 to wait, -2 failed, 0 closed. */
static ssize_t
tls_result(FlightTls *tls, int rc, int *wait_events)
{
	switch (SSL_get_error(tls->ssl, rc))
	{
		case SSL_ERROR_WANT_READ:
			*wait_events = WL_SOCKET_READABLE;
			return -1;
		case SSL_ERROR_WANT_WRITE:
			*wait_events = WL_SOCKET_WRITEABLE;
			return -1;
		case SSL_ERROR_ZERO_RETURN:
			return 0;
		case SSL_ERROR_SYSCALL:
			/* an EOF without close_notify is a closed connection too */
			return errno == 0 ? 0 : -2;
		default:
			return -2;
	}
}

ssize_t
flight_tls_read(FlightTls *tls, void *buf, size_t len, int *wait_events)
{
	size_t		got;
	int			rc;

	ERR_clear_error();
	errno = 0;
	rc = SSL_read_ex(tls->ssl, buf, len, &got);
	if (rc == 1)
		return (ssize_t) got;
	return tls_result(tls, rc, wait_events);
}

ssize_t
flight_tls_write(FlightTls *tls, const void *buf, size_t len, int *wait_events)
{
	size_t		put;
	int			rc;

	ERR_clear_error();
	errno = 0;
	rc = SSL_write_ex(tls->ssl, buf, len, &put);
	if (rc == 1)
		return (ssize_t) put;
	{
		ssize_t		r = tls_result(tls, rc, wait_events);

		/* a write that "returns 0" means the peer is gone, not a short write */
		return r == 0 ? -2 : r;
	}
}

void
flight_tls_close(FlightTls *tls)
{
	if (tls->ssl)
	{
		(void) SSL_shutdown(tls->ssl);
		SSL_free(tls->ssl);
		tls->ssl = NULL;
	}
}
