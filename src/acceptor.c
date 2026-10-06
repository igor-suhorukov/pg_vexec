/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * acceptor.c
 *	  The acceptor: a background worker without threads that listens on the
 *	  Flight port, and starts a session for each connection
 *	  (pg_vector_executor.md §3.15).
 *
 * The postmaster accepts a connection and forks a backend that owns its
 * socket (PG19:src/backend/postmaster/postmaster.c:3584).  An extension
 * cannot fork from the postmaster, so the acceptor does what it can: for
 * each connection it reserves a slot of the session table, asks the
 * postmaster for a session worker (RegisterDynamicBackgroundWorker,
 * PG19:src/include/postmaster/bgworker.h:125) with the slot and a token in
 * bgw_extra, and hands the worker the socket when it connects to the
 * acceptor's UNIX-domain socket and shows the token (SCM_RIGHTS).  The
 * session then owns the socket, as a backend does.
 *
 * The acceptor also checks the sessions' logins (login.c): it is connected
 * to no database, and reads the shared catalogs a login needs, and its own
 * copy of pg_hba.conf, which a background worker does not inherit -- the
 * postmaster's parsed lines are freed when a worker starts
 * (PG19:src/backend/postmaster/bgworker.c:748-751), so the acceptor reads
 * them again with PostgreSQL's parser into a context of its own (hba.c).  A
 * session asks once it has read its client's credentials, and connects only
 * to the role and the database the acceptor names.
 *
 * Without vexec's egress API in this server the acceptor logs a WARNING
 * and exits, and is not started again: the Flight endpoint serves only
 * while vexec is there to serve it (§3.15).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/ip.h"
#include "libpq/hba.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/waiteventset.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"
#include "utils/wait_event.h"

#include "flight.h"

#define MAX_LISTEN			16
#define HANDOVER_TIMEOUT_MS	60000

/* A connection waiting for its session worker. */
typedef struct Pending
{
	bool		used;
	int			fd;
	int			slot;
	uint64		token;
	TimestampTz since;
	BackgroundWorkerHandle *handle;
} Pending;

/* A session worker's connection to the acceptor. */
typedef struct SessionLink
{
	bool		used;
	int			fd;
	bool		helloed;		/* it showed its token, and has its socket */
	int			slot;
	char		buf[sizeof(FlightLoginRequest)];
	size_t		len;
	TimestampTz since;
} SessionLink;

static pgsocket listen_fds[MAX_LISTEN];
static int	nlisten = 0;
static int	unix_fd = -1;
static char unix_path[MAXPGPATH];
static Pending *pending;
static SessionLink *links;
static int	nlinks;
static MemoryContext loopcxt;

static void
remove_unix_socket(int code, Datum arg)
{
	(void) code;
	(void) arg;
	if (unix_path[0])
		unlink(unix_path);
}

/* pg_hba.conf, read again (hba.c): at the start, and on SIGHUP. */
static void
reload_hba(void)
{
	if (!flight_load_hba())
		ereport(LOG,
				(errmsg("vexec_flight: pg_hba.conf was not reloaded"),
				 errdetail("Flight logins keep the lines read before.")));
}

/* The Flight port on each of vexec_flight.listen_addresses. */
static void
listen_flight(void)
{
	char	   *raw = pstrdup(flight_listen_addresses);
	List	   *elems;
	ListCell   *lc;

	if (!SplitGUCList(raw, ',', &elems))
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid list syntax in \"vexec_flight.listen_addresses\"")));
	foreach(lc, elems)
	{
		char	   *host = lfirst(lc);

		(void) ListenServerPort(AF_UNSPEC, strcmp(host, "*") == 0 ? NULL : host,
								(unsigned short) flight_port, NULL,
								listen_fds, &nlisten, MAX_LISTEN);
	}
	if (nlisten == 0)
		ereport(FATAL,
				(errmsg("vexec_flight could not listen on any of \"%s\", port %d",
						flight_listen_addresses, flight_port)));
}

