/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * options.c
 *	  A table's options, in pg_class.reloptions, through the ProcessUtility
 *	  hook, as ch_table_engines' options.c keeps its own.
 *
 * PostgreSQL 19 has no table-AM option parser: CREATE TABLE and ALTER TABLE
 * ... SET validate a table's options as heap's, which refuse a name they do
 * not know.  So the hook takes the extension's options out of CREATE TABLE,
 * CREATE TABLE AS, SELECT INTO, CREATE MATERIALIZED VIEW and ALTER TABLE
 * SET/RESET, lets PostgreSQL do the rest with heap's alone, and writes ours
 * into pg_class.reloptions once the command has made the relation.  The
 * relcache parses reloptions without validation and ignores ours; this file
 * parses them.  A relation made by the running command has its options bound
 * in memory until they are written, for CREATE TABLE AS writes its rows
 * before its command ends.
 *
 *	location			external: a file, a directory or a glob, local or
 *						s3://; set again at any time, never reset
 *	storage				own: the files' root, a local directory or an s3://
 *						URI, the database's own directory where none (and
 *						vexec_parquet.storage empty); changed only while the
 *						table has no file
 *	codec, compression_level, row_group_rows, page_bytes
 *						own: the files written from now on -- page_bytes
 *						their data pages' size, which a fetch by TID reads
 *						one of for each column (scan.c)
 *
 * The table form of COPY ... TO of an external table runs as its query form
 * under WHERE false, so that pg_dump dumps no row of files PostgreSQL does
 * not own, as ch_table_engines' external tables do (its §2.17).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <sys/stat.h>

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "catalog/pg_database.h"
#include "catalog/pg_tablespace.h"
#include "catalog/toasting.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "common/relpath.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_node.h"
#include "storage/fd.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "vexec_parquet.h"

#define VPQ_AM_NAME "vexec_parquet"

static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static object_access_hook_type prev_object_access = NULL;

typedef struct OptionName
{
	const char *name;
	bool		external;		/* an external table's, else an own one's */
} OptionName;

static const OptionName option_names[] = {
	{"location", true},
	{"storage", false},
	{"codec", false},
	{"compression_level", false},
	{"row_group_rows", false},
	{"page_bytes", false},
};

/* Relations the running utility command made, and the options they get */
typedef struct BoundOptions
{
	Oid			relid;
	List	   *defs;
} BoundOptions;

static MemoryContext bound_cxt = NULL;
static List *bound = NIL;
static List *pending_defs = NIL;	/* the options of the CREATE being run */
static bool pending_create = false;
static int	utility_depth = 0;

static void vpq_ProcessUtility(PlannedStmt *pstmt, const char *queryString, bool readOnlyTree,
							   ProcessUtilityContext context, ParamListInfo params,
							   QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc);
static void vpq_object_access(ObjectAccessType access, Oid classId, Oid objectId, int subId, void *arg);
extern void vpq_relation_dropped(Oid relid);
extern void vpq_external_check(Relation rel);
extern bool vpq_storage_has_files(Relation rel);

void
vpq_options_init(void)
{
	prev_ProcessUtility = ProcessUtility_hook;
	ProcessUtility_hook = vpq_ProcessUtility;
	prev_object_access = object_access_hook;
	object_access_hook = vpq_object_access;
}

static bool
is_option_name(const char *name)
{
	for (int i = 0; i < lengthof(option_names); i++)
		if (strcmp(option_names[i].name, name) == 0)
			return true;
	return false;
}

bool
vpq_is_vpq_rel(Relation rel)
{
	return rel != NULL && rel->rd_tableam == vpq_routine();
}

static bool
am_is_ours(const char *am)
{
	return strcmp(am ? am : default_table_access_method, VPQ_AM_NAME) == 0;
}

static bool
amoid_is_ours(Oid amoid)
{
	char	   *name = OidIsValid(amoid) ? get_am_name(amoid) : NULL;

	return name != NULL && strcmp(name, VPQ_AM_NAME) == 0;
}

