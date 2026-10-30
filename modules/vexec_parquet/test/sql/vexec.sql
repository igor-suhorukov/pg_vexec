-- vexec over tables of Parquet files: the batch source (scans, answers from
-- statistics, row groups passed over) and the sink (VecInsert), in both
-- batch formats, each answer the plain executor's
CREATE TABLE facts (id int8, grp int4, name text, amount numeric(12,2), ratio float8, day date,
	ts timestamp, flag bool, u uuid) USING vexec_parquet WITH (row_group_rows = 10000);
INSERT INTO facts SELECT i, i % 10, 'name ' || (i % 1000), i * 0.01, i / 3.0,
	date '2020-01-01' + (i % 1000), timestamp '2020-01-01' + i * interval '1 minute', i % 3 = 0,
	('00000000-0000-0000-0000-' || lpad(to_hex(i), 12, '0'))::uuid
FROM generate_series(1, 100000) i;
-- a deleted row and an updated one, and the last rows of the last unit --
-- whose last batch, 784 rows, ends inside a word of its visible bits
DELETE FROM facts WHERE id = 7;
UPDATE facts SET amount = -1 WHERE id = 8;
DELETE FROM facts WHERE id > 99990;
ANALYZE facts;
CREATE TEMP TABLE answers (fmt text, q int4, answer text);

SET vexec.mode = force;
EXPLAIN (VERBOSE, COSTS OFF) SELECT grp, count(*), sum(amount) FROM facts GROUP BY grp;
-- count, min and max asked of the source, which answers from its files'
-- statistics for the units no row of which is deleted
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*), min(id), max(day) FROM facts;
-- a text's minimum and maximum too, under C's collation, which orders it as
-- its statistics do (pg_vector_executor.md §3.36, "Universal?"): the planner
-- priced them answered, and VecAgg asked the source none, reading every unit
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT min(name), max(name), count(name) FROM facts;
-- row groups whose statistics no row of which meets the quals passed over
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
	SELECT count(*), sum(amount) FROM facts WHERE id BETWEEN 25000 AND 25100;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
	SELECT count(*) FROM facts WHERE u = '00000000-0000-0000-0000-00000000ffff';

DO $$
DECLARE
	fmt text;
	qs text[] := ARRAY[
		'SELECT grp, count(*), sum(amount), avg(ratio), min(name), max(day) FROM facts GROUP BY grp ORDER BY grp',
		'SELECT count(*), min(id), max(id), min(day), max(ts), sum(amount) FROM facts',
		'SELECT count(*), sum(amount) FROM facts WHERE id BETWEEN 25000 AND 25100',
		'SELECT id, amount, name FROM facts WHERE id IN (6, 7, 8, 9) ORDER BY id',
		'SELECT count(*) FROM facts WHERE flag AND name LIKE ''name 1%''',
		'SELECT day, count(*) FROM facts WHERE day < ''2020-01-05'' GROUP BY day ORDER BY day',
		'SELECT count(*) FROM facts WHERE u = ''00000000-0000-0000-0000-00000000ffff''',
		'SELECT name, count(DISTINCT grp) FROM facts GROUP BY name ORDER BY count(*) DESC, name LIMIT 3',
		'SELECT f.grp, count(*) FROM facts f JOIN facts g ON f.id = g.id + 1 WHERE f.id < 1000 GROUP BY f.grp ORDER BY 1',
		'SELECT min(name), max(name), count(name), count(*) FROM facts'];
	r record;
BEGIN
	FOREACH fmt IN ARRAY ARRAY['off', 'postgres', 'arrow'] LOOP
		IF fmt = 'off' THEN
			SET LOCAL vexec.mode = off;
		ELSE
			SET LOCAL vexec.mode = force;
			EXECUTE format('SET LOCAL vexec.batch_format = %L', fmt);
		END IF;
		FOR i IN 1 .. array_length(qs, 1) LOOP
			EXECUTE format('INSERT INTO answers SELECT %L, %s, string_agg(t::text, %L) FROM (%s) t', fmt, i, ' ', qs[i]);
		END LOOP;
	END LOOP;
END $$;
-- every format's answers the plain executor's
SELECT a.fmt, a.q FROM answers a JOIN answers o ON o.fmt = 'off' AND o.q = a.q
WHERE a.fmt <> 'off' AND a.answer IS DISTINCT FROM o.answer ORDER BY 1, 2;
SELECT q, left(answer, 120) FROM answers WHERE fmt = 'off' ORDER BY q;

-- parallel workers read units of their own
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
EXPLAIN (COSTS OFF) SELECT grp, count(*) FROM facts WHERE ratio > 10 GROUP BY grp;
SELECT grp, count(*) FROM facts WHERE ratio > 10 GROUP BY grp ORDER BY grp;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

-- the sink: INSERT ... SELECT through VecInsert, in both formats
CREATE TABLE copied (LIKE facts) USING vexec_parquet;
EXPLAIN (COSTS OFF) INSERT INTO copied SELECT * FROM facts WHERE grp = 1;
INSERT INTO copied SELECT * FROM facts WHERE grp = 1;
SET vexec.batch_format = arrow;
INSERT INTO copied SELECT * FROM facts WHERE grp = 2;
RESET vexec.batch_format;
SET vexec.mode = off;
SELECT count(*) AS differ FROM ((TABLE copied EXCEPT ALL SELECT * FROM facts WHERE grp IN (1, 2))
	UNION ALL (SELECT * FROM facts WHERE grp IN (1, 2) EXCEPT ALL TABLE copied)) x;
SET vexec.mode = force;
-- the rows the sink wrote: updated, deleted, read back
UPDATE copied SET name = 'changed' WHERE id < 100;
DELETE FROM copied WHERE id > 99000;
SELECT count(*), count(*) FILTER (WHERE name = 'changed') FROM copied;
-- a table that reads files through the source
SELECT regexp_replace(min(path), '/[^/]+$', '') AS dir FROM vexec_parquet.files('facts') \gset
CREATE TABLE facts_ext (id int8, grp int4, amount numeric(12,2), day date) USING vexec_parquet
	WITH (location = :'dir');
EXPLAIN (COSTS OFF) SELECT count(*), max(day) FROM facts_ext;
SELECT count(*), max(day) FROM facts_ext;
SELECT grp, count(*), sum(amount) FROM facts_ext WHERE id < 100 GROUP BY grp ORDER BY grp;
-- a later step's column read for the row a fallback step loaded last, the
-- only one its qual passed: the row loaded again with the step (vexec's
-- load_steps()), not its placeholder's NULL
CREATE TABLE stepped (k int4, nm name, t text) USING vexec_parquet;
INSERT INTO stepped VALUES (1, 'bob', 'first'), (2, 'carol', 'second');
CREATE FUNCTION stepped_seen(text) RETURNS bool COST 1000 LANGUAGE plpgsql AS 'BEGIN RETURN $1 IS NOT NULL; END';
EXPLAIN (VERBOSE, COSTS OFF) SELECT k, t FROM stepped WHERE nm = 'carol' AND stepped_seen(t);
SELECT k, t FROM stepped WHERE nm = 'carol' AND stepped_seen(t);
RESET vexec.mode;
