-- SPDX-License-Identifier: Apache-2.0
--
-- vexec_pgvector's SQL objects, for a person and the pack's tests: the pack
-- itself declares by being preloaded, and needs none of them.

\echo Use "CREATE EXTENSION vexec_pgvector" to load this file. \quit

-- What the pack declares of pgvector's functions, the pgvector versions it
-- holds for, and whether the pack registered in this server: it does where
-- it is in shared_preload_libraries.
CREATE FUNCTION declarations(OUT name text, OUT argtypes text[], OUT symbol text,
                             OUT declaration text, OUT versions text[], OUT registered bool)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_pgvector_declarations'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- The distances' check, as vexec calls it: the two dimensions agree.  It
-- raises nothing; vexec hands it its arguments detoasted, and a call from
-- SQL reads a toasted value's first bytes alone.
CREATE FUNCTION dims_check(@extschema:vector@.vector, @extschema:vector@.vector) RETURNS bool
AS 'MODULE_PATHNAME', 'vexec_pgvector_dims_check'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION dims_check(@extschema:vector@.halfvec, @extschema:vector@.halfvec) RETURNS bool
AS 'MODULE_PATHNAME', 'vexec_pgvector_dims_check'
LANGUAGE C IMMUTABLE PARALLEL SAFE;
