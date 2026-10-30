/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * table.c
 *	  A vexec_parquet relation as the extension reads it: its kind and
 *	  options, its storage and the directory its files are in, and each
 *	  attribute's kind -- the Parquet column an own file keeps it as.
 *
 * Kept in a backend's hash by relation OID until the relation's relcache
 * entry is invalidated; an entry dropped then stays allocated until the
 * transaction ends, since a scan of the transaction may still read it.
 *
 * An own table's files:
 *
 *	the database's directory:	$PGDATA/<the database's path in the
 *								tablespace>/vexec_parquet/<relfilenumber>[_t<backend>]
 *	a storage root:				<root>/<system identifier>/<database OID>/
 *								<tablespace OID>_<relfilenumber>[_t<backend>]
 *
 * each file <file id>_<the writer's FullTransactionId>.parquet.  A column of
 * an own file is named as the attribute was when the file was written, its
 * Parquet field id the attribute's number, by which it is found again: a
 * renamed attribute keeps its files, an added one is missing from older
 * files, and reads as its missing value.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <inttypes.h>
#include <limits.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "catalog/pg_type.h"
#include "common/relpath.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/syscache.h"
#include "access/htup_details.h"

#include "vexec_parquet.h"

typedef struct TableEntry
{
	Oid			relid;			/* hash key */
	VpqTable   *table;
} TableEntry;

static HTAB *tables = NULL;
static List *graveyard = NIL;	/* in TopMemoryContext: tables dropped this transaction */

static void
drop_entry(TableEntry *e)
{
	MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

	graveyard = lappend(graveyard, e->table->cxt);
	MemoryContextSwitchTo(old);
}

static void
relcache_callback(Datum arg, Oid relid)
{
	HASH_SEQ_STATUS seq;
	TableEntry *e;

	if (tables == NULL)
		return;
	if (OidIsValid(relid))
	{
		e = hash_search(tables, &relid, HASH_FIND, NULL);
		if (e)
		{
			drop_entry(e);
			hash_search(tables, &relid, HASH_REMOVE, NULL);
		}
		return;
	}
	hash_seq_init(&seq, tables);
	while ((e = hash_seq_search(&seq)) != NULL)
	{
		drop_entry(e);
		hash_search(tables, &e->relid, HASH_REMOVE, NULL);
	}
}

void
vpq_table_init(void)
{
	CacheRegisterRelcacheCallback(relcache_callback, (Datum) 0);
}

void
vpq_tables_xact_end(void)
{
	ListCell   *lc;

	foreach(lc, graveyard)
		MemoryContextDelete((MemoryContext) lfirst(lc));
	list_free(graveyard);
	graveyard = NIL;
}

void
vpq_storage_of(Relation rel, const RelFileLocator *locator, VpqStorage *st)
{
	memset(st, 0, sizeof(*st));
	st->spc = locator->spcOid;
	st->relnumber = locator->relNumber;
	st->backend = rel->rd_backend;
	st->persistence = rel->rd_rel->relpersistence;
}

/* The root of an own table's storage: its option's, the setting's, or NULL for the database's directory */
char *
vpq_storage_root_of(const VpqOptions *opts, const VpqStorage *st)
{
	if (opts->storage != NULL && opts->storage[0] != '\0')
		return pstrdup(opts->storage);
	if (vpq_storage_root != NULL && vpq_storage_root[0] != '\0')
		return pstrdup(vpq_storage_root);
	return NULL;
}

static char *
trim_slashes(const char *s)
{
	char	   *t = pstrdup(s);
	size_t		n = strlen(t);

	while (n > 1 && t[n - 1] == '/')
		t[--n] = '\0';
	return t;
}

/* The database's own directory of the extension's files in a tablespace */
static char *
database_dir(Oid dboid, Oid spc)
{
	char	   *dbpath = GetDatabasePath(dboid, spc);
	char	   *dir = psprintf("%s/%s/vexec_parquet", DataDir, dbpath);

	pfree(dbpath);
	return dir;
}

char *
vpq_storage_dir(const VpqStorage *st, const char *root, Oid dboid)
{
	char		backend[24] = "";

	if (st->backend >= 0)
		snprintf(backend, sizeof(backend), "_t%d", st->backend);
	if (root == NULL)
		return psprintf("%s/%u%s", database_dir(dboid, st->spc), st->relnumber, backend);
	return psprintf("%s/" UINT64_FORMAT "/%u/%u_%u%s", trim_slashes(root), GetSystemIdentifier(),
					dboid, st->spc, st->relnumber, backend);
}

