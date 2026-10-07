/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * login.c
 *	  A Flight login, checked as PostgreSQL checks a backend's, with the
 *	  client's real address (pg_vector_executor.md §3.15).
 *
 * It runs in the acceptor, which is connected to no database: what a login
 * needs -- pg_authid, pg_auth_members, pg_database -- is shared, and a
 * background worker connects to a database once, so a session can be told
 * its role and its database before it connects to them, as a backend
 * authenticates before it selects its database (PG19:src/backend/utils/
 * init/postinit.c, InitPostgres).
 *
 *	pg_hba.conf		its lines matched as hba_getauthmethod() matches them
 *					(PG19:src/include/libpq/hba.h:192), on a Port carrying
 *					the client's address, its user, its database and
 *					whether its connection is TLS -- by the acceptor's own
 *					copy of the lines and of the match (hba.c), since a
 *					background worker cannot use the postmaster's;
 *					only trust, reject, password, md5 and scram-sha-256
 *					lines are honoured, and any other method, or a line
 *					that asks for a client certificate, refuses the login
 *	the password	Flight's basic authentication carries it in the clear,
 *					inside TLS where the line asks for it: get_role_password()
 *					and plain_crypt_verify() check it against the stored
 *					secret (PG19:src/include/libpq/crypt.h:54, 59), an
 *					expired rolvaliduntil failing as it does for a backend;
 *					a scram-sha-256 line takes a SCRAM secret only, as
 *					PostgreSQL's SCRAM exchange does
 *	the role		rolcanlogin
 *	the database	datallowconn, and CONNECT
 *	limits			rolconnlimit and datconnlimit, counting the backends
 *					PostgreSQL counts and the Flight sessions it does not
 *					(PG19:src/backend/storage/ipc/procarray.c:3662-3720)
 *
 * The client sees one message for a password that failed, whatever the
 * reason, as a backend's client does; the server's log has the reason.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "common/ip.h"
#include "libpq/crypt.h"
#include "libpq/hba.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "flight.h"

static void
refuse(FlightLoginReply *reply, int grpc_status, int sqlerrcode, const char *fmt,...)
	pg_attribute_printf(4, 5);

static void
refuse(FlightLoginReply *reply, int grpc_status, int sqlerrcode, const char *fmt,...)
{
	va_list		args;

	reply->ok = false;
	reply->grpc_status = grpc_status;
	strlcpy(reply->sqlstate, unpack_sql_state(sqlerrcode), sizeof(reply->sqlstate));
	va_start(args, fmt);
	vsnprintf(reply->message, sizeof(reply->message), fmt, args);
	va_end(args);
}

/*
 * A database's row, by its name, as InitPostgres finds it: a heap scan, since
 * a process connected to no database cannot open pg_database's indexes on
 * names (PG19:src/backend/utils/init/postinit.c, GetDatabaseTuple).  Its
 * CONNECT privilege for a role, from the row's ACL.
 */
