-- SPDX-License-Identifier: Apache-2.0
--
-- vexec's egress (pg_vector_executor.md §3.15; §5 V10; vexec_egress.h): a
-- query's result as Arrow IPC messages, for an extension that serves Arrow
-- to clients, vexec_flight.  vexec_test.egress() runs a query into the
-- egress's receiver as a Flight session's statement goes, and
-- vexec_test.egress_params() reads a stream back through the egress's
-- parameter reader.  The clients' own checks -- every corpus type's Arrow
-- type and values, against adbc_driver_postgresql and psql -- are
-- vexec_flight's (its test/test_flight.py).

\i sql/vexec_corpus_setup.sql

-- the rows of the corpus whose values Arrow holds in every column
CREATE TEMP VIEW egress_corpus AS
  SELECT * FROM corpus WHERE id % 50 NOT IN (15, 16, 19, 20, 21, 23, 24, 26, 27, 28, 36);

SET vexec.mode = force;

-- every column's Arrow type: "name format", an extension's name after it
SELECT unnest(fields) AS field FROM vexec_test.egress('SELECT * FROM egress_corpus');

-- a vector node at the top, whose batches go out from its own buffers; a
-- row node at the top, a CTE Scan, whose rows the receiver gathers
CREATE TEMP TABLE egress_streams AS
  SELECT 'vector' AS path, * FROM vexec_test.egress('SELECT * FROM egress_corpus')
  UNION ALL
  SELECT 'row', * FROM vexec_test.egress(
    'WITH c AS MATERIALIZED (SELECT * FROM egress_corpus) SELECT * FROM c');
SELECT path, rows, batches, vector_batches > 0 AS from_vectors
FROM egress_streams ORDER BY path;

-- each stream is Arrow's IPC: a schema, its batches, the end; vexec's
-- reader reads it, and its writer writes it back as it was
SELECT s.path, m.n, m.kind FROM egress_streams s, LATERAL vexec_test.ipc_messages(s.stream) m
ORDER BY 1, 2;
SELECT path, vexec_test.ipc_reencode(stream) = stream AS rewritten,
       vexec_test.ipc_reencode(stream, false) = stream AS rewritten_unaligned
FROM egress_streams ORDER BY path;

-- each column, through each path and read back by the parameter reader as
-- its own type: its values, as its output function writes them
CREATE FUNCTION pg_temp.read_back(path text, col name)
RETURNS TABLE (extra int8, missing int8)
LANGUAGE plpgsql AS $$
DECLARE
	q text := format('SELECT %I FROM egress_corpus', col);
	t regtype;
BEGIN
	IF path = 'row' THEN
		q := format('WITH c AS MATERIALIZED (%s) SELECT * FROM c', q);
	END IF;
	SELECT a.atttypid::regtype INTO t
	FROM pg_attribute a WHERE a.attrelid = 'corpus'::regclass AND a.attname = col;
	CREATE TEMP TABLE IF NOT EXISTS egress_got (v text);
	CREATE TEMP TABLE IF NOT EXISTS egress_want (v text);
	TRUNCATE egress_got, egress_want;
	INSERT INTO egress_got
	  SELECT p.row FROM vexec_test.egress(q) e, LATERAL vexec_test.egress_params(e.stream, ARRAY[t]) p;
	EXECUTE format('INSERT INTO egress_want SELECT CASE WHEN %1$I IS NULL THEN ''NULL'' '
				   'ELSE format(''%%s'', %1$I) END FROM egress_corpus', col);
	RETURN QUERY
	  SELECT (SELECT count(*) FROM (TABLE egress_got EXCEPT ALL TABLE egress_want) x),
			 (SELECT count(*) FROM (TABLE egress_want EXCEPT ALL TABLE egress_got) x);
END $$;
SET client_min_messages = warning;
SELECT p.path, a.attname, r.extra, r.missing
FROM (VALUES ('vector'), ('row')) p(path), pg_attribute a,
     LATERAL pg_temp.read_back(p.path, a.attname) r
WHERE a.attrelid = 'corpus'::regclass AND a.attnum > 0 AND (r.extra > 0 OR r.missing > 0);
RESET client_min_messages;

-- a value its column's Arrow type cannot hold fails the statement, naming
-- the column, the type and the value's kind
SELECT rows FROM vexec_test.egress('SELECT d FROM corpus WHERE id % 50 = 15 AND d IS NOT NULL');
SELECT rows FROM vexec_test.egress('SELECT t FROM corpus WHERE id % 50 = 19 AND t IS NOT NULL');
SELECT rows FROM vexec_test.egress('SELECT ts FROM corpus WHERE id % 50 = 20 AND ts IS NOT NULL');
SELECT rows FROM vexec_test.egress('SELECT ts FROM corpus WHERE id % 50 = 24 AND ts IS NOT NULL');
SELECT rows FROM vexec_test.egress('SELECT iv FROM corpus WHERE id % 50 = 26 AND iv IS NOT NULL');
SELECT rows FROM vexec_test.egress('SELECT iv FROM corpus WHERE id % 50 = 28 AND iv IS NOT NULL');
SELECT rows FROM vexec_test.egress('SELECT n38_5 FROM corpus WHERE id % 50 = 36 AND n38_5 IS NOT NULL');
-- the last microsecond Arrow's timestamp holds goes out
SELECT rows FROM vexec_test.egress('SELECT ts FROM corpus WHERE id % 50 = 25');

-- an empty result: its schema, and the end
SELECT rows, batches, cardinality(fields) AS fields,
       (SELECT array_agg(kind ORDER BY n) FROM vexec_test.ipc_messages(stream)) AS messages
FROM vexec_test.egress('SELECT * FROM egress_corpus WHERE false');

-- parameters: a stream of the egress's, read as other types -- by
-- assignment, utf8 by the type's input function -- and refused where
-- neither applies
SELECT * FROM vexec_test.egress_params(
  (SELECT stream FROM vexec_test.egress('SELECT i2, i4, f4, n10_2, ''2026-10-06''::text FROM egress_corpus WHERE id BETWEEN 2 AND 4 ORDER BY id')),
  '{int8,numeric,float8,float8,date}');
SELECT * FROM vexec_test.egress_params(
  (SELECT stream FROM vexec_test.egress('SELECT tx FROM egress_corpus WHERE id = 2')), '{date}');
SELECT * FROM vexec_test.egress_params(
  (SELECT stream FROM vexec_test.egress('SELECT d FROM egress_corpus WHERE id = 2')), '{int4}');
SELECT * FROM vexec_test.egress_params(
  (SELECT stream FROM vexec_test.egress('SELECT d FROM egress_corpus WHERE id = 2')), '{int4,int4}');

-- the egress serves only while the vector executor is active
SET vexec.mode = off;
SELECT rows FROM vexec_test.egress('SELECT 1');
RESET vexec.mode;

DROP TABLE egress_streams;
DROP VIEW egress_corpus;
DROP TABLE corpus;
DROP FUNCTION corpus_null(int4, int4);