char *
vpq_file_path(const char *dir, int64 file_id, uint64 fxid)
{
	return psprintf("%s/" INT64_FORMAT "_" UINT64_FORMAT ".parquet", dir, file_id, fxid);
}

/* A file's id and its writer's FullTransactionId from its name; false for a file not of this form */
bool
vpq_file_name_parse(const char *path, int64 *file_id, uint64 *fxid)
{
	const char *name = strrchr(path, '/');
	char	   *end;

	name = name ? name + 1 : path;
	if (*name < '0' || *name > '9')
		return false;
	errno = 0;
	*file_id = strtoll(name, &end, 10);
	if (errno != 0 || *end != '_')
		return false;
	*fxid = strtoull(end + 1, &end, 10);
	if (errno != 0 || (strcmp(end, ".parquet") != 0 && strcmp(end, ".parquet.tmp") != 0))
		return false;
	return true;
}

static void
set_ftype(VpqColumn *col, int id)
{
	memset(&col->ftype, 0, sizeof(col->ftype));
	col->ftype.id = id;
}

/* numeric's typmod (numeric.c's make_numeric_typmod() turned around) */
static void
numeric_typmod_split(int32 typmod, int *precision, int *scale)
{
	int32		t = typmod - VARHDRSZ;

	*precision = (t >> 16) & 0xffff;
	*scale = ((t & 0x7ff) ^ 1024) - 1024;
}

/*
 * An attribute's kind, and the Parquet column an own file keeps it as: its
 * own type where Parquet has one, else the value's image -- a fixed-length
 * one's bytes, a varlena's without its header
 */
void
vpq_column_kind(VpqColumn *col, bool utf8)
{
	col->json = false;
	switch (col->basetype)
	{
		case BOOLOID:
			col->kind = VPQ_K_BOOL;
			set_ftype(col, VPQ_T_BOOL);
			break;
		case INT2OID:
			col->kind = VPQ_K_INT2;
			set_ftype(col, VPQ_T_INT16);
			break;
		case INT4OID:
			col->kind = VPQ_K_INT4;
			set_ftype(col, VPQ_T_INT32);
			break;
		case INT8OID:
			col->kind = VPQ_K_INT8;
			set_ftype(col, VPQ_T_INT64);
			break;
		case FLOAT4OID:
			col->kind = VPQ_K_FLOAT4;
			set_ftype(col, VPQ_T_FLOAT32);
			break;
		case FLOAT8OID:
			col->kind = VPQ_K_FLOAT8;
			set_ftype(col, VPQ_T_FLOAT64);
			break;
		case OIDOID:
			col->kind = VPQ_K_OID;
			set_ftype(col, VPQ_T_UINT32);
			break;
		case CHAROID:
			col->kind = VPQ_K_CHAR;
			set_ftype(col, VPQ_T_INT8);
			break;
		case DATEOID:
			col->kind = VPQ_K_DATE;
			set_ftype(col, VPQ_T_DATE32);
			break;
		case TIMEOID:
			col->kind = VPQ_K_TIME;
			set_ftype(col, VPQ_T_TIME64);
			col->ftype.unit = VPQ_UNIT_US;
			break;
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			col->kind = col->basetype == TIMESTAMPOID ? VPQ_K_TIMESTAMP : VPQ_K_TIMESTAMPTZ;
			set_ftype(col, VPQ_T_TIMESTAMP);
			col->ftype.unit = VPQ_UNIT_US;
			col->ftype.has_tz = col->basetype == TIMESTAMPTZOID;
			break;
		case NUMERICOID:
			col->precision = col->scale = -1;
			if (col->typmod >= (int32) VARHDRSZ)
				numeric_typmod_split(col->typmod, &col->precision, &col->scale);
			if (col->precision >= 1 && col->precision <= 38 && col->scale >= 0 &&
				col->scale <= col->precision)
			{
				col->kind = VPQ_K_NUMERIC;
				set_ftype(col, VPQ_T_DECIMAL128);
				col->ftype.precision = col->precision;
				col->ftype.scale = col->scale;
				col->ftype.byte_width = 16;
			}
			else
			{
				col->kind = VPQ_K_NUMERIC_ANY;
				set_ftype(col, VPQ_T_BINARY);
			}
			break;
		case TEXTOID:
		case VARCHAROID:
			col->kind = VPQ_K_TEXT;
			set_ftype(col, utf8 ? VPQ_T_STRING : VPQ_T_BINARY);
			break;
		case BPCHAROID:
			col->kind = VPQ_K_BPCHAR;
			set_ftype(col, utf8 ? VPQ_T_STRING : VPQ_T_BINARY);
			break;
		case BYTEAOID:
			col->kind = VPQ_K_BYTEA;
			set_ftype(col, VPQ_T_BINARY);
			break;
		case UUIDOID:
			col->kind = VPQ_K_UUID;
			set_ftype(col, VPQ_T_FIXED_BINARY);
			col->ftype.byte_width = 16;
			col->ftype.uuid = true;
			break;
		case JSONOID:
			col->kind = VPQ_K_JSON;
			set_ftype(col, utf8 ? VPQ_T_STRING : VPQ_T_BINARY);
			col->json = utf8;
			break;
		default:
			if (col->typlen > 0)
			{
				col->kind = VPQ_K_FIXED;
				set_ftype(col, VPQ_T_FIXED_BINARY);
				col->ftype.byte_width = col->typlen;
			}
			else if (col->typlen == -1)
			{
				col->kind = VPQ_K_VARLENA;
				set_ftype(col, VPQ_T_BINARY);
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("a vexec_parquet table cannot keep column \"%s\" of type %s",
								col->name, format_type_be(col->typid))));
			break;
	}
}

