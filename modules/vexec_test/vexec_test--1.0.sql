-- SPDX-License-Identifier: Apache-2.0
--
-- vexec_test's functions, for vexec's own regression suite (V0).  vexec must
-- be preloaded wherever they run.

\echo Use "CREATE EXTENSION vexec_test" to load this file. \quit

-- Every batch of a query's rows, every column through every layout its type
-- has, between every pair and back, dictionary- and constant-encoded, then
-- out through a slot in both formats, and compacted.  An error names the
-- first value that did not come back as it went in.
CREATE FUNCTION roundtrip(query text, typmods int4[] DEFAULT NULL,
                          OUT col int4, OUT type text, OUT layouts int4,
                          OUT pairs int4, OUT kept int4, OUT dictionaries int4,
                          OUT constants int4, OUT rows int8, OUT batches int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_test_roundtrip'
LANGUAGE C VOLATILE;

-- The export check: every batch, in a configuration of vexec's batch
-- settings, exported through the C Data Interface, validated by nanoarrow's
-- full validation and vexec's own check of views, and read back value by
-- value.
CREATE FUNCTION export(query text, format text DEFAULT 'postgres',
                       varlena text DEFAULT 'format', bool text DEFAULT 'format',
                       temporal text DEFAULT 'format', "numeric" text DEFAULT 'format',
                       dictionary bool DEFAULT false,
                       OUT col int4, OUT type text, OUT layout text,
                       OUT arrow text, OUT extension text,
                       OUT rows int8, OUT nulls int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_test_export'
LANGUAGE C VOLATILE;

-- A numeric through its scaled form at a scale, and back.
CREATE FUNCTION scaled(value numeric, scale int4, width int4 DEFAULT 16)
RETURNS text
AS 'MODULE_PATHNAME', 'vexec_test_scaled'
LANGUAGE C STRICT IMMUTABLE;

-- The IPC codec (V7_0): every batch of a query, exported as export() exports
-- it, written as Arrow IPC messages and read back, validated, and compared
-- with the export bit for bit.  An error names the first difference.
CREATE FUNCTION ipc(query text, format text DEFAULT 'postgres',
                    varlena text DEFAULT 'format', bool text DEFAULT 'format',
                    temporal text DEFAULT 'format', "numeric" text DEFAULT 'format',
                    OUT col int4, OUT type text, OUT arrow text, OUT rows int8,
                    OUT nulls int8, OUT schemas int4, OUT batches int4)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_test_ipc'
LANGUAGE C VOLATILE;

-- The same batches as encapsulated streams, a stream for each run of
-- batches of one schema, with the export's format strings.
CREATE FUNCTION ipc_stream(query text, format text DEFAULT 'postgres',
                           varlena text DEFAULT 'format', bool text DEFAULT 'format',
                           temporal text DEFAULT 'format', "numeric" text DEFAULT 'format',
                           OUT stream bytea, OUT formats text[], OUT batches int4,
                           OUT rows int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_test_ipc_stream'
LANGUAGE C VOLATILE;

-- A stream read by vexec's reader and written back by its writer; read from
-- an 8-byte aligned copy, or from one byte past it.
CREATE FUNCTION ipc_reencode(stream bytea, aligned bool DEFAULT true)
RETURNS bytea
AS 'MODULE_PATHNAME', 'vexec_test_ipc_reencode'
LANGUAGE C STRICT VOLATILE;

-- Where a stream's messages lie.
CREATE FUNCTION ipc_messages(stream bytea, OUT n int4, OUT kind text, OUT start int8,
                             OUT metadata_len int8, OUT body_start int8, OUT body_len int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_test_ipc_messages'
LANGUAGE C STRICT VOLATILE;

-- A stream with one number of its message-th message changed: version,
-- body_length, batch_length, node_length, node_nulls, buffer_offset or
-- buffer_length, the last four of FieldNode or Buffer idx.
CREATE FUNCTION ipc_patch(stream bytea, message int4, what text, idx int4, value int8)
RETURNS bytea
AS 'MODULE_PATHNAME', 'vexec_test_ipc_patch'
LANGUAGE C STRICT VOLATILE;

-- A value's bytes as PostgreSQL holds them: a varlena's payload, detoasted.
CREATE FUNCTION raw(value anyelement)
RETURNS bytea
AS 'MODULE_PATHNAME', 'vexec_test_raw'
LANGUAGE C STRICT IMMUTABLE;
