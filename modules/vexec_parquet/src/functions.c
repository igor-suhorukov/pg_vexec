/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * functions.c
 *	  The extension's SQL functions: a location's columns, as a CREATE
 *	  TABLE would declare them; a table's files; a file's schema and its
 *	  row groups; the process's fetches by TID; the footers it read and the
 *	  shared footer cache's figures; an own table's files' facts checked.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "catalog/pg_authid.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

#include "vexec_parquet.h"

PG_FUNCTION_INFO_V1(vexec_parquet_columns);
PG_FUNCTION_INFO_V1(vexec_parquet_files);
PG_FUNCTION_INFO_V1(vexec_parquet_file_schema);
PG_FUNCTION_INFO_V1(vexec_parquet_file_row_groups);
PG_FUNCTION_INFO_V1(vexec_parquet_fetch_stats);
PG_FUNCTION_INFO_V1(vexec_parquet_read_stats);
PG_FUNCTION_INFO_V1(vexec_parquet_scan_stats);
PG_FUNCTION_INFO_V1(vexec_parquet_verify_facts);

/* Reading a location's files is a superuser's, or pg_read_server_files' */
static void
check_file_access(void)
{
	if (!has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to read a location's files"),
				 errdetail("Only roles with privileges of the \"%s\" role may read them.",
						   "pg_read_server_files")));
}

/* The first file a location names */
static vpq_file *
first_file(const char *location)
{
	vpq_entry  *entries = NULL;
	int64		n = 0;
	int			rc;
	vpq_file   *h;

	vpq_engine_ready();
	rc = vpq_list(location, &entries, &n);
	if (rc != VPQ_OK)
		vpq_engine_error(rc, psprintf("listing \"%s\"", location));
	if (n == 0)
	{
		vpq_list_free(entries, n);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FILE),
				 errmsg("no Parquet file at \"%s\"", location)));
	}
	rc = vpq_file_open(entries[0].path, entries[0].size, entries[0].mtime_ns, &h);
	vpq_list_free(entries, n);
	if (rc != VPQ_OK)
		vpq_engine_error(rc, psprintf("reading the footer of a file of \"%s\"", location));
	return h;
}

/*
 * vexec_parquet.columns(location): the columns of the location's first
 * file, each with the type an external table declares it as
 */