/* PQ15 (pg_vector_executor.md §3.39): whether a type has a binary receive function, for read.c's VPQ_RM_RECV */
bool
vpq_type_has_receive(Oid typid)
{
	HeapTuple	tup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	bool		has;

	if (!HeapTupleIsValid(tup))
		return false;
	has = OidIsValid(((Form_pg_type) GETSTRUCT(tup))->typreceive);
	ReleaseSysCache(tup);
	return has;
}

static VpqTable *
build_table(Relation rel, TupleDesc desc, MemoryContext parent)
{
	MemoryContext cxt;
	MemoryContext old;
	VpqTable   *t;

	cxt = AllocSetContextCreate(parent, "vexec_parquet table", ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(cxt);
	t = palloc0(sizeof(VpqTable));
	t->cxt = cxt;
	t->relid = RelationGetRelid(rel);
	vpq_relation_options(rel, &t->opts);
	t->external = t->opts.location != NULL;
	vpq_storage_of(rel, &rel->rd_locator, &t->storage);
	t->utf8 = GetDatabaseEncoding() == PG_UTF8;
	if (!t->external)
	{
		t->root = vpq_storage_root_of(&t->opts, &t->storage);
		t->dir = vpq_storage_dir(&t->storage, t->root, MyDatabaseId);
	}
	t->natts = desc->natts;
	t->cols = palloc0(sizeof(VpqColumn) * Max(desc->natts, 1));
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, i);
		VpqColumn  *c = &t->cols[i];

		c->attnum = i + 1;
		c->name = pstrdup(NameStr(att->attname));
		if (att->attisdropped)
		{
			c->kind = VPQ_K_DROPPED;
			continue;
		}
		c->typid = att->atttypid;
		c->typmod = att->atttypmod;
		c->basetype = getBaseTypeAndTypmod(att->atttypid, &c->typmod);
		get_typlenbyvalalign(c->basetype, &c->typlen, &c->typbyval, &c->typalign);
		c->collation = att->attcollation;
		c->notnull = att->attnotnull;
		vpq_column_kind(c, t->utf8);
		c->recv = vpq_type_has_receive(c->typid);
		c->geo = c->kind == VPQ_K_VARLENA ? vpq_geo_type(c->basetype) : VPQ_GEO_NONE;
		c->elemtype = c->kind == VPQ_K_VARLENA ? vpq_nested_array_element(c->basetype) : InvalidOid;
		c->hstore = c->kind == VPQ_K_VARLENA && vpq_nested_is_hstore(c->basetype);
		{
			bool		isnull;
			Datum		missing = getmissingattr(desc, c->attnum, &isnull);

			c->missing_null = isnull;
			c->missing = isnull ? (Datum) 0 : datumCopy(missing, c->typbyval, c->typlen);
		}
	}
	MemoryContextSwitchTo(old);
	return t;
}

