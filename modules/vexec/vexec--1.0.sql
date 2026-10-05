-- SPDX-License-Identifier: Apache-2.0
--
-- vexec's SQL objects: what a person asks of it.  vexec stores no data, so
-- none of these holds any; each reads the running server.  Calling one where
-- vexec is not preloaded loads the library, which refuses (§1.2).

\echo Use "CREATE EXTENSION vexec" to load this file. \quit

-- The batch sources storage modules registered (vexec_source.h), each with
-- the table access method of this database it serves.
CREATE FUNCTION sources(OUT source text, OUT access_method text,
                        OUT minor int4, OUT estimates bool)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_sources'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- How a batch holds a type: its class, its layout in the PostgreSQL format
-- and in the Arrow format, and every layout it can be held in.  The type is
-- named as SQL names it, typmod and all: 'numeric(10,2)'.
CREATE FUNCTION type_layouts(type text, OUT class text, OUT postgres text,
                             OUT arrow text, OUT layouts text[])
RETURNS record
AS 'MODULE_PATHNAME', 'vexec_type_layouts'
LANGUAGE C STRICT STABLE PARALLEL SAFE;
