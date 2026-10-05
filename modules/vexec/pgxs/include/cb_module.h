/*-------------------------------------------------------------------------
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * cb_module.h
 *	  What every module of the PostgreSQL 19 port shares.
 *
 * Two things live here: how a module says that it may only be preloaded, and
 * how a module finds gp_core.  Both exist so that a server which was started
 * without the right "shared_preload_libraries" fails with an error a person
 * can act on, instead of running with half the port initialised.
 *
 *-------------------------------------------------------------------------
 */
#ifndef CB_MODULE_H
#define CB_MODULE_H

#include "miscadmin.h"
#include "utils/guc.h"

/*
 * The rendezvous variable gp_core publishes.  Modules find each other through
 * it rather than by linking, so that a module listed before gp_core reports
 * that instead of failing to resolve a symbol at load time.
 */
#define CB_CORE_RENDEZVOUS	"Cloudberry/gp_core"

/*
 * Set by gp_security while it is preloaded.  O26 carries a role's PROFILE and
 * ACCOUNT LOCK to its ALTER USER for gp_security's ProcessUtility hook to take
 * out, and asks this first, so that without gp_security the statement is
 * refused with a reason rather than by ALTER ROLE as an option it does not
 * know.
 */
#define CB_SECURITY_RENDEZVOUS	"Cloudberry/gp_security"

/*
 * Set by gp_resource while it is preloaded, for the same reason: O26 carries
 * a role's RESOURCE QUEUE and RESOURCE GROUP to its statement for
 * gp_resource's hook to take out.
 */
#define CB_RESOURCE_RENDEZVOUS	"Cloudberry/gp_resource"

/*
 * Cloudberry's query_info_collect_hook, which gp_stats_collector sets while
 * it is preloaded and the port's modules call where Cloudberry's core calls
 * it (gp_query_info.h).
 */
#define CB_QUERY_INFO_RENDEZVOUS	"Cloudberry/query_info_collect_hook"

/*
 * Set by gp_sql while it is preloaded, and so O26 with it in every backend,
 * a background worker's too: gp_matview then writes a dynamic table's job in
 * Cloudberry's words, REFRESH DYNAMIC TABLE, which gp_task's worker runs
 * through O26 as Cloudberry's scheduler runs it.
 */
#define CB_SQL_RENDEZVOUS	"Cloudberry/gp_sql"

/*
 * Refuse to load outside shared_preload_libraries.
 *
 * A module that registers a custom WAL resource manager, requests shared
 * memory or installs a hook the whole cluster depends on can only do that
 * during preload.  Loading it later -- through LOAD, through CREATE EXTENSION
 * or by calling a function whose library it is -- would leave it half set up,
 * so it is an error.  This is the check Citus makes for the same reason.
 */
#define CB_REQUIRE_PRELOAD(modname) \
	do { \
		if (!process_shared_preload_libraries_in_progress) \
			ereport(ERROR, \
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), \
					 errmsg("%s can only be loaded through \"shared_preload_libraries\"", \
							(modname)), \
					 errhint("Add \"%s\" to \"shared_preload_libraries\" in postgresql.conf and restart the server.", \
							 (modname)))); \
	} while (0)

/*
 * Require gp_core, which must already have been preloaded.  Every module but
 * gp_core itself calls this first thing in _PG_init.
 */
#define CB_REQUIRE_CORE(modname) \
	do { \
		void	  **_cb_rv = find_rendezvous_variable(CB_CORE_RENDEZVOUS); \
		\
		if (*_cb_rv == NULL) \
			ereport(ERROR, \
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), \
					 errmsg("%s requires \"gp_core\" to be loaded first", \
							(modname)), \
					 errhint("List \"gp_core\" before \"%s\" in \"shared_preload_libraries\".", \
							 (modname)))); \
	} while (0)

#endif							/* CB_MODULE_H */
