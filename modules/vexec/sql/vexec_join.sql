-- SPDX-License-Identifier: Apache-2.0
--
-- VecHashJoin (pg_vector_executor.md §3.8): hash joins -- inner, left,
-- semi, anti and right -- against PostgreSQL's own HashJoin.  Each query runs
-- off and in force mode and its answers are compared row for row; errors
-- are compared as they are raised.

CREATE TABLE jo AS
  SELECT g AS id, g % 100 AS k, (g % 50)::int2 AS s, g::int8 % 300 AS b,
         (g % 13)::numeric(10,2) / 4 AS n, (g % 11)::float8 / 3 AS f,
         CASE WHEN g % 7 = 0 THEN NULL ELSE 'v' || (g % 37) END AS t,
         date '2020-01-01' + (g % 40) AS d,
         CASE WHEN g % 9 = 0 THEN NULL ELSE g % 250 END AS nk,
         g % 5 = 0 AS bo
  FROM generate_series(1, 6000) g;
CREATE TABLE ji AS
  SELECT g AS id, g % 120 AS k, (g % 60)::int8 AS s, (g % 300)::int4 AS b,
         (g % 17)::numeric(10,2) / 4 AS n, (g % 13)::float8 / 3 AS f,
         CASE WHEN g % 11 = 0 THEN NULL ELSE 'v' || (g % 41) END AS t,
         date '2020-01-01' + (g % 60) AS d,
         CASE WHEN g % 8 = 0 THEN NULL ELSE g % 300 END AS nk,
         repeat('x', g % 50) AS pad
  FROM generate_series(1, 2500) g;
CREATE TABLE je (LIKE ji);
ANALYZE jo, ji, je;

-- off and force, compared: 'same' and whether force's plan has a VecHashJoin.
-- Rows compare as their text, which prints every bit of a float.
CREATE FUNCTION join_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	n bigint;
	plan text := '';
	r record;
BEGIN
	PERFORM set_config('vexec.mode', 'off', true);
	EXECUTE 'CREATE TEMP TABLE join_off AS SELECT q::text AS r FROM (' || q || ') q';
	PERFORM set_config('vexec.mode', 'force', true);
	FOR r IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
		plan := plan || r."QUERY PLAN" || E'\n';
	END LOOP;
	EXECUTE 'CREATE TEMP TABLE join_on AS SELECT q::text AS r FROM (' || q || ') q';
	EXECUTE 'SELECT count(*) FROM ((TABLE join_off EXCEPT ALL TABLE join_on) UNION ALL '
		'(TABLE join_on EXCEPT ALL TABLE join_off)) d' INTO n;
	DROP TABLE join_off;
	DROP TABLE join_on;
	RETURN CASE WHEN n = 0 THEN 'same' ELSE n || ' rows differ' END ||
		CASE WHEN plan ~ 'Vec Hash [A-Za-z ]*Join' THEN ', VecHashJoin' ELSE ', no VecHashJoin' END;
END $$;

-- a query's answer or its error, off and in force mode
CREATE FUNCTION join_error(q text) RETURNS text LANGUAGE plpgsql AS $$
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
CREATE FUNCTION join_explain(q text, pattern text) RETURNS SETOF text LANGUAGE plpgsql AS $$
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

-- the join types, each a VecHashJoin, its sides vector scans
EXPLAIN (COSTS OFF) SELECT jo.id, ji.b FROM jo JOIN ji ON jo.id = ji.id;
EXPLAIN (COSTS OFF) SELECT jo.id, ji.b FROM jo LEFT JOIN ji ON jo.id = ji.id;
EXPLAIN (COSTS OFF) SELECT jo.id FROM jo WHERE EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k AND ji.id > jo.id);
EXPLAIN (COSTS OFF) SELECT jo.id FROM jo WHERE NOT EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k);
EXPLAIN (COSTS OFF, VERBOSE) SELECT jo.id, ji.t FROM jo JOIN ji ON jo.k = ji.k AND jo.id < ji.id;

