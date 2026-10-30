-- Tables that read the files of another writer, pyarrow's
-- (test/data/make_files.py, written into VPQ_TEST_DATA before the test)
\getenv data VPQ_TEST_DATA
SELECT :'data' || '/all.parquet' AS all_file, :'data' || '/part-*.parquet' AS parts,
	:'data' || '/nested.parquet' AS nested, :'data' || '/paged.parquet' AS paged \gset
-- the columns as a table would declare them
SELECT name, type, nullable, arrow_type FROM vexec_parquet.columns(:'all_file');
SELECT vexec_parquet.column_list(:'all_file') AS columns \gset
CREATE TABLE arrow_all (:columns) USING vexec_parquet WITH (location = :'all_file');
SELECT count(*) FROM arrow_all;
SELECT * FROM arrow_all WHERE id IN (0, 1, 3, 5, 999) ORDER BY id;
SELECT count(*), sum(i8), sum(i16), sum(i32), sum(u8), sum(u16), sum(u32), sum(u64), sum(f16), sum(f32),
	count(b) FILTER (WHERE b), count(DISTINCT dict), min(s), max(ls), sum(length(bin)) FROM arrow_all;
SELECT min(d32), max(d64), min(ts_s), max(ts_ms), min(ts_us), max(ts_ns), min(tz_us), max(t32), min(t64)
FROM arrow_all;
SELECT sum(dec9), sum(dec18), sum(dec38), count(uu), count(nul) FROM arrow_all;
SELECT row_group, rows FROM vexec_parquet.file_row_groups(:'all_file');
-- the same rows in three files: other codecs, another order of columns
CREATE TABLE arrow_parts (:columns) USING vexec_parquet WITH (location = :'parts');
SELECT count(*) AS differ FROM ((TABLE arrow_all EXCEPT ALL TABLE arrow_parts)
	UNION ALL (TABLE arrow_parts EXCEPT ALL TABLE arrow_all)) x;
-- through vexec's source, in both formats
SET vexec.mode = force;
SELECT count(*) AS differ FROM ((TABLE arrow_all EXCEPT ALL TABLE arrow_parts)
	UNION ALL (TABLE arrow_parts EXCEPT ALL TABLE arrow_all)) x;
SELECT count(*), sum(u32), sum(f16), min(ts_ns), max(dec38), count(DISTINCT dict) FROM arrow_parts;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
	SELECT count(*), sum(i32) FROM arrow_all WHERE id BETWEEN 600 AND 610;
SELECT count(*), sum(i32) FROM arrow_all WHERE id BETWEEN 600 AND 610;
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*), min(id), max(ts_us) FROM arrow_all;
SELECT count(*), min(id), max(ts_us) FROM arrow_all;
SET vexec.batch_format = arrow;
SELECT count(*), sum(u64), max(s), min(d64), max(t32) FROM arrow_parts;
RESET vexec.batch_format;
RESET vexec.mode;
-- columns read as other types: integers, half and single floats as
-- doubles, every unit of time, decimals as numeric, a dictionary as text
CREATE TABLE arrow_cast (id int4, u8 int8, f16 float8, f32 float8, s varchar(9), ts_ns timestamptz,
	t32 time, dec9 numeric, u64 numeric, dict varchar) USING vexec_parquet WITH (location = :'all_file');
SELECT * FROM arrow_cast WHERE id < 3 ORDER BY id;
-- a type no file's column is read as
SELECT vpq_error(format('CREATE TABLE arrow_bad (dec9 float8) USING vexec_parquet WITH (location = %L)', :'all_file'));
-- an unsigned value past the column's type, and a nested column as text; columns() declares it an array (PQ20)
CREATE TABLE arrow_narrow (u32 int2) USING vexec_parquet WITH (location = :'all_file');
SELECT max(u32) FROM arrow_narrow;
SELECT vpq_error(format('CREATE TABLE arrow_nested (id int8, l text) USING vexec_parquet WITH (location = %L)', :'nested'));
SELECT * FROM vexec_parquet.columns(:'nested');
-- rows fetched by their TIDs (a TID scan; scan.c's point reads): each
-- column's data page holding the row read alone, walked to by the pages'
-- headers where the file has no offset index (pyarrow's default), found by
-- it where it has one; a type a point read does not make -- a half float, a
-- dictionary, a date64, seconds and nanoseconds -- read from its chunk whole
CREATE TABLE arrow_paged (:columns) USING vexec_parquet WITH (location = :'paged');
SELECT vexec_parquet.fetch_stats(true) IS NOT NULL AS reset;
SET enable_seqscan = off;
CREATE TEMP TABLE fetched_all AS SELECT * FROM arrow_all
WHERE ctid = ANY (ARRAY(SELECT ctid FROM arrow_all WHERE id % 37 = 0 OR id IN (0, 249, 250, 999)));
RESET enable_seqscan;
SELECT fetches, pages > 0 AS pages_read, indexed AS by_offset_index, whole > 0 AS some_read_whole
FROM vexec_parquet.fetch_stats();
SELECT count(*) AS rows, (SELECT count(*) FROM ((TABLE fetched_all EXCEPT ALL
	(SELECT * FROM arrow_all WHERE id % 37 = 0 OR id IN (0, 249, 250, 999))) UNION ALL
	((SELECT * FROM arrow_all WHERE id % 37 = 0 OR id IN (0, 249, 250, 999)) EXCEPT ALL TABLE fetched_all)) x) AS differ
FROM fetched_all;
SELECT vexec_parquet.fetch_stats(true) IS NOT NULL AS reset;
SET enable_seqscan = off;
CREATE TEMP TABLE fetched_paged AS SELECT * FROM arrow_paged
WHERE ctid = ANY (ARRAY(SELECT ctid FROM arrow_paged WHERE id % 37 = 0 OR id IN (0, 399, 400, 999)));
RESET enable_seqscan;
SELECT fetches, pages > 0 AS pages_read, indexed = pages AS by_offset_index, whole > 0 AS some_read_whole
FROM vexec_parquet.fetch_stats();
SELECT count(*) AS rows, (SELECT count(*) FROM ((TABLE fetched_paged EXCEPT ALL
	(SELECT * FROM arrow_all WHERE id % 37 = 0 OR id IN (0, 399, 400, 999))) UNION ALL
	((SELECT * FROM arrow_all WHERE id % 37 = 0 OR id IN (0, 399, 400, 999)) EXCEPT ALL TABLE fetched_paged)) x) AS differ
FROM fetched_paged;
