-- SPDX-License-Identifier: Apache-2.0
--
-- The ingest stream (pg_vector_executor.md §3.16; §5 VI; vexec_egress.h's
-- minor 1): a client's Arrow, as vexec_flight hands it to vexec, read by
-- vexec.ingest_stream(handle) -- a VecIngest in vector plans, which maps
-- the stream's buffers onto batches where the declared type is the Arrow
-- type's own and converts a value at a time elsewhere.  vexec_test's
-- ingest_begin() opens a stream over a query's IPC messages, which
-- vexec_test.egress() writes, as a Flight client's DoPut would bring them.
-- A stream lives as long as its transaction, and is read once.

\i sql/vexec_corpus_setup.sql

-- columns of each kind of Arrow type, of the rows whose values Arrow holds
-- (vexec_egress's), sent and read back
CREATE TEMP VIEW ing_corpus AS
	SELECT id, b, i2, i4, i8, f4, f8, d, ts, tstz, n10_2, n18_0, tx, vc, bt, u
	FROM corpus WHERE id % 50 NOT IN (15, 16, 19, 20, 21, 23, 24, 26, 27, 28, 36);
CREATE TABLE ing_dst (LIKE ing_corpus);
SET vexec.mode = force;
BEGIN;
SELECT handle AS h, array_to_string(columns, ', ') AS cols
FROM vexec_test.ingest_begin((SELECT stream FROM vexec_test.egress('SELECT * FROM ing_corpus'))) \gset
SELECT :'cols' AS columns;
EXPLAIN (COSTS OFF, VEXEC) INSERT INTO ing_dst
SELECT * FROM vexec.ingest_stream(:h) AS s(id int4, b bool, i2 int2, i4 int4, i8 int8, f4 float4,
	f8 float8, d date, ts timestamp, tstz timestamptz, n10_2 numeric(10,2), n18_0 numeric(18,0),
	tx text, vc text, bt bytea, u uuid);
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) INSERT INTO ing_dst
SELECT * FROM vexec.ingest_stream(:h) AS s(id int4, b bool, i2 int2, i4 int4, i8 int8, f4 float4,
	f8 float8, d date, ts timestamp, tstz timestamptz, n10_2 numeric(10,2), n18_0 numeric(18,0),
	tx text, vc text, bt bytea, u uuid);
SELECT rows, finished FROM vexec_test.ingest_status(:h);
COMMIT;
SELECT count(*) AS differ FROM (
	(SELECT * FROM ing_dst EXCEPT ALL SELECT * FROM ing_corpus)
	UNION ALL
	(SELECT * FROM ing_corpus EXCEPT ALL SELECT * FROM ing_dst)) x;

-- declared otherwise: each value converted, the declared typmods enforced
CREATE TABLE ing_other (a int8, n numeric(12,1), v varchar(8), d text);
BEGIN;
SELECT handle AS h FROM vexec_test.ingest_begin((SELECT stream FROM vexec_test.egress(
	$$SELECT i4, n10_2, tx, d FROM ing_corpus WHERE tx IS NULL OR length(tx) <= 8$$))) \gset
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) INSERT INTO ing_other
SELECT * FROM vexec.ingest_stream(:h) AS s(a int8, n numeric(12,1), v varchar(8), d text);
COMMIT;
SELECT count(*), sum(a), sum(n), max(scale(n)), count(v), count(d) FROM ing_other;
SELECT sum(round(n10_2, 1)) AS expected FROM ing_corpus WHERE tx IS NULL OR length(tx) <= 8;

-- the same through a function scan, PostgreSQL's, row by row
SET vexec.enable_scan = off;
TRUNCATE ing_dst;
BEGIN;
SELECT handle AS h FROM vexec_test.ingest_begin((SELECT stream FROM vexec_test.egress('SELECT * FROM ing_corpus'))) \gset
EXPLAIN (COSTS OFF) SELECT * FROM vexec.ingest_stream(:h) AS s(id int4, b bool, i2 int2, i4 int4,
	i8 int8, f4 float4, f8 float8, d date, ts timestamp, tstz timestamptz, n10_2 numeric(10,2),
	n18_0 numeric(18,0), tx text, vc text, bt bytea, u uuid);
INSERT INTO ing_dst SELECT * FROM vexec.ingest_stream(:h) AS s(id int4, b bool, i2 int2, i4 int4,
	i8 int8, f4 float4, f8 float8, d date, ts timestamp, tstz timestamptz, n10_2 numeric(10,2),
	n18_0 numeric(18,0), tx text, vc text, bt bytea, u uuid);
COMMIT;
RESET vexec.enable_scan;
SELECT count(*) AS differ FROM (
	(SELECT * FROM ing_dst EXCEPT ALL SELECT * FROM ing_corpus)
	UNION ALL
	(SELECT * FROM ing_corpus EXCEPT ALL SELECT * FROM ing_dst)) x;

-- a stream's errors: a value too long for its typmod, a decimal past its
-- precision, a column list that is not the stream's, a second reader, no
-- such stream, a stream past its transaction
BEGIN;
SELECT handle AS h FROM vexec_test.ingest_begin((SELECT stream FROM vexec_test.egress(
	$$SELECT 'abcdefghij'::text AS v$$))) \gset
SELECT * FROM vexec.ingest_stream(:h) AS s(v varchar(5));
ROLLBACK;
BEGIN;
SELECT handle AS h FROM vexec_test.ingest_begin((SELECT stream FROM vexec_test.egress(
	$$SELECT 12345.67::numeric(10,2) AS n$$))) \gset
SELECT * FROM vexec.ingest_stream(:h) AS s(n numeric(6,2));
ROLLBACK;
BEGIN;
SELECT handle AS h FROM vexec_test.ingest_begin((SELECT stream FROM vexec_test.egress(
	$$SELECT 1::int4 AS a, 'x'::text AS b$$))) \gset
SELECT * FROM vexec.ingest_stream(:h) AS s(a int4);
ROLLBACK;
BEGIN;
SELECT handle AS h FROM vexec_test.ingest_begin((SELECT stream FROM vexec_test.egress(
	$$SELECT 1::int4 AS a$$))) \gset
SELECT * FROM vexec.ingest_stream(:h) AS s(a int4);
SELECT * FROM vexec.ingest_stream(:h) AS s(a int4);
ROLLBACK;
SELECT * FROM vexec.ingest_stream(987654) AS s(a int4);
SELECT * FROM vexec.ingest_stream(:h) AS s(a int4);

DROP VIEW ing_corpus;
DROP TABLE ing_dst, ing_other;
DROP TABLE corpus;
DROP FUNCTION corpus_null(int4, int4);
