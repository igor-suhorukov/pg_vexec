-- A file's nested columns read as PostgreSQL's arrays, hstore and jsonb
-- (pg_vector_executor.md §3.40's PQ19 and PQ20): a list of flat elements as
-- an array of the elements' type, a map of strings to strings as hstore, any
-- map as a jsonb object.  The files are pyarrow's (test/data/make_files.py,
-- written into VPQ_TEST_DATA/nested before the test).
CREATE EXTENSION IF NOT EXISTS hstore;
\getenv data VPQ_TEST_DATA
SELECT :'data' || '/nested' AS nested \gset
\set f_lists :nested '/lists.parquet'
\set f_maps :nested '/maps.parquet'
\set f_shapes :nested '/shapes.parquet'
-- the types a table declares them as
SELECT name, type, arrow_type FROM vexec_parquet.columns(:'f_lists');
SELECT name, type, arrow_type FROM vexec_parquet.columns(:'f_maps');
SELECT name, type, arrow_type FROM vexec_parquet.columns(:'f_shapes');
SELECT vexec_parquet.column_list(:'f_lists') AS lists \gset
SELECT vexec_parquet.column_list(:'f_maps') AS maps \gset
CREATE TABLE n_lists (:lists) USING vexec_parquet WITH (location = :'f_lists');
CREATE TABLE n_maps (:maps) USING vexec_parquet WITH (location = :'f_maps');
-- every list: NULL lists and elements, empty lists, of every element type
SELECT id, l64, l32, l16, l8, ls, lf, ld FROM n_lists ORDER BY id;
SELECT id, lb, ldate, lts, ldec, lbin, large FROM n_lists ORDER BY id;
SELECT id, pg_typeof(l64), array_lower(l64, 1), array_upper(l64, 1), cardinality(l64), l64[1] FROM n_lists ORDER BY id;
-- other declarations of the same lists: the elements' types read as a column of them is, typmods applied
CREATE TABLE n_other (id int8, l8 "char"[], l32 int8[], l64 numeric[], ls varchar(40)[], lf float8[])
	USING vexec_parquet WITH (location = :'f_lists');
SELECT * FROM n_other ORDER BY id;
CREATE TABLE n_short (id int8, ls varchar(3)[]) USING vexec_parquet WITH (location = :'f_lists');
SELECT * FROM n_short ORDER BY id;
-- every map: hstore of strings, jsonb of any keys and values -- NULL and empty maps,
-- NULL values, a key twice (hstore keeps one, jsonb its last), a float's NaN and infinity as text
SELECT id, tags, counts, scores, flags, dates FROM n_maps ORDER BY id;
SELECT id, tags -> 'highway' AS highway, tags ? 'building' AS building, tags ?| ARRAY['a', 'name'] AS any_of,
	akeys(tags), hstore_to_jsonb(tags) FROM n_maps ORDER BY id;
SELECT id, jsonb_typeof(counts -> 'a') AS a, counts ->> 'big' AS big, jsonb_typeof(scores -> '2') AS nan,
	scores ->> '3' AS inf, flags -> 'on' AS on_flag FROM n_maps ORDER BY id;
