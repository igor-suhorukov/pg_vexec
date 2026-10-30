/* SPDX-License-Identifier: Apache-2.0 */
/* vexec_parquet--1.0.sql: the access method, its catalogs, its functions */

\echo Use "CREATE EXTENSION vexec_parquet" to load this file. \quit

CREATE FUNCTION vexec_parquet.am_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'vexec_parquet_am_handler'
LANGUAGE C;

CREATE ACCESS METHOD vexec_parquet TYPE TABLE HANDLER vexec_parquet.am_handler;
COMMENT ON ACCESS METHOD vexec_parquet IS 'Parquet files read and written by Apache Arrow C++, on the local filesystem or S3';

/*
 * The catalogs of tables of their own files -- heap's, whatever
 * default_table_access_method names -- each keyed by a storage -- a
 * relfilenumber's incarnation, its tablespace, and a temporary table's
 * backend -- with an UNLOGGED twin for UNLOGGED and temporary tables, whose
 * rows a crash takes with it.  Written by the access method alone.
 */
CREATE TABLE vexec_parquet.storages (
	spc oid NOT NULL,
	relnumber oid NOT NULL,
	backend int4 NOT NULL,
	next_rowid int8 NOT NULL,		-- moves in place, never back
	next_file int8 NOT NULL
) USING heap;
CREATE UNIQUE INDEX storages_key ON vexec_parquet.storages (spc, relnumber, backend);

CREATE TABLE vexec_parquet.files (
	spc oid NOT NULL,
	relnumber oid NOT NULL,
	backend int4 NOT NULL,
	file_id int8 NOT NULL,
	first_rowid int8 NOT NULL,		-- its rows' ids: [first_rowid, first_rowid + rows)
	rows int8 NOT NULL,
	bytes int8 NOT NULL,
	fxid int8 NOT NULL,				-- its writer's FullTransactionId, in its name
	stats bytea						-- its row groups' rows and columns' statistics
) USING heap;
CREATE INDEX files_key ON vexec_parquet.files (spc, relnumber, backend, first_rowid);

CREATE TABLE vexec_parquet.tombstones (
	spc oid NOT NULL,
	relnumber oid NOT NULL,
	backend int4 NOT NULL,
	rowid int8 NOT NULL,
	kind "char" NOT NULL,			-- d deleted, u updated, l locked
	successor int8					-- an update's new row id
) USING heap;
CREATE INDEX tombstones_key ON vexec_parquet.tombstones (spc, relnumber, backend, rowid);

CREATE UNLOGGED TABLE vexec_parquet.storages_unlogged (LIKE vexec_parquet.storages) USING heap;
CREATE UNIQUE INDEX storages_unlogged_key ON vexec_parquet.storages_unlogged (spc, relnumber, backend);
CREATE UNLOGGED TABLE vexec_parquet.files_unlogged (LIKE vexec_parquet.files) USING heap;
CREATE INDEX files_unlogged_key ON vexec_parquet.files_unlogged (spc, relnumber, backend, first_rowid);
CREATE UNLOGGED TABLE vexec_parquet.tombstones_unlogged (LIKE vexec_parquet.tombstones) USING heap;
CREATE INDEX tombstones_unlogged_key ON vexec_parquet.tombstones_unlogged (spc, relnumber, backend, rowid);

REVOKE ALL ON vexec_parquet.storages, vexec_parquet.files, vexec_parquet.tombstones,
	vexec_parquet.storages_unlogged, vexec_parquet.files_unlogged, vexec_parquet.tombstones_unlogged
	FROM PUBLIC;

