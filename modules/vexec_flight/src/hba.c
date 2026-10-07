/* SPDX-License-Identifier: Apache-2.0 AND PostgreSQL */
/*-------------------------------------------------------------------------
 *
 * hba.c
 *	  pg_hba.conf for Flight logins: the acceptor's own copy of its lines,
 *	  and the match of a login's client, database and role against them
 *	  (pg_vector_executor.md §3.15).
 *
 * PostgreSQL's hba_getauthmethod() matches against the lines the postmaster
 * parsed into its PostmasterContext (PG19:src/backend/libpq/hba.c:2338-2438,
 * check_hba).  A background worker frees that context when it starts
 * (PG19:src/backend/postmaster/bgworker.c:748-752), and the static pointers
 * hba.c keeps to the lines and their context are left dangling: matching
 * would read freed memory, and load_hba() deletes the old context before it
 * keeps the new one (hba.c:2535), a second free of a freed context
 * that corrupted the acceptor's memory when it was tried.  So the acceptor
 * reads pg_hba.conf with PostgreSQL's own exported parser --
 * open_auth_file(), tokenize_auth_file(), parse_hba_line() and
 * free_auth_file() (PG19:src/include/libpq/hba.h:196-201) -- into a memory
 * context of its own, and matches a login with the functions below.
 *
 * Those functions are copies of hba.c's static check_hba(), check_db(),
 * check_role(), is_member(), check_hostname(), hostname_match(), ipv4eq(),
 * ipv6eq(), check_ip(), check_network_callback(),
 * check_same_host_or_net() and regexec_auth_token() at 0f433a6b357
 * (PG19:src/backend/libpq/hba.c:343-360, 920-1223, 2338-2438), changed only
 * where a Flight login is not a backend's: no walsender, the lines passed in.
 * They carry PostgreSQL's notice:
 *
 *	Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *
 *	Permission to use, copy, modify, and distribute this software and its
 *	documentation for any purpose, without fee, and without a written
 *	agreement is hereby granted, provided that the above copyright notice
 *	and this paragraph and the following two paragraphs appear in all
 *	copies.  (The PostgreSQL License, PG19:COPYRIGHT.)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "common/ip.h"
#include "libpq/hba.h"
#include "libpq/ifaddr.h"
#include "libpq/libpq-be.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "regex/regex.h"
#include "utils/acl.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "flight.h"

#define token_has_regexp(t)	(t->regex != NULL)
#define token_is_member_check(t)	(!t->quoted && t->string[0] == '+')
#define token_is_keyword(t, k)	(!t->quoted && strcmp(t->string, k) == 0)
#define token_matches(t, k)  (strcmp(t->string, k) == 0)
#define token_matches_insensitive(t,k) (pg_strcasecmp(t->string, k) == 0)

typedef struct
{
	IPCompareMethod method;
	SockAddr   *raddr;
	bool		result;
} check_network_data;

static List *flight_hba_lines = NIL;
static MemoryContext flight_hba_context = NULL;

/*
 * pg_hba.conf, read into the acceptor's own context, as load_hba() reads it
 * (hba.c:2452-2540): on an error the lines read before are kept.
 */
