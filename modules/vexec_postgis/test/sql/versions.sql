-- SPDX-License-Identifier: Apache-2.0
--
-- Binding by PostGIS's version (pg_vector_executor.md §3.17, VK's "done
-- when"): ALTER EXTENSION postgis UPDATE to a version the pack does not name
-- unbinds the declarations in every session that had them, and the queries
-- give the same answers, row by row; back at the version it names, they
-- bind again.  test/run.sh installs, beside PostGIS's scripts, two empty
-- update scripts: to the installed version with "-vk" after it, and back.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION IF NOT EXISTS dblink;
RESET client_min_messages;

SELECT extversion AS postgis_version FROM pg_extension WHERE extname = 'postgis' \gset
SELECT format('host=%s port=%s dbname=%s', current_setting('unix_socket_directories'),
			  current_setting('port'), current_database()) AS other_session \gset

CREATE TABLE places (id int, g geometry);
INSERT INTO places
SELECT i, ST_MakeEnvelope(i % 100, i / 30, i % 100 + 0.5, i / 30 + 0.5, 4326) FROM generate_series(1, 3000) i;
ANALYZE places;

-- the declared calls of force mode's scan, or none
CREATE FUNCTION declared(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	PERFORM set_config('vexec.mode', 'force', true);
	FOR l IN EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF) ' || q LOOP
		IF l ~ 'Declared Calls' THEN
			RETURN regexp_replace(l, '^\s+', '');
		END IF;
	END LOOP;
	RETURN 'no declared call';
END
$$;

-- an answer, in force mode and off
CREATE FUNCTION answer(q text, m text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r text;
BEGIN
	PERFORM set_config('vexec.mode', m, true);
	EXECUTE format('SELECT md5(string_agg(x::text, %L ORDER BY x::text)) FROM (%s) x', ',', q) INTO r;
	RETURN r;
END
$$;

\set query 'SELECT id, ST_SRID(g) FROM places WHERE g && ST_MakeEnvelope(10, 10, 40, 40, 4326) AND ST_Intersects(g, ST_MakeEnvelope(20, 20, 30, 30, 4326))'

-- at the installed version, which the pack names
SELECT declared(:'query');
SELECT count(*) AS bound FROM vexec.declared_calls() WHERE extension = 'postgis';

-- another session updates PostGIS to a version the pack does not name
SELECT dblink_exec(:'other_session', format('ALTER EXTENSION postgis UPDATE TO %L', :'postgis_version' || '-vk'));
SELECT extversion = :'postgis_version' || '-vk' AS updated FROM pg_extension WHERE extname = 'postgis';
SELECT declared(:'query');
SELECT count(*) AS bound FROM vexec.declared_calls() WHERE extension = 'postgis';
SELECT answer(:'query', 'force') = answer(:'query', 'off') AS same_answer;

-- and back
ALTER EXTENSION postgis UPDATE TO :'postgis_version';
SELECT declared(:'query');
SELECT count(*) AS bound FROM vexec.declared_calls() WHERE extension = 'postgis';
SELECT answer(:'query', 'force') = answer(:'query', 'off') AS same_answer;

DROP TABLE places;
DROP FUNCTION declared(text);
DROP FUNCTION answer(text, text);