CREATE TABLE n_json (id int8, tags jsonb) USING vexec_parquet WITH (location = :'f_maps');
SELECT * FROM n_json ORDER BY id;
-- what no declaration reads; a string element of another type through that type's input, as a string column's is
SELECT vpq_error(format('CREATE TABLE n_bad1 (id int8, l64 text[]) USING vexec_parquet WITH (location = %L)', :'f_lists'));
SELECT vpq_error(format('CREATE TABLE n_bad2 (id int8, lb int8[]) USING vexec_parquet WITH (location = %L)', :'f_lists'));
SELECT vpq_error(format('CREATE TABLE n_bad3 (id int8, l64 jsonb) USING vexec_parquet WITH (location = %L)', :'f_lists'));
SELECT vpq_error(format('CREATE TABLE n_bad4 (id int8, counts hstore) USING vexec_parquet WITH (location = %L)', :'f_maps'));
SELECT vpq_error(format('CREATE TABLE n_bad5 (id int8, tags text[]) USING vexec_parquet WITH (location = %L)', :'f_maps'));
SELECT vpq_error(format('CREATE TABLE n_bad6 (id int8, ll int8[]) USING vexec_parquet WITH (location = %L)', :'f_shapes'));
SELECT vpq_error(format('CREATE TABLE n_bad7 (id int8, lst text) USING vexec_parquet WITH (location = %L)', :'f_shapes'));
SELECT vpq_error(format('CREATE TABLE n_bad8 (id int8, ml jsonb) USING vexec_parquet WITH (location = %L)', :'f_shapes'));
CREATE TABLE n_input (id int8, ls int8[]) USING vexec_parquet WITH (location = :'f_lists');
SELECT vpq_error('SELECT * FROM n_input');
-- the arrays' and maps' operators, row by row
SELECT id FROM n_lists WHERE 5 = ANY(l64) OR 'inner' = ANY(ls) ORDER BY id;
SELECT id, u, o FROM n_lists, unnest(l64) WITH ORDINALITY AS x(u, o) ORDER BY id, o;
SELECT id FROM n_lists WHERE l16 && ARRAY[3, 32767]::int2[] ORDER BY id;
SELECT id FROM n_maps WHERE tags @> 'highway=>residential' ORDER BY id;
SELECT id, k, v FROM n_maps, each(tags) AS e(k, v) ORDER BY id, k;
-- through vexec's source, in both formats: the same rows as the plain scan's
CREATE TEMP TABLE off_rows AS SELECT l::text AS r FROM n_lists l UNION ALL SELECT m::text FROM n_maps m;
SET vexec.mode = force;
SELECT count(*) AS differ FROM ((SELECT l::text FROM n_lists l UNION ALL SELECT m::text FROM n_maps m
	EXCEPT ALL TABLE off_rows) UNION ALL (TABLE off_rows EXCEPT ALL
	(SELECT l::text FROM n_lists l UNION ALL SELECT m::text FROM n_maps m))) x;
SELECT id, cardinality(l64), ls[2], tags -> 'highway', counts -> 'a', scores ->> '1' FROM n_lists JOIN n_maps USING (id)
	ORDER BY id;
SET vexec.batch_format = arrow;
SELECT count(*) AS differ FROM ((SELECT l::text FROM n_lists l UNION ALL SELECT m::text FROM n_maps m
	EXCEPT ALL TABLE off_rows) UNION ALL (TABLE off_rows EXCEPT ALL
	(SELECT l::text FROM n_lists l UNION ALL SELECT m::text FROM n_maps m))) x;
