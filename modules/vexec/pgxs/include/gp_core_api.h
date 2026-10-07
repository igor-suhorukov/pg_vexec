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
 * gp_core_api.h
 *	  The surface gp_core offers the other modules of the port.
 *
 * gp_core is preloaded first, and PostgreSQL opens libraries with RTLD_GLOBAL,
 * so the other modules resolve these at load time.  The rendezvous variable
 * carries a version, so that a module built against an older gp_core says so
 * instead of reading a struct that has moved.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_CORE_API_H
#define GP_CORE_API_H

#include "postgres.h"

#include "access/attnum.h"
#include "nodes/parsenodes.h"

/*
 * Bump the minor when something is added, the major when anything already
 * here changes meaning or moves.
 */
#define GP_CORE_API_VERSION_MAJOR	1
#define GP_CORE_API_VERSION_MINOR	15

struct Node;
struct List;
struct Plan;
struct PlanState;
struct PlannedStmt;
struct Query;
struct FileSet;
struct TableAmRoutine;
struct StringInfoData;

/*
 * What the rendezvous variable points at.  It is the first thing a module
 * sees of gp_core, so it never grows a field in the middle.
 */
typedef struct GpCoreApi
{
	int			version_major;
	int			version_minor;

	/*
	 * What this backend is: a connection the dispatcher opened executes, the
	 * coordinator dispatches, and anything else is a utility session -- which
	 * is what a psql opened on a segment is, here as in Cloudberry.  It is not
	 * always the node's role; see gp.role for that one.
	 */
	int			(*get_role) (void);

	/*
	 * How many primary segments to compute with.  Never 0: it is a divisor,
	 * not a flag -- ORCA's cost model asserts 0 < segments and divides by it,
	 * and Cloudberry's own getgpsegmentCount() answers 1 for a singleton for
	 * the same reason.  Ask is_single_node() for the question this is not.
	 */
	int			(*get_segment_count) (void);

	/* This node's content id: -1 on the coordinator, 0..n-1 on segments. */
	int			(*get_content_id) (void);

	/*
	 * Is this a single-node server -- the extension loaded, with no segments
	 * configured?  It is a flag of its own and not a segment count of zero,
	 * as Cloudberry's gp_internal_is_singlenode is.  Added in API 1.1.
	 */
	bool		(*is_single_node) (void);

	/*
	 * Which node of the cluster this is: the dbid of its line in the cluster
	 * configuration.  The coordinator keeps 1.  Added in API 1.2.
	 */
	int			(*get_dbid) (void);

	/*
	 * ORCA's Motions, carried out by gp_core (gp_motion.h says what each
	 * does).  Added in API 1.3.
	 */
	bool		(*motion_can_dispatch) (void);
	struct Plan *(*motion_make_gather) (struct Plan *fragment,
										struct List *targetlist,
										struct List *qual,
										int content, int slice, int nkeys,
										const AttrNumber *keys,
										const Oid *sortops,
										const Oid *collations,
										const bool *nullsfirst);
	bool		(*motion_is) (struct Plan *plan);
	int			(*motion_segment) (struct Plan *plan);
	void		(*motion_set_segment) (struct Plan *plan, int content);
	int			(*direct_dispatch_segment) (Oid relid, int nvalues,
											const Oid *types,
											const Datum *values,
											const bool *isnull);

	/* The Motions between segments.  Added in API 1.4. */
	struct Plan *(*motion_make_send) (int type, struct Plan *fragment,
									  struct List *targetlist,
									  struct List *qual, int content,
									  int slice, struct List *hashexprs,
									  struct List *hashfuncs);
	int			(*motion_type) (struct Plan *plan);
	int			(*motion_slice) (struct Plan *plan);
	void		(*motion_set_prepare) (struct Plan *plan, struct List *slices);
	struct Plan *(*motion_make_hash_filter) (struct Plan *child,
											 struct List *targetlist,
											 struct List *qual, int nkeys,
											 const AttrNumber *cols,
											 const Oid *hashfuncs,
											 int segment);
	struct Plan *(*motion_make_dml) (struct Plan *modify, int content,
									 int slice);
	struct Plan *(*split_make) (struct Plan *child, struct List *targetlist,
								struct List *deletecols,
								struct List *insertcols,
								AttrNumber actioncol);
	struct Plan *(*split_modify_make) (struct Plan *child, Index rti,
									   int natts, AttrNumber actioncol,
									   AttrNumber ctidcol);

	/* Since 1.5: the slice that receives a Motion between segments. */
	void		(*motion_set_parent) (struct Plan *plan, int parent);

	/*
	 * Since 1.6: the parameters a Motion's fragment is sent with, as lists of
	 * ids -- PARAM_EXEC ones the coordinator sets, and PARAM_EXTERN ones.
	 */
	void		(*motion_set_params) (struct Plan *plan, struct List *exec_params,
									  struct List *extern_params);

	/*
	 * Since 1.7: direct dispatch to several segments.  The segments a
	 * Gather or a write is sent to, as a list of content ids in the order
	 * Cloudberry names them (NIL: as motion_segment() says); and the
	 * segments conditions on relation "relid", range table entry "varno" of
	 * them, confine its rows to, as the planner's direct dispatch works them
	 * out (gp_scan.c) -- NIL where that is every segment.
	 */
	void		(*motion_set_segments) (struct Plan *plan, struct List *contents);
	struct List *(*motion_segments) (struct Plan *plan);
	struct List *(*direct_dispatch_contents) (Oid relid, struct Node *quals,
											  Index varno);

	/*
	 * Since 1.7: gp_segment_id, which ORCA's metadata has as the system
	 * column GP_SEGMENT_ID_ATTNO, and a plan as a call of this function, of
	 * the row of the relation it is read from (gp_segment.c); InvalidOid
	 * where gp_core's extension is not in the database.
	 */
	Oid			(*segment_of_function) (void);

	/*
	 * Since 1.8: a statement as gp_core has it planned, whichever planner
	 * plans it (GpPrepareQuery(), gp_segment.c), for ORCA to plan as
	 * PostgreSQL's planner does.
	 */
	void		(*prepare_query) (struct Query *parse);

	/*
	 * Since 1.9, nothing new here but what a Motion does: a Gather in a
	 * fragment a segment runs -- the slice it is in runs on one -- is
	 * received there as a Motion between segments is (gp_motion.c).
	 */

	/*
	 * Since 1.10: the FileSet a segment keeps the rows of a statement's CTEs
	 * in, where ORCA reads one in more than one slice -- Cloudberry's
	 * cross-slice ShareInputScan (gp_orca's compat/sharedscan.c).  Named
	 * after the segment's writer and the key of the Gather whose fragment
	 * "stmt" is, so that every process of the statement there, the writer
	 * and its readers, names it alike; false where "stmt" is no fragment.
	 * The Gather's end removes it, as it removes the rows its Motions sent
	 * (gp_internal.motion_drop()).
	 */
	bool		(*share_fileset) (struct PlannedStmt *stmt,
								  struct FileSet *fileset);

	/*
	 * Since 1.11: a partitioned table's split update, on the node
	 * split_modify_make() made -- the column of its rows that says which
	 * partition each DELETE's row is in, by its tableoid; each INSERT is
	 * routed into the table's partitions, as an INSERT into it is.
	 */
	void		(*split_modify_set_tableoid) (struct Plan *plan,
											  AttrNumber tableoidcol);

	/*
	 * Since 1.11: a MERGE ORCA plans on a cluster (gp_orca's merge.c) is
	 * written by the explicit write, as the planner's is -- over ORCA's plan
	 * of its join, which reads each target row's segment and ctid:
	 * row_identity_make() makes the ctid the explicit write knows the row by
	 * of the two, the other columns passed on; explicit_write() puts the
	 * explicit write in the MERGE's ModifyTable's place, refused as the
	 * planner's route refuses it (gp_explicit.c, gp_modify.c).
	 */
	struct Plan *(*row_identity_make) (struct Plan *child,
									   AttrNumber contentcol,
									   AttrNumber ctidcol);
	struct Plan *(*explicit_write) (struct PlannedStmt *stmt,
									struct Plan *modify);

	/*
	 * Since 1.11 too, nothing new here but what a Gather does: the
	 * coordinator's own slices it relays, run in its process, are ended
	 * with it rather than as each is relayed, so that a CTE the
	 * coordinator's slice produces is read in them; and the fragment of an
	 * UPDATE or DELETE whose ModifyTable has row marks keeps the
	 * statement's, which its re-check of a row changed meanwhile fetches
	 * the rows it was joined to by (gp_motion.c).
	 */

	/*
	 * Since 1.12: a partitioned table's PARTITION row in
	 * pg_stat_last_operation, of Cloudberry's own partition commands, which
	 * gp_sql carries out -- "ADD", "DROP", "EXCHANGE", "SPLIT", "TRUNCATE",
	 * "RENAME", "SET TEMPLATE" (gp_metatrack.c).
	 */
	void		(*metatrack_partition) (Oid relid, const char *subtype);

	/*
	 * Since 1.13: parallel retrieve cursors (gp_endpoint.c).  ORCA's plan
	 * of one, which endpoint_plan() gives its endpoints -- the segments its
	 * top slice runs on, the Gather above it taken off, or the coordinator
	 * -- and O26's RETRIEVE { ALL | count }, whose SELECT of the endpoint's
	 * columns retrieve_sql() writes, in a retrieve session.
	 */
	void		(*endpoint_plan) (struct PlannedStmt *stmt);
	char	   *(*retrieve_sql) (const char *endpoint, bool all, int64 count);

	/*
	 * Since 1.14: mark the entries of a database directory named by a number
	 * and "suffix" as a module's own, which pg_checksums passes over (O23)
	 * and a database copied or moved takes with it (gp_extmark.c).  Only
	 * while the postmaster loads the module.
	 */
	void		(*extension_mark_add) (const char *suffix);

	/*
	 * Since 1.14: a table access method whose tables' files are not their
	 * relfilenumber's, which the size functions then measure by its
	 * relation_size (gp_size.c).  Only while the postmaster loads it.
	 */
	void		(*size_from_am_register) (const struct TableAmRoutine *am);

	/*
	 * Since 1.15, for a vectorized executor that sends its batches across
	 * ORCA's Motions as frames, building its own nodes around them
	 * (pg_vector_executor.md §3.10): a Redistribute's hash functions, one
	 * for each of its hash expressions (custom_exprs), with which cdbhash
	 * hashes its rows, and whether one is a legacy function, which hashes
	 * the whole key as the legacy cdbhash does (gp_hash.h) -- NIL for a
	 * Motion of another kind; and how many sort keys a Gather merges its
	 * senders' streams by, 0 where it merges none.
	 */
	struct List *(*motion_hash_functions) (struct Plan *plan, bool *legacy);
	int			(*motion_merge_keys) (struct Plan *plan);

	/*
	 * Since 1.15: cdbhash, a row at a time -- for a key whose columns the
	 * caller cannot hash itself, a legacy one or one of a type it has no
	 * hash of its own for, and for its tests: the hash of "nkeys" columns
	 * with these hash functions over "nsegs" segments, made in the current
	 * memory context, and the segment of a row whose key columns are
	 * values[]/isnull[], in the key's order, as GpHashSegment() gives it.
	 */
	void	   *(*hash_make) (int nsegs, int nkeys, const Oid *hashfuncs);
	int			(*hash_segment) (void *hash, const Datum *values,
								 const bool *isnull);

	/*
	 * Since 1.15: the subtree a node of another module's reads no more,
	 * squelched as gp_core squelches a hash join's or a Limit's (gp_motion.c,
	 * "Stopping the senders in the middle of a plan"): its children shut
	 * down, and every Motion in it that receives a streaming slice ended, so
	 * that the senders stop waiting for it -- in place of gp_core's
	 * wrappers, which know PostgreSQL's nodes alone.  True where it did;
	 * false where the node may be run again -- below a nested loop's inner
	 * side, a Memoize, a Gather -- or no Motion below it streams, and then
	 * the caller reads what it must to the end itself.
	 */
	bool		(*squelch_subtree) (struct PlanState *ps);

	/*
	 * Since 1.15: EXPLAIN ANALYZE's figures of a module's own nodes, which
	 * the segments ran and the coordinator prints (gp_explain.c).  On a
	 * segment, "collect" is asked of each node of the fragment it explains
	 * that ran, and appends to "buf" what it keeps of the node's, or
	 * nothing; on the coordinator, "deposit" is handed each segment's bytes
	 * for the node of the same plan node id, before the plan is printed --
	 * as many times as that segment reported the node.  One module
	 * registers them; a second registration replaces the first.
	 */
	void		(*explain_register) (bool (*collect) (struct PlanState *ps,
													  struct StringInfoData *buf),
									 void (*deposit) (struct PlanState *ps,
													  int content,
													  const char *data,
													  int len));
} GpCoreApi;

