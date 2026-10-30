-- Tables that read files: a file, a directory, a glob; columns by name;
-- the files a statement finds as it starts.  The files here are the
-- access method's own, written by tables of their own files; those of
-- another writer are external_arrow's.
CREATE TABLE w1 (id int8, name text, day date, amount numeric(12,2), flag bool) USING vexec_parquet
	WITH (row_group_rows = 1024);
INSERT INTO w1 SELECT i, 'name ' || i, date '2024-01-01' + (i % 366), i * 1.25, i % 2 = 0
FROM generate_series(1, 3000) i;
INSERT INTO w1 SELECT i, NULL, NULL, NULL, NULL FROM generate_series(3001, 3100) i;
SELECT regexp_replace(min(path), '/[^/]+$', '') AS dir, min(path) AS first_file FROM vexec_parquet.files('w1') \gset
SELECT count(*) AS files FROM vexec_parquet.files('w1');

-- a directory: every *.parquet under it
CREATE TABLE e_dir (id int8, name text, day date, amount numeric(12,2), flag bool) USING vexec_parquet
	WITH (location = :'dir');
SELECT count(*), sum(id), count(name), min(day), max(amount), count(*) FILTER (WHERE flag) FROM e_dir;
SELECT count(*) AS differ FROM ((TABLE w1 EXCEPT ALL TABLE e_dir) UNION ALL (TABLE e_dir EXCEPT ALL TABLE w1)) x;
-- a file
CREATE TABLE e_file (id int8, name text) USING vexec_parquet WITH (location = :'first_file');
SELECT count(*), min(id), max(id) FROM e_file;
-- a glob
SELECT :'dir' || '/*.parquet' AS glob \gset
CREATE TABLE e_glob (id int8) USING vexec_parquet WITH (location = :'glob');
SELECT count(*) FROM e_glob;
-- some of the files' columns, in another order, named in other cases
CREATE TABLE e_some ("FLAG" bool, "Id" int8) USING vexec_parquet WITH (location = :'dir');
SELECT "FLAG", count(*), min("Id") FROM e_some GROUP BY 1 ORDER BY 1;
-- columns of other types: integers narrowed, decimals rescaled, strings
-- under a typmod; a type no file's column is read as
CREATE TABLE e_types (id int4, name varchar(9), amount numeric(14,4)) USING vexec_parquet
	WITH (location = :'dir');
SELECT * FROM e_types WHERE id IN (1, 2, 3001) ORDER BY id;
CREATE TABLE e_short (name varchar(6)) USING vexec_parquet WITH (location = :'dir');
SELECT count(*) FROM e_short;
SELECT vpq_error(format('CREATE TABLE e_bad (day timestamp) USING vexec_parquet WITH (location = %L)', :'dir'));
-- NOT NULL is checked as each row is read
CREATE TABLE e_notnull (id int8, name text NOT NULL) USING vexec_parquet WITH (location = :'dir');
SELECT count(*) FROM e_notnull;
CREATE TABLE e_notnull_file (id int8, name text NOT NULL) USING vexec_parquet WITH (location = :'first_file');
SELECT count(*) FROM e_notnull_file;

-- statistics and plans over a table that reads files
ANALYZE e_dir;
SELECT reltuples FROM pg_class WHERE relname = 'e_dir';
SELECT attname, n_distinct FROM pg_stats WHERE tablename = 'e_dir' AND attname IN ('flag', 'id') ORDER BY 1;
-- a join of both kinds of tables
CREATE TABLE names (id int8, nick text) USING vexec_parquet;
INSERT INTO names SELECT i, 'n' || i FROM generate_series(1, 100, 10) i;
SELECT n.nick, e.name FROM names n JOIN e_dir e USING (id) ORDER BY id;
-- the files' metadata
SELECT s.name, s.field_id, s.arrow_type, s.nullable FROM vexec_parquet.file_schema(:'first_file') s ORDER BY 2;
SELECT row_group, rows, created_by ~ '^parquet-cpp-arrow' AS arrow_wrote FROM vexec_parquet.file_row_groups(:'first_file');
SELECT file_id, rows, row_groups FROM vexec_parquet.files('e_dir') ORDER BY 1;
SELECT * FROM vexec_parquet.file_schema('/nonexistent.parquet');

-- a statement reads the files there as it starts: a new one; then the
-- directory gone with its table's storage, which TRUNCATE replaces
INSERT INTO w1 VALUES (5000, 'later', NULL, NULL, NULL);
SELECT count(*) FROM e_dir;
TRUNCATE w1;
SELECT vpq_error('SELECT count(*) FROM e_dir');
