-- SPDX-License-Identifier: Apache-2.0
--
-- vexec's SQL objects: what a person asks of it.  vexec stores no data, so
-- none of these holds any; each reads the running server.  Calling one where
-- vexec is not preloaded loads the library, which refuses (§1.2).

\echo Use "CREATE EXTENSION vexec" to load this file. \quit

-- The batch sources storage modules registered (vexec_source.h), each with
-- the table access method of this database it serves, and the optional
-- members it has: estimates for the cost model, aggregates answered from
-- its statistics.
CREATE FUNCTION sources(OUT source text, OUT access_method text,
                        OUT minor int4, OUT estimates bool, OUT aggregates bool)
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

-- The built-in functions vexec runs as kernels, bound by their OIDs
-- (pg_vector_executor.md §3.7), with each kernel's family and whether it can
-- mark a row PostgreSQL's evaluator must compute.
CREATE FUNCTION kernels(OUT funcid oid, OUT function text, OUT family text,
                        OUT can_fail bool)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_kernel_list'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- The kernel packs registered in this server (vexec_kernels.h, §3.17): each
-- with the extension it serves, the versions of it its declarations were
-- written for, and how many it declares.
CREATE FUNCTION kernel_packs(OUT pack text, OUT extension text, OUT versions text[],
                             OUT declarations int4, OUT minor int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_kernel_packs'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- The functions of this database a call of which binds to a pack's
-- declaration now: the extension's own C function, of a version the pack
-- names, under its name and signature.
CREATE FUNCTION declared_calls(OUT funcid oid, OUT function text, OUT extension text,
                               OUT version text, OUT pack text, OUT declaration text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_declared_calls'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;
