/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * catalog.c
 *	  Flight SQL's catalog commands, server information and type information
 *	  (arrow/format/FlightSql.proto; pg_vector_executor.md §3.15).
 *
 * The catalog commands are SQL over pg_catalog, with the caller's
 * privileges, run as any statement is, their results going through vexec's
 * receiver with the field names, nullability and integer widths
 * FlightSql.proto gives each result.  A catalog is a database: a session
 * sees its own.  A schema is a namespace, and a table any relation a client
 * can select from: a table, a partitioned table, a view, a materialized view
 * or a foreign table, the system's own named as such.  Name patterns are
 * LIKE's.
 *
 * Server information and type information are no statement's result: the
 * endpoint makes their arrays itself (arrays.c), and the egress API writes
 * them.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "common/keywords.h"
#include "executor/tuptable.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/varlena.h"

#include "arrays.h"
#include "session.h"

/* ---------------------------------------------------------------------
 * The commands' SQL
 * ---------------------------------------------------------------------
 */

#define TABLE_TYPE \
	"CASE WHEN n.nspname IN ('pg_catalog', 'information_schema') AND c.relkind IN ('r', 'p') " \
	"THEN 'SYSTEM TABLE' " \
	"WHEN n.nspname IN ('pg_catalog', 'information_schema') AND c.relkind = 'v' THEN 'SYSTEM VIEW' " \
	"WHEN c.relkind = 'r' AND c.relpersistence = 't' THEN 'TEMPORARY TABLE' " \
	"WHEN c.relkind = 'r' THEN 'TABLE' " \
	"WHEN c.relkind = 'p' THEN 'PARTITIONED TABLE' " \
	"WHEN c.relkind = 'v' AND c.relpersistence = 't' THEN 'TEMPORARY VIEW' " \
	"WHEN c.relkind = 'v' THEN 'VIEW' " \
	"WHEN c.relkind = 'm' THEN 'MATERIALIZED VIEW' " \
	"WHEN c.relkind = 'f' THEN 'FOREIGN TABLE' END"

/* Schemas a session sees: not TOAST's, nor another session's temporary ones. */
#define VISIBLE_SCHEMA \
	"n.nspname !~ '^pg_toast' AND (n.nspname !~ '^pg_temp_' OR n.oid = pg_catalog.pg_my_temp_schema())"

static const char *const table_types[] = {
	"FOREIGN TABLE", "MATERIALIZED VIEW", "PARTITIONED TABLE", "SYSTEM TABLE",
	"SYSTEM VIEW", "TABLE", "TEMPORARY TABLE", "TEMPORARY VIEW", "VIEW"
};

/* A command's result: SQL, its arguments, and its fields' names and widths. */
typedef struct CatalogQuery
{
	const char *sql;
	int			nargs;
	Oid			argtypes[6];
	Datum		args[6];
	bool		nulls[6];
	TupleDesc	desc;			/* the result's columns */
	VexecEgressField *fields;
	bool		include_schema; /* GetTables: a table_schema column */
} CatalogQuery;

static void
arg_text(CatalogQuery *q, bool set, const char *value)
{
	q->argtypes[q->nargs] = TEXTOID;
	q->nulls[q->nargs] = !set;
	q->args[q->nargs] = set ? CStringGetTextDatum(value) : (Datum) 0;
	q->nargs++;
}

/* The result's columns, by name and type, and the fields FlightSql.proto names. */
static void
columns(CatalogQuery *q, int n, const char *const *names, const Oid *types,
		const bool *not_null, const char *const *formats)
{
	int			i;

	q->desc = CreateTemplateTupleDesc(n);
	q->fields = palloc0(sizeof(VexecEgressField) * n);
	for (i = 0; i < n; i++)
	{
		TupleDescInitEntry(q->desc, (AttrNumber) (i + 1), names[i], types[i], -1, 0);
		q->fields[i].name = names[i];
		q->fields[i].not_null = not_null[i];
		q->fields[i].format = formats ? formats[i] : NULL;
		q->fields[i].bare = true;	/* FlightSql.proto's schema, exactly */
	}
}

static const char *const key_names[] = {
	"pk_catalog_name", "pk_db_schema_name", "pk_table_name", "pk_column_name",
	"fk_catalog_name", "fk_db_schema_name", "fk_table_name", "fk_column_name",
	"key_sequence", "fk_key_name", "pk_key_name", "update_rule", "delete_rule"
};
static const Oid key_types[] = {
	TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID,
	INT4OID, TEXTOID, TEXTOID, INT4OID, INT4OID
};
static const bool key_not_null[] = {
	false, false, true, true, false, false, true, true, true, false, false, true, true
};
static const char *const key_formats[] = {
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "i", NULL, NULL, "C", "C"
};

/*
 * Foreign keys, in JDBC's rules for updates and deletes, which Flight SQL's
 * UpdateDeleteRules follow: CASCADE 0, RESTRICT 1, SET NULL 2, NO ACTION 3,
 * SET DEFAULT 4.
 */