static void
check_location(const char *value, const char *option)
{
	if (value[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("option \"%s\" of a vexec_parquet table needs a value", option)));
	if (value[0] != '/' && strncmp(value, "file://", 7) != 0 && strncmp(value, "s3://", 5) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("option \"%s\" of a vexec_parquet table is an absolute path, a file:// URI or an s3:// URI: \"%s\"",
						option, value)));
}

static int
int_value(const char *option, const char *value, int lo, int hi)
{
	char	   *end;
	long		v;

	errno = 0;
	v = strtol(value, &end, 10);
	if (errno != 0 || *end != '\0' || end == value || v < lo || v > hi)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("option \"%s\" of a vexec_parquet table is an integer from %d to %d: \"%s\"",
						option, lo, hi, value)));
	return (int) v;
}

static void
check_codec_value(const char *value)
{
	static const char *const codecs[] = {"zstd", "lz4", "snappy", "gzip", "brotli", "none", NULL};

	for (int i = 0; codecs[i]; i++)
		if (strcmp(value, codecs[i]) == 0)
			return;
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unknown codec \"%s\" of a vexec_parquet table", value),
			 errhint("The codecs are zstd, lz4, snappy, gzip, brotli and none.")));
}

static const char *
def_string(DefElem *def)
{
	if (def->arg == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("option \"%s\" of a vexec_parquet table needs a value", def->defname)));
	return defGetString(def);
}

/*
 * The options of a CREATE, or of an ALTER ... SET (external: whether the
 * table is an external one; creating: a CREATE's, whose location makes it so)
 */
static void
validate_options(List *defs, bool creating, bool *external)
{
	ListCell   *lc;
	bool		has_location = false;
	bool		has_own = false;

	foreach(lc, defs)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (def->defaction == DEFELEM_DROP)
			continue;
		for (int i = 0; i < lengthof(option_names); i++)
			if (strcmp(option_names[i].name, def->defname) == 0)
			{
				if (option_names[i].external)
					has_location = true;
				else
					has_own = true;
			}
		/* the server's files and its S3 credentials, as file_fdw guards its filename */
		if (strcmp(def->defname, "location") == 0)
		{
			check_location(def_string(def), "location");
			if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set the location of a vexec_parquet table"),
						 errdetail("Only roles with privileges of the \"%s\" role may read files of the server.",
								   "pg_read_server_files")));
		}
		else if (strcmp(def->defname, "storage") == 0)
		{
			check_location(def_string(def), "storage");
			if (!has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
				ereport(ERROR,
						(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						 errmsg("permission denied to set the storage of a vexec_parquet table"),
						 errdetail("Only roles with privileges of the \"%s\" role may write files of the server.",
								   "pg_write_server_files")));
		}
		else if (strcmp(def->defname, "codec") == 0)
			check_codec_value(def_string(def));
		else if (strcmp(def->defname, "compression_level") == 0)
			(void) int_value("compression_level", def_string(def), -131072, 22);
		else if (strcmp(def->defname, "row_group_rows") == 0)
			(void) int_value("row_group_rows", def_string(def), 1024, 64 * 1024 * 1024);
		else if (strcmp(def->defname, "page_bytes") == 0)
			(void) int_value("page_bytes", def_string(def), 1024, 64 * 1024 * 1024);
	}
	if (creating)
		*external = has_location;
	if (*external && has_own)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a vexec_parquet table with a location reads its files and writes none"),
				 errdetail("storage, codec, compression_level, row_group_rows and page_bytes are options of a table of its own files, made without location.")));
	if (!*external && has_location)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a vexec_parquet table of its own files takes no location"),
				 errhint("Make a table that reads files with CREATE TABLE ... USING vexec_parquet WITH (location = '...').")));
}

static void
set_defaults(VpqOptions *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->codec = NULL;
	opts->compression_level = INT32_MIN;
	opts->row_group_rows = 0;
	opts->page_bytes = 0;
}