SELECT join_check('SELECT jo.id, jo.t, ji.id, ji.t FROM jo JOIN ji ON jo.id = ji.id');
SELECT join_check('SELECT jo.id, jo.t, ji.id, ji.t FROM jo LEFT JOIN ji ON jo.id = ji.id');
SELECT join_check('SELECT jo.id, jo.t, ji.id, ji.t FROM jo RIGHT JOIN ji ON jo.id = ji.id');
SELECT join_check('SELECT jo.* FROM jo WHERE EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k)');
SELECT join_check('SELECT jo.* FROM jo WHERE NOT EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k)');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.k = ji.k');
SELECT join_check('SELECT jo.id, ji.id FROM jo LEFT JOIN ji ON jo.k = ji.k');
SELECT join_check('SELECT jo.id, ji.id FROM jo RIGHT JOIN ji ON jo.k = ji.k');

-- NULL keys meet no row: dropped from an inner join, null-extended by an
-- outer join, kept by an anti join, at the end where PostgreSQL 19 keeps
-- them aside
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.nk = ji.nk');
SELECT join_check('SELECT jo.id, ji.id FROM jo LEFT JOIN ji ON jo.nk = ji.nk');
SELECT join_check('SELECT jo.id, ji.id FROM jo RIGHT JOIN ji ON jo.nk = ji.nk');
SELECT join_check('SELECT jo.id FROM jo WHERE EXISTS (SELECT 1 FROM ji WHERE ji.nk = jo.nk)');
SELECT join_check('SELECT jo.id FROM jo WHERE NOT EXISTS (SELECT 1 FROM ji WHERE ji.nk = jo.nk)');
SELECT join_check('SELECT jo.id, ji.id FROM jo FULL JOIN ji ON jo.nk = ji.nk');

-- keys of each kind: integers of other widths, bits, bytes, and through the
-- operators' own functions; several keys
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.s = ji.s');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.b = ji.b');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.d = ji.d AND jo.k = ji.k');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.t = ji.t');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.t COLLATE "C" = ji.t');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.t::varchar = ji.t::varchar');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.n = ji.n');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.f = ji.f');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.t::bytea = ji.t::bytea AND jo.s = ji.s');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.bo = (ji.id % 5 = 0) AND jo.k = ji.k');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON (jo.t || jo.k) = (ji.t || ji.k)');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.k + 1 = ji.k AND jo.d = ji.d - 1');
SELECT join_check($$SELECT x.v, y.v FROM (VALUES (0.0::float8), (-0.0), ('NaN'), ('NaN'),
       ('Infinity'), (NULL), (1)) x(v) JOIN (VALUES (-0.0::float8), ('NaN'), (0.0), (NULL),
       ('-Infinity'), ('Infinity')) y(v) ON x.v = y.v$$);
SELECT join_check($$SELECT x.v, y.v FROM (VALUES ('a '::bpchar), ('a'), ('b  '), (NULL)) x(v)
       JOIN (VALUES ('a  '::char(3)), ('b'), ('c')) y(v) ON x.v = y.v$$);
SELECT join_check($$SELECT x.v, y.v FROM (VALUES (1::numeric), (1.0), (1.00), ('NaN'), (2.5))
       x(v) JOIN (VALUES (1.000::numeric), ('NaN'), (2.50), (3)) y(v) ON x.v = y.v$$);
SELECT join_check($$SELECT x.v, y.v FROM (VALUES (32767::int2), (-1), (0)) x(v)
       JOIN (VALUES (32767::int8), (4294967295), (-1), (0)) y(v) ON x.v = y.v$$);

-- an outer row's many matches, past a batch: a long bucket taken in rounds
SELECT join_check('SELECT jo.k, count(*), sum(ji.id) FROM jo JOIN ji ON jo.bo = (ji.id % 3 = 0) GROUP BY jo.k');
SELECT join_check('SELECT jo.id, count(ji.id) FROM jo LEFT JOIN ji ON jo.bo = (ji.id % 3 = 0) AND jo.k < 3 GROUP BY jo.id');
SELECT join_check('SELECT count(*) FROM jo WHERE EXISTS (SELECT 1 FROM ji WHERE (ji.id % 3 = 0) = jo.bo AND ji.id > jo.id)');
SELECT join_check('SELECT count(*) FROM jo WHERE NOT EXISTS (SELECT 1 FROM ji WHERE (ji.id % 3 = 0) = jo.bo AND ji.id > jo.id)');
-- batch edges: 1023, 1024 and 1025 rows a side
SELECT join_check('SELECT count(*), sum(a.id) FROM (SELECT * FROM jo WHERE id <= 1023) a JOIN (SELECT * FROM ji WHERE id <= 1024) b ON a.id = b.id');
SELECT join_check('SELECT count(*), sum(a.id), count(b.id) FROM (SELECT * FROM jo WHERE id <= 1025) a LEFT JOIN (SELECT * FROM ji WHERE id <= 1023) b ON a.id = b.id');

