-- SPDX-License-Identifier: Apache-2.0
--
-- VecSort (pg_vector_executor.md §3.8, V4): the query's ORDER BY, with the
-- LIMIT's bound where one is given, against PostgreSQL's own Sort.  Each
-- query runs off and in force mode, and its answers are compared row for
-- row, in their order.

CREATE TABLE so AS
  SELECT g AS id, (g * 7919) % 10007 AS k,
         CASE WHEN g % 11 = 0 THEN NULL ELSE (g % 97)::float8 / 7 END AS f,
         CASE WHEN g % 13 = 0 THEN NULL ELSE 'v' || (g % 313) END AS t,
         date '2020-01-01' + (g % 400) AS d, (g % 1000)::numeric(8,2) / 3 AS n,
         g % 5 = 0 AS bo, (g % 50)::int2 AS s
  FROM generate_series(1, 30000) g;
CREATE TABLE sd AS SELECT g AS k, 'name' || g AS name FROM generate_series(0, 10006, 3) g;
ANALYZE so, sd;

-- off and force, compared in order: 'same' and whether force's plan sorts
-- with a VecSort, bounded or not
CREATE FUNCTION sort_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	n bigint;
	plan text := '';
	r record;
BEGIN
	PERFORM set_config('vexec.mode', 'off', true);
	EXECUTE 'CREATE TEMP TABLE sort_off AS SELECT row_number() OVER () AS rn, q::text AS r FROM (' ||
		q || ') q';
	PERFORM set_config('vexec.mode', 'force', true);
	FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || r."QUERY PLAN" || E'\n';
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE sort_on AS SELECT row_number() OVER () AS rn, q::text AS r FROM (' ||
		q || ') q';
	EXECUTE 'SELECT count(*) FROM ((TABLE sort_off EXCEPT ALL TABLE sort_on) UNION ALL '
		'(TABLE sort_on EXCEPT ALL TABLE sort_off)) d' INTO n;
	DROP TABLE sort_off;
	DROP TABLE sort_on;
	RETURN CASE WHEN n = 0 THEN 'same' ELSE n || ' rows differ' END ||
		CASE WHEN plan ~ 'Bound:' THEN ', bounded VecSort'
		WHEN plan ~ 'Vec Sort' THEN ', VecSort' ELSE ', no VecSort' END;
END $$;

-- a query's answer or its error, off and in force mode
CREATE FUNCTION sort_error(q text) RETURNS text LANGUAGE plpgsql AS $$
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

-- EXPLAIN ANALYZE lines of force mode's plan, numbers masked
CREATE FUNCTION sort_explain(q text, pattern text) RETURNS SETOF text LANGUAGE plpgsql AS $$
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

-- the sort, over a vector scan's batches; with a LIMIT, bounded
EXPLAIN (COSTS OFF) SELECT id, k FROM so ORDER BY k, id;
EXPLAIN (COSTS OFF) SELECT id, k FROM so ORDER BY k DESC, id LIMIT 10 OFFSET 5;
EXPLAIN (COSTS OFF, VERBOSE) SELECT id, f, t FROM so ORDER BY f NULLS FIRST, t COLLATE "C" DESC, id;

-- keys of each kind, both directions, NULLs first and last, collations
SELECT sort_check('SELECT id, k FROM so ORDER BY k, id');
SELECT sort_check('SELECT id, f FROM so ORDER BY f, id');
SELECT sort_check('SELECT id, f FROM so ORDER BY f DESC, id');
SELECT sort_check('SELECT id, f FROM so ORDER BY f NULLS FIRST, id DESC');
SELECT sort_check('SELECT id, f FROM so ORDER BY f DESC NULLS LAST, id');
SELECT sort_check('SELECT id, t FROM so ORDER BY t, id');
SELECT sort_check('SELECT id, t FROM so ORDER BY t COLLATE "C" DESC, id');
SELECT sort_check('SELECT id, n, d FROM so ORDER BY n DESC, d, id');
SELECT sort_check('SELECT id, bo, s FROM so ORDER BY bo, s DESC, id');
SELECT sort_check($$SELECT x FROM (VALUES (0.0::float8), (-0.0), ('NaN'), ('Infinity'), ('-Infinity'),
       (NULL), (1), ('NaN')) v(x) ORDER BY x$$);
SELECT sort_check($$SELECT x FROM (VALUES ('a '::bpchar), ('a'), ('A'), ('b  '), (NULL), ('')) v(x)
       ORDER BY x, x::text$$);
-- keys the select list lacks, and expressions
SELECT sort_check('SELECT id FROM so ORDER BY k % 100, f, id');
SELECT sort_check('SELECT id, t FROM so ORDER BY length(t) DESC NULLS LAST, id');
-- a projection over the sorted rows: the final target, evaluated as the
-- sorted rows come out
SELECT sort_check('SELECT id * 2 AS x, upper(t) AS u, f + 1 AS g FROM so ORDER BY k, id');

-- bounded: LIMIT and OFFSET; a bound past the rows; LIMIT 0 and ALL
SELECT sort_check('SELECT id, k FROM so ORDER BY k, id LIMIT 7');
SELECT sort_check('SELECT id, k FROM so ORDER BY k DESC, id LIMIT 10 OFFSET 5');
SELECT sort_check('SELECT id, f FROM so ORDER BY f NULLS FIRST, id LIMIT 3000');
SELECT sort_check('SELECT id FROM so ORDER BY k, id LIMIT 40000');
SELECT sort_check('SELECT id FROM so ORDER BY k, id OFFSET 29990');
SELECT sort_check('SELECT id FROM so ORDER BY k, id LIMIT 0');
SELECT sort_check('SELECT id FROM so ORDER BY k, id LIMIT ALL');
SELECT sort_check('SELECT id FROM so ORDER BY k, id LIMIT NULL');
-- the bound's edge: 1023, 1024 and 1025 rows, a batch's
SELECT sort_check('SELECT id, k FROM so ORDER BY k, id LIMIT 1023');
SELECT sort_check('SELECT id, k FROM so ORDER BY k, id LIMIT 1024');
SELECT sort_check('SELECT id, k FROM so ORDER BY k, id LIMIT 1025');
-- not bounded where rows past the bound may be read: WITH TIES, and a
-- set-returning function after the sort
SELECT sort_check('SELECT id, s FROM so ORDER BY s FETCH FIRST 5 ROWS WITH TIES');
SELECT sort_check('SELECT id, generate_series(1, s % 3) FROM so ORDER BY k, id LIMIT 12');
-- a LIMIT of a parameter: a custom plan's bound, a generic plan's none
PREPARE sl(int8) AS SELECT id, k FROM so ORDER BY k, id LIMIT $1;
SET plan_cache_mode = force_custom_plan;
EXPLAIN (COSTS OFF) EXECUTE sl(3);
EXECUTE sl(3);
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE sl(3);
EXECUTE sl(4);
RESET plan_cache_mode;
DEALLOCATE sl;

-- over an aggregation: the counts ordered, the first few kept
SELECT sort_check('SELECT t, count(*), sum(k) FROM so GROUP BY t ORDER BY count(*) DESC, t LIMIT 10');
SELECT sort_check('SELECT s, avg(n) FROM so GROUP BY s ORDER BY avg(n) DESC, s');
-- over a join, and its columns of both sides
SELECT sort_check('SELECT so.id, sd.name FROM so JOIN sd ON so.k = sd.k ORDER BY sd.name, so.id LIMIT 50');
SELECT sort_check('SELECT DISTINCT s, bo FROM so ORDER BY bo, s');
SELECT sort_check($$SELECT id, k FROM so WHERE id < 100 UNION ALL SELECT k, k FROM sd
       ORDER BY 2, 1 LIMIT 20$$);

-- volatile functions in the target list run after the sort, for the rows
-- that come out, as PostgreSQL runs them (make_sort_input_target())
CREATE SEQUENCE sseq;
SELECT id, nextval('sseq') > 0 FROM so ORDER BY k, id LIMIT 5;
SELECT currval('sseq');
-- errors: a key's error as the rows are sorted, and an expression of the
-- target list's only for the rows that come out
SELECT sort_error('SELECT id FROM so ORDER BY 100 / (k - 5000), id');
SELECT sort_error('SELECT id, 100 / (k - 5000) FROM so ORDER BY k LIMIT 3');
SELECT sort_error('SELECT id, 100 / (k - 1) FROM so ORDER BY k, id LIMIT 3');

-- rescanned: in a correlated subquery, each outer row's sort made again
SELECT sort_check($$SELECT sd.k, (SELECT so.id FROM so WHERE so.s = sd.k % 50 ORDER BY so.f DESC NULLS LAST,
       so.id LIMIT 1) FROM sd WHERE sd.k < 300 ORDER BY sd.k$$);

-- spilled past work_mem: an external sort; and the batch formats
SET work_mem = '64kB';
SELECT sort_check('SELECT id, t, n FROM so ORDER BY t, n, id');
SELECT sort_explain('SELECT id, t FROM so ORDER BY t, id', 'Vec Sort|Sort Key|Sort Method');
RESET work_mem;
SET vexec.batch_format = arrow;
SELECT sort_check('SELECT id, t, d, n, bo FROM so ORDER BY d DESC, t, id LIMIT 100');
SELECT sort_check('SELECT id, t, d, n, bo FROM so ORDER BY bo, n, id');
RESET vexec.batch_format;
SET vexec.batch_numeric_layout = varlena;
SELECT sort_check('SELECT id, n FROM so ORDER BY n, id LIMIT 33');
RESET vexec.batch_numeric_layout;

-- late columns (H6): a bounded sort over a scan sorts the keys and TIDs,
-- and fetches the rest of the rows it keeps by TID; a column the scan
-- computes, a whole row or a bound past a batch keeps the child's row
SELECT sort_check('SELECT * FROM so ORDER BY f DESC NULLS LAST, id LIMIT 12');
SELECT sort_check($$SELECT id, t, n, d FROM so WHERE t LIKE '%v1%' ORDER BY d, id LIMIT 30$$);
SELECT sort_check('SELECT * FROM so ORDER BY k, id OFFSET 5 LIMIT 1019');
SELECT sort_check('SELECT ctid, tableoid::regclass, id FROM so ORDER BY n, id LIMIT 4');
SELECT sort_explain('SELECT * FROM so ORDER BY f DESC NULLS LAST, id LIMIT 12',
                    'Vec Sort|Columns|Fetched|Output');
SELECT sort_explain('SELECT id * 2, t FROM so ORDER BY k, id LIMIT 3', 'Columns');
SELECT sort_explain('SELECT so, id FROM so ORDER BY k, id LIMIT 3', 'Columns');
SELECT sort_check('SELECT * FROM so ORDER BY k, id LIMIT 2000');
SELECT sort_explain('SELECT * FROM so ORDER BY k, id LIMIT 2000', 'Columns');
-- rows this transaction deleted: the sort and the fetch see one snapshot
BEGIN;
DELETE FROM so WHERE id % 1000 = 1;
SELECT sort_check('SELECT * FROM so ORDER BY f, id LIMIT 50');
ROLLBACK;
SET vexec.batch_format = arrow;
SELECT sort_check('SELECT * FROM so ORDER BY t, id LIMIT 33');
RESET vexec.batch_format;

-- in parallel: the partial input sorted in each participant, under a
-- Gather Merge, each one's sort bounded as the whole one is
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
EXPLAIN (COSTS OFF) SELECT id, t, n FROM so WHERE k < 5000 ORDER BY t, n, id LIMIT 20;
SELECT sort_check('SELECT id, t, n FROM so WHERE k < 5000 ORDER BY t, n, id LIMIT 20');
SELECT sort_check('SELECT id, t, n FROM so ORDER BY n DESC, t, id OFFSET 100 LIMIT 50');
SELECT sort_check('SELECT id, d, f FROM so WHERE s < 10 ORDER BY d, f NULLS FIRST, id');
SELECT sort_explain('SELECT id, t, n FROM so WHERE k < 5000 ORDER BY t, n, id LIMIT 20',
                    'Gather Merge|Vec Sort|Bound|Sort Method');
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
SET max_parallel_workers_per_gather = 0;

-- the running bound (H6): once the sort holds 2N+1 rows, its scan drops a
-- row whose first key is strictly past the first key of the sort's N-th,
-- a row tuplesort would discard as it came -- in heap's page reader before
-- the row is deformed, where none of the scan's quals may raise; after them
-- where one may -- and the rows are those the sort keeps without it, the
-- ones that tie at the bound's key among them
SELECT sort_explain('SELECT * FROM so ORDER BY k, id LIMIT 10',
                    'Running Bound|Rows Removed by Bound|Rows Sorted');
SELECT sort_check('SELECT id, k FROM so ORDER BY k, id LIMIT 10');
SELECT sort_check('SELECT id, s FROM so ORDER BY s LIMIT 25');
SELECT sort_check('SELECT id, s FROM so ORDER BY s DESC LIMIT 100');
SELECT sort_check('SELECT id, f FROM so ORDER BY f NULLS FIRST LIMIT 30');
SELECT sort_check('SELECT id, f FROM so ORDER BY f DESC NULLS LAST, id LIMIT 30');
SELECT sort_check('SELECT id, t FROM so ORDER BY t, id LIMIT 7 OFFSET 3');
SELECT sort_check('SELECT id, t FROM so ORDER BY t COLLATE "C" DESC LIMIT 40');
SELECT sort_check('SELECT id, d, n FROM so ORDER BY d, n LIMIT 60');
SELECT sort_check($$SELECT id, n FROM so WHERE t LIKE '%v1%' ORDER BY n, id LIMIT 20$$);
SELECT sort_explain($$SELECT id, n FROM so WHERE t LIKE '%v1%' ORDER BY n, id LIMIT 20$$,
                    'Running Bound');
-- after a qual that may raise: the row PostgreSQL raises on, past the
-- bound, raises
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM so WHERE 1000 / (id - 29990) > -1000 ORDER BY k LIMIT 5;
SELECT sort_error('SELECT id FROM so WHERE 1000 / (id - 29990) > -1000 ORDER BY k LIMIT 5');
SELECT sort_check('SELECT id, k FROM so WHERE 1000 / (id + 1) >= 0 ORDER BY k, id LIMIT 5');
-- a lazy qual: the bound once the row has passed it
SELECT sort_check('SELECT id, k FROM so WHERE random() >= 0 ORDER BY k, id LIMIT 8');
-- through the slot path, in the Arrow format, over a bitmap heap scan
SET vexec.heap_page_reader = off;
SELECT sort_check('SELECT id, k, t FROM so ORDER BY k, id LIMIT 10');
SELECT sort_explain('SELECT id, k, t FROM so ORDER BY k, id LIMIT 10', 'Rows Removed by Bound');
RESET vexec.heap_page_reader;
SET vexec.batch_format = arrow;
SELECT sort_check('SELECT id, t, n FROM so ORDER BY t DESC, n LIMIT 15');
RESET vexec.batch_format;
CREATE INDEX so_s ON so (s);
SET enable_seqscan = off;
SELECT sort_check('SELECT id, k FROM so WHERE s IN (3, 4) ORDER BY k, id LIMIT 6');
SELECT sort_explain('SELECT id, k FROM so WHERE s IN (3, 4) ORDER BY k, id LIMIT 6',
                    'Vec Bitmap|Running Bound|Rows Removed by Bound');
RESET enable_seqscan;
DROP INDEX so_s;
-- no bound: the setting off, or a first key the scan computes
SET vexec.enable_running_bound = off;
SELECT sort_explain('SELECT id, k FROM so ORDER BY k, id LIMIT 10', 'Running Bound|Rows Removed by Bound');
RESET vexec.enable_running_bound;
SELECT sort_explain('SELECT id, k FROM so ORDER BY k + 1, id LIMIT 10', 'Running Bound|Rows Removed by Bound');

-- EXPLAIN ANALYZE: a bounded sort's top-N heap, an unbounded one's quicksort
SELECT sort_explain('SELECT id, k FROM so ORDER BY k LIMIT 10', 'Vec Sort|Sort Method|Bound|Rows Sorted|Input');
SELECT sort_explain('SELECT id, k FROM so ORDER BY k', 'Vec Sort|Sort Method|Bound|Rows Sorted');

-- a nearest-neighbour search, a LIMIT over a sort by a distance an index
-- answers: VecSort where no index answers it; where one does, force mode
-- keeps the index's order beside VecSort, for cost to choose
EXPLAIN (COSTS OFF) SELECT id FROM so ORDER BY point(k, id) <-> point(0, 0) LIMIT 3;
SELECT sort_check('SELECT id FROM so ORDER BY point(k, id) <-> point(0, 0), id LIMIT 3');
CREATE TABLE sp AS SELECT g AS id, point(g % 1000, g / 1000) AS p FROM generate_series(1, 20000) g;
CREATE INDEX sp_p ON sp USING gist (p);
ANALYZE sp;
EXPLAIN (COSTS OFF) SELECT id FROM sp ORDER BY p <-> point(500.3, 10.7) LIMIT 5;
SELECT sort_check('SELECT id, p <-> point(500.3, 10.7) FROM sp ORDER BY p <-> point(500.3, 10.7) LIMIT 5');
DROP TABLE sp;

-- refused: an input already in its order -- a subquery's, which a VecSort
-- sorted; the setting off; explain mode
EXPLAIN (VEXEC, COSTS OFF) SELECT * FROM (SELECT id, k FROM so ORDER BY k) s ORDER BY k LIMIT 5;
SET vexec.enable_sort = off;
SELECT sort_check('SELECT id FROM so ORDER BY k, id LIMIT 5');
RESET vexec.enable_sort;
SET vexec.mode = explain;
EXPLAIN (COSTS OFF) SELECT id FROM so ORDER BY k, id LIMIT 5;

-- auto mode: a sort over vector input takes VecSort, a small one not
SET vexec.mode = auto;
EXPLAIN (COSTS OFF) SELECT id, k FROM so ORDER BY k, id;
EXPLAIN (COSTS OFF) SELECT k, name FROM sd WHERE k < 30 ORDER BY name;

DROP FUNCTION sort_check(text);
DROP FUNCTION sort_error(text);
DROP FUNCTION sort_explain(text, text);
DROP TABLE so, sd;
DROP SEQUENCE sseq;