VpqTable *
vpq_table(Relation rel)
{
	Oid			relid = RelationGetRelid(rel);
	TableEntry *e;
	bool		found;

	if (tables == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(Oid);
		ctl.entrysize = sizeof(TableEntry);
		ctl.hcxt = CacheMemoryContext;
		tables = hash_create("vexec_parquet tables", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	e = hash_search(tables, &relid, HASH_FIND, &found);
	if (found)
	{
		VpqTable   *t = e->table;

		/* a relation of another storage or shape by now: a rewrite's, an ALTER's */
		if (t->storage.relnumber == rel->rd_locator.relNumber && t->storage.spc == rel->rd_locator.spcOid &&
			t->natts == RelationGetNumberOfAttributes(rel))
			return t;
		drop_entry(e);
		hash_search(tables, &relid, HASH_REMOVE, NULL);
	}

	{
		VpqTable   *t = build_table(rel, RelationGetDescr(rel), CacheMemoryContext);

		e = hash_search(tables, &relid, HASH_ENTER, &found);
		e->table = t;
		return t;
	}
}

/*
 * Whether a slot's descriptor reads the relation's rows as the relation's
 * own does: a rewrite's scan reads by the old one (ALTER TABLE ... TYPE)
 */
bool
vpq_desc_matches(Relation rel, TupleDesc desc)
{
	TupleDesc	reldesc = RelationGetDescr(rel);

	if (desc == reldesc)
		return true;
	if (desc->natts != reldesc->natts)
		return false;
	for (int i = 0; i < desc->natts; i++)
	{
		Form_pg_attribute a = TupleDescAttr(desc, i);
		Form_pg_attribute b = TupleDescAttr(reldesc, i);

		if (a->attisdropped != b->attisdropped || a->atttypid != b->atttypid ||
			a->atttypmod != b->atttypmod || a->attcollation != b->attcollation)
			return false;
	}
	return true;
}

/* The relation read by another descriptor's columns, in the caller's memory */
VpqTable *
vpq_table_for_desc(Relation rel, TupleDesc desc)
{
	return build_table(rel, desc, CurrentMemoryContext);
}

/* A Parquet type as SQL would declare it, for vexec_parquet.columns() */
const char *
vpq_type_sql(const vpq_type *t)
{
	switch (t->id)
	{
		case VPQ_T_BOOL:
			return "boolean";
		case VPQ_T_INT8:
		case VPQ_T_INT16:
		case VPQ_T_UINT8:
			return "smallint";
		case VPQ_T_INT32:
		case VPQ_T_UINT16:
			return "integer";
		case VPQ_T_INT64:
		case VPQ_T_UINT32:
			return "bigint";
		case VPQ_T_UINT64:
			return "numeric(20,0)";
		case VPQ_T_FLOAT16:
		case VPQ_T_FLOAT32:
			return "real";
		case VPQ_T_FLOAT64:
			return "double precision";
		case VPQ_T_DATE32:
		case VPQ_T_DATE64:
			return "date";
		case VPQ_T_TIME32:
		case VPQ_T_TIME64:
			return "time";
		case VPQ_T_TIMESTAMP:
			return t->has_tz ? "timestamptz" : "timestamp";
		case VPQ_T_DECIMAL32:
		case VPQ_T_DECIMAL64:
		case VPQ_T_DECIMAL128:
		case VPQ_T_DECIMAL256:
			return t->precision <= 1000 ? psprintf("numeric(%d,%d)", t->precision, Max(t->scale, 0)) : "numeric";
		case VPQ_T_STRING:
		case VPQ_T_LARGE_STRING:
		case VPQ_T_STRING_VIEW:
			return "text";
		case VPQ_T_BINARY:
		case VPQ_T_LARGE_BINARY:
		case VPQ_T_BINARY_VIEW:
			return "bytea";
		case VPQ_T_FIXED_BINARY:
			return t->uuid || t->byte_width == 16 ? (t->uuid ? "uuid" : "bytea") : "bytea";
		case VPQ_T_DICTIONARY:
			{
				vpq_type	v = *t;

				v.id = t->value_id;
				return vpq_type_sql(&v);
			}
		default:
			return NULL;
	}
}