static void
apply_option(VpqOptions *opts, const char *name, const char *value)
{
	if (strcmp(name, "location") == 0)
		opts->location = pstrdup(value);
	else if (strcmp(name, "storage") == 0)
		opts->storage = pstrdup(value);
	else if (strcmp(name, "codec") == 0)
		opts->codec = pstrdup(value);
	else if (strcmp(name, "compression_level") == 0)
		opts->compression_level = atoi(value);
	else if (strcmp(name, "row_group_rows") == 0)
		opts->row_group_rows = atoi(value);
	else if (strcmp(name, "page_bytes") == 0)
		opts->page_bytes = atoi(value);
}

static List *
bound_defs(Oid relid, bool *found)
{
	ListCell   *lc;

	foreach(lc, bound)
	{
		BoundOptions *b = lfirst(lc);

		if (b->relid == relid)
		{
			*found = true;
			return b->defs;
		}
	}
	*found = false;
	return NIL;
}

/* The options of a relation of the access method */
void
vpq_relation_options(Relation rel, VpqOptions *opts)
{
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	bool		found;
	List	   *defs;
	ListCell   *lc;

	set_defaults(opts);
	defs = bound_defs(RelationGetRelid(rel), &found);
	if (!found)
	{
		tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(RelationGetRelid(rel)));
		if (!HeapTupleIsValid(tuple))
		{
			/* made by the running command, before its options were bound: the CREATE's own */
			if (pending_create)
				defs = pending_defs;
		}
		else
		{
			datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
			defs = isnull ? NIL : untransformRelOptions(datum);
			ReleaseSysCache(tuple);
		}
	}
	foreach(lc, defs)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (def->defnamespace == NULL && is_option_name(def->defname) && def->arg != NULL)
			apply_option(opts, def->defname, defGetString(def));
	}
}

static void
bind_options(Oid relid, List *defs)
{
	BoundOptions *b;
	MemoryContext old;

	if (bound_cxt == NULL)
		bound_cxt = AllocSetContextCreate(TopMemoryContext, "vexec_parquet bound options",
										  ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(bound_cxt);
	b = palloc(sizeof(BoundOptions));
	b->relid = relid;
	b->defs = copyObject(defs);
	bound = lappend(bound, b);
	MemoryContextSwitchTo(old);
}

static void
forget_bound(void)
{
	bound = NIL;
	if (bound_cxt != NULL)
		MemoryContextReset(bound_cxt);
}

static List *
take_options(List **options)
{
	List	   *ours = NIL;
	List	   *rest = NIL;
	ListCell   *lc;

	foreach(lc, *options)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (def->defnamespace == NULL && is_option_name(def->defname))
			ours = lappend(ours, def);
		else
			rest = lappend(rest, def);
	}
	*options = rest;
	return ours;
}

static List *
stringify_defs(List *defs)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, defs)
	{
		DefElem    *def = lfirst_node(DefElem, lc);

		if (def->arg == NULL || IsA(def->arg, String))
			result = lappend(result, def);
		else
			result = lappend(result, makeDefElem(def->defname, (Node *) makeString(defGetString(def)),
												 def->location));
	}
	return result;
}

static void
store_reloptions(Oid relid, List *defs, bool reset, bool keep_others)
{
	Relation	classrel;
	HeapTuple	tuple;
	HeapTuple	newtuple;
	Datum		old;
	Datum		newopts;
	bool		isnull;
	Datum		repl_val[Natts_pg_class];
	bool		repl_null[Natts_pg_class];
	bool		repl_repl[Natts_pg_class];

	classrel = table_open(RelationRelationId, RowExclusiveLock);
	tuple = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	old = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
	if (isnull || !keep_others)
		old = (Datum) 0;
	{
		/* toast.* validated and left out: the relation's own options alone, as heap's are kept */
		const char *const validnsps[] = HEAP_RELOPT_NAMESPACES;

		newopts = transformRelOptions(old, defs, NULL, validnsps, false, reset);
	}

	memset(repl_val, 0, sizeof(repl_val));
	memset(repl_null, false, sizeof(repl_null));
	memset(repl_repl, false, sizeof(repl_repl));
	if (newopts != (Datum) 0)
		repl_val[Anum_pg_class_reloptions - 1] = newopts;
	else
		repl_null[Anum_pg_class_reloptions - 1] = true;
	repl_repl[Anum_pg_class_reloptions - 1] = true;
	newtuple = heap_modify_tuple(tuple, RelationGetDescr(classrel), repl_val, repl_null, repl_repl);
	CatalogTupleUpdate(classrel, &newtuple->t_self, newtuple);
	heap_freetuple(newtuple);
	heap_freetuple(tuple);
	table_close(classrel, RowExclusiveLock);
}

