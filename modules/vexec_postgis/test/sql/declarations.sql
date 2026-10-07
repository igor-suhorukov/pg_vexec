-- SPDX-License-Identifier: Apache-2.0
--
-- vexec_postgis's declarations against PostGIS itself
-- (pg_vector_executor.md §3.17, VK's "done when"), over every pair of
-- reader.sql's corpus -- random geometries and edges: every type empty,
-- differing SRIDs, NaN, infinite and huge coordinates, curves, Z and M,
-- values stored out of line -- called directly, without vexec:
--
--   - each prefilter's answer, where it gives one, is the predicate's own,
--     and it gives one for no pair on which the predicate raises: the
--     predicate is called on exactly the pairs the prefilter decides, and
--     an error would fail this file;
--   - no box operator declared never raising, nor ST_SRID, raises on any
--     pair or value;
--   - ST_X and ST_Y raise on no value their check passes; and their check
--     is exact: they raise on every other value the reader knows.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS vexec_postgis;
RESET client_min_messages;

-- the prefilters: decided, and equal to the predicate there
CREATE FUNCTION prefilter_outcome(fn text, OUT pairs int8, OUT decided int8, OUT decided_true int8,
								  OUT differ int8)
LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format($q$
		SELECT count(*), count(pf), count(*) FILTER (WHERE pf), count(*) FILTER (WHERE pf IS NOT NULL AND pf IS DISTINCT FROM r)
		FROM (SELECT pf, CASE WHEN pf IS NULL THEN NULL ELSE %I(ga, gb) END AS r
			  FROM (SELECT vexec_postgis.prefilter(%L, a.g, b.g) AS pf, a.g AS ga, b.g AS gb
					FROM geoms a, geoms b OFFSET 0) s
			  OFFSET 0) t$q$, fn, fn)
		INTO pairs, decided, decided_true, differ;
END
$$;
SELECT name, (prefilter_outcome(name)).*
FROM vexec_postgis.declarations() WHERE declaration LIKE 'prefilter%' ORDER BY name;

-- what decides, and what does not: by the kinds of the two values
CREATE TABLE decided AS
SELECT a.kind AS ka, b.kind AS kb, vexec_postgis.prefilter('st_intersects', a.g, b.g) AS pf,
	   ST_SRID(a.g) = ST_SRID(b.g) AS same_srid
FROM geoms a, geoms b;
SELECT count(*) FILTER (WHERE pf IS NOT NULL AND NOT same_srid) AS decided_across_srids,
	   count(*) FILTER (WHERE pf IS NOT NULL AND (ka LIKE '%empty%' OR kb LIKE '%empty%')) AS decided_with_an_empty,
	   count(*) FILTER (WHERE pf IS NOT NULL AND (ka LIKE '%NaN%' OR kb LIKE '%NaN%')) AS decided_with_nan
FROM decided;
-- PostGIS raises its own error on pairs of different SRIDs, which the
-- prefilter left to it (a seventh of them)
DO $$
DECLARE
	r record;
	n int := 0;
	e text;
BEGIN
	FOR r IN SELECT a.g AS ga, b.g AS gb FROM geoms a, geoms b
			 WHERE ST_SRID(a.g) <> ST_SRID(b.g) AND NOT ST_IsEmpty(a.g) AND NOT ST_IsEmpty(b.g)
			   AND a.id % 7 = 0 LOOP
		BEGIN
			PERFORM ST_Intersects(r.ga, r.gb);
		EXCEPTION WHEN OTHERS THEN
			e := SQLERRM;
			n := n + 1;
		END;
	END LOOP;
	RAISE NOTICE 'ST_Intersects raised on % pairs of different SRIDs: %', n, regexp_replace(e, '\d+', 'N', 'g');
END
$$;

-- the box operators and ST_SRID raise nothing
SELECT count(*) AS pairs,
	   count(*) FILTER (WHERE a.g && b.g) AS "&&", count(*) FILTER (WHERE a.g ~= b.g) AS "~=",
	   count(*) FILTER (WHERE a.g ~ b.g) AS "~", count(*) FILTER (WHERE a.g @ b.g) AS "@",
	   count(*) FILTER (WHERE a.g << b.g) AS "<<", count(*) FILTER (WHERE a.g &< b.g) AS "&<",
	   count(*) FILTER (WHERE a.g <<| b.g) AS "<<|", count(*) FILTER (WHERE a.g &<| b.g) AS "&<|",
	   count(*) FILTER (WHERE a.g &> b.g) AS "&>", count(*) FILTER (WHERE a.g >> b.g) AS ">>",
	   count(*) FILTER (WHERE a.g |&> b.g) AS "|&>", count(*) FILTER (WHERE a.g |>> b.g) AS "|>>"
FROM geoms a, geoms b;
SELECT count(DISTINCT ST_SRID(g)) AS srids, count(*) AS values FROM geoms;

-- ST_X and ST_Y on the values their check passes
SELECT count(*) FILTER (WHERE vexec_postgis.is_point(g)) AS points,
	   count(CASE WHEN vexec_postgis.is_point(g) THEN ST_X(g) END) AS x_not_null,
	   count(CASE WHEN vexec_postgis.is_point(g) THEN ST_Y(g) END) AS y_not_null
FROM geoms;
-- and they raise on every other value the reader knows
DO $$
DECLARE
	r record;
	raised int := 0;
	calm int := 0;
BEGIN
	FOR r IN SELECT g FROM geoms WHERE NOT vexec_postgis.is_point(g) AND (vexec_postgis.reader(g)).known LOOP
		BEGIN
			PERFORM ST_X(r.g);
			calm := calm + 1;
		EXCEPTION WHEN OTHERS THEN
			raised := raised + 1;
		END;
	END LOOP;
	RAISE NOTICE 'ST_X on the values the check fails: % raised, % did not', raised, calm;
END
$$;

DROP TABLE decided;
DROP FUNCTION prefilter_outcome(text);
