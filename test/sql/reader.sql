-- SPDX-License-Identifier: Apache-2.0
--
-- vexec_postgis's reader against PostGIS (pg_vector_executor.md §3.17): the
-- pack reads the serialized form with code of its own, written from
-- gserialized.txt, and decides nothing where it does not know a value.  For
-- every geometry of the corpus it knows, its type, its emptiness and its box
-- are PostGIS's own: ST_GeometryType, ST_IsEmpty, and the box PostGIS's
-- predicates short-circuit on, which postgis_getbbox() gives; and two
-- values' SRID bytes are equal where their SRIDs are.  What it does not know
-- is listed: the types and values it leaves to PostGIS.
--
-- The corpus, kept for the declarations' suite (declarations.sql): random
-- points, lines, boxes, buffers and collections of SRIDs 0, 4326 and 3857;
-- every type empty; points and short lines, which keep no box; curves;
-- Z, M and ZM; triangles, TINs and polyhedral surfaces; NaN, infinite and
-- huge coordinates; and values compressed and stored out of line.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS postgis;
CREATE EXTENSION vexec_postgis;
RESET client_min_messages;

SELECT name, argtypes, symbol, declaration, versions FROM vexec_postgis.declarations()
ORDER BY declaration, name;
-- the functions of this database they name: PostGIS's own, by name,
-- signature, C symbol and library
SELECT d.name, p.oid::regprocedure AS function
FROM vexec_postgis.declarations() d
	 LEFT JOIN pg_proc p ON p.proname = d.name AND p.prosrc = d.symbol
		 AND p.probin = '$libdir/postgis-3' AND p.pronargs = cardinality(d.argtypes)
		 AND p.proargtypes[0] = 'geometry'::regtype
ORDER BY d.name;
SELECT extversion = ANY ((SELECT versions FROM vexec_postgis.declarations() LIMIT 1)::text[]) AS named
FROM pg_extension WHERE extname = 'postgis';

SELECT setseed(0.2026);
CREATE TABLE geoms (id serial, kind text, g geometry);
ALTER TABLE geoms ALTER COLUMN g SET STORAGE EXTERNAL;
-- random ones
INSERT INTO geoms (kind, g)
SELECT 'point', ST_SetSRID(ST_MakePoint(random() * 100, random() * 100), (ARRAY[0, 4326, 3857])[1 + i % 3])
FROM generate_series(1, 18) i;
INSERT INTO geoms (kind, g)
SELECT 'line of 2', ST_SetSRID(ST_MakeLine(ST_MakePoint(random() * 100, random() * 100),
										   ST_MakePoint(random() * 100, random() * 100)), (ARRAY[0, 4326])[1 + i % 2])
FROM generate_series(1, 10) i;
INSERT INTO geoms (kind, g)
SELECT 'line of 5', ST_SetSRID(ST_MakeLine(ARRAY(SELECT ST_MakePoint(random() * 100, random() * 100)
												  FROM generate_series(1, 5) WHERE i > 0)), 4326)
FROM generate_series(1, 6) i;
INSERT INTO geoms (kind, g)
SELECT 'box', ST_MakeEnvelope(x, y, x + random() * 30, y + random() * 30, (ARRAY[0, 4326])[1 + i % 2])
FROM (SELECT i, random() * 100 AS x, random() * 100 AS y FROM generate_series(1, 14) i) s;
INSERT INTO geoms (kind, g)
SELECT 'buffer', ST_SetSRID(ST_Buffer(ST_MakePoint(random() * 100, random() * 100), 1 + random() * 20), 4326)
FROM generate_series(1, 8) i;
INSERT INTO geoms (kind, g)
SELECT 'multipoint of 1', ST_SetSRID(ST_Multi(ST_MakePoint(random() * 100, random() * 100)), 4326)
FROM generate_series(1, 4) i;
INSERT INTO geoms (kind, g)
SELECT 'multipoint of 3', ST_SetSRID(ST_Collect(ARRAY(SELECT ST_MakePoint(random() * 100, random() * 100)
													 FROM generate_series(1, 3) WHERE i > 0)), 4326)
FROM generate_series(1, 4) i;
INSERT INTO geoms (kind, g)
SELECT 'multiline of 1', ST_SetSRID(ST_Multi(ST_MakeLine(ST_MakePoint(random() * 100, random() * 100),
														 ST_MakePoint(random() * 100, random() * 100))), 4326)