/* The columns of a location's first file, as an external table declares them */
CREATE FUNCTION vexec_parquet.columns(location text,
	OUT name text, OUT type text, OUT nullable boolean, OUT arrow_type text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_parquet_columns'
LANGUAGE C STRICT VOLATILE;

/* The same as a column list for CREATE TABLE */
CREATE FUNCTION vexec_parquet.column_list(location text)
RETURNS text
LANGUAGE sql STRICT VOLATILE
BEGIN ATOMIC
	SELECT string_agg(format('%I %s', c.name, coalesce(c.type, 'text')), ', ' ORDER BY c.ord)
	FROM vexec_parquet.columns(location) WITH ORDINALITY AS c(name, type, nullable, arrow_type, ord);
END;

/* The files a scan of a table now reads; their paths for those who may read the server's files */
CREATE FUNCTION vexec_parquet.files(rel regclass,
	OUT file_id int8, OUT path text, OUT rows int8, OUT bytes int8, OUT row_groups int4,
	OUT first_rowid int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_parquet_files'
LANGUAGE C STRICT VOLATILE;

/* A file's columns as Arrow's reader makes them */
CREATE FUNCTION vexec_parquet.file_schema(path text,
	OUT name text, OUT field_id int4, OUT arrow_type text, OUT nullable boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_parquet_file_schema'
LANGUAGE C STRICT VOLATILE;

/* A file's row groups: rows, compressed bytes, and its writer */
CREATE FUNCTION vexec_parquet.file_row_groups(path text,
	OUT row_group int4, OUT rows int8, OUT bytes int8, OUT created_by text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_parquet_file_row_groups'
LANGUAGE C STRICT VOLATILE;

/*
 * The process's fetches by TID (scan.c): rows, the columns read for them,
 * data pages read alone and those found by the offset index, columns a kept
 * page or chunk answered, column chunks read whole; reset where asked
 */
CREATE FUNCTION vexec_parquet.fetch_stats(reset bool DEFAULT false,
	OUT fetches int8, OUT "values" int8, OUT pages int8, OUT indexed int8, OUT kept int8, OUT whole int8)
RETURNS record
AS 'MODULE_PATHNAME', 'vexec_parquet_fetch_stats'
LANGUAGE C STRICT VOLATILE;

/*
 * What the process read to plan and scan (reset where asked): footers read
 * from files, handles of files opened of their facts without a footer, those
 * of them whose footer was read since, own tables' listings read from the
 * catalog, files' columns' statistics decoded; the shared footer cache's
 * entries and bytes now, its finds, misses and entries kept since the
 * server started; and data pages' headers read for their versions, which
 * the shared facts keep since (PQ7)
 */
CREATE FUNCTION vexec_parquet.read_stats(reset bool DEFAULT false,
	OUT footers_read int8, OUT facts_opened int8, OUT footers_late int8, OUT listings int8,
	OUT stat_columns int8, OUT cache_entries int8, OUT cache_bytes int8, OUT cache_hits int8,
	OUT cache_misses int8, OUT cache_kept int8, OUT versions_read int8)
RETURNS record
AS 'MODULE_PATHNAME', 'vexec_parquet_read_stats'
LANGUAGE C STRICT VOLATILE;

/*
 * The process's scans' reads (pg_vector_executor.md §3.37; reset where
 * asked): Arrow's large blocks mapped afresh, taken from those kept, moved by
 * mremap(), and the bytes kept now (PQ8); batches of external text checked
 * at once and values checked one by one (PQ9); batches of external text
 * found UTF-8 whole by a reader before, their check passed over, and column
 * chunks marked so in the shared footer cache (PQ14); CPU readers opened over a
 * footer of their columns, and those whose types differed, opened over the
 * footer whole (PQ11); units read as a range of a row group's rows (PQ12);
 * batches whose later steps' columns were decoded by their pages, and the
 * data pages range reads decoded and passed over (PQ12, PQ13)
 */
CREATE FUNCTION vexec_parquet.scan_stats(reset bool DEFAULT false,
	OUT pool_mapped int8, OUT pool_reused int8, OUT pool_moved int8, OUT pool_kept_bytes int8,
	OUT text_batches int8, OUT text_values int8, OUT text_known int8, OUT text_marked int8,
	OUT footers_projected int8, OUT projections_refused int8,
	OUT units_cut int8, OUT step_batches int8, OUT pages_decoded int8, OUT pages_passed int8)
RETURNS record
AS 'MODULE_PATHNAME', 'vexec_parquet_scan_stats'
LANGUAGE C STRICT VOLATILE;

/*
 * An own table's files: whether the catalog keeps each one's facts, whether
 * they are its footer's read again, and whether every column chunk's first
 * data page is of version 1
 */
CREATE FUNCTION vexec_parquet.verify_facts(rel regclass,
	OUT file_id int8, OUT facts boolean, OUT same boolean, OUT pages_v1 boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_parquet_verify_facts'
LANGUAGE C STRICT VOLATILE;

REVOKE ALL ON FUNCTION vexec_parquet.columns(text), vexec_parquet.column_list(text),
	vexec_parquet.file_schema(text), vexec_parquet.file_row_groups(text),
	vexec_parquet.verify_facts(regclass) FROM PUBLIC;

/*
 * Anyone may name the schema's objects: files() answers for a table its
 * caller may read, its paths shown to those who may read the server's files
 */
GRANT USAGE ON SCHEMA vexec_parquet TO PUBLIC;
