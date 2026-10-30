-- pg_otel_probe's spans of scans of tables of Parquet files
-- (pg_vector_executor.md §3.41, OT4, OT5): the access method's reporter of a
-- scan vexec does not take, the source's figures of one it does, and what a
-- statement's planning read -- through vexec_test's side of the probe,
-- otel_report() -- and the files' waits under events of their own
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec_parquet;
CREATE EXTENSION IF NOT EXISTS vexec;
CREATE EXTENSION IF NOT EXISTS vexec_test;
RESET client_min_messages;
CREATE TABLE otf (id int8, grp int4, name text) USING vexec_parquet WITH (row_group_rows = 10000);
INSERT INTO otf SELECT i, i % 10, 'n' || (i % 100) FROM generate_series(1, 50000) i;
ANALYZE otf;

-- a Seq Scan, vexec off: the units it read, of the table's
SET vexec.mode = off;
SELECT name, key, value FROM vexec_test.otel_report($$SELECT count(*) FROM otf WHERE grp = 3$$)
  WHERE key LIKE 'vexec_parquet.%' ORDER BY node, key;

-- a VecScan: the units read here, handed to the device, and passed over by
-- their statistics
SET vexec.mode = force;
SELECT name, key, value FROM vexec_test.otel_report($$SELECT count(*), sum(grp) FROM otf WHERE id BETWEEN 25000 AND 26000$$)
  WHERE name = 'VecScan' AND key LIKE 'vexec_parquet.%' ORDER BY node, key;

-- what a fresh process's planning of it read: its listing, from the catalog
\c
SET vexec.mode = force;
SELECT key, value::int > 0 AS grew
  FROM vexec_test.otel_report($$SELECT grp, count(*) FROM otf GROUP BY grp$$)
  WHERE node = 0 AND key LIKE 'vexec_parquet.planning.%' ORDER BY key;

-- the files' reads and the point reads' threads waited on under events of
-- their own, registered as the engine started in a process
SELECT name FROM pg_wait_events WHERE type = 'Extension' AND name LIKE 'VexecParquet%' ORDER BY name;

DROP TABLE otf;