/* Our options out of a relation that left the access method */
static void
strip_options(Oid relid)
{
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	List	   *keep = NIL;
	ListCell   *lc;
	bool		had = false;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		return;
	datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
	if (!isnull)
		foreach(lc, untransformRelOptions(datum))
		{
			DefElem    *def = lfirst_node(DefElem, lc);

			if (def->defnamespace == NULL && is_option_name(def->defname))
				had = true;
			else
				keep = lappend(keep, def);
		}
	ReleaseSysCache(tuple);
	if (had)
		store_reloptions(relid, keep, false, false);
}

static PlannedStmt *
copy_with_utility(PlannedStmt *pstmt, Node *stmt)
{
	PlannedStmt *copy = makeNode(PlannedStmt);

	memcpy(copy, pstmt, sizeof(PlannedStmt));
	copy->utilityStmt = stmt;
	return copy;
}

static void
run_utility(PlannedStmt *pstmt, const char *queryString, bool readOnlyTree,
			ProcessUtilityContext context, ParamListInfo params,
			QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc)
{
	if (prev_ProcessUtility)
		prev_ProcessUtility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
	else
		standard_ProcessUtility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
}

static void
store_bound(void)
{
	ListCell   *lc;

	if (bound == NIL)
		return;
	CommandCounterIncrement();
	foreach(lc, bound)
	{
		BoundOptions *b = lfirst(lc);

		if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(b->relid)))
			store_reloptions(b->relid, b->defs, false, true);
	}
	CommandCounterIncrement();
}

/*
 * ALTER TABLE ... SET/RESET (...) on a vexec_parquet table: heap's options
 * validated as heap's alone, ours here
 */
static void
alter_table_options(Oid relid, AlterTableCmd *cmd)
{
	Relation	rel;
	List	   *defs = (List *) cmd->def;
	List	   *ours;
	List	   *heaps;
	ListCell   *lc;
	LOCKMODE	lockmode;
	HeapTuple	tuple;
	Datum		datum;
	bool		isnull;
	Datum		merged;
	List	   *heap_only = NIL;
	VpqOptions	opts;
	bool		external;
	bool		storage_changes = false;
	bool		location_set = false;

	heaps = list_copy(defs);
	ours = take_options(&heaps);
	lockmode = ours != NIL ? AccessExclusiveLock :
		Max(AlterTableGetRelOptionsLockLevel(heaps), ShareUpdateExclusiveLock);
	rel = table_open(relid, lockmode);
	vpq_relation_options(rel, &opts);
	external = opts.location != NULL;
	if (cmd->subtype == AT_ResetRelOptions)
	{
		foreach(lc, ours)
		{
			DefElem    *def = lfirst_node(DefElem, lc);

			if (strcmp(def->defname, "location") == 0)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("the location of a vexec_parquet table is set again, never reset")));
			if (strcmp(def->defname, "storage") == 0)
				storage_changes = true;
			def->defaction = DEFELEM_DROP;
		}
	}
	else
	{
		validate_options(ours, false, &external);
		foreach(lc, ours)
		{
			if (strcmp(lfirst_node(DefElem, lc)->defname, "storage") == 0)
				storage_changes = true;
			if (strcmp(lfirst_node(DefElem, lc)->defname, "location") == 0)
				location_set = true;
		}
	}
	if (storage_changes && vpq_storage_has_files(rel))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("cannot change the storage of \"%s\" while it has files", RelationGetRelationName(rel)),
				 errhint("TRUNCATE the table first, or make a new table with CREATE TABLE AS.")));

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);
	if (!isnull)
		foreach(lc, untransformRelOptions(datum))
		{
			DefElem    *def = lfirst_node(DefElem, lc);

			if (!(def->defnamespace == NULL && is_option_name(def->defname)))
				heap_only = lappend(heap_only, def);
		}
	ReleaseSysCache(tuple);
	{
		/* toast.* as ALTER TABLE takes them: kept, and unused (the table has no TOAST table) */
		const char *const validnsps[] = HEAP_RELOPT_NAMESPACES;

		merged = transformRelOptions(heap_only == NIL ? (Datum) 0 :
									 transformRelOptions((Datum) 0, heap_only, NULL, validnsps, false, false),
									 heaps, NULL, validnsps, false, cmd->subtype == AT_ResetRelOptions);
		(void) heap_reloptions(RELKIND_RELATION, merged, true);
		store_reloptions(relid, stringify_defs(defs), cmd->subtype == AT_ResetRelOptions, true);
	}
	CacheInvalidateRelcache(rel);
	/* a new location's files checked as CREATE TABLE checks a table's first */
	if (location_set)
	{
		CommandCounterIncrement();
		vpq_external_check(rel);
	}
	table_close(rel, NoLock);
}