#define KEYS_SELECT \
	"SELECT pg_catalog.current_database()::text, pn.nspname::text, pc.relname::text, pa.attname::text, " \
	"pg_catalog.current_database()::text, fn.nspname::text, fc.relname::text, fa.attname::text, " \
	"k.ord::int4, con.conname::text, pk.conname::text, " \
	"CASE con.confupdtype WHEN 'c' THEN 0 WHEN 'r' THEN 1 WHEN 'n' THEN 2 WHEN 'a' THEN 3 ELSE 4 END::int4, " \
	"CASE con.confdeltype WHEN 'c' THEN 0 WHEN 'r' THEN 1 WHEN 'n' THEN 2 WHEN 'a' THEN 3 ELSE 4 END::int4 " \
	"FROM pg_catalog.pg_constraint con " \
	"JOIN pg_catalog.pg_class fc ON fc.oid = con.conrelid " \
	"JOIN pg_catalog.pg_namespace fn ON fn.oid = fc.relnamespace " \
	"JOIN pg_catalog.pg_class pc ON pc.oid = con.confrelid " \
	"JOIN pg_catalog.pg_namespace pn ON pn.oid = pc.relnamespace " \
	"CROSS JOIN LATERAL pg_catalog.unnest(con.conkey, con.confkey) WITH ORDINALITY AS k(fkatt, pkatt, ord) " \
	"JOIN pg_catalog.pg_attribute fa ON fa.attrelid = fc.oid AND fa.attnum = k.fkatt " \
	"JOIN pg_catalog.pg_attribute pa ON pa.attrelid = pc.oid AND pa.attnum = k.pkatt " \
	"LEFT JOIN pg_catalog.pg_constraint pk ON pk.conindid = con.conindid AND pk.conrelid = con.confrelid " \
	"AND pk.contype IN ('p', 'u') " \
	"WHERE con.contype = 'f' "

/* A catalog filter: unset, any; else this database's name, or none. */
#define CATALOG_IS(n) "($" #n "::text IS NULL OR $" #n " = pg_catalog.current_database()) "

/*
 * The query of a catalog command, or false when the type is not one.  The
 * value is the command's protobuf message.
 */