-- join quals beside the hash clauses: they decide a match, the other quals
-- decide what comes out; a semi or anti join stops at its first match
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.k = ji.k AND jo.id < ji.id');
SELECT join_check('SELECT jo.id, ji.id FROM jo LEFT JOIN ji ON jo.k = ji.k AND jo.id + 2000 < ji.id');
SELECT join_check('SELECT jo.id, ji.id FROM jo RIGHT JOIN ji ON jo.k = ji.k AND jo.id + 2000 < ji.id');
SELECT join_check('SELECT jo.id FROM jo WHERE EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k AND ji.id > jo.id)');
SELECT join_check('SELECT jo.id FROM jo WHERE NOT EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k AND ji.id > jo.id)');
SELECT join_check('SELECT jo.id, ji.id, ji.t FROM jo LEFT JOIN ji ON jo.id = ji.id WHERE ji.t IS NULL');
SELECT join_check('SELECT jo.id, ji.s FROM jo LEFT JOIN ji ON jo.id = ji.id WHERE coalesce(ji.s, 0) < 5');
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.k = ji.k AND (jo.t = ji.t OR jo.d = ji.d)');
-- a join qual a kernel cannot decide for some pairs, and one that runs row
-- by row: decided as the pairs are reached
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.k = ji.k AND jo.s * 1000 + ji.b > 40000');
SELECT join_check('SELECT jo.id FROM jo WHERE EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k AND jo.s::int2 * 600::int2 > ji.b)');
SELECT join_check('SELECT jo.id, ji.id FROM jo LEFT JOIN ji ON jo.k = ji.k AND ji.id / (jo.id % 4 + 1) > 300');
SELECT join_check('SELECT jo.id FROM jo WHERE NOT EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k AND ji.id / (jo.id % 4 + 1) > 300)');

-- errors are PostgreSQL's own, from the pair PostgreSQL raises them at, and
-- none from a pair it never reaches: a semi join's pairs after its match
SELECT join_error('SELECT count(*) FROM jo JOIN ji ON jo.k = ji.k AND 100 / (ji.id - 50) > 0');
SELECT join_error('SELECT count(*) FROM jo WHERE EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k AND 100 / (ji.id - 50) > 0)');
SELECT join_error('SELECT count(*) FROM jo WHERE NOT EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k AND 100 / (ji.id - 50) > 0)');
SELECT join_error('SELECT count(*) FROM jo LEFT JOIN ji ON jo.k = ji.k AND 100 / (ji.id - 50) > 0');
SELECT join_error('SELECT count(*) FROM jo JOIN ji ON jo.k = ji.k AND jo.s::int2 * 1000::int2 > 0');
-- a kernel's failure on a pair a semi or anti join never reaches: its
-- buckets give the latest rows first, which match before it
SELECT join_error('SELECT count(*) FROM jo WHERE EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k AND (ji.id > 100 OR jo.s::int2 * 1000::int2 > 0))');
SELECT join_error('SELECT count(*) FROM jo WHERE NOT EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k AND (ji.id > 100 OR jo.s::int2 * 1000::int2 > 0))');
SELECT join_error('SELECT count(*) FROM jo JOIN ji ON jo.k = ji.k AND (ji.id > 100 OR jo.s::int2 * 1000::int2 > 0)');
SELECT join_error('SELECT sum(jo.id / (ji.id - 50)) FROM jo JOIN ji ON jo.id = ji.id');
SELECT join_error('SELECT count(*) FROM jo JOIN (SELECT id, 100 / (id - 50) AS z FROM ji) q ON jo.id = q.id WHERE jo.id < 0');

