-- SPDX-License-Identifier: Apache-2.0
--
-- What the pack changes of vexec's plans (pg_vector_executor.md §3.17, VK's
-- "done when"), with vexec_postgis preloaded: a qual or a target over the
-- declared functions compiles eager, where it was row by row; EXPLAIN
-- VERBOSE names each declared call, its pack and its declaration; and
-- EXPLAIN ANALYZE counts the rows a prefilter decided and those it sent to
-- PostGIS's own function, through PostgreSQL's evaluator.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
CREATE EXTENSION IF NOT EXISTS postgis;
RESET client_min_messages;

SELECT pack, extension, versions, declarations FROM vexec.kernel_packs();
SELECT function, declaration FROM vexec.declared_calls() ORDER BY function COLLATE "C";

-- how force mode's vector nodes run a query
CREATE FUNCTION vexec_how(q text, with_analyze bool DEFAULT false) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	PERFORM set_config('vexec.mode', 'force', true);
	FOR l IN EXECUTE format('EXPLAIN (VERBOSE, COSTS OFF%s) %s',
							CASE WHEN with_analyze THEN ', ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF' ELSE '' END, q) LOOP
		IF l ~ '^\s*(->\s+)?(Vec |Limit|Sort|Seq Scan)' OR
		   l ~ '(Vector Quals|Row-by-Row|Fallback Steps|Declared Calls|Rows Sent to PostgreSQL|Rows Through Declared Calls|Input:)' THEN
			RETURN NEXT regexp_replace(l, '^\s+(->\s+)?', '');
		END IF;
	END LOOP;
END
$$;

CREATE TABLE places (id int, g geometry);
INSERT INTO places
SELECT i, CASE WHEN i % 2 = 0 THEN ST_SetSRID(ST_MakePoint(i % 100, i / 30), 4326)
			   ELSE ST_MakeEnvelope(i % 100, i / 30, i % 100 + 0.5, i / 30 + 0.5, 4326) END
FROM generate_series(1, 3000) i;
ANALYZE places;

-- a box filter: a declared call that never raises
SELECT * FROM vexec_how('SELECT id FROM places WHERE g && ST_MakeEnvelope(10, 10, 20, 20, 4326)', true);
-- the other box operators and ST_SRID
SELECT * FROM vexec_how('SELECT g ~= g, g @ g, g ~ g, g << g, g &< g, g <<| g, g &<| g, g &> g, g >> g, g |&> g, g |>> g, ST_SRID(g) FROM places');
-- ST_X under its check
SELECT * FROM vexec_how('SELECT ST_X(g), ST_Y(g) FROM places WHERE id % 2 = 0', true);
-- a predicate's prefilter: its answers where the boxes decide, PostGIS's
-- function on the other rows
SELECT * FROM vexec_how('SELECT id FROM places WHERE ST_Intersects(g, ST_MakeEnvelope(10, 10, 20, 20, 4326))', true);
SELECT * FROM vexec_how('SELECT ST_Contains(ST_MakeEnvelope(10, 10, 60, 60, 4326), g), ST_Within(g, ST_MakeEnvelope(10, 10, 60, 60, 4326)), ST_Equals(g, g), ST_Disjoint(g, g) FROM places');
-- a function the pack does not declare stays row by row
SELECT * FROM vexec_how('SELECT id FROM places WHERE ST_DWithin(g, ''SRID=4326;POINT(50 50)''::geometry, 5)');

DROP TABLE places;
DROP FUNCTION vexec_how(text, bool);
