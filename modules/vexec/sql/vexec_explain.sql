-- SPDX-License-Identifier: Apache-2.0
--
-- The vectorized planner in V0 (pg_vector_executor.md §3.3, §5): its hooks
-- in place, which in off mode build nothing and in explain mode only cost
-- and record the vector alternatives; EXPLAIN's vexec option, which prints
-- them; the statement's gates; the oracle's refusals; and plans that stay
-- PostgreSQL's in every mode, since no vector node exists before V1.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
RESET client_min_messages;

CREATE TABLE vt (a int4, b int4, c text, d numeric(12,2), m money);
INSERT INTO vt SELECT g, g % 10, 'c' || g, g / 4.0, g FROM generate_series(1, 2000) g;
CREATE TABLE vu (a int4, e float8);
INSERT INTO vu SELECT g, g / 3.0 FROM generate_series(1, 1500) g;
ANALYZE vt, vu;

CREATE FUNCTION explain_lines(query text) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	line text;
BEGIN
	FOR line IN EXECUTE query LOOP
		RETURN NEXT line;
	END LOOP;
END
$$;

-- off: nothing is considered
SHOW vexec.mode;
EXPLAIN (VEXEC, COSTS OFF) SELECT b, count(*) FROM vt GROUP BY b;

-- without the option, EXPLAIN prints nothing of vexec's in any mode
SET vexec.mode = explain;
EXPLAIN (COSTS OFF) SELECT b, count(*) FROM vt GROUP BY b;

-- explain: the alternatives and why none was chosen
SET vexec.min_rows = 0;
EXPLAIN (VEXEC, COSTS OFF) SELECT b, count(*), sum(d) FROM vt WHERE a > 10 GROUP BY b ORDER BY b;
EXPLAIN (VEXEC, COSTS OFF) SELECT * FROM vt JOIN vu USING (a) WHERE vu.e > 1;
EXPLAIN (VEXEC, COSTS OFF) SELECT * FROM vt LEFT JOIN vu ON vt.a = vu.a;
EXPLAIN (VEXEC, COSTS OFF) SELECT * FROM vt WHERE EXISTS (SELECT 1 FROM vu WHERE vu.a = vt.a);
EXPLAIN (VEXEC, COSTS OFF) SELECT * FROM vt FULL JOIN vu ON vt.a = vu.a;
EXPLAIN (VEXEC, COSTS OFF) SELECT * FROM vt JOIN vu ON vt.a < vu.a;

-- the statement's gates (§3.3.3, §3.3.7)
EXPLAIN (VEXEC, COSTS OFF) SELECT * FROM vt WHERE a = 1 FOR UPDATE;
EXPLAIN (VEXEC, COSTS OFF) UPDATE vt SET b = 0 WHERE a = 1;
EXPLAIN (VEXEC, COSTS OFF) DELETE FROM vt WHERE a = 1;
EXPLAIN (VEXEC, COSTS OFF) MERGE INTO vt USING vu ON vt.a = vu.a WHEN MATCHED THEN DO NOTHING;
CREATE UNIQUE INDEX vu_a ON vu (a);
EXPLAIN (VEXEC, COSTS OFF) INSERT INTO vu SELECT a, b FROM vt ON CONFLICT (a) DO UPDATE SET e = 0;
EXPLAIN (VEXEC, COSTS OFF) INSERT INTO vu SELECT a, b FROM vt ON CONFLICT DO NOTHING;
DROP INDEX vu_a;
EXPLAIN (VEXEC, COSTS OFF) DECLARE vc CURSOR FOR SELECT * FROM vt;

-- the oracle's refusals, and what a node kind's setting turns off
EXPLAIN (VEXEC, COSTS OFF) SELECT * FROM vt TABLESAMPLE SYSTEM (50);
EXPLAIN (VEXEC, COSTS OFF) SELECT count(DISTINCT b) FROM vt;
EXPLAIN (VEXEC, COSTS OFF) SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY a) FROM vt;
EXPLAIN (VEXEC, COSTS OFF) SELECT b, count(*) FROM vt GROUP BY GROUPING SETS ((b), ());
EXPLAIN (VEXEC, COSTS OFF) SELECT m, count(*) FROM vt GROUP BY m;
SET enable_seqscan = off;
EXPLAIN (VEXEC, COSTS OFF) SELECT count(*) FROM vt;
RESET enable_seqscan;
SET enable_hashjoin = off;
EXPLAIN (VEXEC, COSTS OFF) SELECT * FROM vt JOIN vu USING (a);
RESET enable_hashjoin;
SET vexec.enable_scan = off;
SET vexec.enable_agg = off;
EXPLAIN (VEXEC, COSTS OFF) SELECT count(*) FROM vt;
RESET vexec.enable_scan;
RESET vexec.enable_agg;
RESET vexec.min_rows;
EXPLAIN (VEXEC, COSTS OFF) SELECT count(*) FROM vt;

-- a SubPlan and a volatile function run in the fallback, row by row
SET vexec.min_rows = 0;
EXPLAIN (VEXEC, COSTS OFF) SELECT a FROM vt WHERE b < (SELECT max(e) FROM vu WHERE vu.a = vt.a) AND random() >= 0;

-- each query level numbers its relations from 1: a CTE's join and the
-- outer query's are two records
EXPLAIN (VEXEC, COSTS OFF)
WITH c AS MATERIALIZED (SELECT vt.a FROM vt JOIN vu USING (a))
SELECT * FROM vt JOIN vu USING (a) JOIN c USING (a);

