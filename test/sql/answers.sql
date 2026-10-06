-- SPDX-License-Identifier: Apache-2.0
--
-- Same answers with or without the pack (pg_vector_executor.md §3.17, VK's
-- "done when"): each query over PostGIS's box operators, accessors and
-- predicates gives PostgreSQL's answer -- its rows, or its error -- in
-- vexec's force mode, in the PostgreSQL and the Arrow format, as
-- vexec_check() compares.  test/run.sh runs this file on a server with
-- vexec_postgis preloaded, where they are declared calls, and on one
-- without it, where they are calls row by row: its expected output is the
-- same, PostgreSQL's.
--
-- The cases: a query filtered by a box; each box operator; ST_SRID, ST_X
-- and ST_Y; each predicate, in quals and targets, decided by the boxes and
-- not; geometries of other SRIDs, and non-points under ST_X, which raise
-- PostGIS's error in row order, and nowhere a qual before has removed them
-- or a LIMIT stopped short of them; empty geometries; curves; values stored
-- out of line; batch edges.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
CREATE EXTENSION IF NOT EXISTS postgis;
RESET client_min_messages;

-- a query's answer in one configuration: its rows as text, sorted -- a
-- long one as its count and md5 -- or its error
CREATE FUNCTION vexec_run(q text, m text, f text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r text;
	n int8;
BEGIN
	PERFORM set_config('vexec.mode', m, true);
	PERFORM set_config('vexec.batch_format', f, true);
	BEGIN
		EXECUTE format('SELECT coalesce(string_agg(x::text, %L ORDER BY x::text COLLATE "C"), %L), count(*) FROM (%s) x',
					   ' | ', '(none)', q) INTO r, n;
		IF length(r) > 200 THEN
			r := format('%s rows, md5 %s', n, md5(r));
		END IF;
	EXCEPTION WHEN OTHERS THEN
		r := 'ERROR ' || SQLSTATE || ': ' || SQLERRM;
	END;
	RETURN r;
END
$$;

-- PostgreSQL's answer, where force mode in both formats gives it
CREATE FUNCTION vexec_check(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ref text := vexec_run(q, 'off', 'postgres');
	got text;
	f text;
BEGIN
	FOREACH f IN ARRAY ARRAY['postgres', 'arrow'] LOOP
		got := vexec_run(q, 'force', f);
		IF got IS DISTINCT FROM ref THEN
			RETURN format('DIFFERS in the %s format: off gives [%s], force gives [%s]', f, ref, got);
		END IF;
	END LOOP;
	RETURN ref;
END
$$;

-- 5000 places: points, small boxes and lines, a category each, SRID 4326;
-- every 1000th row a point of SRID 3857, every 499th an empty point
SELECT setseed(0.77);
CREATE TABLE places (id int, category int, g geometry);
INSERT INTO places
SELECT i, i % 10,
	   CASE WHEN i % 1000 = 0 THEN ST_SetSRID(ST_MakePoint(x, y), 3857)
			WHEN i % 499 = 0 THEN 'SRID=4326;POINT EMPTY'::geometry
			WHEN i % 3 = 0 THEN ST_SetSRID(ST_MakePoint(x, y), 4326)
			WHEN i % 3 = 1 THEN ST_MakeEnvelope(x, y, x + 0.5, y + 0.5, 4326)
			ELSE ST_SetSRID(ST_MakeLine(ST_MakePoint(x, y), ST_MakePoint(x + 1, y - 1)), 4326) END
FROM (SELECT i, random() * 100 AS x, random() * 100 AS y FROM generate_series(1, 5000) i) s;
UPDATE places SET g = NULL WHERE id % 777 = 0;
ANALYZE places;

-- 1025 polygons of many points, stored out of line
CREATE TABLE shapes (id int, g geometry);
ALTER TABLE shapes ALTER COLUMN g SET STORAGE EXTERNAL;
INSERT INTO shapes
SELECT i, ST_SetSRID(ST_Buffer(ST_MakePoint((i * 37) % 100, (i * 53) % 100), 1 + i % 5, 60), 4326)
FROM generate_series(1, 1025) i;
ANALYZE shapes;
SELECT count(*) FILTER (WHERE pg_column_toast_chunk_id(g) IS NOT NULL) AS out_of_line FROM shapes;

-- a query filtered by a box, and each box operator
SELECT vexec_check('SELECT id FROM places WHERE g && ST_MakeEnvelope(10, 10, 20, 20, 4326) AND id % 1000 <> 0');
SELECT vexec_check('SELECT category, count(*) FROM places WHERE g && ST_MakeEnvelope(0, 0, 50, 50, 4326) GROUP BY category');
SELECT vexec_check('SELECT count(*) FROM places WHERE g && ST_MakeEnvelope(0, 0, 50, 50, 3857)');
SELECT vexec_check('SELECT id, g ~= g, g @ ST_MakeEnvelope(0, 0, 50, 50, 4326), g ~ ''SRID=4326;POINT(25 25)''::geometry FROM places WHERE id % 50 = 0');
SELECT vexec_check('SELECT id, g << ''SRID=4326;POINT(50 50)''::geometry, g &< ''SRID=4326;POINT(50 50)''::geometry, g <<| ''SRID=4326;POINT(50 50)''::geometry, g &<| ''SRID=4326;POINT(50 50)''::geometry FROM places WHERE id % 41 = 0');
SELECT vexec_check('SELECT id, g &> ''SRID=4326;POINT(50 50)''::geometry, g >> ''SRID=4326;POINT(50 50)''::geometry, g |&> ''SRID=4326;POINT(50 50)''::geometry, g |>> ''SRID=4326;POINT(50 50)''::geometry FROM places WHERE id % 43 = 0');
-- ST_SRID
SELECT vexec_check('SELECT ST_SRID(g), count(*) FROM places GROUP BY 1');

-- ST_X and ST_Y: on points, NULL on an empty point ...
SELECT vexec_check('SELECT id, ST_X(g), ST_Y(g) FROM places WHERE id % 3 = 0 AND id % 1000 <> 0');
SELECT vexec_check('SELECT sum(ST_X(g)), count(ST_Y(g)) FROM places WHERE GeometryType(g) = ''POINT''');
-- ... and on anything else PostGIS's error, at the first row reached
SELECT vexec_check('SELECT id, ST_X(g) FROM places');
SELECT vexec_check('SELECT count(*) FROM places WHERE ST_Y(g) > 50');
-- and none where PostgreSQL reaches no such row: a qual before it -- not
-- an equality to a constant, which the planner makes again from its
-- equivalence class, after the other quals -- or a LIMIT
SELECT vexec_check('SELECT id, ST_X(g) FROM places WHERE id % 3 = 0 LIMIT 300');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 3 < 1 AND ST_X(g) > 50');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 3 = 0 AND ST_X(g) > 50');

-- each predicate, decided by the boxes and not
SELECT vexec_check('SELECT id FROM places WHERE ST_Intersects(g, ST_MakeEnvelope(10, 10, 30, 30, 4326)) AND id % 1000 <> 0');
SELECT vexec_check('SELECT id FROM places WHERE id % 1000 <> 0 AND ST_Intersects(g, ST_MakeEnvelope(10, 10, 30, 30, 4326))');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 1000 <> 0 AND ST_Touches(g, ST_MakeEnvelope(20, 20, 30, 30, 4326))');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 1000 <> 0 AND ST_Overlaps(g, ST_MakeEnvelope(20, 20, 30, 30, 4326))');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 1000 <> 0 AND ST_Crosses(g, ''SRID=4326;LINESTRING(0 0, 100 100)''::geometry)');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 1000 <> 0 AND ST_Disjoint(g, ST_MakeEnvelope(20, 20, 30, 30, 4326))');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 1000 <> 0 AND ST_Contains(ST_MakeEnvelope(20, 20, 60, 60, 4326), g)');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 1000 <> 0 AND ST_ContainsProperly(ST_MakeEnvelope(20, 20, 60, 60, 4326), g)');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 1000 <> 0 AND ST_Covers(ST_MakeEnvelope(20, 20, 60, 60, 4326), g)');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 1000 <> 0 AND ST_Within(g, ST_MakeEnvelope(20, 20, 60, 60, 4326))');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 1000 <> 0 AND ST_CoveredBy(g, ST_MakeEnvelope(20, 20, 60, 60, 4326))');
SELECT vexec_check('SELECT count(*) FROM places WHERE id % 1000 <> 0 AND ST_Equals(g, ''SRID=4326;POINT(25 25)''::geometry)');
SELECT vexec_check('SELECT id, ST_Intersects(g, ST_MakeEnvelope(40, 40, 45, 45, 4326)), ST_Disjoint(g, ST_MakeEnvelope(40, 40, 45, 45, 4326)), ST_Equals(g, g) FROM places WHERE id % 29 = 0 AND id % 1000 <> 0');
-- geometries of another SRID: PostGIS's error, at the first row reached
SELECT vexec_check('SELECT count(*) FROM places WHERE ST_Intersects(g, ST_MakeEnvelope(10, 10, 30, 30, 4326))');
SELECT vexec_check('SELECT id, ST_Within(g, ST_MakeEnvelope(10, 10, 30, 30, 4326)) FROM places ORDER BY id');
-- polygons stored out of line, against each other
SELECT vexec_check('SELECT a.id, b.id FROM shapes a JOIN shapes b ON b.id = a.id + 100 WHERE ST_Intersects(a.g, b.g)');
SELECT vexec_check('SELECT count(*) FROM shapes WHERE ST_Contains(g, ''SRID=4326;POINT(50 50)''::geometry)');
SELECT vexec_check('SELECT count(*) FROM shapes WHERE g && ''SRID=4326;LINESTRING(0 0, 100 100)''::geometry');

-- batch edges: 1023, 1024 and 1025 rows
SELECT vexec_check('SELECT count(*), sum(ST_SRID(g)) FROM shapes WHERE id <= 1023 AND ST_Intersects(g, ST_MakeEnvelope(0, 0, 40, 40, 4326))');
SELECT vexec_check('SELECT count(*), sum(ST_SRID(g)) FROM shapes WHERE id <= 1024 AND ST_Intersects(g, ST_MakeEnvelope(0, 0, 40, 40, 4326))');
SELECT vexec_check('SELECT count(*), sum(ST_SRID(g)) FROM shapes WHERE ST_Intersects(g, ST_MakeEnvelope(0, 0, 40, 40, 4326))');

DROP TABLE places, shapes;
DROP FUNCTION vexec_check(text);
DROP FUNCTION vexec_run(text, text, text);
