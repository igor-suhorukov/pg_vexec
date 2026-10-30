-- A table's options: its codec, its row groups, its storage; who may name
-- files; tables moved between access methods; what COPY TO does with a
-- table that reads files
CREATE TABLE opt (a int4, b text) USING vexec_parquet
	WITH (codec = 'lz4', compression_level = 1, row_group_rows = 2048);
SELECT reloptions FROM pg_class WHERE relname = 'opt';
INSERT INTO opt SELECT i, md5(i::text) FROM generate_series(1, 5000) i;
SELECT g.row_group, g.rows FROM vexec_parquet.files('opt') f, vexec_parquet.file_row_groups(f.path) g ORDER BY 1;
ALTER TABLE opt SET (row_group_rows = 10000, codec = 'snappy');
INSERT INTO opt SELECT i, md5(i::text) FROM generate_series(5001, 8000) i;
SELECT f.file_id, g.row_group, g.rows FROM vexec_parquet.files('opt') f, vexec_parquet.file_row_groups(f.path) g ORDER BY 1, 2;
ALTER TABLE opt RESET (codec, compression_level);
SELECT reloptions FROM pg_class WHERE relname = 'opt';
-- options refused
CREATE TABLE bad (a int4) USING vexec_parquet WITH (codec = 'lz5');
CREATE TABLE bad (a int4) USING vexec_parquet WITH (row_group_rows = 0);
CREATE TABLE bad (a int4) USING vexec_parquet WITH (no_such_option = 1);
CREATE TABLE bad (a int4) USING vexec_parquet WITH (location = '/nonexistent/vpq', storage = '/tmp');
-- heap's options in heap's tables, as before
CREATE TABLE heapopt (a int4) USING heap WITH (fillfactor = 50);
SELECT reloptions FROM pg_class WHERE relname = 'heapopt';
CREATE TABLE heapbad (a int4) USING heap WITH (codec = 'zstd');
-- the default access method's tables take the options too
SET default_table_access_method = vexec_parquet;
CREATE TABLE dflt (a int4) WITH (codec = 'gzip');
SELECT a.amname, c.reloptions FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE c.relname = 'dflt';
RESET default_table_access_method;

-- a storage of the table's own: a root of its own, a directory under it per table
SELECT current_setting('data_directory') || '/vpq_root' AS root \gset
CREATE TABLE stored (a int4) USING vexec_parquet WITH (storage = :'root');
INSERT INTO stored SELECT generate_series(1, 10);
SELECT count(*), sum(a) FROM stored;
SELECT regexp_replace(path, '^.*/vpq_root/[0-9]+/[0-9]+/[0-9]+_[0-9]+/([0-9]+)_[0-9]+[.]parquet$',
	'<root>/<system>/<db>/<spc>_<relnumber>/\1_<fxid>.parquet') AS path
FROM vexec_parquet.files('stored');
-- a storage changes only while the table has no file
ALTER TABLE stored SET (storage = '/tmp');
TRUNCATE stored;
ALTER TABLE opt RESET (storage);

-- naming files is for those who may read the server's files
SET ROLE regress_vpq_user;
CREATE TABLE mine (a int4) USING vexec_parquet;
INSERT INTO mine VALUES (1);
SELECT * FROM mine;
-- its files, without their paths
SELECT file_id, path, rows FROM vexec_parquet.files('mine');
SELECT file_id FROM vexec_parquet.files('opt');
CREATE TABLE theirs (a int4) USING vexec_parquet WITH (location = '/etc');
CREATE TABLE theirs (a int4) USING vexec_parquet WITH (storage = '/tmp');
SELECT * FROM vexec_parquet.columns('/etc');
SELECT * FROM vexec_parquet.file_schema('/etc/passwd');
-- the catalogs are the access method's
SELECT count(*) FROM vexec_parquet.files;
RESET ROLE;
DROP TABLE mine;

-- tables moved between access methods keep their rows
CREATE TABLE moved (a int4, b text) USING heap;
INSERT INTO moved SELECT i, 'm' || i FROM generate_series(1, 100) i;
ALTER TABLE moved SET ACCESS METHOD vexec_parquet;
SELECT a.amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE c.relname = 'moved';
SELECT count(*), sum(a), min(b) FROM moved;
ALTER TABLE moved SET (codec = 'none');
ALTER TABLE moved SET ACCESS METHOD heap;
SELECT a.amname, c.reloptions FROM pg_class c JOIN pg_am a ON a.oid = c.relam WHERE c.relname = 'moved';
SELECT count(*), sum(a), min(b) FROM moved;

-- a table that reads files: none to write, COPY TO answers with no rows
CREATE TABLE src (a int4, b text) USING vexec_parquet;
INSERT INTO src VALUES (1, 'x'), (2, 'y');
SELECT regexp_replace(path, '/[^/]+$', '') AS dir FROM vexec_parquet.files('src') LIMIT 1 \gset
CREATE TABLE ext (a int4, b text) USING vexec_parquet WITH (location = :'dir');
SELECT * FROM ext ORDER BY a;
COPY ext TO stdout;
COPY (SELECT * FROM ext ORDER BY a) TO stdout;
INSERT INTO ext VALUES (3, 'z');
UPDATE ext SET b = 'q';
DELETE FROM ext;
TRUNCATE ext;
SELECT count(*) FROM ext;
-- its location is set again, its files checked as CREATE TABLE checks
-- them, never reset
ALTER TABLE ext SET (location = '/nonexistent/vpq');
SELECT vpq_error(format('ALTER TABLE ext SET (location = %L)', :'dir' || '/../..'));
ALTER TABLE ext SET (location = :'dir');
SELECT count(*) FROM ext;
ALTER TABLE ext RESET (location);
-- nor is it made with rows, or made a partition
CREATE TABLE ext2 USING vexec_parquet WITH (location = :'dir') AS SELECT 1 AS a;
CREATE TABLE ext2 USING vexec_parquet WITH (location = :'dir') AS SELECT 1::int4 AS a, 'x'::text AS b WITH NO DATA;
SELECT count(*) FROM ext2;
CREATE INDEX ON ext (a);
CREATE TABLE p (a int4, b text) PARTITION BY LIST (a);
ALTER TABLE p ATTACH PARTITION ext FOR VALUES IN (1);
-- a location's files' columns, as CREATE TABLE would name them
SELECT * FROM vexec_parquet.columns(:'dir');
SELECT vexec_parquet.column_list(:'dir');
-- a location of no file, and a column the files have not
CREATE TABLE ext3 (a int4) USING vexec_parquet WITH (location = '/nonexistent/vpq');
SELECT vpq_error(format('CREATE TABLE ext3 (a int4, c int4) USING vexec_parquet WITH (location = %L)', :'dir'));
SELECT vpq_error(format('CREATE TABLE ext3 (a text, b text) USING vexec_parquet WITH (location = %L)', :'dir'));