-- empty sides: an inner side with no row leaves the outer side unread but
-- where its rows are kept; an outer side with none leaves the inner unread
SELECT join_check('SELECT jo.id, je.id FROM jo JOIN je ON jo.id = je.id');
SELECT join_check('SELECT jo.id, je.id FROM jo LEFT JOIN je ON jo.id = je.id');
SELECT join_check('SELECT jo.id FROM jo WHERE EXISTS (SELECT 1 FROM je WHERE je.k = jo.k)');
SELECT join_check('SELECT jo.id FROM jo WHERE NOT EXISTS (SELECT 1 FROM je WHERE je.k = jo.k)');
SELECT join_check('SELECT jo.id, je.id FROM jo RIGHT JOIN je ON jo.id = je.id');
SELECT join_check('SELECT je.id, ji.id FROM je JOIN ji ON je.id = ji.id');
SELECT join_check('SELECT je.id, ji.id FROM je RIGHT JOIN ji ON je.id = ji.id');
SELECT join_check('SELECT a.id, ji.id FROM (SELECT * FROM jo WHERE id < 0) a JOIN ji ON a.id = ji.id');

-- inner-unique joins stop at an outer row's first match too
CREATE UNIQUE INDEX ji_id ON ji (id);
ANALYZE ji;
SELECT join_check('SELECT jo.id, ji.b FROM jo JOIN ji ON jo.id = ji.id');
SELECT join_check('SELECT jo.id, ji.b FROM jo LEFT JOIN ji ON jo.id = ji.id AND ji.b > 10');
DROP INDEX ji_id;

-- three tables, the joins one over the other, and an aggregation over them
EXPLAIN (COSTS OFF) SELECT ji.k, count(*), sum(jo.b) FROM jo JOIN ji ON jo.id = ji.id GROUP BY ji.k;
SELECT join_check('SELECT ji.k, count(*), sum(jo.b), max(jo.t) FROM jo JOIN ji ON jo.id = ji.id GROUP BY ji.k');
SELECT join_check($$SELECT a.id, b.id, c.id FROM jo a JOIN ji b ON a.k = b.k
       JOIN ji c ON b.id = c.b AND c.k = a.s$$);
SELECT join_check($$SELECT a.id, b.id, c.id FROM jo a LEFT JOIN ji b ON a.id = b.id
       LEFT JOIN jo c ON b.nk = c.nk WHERE a.id < 500$$);
SELECT join_check($$SELECT count(*), sum(a.id) FROM jo a JOIN ji b ON a.k = b.k
       WHERE EXISTS (SELECT 1 FROM ji c WHERE c.s = b.s AND c.id < 100)$$);

-- outer joins over outer joins: a nullable side's column, read below the
-- join that nulls it and above it, and placeholders -- a subquery's
-- expressions over a nullable side, which a join evaluates and the next
-- one up reads
SELECT join_check($$SELECT a.id, ss.id, ss.c, ss.m FROM jo a LEFT JOIN
       (SELECT b.id, coalesce(g.cnt, 0) AS c, -1 AS m FROM ji b LEFT JOIN
          (SELECT k, count(*) AS cnt FROM jo WHERE id < 3000 GROUP BY k) g ON b.s = g.k) ss
       ON a.id = ss.id WHERE a.id < 400$$);
SELECT join_check($$SELECT * FROM (SELECT 1 AS x) s1 LEFT JOIN
       (SELECT jo.id, coalesce(ji.b, jo.k) AS y FROM jo LEFT JOIN ji ON jo.id = ji.id
        WHERE jo.id < 200) s2 ON true$$);

-- eager aggregation: the join under a partial aggregate is a VecHashJoin,
-- and the grouped join over the aggregate PostgreSQL's own, though the
-- relation it groups has a VecHashJoin
CREATE TABLE jg1 AS SELECT g AS a, g AS b, g::float8 AS c FROM generate_series(1, 1000) g;
CREATE TABLE jg2 AS SELECT g AS a, g % 10 AS b, g::float8 AS c FROM generate_series(1, 1000) g;
CREATE TABLE jg3 AS SELECT g % 10 AS a, g % 10 AS b, g::float8 AS c FROM generate_series(1, 1000) g;
ANALYZE jg1, jg2, jg3;
SET enable_hashagg = off;
EXPLAIN (COSTS OFF) SELECT jg1.a, avg(jg2.c + jg3.c) FROM jg1 JOIN jg2 ON jg1.b = jg2.b
  JOIN jg3 ON jg2.a = jg3.a GROUP BY jg1.a;