Datum
vexec_parquet_columns(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *location = text_to_cstring(PG_GETARG_TEXT_PP(0));
	vpq_file   *h;

	check_file_access();
	InitMaterializedSRF(fcinfo, 0);
	h = first_file(location);
	PG_TRY();
	{
		for (int i = 0; i < vpq_file_columns(h); i++)
		{
			const char *name;
			int			field_id;
			vpq_type	type;
			bool		nullable;
			const char *sql;
			Datum		values[4];
			bool		nulls[4] = {false, false, false, false};

			(void) vpq_file_column(h, i, &name, &field_id, &type, &nullable);
			sql = vpq_type_sql(&type);
			/* PQ16 (pg_vector_executor.md §3.39): a column of geometries as PostGIS's geometry, where it is here */
			{
				char	   *geo = vpq_geo_type_sql(h, i, name);

				if (geo != NULL)
					sql = geo;
			}
			/* PQ20 (§3.40): a list as an array, a map as hstore or jsonb */
			if (sql == NULL)
				sql = vpq_nested_type_sql(h, i, &type);
			values[0] = CStringGetTextDatum(name);
			if (sql)
				values[1] = CStringGetTextDatum(sql);
			else
				nulls[1] = true;
			values[2] = BoolGetDatum(nullable);
			values[3] = CStringGetTextDatum(type.name);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	PG_FINALLY();
	{
		vpq_file_close(h);
	}
	PG_END_TRY();
	PG_RETURN_VOID();
}

/*
 * vexec_parquet.files(table): the files a scan of it now reads, each with
 * its rows, bytes and row groups
 */
Datum
vexec_parquet_files(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	VpqTable   *t;
	VpqListing *l;

	InitMaterializedSRF(fcinfo, 0);
	rel = relation_open(relid, AccessShareLock);
	if (!vpq_is_vpq_rel(rel))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a vexec_parquet table", RelationGetRelationName(rel))));
	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(rel));
	t = vpq_table(rel);
	if (!t->external)
		vpq_buffer_flush_rel(relid);
	l = vpq_listing(rel, t, GetActiveSnapshot());
	PG_TRY();
	{
		/* a path names the server's directories, or S3's: not every reader's to see */
		bool		paths = has_privs_of_role(GetUserId(), ROLE_PG_READ_SERVER_FILES);

		for (int i = 0; i < l->nfiles; i++)
		{
			VpqFileEntry *f = &l->files[i];
			Datum		values[6];
			bool		nulls[6] = {false, !paths, false, false, false, false};

			values[0] = Int64GetDatum(f->file_id);
			values[1] = paths ? CStringGetTextDatum(f->path) : (Datum) 0;
			values[2] = Int64GetDatum(f->rows);
			values[3] = Int64GetDatum(f->size);
			values[4] = Int32GetDatum(f->nunits);
			values[5] = Int64GetDatum((int64) f->first_rowid);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	PG_FINALLY();
	{
		vpq_listing_release(l);
	}
	PG_END_TRY();
	relation_close(rel, AccessShareLock);
	PG_RETURN_VOID();
}

/* vexec_parquet.file_schema(path): a file's columns as Arrow's reader makes them */
Datum
vexec_parquet_file_schema(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
	vpq_file   *h;
	int			rc;

	check_file_access();
	InitMaterializedSRF(fcinfo, 0);
	vpq_engine_ready();
	rc = vpq_file_open(path, -1, -1, &h);
	if (rc != VPQ_OK)
		vpq_engine_error(rc, psprintf("reading the footer of \"%s\"", path));
	PG_TRY();
	{
		for (int i = 0; i < vpq_file_columns(h); i++)
		{
			const char *name;
			int			field_id;
			vpq_type	type;
			bool		nullable;
			Datum		values[4];
			bool		nulls[4] = {false, false, false, false};

			(void) vpq_file_column(h, i, &name, &field_id, &type, &nullable);
			values[0] = CStringGetTextDatum(name);
			values[1] = Int32GetDatum(field_id);
			values[2] = CStringGetTextDatum(type.name);
			values[3] = BoolGetDatum(nullable);
			if (field_id < 0)
				nulls[1] = true;
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	PG_FINALLY();
	{
		vpq_file_close(h);
	}
	PG_END_TRY();
	PG_RETURN_VOID();
}

/*
 * vexec_parquet.file_row_groups(path): each row group's rows and compressed
 * bytes, and the writer
 */
Datum
vexec_parquet_file_row_groups(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	char	   *path = text_to_cstring(PG_GETARG_TEXT_PP(0));
	vpq_file   *h;
	int			rc;

	check_file_access();
	InitMaterializedSRF(fcinfo, 0);
	vpq_engine_ready();
	rc = vpq_file_open(path, -1, -1, &h);
	if (rc != VPQ_OK)
		vpq_engine_error(rc, psprintf("reading the footer of \"%s\"", path));
	PG_TRY();
	{
		for (int g = 0; g < vpq_file_row_groups(h); g++)
		{
			Datum		values[4];
			bool		nulls[4] = {false, false, false, false};
			int64		bytes = 0;

			for (int c = 0; c < vpq_file_columns(h); c++)
				bytes += vpq_file_column_bytes(h, g, c);
			values[0] = Int32GetDatum(g);
			values[1] = Int64GetDatum(vpq_file_row_group_rows(h, g));
			values[2] = Int64GetDatum(bytes);
			values[3] = CStringGetTextDatum(vpq_file_created_by(h));
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	PG_FINALLY();
	{
		vpq_file_close(h);
	}
	PG_END_TRY();
	PG_RETURN_VOID();
}

/*
 * vexec_parquet.fetch_stats(reset): the process's fetches by TID since it
 * started or since a call that reset them (scan.c): the rows fetched, the
 * columns read for them, the data pages read alone and those of them found
 * by the offset index, the columns a page or chunk kept from a fetch before
 * answered, and the column chunks read whole
 */
Datum
vexec_parquet_fetch_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		values[6];
	bool		nulls[6] = {false, false, false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vexec_parquet.fetch_stats() must return a row");
	values[0] = Int64GetDatum(vpq_fetch_stats.fetches);
	values[1] = Int64GetDatum(vpq_fetch_stats.values);
	values[2] = Int64GetDatum(vpq_fetch_stats.pages);
	values[3] = Int64GetDatum(vpq_fetch_stats.indexed);
	values[4] = Int64GetDatum(vpq_fetch_stats.kept);
	values[5] = Int64GetDatum(vpq_fetch_stats.whole);
	if (PG_GETARG_BOOL(0))
		memset(&vpq_fetch_stats, 0, sizeof(vpq_fetch_stats));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

/*
 * vexec_parquet.read_stats(reset): what the process read to plan and scan
 * since it started or since a call that reset them -- footers read from
 * files, handles opened of files' facts without a footer, those of them
 * whose footer was read since, own tables' listings read from the catalog,
 * and files' columns' statistics decoded -- and the shared footer cache's
 * entries and bytes now, its finds, misses and entries kept since the
 * server started (footers.c)
 */
Datum
vexec_parquet_read_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		values[11];
	bool		nulls[11] = {0};
	vpq_engine_stats es;
	int64		entries;
	int64		bytes;
	int64		hits;
	int64		misses;
	int64		kept;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vexec_parquet.read_stats() must return a row");
	vpq_engine_stats_get(&es, PG_GETARG_BOOL(0));
	vpq_footers_stats(&entries, &bytes, &hits, &misses, &kept);
	values[0] = Int64GetDatum(es.footers_read);
	values[1] = Int64GetDatum(es.facts_opened);
	values[2] = Int64GetDatum(es.footers_late);
	values[3] = Int64GetDatum(vpq_listing_stats.listings);
	values[4] = Int64GetDatum(vpq_listing_stats.stat_columns);
	values[5] = Int64GetDatum(entries);
	values[6] = Int64GetDatum(bytes);
	values[7] = Int64GetDatum(hits);
	values[8] = Int64GetDatum(misses);
	values[9] = Int64GetDatum(kept);
	values[10] = Int64GetDatum(es.versions_read);
	if (PG_GETARG_BOOL(0))
		memset(&vpq_listing_stats, 0, sizeof(vpq_listing_stats));
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

/*
 * vexec_parquet.scan_stats(reset): the process's scans' reads since it
 * started or since a call that reset them (pg_vector_executor.md §3.37) --
 * the engine's pool's large blocks (PQ8), text checked at once or a value at
 * a time (PQ9), batches of text found UTF-8 whole before and chunks marked
 * so (PQ14), readers over footers of their columns (PQ11), units cut
 * into ranges of rows (PQ12), batches whose later steps were read by pages
 * and the pages range reads decoded and passed over (PQ12, PQ13) -- the
 * pool's bytes kept now
 */
Datum
vexec_parquet_scan_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		values[14];
	bool		nulls[14] = {0};
	vpq_engine_stats es;

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "vexec_parquet.scan_stats() must return a row");
	vpq_engine_stats_get(&es, false);
	values[0] = Int64GetDatum(es.pool_mapped);
	values[1] = Int64GetDatum(es.pool_reused);
	values[2] = Int64GetDatum(es.pool_moved);
	values[3] = Int64GetDatum(es.pool_kept_bytes);
	values[4] = Int64GetDatum(vpq_scan_stats.text_batches);
	values[5] = Int64GetDatum(vpq_scan_stats.text_values);
	values[6] = Int64GetDatum(vpq_scan_stats.text_known);
	values[7] = Int64GetDatum(vpq_scan_stats.text_marked);
	values[8] = Int64GetDatum(es.footers_projected);
	values[9] = Int64GetDatum(es.projections_refused);
	values[10] = Int64GetDatum(vpq_scan_stats.units_cut);
	values[11] = Int64GetDatum(vpq_scan_stats.step_batches);
	values[12] = Int64GetDatum(es.pages_decoded);
	values[13] = Int64GetDatum(es.pages_passed);
	if (PG_GETARG_BOOL(0))
	{
		vpq_engine_stats_reset_scans();
		memset(&vpq_scan_stats, 0, sizeof(vpq_scan_stats));
	}
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

/*
 * vexec_parquet.verify_facts(table): an own table's files, each with
 * whether its catalog row keeps its facts, whether they are the facts of
 * its footer read again from the file, and whether every column chunk's
 * first data page is of version 1, as its header says -- what the facts say
 * of every file the extension writes
 */
Datum
vexec_parquet_verify_facts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid			relid = PG_GETARG_OID(0);
	Relation	rel;
	VpqTable   *t;
	VpqListing *l;

	check_file_access();
	InitMaterializedSRF(fcinfo, 0);
	rel = relation_open(relid, AccessShareLock);
	if (!vpq_is_vpq_rel(rel) || vpq_table(rel)->external)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a vexec_parquet table of its own files", RelationGetRelationName(rel))));
	t = vpq_table(rel);
	vpq_buffer_flush_rel(relid);
	l = vpq_listing(rel, t, GetActiveSnapshot());
	vpq_engine_ready();
	PG_TRY();
	{
		for (int i = 0; i < l->nfiles; i++)
		{
			VpqFileEntry *f = &l->files[i];
			Datum		values[4];
			bool		nulls[4] = {false, false, false, false};
			vpq_file   *h;
			char	   *facts;
			size_t		len;
			bool		v1 = true;
			int			rc;

			/* its size and time from the filesystem: a key no footer the writer kept has, so read again */
			rc = vpq_file_open(f->path, -1, -1, &h);
			if (rc != VPQ_OK)
				vpq_engine_error(rc, psprintf("opening \"%s\"", f->path));
			rc = vpq_file_facts(h, VPQ_FACTS_PAGES_V1, &facts, &len);
			if (rc != VPQ_OK)
			{
				vpq_file_close(h);
				vpq_engine_error(rc, psprintf("the facts of \"%s\"", f->path));
			}
			for (int g = 0; g < vpq_file_row_groups(h); g++)
				for (int c = 0; c < vpq_file_columns(h); c++)
				{
					const char *name;
					int			field_id;
					vpq_type	type;
					bool		nullable;

					if (vpq_file_column(h, c, &name, &field_id, &type, &nullable) &&
						vpq_file_page_version(h, g, c) != 1)
						v1 = false;
				}
			values[0] = Int64GetDatum(f->file_id);
			values[1] = BoolGetDatum(f->facts != NULL);
			values[2] = BoolGetDatum(f->facts != NULL && f->facts_len == len && memcmp(f->facts, facts, len) == 0);
			values[3] = BoolGetDatum(v1);
			free(facts);
			vpq_file_close(h);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
		}
	}
	PG_FINALLY();
	{
		vpq_listing_release(l);
	}
	PG_END_TRY();
	relation_close(rel, AccessShareLock);
	PG_RETURN_VOID();
}