RESET vexec.batch_format;
RESET vexec.mode;
-- PQ21: the kernels over the nested layouts -- hstore's ->, ? and ?|, cardinality() and x = ANY
-- of a list -- in the scan and in an aggregation above it: the same answers as the plain scan's,
-- no row sent to PostgreSQL but a map's key given twice (hstore keeps one, its sort's)
CREATE FUNCTION nested_scan(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Kernel Steps|Rows Sent|Rows Evaluated|Kept as Handed' THEN
			RETURN NEXT regexp_replace(l, '^\s*(->\s*)?', '');
		END IF;
	END LOOP;
END
$$;
CREATE TEMP TABLE nested_q (q text);
INSERT INTO nested_q VALUES
	('SELECT id, tags -> ''highway'' FROM n_maps WHERE tags -> ''highway'' = ''residential'''),
	('SELECT id FROM n_maps WHERE tags -> ''k'' IS NOT NULL'),
	('SELECT id FROM n_maps WHERE tags -> ''a'' IN (''1'', ''x'')'),
	('SELECT id FROM n_maps WHERE tags ? ''building'''),
	('SELECT id FROM n_maps WHERE NOT tags ? ''name'''),
	('SELECT id FROM n_maps WHERE tags ?| ARRAY[''c'', ''building'', NULL]'),
	('SELECT id FROM n_maps WHERE tags ?| ARRAY[]::text[]'),
	('SELECT id, cardinality(l64), cardinality(ls), cardinality(large) FROM n_lists'),
	('SELECT id FROM n_lists WHERE cardinality(l64) > 1'),
	('SELECT id FROM n_lists WHERE 7 = ANY(l64)'),
	('SELECT id FROM n_lists WHERE 5::int8 = ANY(l64)'),
	('SELECT id FROM n_lists WHERE 3 = ANY(l16)'),
	('SELECT id FROM n_lists WHERE NULL::int8 = ANY(l64)'),
	('SELECT id, 5 = ANY(l64) AS any5 FROM n_lists'),
	('SELECT id FROM n_lists WHERE ''x'' = ANY(ls)'),
	('SELECT id, ''zz'' = ANY(ls) AS anyzz FROM n_lists'),
	('SELECT sum(cardinality(l64)), count(*) FILTER (WHERE 7 = ANY(l64)) FROM n_lists'),
	('SELECT id % 2 AS g, sum(cardinality(l32)) FROM n_lists GROUP BY 1');
SET vexec.mode = off;
CREATE TEMP TABLE nested_off (q text, x text);
DO $$
DECLARE
	r record;
	res text;
BEGIN
	FOR r IN SELECT q FROM nested_q LOOP
		EXECUTE format('SELECT string_agg(t::text, '' '' ORDER BY t::text) FROM (%s) t', r.q) INTO res;
		INSERT INTO nested_off VALUES (r.q, res);
	END LOOP;
END
$$;
SET vexec.mode = force;
SET max_parallel_workers_per_gather = 0;
SELECT q, (SELECT string_agg(t, E'\n') FROM nested_scan(q) t) AS plan FROM nested_q ORDER BY q;
DO $$
DECLARE
	r record;
	res text;
	bad int := 0;
BEGIN
	FOR r IN SELECT q, x FROM nested_off LOOP
		EXECUTE format('SELECT string_agg(t::text, '' '' ORDER BY t::text) FROM (%s) t', r.q) INTO res;
		IF res IS DISTINCT FROM r.x THEN
			RAISE NOTICE 'differs: % -- % against %', r.q, res, r.x;
			bad := bad + 1;
		END IF;
	END LOOP;
	RAISE NOTICE '% of % queries answer as the plain scan does', (SELECT count(*) FROM nested_q) - bad,
		(SELECT count(*) FROM nested_q);
END
$$;
SELECT id, tags -> 'k' AS k FROM n_maps ORDER BY id;
SET vexec.batch_format = arrow;
SELECT id FROM n_maps WHERE tags ? 'building' OR tags -> 'a' = '1' ORDER BY id;
SELECT id, cardinality(l64) FROM n_lists WHERE 7 = ANY(l64) OR 2147483647 = ANY(l32) ORDER BY id;
RESET vexec.batch_format;
-- x = ANY of a heap's array column: each row's array, as ScalarArrayOpExpr's evaluation reads it
CREATE TABLE n_heap AS SELECT id, l64, ls, l16 FROM n_lists;
SELECT nested_scan('SELECT id FROM n_heap WHERE 7 = ANY(l64)');
SELECT id, 5 = ANY(l64) AS any5, 'x' = ANY(ls) AS anyx, 3 = ANY(l16) AS any3, NULL::int8 = ANY(l64) AS anynull
FROM n_heap ORDER BY id;
RESET vexec.mode;
SELECT id, 5 = ANY(l64) AS any5, 'x' = ANY(ls) AS anyx, 3 = ANY(l16) AS any3, NULL::int8 = ANY(l64) AS anynull
FROM n_heap ORDER BY id;
RESET max_parallel_workers_per_gather;
DROP TABLE n_heap;
DROP FUNCTION nested_scan(text);
-- another process: the children's types from the files' facts in shared memory
\c
SELECT id, l64, tags, scores FROM n_lists JOIN n_maps USING (id) ORDER BY id;
DROP TABLE n_lists, n_maps, n_other, n_short, n_json, n_input;