static bool
catalog_query(const char *type, const ProtobufCBinaryData *value, CatalogQuery *q)
{
	ProtobufCAllocator *pa = flight_pb_allocator();

	memset(q, 0, sizeof(*q));
	if (strcmp(type, "CommandGetCatalogs") == 0)
	{
		static const char *const names[] = {"catalog_name"};
		static const Oid types[] = {TEXTOID};
		static const bool nn[] = {true};

		q->sql = "SELECT pg_catalog.current_database()::text";
		columns(q, 1, names, types, nn, NULL);
		return true;
	}
	if (strcmp(type, "CommandGetDbSchemas") == 0)
	{
		Arrow__Flight__Protocol__Sql__CommandGetDbSchemas *c =
			(Arrow__Flight__Protocol__Sql__CommandGetDbSchemas *) protobuf_c_message_unpack(&arrow__flight__protocol__sql__command_get_db_schemas__descriptor,
									  pa, value->len, value->data);
		static const char *const names[] = {"catalog_name", "db_schema_name"};
		static const Oid types[] = {TEXTOID, TEXTOID};
		static const bool nn[] = {false, true};

		if (c == NULL)
			flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "a malformed %s", type);
		arg_text(q, c->_catalog_case != 0, c->catalog);
		arg_text(q, c->_db_schema_filter_pattern_case != 0, c->db_schema_filter_pattern);
		q->sql = "SELECT pg_catalog.current_database()::text, n.nspname::text "
			"FROM pg_catalog.pg_namespace n "
			"WHERE " CATALOG_IS(1) "AND ($2::text IS NULL OR n.nspname LIKE $2) AND " VISIBLE_SCHEMA
			" ORDER BY 1, 2";
		columns(q, 2, names, types, nn, NULL);
		return true;
	}
	if (strcmp(type, "CommandGetTables") == 0)
	{
		Arrow__Flight__Protocol__Sql__CommandGetTables *c =
			(Arrow__Flight__Protocol__Sql__CommandGetTables *) protobuf_c_message_unpack(&arrow__flight__protocol__sql__command_get_tables__descriptor,
									  pa, value->len, value->data);
		static const char *const names[] = {"catalog_name", "db_schema_name", "table_name",
		"table_type", "table_schema"};
		static const Oid types[] = {TEXTOID, TEXTOID, TEXTOID, TEXTOID, BYTEAOID};
		static const bool nn[] = {false, false, true, true, true};

		if (c == NULL)
			flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "a malformed %s", type);
		arg_text(q, c->_catalog_case != 0, c->catalog);
		arg_text(q, c->_db_schema_filter_pattern_case != 0, c->db_schema_filter_pattern);
		arg_text(q, c->_table_name_filter_pattern_case != 0, c->table_name_filter_pattern);
		{
			/* the table types asked for, as text[]; none: all */
			Datum	   *elems = palloc(sizeof(Datum) * Max(c->n_table_types, 1));
			size_t		i;

			for (i = 0; i < c->n_table_types; i++)
				elems[i] = CStringGetTextDatum(c->table_types[i]);
			q->argtypes[q->nargs] = TEXTARRAYOID;
			q->nulls[q->nargs] = c->n_table_types == 0;
			q->args[q->nargs] = c->n_table_types == 0 ? (Datum) 0 :
				PointerGetDatum(construct_array_builtin(elems, (int) c->n_table_types, TEXTOID));
			q->nargs++;
		}
		q->include_schema = c->include_schema;
		q->sql = "SELECT * FROM (SELECT pg_catalog.current_database()::text AS catalog_name, "
			"n.nspname::text AS db_schema_name, c.relname::text AS table_name, "
			TABLE_TYPE " AS table_type, c.oid AS table_oid "
			"FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
			"WHERE c.relkind IN ('r', 'p', 'v', 'm', 'f') AND " CATALOG_IS(1)
			"AND ($2::text IS NULL OR n.nspname LIKE $2) AND ($3::text IS NULL OR c.relname LIKE $3) "
			"AND " VISIBLE_SCHEMA ") t "
			"WHERE $4::text[] IS NULL OR t.table_type = ANY ($4) ORDER BY 1, 2, 3, 4";
		columns(q, c->include_schema ? 5 : 4, names, types, nn, NULL);
		return true;
	}
	if (strcmp(type, "CommandGetTableTypes") == 0)
	{
		static const char *const names[] = {"table_type"};
		static const Oid types[] = {TEXTOID};
		static const bool nn[] = {true};
		StringInfoData sql;
		int			i;

		initStringInfo(&sql);
		appendStringInfoString(&sql, "SELECT t FROM (VALUES ");
		for (i = 0; i < (int) lengthof(table_types); i++)
			appendStringInfo(&sql, "%s(%s)", i ? ", " : "", quote_literal_cstr(table_types[i]));
		appendStringInfoString(&sql, ") v(t) ORDER BY 1");
		q->sql = sql.data;
		columns(q, 1, names, types, nn, NULL);
		return true;
	}
	if (strcmp(type, "CommandGetPrimaryKeys") == 0)
	{
		Arrow__Flight__Protocol__Sql__CommandGetPrimaryKeys *c =
			(Arrow__Flight__Protocol__Sql__CommandGetPrimaryKeys *) protobuf_c_message_unpack(&arrow__flight__protocol__sql__command_get_primary_keys__descriptor,
									  pa, value->len, value->data);
		static const char *const names[] = {"catalog_name", "db_schema_name", "table_name",
		"column_name", "key_name", "key_sequence"};
		static const Oid types[] = {TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, INT4OID};
		static const bool nn[] = {false, false, true, true, false, true};

		if (c == NULL)
			flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "a malformed %s", type);
		arg_text(q, c->_catalog_case != 0, c->catalog);
		arg_text(q, c->_db_schema_case != 0, c->db_schema);
		arg_text(q, true, c->table ? c->table : "");
		q->sql = "SELECT pg_catalog.current_database()::text, n.nspname::text, c.relname::text, "
			"a.attname::text, con.conname::text, k.ord::int4 "
			"FROM pg_catalog.pg_constraint con "
			"JOIN pg_catalog.pg_class c ON c.oid = con.conrelid "
			"JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
			"CROSS JOIN LATERAL pg_catalog.unnest(con.conkey) WITH ORDINALITY AS k(att, ord) "
			"JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid AND a.attnum = k.att "
			"WHERE con.contype = 'p' AND " CATALOG_IS(1)
			"AND ($2::text IS NULL OR n.nspname = $2) AND c.relname = $3 ORDER BY 1, 2, 3, 6";
		columns(q, 6, names, types, nn, NULL);
		return true;
	}
	if (strcmp(type, "CommandGetExportedKeys") == 0 || strcmp(type, "CommandGetImportedKeys") == 0)
	{
		bool		exported = type[10] == 'E';
		Arrow__Flight__Protocol__Sql__CommandGetExportedKeys *c =
			(Arrow__Flight__Protocol__Sql__CommandGetExportedKeys *) protobuf_c_message_unpack(exported ?
									  &arrow__flight__protocol__sql__command_get_exported_keys__descriptor :
									  &arrow__flight__protocol__sql__command_get_imported_keys__descriptor,
									  pa, value->len, value->data);

		/* the two messages have the same fields, in the same places */
		StaticAssertStmt(sizeof(Arrow__Flight__Protocol__Sql__CommandGetExportedKeys) ==
						 sizeof(Arrow__Flight__Protocol__Sql__CommandGetImportedKeys),
						 "CommandGetExportedKeys and CommandGetImportedKeys differ");
		if (c == NULL)
			flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "a malformed %s", type);
		arg_text(q, c->_catalog_case != 0, c->catalog);
		arg_text(q, c->_db_schema_case != 0, c->db_schema);
		arg_text(q, true, c->table ? c->table : "");
		q->sql = exported ?
			KEYS_SELECT "AND " CATALOG_IS(1) "AND ($2::text IS NULL OR pn.nspname = $2) AND pc.relname = $3 "
			"ORDER BY 5, 6, 7, 10, 9" :
			KEYS_SELECT "AND " CATALOG_IS(1) "AND ($2::text IS NULL OR fn.nspname = $2) AND fc.relname = $3 "
			"ORDER BY 1, 2, 3, 11, 9";
		columns(q, 13, key_names, key_types, key_not_null, key_formats);
		return true;
	}
	if (strcmp(type, "CommandGetCrossReference") == 0)
	{
		Arrow__Flight__Protocol__Sql__CommandGetCrossReference *c =
			(Arrow__Flight__Protocol__Sql__CommandGetCrossReference *) protobuf_c_message_unpack(&arrow__flight__protocol__sql__command_get_cross_reference__descriptor,
									  pa, value->len, value->data);

		if (c == NULL)
			flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "a malformed %s", type);
		arg_text(q, c->_pk_catalog_case != 0, c->pk_catalog);
		arg_text(q, c->_pk_db_schema_case != 0, c->pk_db_schema);
		arg_text(q, true, c->pk_table ? c->pk_table : "");
		arg_text(q, c->_fk_catalog_case != 0, c->fk_catalog);
		arg_text(q, c->_fk_db_schema_case != 0, c->fk_db_schema);
		arg_text(q, true, c->fk_table ? c->fk_table : "");
		q->sql = KEYS_SELECT "AND " CATALOG_IS(1) "AND ($2::text IS NULL OR pn.nspname = $2) "
			"AND pc.relname = $3 AND " CATALOG_IS(4) "AND ($5::text IS NULL OR fn.nspname = $5) "
			"AND fc.relname = $6 ORDER BY 5, 6, 7, 10, 9";
		columns(q, 13, key_names, key_types, key_not_null, key_formats);
		return true;
	}
	return false;
}

