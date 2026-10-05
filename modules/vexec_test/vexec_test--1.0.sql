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
