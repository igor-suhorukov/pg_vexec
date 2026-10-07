-- SPDX-License-Identifier: Apache-2.0
--
-- vexec_postgis's SQL objects, for a person and the pack's tests: the pack
-- itself declares by being preloaded, and needs none of them.

\echo Use "CREATE EXTENSION vexec_postgis" to load this file. \quit

-- What the pack declares of PostGIS's functions, the PostGIS versions it
-- holds for, and whether the pack registered in this server: it does where
-- it is in shared_preload_libraries.
CREATE FUNCTION declarations(OUT name text, OUT argtypes text[], OUT symbol text,
                             OUT declaration text, OUT versions text[], OUT registered bool)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_postgis_declarations'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- A predicate's prefilter's answer, as vexec takes it: NULL where it leaves
-- the row to PostGIS's own function.  The predicate by its function's name:
-- 'st_intersects'.
CREATE FUNCTION prefilter(predicate text, a @extschema:postgis@.geometry, b @extschema:postgis@.geometry)
RETURNS bool
AS 'MODULE_PATHNAME', 'vexec_postgis_prefilter'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- ST_X's and ST_Y's check: the value is a point.
CREATE FUNCTION is_point(@extschema:postgis@.geometry) RETURNS bool
AS 'MODULE_PATHNAME', 'vexec_postgis_is_point'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- What the pack's reader makes of a value: whether it knows it, its type,
-- whether it is empty, its SRID's bytes, and the predicates' box -- NULL
-- where it has none.
CREATE FUNCTION reader(@extschema:postgis@.geometry, OUT known bool, OUT type int4, OUT empty bool,
                       OUT srid bytea, OUT box_xmin float8, OUT box_xmax float8, OUT box_ymin float8, OUT box_ymax float8)
RETURNS record
AS 'MODULE_PATHNAME', 'vexec_postgis_reader'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;