bool
flight_load_hba(void)
{
	FILE	   *file;
	List	   *hba_lines = NIL;
	List	   *new_lines = NIL;
	ListCell   *line;
	bool		ok = true;
	MemoryContext cxt;
	MemoryContext old;

	file = open_auth_file(HbaFileName, LOG, 0, NULL);
	if (file == NULL)
		return false;
	tokenize_auth_file(HbaFileName, file, &hba_lines, LOG, 0);
	cxt = AllocSetContextCreate(TopMemoryContext, "vexec_flight pg_hba.conf", ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(cxt);
	foreach(line, hba_lines)
	{
		TokenizedAuthLine *tok_line = (TokenizedAuthLine *) lfirst(line);
		HbaLine    *parsed;

		if (tok_line->err_msg != NULL)
		{
			ok = false;
			continue;
		}
		if ((parsed = parse_hba_line(tok_line, LOG)) == NULL)
		{
			ok = false;
			continue;
		}
		new_lines = lappend(new_lines, parsed);
	}
	MemoryContextSwitchTo(old);
	free_auth_file(file, 0);
	if (!ok || new_lines == NIL)
	{
		MemoryContextDelete(cxt);
		return false;
	}
	if (flight_hba_context)
		MemoryContextDelete(flight_hba_context);
	flight_hba_context = cxt;
	flight_hba_lines = new_lines;
	return true;
}

/* hba.c:343-360 */
static int
regexec_auth_token(const char *match, AuthToken *token, size_t nmatch,
				   regmatch_t pmatch[])
{
	pg_wchar   *wmatchstr;
	int			wmatchlen;
	int			r;

	Assert(token->string[0] == '/' && token->regex);

	wmatchstr = palloc((strlen(match) + 1) * sizeof(pg_wchar));
	wmatchlen = pg_mb2wchar_with_len(match, wmatchstr, strlen(match));

	r = pg_regexec(token->regex, wmatchstr, wmatchlen, 0, NULL, nmatch, pmatch, 0);

	pfree(wmatchstr);
	return r;
}

/* hba.c:920-941 */
static bool
is_member(Oid userid, const char *role)
{
	Oid			roleid;

	if (!OidIsValid(userid))
		return false;			/* if user not exist, say "no" */

	roleid = get_role_oid(role, true);

	if (!OidIsValid(roleid))
		return false;			/* if target role not exist, say "no" */

	/*
	 * See if user is directly or indirectly a member of role. For this
	 * purpose, a superuser is not considered to be automatically a member of
	 * the role, so group auth only applies to explicit membership.
	 */
	return is_member_of_role_nosuper(userid, roleid);
}

/* hba.c:949-980 */
static bool
check_role(const char *role, Oid roleid, List *tokens, bool case_insensitive)
{
	ListCell   *cell;
	AuthToken  *tok;

	foreach(cell, tokens)
	{
		tok = lfirst(cell);
		if (token_is_member_check(tok))
		{
			if (is_member(roleid, tok->string + 1))
				return true;
		}
		else if (token_is_keyword(tok, "all"))
			return true;
		else if (token_has_regexp(tok))
		{
			if (regexec_auth_token(role, tok, 0, NULL) == REG_OKAY)
				return true;
		}
		else if (case_insensitive)
		{
			if (token_matches_insensitive(tok, role))
				return true;
		}
		else if (token_matches(tok, role))
			return true;
	}
	return false;
}

/* hba.c:988-1030, without the walsender's case: a Flight login is no replication */
static bool
check_db(const char *dbname, const char *role, Oid roleid, List *tokens)
{
	ListCell   *cell;
	AuthToken  *tok;

	foreach(cell, tokens)
	{
		tok = lfirst(cell);
		if (token_is_keyword(tok, "all"))
			return true;
		else if (token_is_keyword(tok, "sameuser"))
		{
			if (strcmp(dbname, role) == 0)
				return true;
		}
		else if (token_is_keyword(tok, "samegroup") ||
				 token_is_keyword(tok, "samerole"))
		{
			if (is_member(roleid, dbname))
				return true;
		}
		else if (token_is_keyword(tok, "replication"))
			continue;			/* never match this if not walsender */
		else if (token_has_regexp(tok))
		{
			if (regexec_auth_token(dbname, tok, 0, NULL) == REG_OKAY)
				return true;
		}
		else if (token_matches(tok, dbname))
			return true;
	}
	return false;
}

/* hba.c:1032-1050 */
static bool
ipv4eq(struct sockaddr_in *a, struct sockaddr_in *b)
{
	return (a->sin_addr.s_addr == b->sin_addr.s_addr);
}

static bool
ipv6eq(struct sockaddr_in6 *a, struct sockaddr_in6 *b)
{
	int			i;

	for (i = 0; i < 16; i++)
		if (a->sin6_addr.s6_addr[i] != b->sin6_addr.s6_addr[i])
			return false;

	return true;
}

/* hba.c:1053-1068 */
static bool
hostname_match(const char *pattern, const char *actual_hostname)
{
	if (pattern[0] == '.')		/* suffix match */
	{
		size_t		plen = strlen(pattern);
		size_t		hlen = strlen(actual_hostname);

		if (hlen < plen)
			return false;

		return (pg_strcasecmp(pattern, actual_hostname + (hlen - plen)) == 0);
	}
	else
		return (pg_strcasecmp(pattern, actual_hostname) == 0);
}

/* hba.c:1073-1160 */
static bool
check_hostname(Port *port, const char *hostname)
{
	struct addrinfo *gai_result,
			   *gai;
	int			ret;
	bool		found;

	/* Quick out if remote host name already known bad */
	if (port->remote_hostname_resolv < 0)
		return false;

	/* Lookup remote host name if not already done */
	if (!port->remote_hostname)
	{
		char		remote_hostname[NI_MAXHOST];

		ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
								 remote_hostname, sizeof(remote_hostname),
								 NULL, 0,
								 NI_NAMEREQD);
		if (ret != 0)
		{
			/* remember failure; don't complain in the postmaster log yet */
			port->remote_hostname_resolv = -2;
			port->remote_hostname_errcode = ret;
			return false;
		}

		port->remote_hostname = pstrdup(remote_hostname);
	}

	/* Now see if remote host name matches this pg_hba line */
	if (!hostname_match(hostname, port->remote_hostname))
		return false;

	/* If we already verified the forward lookup, we're done */
	if (port->remote_hostname_resolv == +1)
		return true;

	/* Lookup IP from host name and check against original IP */
	ret = getaddrinfo(port->remote_hostname, NULL, NULL, &gai_result);
	if (ret != 0)
	{
		/* remember failure; don't complain in the postmaster log yet */
		port->remote_hostname_resolv = -2;
		port->remote_hostname_errcode = ret;
		return false;
	}

	found = false;
	for (gai = gai_result; gai; gai = gai->ai_next)
	{
		if (gai->ai_addr->sa_family == port->raddr.addr.ss_family)
		{
			if (gai->ai_addr->sa_family == AF_INET)
			{
				if (ipv4eq((struct sockaddr_in *) gai->ai_addr,
						   (struct sockaddr_in *) &port->raddr.addr))
				{
					found = true;
					break;
				}
			}
			else if (gai->ai_addr->sa_family == AF_INET6)
			{
				if (ipv6eq((struct sockaddr_in6 *) gai->ai_addr,
						   (struct sockaddr_in6 *) &port->raddr.addr))
				{
					found = true;
					break;
				}
			}
		}
	}

	if (gai_result)
		freeaddrinfo(gai_result);

	if (!found)
		elog(DEBUG2, "pg_hba.conf host name \"%s\" rejected because address resolution did not return a match with IP address of client",
			 hostname);

	port->remote_hostname_resolv = found ? +1 : -1;

	return found;
}