/* The acceptor's UNIX-domain socket, in the data directory. */
static void
listen_sessions(void)
{
	struct sockaddr_un addr;

	snprintf(unix_path, sizeof(unix_path), FLIGHT_SOCKET_NAME, flight_port);
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(unix_path) >= sizeof(addr.sun_path))
		elog(FATAL, "vexec_flight: the socket path \"%s\" is too long", unix_path);
	strlcpy(addr.sun_path, unix_path, sizeof(addr.sun_path));
	unlink(unix_path);
	unix_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (unix_fd < 0 ||
		bind(unix_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
		chmod(unix_path, S_IRUSR | S_IWUSR) < 0 ||
		listen(unix_fd, 64) < 0)
		ereport(FATAL,
				(errcode_for_socket_access(),
				 errmsg("vexec_flight could not listen on \"%s\": %m", unix_path)));
	before_shmem_exit(remove_unix_socket, 0);
}

static void
free_slot(int slot)
{
	SpinLockAcquire(&flight_shared->mutex);
	if (flight_shared->slots[slot].state == FLIGHT_SLOT_RESERVED)
		flight_shared->slots[slot].state = FLIGHT_SLOT_FREE;
	SpinLockRelease(&flight_shared->mutex);
}

static uint64
random_token(void)
{
	uint64		t;

	if (!pg_strong_random(&t, sizeof(t)))
		elog(ERROR, "vexec_flight could not make a random token");
	return t;
}

/* A client's connection: a slot, a session worker, and the socket held for it. */
static void
accept_client(pgsocket lfd)
{
	struct sockaddr_storage addr;
	socklen_t	addrlen = sizeof(addr);
	int			fd;
	int			slot = -1;
	int			p;
	uint64		token;
	BackgroundWorker worker;
	BackgroundWorkerHandle *handle;

	fd = accept4(lfd, (struct sockaddr *) &addr, &addrlen, SOCK_CLOEXEC);
	if (fd < 0)
		return;

	/*
	 * As pq_init() sets a backend's TCP connection
	 * (PG19:src/backend/libpq/pqcomm.c:207-222): without TCP_NODELAY, a
	 * response's last frames wait for the client's delayed ACK, some 40 ms a
	 * call.
	 */
	{
		int			on = 1;

		if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) < 0 ||
			setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) < 0)
		{
			ereport(LOG,
					(errmsg("vexec_flight: %s failed on a client's connection: %m", "setsockopt")));
			closesocket(fd);
			return;
		}
	}

	for (p = 0; p < flight_max_sessions && pending[p].used; p++)
		;
	token = random_token();
	SpinLockAcquire(&flight_shared->mutex);
	if (p < flight_max_sessions)
	{
		int			i;

		for (i = 0; i < flight_shared->nslots; i++)
			if (flight_shared->slots[i].state == FLIGHT_SLOT_FREE)
			{
				slot = i;
				memset(&flight_shared->slots[i], 0, sizeof(FlightSlot));
				flight_shared->slots[i].state = FLIGHT_SLOT_RESERVED;
				flight_shared->slots[i].token = token;
				flight_shared->slots[i].since = GetCurrentTimestamp();
				break;
			}
	}
	SpinLockRelease(&flight_shared->mutex);
	if (slot < 0)
	{
		ereport(LOG,
				(errmsg("vexec_flight: a connection was refused: \"vexec_flight.max_sessions\" (%d) sessions are open",
						flight_max_sessions)));
		closesocket(fd);
		return;
	}

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	strlcpy(worker.bgw_library_name, "vexec_flight", BGW_MAXLEN);
	strlcpy(worker.bgw_function_name, "vexec_flight_session_main", BGW_MAXLEN);
	strlcpy(worker.bgw_name, "vexec_flight session", BGW_MAXLEN);
	strlcpy(worker.bgw_type, "vexec_flight session", BGW_MAXLEN);
	worker.bgw_main_arg = Int32GetDatum(slot);
	memcpy(worker.bgw_extra, &token, sizeof(token));
	worker.bgw_notify_pid = MyProcPid;
	{
		/* the handle outlives the loop's memory */
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
		bool		registered = RegisterDynamicBackgroundWorker(&worker, &handle);

		MemoryContextSwitchTo(old);
		if (!registered)
		{
			ereport(LOG,
					(errmsg("vexec_flight: a connection was refused: no background worker slot is free"),
					 errhint("Raise \"max_worker_processes\".")));
			free_slot(slot);
			closesocket(fd);
			return;
		}
	}
	pending[p].used = true;
	pending[p].fd = fd;
	pending[p].slot = slot;
	pending[p].token = token;
	pending[p].since = GetCurrentTimestamp();
	pending[p].handle = handle;
}

