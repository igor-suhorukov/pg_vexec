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

-- The egress (V10, vexec_egress.h): a query through vexec's receiver, as a
-- Flight session's statement goes, and the stream of IPC messages it wrote
-- -- its schema, its batches, the end-of-stream marker -- with what the
-- receiver counted: rows, batches, and the batches that went out from a
-- vector node's own buffers; and the schema's fields, "name format", an
-- extension's name after the format.
CREATE FUNCTION egress(query text, OUT rows int8, OUT batches int8,
                       OUT vector_batches int8, OUT fields text[], OUT stream bytea)
RETURNS record
AS 'MODULE_PATHNAME', 'vexec_test_egress'
LANGUAGE C STRICT VOLATILE;

-- A stream of parameter batches -- the egress's own, or pyarrow's -- read by
-- the egress's parameter reader as values of the given types: a row of
-- their text each, " | " between them.
CREATE FUNCTION egress_params(stream bytea, types regtype[], OUT rownum int8, OUT "row" text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_test_egress_params'
LANGUAGE C STRICT VOLATILE;

-- Ingest (§3.16, the egress's minor version 1): an encapsulated IPC stream --
-- egress()'s, or pyarrow's -- begun as a client's stream for
-- vexec.ingest_stream(handle), in this transaction: its handle, and its
-- columns, "name type" each, the type its Arrow type names.
CREATE FUNCTION ingest_begin(stream bytea, OUT handle int8, OUT columns text[])
RETURNS record
AS 'MODULE_PATHNAME', 'vexec_test_ingest_begin'
LANGUAGE C STRICT VOLATILE;

-- What a test ingest stream's reader has read: its rows, and whether it read
-- to the end.
CREATE FUNCTION ingest_status(handle int8, OUT rows int8, OUT finished bool)
RETURNS record
AS 'MODULE_PATHNAME', 'vexec_test_ingest_status'
LANGUAGE C STRICT VOLATILE;

-- The frames of V7: every batch of a query, in a configuration, sent as a
-- Redistribute sends it -- its rows dealt among "parts" segments, a frame
-- each, after its shapes' schema -- each frame decoded where it lies
-- aligned and from one byte past it, and every value compared with the
-- row's.  An error names the first value that did not come back.
CREATE FUNCTION frames(query text, format text DEFAULT 'postgres',
                       varlena text DEFAULT 'format', bool text DEFAULT 'format',
                       temporal text DEFAULT 'format', "numeric" text DEFAULT 'format',
                       parts int4 DEFAULT 1,
                       OUT col int4, OUT type text, OUT layouts text, OUT rows int8,
                       OUT nulls int8, OUT frames int8, OUT schemas int4, OUT bytes int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_test_frames'
LANGUAGE C VOLATILE;

-- The first batch's frame of a query with byte "at" set to "value", cut to
-- "cut" bytes -- both counted from its end where negative -- decoded: its
-- error where it is malformed.
CREATE FUNCTION frame_patch(query text, at int4, value int4, cut int4 DEFAULT NULL)
RETURNS text
AS 'MODULE_PATHNAME', 'vexec_test_frame_patch'
LANGUAGE C VOLATILE;

-- The vector cdbhash of a query's rows, its columns a distribution key,
-- against gp_core's own cdbhash, a row at a time, over "nsegs" segments:
-- the rows, the key columns a kernel hashed, and the rows whose segments
-- differ.  Needs gp_core.
CREATE FUNCTION cdbhash(query text, nsegs int4, format text DEFAULT 'postgres',
                        varlena text DEFAULT 'format', bool text DEFAULT 'format',
                        temporal text DEFAULT 'format', "numeric" text DEFAULT 'format',
                        OUT rows int8, OUT kernels int4, OUT mismatches int8)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'vexec_test_cdbhash'
LANGUAGE C VOLATILE;