-- auto and force: no vector node exists yet, so the plan is PostgreSQL's,
-- and EXPLAIN (VEXEC) says why
SET vexec.mode = auto;
EXPLAIN (VEXEC, COSTS OFF) SELECT b, count(*) FROM vt GROUP BY b;
SET vexec.mode = force;
EXPLAIN (VEXEC, COSTS OFF) SELECT b, count(*) FROM vt GROUP BY b;
-- the other formats are priced too, and recorded
SET vexec.batch_format = arrow;
EXPLAIN (VEXEC, COSTS OFF) SELECT count(*) FROM vt;
RESET vexec.batch_format;

-- a test that needs a vector node fails where none was built
SET vexec.debug_require_vector = on;
SELECT count(*) FROM vt;
RESET vexec.debug_require_vector;

-- in explain mode it requires nothing
SET vexec.mode = explain;
SET vexec.debug_require_vector = on;
SELECT count(*) FROM vt;
RESET vexec.debug_require_vector;

-- the other formats of EXPLAIN
EXPLAIN (VEXEC, COSTS OFF, FORMAT JSON) SELECT b, count(*) FROM vt GROUP BY b;
EXPLAIN (VEXEC, COSTS OFF, FORMAT YAML) SELECT * FROM vt WHERE a = 1 FOR SHARE;

-- the costs: the row terms are PostgreSQL's own (§6.8's alignment), and a
-- vector alternative's are derived from them
SELECT alt->>'Node' AS node,
       (alt->>'Row Total Cost')::float8 = (plan->'Plan'->>'Total Cost')::float8 AS row_cost_is_the_plans,
       (alt->>'Total Cost')::float8 > 0 AS vector_cost,
       (alt->>'Rows In Cost')::float8 > 0 AS transposes_rows,
       (alt->>'Rows Out Cost')::float8 > 0 AS hands_rows_out
FROM (SELECT l::json->0 AS plan FROM explain_lines('EXPLAIN (VEXEC, FORMAT JSON) SELECT a FROM vt') l) p,
     json_array_elements(p.plan->'Vexec'->'Alternatives') alt;
-- the factors move the vector costs, never the plan
SET vexec.cpu_tuple_factor = 0;
SELECT (alt->>'Total Cost')::float8 < (alt->>'Row Total Cost')::float8 AS cheaper
FROM (SELECT l::json->0 AS plan FROM explain_lines('EXPLAIN (VEXEC, FORMAT JSON) SELECT a FROM vt') l) p,
     json_array_elements(p.plan->'Vexec'->'Alternatives') alt;
SET vexec.cpu_tuple_factor = 100;
SELECT (alt->>'Total Cost')::float8 > (alt->>'Row Total Cost')::float8 AS dearer
FROM (SELECT l::json->0 AS plan FROM explain_lines('EXPLAIN (VEXEC, FORMAT JSON) SELECT a FROM vt') l) p,
     json_array_elements(p.plan->'Vexec'->'Alternatives') alt;
RESET vexec.cpu_tuple_factor;
-- a factor past any meaning gives a cost the plan still reads back
SET vexec.convert_cost = 1e308;
SELECT (alt->>'Total Cost')::float8 AS clamped
FROM (SELECT l::json->0 AS plan FROM explain_lines('EXPLAIN (VEXEC, FORMAT JSON) SELECT a FROM vt') l) p,
     json_array_elements(p.plan->'Vexec'->'Alternatives') alt;
RESET vexec.convert_cost;

-- plans are PostgreSQL's in every mode, costs and all
CREATE TABLE vexec_plans (mode text, query int4, plan text);
DO $$
DECLARE
	m text;
	q text;
	i int4 := 0;
	queries text[] := ARRAY[
		'SELECT b, count(*), sum(d) FROM vt WHERE a > 10 GROUP BY b ORDER BY b',
		'SELECT * FROM vt JOIN vu USING (a) WHERE vu.e > 1',
		'SELECT * FROM vt WHERE a = 1 FOR UPDATE',
		'SELECT a, (SELECT max(e) FROM vu WHERE vu.a = vt.b) FROM vt ORDER BY a DESC LIMIT 5'];
BEGIN
	FOREACH m IN ARRAY ARRAY['off', 'explain', 'auto', 'force'] LOOP
		PERFORM set_config('vexec.mode', m, false);
		i := 0;
		FOREACH q IN ARRAY queries LOOP
			i := i + 1;
			INSERT INTO vexec_plans
				SELECT m, i, string_agg(l, E'\n') FROM explain_lines('EXPLAIN ' || q) l;
		END LOOP;
	END LOOP;
END
$$;
SELECT query, count(DISTINCT plan) AS plans FROM vexec_plans GROUP BY query ORDER BY query;

-- the plan check walks every plan, subplans included
SET vexec.debug_check_plans = on;
SELECT count(*) FROM vt WHERE b IN (SELECT a FROM vu) AND a > (SELECT min(a) FROM vu);
WITH w AS MATERIALIZED (SELECT * FROM vt) SELECT count(*) FROM w, vu WHERE w.a = vu.a;

DROP TABLE vexec_plans;
DROP FUNCTION explain_lines(text);
DROP TABLE vt, vu;
RESET ALL;