/* A session worker connects: it will show its token. */
static void
accept_session(void)
{
	int			fd = accept4(unix_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	int			i;

	if (fd < 0)
		return;
	for (i = 0; i < nlinks && links[i].used; i++)
		;
	if (i == nlinks)
	{
		closesocket(fd);
		return;
	}
	memset(&links[i], 0, sizeof(SessionLink));
	links[i].used = true;
	links[i].fd = fd;
	links[i].since = GetCurrentTimestamp();
}

static void
close_link(SessionLink *link)
{
	closesocket(link->fd);
	link->used = false;
}

/* The client's socket, to the worker that showed its slot's token. */
static void
hand_over(SessionLink *link, const FlightHello *hello)
{
	int			p;

	for (p = 0; p < flight_max_sessions; p++)
		if (pending[p].used && pending[p].slot == hello->slot && pending[p].token == hello->token)
			break;
	if (hello->magic != FLIGHT_MAGIC || p == flight_max_sessions)
	{
		close_link(link);
		return;
	}
	{
		struct msghdr msg;
		struct iovec iov;
		char		ok = 1;
		union
		{
			struct cmsghdr hdr;
			char		buf[CMSG_SPACE(sizeof(int))];
		}			control;
		struct cmsghdr *cmsg;

		memset(&msg, 0, sizeof(msg));
		memset(&control, 0, sizeof(control));
		iov.iov_base = &ok;
		iov.iov_len = 1;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control.buf;
		msg.msg_controllen = sizeof(control.buf);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &pending[p].fd, sizeof(int));
		if (sendmsg(link->fd, &msg, MSG_NOSIGNAL) != 1)
		{
			close_link(link);
			return;
		}
	}
	closesocket(pending[p].fd);
	pending[p].used = false;
	pfree(pending[p].handle);
	link->helloed = true;
	link->slot = hello->slot;
	link->len = 0;
}

/* Bytes from a session's link: its hello, then its logins, a whole struct each. */
static void
read_link(SessionLink *link)
{
	size_t		want = link->helloed ? sizeof(FlightLoginRequest) : sizeof(FlightHello);
	ssize_t		n = recv(link->fd, link->buf + link->len, want - link->len, 0);

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
	{
		close_link(link);
		return;
	}
	if (n < 0)
		return;
	link->len += n;
	if (link->len < want)
		return;
	link->len = 0;
	if (!link->helloed)
	{
		FlightHello hello;

		memcpy(&hello, link->buf, sizeof(hello));
		hand_over(link, &hello);
	}
	else
	{
		FlightLoginRequest *req = (FlightLoginRequest *) link->buf;
		FlightLoginReply reply;

		if (req->magic != FLIGHT_MAGIC || req->slot != link->slot)
		{
			close_link(link);
			return;
		}
		flight_login_check(req, &reply);
		explicit_bzero(link->buf, sizeof(link->buf));
		if (send(link->fd, &reply, sizeof(reply), MSG_NOSIGNAL) != sizeof(reply))
			close_link(link);
	}
}

