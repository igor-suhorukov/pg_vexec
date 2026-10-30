-- GeoParquet's files read as PostGIS's types (pg_vector_executor.md §3.39's
-- PQ15-PQ18): a binary column of a file from elsewhere through its declared
-- type's receive function, as COPY BINARY reads a value; a geometry's SRID
-- of the file's CRS, checked against the column's typmod; and && pruning
-- units by their bounds.  The files are pyarrow's (test/data/make_files.py,
-- written into VPQ_TEST_DATA/geo before the test); the test needs PostGIS.
CREATE EXTENSION IF NOT EXISTS postgis;
\getenv data VPQ_TEST_DATA
SELECT :'data' || '/geo' AS geo \gset
\set f_both :geo '/both.parquet'
\set f_cases :geo '/cases.parquet'
\set f_ewkb_3857 :geo '/ewkb_3857.parquet'
\set f_geography :geo '/geography.parquet'
\set f_none :geo '/none.parquet'
\set f_one_point :geo '/one_point.parquet'
\set f_recv :geo '/recv.parquet'
\set f_recv_long :geo '/recv_long.parquet'
\set f_truncated :geo '/truncated.parquet'
\set f_type_3857 :geo '/type_3857.parquet'
\set f_type_srid :geo '/type_srid.parquet'
\set f_v1 :geo '/v1.parquet'
\set f_v1_crs_null :geo '/v1_crs_null.parquet'
\set f_v1_projjson :geo '/v1_projjson.parquet'
\set f_v1_spherical :geo '/v1_spherical.parquet'
\set f_v2 :geo '/v2.parquet'
-- an error's message, detail and the context of where a value was read, the paths left out
CREATE FUNCTION geo_error(stmt text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	detail text;
	context text;
BEGIN
	EXECUTE stmt;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS detail = PG_EXCEPTION_DETAIL, context = PG_EXCEPTION_CONTEXT;
	RETURN regexp_replace(SQLERRM || CASE WHEN detail <> '' THEN ' / ' || detail ELSE '' END ||
		CASE WHEN context LIKE 'column %' THEN ' / ' || split_part(context, E'\n', 1) ELSE '' END,
		'"(/|s3://)[^"]*"', '"<path>"', 'g');
END $$;
-- the type a table declares each file's column of WKB as: geometry of
-- GeoParquet's geometry_types and the CRS's SRID where they are known --
-- GeoParquet's metadata alone (v1), Parquet's GEOMETRY with it (both,
-- v2) or without it (none) -- and bytea for spherical edges
SELECT f, c.type, c.arrow_type
FROM unnest(ARRAY['v1', 'both', 'v2', 'none', 'type_3857', 'type_srid', 'v1_projjson', 'v1_crs_null',
				  'v1_spherical', 'geography', 'cases']) f,
	LATERAL vexec_parquet.columns(:'geo' || '/' || f || '.parquet') c
WHERE c.name = 'g' ORDER BY f;
-- a geometry without a typmod: its values' SRID the file's CRS's -- none
-- named, OGC:CRS84's 4326; a PROJJSON's or Parquet's EPSG:3857 and srid:3857;
-- GeoParquet's null, unknown
CREATE TABLE g_v1 (id int8, g geometry) USING vexec_parquet WITH (location = :'f_v1');
CREATE TABLE g_both (id int8, g geometry) USING vexec_parquet WITH (location = :'f_both');
CREATE TABLE g_v2 (id int8, g geometry) USING vexec_parquet WITH (location = :'f_v2');
CREATE TABLE g_none (id int8, g geometry) USING vexec_parquet WITH (location = :'f_none');
CREATE TABLE g_3857 (id int8, g geometry) USING vexec_parquet WITH (location = :'f_type_3857');
CREATE TABLE g_srid (id int8, g geometry) USING vexec_parquet WITH (location = :'f_type_srid');
CREATE TABLE g_projjson (id int8, g geometry) USING vexec_parquet WITH (location = :'f_v1_projjson');
CREATE TABLE g_crs_null (id int8, g geometry) USING vexec_parquet WITH (location = :'f_v1_crs_null');
SELECT 'v1' AS f, count(*), count(g), min(ST_SRID(g)), max(ST_SRID(g)), sum(ST_NPoints(g)) FROM g_v1
UNION ALL SELECT 'both', count(*), count(g), min(ST_SRID(g)), max(ST_SRID(g)), sum(ST_NPoints(g)) FROM g_both
UNION ALL SELECT 'v2', count(*), count(g), min(ST_SRID(g)), max(ST_SRID(g)), sum(ST_NPoints(g)) FROM g_v2
UNION ALL SELECT 'none', count(*), count(g), min(ST_SRID(g)), max(ST_SRID(g)), sum(ST_NPoints(g)) FROM g_none
UNION ALL SELECT 'type_3857', count(*), count(g), min(ST_SRID(g)), max(ST_SRID(g)), sum(ST_NPoints(g)) FROM g_3857
UNION ALL SELECT 'type_srid', count(*), count(g), min(ST_SRID(g)), max(ST_SRID(g)), sum(ST_NPoints(g)) FROM g_srid
UNION ALL SELECT 'v1_projjson', count(*), count(g), min(ST_SRID(g)), max(ST_SRID(g)), sum(ST_NPoints(g)) FROM g_projjson
UNION ALL SELECT 'v1_crs_null', count(*), count(g), min(ST_SRID(g)), max(ST_SRID(g)), sum(ST_NPoints(g)) FROM g_crs_null;
SELECT id, ST_AsEWKT(g) FROM g_v1 WHERE id IN (0, 7, 51) ORDER BY id;
-- the same values from every form of the file
SELECT count(*) AS differ FROM (
	(SELECT id, ST_AsEWKB(g) FROM g_v1 EXCEPT ALL SELECT id, ST_AsEWKB(g) FROM g_both)
	UNION ALL (SELECT id, ST_AsEWKB(g) FROM g_both EXCEPT ALL SELECT id, ST_AsEWKB(g) FROM g_v2)
	UNION ALL (SELECT id, ST_AsEWKB(g) FROM g_v2 EXCEPT ALL SELECT id, ST_AsEWKB(g) FROM g_none)) x;
-- a typmod's SRID: the file's known CRS must be of it; an unknown one takes it
CREATE TABLE t_v1 (id int8, g geometry(LineString, 4326)) USING vexec_parquet WITH (location = :'f_v1');
SELECT count(g), min(ST_SRID(g)), max(ST_SRID(g)) FROM t_v1;
CREATE TABLE t_crs_null (id int8, g geometry(LineString, 4326)) USING vexec_parquet
	WITH (location = :'f_v1_crs_null');
SELECT id, ST_AsEWKT(g) FROM t_crs_null ORDER BY id;
SELECT geo_error(format('CREATE TABLE t_bad (id int8, g geometry(LineString, 4326)) USING vexec_parquet
	WITH (location = %L)', :'f_type_3857'));
SELECT geo_error(format('CREATE TABLE t_bad (id int8, g geometry(LineString, 4326)) USING vexec_parquet
	WITH (location = %L)', :'f_v1_projjson'));
CREATE TABLE t_3857 (id int8, g geometry(LineString, 3857)) USING vexec_parquet
	WITH (location = :'f_type_3857');
SELECT count(g), min(ST_SRID(g)) FROM t_3857;
-- spherical edges are not geometry's; geography reads them
SELECT geo_error(format('CREATE TABLE s_bad (id int8, g geometry) USING vexec_parquet WITH (location = %L)',
	:'f_v1_spherical'));
SELECT geo_error(format('CREATE TABLE s_bad (id int8, g geometry) USING vexec_parquet WITH (location = %L)',
	:'f_geography'));
CREATE TABLE s_geog (id int8, g geography) USING vexec_parquet WITH (location = :'f_geography');
SELECT id, ST_AsText(g) FROM s_geog ORDER BY id;
-- WKB of every kind: NULL, an empty line, a point, big-endian WKB, and EWKB
-- whose own SRID stays
CREATE TABLE c_any (id int8, g geometry) USING vexec_parquet WITH (location = :'f_cases');
SELECT id, ST_AsEWKT(g) FROM c_any ORDER BY id;
CREATE TABLE c_bytes (id int8, g bytea) USING vexec_parquet WITH (location = :'f_cases');
SELECT id, length(g) FROM c_bytes ORDER BY id;
-- what PostGIS's receive function refuses is its error, as COPY's, with where it was read
CREATE TABLE c_line (id int8, g geometry(LineString, 4326)) USING vexec_parquet WITH (location = :'f_cases');
SELECT geo_error('SELECT id, ST_AsEWKT(g) FROM c_line ORDER BY id');
CREATE TABLE c_one (id int8, g geometry) USING vexec_parquet WITH (location = :'f_one_point');
SELECT geo_error('SELECT count(g) FROM c_one');
CREATE TABLE c_ewkb (id int8, g geometry(LineString, 4326)) USING vexec_parquet WITH (location = :'f_ewkb_3857');
SELECT geo_error('SELECT count(g) FROM c_ewkb');
CREATE TABLE c_short (id int8, g geometry) USING vexec_parquet WITH (location = :'f_truncated');
SELECT geo_error('SELECT count(g) FROM c_short');
-- other types' binary forms, as COPY BINARY reads them; a value its function leaves bytes of is refused
CREATE TABLE r_recv (id int8, p point, n int8) USING vexec_parquet WITH (location = :'f_recv');
SELECT * FROM r_recv ORDER BY id;
CREATE TABLE r_long (n int8) USING vexec_parquet WITH (location = :'f_recv_long');
SELECT geo_error('SELECT * FROM r_long');
-- through vexec's source, in both formats
SET vexec.mode = force;
SELECT id, ST_AsEWKT(g) FROM c_any ORDER BY id;
SELECT geo_error('SELECT id, ST_AsEWKT(g) FROM c_line ORDER BY id');
SELECT count(g), min(ST_SRID(g)), sum(ST_NPoints(g)) FROM g_both;
SELECT * FROM r_recv ORDER BY id;
SET vexec.batch_format = arrow;
SELECT count(g), min(ST_SRID(g)), sum(ST_NPoints(g)) FROM g_v2;
RESET vexec.batch_format;
-- && prunes units: Parquet's GeospatialStatistics a row group's, GeoParquet's
-- bbox (v1) the file's whole; the answers are the same, fewer rows read
CREATE FUNCTION geo_scan(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l ~ 'Seq Scan|Rows Removed' THEN
			RETURN NEXT regexp_replace(l, '^\s*(->\s*)?', '');
		END IF;
	END LOOP;
END
$$;
SET max_parallel_workers_per_gather = 0;
SELECT geo_scan('SELECT count(*) FROM g_both WHERE g && ST_MakeEnvelope(20.2, 60.2, 20.4, 60.4, 4326)');
SELECT geo_scan('SELECT count(*) FROM g_v1 WHERE g && ST_MakeEnvelope(20.2, 60.2, 20.4, 60.4, 4326)');
SELECT geo_scan('SELECT count(*) FROM g_both WHERE ST_MakeEnvelope(0, 0, 1, 1, 4326) && g');
SELECT geo_scan('SELECT count(*) FROM g_v1 WHERE g && ST_MakeEnvelope(0, 0, 1, 1, 4326)');
SELECT count(*) FROM g_both WHERE g && ST_MakeEnvelope(20.2, 60.2, 20.4, 60.4, 4326);
SELECT count(*) FROM g_v1 WHERE g && ST_MakeEnvelope(20.2, 60.2, 20.4, 60.4, 4326);
SELECT count(*) FROM g_both WHERE g && ST_MakeEnvelope(10.49, 50.49, 20.0, 60.0, 4326);
SELECT count(*) FROM g_both WHERE g && NULL::geometry;
SET vexec_parquet.enable_pruning = off;
SELECT geo_scan('SELECT count(*) FROM g_both WHERE g && ST_MakeEnvelope(20.2, 60.2, 20.4, 60.4, 4326)');
RESET vexec_parquet.enable_pruning;
RESET vexec.mode;
SELECT count(*) FROM g_both WHERE g && ST_MakeEnvelope(20.2, 60.2, 20.4, 60.4, 4326);
SELECT count(*) FROM g_both WHERE g && ST_MakeEnvelope(10.49, 50.49, 20.0, 60.0, 4326);
RESET max_parallel_workers_per_gather;
-- another process: the files' facts in shared memory, their geospatial facts
-- with them -- bounds, GeoParquet's key -- and no footer read for them
\c
SET vexec.mode = force;
SET max_parallel_workers_per_gather = 0;
SELECT count(*) >= 0 AS reset FROM vexec_parquet.read_stats(true);
SELECT geo_scan('SELECT count(*) FROM g_both WHERE g && ST_MakeEnvelope(0, 0, 1, 1, 4326)');
SELECT geo_scan('SELECT count(*) FROM g_v1 WHERE g && ST_MakeEnvelope(0, 0, 1, 1, 4326)');
SELECT geo_scan('SELECT count(*) FROM g_both WHERE g && ST_MakeEnvelope(20.2, 60.2, 20.4, 60.4, 4326)');
SELECT footers_read, facts_opened > 0 AS of_facts FROM vexec_parquet.read_stats();
SELECT count(g), min(ST_SRID(g)), max(ST_SRID(g)) FROM g_projjson;
SELECT count(g), min(ST_SRID(g)), max(ST_SRID(g)) FROM g_v1;
RESET vexec.mode;
RESET max_parallel_workers_per_gather;
DROP FUNCTION geo_scan(text);
DROP FUNCTION geo_error(text);
DROP TABLE g_v1, g_both, g_v2, g_none, g_3857, g_srid, g_projjson, g_crs_null, t_v1, t_crs_null, t_3857, s_geog,
	c_any, c_bytes, c_line, c_one, c_ewkb, c_short, r_recv, r_long;