/* hba.c:1164-1175 */
static bool
check_ip(SockAddr *raddr, struct sockaddr *addr, struct sockaddr *mask)
{
	if (raddr->addr.ss_family == addr->sa_family &&
		pg_range_sockaddr(&raddr->addr,
						  (struct sockaddr_storage *) addr,
						  (struct sockaddr_storage *) mask))
		return true;
	return false;
}

/* hba.c:1178-1202 */
static void
check_network_callback(struct sockaddr *addr, struct sockaddr *netmask,
					   void *cb_data)
{
	check_network_data *cn = (check_network_data *) cb_data;
	struct sockaddr_storage mask;

	/* Already found a match? */
	if (cn->result)
		return;

	if (cn->method == ipCmpSameHost)
	{
		/* Make an all-ones netmask of appropriate length for family */
		pg_sockaddr_cidr_mask(&mask, NULL, addr->sa_family);
		cn->result = check_ip(cn->raddr, addr, (struct sockaddr *) &mask);
	}
	else
	{
		/* Use the netmask of the interface itself */
		cn->result = check_ip(cn->raddr, addr, netmask);
	}
}

/* hba.c:1205-1223 */
static bool
check_same_host_or_net(SockAddr *raddr, IPCompareMethod method)
{
	check_network_data cn;

	cn.method = method;
	cn.raddr = raddr;
	cn.result = false;

	errno = 0;
	if (pg_foreach_ifaddr(check_network_callback, &cn) < 0)
	{
		ereport(LOG,
				(errmsg("error enumerating network interfaces: %m")));
		return false;
	}

	return cn.result;
}

/*
 * The first line that matches a login, into port->hba, or an implicit
 * reject: hba.c:2338-2438, check_hba, over the acceptor's lines.
 */
void
flight_check_hba(Port *port)
{
	Oid			roleid;
	ListCell   *line;
	HbaLine    *hba;

	/* Get the target role's OID.  Note we do not error out for bad role. */
	roleid = get_role_oid(port->user_name, true);

	foreach(line, flight_hba_lines)
	{
		hba = (HbaLine *) lfirst(line);

		/* Check connection type */
		if (hba->conntype == ctLocal)
		{
			if (port->raddr.addr.ss_family != AF_UNIX)
				continue;
		}
		else
		{
			if (port->raddr.addr.ss_family == AF_UNIX)
				continue;

			/* Check SSL state */
			if (port->ssl_in_use)
			{
				/* Connection is SSL, match both "host" and "hostssl" */
				if (hba->conntype == ctHostNoSSL)
					continue;
			}
			else
			{
				/* Connection is not SSL, match both "host" and "hostnossl" */
				if (hba->conntype == ctHostSSL)
					continue;
			}

			/* Check GSSAPI state: a Flight login has none */
			if (hba->conntype == ctHostGSS)
				continue;

			/* Check IP address */
			switch (hba->ip_cmp_method)
			{
				case ipCmpMask:
					if (hba->hostname)
					{
						if (!check_hostname(port,
											hba->hostname))
							continue;
					}
					else
					{
						if (!check_ip(&port->raddr,
									  (struct sockaddr *) &hba->addr,
									  (struct sockaddr *) &hba->mask))
							continue;
					}
					break;
				case ipCmpAll:
					break;
				case ipCmpSameHost:
				case ipCmpSameNet:
					if (!check_same_host_or_net(&port->raddr,
												hba->ip_cmp_method))
						continue;
					break;
				default:
					/* shouldn't get here, but deem it no-match if so */
					continue;
			}
		}						/* != ctLocal */

		/* Check database and role */
		if (!check_db(port->database_name, port->user_name, roleid,
					  hba->databases))
			continue;

		if (!check_role(port->user_name, roleid, hba->roles, false))
			continue;

		/* Found a record that matched! */
		port->hba = hba;
		return;
	}

	/* If no matching entry was found, then implicitly reject. */
	hba = palloc0_object(HbaLine);
	hba->auth_method = uaImplicitReject;
	port->hba = hba;
}