/* ---------------------------------------------------------------------
 * GetTables' table_schema: a column the SQL does not make, in place of
 * its table_oid
 * ---------------------------------------------------------------------
 */

typedef struct TablesReceiver
{
	DestReceiver pub;
	DestReceiver *egress;
	TupleDesc	desc;			/* the five columns */
	TupleTableSlot *slot;
} TablesReceiver;

/* A table's columns, with Flight SQL's metadata, as an encapsulated IPC schema. */
static bytea *
table_schema(Oid relid)
{
	Relation	rel = try_relation_open(relid, AccessShareLock);
	TupleDesc	desc;
	TupleDesc	live;
	StringInfoData bytes;
	VexecEgressField *fields;
	int			n = 0;
	int			i;
	bytea	   *out;

	initStringInfo(&bytes);
	if (rel == NULL)
		live = CreateTemplateTupleDesc(0);	/* dropped meanwhile */
	else
	{
		desc = RelationGetDescr(rel);
		live = CreateTemplateTupleDesc(desc->natts);
		for (i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (att->attisdropped)
				continue;
			TupleDescInitEntry(live, (AttrNumber) (++n), NameStr(att->attname),
							   att->atttypid, att->atttypmod, 0);
			TupleDescAttr(live, n - 1)->attnotnull = att->attnotnull;
			TupleDescInitEntryCollation(live, (AttrNumber) n, att->attcollation);
		}
		live->natts = n;
	}
	fields = flight_sql_fields(live);
	for (i = 0; i < live->natts; i++)
	{
		/* the table's own names, and NOT NULL as the field's nullability */
		const char **keys = palloc(sizeof(char *) * (fields[i].nmetadata + 3));
		const char **values = palloc(sizeof(char *) * (fields[i].nmetadata + 3));

		memcpy(keys, fields[i].keys, sizeof(char *) * fields[i].nmetadata);
		memcpy(values, fields[i].values, sizeof(char *) * fields[i].nmetadata);
		keys[fields[i].nmetadata] = "ARROW:FLIGHT:SQL:CATALOG_NAME";
		values[fields[i].nmetadata] = get_database_name(MyDatabaseId);
		keys[fields[i].nmetadata + 1] = "ARROW:FLIGHT:SQL:DB_SCHEMA_NAME";
		values[fields[i].nmetadata + 1] = get_namespace_name(RelationGetNamespace(rel));
		keys[fields[i].nmetadata + 2] = "ARROW:FLIGHT:SQL:TABLE_NAME";
		values[fields[i].nmetadata + 2] = RelationGetRelationName(rel);
		fields[i].keys = keys;
		fields[i].values = values;
		fields[i].nmetadata += 3;
		fields[i].not_null = TupleDescAttr(live, i)->attnotnull;
	}
	flight_schema_bytes(live, fields, live->natts, &bytes);
	if (rel)
		relation_close(rel, AccessShareLock);
	out = palloc(VARHDRSZ + bytes.len);
	SET_VARSIZE(out, VARHDRSZ + bytes.len);
	memcpy(VARDATA(out), bytes.data, bytes.len);
	return out;
}

static void
tables_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	TablesReceiver *r = (TablesReceiver *) self;

	(void) typeinfo;
	r->slot = MakeSingleTupleTableSlot(r->desc, &TTSOpsVirtual);
	r->egress->rStartup(r->egress, operation, r->desc);
}

