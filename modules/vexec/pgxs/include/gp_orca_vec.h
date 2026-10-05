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
 * gp_orca_vec.h
 *	  gp_orca's API for a vectorized executor: how one plans vector nodes
 *	  into ORCA's plans without changing ORCA (pg_vector_executor.md §3.3.4).
 *
 * ORCA's plans reach none of PostgreSQL's planner hooks: gp_orca's
 * planner_hook returns them without standard_planner.  An engine that runs
 * plans in its own nodes -- vexec -- registers a routine here, and gp_orca
 * calls it at three points of each statement ORCA plans:
 *
 *	begin_statement		before ORCA is asked: whether the statement gets
 *						vector nodes at all, the engine's gates; the state
 *						it returns is handed to the other two, and NULL
 *						means none
 *	build_node			after the translator has built the plan, and before
 *						its Motions are checked and its slice table made:
 *						for each node, children first, the engine's node in
 *						its place or NULL to keep it.  So every later step
 *						-- the Motions' check, the slice table, the port's
 *						passes over the finished plan (lockrows.c, merge.c,
 *						parallel.c), the dependency walk -- sees the final
 *						tree, and a node replaced keeps its plan node id,
 *						its costs and its parameters
 *	end_statement		once ORCA's plan is made: the engine's check of it,
 *						and what it records in PlannedStmt.extension_state
 *
 * A node built so is the engine's CustomScan, found by name where the
 * plan is read back, a segment's fragment among them.  Without a
 * registration gp_orca calls nothing, and its plans are as they were.
 *
 * Header only, and a type and inline functions alone, as vexec_source.h
 * is, so that neither module links the other and there is no order
 * between them in "shared_preload_libraries": whichever comes first makes
 * the registry the rendezvous variable points at.  vexec builds by PGXS
 * on a server without the port too, against a copy of this header kept
 * equal to it (its checks/headers.sh).
 *
 * Versions: the major version is in the rendezvous name, and a routine's
 * "size" says how much of it the engine was built with; a member past it
 * reads as absent.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_ORCA_VEC_H
#define GP_ORCA_VEC_H

#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "utils/memutils.h"

#define GP_ORCA_VEC_RENDEZVOUS	"Cloudberry/gp_orca_vec_v1"	/* the major
															 * version is in
															 * the name */
#define GP_ORCA_VEC_MINOR		0
#define GP_ORCA_VEC_MAGIC		0x47564331	/* "GVC1" */

struct ExplainState;

typedef struct GpOrcaVecRoutine
{
	Size		size;			/* sizeof as the engine was built */
	int			minor;			/* GP_ORCA_VEC_MINOR it was built with */
	const char *name;			/* for errors */

	/*
	 * Before ORCA is asked to plan a statement, in the planner_hook's memory
	 * context; es is EXPLAIN's state when an EXPLAIN plans it.  NULL: no
	 * vector node for this statement.
	 */
	void	   *(*begin_statement) (Query *parse, int cursorOptions,
									struct ExplainState *es);

	/*
	 * A node of the translated plan, in executor form, its children already
	 * offered; rtable is the plan's range table, which a scan's scanrelid
	 * indexes.  The engine's node in its place, or NULL.  It runs inside
	 * ORCA's translation: an error it raises makes ORCA decline the
	 * statement, which the planner then plans.
	 */
	Plan	   *(*build_node) (void *state, Plan *plan, List *rtable);

	/* ORCA's plan is made: the engine's check, and what it records. */
	void		(*end_statement) (void *state, PlannedStmt *stmt);
} GpOrcaVecRoutine;

typedef struct GpOrcaVecRegistry
{
	uint32		magic;
	const GpOrcaVecRoutine *routine;
} GpOrcaVecRegistry;

/* The registry, made if no module has made it yet. */
static inline GpOrcaVecRegistry *
gp_orca_vec_registry(void)
{
	GpOrcaVecRegistry **rv;

	rv = (GpOrcaVecRegistry **) find_rendezvous_variable(GP_ORCA_VEC_RENDEZVOUS);
	if (*rv == NULL)
	{
		GpOrcaVecRegistry *reg;

		reg = (GpOrcaVecRegistry *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(GpOrcaVecRegistry));
		reg->magic = GP_ORCA_VEC_MAGIC;
		*rv = reg;
	}
	else if ((*rv)->magic != GP_ORCA_VEC_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("the rendezvous variable \"%s\" does not hold gp_orca's vector registry",
						GP_ORCA_VEC_RENDEZVOUS)));
	return *rv;
}

/*
 * Register the engine, from its _PG_init.  Only while the postmaster
 * preloads libraries, so that every backend has the same; one engine.
 */
static inline void
gp_orca_vec_register(const GpOrcaVecRoutine *routine)
{
	GpOrcaVecRegistry *reg;

	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a vector engine can register with gp_orca only while \"shared_preload_libraries\" are loaded")));
	reg = gp_orca_vec_registry();
	if (reg->routine != NULL && reg->routine != routine)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("a vector engine is already registered with gp_orca: \"%s\"",
						reg->routine->name ? reg->routine->name : "?")));
	reg->routine = routine;
}

/* The registered engine, or NULL. */
static inline const GpOrcaVecRoutine *
gp_orca_vec_find(void)
{
	GpOrcaVecRegistry **rv;

	rv = (GpOrcaVecRegistry **) find_rendezvous_variable(GP_ORCA_VEC_RENDEZVOUS);
	if (*rv == NULL || (*rv)->magic != GP_ORCA_VEC_MAGIC)
		return NULL;
	return (*rv)->routine;
}

/* Whether a routine has a member: it was built with a struct that long. */
#define GP_ORCA_VEC_HAS(routine, member) \
	((routine)->size >= offsetof(GpOrcaVecRoutine, member) + sizeof((routine)->member) && \
	 (routine)->member != NULL)

#endif							/* GP_ORCA_VEC_H */
