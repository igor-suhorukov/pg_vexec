-- SPDX-License-Identifier: Apache-2.0
--
-- What the pack changes of vexec's plans (pg_vector_executor.md §3.17, VK's
-- "done when"), with vexec_pgvector preloaded: a qual or a target over the
-- distances compiles eager, where it was row by row; EXPLAIN VERBOSE names
-- each declared call, its pack and its declaration; a nearest-neighbour
-- search reads its scan's batches into VecSort; and EXPLAIN ANALYZE counts
-- the rows the check passed and those it sent to PostgreSQL's evaluator.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
CREATE EXTENSION IF NOT EXISTS vector;
RESET client_min_messages;

SELECT pack, extension, versions, declarations FROM vexec.kernel_packs();
SELECT function, declaration FROM vexec.declared_calls() ORDER BY function COLLATE "C";

-- how force mode's vector nodes run a query: their kinds, their quals and
-- targets eager or row by row, their declared calls and inputs; with
-- ANALYZE, the rows declared calls answered and those sent to PostgreSQL
CREATE FUNCTION vexec_how(q text, with_analyze bool DEFAULT false) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	PERFORM set_config('vexec.mode', 'force', true);
	FOR l IN EXECUTE format('EXPLAIN (VERBOSE, COSTS OFF%s) %s',
							CASE WHEN with_analyze THEN ', ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF' ELSE '' END, q) LOOP
		IF l ~ '^\s*(->\s+)?(Vec |Limit|Sort|Seq Scan)' OR
		   l ~ '(Vector Quals|Row-by-Row|Declared Calls|Rows Sent to PostgreSQL|Rows Through Declared Calls|Input:)' THEN
			RETURN NEXT regexp_replace(l, '^\s+(->\s+)?', '');
		END IF;
	END LOOP;
END
$$;

CREATE TABLE items (id int, category int, embedding vector(3), half halfvec(3));
INSERT INTO items
SELECT g, g % 10, ARRAY[sin(g), cos(g), sin(g * 0.5)]::real[]::vector, ARRAY[sin(g), cos(g), 0]::real[]::halfvec
FROM generate_series(1, 3000) g;
CREATE TABLE mixed (id int, e vector);
INSERT INTO mixed
SELECT g, CASE WHEN g % 1000 = 0 THEN ARRAY[g, 1]::real[] ELSE ARRAY[g % 7, g % 11, g % 13]::real[] END::vector
FROM generate_series(1, 3000) g;
ANALYZE items, mixed;

-- a nearest-neighbour search with a filter: VecSort over the scan's batches,
-- the distance a declared call in the scan's target
SELECT * FROM vexec_how('SELECT id FROM items WHERE category = 3 ORDER BY embedding <-> ''[1,0,0]'' LIMIT 10');
-- each operator, and halfvec's
SELECT * FROM vexec_how('SELECT embedding <=> ''[1,0,0]'', embedding <#> ''[1,0,0]'', embedding <+> ''[1,0,0]'', half <-> ''[1,0,0]'' FROM items');
-- a qual
SELECT * FROM vexec_how('SELECT count(*) FROM items WHERE l2_distance(embedding, ''[1,0,0]'') < 0.5');

-- rows of other dimensions go to PostgreSQL's evaluator, which never calls
-- the distance on them: the qual before rejects them -- the distance's cost
-- puts it after the lazy qual, as PostgreSQL orders its quals
ALTER FUNCTION l2_distance(vector, vector) COST 1000;
SELECT * FROM vexec_how('SELECT id FROM mixed WHERE abs(vector_dims(e) - 3) = 0 AND e <-> ''[1,2,3]'' < 3', true);
ALTER FUNCTION l2_distance(vector, vector) COST 1;

-- without the pack's declaration -- another function, of the same
-- arguments, that pgvector does not have -- the qual is row by row
CREATE FUNCTION my_distance(vector, vector) RETURNS float8
AS '$libdir/vector', 'l2_distance' LANGUAGE C IMMUTABLE STRICT;
SELECT * FROM vexec_how('SELECT count(*) FROM items WHERE my_distance(embedding, ''[1,0,0]'') < 0.5');
DROP FUNCTION my_distance(vector, vector);

DROP TABLE items, mixed;
DROP FUNCTION vexec_how(text, bool);
