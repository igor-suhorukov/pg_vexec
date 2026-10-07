/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_flight.c
 *	  _PG_init: vexec_flight's settings, the shared table of sessions, and
 *	  the acceptor (pg_vector_executor.md §3.15).
 *
 * vexec_flight may only be preloaded.  It serves Flight only while vexec's
 * vector executor is active, at three levels (§3.15):
 *
 *	1. its acceptor starts only when vexec_flight.listen_addresses names an
 *	   address -- empty by default -- and, when it starts, exits with a
 *	   WARNING if vexec's egress API is not there, since the preload order
 *	   is free;
 *	2. a session runs a statement only while the API says the vector
 *	   executor is active in it, checked at each statement, and fails it
 *	   with FAILED_PRECONDITION, naming vexec.mode, otherwise;
 *	3. every result goes through vexec's receiver: the extension has no row
 *	   path to clients.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/guc.h"

#include "flight.h"

PG_MODULE_MAGIC_EXT(
					.name = "vexec_flight",
					.version = VEXEC_FLIGHT_VERSION
);

char	   *flight_listen_addresses = NULL;
int			flight_port = 32010;
char	   *flight_ssl_cert_file = NULL;
char	   *flight_ssl_key_file = NULL;
int			flight_max_sessions = 16;
int			flight_idle_session_timeout = 10 * 60 * 1000;

FlightShared *flight_shared = NULL;

static shmem_request_hook_type prev_shmem_request = NULL;
static shmem_startup_hook_type prev_shmem_startup = NULL;

static Size
shared_size(void)
{
	return add_size(offsetof(FlightShared, slots),
					mul_size(sizeof(FlightSlot), flight_max_sessions));
}

static void
flight_shmem_request(void)
{
	if (prev_shmem_request)
		prev_shmem_request();
	RequestAddinShmemSpace(shared_size());
}

static void
flight_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup)
		prev_shmem_startup();
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	flight_shared = ShmemInitStruct("vexec_flight sessions", shared_size(), &found);
	if (!found)
	{
		memset(flight_shared, 0, shared_size());
		SpinLockInit(&flight_shared->mutex);
		flight_shared->nslots = flight_max_sessions;
	}
	LWLockRelease(AddinShmemInitLock);
}

/*
 * The Flight sessions of a role or a database, logged in or logging in, for
 * their connection limits: PostgreSQL counts backends only.
 */
int
flight_count_sessions(Oid roleid, Oid dboid)
{
	int			n = 0;
	int			i;

	SpinLockAcquire(&flight_shared->mutex);
	for (i = 0; i < flight_shared->nslots; i++)
	{
		FlightSlot *s = &flight_shared->slots[i];

		if (s->state != FLIGHT_SLOT_ACTIVE)
			continue;
		if ((OidIsValid(roleid) && s->roleid == roleid) ||
			(OidIsValid(dboid) && s->dboid == dboid))
			n++;
	}
	SpinLockRelease(&flight_shared->mutex);
	return n;
}

/* gRPC's grpc-message: UTF-8, any byte outside ' '..'~', and '%', as %XX. */
char *
flight_percent_encode(const char *s)
{
	StringInfoData out;
	const unsigned char *p;

	initStringInfo(&out);
	for (p = (const unsigned char *) s; *p; p++)
	{
		if (*p < 0x20 || *p > 0x7e || *p == '%')
			appendStringInfo(&out, "%%%02X", *p);
		else
			appendStringInfoChar(&out, (char) *p);
	}
	return out.data;
}

static void
define_settings(void)
{
	DefineCustomStringVariable("vexec_flight.listen_addresses",
							   "The addresses the Flight SQL endpoint listens on.",
							   "A comma-separated list of host names or numeric addresses, "
							   "or * for all; empty, the default, starts no acceptor.",
							   &flight_listen_addresses,
							   "",
							   PGC_POSTMASTER, GUC_LIST_INPUT,
							   NULL, NULL, NULL);
	DefineCustomIntVariable("vexec_flight.port",
							"The Flight SQL endpoint's TCP port.",
							NULL,
							&flight_port,
							32010, 1, 65535,
							PGC_POSTMASTER, 0,
							NULL, NULL, NULL);
	DefineCustomStringVariable("vexec_flight.ssl_cert_file",
							   "The Flight SQL endpoint's TLS certificate; empty takes ssl_cert_file's.",
							   "Without a certificate and a key the endpoint serves plaintext only, "
							   "which hostssl lines of pg_hba.conf do not match.",
							   &flight_ssl_cert_file,
							   "",
							   PGC_SIGHUP, 0,
							   NULL, NULL, NULL);
	DefineCustomStringVariable("vexec_flight.ssl_key_file",
							   "The Flight SQL endpoint's TLS key; empty takes ssl_key_file's.",
							   NULL,
							   &flight_ssl_key_file,
							   "",
							   PGC_SIGHUP, 0,
							   NULL, NULL, NULL);
	DefineCustomIntVariable("vexec_flight.max_sessions",
							"Flight sessions at once.",
							"Each is a background worker, counted in max_worker_processes.",
							&flight_max_sessions,
							16, 1, MAX_BACKENDS,
							PGC_POSTMASTER, 0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("vexec_flight.idle_session_timeout",
							"A Flight session idle this long ends.",
							"0 never ends one.",
							&flight_idle_session_timeout,
							10 * 60 * 1000, 0, INT_MAX,
							PGC_SIGHUP, GUC_UNIT_MS,
							NULL, NULL, NULL);
}

void
_PG_init(void)
{
	BackgroundWorker worker;

	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("vexec_flight must be loaded via \"shared_preload_libraries\"")));

	define_settings();
	MarkGUCPrefixReserved("vexec_flight");

	prev_shmem_request = shmem_request_hook;
	shmem_request_hook = flight_shmem_request;
	prev_shmem_startup = shmem_startup_hook;
	shmem_startup_hook = flight_shmem_startup;

	/* level 1: no address, no acceptor */
	if (flight_listen_addresses == NULL || flight_listen_addresses[0] == '\0')
		return;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 5;
	strlcpy(worker.bgw_library_name, "vexec_flight", BGW_MAXLEN);
	strlcpy(worker.bgw_function_name, "vexec_flight_acceptor_main", BGW_MAXLEN);
	strlcpy(worker.bgw_name, "vexec_flight acceptor", BGW_MAXLEN);
	strlcpy(worker.bgw_type, "vexec_flight acceptor", BGW_MAXLEN);
	RegisterBackgroundWorker(&worker);
}
