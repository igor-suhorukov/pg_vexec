-- SPDX-License-Identifier: Apache-2.0
--
-- Heap's page reader and the partial paths (pg_vector_executor.md §3.5.2,
-- §3.3.3, V4).  A VecScan of a heap table reads it a page at a time, its
-- visible tuples deformed straight into the batch; each query runs off, in
-- force mode, and in force mode through the slot path, and the three
-- answers are compared row for row.  Then the same with workers: partial
-- VecScans and VecHashJoins under a Gather, every row read once.

CREATE TABLE hp (id int4, s int2, b int8, n numeric(12,3), f float8, bo bool, d date,
                 ts timestamp, iv interval, u uuid, t text, c char(5), v varchar(40),
                 j jsonb, a int4[], gone text, nn numeric);
-- NULLs at the bitmaps' word edges: rows 63, 64, 65, 127, 128, ...
INSERT INTO hp
  SELECT g, (g % 300)::int2, g::int8 * 1000003, (g % 997)::numeric / 7,
         CASE WHEN g % 64 IN (0, 63) THEN NULL ELSE (g % 113)::float8 / 3 END,
         CASE WHEN g % 64 = 1 THEN NULL ELSE g % 3 = 0 END,
         date '2020-01-01' + (g % 900), timestamp '2021-06-01 12:00' + g * interval '17 minutes',
         (g % 50) * interval '1 hour 3 minutes', md5(g::text)::uuid,
         CASE WHEN g % 64 = 63 THEN NULL ELSE repeat(chr(97 + g % 26), g % 140) END,
         (g % 1000)::text, 'v' || (g % 777),
         CASE WHEN g % 5 = 0 THEN NULL ELSE jsonb_build_object('g', g, 'k', g % 9) END,
         ARRAY[g, g % 7], 'x', CASE WHEN g % 4 = 0 THEN 'NaN' ELSE g::numeric / 3 END
  FROM generate_series(1, 9000) g;
-- a dropped column, and columns added after rows were written: those rows
-- have none, and give the column's missing value
ALTER TABLE hp DROP COLUMN gone;
ALTER TABLE hp ADD COLUMN added int4 DEFAULT 42;
ALTER TABLE hp ADD COLUMN added_t text DEFAULT 'dflt';
INSERT INTO hp (id, t, added, added_t) SELECT g, 'late' || g, g, NULL FROM generate_series(9001, 9300) g;
-- dead tuples: updated, HOT and not, and deleted rows
UPDATE hp SET t = t || '!' WHERE id % 10 = 0;
UPDATE hp SET s = s + 1 WHERE id % 13 = 0;
DELETE FROM hp WHERE id % 17 = 0;
-- long values: compressed in the row, and stored out of line
INSERT INTO hp (id, t) SELECT 100000 + g, repeat(md5(g::text), 400) FROM generate_series(1, 20) g;
INSERT INTO hp (id, t)
  SELECT 200000 + g, (SELECT string_agg(md5((g * 1000 + i)::text), '') FROM generate_series(1, 80) i)
  FROM generate_series(1, 20) g;
-- rows a page each, more pages than a batch points into
CREATE TABLE hw (id int4, k int4, w text);
ALTER TABLE hw ALTER COLUMN w SET STORAGE PLAIN;
INSERT INTO hw SELECT g, g % 7, repeat(chr(65 + g % 26), 3000) || g FROM generate_series(1, 2500) g;
ANALYZE hp, hw;

-- off, force, and force through the slot path, compared: 'same' and
-- whether force's plan has a vector scan, a parallel one
CREATE FUNCTION heap_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	n bigint;
	m bigint;
	plan text := '';
	r record;