/*
 * A parallel retrieve cursor, as O26 marks DECLARE ... PARALLEL RETRIEVE
 * CURSOR's DeclareCursorStmt: a bit of its options that no CURSOR_OPT_* flag
 * of PostgreSQL 19's is.  PostgreSQL hands the options to the planner and to
 * the portal (PerformCursorOpen(), ExplainOneUtility()) and reads only its
 * own bits of them, so the bit reaches the planner hooks and the portal
 * without a patch; nothing of PostgreSQL's reads it after, and it is not
 * cleared.  Cloudberry's CURSOR_OPT_PARALLEL_RETRIEVE is 0x0400, which is
 * PostgreSQL 19's CURSOR_OPT_CUSTOM_PLAN.  A PostgreSQL that takes this bit
 * for a flag of its own fails the assertion below, which names only the
 * flags there are; each major version's are to be checked against it.
 */
#define GP_CURSOR_OPT_PARALLEL_RETRIEVE	0x40000000

StaticAssertDecl(GP_CURSOR_OPT_PARALLEL_RETRIEVE > CURSOR_OPT_PARALLEL_OK &&
				 (GP_CURSOR_OPT_PARALLEL_RETRIEVE &
				  (CURSOR_OPT_BINARY | CURSOR_OPT_SCROLL |
				   CURSOR_OPT_NO_SCROLL | CURSOR_OPT_INSENSITIVE |
				   CURSOR_OPT_ASENSITIVE | CURSOR_OPT_HOLD |
				   CURSOR_OPT_FAST_PLAN | CURSOR_OPT_GENERIC_PLAN |
				   CURSOR_OPT_CUSTOM_PLAN | CURSOR_OPT_PARALLEL_OK)) == 0,
				 "the parallel retrieve cursor's bit is one of PostgreSQL's cursor options");

/*
 * gp_segment_id's attribute number in ORCA's metadata: Cloudberry's
 * GpSegmentIdAttributeNumber, below PostgreSQL 19's system attributes.  It
 * never reaches a plan -- ORCA's translator makes it a call of
 * segment_of_function() -- nor a catalog.
 */
#define GP_SEGMENT_ID_ATTNO		(-7)

/*
 * The values get_role() returns.  They are the port's spelling of Cloudberry's
 * Gp_role, and the compatibility header maps the old names onto them.
 */
typedef enum GpRole
{
	GP_ROLE_UTILITY = 0,		/* an ordinary local session */
	GP_ROLE_DISPATCH,			/* the coordinator, which dispatches */
	GP_ROLE_EXECUTE,			/* a segment process, which is dispatched to */
} GpRole;

/*
 * Look gp_core up.  Returns NULL when it is not loaded, so a caller that can
 * work without it may check; modules that cannot use CB_REQUIRE_CORE().
 */
extern const GpCoreApi *GpCoreApiLookup(void);

#endif							/* GP_CORE_API_H */