static bool
tables_receive(TupleTableSlot *slot, DestReceiver *self)
{
	TablesReceiver *r = (TablesReceiver *) self;
	int			i;

	slot_getallattrs(slot);
	ExecClearTuple(r->slot);
	for (i = 0; i < 4; i++)
	{
		r->slot->tts_values[i] = slot->tts_values[i];
		r->slot->tts_isnull[i] = slot->tts_isnull[i];
	}
	r->slot->tts_values[4] = PointerGetDatum(table_schema(DatumGetObjectId(slot->tts_values[4])));
	r->slot->tts_isnull[4] = false;
	ExecStoreVirtualTuple(r->slot);
	return r->egress->receiveSlot(r->slot, r->egress);
}

static void
tables_shutdown(DestReceiver *self)
{
	TablesReceiver *r = (TablesReceiver *) self;

	r->egress->rShutdown(r->egress);
	ExecDropSingleTupleTableSlot(r->slot);
}

static void
tables_destroy(DestReceiver *self)
{
	pfree(self);
}

static DestReceiver *
tables_wrap(DestReceiver *egress, void *arg)
{
	TablesReceiver *r = palloc0(sizeof(TablesReceiver));

	r->pub.receiveSlot = tables_receive;
	r->pub.rStartup = tables_startup;
	r->pub.rShutdown = tables_shutdown;
	r->pub.rDestroy = tables_destroy;
	r->pub.mydest = DestNone;
	r->egress = egress;
	r->desc = arg;
	return &r->pub;
}

/* ---------------------------------------------------------------------
 * Server information, and type information: arrays of the endpoint's own
 * ---------------------------------------------------------------------
 */

typedef enum InfoKind
{
	INFO_STRING,
	INFO_BOOL,
	INFO_BIGINT,
	INFO_INT32,
	INFO_STRING_LIST
} InfoKind;

typedef struct InfoValue
{
	uint32		id;
	int			kind;
	const char *s;
	int64		i;
} InfoValue;

/* The information Flight SQL's ids ask for, PostgreSQL's (FlightSql.proto, SqlInfo). */
static const InfoValue sql_info[] = {
	{0, INFO_STRING, "PostgreSQL"},
	{1, INFO_STRING, NULL},		/* server_version, at run time */
	{2, INFO_STRING, "1.5.0"},
	{3, INFO_BOOL, NULL, 0},
	{4, INFO_BOOL, NULL, 1},
	{5, INFO_BOOL, NULL, 0},
	{8, INFO_INT32, NULL, 2},	/* SQL_SUPPORTED_TRANSACTION_SAVEPOINT */
	{9, INFO_BOOL, NULL, 1},
	{10, INFO_BOOL, NULL, 1},	/* bulk ingestion: CommandStatementIngest (ingest.c) */
	{11, INFO_BOOL, NULL, 1},	/* ingest in a transaction: its transaction_id */
	{100, INFO_INT32, NULL, 0},
	{101, INFO_INT32, NULL, 0},
	{500, INFO_BOOL, NULL, 1},
	{501, INFO_BOOL, NULL, 1},
	{502, INFO_BOOL, NULL, 1},
	{503, INFO_INT32, NULL, 3}, /* SQL_CASE_SENSITIVITY_LOWERCASE */
	{504, INFO_STRING, "\""},
	{505, INFO_INT32, NULL, 0},
	{506, INFO_BOOL, NULL, 0},
	{507, INFO_INT32, NULL, 0}, /* SQL_NULLS_SORTED_HIGH */
	{508, INFO_STRING_LIST, NULL},	/* the reserved keywords, at run time */
	{513, INFO_STRING, "\\"},
	{514, INFO_STRING, ""},
	{515, INFO_BOOL, NULL, 1},
	{516, INFO_BOOL, NULL, 1},
	{518, INFO_BOOL, NULL, 1},
	{519, INFO_BOOL, NULL, 0},
	{520, INFO_BOOL, NULL, 1},
	{521, INFO_BOOL, NULL, 1},
	{523, INFO_BOOL, NULL, 1},
	{524, INFO_BOOL, NULL, 1},
	{527, INFO_BOOL, NULL, 1},
	{529, INFO_STRING, "schema"},
	{530, INFO_STRING, "function"},
	{531, INFO_STRING, "database"},
	{532, INFO_BOOL, NULL, 1},
	{536, INFO_BOOL, NULL, 1},
	{537, INFO_BOOL, NULL, 1},
	{539, INFO_BOOL, NULL, 1},
	{543, INFO_BIGINT, NULL, NAMEDATALEN - 1},
	{547, INFO_BIGINT, NULL, 1664},
	{548, INFO_BIGINT, NULL, 1600},
	{552, INFO_BIGINT, NULL, NAMEDATALEN - 1},
	{554, INFO_BIGINT, NULL, NAMEDATALEN - 1},
	{559, INFO_BIGINT, NULL, NAMEDATALEN - 1},
	{561, INFO_BIGINT, NULL, NAMEDATALEN - 1},
	{562, INFO_INT32, NULL, 2}, /* SQL_TRANSACTION_READ_COMMITTED */
	{563, INFO_BOOL, NULL, 1},
	{565, INFO_BOOL, NULL, 0},
	{566, INFO_BOOL, NULL, 0},
	{572, INFO_BOOL, NULL, 1},
	{573, INFO_BOOL, NULL, 1},
	{574, INFO_BOOL, NULL, 0},
};