static HeapTuple
database_tuple(const char *name, Oid roleid, bool *may_connect)
{
	Relation	rel = table_open(DatabaseRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tup;

	ScanKeyInit(&key, Anum_pg_database_datname, BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(name));
	scan = systable_beginscan(rel, DatabaseNameIndexId, false, NULL, 1, &key);
	tup = systable_getnext(scan);
	*may_connect = false;
	if (HeapTupleIsValid(tup))
	{
		Form_pg_database db = (Form_pg_database) GETSTRUCT(tup);
		bool		isnull;
		Datum		datacl = heap_getattr(tup, Anum_pg_database_datacl, RelationGetDescr(rel), &isnull);
		Acl		   *acl = isnull ? acldefault(OBJECT_DATABASE, db->datdba) : DatumGetAclP(datacl);

		*may_connect = aclmask(acl, roleid, db->datdba, ACL_CONNECT, ACLMASK_ANY) != 0;
		tup = heap_copytuple(tup);
	}
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	return tup;
}

static void
check_login(const FlightLoginRequest *req, FlightLoginReply *reply)
{
	Port		port;
	char		host[NI_MAXHOST];
	const char *logdetail = NULL;
	HeapTuple	tup;
	Form_pg_authid role;
	Form_pg_database db;
	Oid			roleid;
	Oid			dboid;
	bool		superuser;
	bool		may_connect;
	int			limit;

	memset(&port, 0, sizeof(port));
	memcpy(&port.raddr.addr, &req->addr, Min(req->addrlen, sizeof(port.raddr.addr)));
	port.raddr.salen = req->addrlen;
	host[0] = '\0';
	(void) pg_getnameinfo_all(&port.raddr.addr, port.raddr.salen, host, sizeof(host),
							  NULL, 0, NI_NUMERICHOST);
	port.remote_host = host;
	port.user_name = (char *) req->user;
	port.database_name = (char *) req->database;
	port.ssl_in_use = req->ssl;

	/* pg_hba.conf: the acceptor's own copy of its lines (hba.c) */
	flight_check_hba(&port);
	switch (port.hba->auth_method)
	{
		case uaTrust:
			break;
		case uaPassword:
		case uaMD5:
		case uaSCRAM:
			{
				char	   *shadow;

				shadow = get_role_password(req->user, &logdetail);
				if (shadow != NULL && port.hba->auth_method == uaSCRAM &&
					get_password_type(shadow) != PASSWORD_TYPE_SCRAM_SHA_256)
				{
					logdetail = psprintf("User \"%s\" does not have a valid SCRAM secret.", req->user);
					shadow = NULL;
				}
				if (shadow == NULL ||
					plain_crypt_verify(req->user, shadow, req->password, &logdetail) != STATUS_OK)
				{
					ereport(LOG,
							(errmsg("vexec_flight: password authentication failed for user \"%s\"",
									req->user),
							 logdetail ? errdetail_log("%s", logdetail) : 0));
					refuse(reply, GRPC_UNAUTHENTICATED, ERRCODE_INVALID_PASSWORD,
						   "password authentication failed for user \"%s\"", req->user);
					return;
				}
				break;
			}
		case uaReject:
		case uaImplicitReject:
			/* PostgreSQL's own words (PG19:src/backend/libpq/auth.c, ClientAuthentication) */
			refuse(reply, GRPC_UNAUTHENTICATED, ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION,
				   "%s for host \"%s\", user \"%s\", database \"%s\", %s",
				   port.hba->auth_method == uaReject ? "pg_hba.conf rejects connection"
				   : "no pg_hba.conf entry", host, req->user, req->database,
				   req->ssl ? "SSL encryption" : "no encryption");
			return;
		default:
			refuse(reply, GRPC_UNAUTHENTICATED, ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION,
				   "authentication method \"%s\" is not supported for Flight logins (user \"%s\")",
				   hba_authname(port.hba->auth_method), req->user);
			return;
	}
	if (port.hba->clientcert != clientCertOff)
	{
		refuse(reply, GRPC_UNAUTHENTICATED, ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION,
			   "the pg_hba.conf entry for user \"%s\" asks for a client certificate, which Flight logins do not offer",
			   req->user);
		return;
	}

	/* the role */
	tup = SearchSysCache1(AUTHNAME, PointerGetDatum(req->user));
	if (!HeapTupleIsValid(tup))
	{
		refuse(reply, GRPC_UNAUTHENTICATED, ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION,
			   "role \"%s\" does not exist", req->user);
		return;
	}
	role = (Form_pg_authid) GETSTRUCT(tup);
	roleid = role->oid;
	superuser = role->rolsuper;
	if (!role->rolcanlogin)
	{
		ReleaseSysCache(tup);
		refuse(reply, GRPC_UNAUTHENTICATED, ERRCODE_INVALID_AUTHORIZATION_SPECIFICATION,
			   "role \"%s\" is not permitted to log in", req->user);
		return;
	}
	limit = role->rolconnlimit;
	ReleaseSysCache(tup);
	if (limit >= 0 && !superuser &&
		CountUserBackends(roleid) + flight_count_sessions(roleid, InvalidOid) >= limit)
	{
		refuse(reply, GRPC_RESOURCE_EXHAUSTED, ERRCODE_TOO_MANY_CONNECTIONS,
			   "too many connections for role \"%s\"", req->user);
		return;
	}

	/* the database */
	tup = database_tuple(req->database, roleid, &may_connect);
	if (!HeapTupleIsValid(tup))
	{
		refuse(reply, GRPC_NOT_FOUND, ERRCODE_UNDEFINED_DATABASE,
			   "database \"%s\" does not exist", req->database);
		return;
	}
	db = (Form_pg_database) GETSTRUCT(tup);
	dboid = db->oid;
	if (database_is_invalid_form(db))
	{
		refuse(reply, GRPC_FAILED_PRECONDITION, ERRCODE_FEATURE_NOT_SUPPORTED,
			   "cannot connect to invalid database \"%s\"", req->database);
		return;
	}
	if (!db->datallowconn)
	{
		refuse(reply, GRPC_FAILED_PRECONDITION, ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
			   "database \"%s\" is not currently accepting connections", req->database);
		return;
	}
	limit = db->datconnlimit;
	if (!superuser && !may_connect)
	{
		refuse(reply, GRPC_PERMISSION_DENIED, ERRCODE_INSUFFICIENT_PRIVILEGE,
			   "permission denied for database \"%s\"", req->database);
		return;
	}
	if (limit >= 0 && !superuser &&
		CountDBConnections(dboid) + flight_count_sessions(InvalidOid, dboid) >= limit)
	{
		refuse(reply, GRPC_RESOURCE_EXHAUSTED, ERRCODE_TOO_MANY_CONNECTIONS,
			   "too many connections for database \"%s\"", req->database);
		return;
	}

	reply->ok = true;
	reply->roleid = roleid;
	reply->dboid = dboid;
	reply->grpc_status = GRPC_OK;
}

/*
 * Check a login, in a transaction of its own; an error inside, such as a
 * catalog that cannot be read, refuses it.
 */
void
flight_login_check(const FlightLoginRequest *req, FlightLoginReply *reply)
{
	MemoryContext oldcxt = CurrentMemoryContext;

	memset(reply, 0, sizeof(*reply));
	reply->magic = FLIGHT_MAGIC;
	if (memchr(req->user, '\0', sizeof(req->user)) == NULL ||
		memchr(req->database, '\0', sizeof(req->database)) == NULL ||
		memchr(req->password, '\0', sizeof(req->password)) == NULL)
	{
		refuse(reply, GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "a malformed login");
		return;
	}

	PG_TRY();
	{
		StartTransactionCommand();
		check_login(req, reply);
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcxt);
		edata = CopyErrorData();
		FlushErrorState();
		AbortCurrentTransaction();
		ereport(LOG,
				(errmsg("vexec_flight: a login for user \"%s\" failed: %s", req->user, edata->message)));
		refuse(reply, GRPC_UNAVAILABLE, edata->sqlerrcode, "%s", edata->message);
		FreeErrorData(edata);
	}
	PG_END_TRY();
}