/*
 * A database's directory, in any tablespace, holding vexec_parquet's
 * directory: the database has tables of their own files, whose files a
 * template's copy would not take with it (CREATE DATABASE's WAL_LOG copies
 * relations alone; tables kept under another root are named by the
 * database's OID)
 */
static bool
database_has_files(Oid dboid)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;

	if (!OidIsValid(dboid))
		return false;
	rel = table_open(TableSpaceRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while (!found && (tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_tablespace spc = (Form_pg_tablespace) GETSTRUCT(tuple);
		char	   *dir;
		char	   *ours;
		struct stat st;

		/* pg_global holds no database's directory */
		if (spc->oid == GLOBALTABLESPACE_OID)
			continue;
		dir = GetDatabasePath(dboid, spc->oid);
		ours = psprintf("%s/%s/vexec_parquet", DataDir, dir);
		if (stat(ours, &st) == 0 && S_ISDIR(st.st_mode))
			found = true;
		pfree(ours);
		pfree(dir);
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);
	return found;
}

static void
check_database_copy(Oid dboid, const char *what)
{
	if (database_has_files(dboid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot %s database \"%s\": it holds vexec_parquet tables of their own files",
						what, get_database_name(dboid)),
				 errdetail("Their files are not relations, which the command copies.")));
}

static ResTarget *
column_target(const char *name)
{
	ResTarget  *target = makeNode(ResTarget);
	ColumnRef  *ref = makeNode(ColumnRef);

	ref->fields = list_make1(makeString(pstrdup(name)));
	ref->location = -1;
	target->val = (Node *) ref;
	target->location = -1;
	return target;
}

/*
 * COPY t TO of an external table: its query form over the same columns --
 * those it lists, or those COPY would name, neither dropped nor generated --
 * under WHERE false, the statement's options kept
 */
