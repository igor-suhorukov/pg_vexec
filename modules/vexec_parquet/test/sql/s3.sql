-- Tables of their own files on S3, and tables that read files there: an
-- S3 server on the loopback, moto's, its bucket vpq-test made before
-- (test/vexec/parquet.sh)
SET vexec_parquet.s3_endpoint = 'http://127.0.0.1:9000';
SET vexec_parquet.s3_access_key_id = 'test';
SET vexec_parquet.s3_secret_access_key = 'test';
SET vexec_parquet.s3_region = 'us-east-1';
CREATE TABLE s3t (id int8, name text, v numeric(10,3)) USING vexec_parquet
	WITH (storage = 's3://vpq-test/root', codec = 'lz4', row_group_rows = 10000);
INSERT INTO s3t SELECT g, 'n' || g, g / 7.0 FROM generate_series(1, 50000) g;
SELECT count(*), sum(v), min(name), max(id) FROM s3t;
SELECT regexp_replace(path, '^s3://vpq-test/root/[0-9]+/[0-9]+/[0-9]+_[0-9]+/([0-9]+)_[0-9]+[.]parquet$',
	's3://vpq-test/root/<system>/<db>/<spc>_<relnumber>/\1_<fxid>.parquet') AS path,
	rows, row_groups FROM vexec_parquet.files('s3t');
-- writes, savepoints, an index
BEGIN;
UPDATE s3t SET name = 'updated' WHERE id <= 10;
SAVEPOINT a;
DELETE FROM s3t WHERE id > 40000;
INSERT INTO s3t VALUES (60000, 'in a savepoint', 1);
ROLLBACK TO SAVEPOINT a;
DELETE FROM s3t WHERE id % 2 = 0;
COMMIT;
SELECT count(*), count(*) FILTER (WHERE name = 'updated'), max(id) FROM s3t;
CREATE INDEX s3t_id ON s3t (id);
SET enable_seqscan = off;
SELECT id, name FROM s3t WHERE id IN (5, 6, 7) ORDER BY id;
RESET enable_seqscan;
-- vexec's source over S3's files: row groups passed over
SET vexec.mode = force;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
	SELECT count(*), sum(v) FROM s3t WHERE id BETWEEN 15000 AND 15100;
SELECT count(*), sum(v) FROM s3t WHERE id BETWEEN 15000 AND 15100;
SET vexec.batch_format = arrow;
SELECT count(*), sum(v), min(name) FROM s3t;
RESET vexec.batch_format;
RESET vexec.mode;
-- a table that reads the files under a prefix, and one that names its
-- endpoint in its URI instead of the settings
SELECT regexp_replace(min(path), '/[^/]+$', '') AS dir FROM vexec_parquet.files('s3t') \gset
CREATE TABLE s3ext (id int8, v numeric(10,3)) USING vexec_parquet WITH (location = :'dir');
SELECT count(*), sum(v) FROM s3ext;
RESET vexec_parquet.s3_endpoint;
SELECT :'dir' || '?endpoint_override=127.0.0.1:9000&scheme=http' AS dir_uri \gset
CREATE TABLE s3ext_uri (id int8) USING vexec_parquet WITH (location = :'dir_uri');
SELECT count(*) FROM s3ext_uri;
SET vexec_parquet.s3_endpoint = 'http://127.0.0.1:9000';
-- credentials in a URI are refused; a bucket that is not there is no table's
CREATE TABLE s3bad (id int8) USING vexec_parquet WITH (location = 's3://key:secret@vpq-test/root');
CREATE TABLE s3bad (id int8) USING vexec_parquet WITH (location = 's3://vpq-no-such-bucket/x');
CREATE TABLE s3bad (id int8) USING vexec_parquet WITH (location = 's3://vpq-test/root?colour=blue');
-- a glob under a prefix
SELECT :'dir' || '/*.parquet' AS glob \gset
CREATE TABLE s3glob (id int8) USING vexec_parquet WITH (location = :'glob');
SELECT count(*) FROM s3glob;
-- TRUNCATE and DROP take the files at commit
TRUNCATE s3t;
SELECT count(*) FROM s3t;
SELECT vpq_error('SELECT count(*) FROM s3ext');
-- a glob that matches no file: no rows
DROP TABLE s3t;
SELECT count(*) FROM s3glob;