static void
reserved_keywords(const char ***words, int *n)
{
	int			i;

	*words = palloc(sizeof(char *) * ScanKeywords.num_keywords);
	*n = 0;
	for (i = 0; i < ScanKeywords.num_keywords; i++)
		if (ScanKeywordCategories[i] == RESERVED_KEYWORD)
			(*words)[(*n)++] = GetScanKeyword(i, &ScanKeywords);
}

/* GetSqlInfo's arrays: info_name, and a dense union of the values. */
static void
sql_info_arrays(const uint32 *ids, int nids, struct ArrowSchema *schema, struct ArrowArray *array)
{
	int			nall = lengthof(sql_info);
	const InfoValue **rows = palloc(sizeof(InfoValue *) * nall);
	int			n = 0;
	uint32	   *names;
	int8	   *type_ids;
	int32	   *offsets;
	const char **strings;
	bool	   *bools;
	int64	   *bigints;
	int32	   *int32s;
	const char *const **lists;
	int		   *lens;
	int			counts[5] = {0};
	ArrowCol   *children[6];
	ArrowCol   *cols[2];
	const char **keywords;
	int			nkeywords;
	int			i;
	int			j;

	for (i = 0; i < nall; i++)
	{
		if (nids > 0)
		{
			for (j = 0; j < nids && ids[j] != sql_info[i].id; j++)
				;
			if (j == nids)
				continue;
		}
		rows[n++] = &sql_info[i];
	}
	reserved_keywords(&keywords, &nkeywords);
	names = palloc(sizeof(uint32) * Max(n, 1));
	type_ids = palloc(Max(n, 1));
	offsets = palloc(sizeof(int32) * Max(n, 1));
	strings = palloc(sizeof(char *) * Max(n, 1));
	bools = palloc(sizeof(bool) * Max(n, 1));
	bigints = palloc(sizeof(int64) * Max(n, 1));
	int32s = palloc(sizeof(int32) * Max(n, 1));
	lists = palloc(sizeof(char **) * Max(n, 1));
	lens = palloc(sizeof(int) * Max(n, 1));
	for (i = 0; i < n; i++)
	{
		const InfoValue *v = rows[i];
		int			k = v->kind;

		names[i] = v->id;
		type_ids[i] = (int8) k;
		offsets[i] = counts[k];
		switch (k)
		{
			case INFO_STRING:
				strings[counts[k]] = v->id == 1 ? GetConfigOption("server_version", false, false) : v->s;
				break;
			case INFO_BOOL:
				bools[counts[k]] = v->i != 0;
				break;
			case INFO_BIGINT:
				bigints[counts[k]] = v->i;
				break;
			case INFO_INT32:
				int32s[counts[k]] = (int32) v->i;
				break;
			case INFO_STRING_LIST:
				lists[counts[k]] = keywords;
				lens[counts[k]] = nkeywords;
				break;
		}
		counts[k]++;
	}
	children[0] = arrays_utf8("string_value", true, strings, counts[INFO_STRING]);
	children[1] = arrays_bool("bool_value", true, bools, NULL, counts[INFO_BOOL]);
	children[2] = arrays_int64("bigint_value", true, bigints, NULL, counts[INFO_BIGINT]);
	children[3] = arrays_int32("int32_bitmask", true, int32s, NULL, counts[INFO_INT32]);
	children[4] = arrays_list_utf8("string_list", true, lists, lens, counts[INFO_STRING_LIST]);
	children[5] = arrays_empty_map_int32_list("int32_to_int32_list_map", true);
	cols[0] = arrays_uint32("info_name", false, names, NULL, n);
	cols[1] = arrays_dense_union("value", children, 6, type_ids, offsets, n);
	arrays_batch(cols, 2, n, schema, array);
}

/* XDBC's types: those of PostgreSQL's a client maps (FlightSql.proto, CommandGetXdbcTypeInfo). */
typedef struct XdbcType
{
	const char *name;
	int32		data_type;
	int32		column_size;	/* 0: none */
	const char *prefix;
	const char *suffix;
	const char *params;			/* comma-separated, or NULL */
	bool		case_sensitive;
	int32		searchable;
	bool		fixed;
	int32		datetime_subcode;	/* 0: none */
	int32		radix;			/* 0: none */
} XdbcType;