BEGIN
	PERFORM set_config('vexec.mode', 'off', true);
	EXECUTE 'CREATE TEMP TABLE heap_off AS SELECT q::text AS r FROM (' || q || ') q';
	PERFORM set_config('vexec.mode', 'force', true);
	FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || r."QUERY PLAN" || E'\n';
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE heap_on AS SELECT q::text AS r FROM (' || q || ') q';
	PERFORM set_config('vexec.heap_page_reader', 'off', true);
	EXECUTE 'CREATE TEMP TABLE heap_slots AS SELECT q::text AS r FROM (' || q || ') q';
	PERFORM set_config('vexec.heap_page_reader', 'on', true);
	EXECUTE 'SELECT count(*) FROM ((TABLE heap_off EXCEPT ALL TABLE heap_on) UNION ALL '
		'(TABLE heap_on EXCEPT ALL TABLE heap_off)) d' INTO n;
	EXECUTE 'SELECT count(*) FROM ((TABLE heap_off EXCEPT ALL TABLE heap_slots) UNION ALL '
		'(TABLE heap_slots EXCEPT ALL TABLE heap_off)) d' INTO m;
	DROP TABLE heap_off;
	DROP TABLE heap_on;
	DROP TABLE heap_slots;
	RETURN CASE WHEN n = 0 AND m = 0 THEN 'same'
		ELSE n || ' rows differ from pages, ' || m || ' from slots' END ||
		CASE WHEN plan ~ 'Parallel Vec Seq Scan' THEN ', parallel vector scan'
		WHEN plan ~ 'Vec Bitmap Heap Scan' THEN ', vector bitmap scan'
		WHEN plan ~ 'Vec Seq Scan' THEN ', vector scan' ELSE ', no vector scan' END ||
		CASE WHEN plan ~ 'Parallel Vec Seq Scan' AND plan ~ 'Vec Hash [A-Za-z ]*Join' THEN ' and join'
		ELSE '' END;
END $$;