SELECT join_check($$SELECT jg1.a, avg(jg2.c + jg3.c) FROM jg1 JOIN jg2 ON jg1.b = jg2.b
       JOIN jg3 ON jg2.a = jg3.a GROUP BY jg1.a$$);
RESET enable_hashagg;
DROP TABLE jg1, jg2, jg3;

-- keys PostgreSQL's evaluator computes, as the rows are reached: behind an
-- InitPlan, which runs where a row reaches it, and with no vector form;
-- and an outer side read a row at a time, its rows not to be read ahead
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.k + (SELECT 1) = ji.k');
SELECT join_check('SELECT jo.id, ji.id FROM jo LEFT JOIN ji ON jo.k = ji.k + (SELECT 1) AND jo.id < 300');
SELECT join_check('SELECT jo.id FROM jo WHERE EXISTS (SELECT 1 FROM ji WHERE ji.k = jo.k + (SELECT 2) AND ji.id > jo.id)');
SELECT join_check('SELECT g, ji.id FROM generate_series(1, 3000) g JOIN ji ON g = ji.id');
SELECT join_check('SELECT g, ji.id FROM generate_series(-5, 3000) g LEFT JOIN ji ON g = ji.id AND ji.k > 3');
SELECT join_explain('SELECT g, ji.id FROM generate_series(1, 3000) g JOIN ji ON g = ji.id', 'Input|One at a Time');
SELECT join_error('SELECT g FROM (SELECT g, 100 / (3000 - g) AS z FROM generate_series(1, 3000) g) q JOIN ji ON q.g = ji.id LIMIT 3');

-- volatile functions, row by row in PostgreSQL's order: once a pair the
-- hash clauses pass, and once a row that comes out
CREATE SEQUENCE jseq;
SELECT count(*), max(currval('jseq')) FROM jo JOIN ji ON jo.k = ji.k AND nextval('jseq') > 0
WHERE jo.id < 600;
SELECT count(*), max(n) - min(n) + 1 FROM (SELECT nextval('jseq') AS n FROM jo JOIN ji ON jo.id = ji.id) q;
SET vexec.mode = off;
ALTER SEQUENCE jseq RESTART;
SELECT count(*), max(currval('jseq')) FROM jo JOIN ji ON jo.k = ji.k AND nextval('jseq') > 0
WHERE jo.id < 600;
SELECT count(*), max(n) - min(n) + 1 FROM (SELECT nextval('jseq') AS n FROM jo JOIN ji ON jo.id = ji.id) q;
SET vexec.mode = force;

-- rescanned: in a correlated subquery, a side's parameter changing each
-- time -- the table is made again where it is the inner side's
SELECT join_check($$SELECT o.k, (SELECT count(*) FROM jo a JOIN ji b ON a.id = b.id
       WHERE b.k = o.k), (SELECT count(*) FROM jo a JOIN ji b ON a.id = b.id WHERE a.k = o.k)
       FROM (SELECT DISTINCT k FROM jo WHERE k < 20) o$$);
SELECT join_check($$SELECT o.k, (SELECT max(a.t) FROM jo a LEFT JOIN ji b ON a.id = b.id
       WHERE a.s = o.k) FROM (SELECT DISTINCT k FROM jo WHERE k < 20) o$$);
-- a parameter in a key: the inner side's keys change with it, and the table
-- is made again, as a Hash node's chgParam has it
SELECT join_check($$SELECT o.v, ss.* FROM (SELECT DISTINCT k AS v FROM jo WHERE k < 6) o,
       LATERAL (SELECT a.id, b.id AS bid FROM jo a JOIN ji b ON a.id = b.k + o.v
                WHERE a.id < 200) ss$$);