static const XdbcType xdbc_types[] = {
	{"boolean", -7, 1, NULL, NULL, NULL, false, 2, false, 0, 0},
	{"smallint", 5, 5, NULL, NULL, NULL, false, 2, false, 0, 10},
	{"integer", 4, 10, NULL, NULL, NULL, false, 2, false, 0, 10},
	{"bigint", -5, 19, NULL, NULL, NULL, false, 2, false, 0, 10},
	{"real", 7, 24, NULL, NULL, NULL, false, 2, false, 0, 2},
	{"double precision", 8, 53, NULL, NULL, NULL, false, 2, false, 0, 2},
	{"numeric", 2, 1000, NULL, NULL, "precision,scale", false, 2, false, 0, 10},
	{"character", 1, 10485760, "'", "'", "length", true, 3, false, 0, 0},
	{"character varying", 12, 10485760, "'", "'", "length", true, 3, false, 0, 0},
	{"text", -1, 0, "'", "'", NULL, true, 3, false, 0, 0},
	{"bytea", -3, 0, "'\\x", "'", NULL, false, 2, false, 0, 0},
	{"date", 91, 10, "'", "'", NULL, false, 2, false, 1, 0},
	{"time", 92, 15, "'", "'", "precision", false, 2, false, 2, 0},
	{"timestamp", 93, 26, "'", "'", "precision", false, 2, false, 3, 0},
	{"timestamp with time zone", 93, 32, "'", "'", "precision", false, 2, false, 5, 0},
	{"interval", 10, 49, "'", "'", "precision", false, 2, false, 0, 0},
	{"uuid", -2, 16, "'", "'", NULL, false, 2, false, 0, 0},
};

static void
xdbc_arrays(bool has_type, int32 data_type, struct ArrowSchema *schema, struct ArrowArray *array)
{
	int			nall = lengthof(xdbc_types);
	const XdbcType **rows = palloc(sizeof(XdbcType *) * nall);
	int			n = 0;
	const char **names;
	int32	   *types;
	int32	   *sizes;
	bool	   *size_nulls;
	const char **prefixes;
	const char **suffixes;
	const char *const **params;
	int		   *nparams;
	int32	   *nullable;
	bool	   *case_sensitive;
	int32	   *searchable;
	bool	   *unsigned_attr;
	bool	   *fixed;
	bool	   *auto_inc;
	int32	   *zeros;
	bool	   *all_null;
	int32	   *subcodes;
	bool	   *subcode_nulls;
	int32	   *radixes;
	bool	   *radix_nulls;
	ArrowCol   *cols[19];
	int			i;

	for (i = 0; i < nall; i++)
		if (!has_type || xdbc_types[i].data_type == data_type)
			rows[n++] = &xdbc_types[i];
#define ALLOC(T) ((T *) palloc0(sizeof(T) * Max(n, 1)))
	names = ALLOC(const char *);
	types = ALLOC(int32);
	sizes = ALLOC(int32);
	size_nulls = ALLOC(bool);
	prefixes = ALLOC(const char *);
	suffixes = ALLOC(const char *);
	params = ALLOC(const char *const *);
	nparams = ALLOC(int);
	nullable = ALLOC(int32);
	case_sensitive = ALLOC(bool);
	searchable = ALLOC(int32);
	unsigned_attr = ALLOC(bool);
	fixed = ALLOC(bool);
	auto_inc = ALLOC(bool);
	zeros = ALLOC(int32);
	all_null = ALLOC(bool);
	subcodes = ALLOC(int32);
	subcode_nulls = ALLOC(bool);
	radixes = ALLOC(int32);
	radix_nulls = ALLOC(bool);
#undef ALLOC
	for (i = 0; i < n; i++)
	{
		const XdbcType *t = rows[i];

		names[i] = t->name;
		types[i] = t->data_type;
		sizes[i] = t->column_size;
		size_nulls[i] = t->column_size == 0;
		prefixes[i] = t->prefix;
		suffixes[i] = t->suffix;
		if (t->params)
		{
			List	   *elems;
			char	   *raw = pstrdup(t->params);
			const char **arr;
			ListCell   *lc;
			int			k = 0;

			(void) SplitIdentifierString(raw, ',', &elems);
			arr = palloc(sizeof(char *) * Max(list_length(elems), 1));
			foreach(lc, elems)
				arr[k++] = lfirst(lc);
			params[i] = arr;
			nparams[i] = k;
		}
		nullable[i] = 1;		/* NULLABILITY_NULLABLE */
		case_sensitive[i] = t->case_sensitive;
		searchable[i] = t->searchable;
		unsigned_attr[i] = false;
		fixed[i] = t->fixed;
		auto_inc[i] = false;
		all_null[i] = true;
		subcodes[i] = t->datetime_subcode;
		subcode_nulls[i] = t->datetime_subcode == 0;
		radixes[i] = t->radix;
		radix_nulls[i] = t->radix == 0;
	}
	cols[0] = arrays_utf8("type_name", false, names, n);
	cols[1] = arrays_int32("data_type", false, types, NULL, n);
	cols[2] = arrays_int32("column_size", true, sizes, size_nulls, n);
	cols[3] = arrays_utf8("literal_prefix", true, prefixes, n);
	cols[4] = arrays_utf8("literal_suffix", true, suffixes, n);
	cols[5] = arrays_list_utf8("create_params", true, params, nparams, n);
	cols[6] = arrays_int32("nullable", false, nullable, NULL, n);
	cols[7] = arrays_bool("case_sensitive", false, case_sensitive, NULL, n);
	cols[8] = arrays_int32("searchable", false, searchable, NULL, n);
	cols[9] = arrays_bool("unsigned_attribute", true, unsigned_attr, NULL, n);
	cols[10] = arrays_bool("fixed_prec_scale", false, fixed, NULL, n);
	cols[11] = arrays_bool("auto_increment", true, auto_inc, NULL, n);
	cols[12] = arrays_utf8("local_type_name", true, names, n);
	cols[13] = arrays_int32("minimum_scale", true, zeros, all_null, n);
	cols[14] = arrays_int32("maximum_scale", true, zeros, all_null, n);
	cols[15] = arrays_int32("sql_data_type", false, types, NULL, n);
	cols[16] = arrays_int32("datetime_subcode", true, subcodes, subcode_nulls, n);
	cols[17] = arrays_int32("num_prec_radix", true, radixes, radix_nulls, n);
	cols[18] = arrays_int32("interval_precision", true, zeros, all_null, n);
	arrays_batch(cols, 19, n, schema, array);
}