static Node *
external_copy_to(CopyStmt *stmt, Oid relid)
{
	Relation	rel = RelationIdGetRelation(relid);
	CopyStmt   *copy;
	SelectStmt *select;
	RangeVar   *from;
	A_Const    *no;
	ListCell   *lc;
	VpqOptions	opts;
	char	   *relname;

	if (!RelationIsValid(rel))
		return NULL;
	if (!vpq_is_vpq_rel(rel))
	{
		RelationClose(rel);
		return NULL;
	}
	vpq_relation_options(rel, &opts);
	if (opts.location == NULL)
	{
		RelationClose(rel);
		return NULL;
	}
	select = makeNode(SelectStmt);
	if (stmt->attlist != NIL)
		foreach(lc, stmt->attlist)
			select->targetList = lappend(select->targetList, column_target(strVal(lfirst(lc))));
	else
	{
		TupleDesc	desc = RelationGetDescr(rel);

		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (att->attisdropped || att->attgenerated)
				continue;
			select->targetList = lappend(select->targetList, column_target(NameStr(att->attname)));
		}
	}
	RelationClose(rel);

	from = copyObject(stmt->relation);
	from->inh = false;
	select->fromClause = list_make1(from);
	no = makeNode(A_Const);
	no->val.boolval.type = T_Boolean;
	no->val.boolval.boolval = false;
	no->location = -1;
	select->whereClause = (Node *) no;

	copy = copyObject(stmt);
	copy->relation = NULL;
	copy->attlist = NIL;
	copy->query = (Node *) select;

	relname = stmt->relation->schemaname ?
		psprintf("%s.%s", quote_identifier(stmt->relation->schemaname),
				 quote_identifier(stmt->relation->relname)) :
		pstrdup(quote_identifier(stmt->relation->relname));
	ereport(NOTICE,
			(errmsg("COPY of %s returns no rows: its rows are those of the files at its location", relname),
			 errhint("COPY (SELECT * FROM %s) TO ... reads them.", relname)));
	return (Node *) copy;
}