/* Connections whose worker never came, or died before it took them. */
static void
expire_pending(void)
{
	TimestampTz now = GetCurrentTimestamp();
	int			p;
	int			i;

	for (p = 0; p < flight_max_sessions; p++)
	{
		pid_t		pid;

		if (!pending[p].used)
			continue;
		if (GetBackgroundWorkerPid(pending[p].handle, &pid) == BGWH_STOPPED ||
			TimestampDifferenceExceeds(pending[p].since, now, HANDOVER_TIMEOUT_MS))
		{
			closesocket(pending[p].fd);
			free_slot(pending[p].slot);
			pfree(pending[p].handle);
			pending[p].used = false;
		}
	}
	for (i = 0; i < nlinks; i++)
		if (links[i].used && !links[i].helloed &&
			TimestampDifferenceExceeds(links[i].since, now, HANDOVER_TIMEOUT_MS))
			close_link(&links[i]);
}

void
vexec_flight_acceptor_main(Datum arg)
{
	(void) arg;

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();

	/* level 1: vexec's egress API, or no endpoint */
	if (vexec_egress_lookup() == NULL)
	{
		ereport(WARNING,
				(errmsg("vexec_flight serves Flight only with vexec, which is not loaded"),
				 errdetail("vexec's egress API, \"%s\", is not in this server.", VEXEC_EGRESS_RENDEZVOUS),
				 errhint("Add vexec to \"shared_preload_libraries\".")));
		proc_exit(0);			/* not started again */
	}

	SpinLockAcquire(&flight_shared->mutex);
	flight_shared->acceptor_pid = MyProcPid;
	SpinLockRelease(&flight_shared->mutex);

	/* the shared catalogs, for logins; no database */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);
	reload_hba();

	pending = MemoryContextAllocZero(TopMemoryContext, sizeof(Pending) * flight_max_sessions);
	nlinks = flight_max_sessions * 2;
	links = MemoryContextAllocZero(TopMemoryContext, sizeof(SessionLink) * nlinks);
	listen_flight();
	listen_sessions();
	loopcxt = AllocSetContextCreate(TopMemoryContext, "vexec_flight acceptor loop",
									ALLOCSET_DEFAULT_SIZES);

	for (;;)
	{
		WaitEventSet *wes;
		WaitEvent	ev[MAX_LISTEN + 64];
		int			n;
		int			i;

		MemoryContextReset(loopcxt);
		MemoryContextSwitchTo(loopcxt);

		/* the set, made again each turn: its sockets come and go */
		wes = CreateWaitEventSet(NULL, 2 + nlisten + 1 + nlinks);
		AddWaitEventToSet(wes, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
		AddWaitEventToSet(wes, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
		for (i = 0; i < nlisten; i++)
			AddWaitEventToSet(wes, WL_SOCKET_ACCEPT, listen_fds[i], NULL, NULL);
		AddWaitEventToSet(wes, WL_SOCKET_ACCEPT, unix_fd, NULL, NULL);
		for (i = 0; i < nlinks; i++)
			if (links[i].used)
				AddWaitEventToSet(wes, WL_SOCKET_READABLE, links[i].fd, NULL, &links[i]);

		n = WaitEventSetWait(wes, 1000, ev, lengthof(ev), PG_WAIT_EXTENSION);
		for (i = 0; i < n; i++)
		{
			if (ev[i].events & WL_LATCH_SET)
			{
				ResetLatch(MyLatch);
				CHECK_FOR_INTERRUPTS();
				if (ConfigReloadPending)
				{
					ConfigReloadPending = false;
					ProcessConfigFile(PGC_SIGHUP);
					reload_hba();
				}
			}
			else if (ev[i].events & (WL_SOCKET_READABLE | WL_SOCKET_ACCEPT))
			{
				/* WL_SOCKET_ACCEPT is WL_SOCKET_READABLE here: told apart by socket */
				int			l;

				if (ev[i].fd == unix_fd)
				{
					accept_session();
					continue;
				}
				for (l = 0; l < nlisten; l++)
					if (ev[i].fd == listen_fds[l])
						break;
				if (l < nlisten)
					accept_client(listen_fds[l]);
				else if (ev[i].user_data != NULL && ((SessionLink *) ev[i].user_data)->used)
					read_link((SessionLink *) ev[i].user_data);
			}
		}
		FreeWaitEventSet(wes);
		expire_pending();
	}
}
