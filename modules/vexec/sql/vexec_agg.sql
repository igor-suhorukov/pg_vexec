-- SPDX-License-Identifier: Apache-2.0
--
-- VecAgg (pg_vector_executor.md §3.8): aggregation, plain and hashed, of
-- every split, against PostgreSQL's own Agg.  Each query runs off and in
-- force mode and its answers are compared row for row: the same rows, the
-- same bits.  Errors are compared as they are raised.

CREATE TABLE at AS
  SELECT g AS id, g % 10 AS k, (g % 7)::int2 AS s, g::int8 * 1000 AS b,
         (g % 13)::numeric(10,2) / 4 AS n, (g % 11)::float8 / 3 AS f,
         ((g % 9) - 4)::float4 / 7 AS r,
         CASE WHEN g % 5 = 0 THEN NULL ELSE 'v' || (g % 17) END AS t,
         g % 3 = 0 AS bo, date '2020-01-01' + (g % 100) AS d,
         timestamp '2020-01-01 00:00' + (g % 50) * interval '1 hour' AS ts,
         CASE WHEN g % 4 = 0 THEN NULL ELSE (g % 23)::numeric END AS u,
         (g % 6)::oid AS o, (g % 19)::money AS m
  FROM generate_series(1, 20000) g;
ALTER TABLE at ADD PRIMARY KEY (id);
CREATE TABLE ae (LIKE at);
ANALYZE at, ae;

-- off and force, compared: 'same' and whether force's plan has a VecAgg.
-- Rows compare as their text, which prints every bit of a float.
CREATE FUNCTION agg_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	n bigint;
	plan text := '';
	r record;
BEGIN
	PERFORM set_config('vexec.mode', 'off', true);
	EXECUTE 'CREATE TEMP TABLE agg_off AS SELECT q::text AS r FROM (' || q || ') q';
	PERFORM set_config('vexec.mode', 'force', true);
	FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || r."QUERY PLAN" || E'\n';
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE agg_on AS SELECT q::text AS r FROM (' || q || ') q';
	EXECUTE 'SELECT count(*) FROM ((TABLE agg_off EXCEPT ALL TABLE agg_on) UNION ALL '
		'(TABLE agg_on EXCEPT ALL TABLE agg_off)) d' INTO n;
	DROP TABLE agg_off;
	DROP TABLE agg_on;
	RETURN CASE WHEN n = 0 THEN 'same' ELSE n || ' rows differ' END ||
		CASE WHEN plan ~ 'Vec [A-Za-z ]*Aggregate' THEN ', VecAgg' ELSE ', no VecAgg' END;
END $$;