static void
vpq_ProcessUtility(PlannedStmt *pstmt, const char *queryString, bool readOnlyTree,
				   ProcessUtilityContext context, ParamListInfo params,
				   QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	bool		outermost = (utility_depth == 0);
	List	   *after_strip = NIL;
	List	   *external_checks = NIL;

	if (IsA(parsetree, CreateStmt))
	{
		CreateStmt *stmt = (CreateStmt *) parsetree;

		if (am_is_ours(stmt->accessMethod) || (stmt->partbound != NULL && stmt->accessMethod == NULL))
		{
			CreateStmt *copy = copyObject(stmt);
			List	   *ours = take_options(&copy->options);
			bool		external = false;

			/* a partition of a partitioned table of ours is ours by its parent's AM */
			if (ours != NIL || am_is_ours(stmt->accessMethod))
			{
				validate_options(ours, true, &external);
				if (external && (stmt->partspec != NULL || stmt->partbound != NULL))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("a vexec_parquet table that reads files cannot be partitioned or a partition")));
				if (ours != NIL && stmt->partspec != NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("a partitioned vexec_parquet table takes no options"),
							 errhint("Give them to each partition.")));
				pending_defs = stringify_defs(ours);
				pending_create = true;
				pstmt = copy_with_utility(pstmt, (Node *) copy);
				readOnlyTree = false;
			}
		}
	}
	else if (IsA(parsetree, CreateTableAsStmt))
	{
		CreateTableAsStmt *stmt = (CreateTableAsStmt *) parsetree;

		if (stmt->into && am_is_ours(stmt->into->accessMethod))
		{
			CreateTableAsStmt *copy = copyObject(stmt);
			List	   *ours = take_options(&copy->into->options);
			bool		external = false;

			validate_options(ours, true, &external);
			if (external && !stmt->into->skipData)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("a vexec_parquet table that reads files takes no rows"),
						 errhint("Use WITH NO DATA.")));
			pending_defs = stringify_defs(ours);
			pending_create = true;
			pstmt = copy_with_utility(pstmt, (Node *) copy);
			readOnlyTree = false;
		}
	}
	else if (IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *stmt = (AlterTableStmt *) parsetree;
		Oid			relid = RangeVarGetRelid(stmt->relation, NoLock, true);

		if (OidIsValid(relid) && stmt->objtype == OBJECT_TABLE)
		{
			Relation	rel = RelationIdGetRelation(relid);
			bool		ours = RelationIsValid(rel) && vpq_is_vpq_rel(rel);
			bool		external = false;
			List	   *rest = NIL;
			ListCell   *lc;

			if (ours)
			{
				VpqOptions	opts;

				vpq_relation_options(rel, &opts);
				external = opts.location != NULL;
			}
			if (RelationIsValid(rel))
				RelationClose(rel);
			if (external)
				external_checks = lappend_oid(external_checks, relid);
			foreach(lc, stmt->cmds)
			{
				AlterTableCmd *cmd = lfirst_node(AlterTableCmd, lc);

				if (ours && (cmd->subtype == AT_SetRelOptions || cmd->subtype == AT_ResetRelOptions))
				{
					alter_table_options(relid, cmd);
					continue;
				}
				if (cmd->subtype == AT_AttachPartition && IsA(cmd->def, PartitionCmd))
				{
					Oid			part = RangeVarGetRelid(((PartitionCmd *) cmd->def)->name, NoLock, true);

					if (OidIsValid(part) && amoid_is_ours(get_rel_relam(part)))
					{
						Relation	p = RelationIdGetRelation(part);
						VpqOptions	popts;

						if (RelationIsValid(p))
						{
							vpq_relation_options(p, &popts);
							RelationClose(p);
							if (popts.location != NULL)
								ereport(ERROR,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("a vexec_parquet table that reads files cannot be a partition")));
						}
					}
				}
				if (cmd->subtype == AT_SetAccessMethod)
				{
					bool		to_ours = am_is_ours(cmd->name);

					if (external)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("cannot change the access method of a vexec_parquet table that reads files")));
					if (ours && !to_ours)
						after_strip = lappend_oid(after_strip, relid);
					else if (!ours && to_ours)
					{
						/* the rewrite's transient table and the table itself: a table of its own files */
						pending_defs = NIL;
						pending_create = true;
					}
				}
				rest = lappend(rest, cmd);
			}
			if (list_length(rest) != list_length(stmt->cmds))
			{
				AlterTableStmt *copy;

				if (rest == NIL)
				{
					CommandCounterIncrement();
					return;
				}
				copy = copyObject(stmt);
				copy->cmds = copyObject(rest);
				pstmt = copy_with_utility(pstmt, (Node *) copy);
				readOnlyTree = false;
			}
		}
	}
	else if (IsA(parsetree, IndexStmt))
	{
		IndexStmt  *stmt = (IndexStmt *) parsetree;
		Oid			relid = RangeVarGetRelid(stmt->relation, NoLock, true);

		if (stmt->concurrent && OidIsValid(relid) && amoid_is_ours(get_rel_relam(relid)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("CREATE INDEX CONCURRENTLY is not supported on a vexec_parquet table"),
					 errhint("Use CREATE INDEX.")));
	}
	else if (IsA(parsetree, ReindexStmt))
	{
		ReindexStmt *stmt = (ReindexStmt *) parsetree;
		ListCell   *lc;
		bool		concurrently = false;

		foreach(lc, stmt->params)
		{
			DefElem    *opt = lfirst_node(DefElem, lc);

			if (strcmp(opt->defname, "concurrently") == 0 && defGetBoolean(opt))
				concurrently = true;
		}
		if (concurrently && stmt->relation != NULL)
		{
			Oid			relid = RangeVarGetRelid(stmt->relation, NoLock, true);

			if (OidIsValid(relid) && stmt->kind == REINDEX_OBJECT_INDEX)
				relid = IndexGetRelation(relid, true);
			if (OidIsValid(relid) && amoid_is_ours(get_rel_relam(relid)))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("REINDEX CONCURRENTLY is not supported on a vexec_parquet table"),
						 errhint("Use REINDEX.")));
		}
	}
	else if (IsA(parsetree, CreatedbStmt))
	{
		ListCell   *lc;

		foreach(lc, ((CreatedbStmt *) parsetree)->options)
			if (strcmp(((DefElem *) lfirst(lc))->defname, "template") == 0)
				check_database_copy(get_database_oid(defGetString(lfirst(lc)), true), "copy");
	}
	else if (IsA(parsetree, AlterDatabaseStmt))
	{
		ListCell   *lc;

		foreach(lc, ((AlterDatabaseStmt *) parsetree)->options)
			if (strcmp(((DefElem *) lfirst(lc))->defname, "tablespace") == 0)
				check_database_copy(get_database_oid(((AlterDatabaseStmt *) parsetree)->dbname, true),
									"move");
	}
	else if (IsA(parsetree, CopyStmt))
	{
		CopyStmt   *stmt = (CopyStmt *) parsetree;

		if (!stmt->is_from && stmt->relation != NULL && stmt->query == NULL)
		{
			Oid			relid = RangeVarGetRelid(stmt->relation, NoLock, true);
			Node	   *query_form = OidIsValid(relid) ? external_copy_to(stmt, relid) : NULL;

			if (query_form)
			{
				pstmt = copy_with_utility(pstmt, query_form);
				readOnlyTree = false;
			}
		}
	}

	utility_depth++;
	PG_TRY();
	{
		run_utility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc);
	}
	PG_CATCH();
	{
		utility_depth--;
		if (outermost)
		{
			pending_defs = NIL;
			pending_create = false;
			forget_bound();
		}
		PG_RE_THROW();
	}
	PG_END_TRY();
	utility_depth--;
	if (outermost)
	{
		pending_defs = NIL;
		pending_create = false;
	}

	if (external_checks != NIL)
	{
		ListCell   *lc;

		/* its columns and location as the ALTER left them, against its files */
		CommandCounterIncrement();
		foreach(lc, external_checks)
		{
			Relation	r = RelationIdGetRelation(lfirst_oid(lc));

			if (!RelationIsValid(r))
				continue;
			PG_TRY();
			{
				if (vpq_is_vpq_rel(r))
					vpq_external_check(r);
			}
			PG_FINALLY();
			{
				RelationClose(r);
			}
			PG_END_TRY();
		}
	}

	/* a utility statement's rows -- COPY's, which no ExecutorFinish ends -- become files as it ends */
	if (IsTransactionState())
		vpq_buffer_flush_all();

	if (outermost && IsTransactionState())
	{
		ListCell   *lc;

		store_bound();
		forget_bound();
		foreach(lc, after_strip)
		{
			strip_options(lfirst_oid(lc));
			/* make_new_heap() gives the new heap a TOAST table only where the old had one, and ours have none */
			CommandCounterIncrement();
			AlterTableCreateToastTable(lfirst_oid(lc), (Datum) 0, AccessExclusiveLock);
		}
	}
}