/* The arrays of a command no statement serves, or false. */
static bool
own_arrays(const char *type, const ProtobufCBinaryData *value,
		   struct ArrowSchema *schema, struct ArrowArray *array)
{
	if (strcmp(type, "CommandGetSqlInfo") == 0)
	{
		Arrow__Flight__Protocol__Sql__CommandGetSqlInfo *c =
			(Arrow__Flight__Protocol__Sql__CommandGetSqlInfo *) protobuf_c_message_unpack(&arrow__flight__protocol__sql__command_get_sql_info__descriptor,
									  flight_pb_allocator(), value->len, value->data);

		if (c == NULL)
			flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "a malformed %s", type);
		sql_info_arrays(c->info, (int) c->n_info, schema, array);
		return true;
	}
	if (strcmp(type, "CommandGetXdbcTypeInfo") == 0)
	{
		Arrow__Flight__Protocol__Sql__CommandGetXdbcTypeInfo *c =
			(Arrow__Flight__Protocol__Sql__CommandGetXdbcTypeInfo *) protobuf_c_message_unpack(&arrow__flight__protocol__sql__command_get_xdbc_type_info__descriptor,
									  flight_pb_allocator(), value->len, value->data);

		if (c == NULL)
			flight_error(GRPC_INVALID_ARGUMENT, ERRCODE_PROTOCOL_VIOLATION, "a malformed %s", type);
		xdbc_arrays(c->_data_type_case != 0, c->data_type, schema, array);
		return true;
	}
	return false;
}

/* ---------------------------------------------------------------------
 * The commands
 * ---------------------------------------------------------------------
 */

typedef struct SchemaOnly
{
	StringInfo	out;
} SchemaOnly;

static void
schema_only(void *arg, const VexecEgressMessage *msg)
{
	SchemaOnly *so = arg;

	if (msg->kind == VEXEC_EGRESS_SCHEMA)
	{
		appendBinaryStringInfo(so->out, msg->prefix, 8);
		appendBinaryStringInfo(so->out, msg->metadata, (int) msg->metadata_len);
	}
}

/* A catalog command's result schema, or false when the type is not one. */
bool
flight_catalog_schema(const char *type, ProtobufCBinaryData *value, StringInfo out)
{
	CatalogQuery q;
	struct ArrowSchema schema;
	struct ArrowArray array;

	if (catalog_query(type, value, &q))
	{
		flight_schema_bytes(q.desc, q.fields, q.desc->natts, out);
		return true;
	}
	if (own_arrays(type, value, &schema, &array))
	{
		SchemaOnly	so = {out};

		flight_session->egress->arrays(&schema, &array, true, schema_only, &so);
		return true;
	}
	return false;
}

/* A catalog command run, its result to the call; false when the type is not one. */
bool
flight_catalog_run(FlightCall *call, const char *type, ProtobufCBinaryData *value)
{
	CatalogQuery q;
	struct ArrowSchema schema;
	struct ArrowArray array;

	if (catalog_query(type, value, &q))
	{
		if (q.include_schema)
			flight_run_sql(call, q.sql, q.nargs, q.argtypes, q.args, q.nulls, q.fields, 5,
						   tables_wrap, q.desc);
		else if (strcmp(type, "CommandGetTables") == 0)
		{
			/* table_oid, the SQL's fifth column, is left out by a projection */
			char	   *sql = psprintf("SELECT catalog_name, db_schema_name, table_name, table_type "
									   "FROM (%s) s ORDER BY 1, 2, 3, 4", q.sql);

			flight_run_sql(call, sql, q.nargs, q.argtypes, q.args, q.nulls, q.fields, 4, NULL, NULL);
		}
		else
			flight_run_sql(call, q.sql, q.nargs, q.argtypes, q.args, q.nulls,
						   q.fields, q.desc->natts, NULL, NULL);
		return true;
	}
	if (own_arrays(type, value, &schema, &array))
	{
		flight_statement_begins(type, 0);
		flight_session->egress->arrays(&schema, &array, true, flight_egress_write, call);
		flight_statement_ends();
		return true;
	}
	return false;
}