-- a query's answer or its error, off and in force mode
CREATE FUNCTION agg_error(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	res text[];
	got text;
	mode text;
BEGIN
	FOREACH mode IN ARRAY ARRAY['off', 'force'] LOOP
		PERFORM set_config('vexec.mode', mode, true);
		BEGIN
			EXECUTE 'SELECT coalesce(string_agg(q::text, '' ''), ''no rows'') FROM (' || q || ') q'
				INTO STRICT got;
			res := res || got;
		EXCEPTION WHEN OTHERS THEN
			res := res || ('ERROR ' || SQLSTATE || ': ' || SQLERRM);
		END;
	END LOOP;
	RETURN CASE WHEN res[1] = res[2] THEN 'both ' || res[1]
		ELSE 'off ' || res[1] || ', force ' || res[2] END;
END $$;

-- an EXPLAIN ANALYZE line of force mode's plan
CREATE FUNCTION agg_explain(q text, pattern text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE
	r record;
BEGIN
	PERFORM set_config('vexec.mode', 'force', true);
	FOR r IN EXECUTE 'EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF r."QUERY PLAN" ~ pattern THEN
			RETURN NEXT regexp_replace(r."QUERY PLAN", '[0-9]+', 'N', 'g');
		END IF;
	END LOOP;
END $$;

SET vexec.mode = force;

-- plain and hashed, every vectorized transition
EXPLAIN (COSTS OFF, VERBOSE)
SELECT count(*), count(t), sum(k), sum(s), sum(b), sum(n), sum(f), sum(r),
       avg(k), avg(s), avg(b), avg(n), avg(f), avg(r), min(id), max(id), min(f), max(r),
       min(t), max(t), min(d), max(ts), bool_and(bo), bool_or(bo), every(bo), min(o), max(m)
FROM at;
SELECT agg_check($$SELECT count(*), count(t), sum(k), sum(s), sum(b), sum(n), sum(f), sum(r),
       avg(k), avg(s), avg(b), avg(n), avg(f), avg(r), min(id), max(id), min(f), max(r),
       min(t), max(t), min(d), max(ts), bool_and(bo), bool_or(bo), every(bo), min(o), max(m)
FROM at$$);
SELECT agg_check($$SELECT k, count(*), count(t), sum(k), sum(s), sum(b), sum(n), sum(f), sum(r),
       avg(k), avg(s), avg(b), avg(n), avg(f), avg(r), min(id), max(id), min(f), max(r),
       min(t), max(t), min(d), max(ts), bool_and(bo), bool_or(bo), every(bo), min(o), max(m)
FROM at GROUP BY k$$);
SELECT agg_check($$SELECT variance(f), stddev(f), var_pop(r), stddev_samp(r), sum(u), avg(u),
       sum(n * 3), avg(n * n) FROM at GROUP BY s$$);
-- keys of each kind: bits, bytes, and through the type's functions
SELECT agg_check('SELECT s, d, bo, count(*), sum(id) FROM at GROUP BY s, d, bo');
SELECT agg_check('SELECT t, count(*), max(id) FROM at GROUP BY t');
SELECT agg_check('SELECT t COLLATE "C", count(*), min(t COLLATE "C") FROM at GROUP BY 1');
SELECT agg_check('SELECT n, u, count(*), sum(b) FROM at GROUP BY n, u');
SELECT agg_check('SELECT ts, o, count(*), max(m) FROM at GROUP BY ts, o');
-- money has no hash operator class: PostgreSQL's sorted aggregation
SELECT agg_check('SELECT m, count(*) FROM at GROUP BY m');
SELECT agg_check('SELECT f, r, count(*) FROM at GROUP BY f, r');
SELECT agg_check($$SELECT x, count(*) FROM (VALUES (0.0::float8), (-0.0), ('NaN'), ('NaN'),
       ('Infinity'), ('-Infinity'), (NULL), (0.0)) v(x) GROUP BY x$$);
-- expressions over keys and aggregates, HAVING, the first row's columns
SELECT agg_check('SELECT k + 1, sum(id) * 2, avg(f) + max(f) FROM at GROUP BY k HAVING count(*) > 10');
SELECT agg_check('SELECT k % 3, count(*) FROM at GROUP BY k % 3 HAVING sum(id) > 0');
SELECT agg_check('SELECT id, k, t, count(*) FROM at WHERE id < 100 GROUP BY id');
SELECT agg_check('SELECT count(*) FROM at HAVING count(*) > 1');
SELECT agg_check('SELECT count(*) FROM at HAVING count(*) < 1');
SELECT agg_check('SELECT 1 FROM at HAVING sum(k) > 0');
-- no rows: a plain aggregation's one row, a hashed one's none
SELECT agg_check('SELECT count(*), sum(id), avg(n), min(t), bool_and(bo), sum(f), variance(r) FROM ae');
SELECT agg_check('SELECT k, count(*) FROM ae GROUP BY k');
SELECT agg_check('SELECT count(*), sum(u), avg(u), max(t) FROM at WHERE u IS NULL');
-- FILTER, through the selection
SELECT agg_check($$SELECT k, count(*) FILTER (WHERE bo), sum(n) FILTER (WHERE id > 50),
       avg(f) FILTER (WHERE t IS NULL), min(t) FILTER (WHERE s > 3), max(b) FILTER (WHERE false)
FROM at GROUP BY k$$);
-- aggregates through fmgr, beside the vectorized ones
SELECT agg_check($$SELECT k, count(*), json_agg(s)::text, array_agg(id)::text IS NOT NULL,
       string_agg(t, ',') IS NOT NULL, max(d), bit_or(id), sum(id)
FROM at WHERE id < 2000 GROUP BY k$$);
SELECT agg_check('SELECT s, sum(interval ''1 hour'' * k), avg(interval ''1 day'' * s), max(t || k) FROM at GROUP BY s');
-- numeric: NaN, the infinities, display scales that differ, more than 38 digits
CREATE TABLE an (g int, x numeric);
INSERT INTO an SELECT g % 4, (g % 97)::numeric / 8 FROM generate_series(1, 5000) g;
INSERT INTO an VALUES (0, 'NaN'), (1, 'Infinity'), (1, '-Infinity'), (2, 'Infinity'),
  (3, 1.123456789), (3, 12345678901234567890123456789012345678901234567890.5),
  (3, -0.000000000000000000000000000000000000000001);
ANALYZE an;
SELECT agg_check('SELECT g, sum(x), avg(x), count(x), min(x), max(x) FROM an GROUP BY g');
SELECT agg_check('SELECT sum(x), avg(x) FROM an');
SELECT g, sum(x), avg(x) FROM an GROUP BY g ORDER BY g;
-- constant arguments: one value for every row
SELECT agg_check($$SELECT max('a'::text COLLATE "C"), min(42), sum(1.5), count(NULL::int),
       bool_or(true) FILTER (WHERE id > 5) FROM at$$);
SELECT agg_check($$SELECT max(foo COLLATE "C") FILTER (WHERE (bar COLLATE "POSIX") > '0')
       FROM (VALUES ('a', 'b')) AS v(foo, bar)$$);
-- a comparison of two constants -- an InitPlan's value and a literal, as a
-- column a storage hands over as one value for every row: one answer for
-- every row
SELECT agg_check($$SELECT count(*) FILTER (WHERE (SELECT 7) = 7), count(*) FILTER (WHERE (SELECT 6) = 7),
       count(*) FILTER (WHERE (SELECT 7::int8) < 9), sum(k) FILTER (WHERE (SELECT 2.5::float8) >= 2.5) FROM at$$);
-- GROUP BY keys the planner proves constant: a grouping on no keys, one
-- group where there are rows and none where there are none
SELECT agg_check('SELECT k, t, count(*) FROM at WHERE k = 3 AND t = ''v3'' GROUP BY k, t');
SELECT agg_check('SELECT k, count(*) FROM at WHERE k = 3 AND id < 0 GROUP BY k');
-- H8: SUM(x + k), an int2 x and an int4 constant no int2 overflows with, as
-- SUM(x) + k * COUNT(x): x read once for every such sum, x + k never
-- computed; a constant past the bound keeps PostgreSQL's x + k, which can
-- raise
EXPLAIN (COSTS OFF, VERBOSE) SELECT sum(s), sum(s + 1), sum(1 + s), sum(s - 3) FROM at;
SELECT agg_check($$SELECT sum(s), sum(s + 1), sum(s + 2), sum(1 + s), sum(s - 3),
       sum(s + 2147450000), sum(s - 2147450000), count(*) FROM at$$);
SELECT agg_check('SELECT k, sum(s + 7), sum(s + 9), avg(s), count(s) FROM at GROUP BY k');
SELECT agg_check('SELECT sum(s + 1) FROM ae');
SELECT agg_check('SELECT sum(s + 1), sum(s + 1) FILTER (WHERE id > 10) FROM at');
SELECT agg_error('SELECT sum(s + 2147483000) FROM at');
-- H7 and H8's kernels, under aggregates: length, octet_length, date_trunc
SELECT agg_check('SELECT k, sum(length(t)), max(char_length(t)), sum(octet_length(t)) FROM at GROUP BY k');
SELECT agg_check($$SELECT date_trunc('minute', ts), count(*), min(date_trunc('hour', ts)),
       max(date_trunc('day', ts)), max(date_trunc('second', ts)) FROM at GROUP BY 1$$);
CREATE TABLE ak (x text, y timestamp);
INSERT INTO ak VALUES ('h' || chr(233) || 'llo', '1999-12-31 23:59:59.999999'),
  ('', '1960-02-29 12:34:56.789'), (NULL, 'infinity'), (repeat(chr(26085), 3), '-infinity'),
  ('plain', NULL), ('a' || chr(128512) || 'b', '2000-01-01 00:00:00');
EXPLAIN (COSTS OFF, VERBOSE) SELECT length(x), date_trunc('minute', y) FROM ak;
SELECT agg_check($$SELECT x, length(x), char_length(x), character_length(x), octet_length(x),
       date_trunc('minute', y), date_trunc('day', y), date_trunc('milliseconds', y),
       date_trunc('hour', y), date_trunc('second', y), date_trunc('microseconds', y),
       date_trunc('month', y) FROM ak$$);
-- int8 sums past int64, as int128 holds them
SELECT agg_check($$SELECT sum(x), avg(x) FROM (VALUES (9223372036854775807::int8),
       (9223372036854775807), (9223372036854775807), (-1)) v(x)$$);

-- the batch formats
SET vexec.batch_format = arrow;
SELECT agg_check($$SELECT k, count(*), sum(b), sum(n), avg(f), min(t), max(t), min(d), max(ts),
       bool_and(bo) FROM at GROUP BY k$$);
SELECT agg_check('SELECT t, d, count(*), sum(n) FROM at GROUP BY t, d');
RESET vexec.batch_format;
SET vexec.batch_numeric_layout = varlena;
SELECT agg_check('SELECT k, sum(n), avg(n), sum(n * 2), max(n) FROM at GROUP BY k');
RESET vexec.batch_numeric_layout;

-- errors are PostgreSQL's own, from the row PostgreSQL raises them at
SELECT agg_error('SELECT sum(100 / (id % 7)) FROM at');
SELECT agg_error('SELECT k, sum(100 / (id % 7)) FROM at GROUP BY k');
SELECT agg_error('SELECT sum(100 / (id % 7)) FILTER (WHERE id % 7 <> 0) FROM at');
SELECT agg_error('SELECT k, sum(100 / (id % 7)) FILTER (WHERE id % 7 <> 0) FROM at GROUP BY k ORDER BY k');
SELECT agg_error('SELECT sum(f * 1e308) FROM at');
SELECT agg_error('SELECT sum(x) FROM (VALUES (1e308::float8), (1e308), (1)) v(x)');
SELECT agg_error('SELECT k, sum(1e308::float8 + f) FROM at GROUP BY k');
SELECT agg_error('SELECT avg(x) FROM (VALUES (1e308::float8), (1e308)) v(x)');
SELECT agg_error('SELECT sum(x) FROM (VALUES (3.0e38::float4), (3.0e38)) v(x)');
SELECT agg_error('SELECT k, count(*), sum(1e308::float8 * id) FILTER (WHERE id = 9999 OR id = 3) FROM at GROUP BY k ORDER BY k');
-- the error of the first row that raises one, whichever aggregate's
SELECT agg_error('SELECT sum(1e308::float8 * (id % 3)), sum(100 / (id - 5)) FROM at');
SELECT agg_error('SELECT sum(100 / (id - 1)), sum(1e308::float8 * (id % 3)) FROM at');
SELECT agg_error('SELECT k, sum(100 / (id - 1)), sum(1e308::float8 * (id % 3)) FROM at GROUP BY k');
SELECT agg_error('SELECT k, sum(1e308::float8 * (id % 3)), json_agg(100 / (id - 1)) FROM at GROUP BY k');
-- a float sum's own overflow at the second row, before the fifth row's error
SELECT agg_error('SELECT sum(1e308::float8), sum(100 / (id - 5)) FROM at');
SELECT agg_error('SELECT sum(100 / (id - 5)), sum(1e308::float8) FROM at');
SELECT agg_error('SELECT json_agg(100 / (id - 5)), sum(1e308::float8) FROM at');
SELECT agg_error('SELECT sum(1e308::float8), json_agg(100 / (id - 5)) FROM at WHERE id > 3');
-- an InitPlan behind a FILTER runs only where PostgreSQL would run it
SELECT agg_error('SELECT sum(id + (SELECT 1 / (count(*) - count(*))::int FROM ae)) FILTER (WHERE false) FROM at');
SELECT agg_error('SELECT sum(id + (SELECT 1 / (count(*) - count(*))::int FROM ae)) FILTER (WHERE id > 0) FROM at');
SELECT agg_error('SELECT k, sum(id + (SELECT 1 / (count(*) - count(*))::int FROM ae)) FILTER (WHERE id < 0) FROM at GROUP BY k ORDER BY k');

-- volatile arguments and FILTERs, row by row in PostgreSQL's order: each
-- aggregate's argument in turn for a row, a FILTER before its arguments
CREATE SEQUENCE aseq;
SELECT count(*), sum(nextval('aseq')) - sum(nextval('aseq')) AS interleaved,
       max(currval('aseq')) AS calls FROM at;
SELECT k, count(*) FILTER (WHERE nextval('aseq') % 2 = 0), sum(nextval('aseq') % 7)
FROM at GROUP BY k ORDER BY k;
SELECT currval('aseq');
SET vexec.mode = off;
ALTER SEQUENCE aseq RESTART;
SELECT count(*), sum(nextval('aseq')) - sum(nextval('aseq')) AS interleaved,
       max(currval('aseq')) AS calls FROM at;
SELECT k, count(*) FILTER (WHERE nextval('aseq') % 2 = 0), sum(nextval('aseq') % 7)
FROM at GROUP BY k ORDER BY k;
SELECT currval('aseq');
SET vexec.mode = force;

-- spill: past hash_mem, rows of new groups to partitions, read back.  A
-- group's rows come in their input order in each, PostgreSQL's hashed
-- aggregation spilling too, and its float sums are the same bits.
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SET enable_sort = off;
SELECT agg_check('SELECT id, count(*), sum(n), max(t) FROM at GROUP BY id');
SELECT agg_check('SELECT id % 5000, t, count(*), avg(f), sum(b) FROM at GROUP BY 1, 2');
SELECT agg_check('SELECT id, json_agg(k)::text FROM at GROUP BY id');
SELECT agg_explain('SELECT id, count(*), sum(n) FROM at GROUP BY id', 'Spill|Vec');
RESET work_mem;
RESET hash_mem_multiplier;
RESET enable_sort;

-- the partial and final pair, under a Gather.  Which rows each worker
-- sums is the workers' race: a float's sum is compared to 10 digits.
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
EXPLAIN (COSTS OFF) SELECT k, count(*), sum(n), avg(f) FROM at GROUP BY k;
SELECT agg_check('SELECT k, count(*), sum(b), avg(b), sum(n), avg(n), min(t), max(d), bool_or(bo) FROM at GROUP BY k');
SELECT agg_check($$SELECT count(*), sum(n), round(avg(f)::numeric, 10), round(variance(f)::numeric, 10),
       max(t), json_agg(k)::text IS NOT NULL FROM at$$);
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;

-- H5: the final stage in each participant too, below the Gather, over a
-- VecRepartition that deals the partial groups out by their keys' hash
-- (exec/vecrepart.c): the Gather receives final groups only.  A group's
-- partial states all meet in one partition, whichever participants made
-- them: sums of integers and numerics are the same bits.
CREATE TABLE ah AS
  SELECT g AS id, (g::int8 * 7919 % 40000)::int AS k, (g % 13)::numeric(10,2) / 4 AS n,
         'v' || (g % 25000) AS t, date '2020-01-01' + (g % 100) AS d
  FROM generate_series(1, 120000) g;
ANALYZE ah;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0.01;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
EXPLAIN (COSTS OFF) SELECT k, count(*), sum(n) FROM ah GROUP BY k;
SELECT agg_check('SELECT k, count(*), sum(id), sum(n), max(t), min(d) FROM ah GROUP BY k');
SELECT agg_check('SELECT t, k % 3, count(*), sum(n) FROM ah GROUP BY 1, 2 HAVING count(*) > 1');
SELECT agg_explain('SELECT k, count(*), sum(n) FROM ah GROUP BY k', 'Repartition|^ *Partitions:');
-- ordered: sorted in each participant too, under a Gather Merge
EXPLAIN (COSTS OFF) SELECT k, count(*), sum(n) FROM ah GROUP BY k ORDER BY 3 DESC, 1;
SELECT agg_check('SELECT k, count(*), sum(n) FROM ah GROUP BY k ORDER BY 3 DESC, 1 LIMIT 7 OFFSET 3');
-- no worker launched: the leader writes every group, and reads them
SET max_parallel_workers = 0;
SELECT agg_check('SELECT k, count(*), sum(id), sum(n) FROM ah GROUP BY k');
RESET max_parallel_workers;
-- the plan run serially, as a SQL function's last query runs a row at a
-- time (functions.c): no DSM, and the node passes its child's groups on.
-- The function's plan is cached at its first call, in force mode.
CREATE FUNCTION ah_total() RETURNS numeric LANGUAGE sql
  AS 'SELECT sum(c * s) FROM (SELECT k, count(*) c, sum(n) s FROM ah GROUP BY k) x';
EXPLAIN (COSTS OFF) SELECT sum(c * s) FROM (SELECT k, count(*) c, sum(n) s FROM ah GROUP BY k) x;
SELECT ah_total();
SET vexec.mode = off;
SELECT sum(c * s) FROM (SELECT k, count(*) c, sum(n) s FROM ah GROUP BY k) x;
SET vexec.mode = force;
DROP FUNCTION ah_total();
-- rescanned: the Gather a nested loop's inner, the partitions made again
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
SET enable_memoize = off;
EXPLAIN (COSTS OFF) WITH o AS MATERIALIZED (SELECT i FROM generate_series(0, 3) i)
SELECT o.i, count(x.k), sum(x.s) FROM o LEFT JOIN (SELECT k, count(*) c, sum(n) s FROM ah GROUP BY k) x
  ON x.c >= o.i + 2 GROUP BY o.i;
SELECT agg_check($$WITH o AS MATERIALIZED (SELECT i FROM generate_series(0, 3) i)
SELECT o.i, count(x.k), sum(x.s) FROM o LEFT JOIN (SELECT k, count(*) c, sum(n) s FROM ah GROUP BY k) x
  ON x.c >= o.i + 2 GROUP BY o.i$$);
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;
RESET enable_memoize;
-- vexec.enable_repartition off: the plans without it
SET vexec.enable_repartition = off;
EXPLAIN (COSTS OFF) SELECT k, count(*), sum(n) FROM ah GROUP BY k;
RESET vexec.enable_repartition;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;

-- rescanned: in a correlated subquery, a parameter changing each time
SELECT agg_check($$SELECT o.k, (SELECT count(*) FROM at WHERE at.k = o.k AND at.id < 1000),
       (SELECT sum(b) FROM at WHERE at.s = o.k) FROM (SELECT DISTINCT k FROM at) o$$);
SELECT agg_check($$SELECT o.k, (SELECT max(t) FROM at WHERE at.id % 10 = o.k GROUP BY at.s
       ORDER BY 1 LIMIT 1) FROM (SELECT DISTINCT k FROM at) o$$);

-- rows in from a row node, which is never called past its end: an index
-- scan called again would begin again
CREATE INDEX at_k ON at (k, id);
SET enable_seqscan = off;
SELECT agg_check('SELECT count(*) FROM at WHERE k = 3 AND id < 50');
SELECT agg_check('SELECT k, count(*) FROM at WHERE k IN (3, 4) GROUP BY k');
RESET enable_seqscan;
DROP INDEX at_k;

-- H3: count(DISTINCT x) in two VecAggs: the lower groups by the keys and
-- x, the other aggregates' partial states beside; the upper counts x once a
-- distinct value and combines the others
EXPLAIN (COSTS OFF) SELECT k, count(DISTINCT s), sum(n) FROM at GROUP BY k;
SELECT agg_check('SELECT k, count(DISTINCT s), sum(n), count(*), avg(b), max(t), min(d) FROM at GROUP BY k');
SELECT agg_check('SELECT count(DISTINCT t), count(t), count(*) FROM at');
SELECT agg_check('SELECT k, count(DISTINCT t) FROM at GROUP BY k HAVING count(DISTINCT t) > 3');
SELECT agg_check('SELECT count(DISTINCT u), sum(u) FROM at');
SELECT agg_check('SELECT count(DISTINCT s) FROM ae');
SELECT agg_check('SELECT k, count(DISTINCT s) FROM ae GROUP BY k');
SELECT agg_check('SELECT id % 100, count(DISTINCT k), count(DISTINCT k) + 1 FROM at GROUP BY 1');
-- left to PostgreSQL's Agg: any other DISTINCT aggregate, one beside a
-- float's sum, which a second stage would add in another order, ORDER BY in
-- an aggregate, ordered-set aggregates, grouping sets
SELECT agg_check('SELECT k, sum(DISTINCT s) FROM at GROUP BY k');
SELECT agg_check('SELECT k, count(DISTINCT s), avg(f) FROM at GROUP BY k');
SELECT agg_check('SELECT k, count(DISTINCT s), count(DISTINCT t) FROM at GROUP BY k');
EXPLAIN (VEXEC, COSTS OFF) SELECT k, count(DISTINCT s), avg(f) FROM at GROUP BY k;
SELECT agg_check('SELECT k, string_agg(t, '','' ORDER BY id) FROM at GROUP BY k');
SELECT agg_check('SELECT percentile_disc(0.5) WITHIN GROUP (ORDER BY id) FROM at');
SELECT agg_check('SELECT k, s, count(*) FROM at GROUP BY GROUPING SETS ((k), (s))');

-- explain mode: costed, recorded, never built
SET vexec.mode = explain;
EXPLAIN (COSTS OFF) SELECT k, count(*) FROM at GROUP BY k;

DROP FUNCTION agg_check(text);
DROP FUNCTION agg_error(text);
DROP FUNCTION agg_explain(text, text);
DROP TABLE at, ae, an, ak, ah;
DROP SEQUENCE aseq;