/*
 * OAT_POST_CREATE of a relation the running CREATE makes with the access
 * method binds it to the CREATE's options; an external table's columns are
 * checked against its files there.  OAT_DROP drops an own table's storage.
 */
static void
vpq_object_access(ObjectAccessType access, Oid classId, Oid objectId, int subId, void *arg)
{
	if (prev_object_access)
		prev_object_access(access, classId, objectId, subId, arg);

	if (access == OAT_POST_CREATE && classId == RelationRelationId && subId == 0 && pending_create)
	{
		Relation	rel = RelationIdGetRelation(objectId);
		bool		found;

		(void) bound_defs(objectId, &found);
		if (RelationIsValid(rel))
		{
			if (!found && (rel->rd_rel->relkind == RELKIND_RELATION ||
						   rel->rd_rel->relkind == RELKIND_MATVIEW) && vpq_is_vpq_rel(rel))
			{
				VpqOptions	opts;

				bind_options(objectId, pending_defs);
				vpq_relation_options(rel, &opts);
				PG_TRY();
				{
					if (opts.location != NULL)
						vpq_external_check(rel);
					else
						(void) vpq_table(rel);
				}
				PG_CATCH();
				{
					RelationClose(rel);
					PG_RE_THROW();
				}
				PG_END_TRY();
			}
			RelationClose(rel);
		}
	}
	else if (access == OAT_DROP && classId == RelationRelationId && subId == 0)
		vpq_relation_dropped(objectId);
}