FROM generate_series(1, 4) i;
INSERT INTO geoms (kind, g)
SELECT 'multipolygon', ST_SetSRID(ST_Collect(ST_MakeEnvelope(x, y, x + 5, y + 5), ST_MakeEnvelope(x + 10, y, x + 15, y + 5)), 4326)
FROM (SELECT i, random() * 100 AS x, random() * 100 AS y FROM generate_series(1, 4) i) s;
INSERT INTO geoms (kind, g)
SELECT 'collection', ST_SetSRID(ST_Collect(ST_MakePoint(x, y), ST_MakeEnvelope(x, y, x + 8, y + 3)), 4326)
FROM (SELECT i, random() * 100 AS x, random() * 100 AS y FROM generate_series(1, 4) i) s;
-- edges
INSERT INTO geoms (kind, g) VALUES
	('point empty', 'POINT EMPTY'), ('line empty', 'LINESTRING EMPTY'),
	('polygon empty', 'POLYGON EMPTY'), ('multipoint empty', 'MULTIPOINT EMPTY'),
	('multiline empty', 'MULTILINESTRING EMPTY'), ('multipolygon empty', 'MULTIPOLYGON EMPTY'),
	('collection empty', 'GEOMETRYCOLLECTION EMPTY'),
	('collection of an empty point', 'GEOMETRYCOLLECTION(POINT EMPTY)'),
	('collection, empty first', 'GEOMETRYCOLLECTION(POINT EMPTY, POINT(50 50))'),
	('empty, SRID 4326', 'SRID=4326;POINT EMPTY'),
	('line of 1', ST_MakeLine(ARRAY[ST_MakePoint(40, 40)])),
	('circular string', 'SRID=4326;CIRCULARSTRING(10 10, 20 20, 30 10)'),
	('compound curve', 'SRID=4326;COMPOUNDCURVE(CIRCULARSTRING(10 10, 20 20, 30 10), (30 10, 40 10))'),
	('curve polygon', 'SRID=4326;CURVEPOLYGON(CIRCULARSTRING(10 10, 40 10, 40 40, 10 40, 10 10))'),
	('point z', 'SRID=4326;POINT Z (30 30 3)'), ('point m', 'SRID=4326;POINT M (31 31 3)'),
	('point zm', 'SRID=4326;POINT ZM (32 32 3 4)'),
	('line zm', 'SRID=4326;LINESTRING ZM (20 20 1 1, 25 30 2 2)'),
	('polygon z', 'SRID=4326;POLYGON Z ((20 20 1, 30 20 1, 30 30 1, 20 20 1))'),
	('triangle', 'SRID=4326;TRIANGLE((20 20, 20 30, 30 30, 20 20))'),
	('tin', 'SRID=4326;TIN(((20 20 0, 20 30 0, 30 30 0, 20 20 0)))'),
	('polyhedral surface', 'SRID=4326;POLYHEDRALSURFACE(((20 20 0, 20 30 0, 30 30 0, 20 20 0)))'),
	('point at NaN', ST_SetSRID(ST_MakePoint('NaN', 50), 4326)),
	('line through NaN', ST_SetSRID(ST_MakeLine(ST_MakePoint('NaN', 'NaN'), ST_MakePoint(50, 50)), 4326)),
	('point at infinity', ST_SetSRID(ST_MakePoint('Infinity', 50), 4326)),
	('point past floats', ST_SetSRID(ST_MakePoint(1e300, -1e300), 4326)),
	('point between floats', ST_SetSRID(ST_MakePoint(0.1, 1e-50), 4326)),
	('box past floats', ST_MakeEnvelope(-1e39, -1e39, 1e39, 1e39, 4326)),
	('line between floats', ST_SetSRID(ST_MakeLine(ST_MakePoint(0.1, 0.2), ST_MakePoint(0.3, 0.7)), 4326));
-- values stored out of line, and compressed: buffers of many points
INSERT INTO geoms (kind, g)
SELECT 'buffer out of line', ST_SetSRID(ST_Buffer(ST_MakePoint(50, 50), 10 + i, 600), 4326)
FROM generate_series(1, 2) i;
CREATE TABLE geoms_main (g geometry);
INSERT INTO geoms_main SELECT ST_SetSRID(ST_Buffer(ST_MakePoint(60, 60), 5, 600), 4326);
INSERT INTO geoms (kind, g) SELECT 'buffer compressed', g FROM geoms_main;
DROP TABLE geoms_main;
SELECT count(*) AS geometries,
	   count(*) FILTER (WHERE pg_column_toast_chunk_id(g) IS NOT NULL) AS out_of_line
FROM geoms;
ANALYZE geoms;

-- the reader's view of each, and PostGIS's
CREATE TABLE seen AS
SELECT id, kind, g, (vexec_postgis.reader(g)).*,
	   ST_GeometryType(g) AS pg_type, ST_IsEmpty(g) AS pg_empty, postgis_getbbox(g) AS pg_box
FROM geoms;

-- what it knows, it knows as PostGIS does
SELECT count(*) FILTER (WHERE known) AS known, count(*) AS geometries,
	   count(*) FILTER (WHERE known AND type IS DISTINCT FROM
			(array_position(ARRAY['ST_Point', 'ST_LineString', 'ST_Polygon', 'ST_MultiPoint', 'ST_MultiLineString',
								  'ST_MultiPolygon', 'ST_GeometryCollection', 'ST_CircularString', 'ST_CompoundCurve'],
							pg_type))) AS other_type,
	   count(*) FILTER (WHERE known AND empty IS DISTINCT FROM pg_empty) AS other_emptiness,
	   count(*) FILTER (WHERE known AND box_xmin IS NOT NULL AND
			(box_xmin, box_xmax, box_ymin, box_ymax) IS DISTINCT FROM
			(ST_XMin(pg_box), ST_XMax(pg_box), ST_YMin(pg_box), ST_YMax(pg_box))) AS other_box,
	   count(*) FILTER (WHERE known AND NOT empty AND box_xmin IS NULL) AS without_box
FROM seen;
-- the boxes it computes, of the values that keep none
SELECT kind, box_xmin, box_xmax, box_ymin, box_ymax,
	   ST_XMin(pg_box) = box_xmin AND ST_XMax(pg_box) = box_xmax AND
	   ST_YMin(pg_box) = box_ymin AND ST_YMax(pg_box) = box_ymax AS postgis_box
FROM seen WHERE kind IN ('line of 1', 'point z', 'point m', 'point zm', 'line zm', 'point at infinity',
						 'point past floats', 'point between floats', 'line between floats')
ORDER BY id;
-- what it leaves to PostGIS
SELECT kind, pg_type, pg_empty FROM seen WHERE NOT known ORDER BY id;
-- equal SRID bytes where the SRIDs are equal, and only there
SELECT count(*) AS pairs,
	   count(*) FILTER (WHERE (a.srid = b.srid) IS DISTINCT FROM (ST_SRID(a.g) = ST_SRID(b.g))) AS disagree
FROM seen a, seen b;
DROP TABLE seen;