-- the rows a bitmap's conditions and the filter remove, which EXPLAIN
-- ANALYZE counts apart, off and in force mode
CREATE FUNCTION heap_removed(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	js json;
	res text[];
	mode text;
BEGIN
	FOREACH mode IN ARRAY ARRAY['off', 'force'] LOOP
		PERFORM set_config('vexec.mode', mode, true);
		EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q
			INTO js;
		res := res || (coalesce(jsonb_path_query_first(js::jsonb,
							'strict $.**."Rows Removed by Index Recheck"')::text, 'none') ||
			' by the recheck, ' ||
			coalesce(jsonb_path_query_first(js::jsonb, 'strict $.**."Rows Removed by Filter"')::text,
					 'none') || ' by the filter');
	END LOOP;
	RETURN CASE WHEN res[1] = res[2] THEN 'both ' || res[1]
		ELSE 'off ' || res[1] || ', force ' || res[2] END;
END $$;

-- EXPLAIN ANALYZE lines of force mode's plan, numbers masked
CREATE FUNCTION heap_explain(q text, pattern text) RETURNS SETOF text LANGUAGE plpgsql AS $$
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
SET max_parallel_workers_per_gather = 0;

-- heap's pages are the source; the slot path where the setting is off
EXPLAIN (COSTS OFF, VERBOSE) SELECT id, t FROM hp WHERE id < 10;
SET vexec.heap_page_reader = off;
EXPLAIN (COSTS OFF, VERBOSE) SELECT id, t FROM hp WHERE id < 10;
RESET vexec.heap_page_reader;

-- every column, every type class, NULLs, missing values, dead rows
SELECT heap_check('SELECT * FROM hp');
SELECT heap_check('SELECT id, t, c, v, added, added_t FROM hp WHERE id % 7 = 1');
SELECT heap_check('SELECT id, n, nn, f, iv, u, j, a FROM hp WHERE s > 150');
SELECT heap_check($$SELECT count(*), sum(id), count(t), sum(length(t)), count(f), sum(f), count(bo),
       sum(added), count(added_t), min(d), max(ts) FROM hp$$);
-- the columns of a tuple's tail only, and its head only
SELECT heap_check('SELECT added_t, nn FROM hp');
SELECT heap_check('SELECT id, s FROM hp WHERE id BETWEEN 60 AND 200');
-- system columns: ctid from each tuple's place, tableoid from the relation
SELECT heap_check('SELECT ctid, id FROM hp WHERE id < 300 OR id > 99999');
SELECT heap_check('SELECT tableoid::regclass, count(*) FROM hp GROUP BY 1');
-- long values: a compressed value and a pointer out of line keep their
-- stored forms, and are read when a kernel or a row needs their bytes
SELECT heap_check($$SELECT id, pg_column_compression(t), length(t), md5(t) FROM hp WHERE id > 99999$$);
SELECT heap_check($$SELECT id FROM hp WHERE t LIKE '%fa3%' AND id > 99999$$);
-- rows a page each: past the pages a batch points into, values are copied
SELECT heap_check('SELECT id, w FROM hw WHERE k = 3');
SELECT heap_check('SELECT k, count(*), sum(length(w)), max(w) FROM hw GROUP BY k');
-- a scan that reads no column only counts each page's visible tuples (H1)
SELECT heap_check('SELECT count(*) FROM hp');
SELECT heap_check('SELECT count(*) FROM hw');
SELECT heap_explain('SELECT count(*) FROM hp', 'Vec Seq Scan|Output|Source|Heap Pages|Batches');
-- batch edges: 1023, 1024 and 1025 rows
SELECT heap_check('SELECT * FROM hp WHERE id <= 1023 + (SELECT count(*) FROM hp WHERE id <= 1023 AND id % 17 = 0)');
SELECT heap_check('SELECT count(*), sum(id) FROM (SELECT id FROM hp ORDER BY id LIMIT 1024) q');
SELECT heap_check('SELECT count(*), sum(id) FROM (SELECT id FROM hp ORDER BY id LIMIT 1025) q');

-- this transaction's own rows, those it deleted, a savepoint rolled back;
-- and SERIALIZABLE, whose checks heap makes as it prepares a page
BEGIN;
INSERT INTO hp (id, t) VALUES (-1, 'mine'), (-2, NULL);
DELETE FROM hp WHERE id BETWEEN 2 AND 9;
UPDATE hp SET t = 'changed' WHERE id = 11;
SAVEPOINT p;
INSERT INTO hp (id) VALUES (-3);
ROLLBACK TO p;
SELECT heap_check('SELECT id, t FROM hp WHERE id < 20');
SELECT heap_check('SELECT count(*), sum(id) FROM hp');
ROLLBACK;
BEGIN ISOLATION LEVEL SERIALIZABLE;
SELECT heap_check('SELECT count(*), sum(id), sum(length(t)) FROM hp');
COMMIT;

-- rescanned, as a nested loop's inner side, its pages read again
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SET enable_material = off;
SELECT heap_check($$SELECT o.id, count(*), sum(i.id) FROM (SELECT id FROM hp WHERE id < 40) o
       JOIN hw i ON i.k = o.id % 7 GROUP BY o.id$$);
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

-- the batch formats: what the page reader writes, converted
SET vexec.batch_format = arrow;
SELECT heap_check('SELECT * FROM hp WHERE id % 3 = 0');
SELECT heap_check('SELECT id, pg_column_compression(t), length(t) FROM hp WHERE id > 99999');
RESET vexec.batch_format;
SET vexec.batch_numeric_layout = varlena;
SELECT heap_check('SELECT id, n, nn FROM hp WHERE id % 5 = 0');
RESET vexec.batch_numeric_layout;

-- EXPLAIN ANALYZE: the pages read
SELECT heap_explain('SELECT id, t FROM hp WHERE id % 100 = 0', 'Vec Seq Scan|Source|Heap Pages');

-- in parallel: partial VecScans, each participant's share of the pages;
-- partial VecHashJoins over them, the inner side read whole by each; the
-- partial and final VecAggs; every row read once
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 3;
EXPLAIN (COSTS OFF) SELECT count(*), sum(id), sum(length(t)) FROM hp;
EXPLAIN (COSTS OFF) SELECT hw.k, count(*), sum(hp.b) FROM hp JOIN hw ON hp.id = hw.id GROUP BY hw.k;
EXPLAIN (COSTS OFF) SELECT hp.id FROM hp WHERE NOT EXISTS (SELECT 1 FROM hw WHERE hw.id = hp.s);
SELECT heap_check('SELECT * FROM hp');
SELECT heap_check('SELECT count(*), sum(id), sum(length(t)), count(added), sum(nn) FROM hp');
SELECT heap_check('SELECT count(*) FROM hp');
SELECT heap_check('SELECT ctid, id, t FROM hp WHERE id % 11 = 1');
SELECT heap_check('SELECT hw.k, count(*), sum(hp.b) FROM hp JOIN hw ON hp.id = hw.id GROUP BY hw.k');
SELECT heap_check('SELECT hp.id, hw.w FROM hp LEFT JOIN hw ON hp.id = hw.id AND hw.k < 3');
SELECT heap_check('SELECT hp.id FROM hp WHERE EXISTS (SELECT 1 FROM hw WHERE hw.id = hp.s)');
SELECT heap_check('SELECT hp.id FROM hp WHERE NOT EXISTS (SELECT 1 FROM hw WHERE hw.id = hp.s)');
SELECT heap_check('SELECT t, count(*), max(id) FROM hp GROUP BY t');
SELECT heap_explain('SELECT count(*), sum(id) FROM hp', 'Parallel|Gather|Workers Planned');
-- a Gather rescanned: its participants' shares made again
SELECT heap_check($$SELECT o.k, (SELECT count(*) FROM hp WHERE hp.s = o.k) FROM
       (SELECT DISTINCT k FROM hw) o$$);
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET max_parallel_workers_per_gather;

-- bitmap heap scans (H9): VecBitmapHeapScan over a bitmap index scan, a
-- BitmapAnd and a BitmapOr, its pages through heap's page reader, the
-- bitmap's conditions rechecked, the rows they remove counted apart; lossy
-- pages past work_mem; under a pseudoconstant's gate; and as a nested
-- loop's inner side, its bitmap made again for each outer row
CREATE INDEX hp_s ON hp (s);
CREATE INDEX hp_d ON hp (d);
ANALYZE hp;
SET enable_seqscan = off;
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
EXPLAIN (COSTS OFF) SELECT count(*), sum(id) FROM hp WHERE s = 7;
EXPLAIN (COSTS OFF) SELECT id FROM hp WHERE s < 20 AND d = '2020-01-05';
EXPLAIN (COSTS OFF) SELECT id FROM hp WHERE s = 9 OR d = '2020-02-01';
SELECT heap_check('SELECT * FROM hp WHERE s = 7');
SELECT heap_check($$SELECT id, t, n FROM hp WHERE s < 20 AND d = '2020-01-05'$$);
SELECT heap_check($$SELECT id, t FROM hp WHERE s = 9 OR d = '2020-02-01'$$);
SELECT heap_check('SELECT count(*), sum(id) FROM hp WHERE s BETWEEN 10 AND 200');
SELECT heap_check('SELECT count(*) FROM hp WHERE s BETWEEN 10 AND 200');
SELECT heap_check($$SELECT ctid, id FROM hp WHERE s = 33 AND t LIKE '%a%'$$);
SET work_mem = '64kB';
SELECT heap_check('SELECT id, length(t), added FROM hp WHERE s BETWEEN 10 AND 200');
SELECT heap_explain('SELECT id, length(t) FROM hp WHERE s BETWEEN 10 AND 200', 'Vec Bitmap|Recheck|Heap Blocks|Source');
SELECT heap_removed($$SELECT id FROM hp WHERE s BETWEEN 10 AND 200 AND t LIKE '%b%'$$);
SELECT heap_removed('SELECT count(*) FROM hp WHERE s = 9 OR d = ''2020-02-01''');
-- a bitmap of more pages than work_mem holds keeps some of them whole,
-- and their rows are rechecked
CREATE TABLE hb AS SELECT g AS id, g % 100 AS a, g % 7 AS b FROM generate_series(1, 300000) g;
CREATE INDEX hb_a ON hb (a);
ANALYZE hb;
SET max_parallel_workers_per_gather = 0;
SELECT heap_check('SELECT count(*), sum(id) FROM hb WHERE a = 5 AND b < 3');
SELECT heap_removed('SELECT count(*) FROM hb WHERE a = 5 AND b < 3');
SELECT heap_explain('SELECT count(*) FROM hb WHERE a = 5 AND b < 3', 'Recheck|Heap Blocks');
RESET max_parallel_workers_per_gather;
DROP TABLE hb;
RESET work_mem;
SELECT heap_removed('SELECT id FROM hp WHERE s = 7 AND id % 3 = 0');
SELECT heap_check($$SELECT id FROM hp WHERE s = 7 AND now() > '2000-01-01'$$);
SELECT heap_check('SELECT k, (SELECT count(*) FROM hp WHERE hp.s = 7 AND o.k > 2) FROM hw o WHERE k < 5');
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT heap_check($$SELECT o.k, count(hp.id), sum(hp.id) FROM (SELECT DISTINCT k FROM hw WHERE k < 5) o
       JOIN hp ON hp.s = o.k GROUP BY o.k$$);
RESET enable_hashjoin;
RESET enable_mergejoin;
SET vexec.batch_format = arrow;
SELECT heap_check('SELECT * FROM hp WHERE s = 7 OR s = 8');
RESET vexec.batch_format;
SET vexec.enable_bitmapscan = off;
EXPLAIN (COSTS OFF) SELECT count(*), sum(id) FROM hp WHERE s = 7;
RESET vexec.enable_bitmapscan;
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
DROP INDEX hp_s;
DROP INDEX hp_d;

DROP FUNCTION heap_check(text);
DROP FUNCTION heap_removed(text);
DROP FUNCTION heap_explain(text, text);
DROP TABLE hp, hw;