SELECT join_check($$SELECT o.v, ss.* FROM (SELECT DISTINCT k AS v FROM jo WHERE k < 6) o,
       LATERAL (SELECT a.id, b.id AS bid FROM jo a JOIN ji b ON a.id + o.v = b.k
                WHERE b.id < 200) ss$$);
SELECT join_check($$SELECT o.v, ss.* FROM (SELECT DISTINCT k AS v FROM jo WHERE k < 6) o,
       LATERAL (SELECT a.id, b.id AS bid FROM jo a LEFT JOIN ji b ON a.id = b.k + o.v AND b.id > o.v
                WHERE a.id < 150) ss$$);

-- spill: past hash_mem the grace join, by partitions, and its parts split
-- again where they do not fit
SET work_mem = '64kB';
SET hash_mem_multiplier = 1;
SELECT join_check('SELECT jo.id, ji.id, ji.pad FROM jo JOIN ji ON jo.b = ji.b');
SELECT join_check('SELECT jo.id, ji.id, ji.pad FROM jo LEFT JOIN ji ON jo.nk = ji.nk');
SELECT join_check('SELECT jo.id, ji.id, ji.pad FROM jo RIGHT JOIN ji ON jo.nk = ji.nk');
SELECT join_check('SELECT jo.id FROM jo WHERE EXISTS (SELECT 1 FROM ji WHERE ji.b = jo.b AND ji.pad > ''x'')');
SELECT join_check('SELECT jo.id FROM jo WHERE NOT EXISTS (SELECT 1 FROM ji WHERE ji.nk = jo.nk AND ji.id > jo.id)');
SELECT join_explain('SELECT jo.id, ji.pad FROM jo JOIN ji ON jo.b = ji.b', 'Spill|Vec Hash|Inner Rows');
-- two keys only: split to the last level of bits, which loads what is left
SELECT join_check('SELECT jo.k, count(*), sum(ji.id) FROM jo JOIN ji ON jo.bo = (ji.id % 3 = 0) GROUP BY jo.k');
SELECT join_explain('SELECT count(*) FROM jo JOIN ji ON jo.bo = (ji.id % 3 = 0)', 'Spill Depth');
RESET work_mem;
RESET hash_mem_multiplier;

-- the batch formats
SET vexec.batch_format = arrow;
SELECT join_check('SELECT jo.id, jo.t, jo.d, jo.n, ji.id, ji.t, ji.d, ji.n FROM jo JOIN ji ON jo.k = ji.k AND jo.t = ji.t');
SELECT join_check('SELECT jo.id, ji.t, ji.d FROM jo LEFT JOIN ji ON jo.d = ji.d AND jo.nk = ji.nk');
SELECT join_check('SELECT jo.id FROM jo WHERE EXISTS (SELECT 1 FROM ji WHERE ji.t = jo.t AND ji.n > jo.n)');
RESET vexec.batch_format;
SET vexec.batch_numeric_layout = varlena;
SELECT join_check('SELECT jo.id, jo.n, ji.n FROM jo JOIN ji ON jo.n = ji.n AND jo.k = ji.k');
RESET vexec.batch_numeric_layout;

-- EXPLAIN ANALYZE: the inner rows, the join filter's
SELECT join_explain('SELECT jo.id FROM jo JOIN ji ON jo.k = ji.k AND jo.id < ji.id', 'Vec Hash|Inner Rows|Join Filter|Input');

-- refused: a full join, a hash join disabled; explain mode costs and records
EXPLAIN (VEXEC, COSTS OFF) SELECT jo.id, ji.id FROM jo FULL JOIN ji ON jo.id = ji.id;
SET enable_hashjoin = off;
SELECT join_check('SELECT jo.id, ji.id FROM jo JOIN ji ON jo.id = ji.id');
RESET enable_hashjoin;
SET vexec.mode = explain;
EXPLAIN (COSTS OFF) SELECT jo.id, ji.id FROM jo JOIN ji ON jo.id = ji.id;

DROP FUNCTION join_check(text);
DROP FUNCTION join_error(text);
DROP FUNCTION join_explain(text, text);
DROP TABLE jo, ji, je;
DROP SEQUENCE jseq;
