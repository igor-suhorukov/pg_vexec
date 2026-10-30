-- SPDX-License-Identifier: Apache-2.0
--
-- pg_otel_probe's spans of vexec's statements and nodes (pg_vector_executor.md
-- §3.41, OT1 to OT8): vexec's reporters, called for a query's statement and
-- nodes as the probe calls them -- vexec_test.otel_report(), the probe's side
-- -- as its executor ends, and on its error path.  A count the machine
-- changes shows as present, or as positive; the rest as it is.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
CREATE EXTENSION IF NOT EXISTS vexec_test;
RESET client_min_messages;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE ot AS
  SELECT g AS id, g % 1000 AS k, (g % 7)::int8 AS v, 'x' || (g % 50000) AS t,
         (g % 9)::int2 AS s, date '2020-01-01' + (g % 300) AS d
  FROM generate_series(1, 200000) g;
CREATE TABLE ot2 AS SELECT g AS k, 'n' || g AS name FROM generate_series(0, 999) g;
ANALYZE ot, ot2;

-- a statement's attributes, and its nodes' keys, by node name; values that
-- vary are shown as what they are
CREATE FUNCTION otel_keys(q text) RETURNS TABLE (name text, key text, value text)
LANGUAGE sql AS $$
  SELECT name, key,
         CASE WHEN key IN ('vexec.mode', 'vexec.batch_format', 'vexec.gate', 'vexec.refusal',
                           'vexec.gpu', 'vexec.device', 'vexec.device.why', 'vexec.sort_method',
                           'vexec.vector_nodes', 'vexec.device_nodes', 'vexec.device_nodes_cpu',
                           'vexec_test.rows', 'error')
              THEN value
              WHEN value ~ '^-?[0-9.e+-]+$' THEN CASE WHEN value::float8 > 0 THEN 'positive' ELSE value END
              ELSE 'present' END
  FROM vexec_test.otel_report(q)
  ORDER BY node, name, key
$$;

-- force mode: the statement's planning summary (OT6) and its vector nodes
SET vexec.mode = force;
SELECT * FROM otel_keys($$SELECT k, sum(v) FROM ot GROUP BY k$$)
  WHERE name = 'statement';

-- a closed gate's reason, in force mode
SELECT * FROM otel_keys($$SELECT id FROM ot WHERE id = 1 FOR UPDATE$$)
  WHERE name = 'statement' AND key IN ('vexec.gate', 'vexec.vector_nodes');

-- an alternative the oracle refused, and its reason
SET vexec.enable_agg = off;
SELECT * FROM otel_keys($$SELECT k, sum(v) FROM ot GROUP BY k$$)
  WHERE name = 'statement' AND key LIKE 'vexec.%refus%';
RESET vexec.enable_agg;

-- VecAgg's hashed table and its spills (OT2)
SET work_mem = '64kB';
SELECT * FROM otel_keys($$SELECT t, count(*) FROM ot GROUP BY t$$)
  WHERE name = 'VecAgg' AND key IN ('vexec.memory_kb', 'vexec.spilled_rows',
                                    'vexec.spilled_partitions', 'vexec.spill_depth');
RESET work_mem;

-- VecSort's method and space (OT2)
SELECT * FROM otel_keys($$SELECT id FROM ot WHERE id <= 1000 ORDER BY v, id$$)
  WHERE name = 'VecSort' AND key LIKE 'vexec.sort%';

-- VecHashJoin's table (OT2)
SELECT * FROM otel_keys($$SELECT count(*) FROM ot JOIN ot2 USING (k)$$)
  WHERE name = 'VecHashJoin' AND key = 'vexec.memory_kb';

-- a vector scan's rows heap's page reader prefiltered (OT3)
SELECT * FROM otel_keys($$SELECT id FROM ot WHERE s = 7 AND d > '2020-02-01' AND id % 2 = 0$$)
  WHERE name = 'VecScan' AND key = 'vexec.rows_prefiltered';

-- a source's figures (OT5): the test source's batches and rows on its scan's
-- span, the units answered from their statistics (OT3), and the estimates
-- its planning asked for on the statement's
CREATE TABLE osrc (a int4, b int8, c text) USING vexec_test_am;
INSERT INTO osrc SELECT g, g * 10, 'c' || (g % 13) FROM generate_series(1, 10000) g;
ANALYZE osrc;
SELECT * FROM otel_keys($$SELECT a, c FROM osrc WHERE a % 3 = 0$$)
  WHERE (name = 'VecScan' AND key LIKE 'vexec_test.%') OR key LIKE 'vexec_test.planning.%';
SELECT * FROM otel_keys($$SELECT count(*), min(a), max(b) FROM osrc$$)
  WHERE key = 'vexec.units_from_statistics';

-- VecAggJoin's groups (OT8): a count(DISTINCT) beside other aggregates, split
SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT * FROM otel_keys($$SELECT d, count(DISTINCT t), sum(id), count(*) FROM ot GROUP BY d$$)
  WHERE name = 'VecAggJoin';
SET max_parallel_workers_per_gather = 0;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

-- vexec.gpu forced where no device provider serves: each device strategy's
-- node says why none (OT1), and the statement the device's (OT7)
SET vexec.gpu = force;
SELECT * FROM otel_keys($$SELECT k, sum(v) FROM ot GROUP BY k$$)
  WHERE key LIKE 'vexec.device%' OR key = 'vexec.gpu';
RESET vexec.gpu;

-- no node past the probe's bounds: 32 attributes, 4 kB of keys and strings
SELECT count(*) AS dropped
  FROM vexec_test.otel_report($$SELECT o.k, count(DISTINCT o.t), sum(o.v)
                                FROM ot o JOIN ot2 ON o.k = ot2.k
                                WHERE o.s < 5 GROUP BY o.k ORDER BY 2 DESC LIMIT 5$$)
  WHERE node = -2;

-- a failed statement: the abort-safe reporters on its nodes as the error
-- left them, and its error
SELECT DISTINCT name, key, value FROM otel_keys($$SELECT sum(100 / (k - 999)) FROM ot$$)
  WHERE key IN ('vexec.batches', 'error') OR name = 'statement' AND key = 'vexec.vector_nodes'
  ORDER BY 1, 2;

-- off: nothing of vexec's
SET vexec.mode = off;
SELECT count(*) FROM vexec_test.otel_report($$SELECT k, sum(v) FROM ot GROUP BY k$$)
  WHERE key LIKE 'vexec.%';
RESET vexec.mode;

DROP TABLE ot, ot2, osrc;
DROP FUNCTION otel_keys(text);
