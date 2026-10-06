-- SPDX-License-Identifier: Apache-2.0
--
-- The Arrow IPC codec (pg_vector_executor.md §3.10, "No library"; §5
-- V7_0): every type of the layouts' corpus (§6.2), exported from both
-- formats and with each per-structure setting, written as Arrow IPC
-- messages and read back bit for bit; streams read and written back as
-- they were; and malformed input refused with its SQLSTATE, never read
-- outside its bytes.  vexec_test runs the checks (ipc_test.c);
-- test/vexec/ipc.sh checks the codec against Arrow's own implementation.

\i sql/vexec_corpus_setup.sql

-- every batch written and read back, in each format: the schema, and every
-- value bit for bit, read where it lies and from an unaligned copy
SELECT * FROM vexec_test.ipc('SELECT * FROM corpus ORDER BY id', 'postgres');
SELECT * FROM vexec_test.ipc('SELECT * FROM corpus ORDER BY id', 'arrow');

-- Arrow's own date32, time64, timestamps and month_day_nano, where the
-- values allow them
SELECT col, type, arrow, rows, nulls FROM vexec_test.ipc(
	'SELECT d, t, ts, tstz, iv FROM corpus WHERE id % 50 NOT BETWEEN 15 AND 29 ORDER BY id', 'arrow');

-- each per-structure setting changed alone, in each format
SELECT f.format, v.varlena, b.bool, t.temporal, n.numeric,
       count(*) AS columns, sum(e.nulls) AS nulls, min(e.rows) AS rows,
       max(e.schemas) AS schemas, string_agg(DISTINCT e.arrow, ' ' ORDER BY e.arrow) AS formats
FROM (VALUES ('postgres'), ('arrow')) f(format),
     (VALUES ('format'), ('datum'), ('view'), ('offsets')) v(varlena),
     (VALUES ('format'), ('byte'), ('bit')) b(bool),
     (VALUES ('format'), ('postgres'), ('arrow')) t(temporal),
     (VALUES ('format'), ('scaled'), ('varlena')) n(numeric),
     LATERAL vexec_test.ipc('SELECT * FROM corpus ORDER BY id', f.format,
                            v.varlena, b.bool, t.temporal, n.numeric) e
WHERE (v.varlena = 'format')::int + (b.bool = 'format')::int +
      (t.temporal = 'format')::int + (n.numeric = 'format')::int >= 3
GROUP BY 1, 2, 3, 4, 5 ORDER BY 1, 2, 3, 4, 5;

-- empty results: a schema and an empty batch
SELECT col, arrow, rows, nulls, schemas, batches
FROM vexec_test.ipc('SELECT * FROM corpus WHERE false', 'arrow', 'offsets')
WHERE col <= 3 OR arrow IN ('u', 'z');

-- the corpus as streams, read and written back as they were, from aligned
-- and unaligned bytes
SELECT f.format, count(*) AS streams, sum(s.batches) AS batches, sum(s.rows) AS rows,
       bool_and(vexec_test.ipc_reencode(s.stream) = s.stream) AS same,
       bool_and(vexec_test.ipc_reencode(s.stream, false) = s.stream) AS same_unaligned
FROM (VALUES ('postgres'), ('arrow')) f(format),
     LATERAL vexec_test.ipc_stream('SELECT * FROM corpus ORDER BY id', f.format) s
GROUP BY 1 ORDER BY 1;

DROP TABLE corpus;
DROP FUNCTION corpus_null(int4, int4);

-- Malformed and unsupported input, each refused with its SQLSTATE
CREATE FUNCTION ipc_try(stream bytea, aligned bool DEFAULT true) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	detail text;
BEGIN
	PERFORM vexec_test.ipc_reencode(stream, aligned);
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS detail = PG_EXCEPTION_DETAIL;
	RETURN SQLSTATE || ' ' || SQLERRM || CASE WHEN detail <> '' THEN ': ' || detail ELSE '' END;
END $$;

-- nested types and unions, written by pyarrow 25.0.1: a list, a large
-- list, a fixed-size list, a struct of a list of views, a sorted map, a
-- dense union of type ids 5, 17 and 2 and a sparse one of 3 and 7, decimal256,
-- month_day_nano and null, four rows; read, written back, and written back
-- the same again
CREATE TABLE ipc_nested AS SELECT decode('
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
UAEAAAAAAAAQAAAAAAAAAGABAAAAAAAAAwAAAAAAAABoAQAAAAAAAAEAAAAAAAAAcAEAAAAAAAAM
AAAAAAAAAIABAAAAAAAABAAAAAAAAACIAQAAAAAAABAAAAAAAAAAmAEAAAAAAAABAAAAAAAAAKAB
AAAAAAAACAAAAAAAAACoAQAAAAAAAAAAAAAAAAAAqAEAAAAAAAAIAAAAAAAAALABAAAAAAAAHQAA
AAAAAADQAQAAAAAAAAAAAAAAAAAA0AEAAAAAAAAIAAAAAAAAANgBAAAAAAAAAAAAAAAAAADYAQAA
AAAAAAEAAAAAAAAA4AEAAAAAAAAEAAAAAAAAAOgBAAAAAAAAAQAAAAAAAADwAQAAAAAAACAAAAAA
AAAAEAIAAAAAAAABAAAAAAAAABgCAAAAAAAAFAAAAAAAAAAwAgAAAAAAAAIAAAAAAAAAOAIAAAAA
AAABAAAAAAAAAEACAAAAAAAAgAAAAAAAAADAAgAAAAAAAAEAAAAAAAAAyAIAAAAAAABAAAAAAAAA
AAAAAAAZAAAABAAAAAAAAAABAAAAAAAAAAQAAAAAAAAAAQAAAAAAAAAEAAAAAAAAAAEAAAAAAAAA
AwAAAAAAAAABAAAAAAAAAAQAAAAAAAAAAQAAAAAAAAAMAAAAAAAAAAQAAAAAAAAABAAAAAAAAAAB
AAAAAAAAAAQAAAAAAAAAAQAAAAAAAAAEAAAAAAAAAAEAAAAAAAAAAgAAAAAAAAAAAAAAAAAAAAQA
AAAAAAAAAQAAAAAAAAADAAAAAAAAAAAAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAAMAAAAAAAAAAQAA
AAAAAAAEAAAAAAAAAAAAAAAAAAAAAgAAAAAAAAABAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAA
AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAEAAAAA
AAAABAAAAAAAAAABAAAAAAAAAAQAAAAAAAAAAQAAAAAAAAAEAAAAAAAAAAEAAAAAAAAABAAAAAAA
AAAEAAAAAAAAAA0AAAAAAAAAAAAAAAMAAAADAAAAAwAAAAQAAAAAAAAADQAAAAAAAAABAAAAAAAA
AAMAAAAEAAAADQAAAAAAAAAAAAAAAAAAAAIAAAAAAAAAAgAAAAAAAAACAAAAAAAAAAMAAAAAAAAA
BQAAAAAAAAAAAAAAAQAAAAEAAAAEAAAAYWJjZAAAAAANAAAAAAAAAIcPAAAAAAAAAQACAAMAAAAA
AAAAAAAFAAYABwAIAAkADQAAAAAAAAALAAAAAAAAAAEAAAAAAAAAAAAAAAQAAAALAAAAAAAAAAAA
AAACAAAAAgAAAAIAAAACAAAAAAAAAAEAAAB4AAAAAAAAAAAAAAAZAAAAYSB2aQAAAAAAAAAAYSB2
aWV3IGxvbmdlciB0aGFuIHR3ZWx2ZQAAAAAAAAANAAAAAAAAAAAAAAACAAAAAgAAAAIAAAADAAAA
AAAAAAAAAAABAAAAAgAAAAMAAABrbG0AAAAAAAUAAAAAAAAAAQAAAAAAAAADAAAAAAAAAAURBQIA
AAAAAAAAAAAAAAABAAAAAAAAAAEAAAAAAAAAAQAAAAAAAAAAAAAAHQAAAGEgdW5pb24ncyBzdHJp
bmcsIG91dCBvZiBsaW5lAAAAAAAAAAEAAAABAAAAAAAAAAMHBwMAAAAADQAAAAAAAAAAAAAAAAD4
PwAAAAAAAAAAAAAAAAAAAAAAAAAAAADwvwsAAAAAAAAAAAAAAAEAAAACAAAAAgAAAAIAAAAAAAAA
eHkAAAAAAAANAAAAAAAAAJYAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA4/////////////////////////////////////////wAA
AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADQAAAAAAAAABAAAAAgAAAAMAAAAAAAAAAAAA
AAAAAAAAAAAAAAAAAP/////+/////f////////8AAAAAAAAAAAAAAAAAAAAA/////wAAAAA=', 'base64') AS stream;
SELECT ipc_try(stream) AS outcome,
       vexec_test.ipc_reencode(vexec_test.ipc_reencode(stream)) =
       vexec_test.ipc_reencode(stream) AS same_again,
       vexec_test.ipc_reencode(stream, false) = vexec_test.ipc_reencode(stream) AS same_unaligned
FROM ipc_nested;

-- its children made shorter than their parents need: FieldNodes in the
-- schema's pre-order, 1 the list's values, 5 the fixed-size list's, 7 the
-- struct's first child, 11 the map's entries, 16 the dense union's
-- strings, 20 the sparse union's first child
SELECT c.what, ipc_try(vexec_test.ipc_patch(stream, 2, 'node_length', c.node, c.length)) AS outcome
FROM ipc_nested,
     (VALUES ('a list''s values', 1, 2), ('a fixed-size list''s values', 5, 11),
             ('a struct''s child', 7, 3), ('a map''s entries', 11, 1),
             ('a dense union''s child', 16, 0), ('a sparse union''s child', 20, 3)) c(what, node, length)
ORDER BY c.node;
DROP TABLE ipc_nested;

-- two small streams: utf8 with offsets, and with views; the second value
-- of each row is longer than a view holds inline
CREATE TABLE ipc_small AS
SELECT l.layout, s.stream
FROM (VALUES ('offsets'), ('view')) l(layout),
     LATERAL vexec_test.ipc_stream($$SELECT 'abc'::text AS t,
                                    'Ünïcödé, longer than a view holds'::text AS u,
                                    7 AS i$$, 'arrow', l.layout) s;
SELECT l.layout, m.* FROM ipc_small l, vexec_test.ipc_messages(l.stream) m ORDER BY 1, 2;

-- cut short anywhere: only a cut between messages reads
SELECT CASE WHEN outcome = 'ok' THEN 'ok' ELSE 'refused' END AS outcome,
       count(*) AS cuts, string_agg(DISTINCT outcome, ' | ') FILTER (WHERE outcome <> 'ok') AS how
FROM (SELECT k, ipc_try(substring(stream FROM 1 FOR k)) AS outcome
      FROM ipc_small, generate_series(0, length(stream)) k WHERE layout = 'offsets') x
GROUP BY 1 ORDER BY 1;
SELECT k, ipc_try(substring(stream FROM 1 FOR k)) AS outcome
FROM ipc_small, LATERAL (SELECT (body_start + body_len)::int AS k FROM vexec_test.ipc_messages(stream)) m
WHERE layout = 'offsets' ORDER BY k;

-- the body of the offsets stream: t's offsets at 0 and data at 8, u's
-- offsets at 16 and data at 24, i's value at 64
CREATE TABLE ipc_case (name text, stream bytea);
INSERT INTO ipc_case
SELECT c.name, CASE c.name
	WHEN 'an offset past its data' THEN overlay(stream PLACING '\x10000000' FROM b + 5)
	WHEN 'an offset before the one before' THEN overlay(stream PLACING '\xfeffffff' FROM b + 21)
	WHEN 'a first offset below 0' THEN overlay(stream PLACING '\xffffffff' FROM b + 1)
	WHEN 'utf8: a byte that starts nothing' THEN overlay(stream PLACING '\xff' FROM b + 9)
	WHEN 'utf8: an overlong form' THEN overlay(stream PLACING '\xc080' FROM b + 25)
	WHEN 'utf8: a surrogate' THEN overlay(stream PLACING '\xeda080' FROM b + 25)
	WHEN 'utf8: a character cut short' THEN overlay(stream PLACING '\x61' FROM b + 26)
	WHEN 'utf8: U+0000, which Arrow allows' THEN overlay(stream PLACING '\x00' FROM b + 10)
	WHEN 'a buffer past the body' THEN vexec_test.ipc_patch(stream, 2, 'buffer_offset', 1, 4096)
	WHEN 'a buffer of 2^40 bytes' THEN vexec_test.ipc_patch(stream, 2, 'buffer_length', 2, 1099511627776)
	WHEN 'a buffer at -8' THEN vexec_test.ipc_patch(stream, 2, 'buffer_offset', 4, -8)
	WHEN 'a body shorter than its buffers' THEN vexec_test.ipc_patch(stream, 2, 'body_length', 0, 16)
	WHEN 'more NULLs than values' THEN vexec_test.ipc_patch(stream, 2, 'node_nulls', 0, 2)
	WHEN 'a NULL without a validity bitmap' THEN vexec_test.ipc_patch(stream, 2, 'node_nulls', 1, 1)
	WHEN 'a column shorter than its batch' THEN vexec_test.ipc_patch(stream, 2, 'node_length', 2, 0)
	WHEN 'a batch longer than its columns' THEN vexec_test.ipc_patch(stream, 2, 'batch_length', 0, 5)
	WHEN 'a batch of 2^40 rows' THEN vexec_test.ipc_patch(stream, 2, 'batch_length', 0, 1099511627776)
	WHEN 'metadata version V3' THEN vexec_test.ipc_patch(stream, 1, 'version', 0, 2)
	WHEN 'metadata version V4' THEN vexec_test.ipc_patch(vexec_test.ipc_patch(stream, 1, 'version', 0, 3), 2, 'version', 0, 3)
	END
FROM ipc_small, LATERAL (SELECT body_start::int AS b FROM vexec_test.ipc_messages(stream) WHERE n = 2) m,
     (VALUES ('an offset past its data'), ('an offset before the one before'),
             ('a first offset below 0'), ('utf8: a byte that starts nothing'),
             ('utf8: an overlong form'), ('utf8: a surrogate'), ('utf8: a character cut short'),
             ('utf8: U+0000, which Arrow allows'), ('a buffer past the body'),
             ('a buffer of 2^40 bytes'), ('a buffer at -8'), ('a body shorter than its buffers'),
             ('more NULLs than values'), ('a NULL without a validity bitmap'),
             ('a column shorter than its batch'), ('a batch longer than its columns'),
             ('a batch of 2^40 rows'), ('metadata version V3'), ('metadata version V4')) c(name)
WHERE layout = 'offsets';

-- the body of the views stream: t's inline view at 0, then t's variadic
-- buffer, the batch's arena, which holds both values, varlena headers and
-- all; then u's view, found by its size and prefix, and u's variadic
-- buffer, the arena again, where u's view points.  No view of t points into
-- t's buffer, so its bytes need not be UTF-8.
INSERT INTO ipc_case
SELECT c.name, CASE c.name
	WHEN 'utf8_view: an inline value that is not UTF-8' THEN overlay(stream PLACING '\xff' FROM b + 5)
	WHEN 'utf8_view: inline padding that is not zero' THEN overlay(stream PLACING '\x78' FROM b + 9)
	WHEN 'utf8_view: a value that is not UTF-8' THEN overlay(stream PLACING '\xc0' FROM v2 + 6)
	WHEN 'utf8_view: bytes no view refers to, not UTF-8' THEN overlay(stream PLACING '\xc0' FROM v1 + 6)
	WHEN 'utf8_view: a prefix that is not its value''s' THEN overlay(stream PLACING '\x58' FROM uv + 4)
	WHEN 'utf8_view: a value past its buffer' THEN overlay(stream PLACING '\xffffff7f' FROM uv + 12)
	WHEN 'utf8_view: a buffer index past the buffers' THEN overlay(stream PLACING '\xffffff7f' FROM uv + 8)
	WHEN 'utf8_view: a size below 0' THEN overlay(stream PLACING '\xffffffff' FROM uv)
	END
FROM ipc_small,
     LATERAL (SELECT body_start::int AS b FROM vexec_test.ipc_messages(stream) WHERE n = 2) m,
     LATERAL (SELECT position('\x25000000c39c6ec3'::bytea IN stream) AS uv,
                     position(convert_to('Ünïcödé', 'UTF8') IN stream) AS v1) p,
     LATERAL (SELECT v1 + position(convert_to('Ünïcödé', 'UTF8') IN substring(stream FROM v1 + 1)) AS v2) q,
     (VALUES ('utf8_view: an inline value that is not UTF-8'),
             ('utf8_view: inline padding that is not zero'), ('utf8_view: a value that is not UTF-8'),
             ('utf8_view: bytes no view refers to, not UTF-8'),
             ('utf8_view: a prefix that is not its value''s'), ('utf8_view: a value past its buffer'),
             ('utf8_view: a buffer index past the buffers'), ('utf8_view: a size below 0')) c(name)
WHERE layout = 'view';

-- written by Arrow's own implementation (pyarrow 25.0.1): a dictionary-
-- encoded field; an int32 column's body compressed with zstd; a stream
-- whose second message is a DictionaryBatch; and two that read, the int32
-- column in metadata V4 and in the format before 0.15, without
-- continuation markers
INSERT INTO ipc_case VALUES
('a dictionary-encoded field', decode('/////5AAAAAQAAAAAAAKAAwABgAFAAgACgAAAAABBAAEAAAAvP///wQAAAABAAAAFAAAABAAGAAIAAYABwAMABAAFAAQAAAAAAABBRQAAABAAAAAHAAAAAQAAAAAAAAAAQAAAGQAAAAIAAgAAAAEAAgAAAAMAAAACAAMAAgABwAIAAAAAAAAASAAAAAEAAQABAAAAAAAAAD/////qAAAABQAAAAAAAAADAAUAAYABQAIAAwADAAAAAACBAAUAAAAGAAAAAAAAAAIAAoAAAAEAAgAAAAQAAAAAAAKABgADAAEAAgACgAAAEwAAAAQAAAAAgAAAAAAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAwAAAAAAAAAEAAAAAAAAAACAAAAAAAAAAAAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAgAAAAAAAAB4eQAAAAAAAP////+IAAAAFAAAAAAAAAAMABYABgAFAAgADAAMAAAAAAMEABgAAAAQAAAAAAAAAAAACgAYAAwABAAIAAoAAAA8AAAAEAAAAAMAAAAAAAAAAAAAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMAAAAAAAAAAAAAAABAAAAAwAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAD/////AAAAAA==', 'base64')),
('a compressed body', decode('/////3gAAAAQAAAAAAAKAAwABgAFAAgACgAAAAABBAAMAAAACAAIAAAABAAIAAAABAAAAAEAAAAUAAAAEAAUAAgABgAHAAwAAAAQABAAAAAAAAECEAAAABwAAAAEAAAAAAAAAAEAAABjAAAACAAMAAgABwAIAAAAAAAAASAAAAD/////oAAAABQAAAAAAAAADAAYAAYABQAIAAwADAAAAAADBAAcAAAAIAAAAAAAAAAAAAAADAAeABAABAAIAAwADAAAAFAAAAAkAAAAGAAAAAMAAAAAAAAAAAAAAAAABgAIAAcABgAAAAAAAAECAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHQAAAAAAAAAAAAAAAQAAAAMAAAAAAAAAAAAAAAAAAAAMAAAAAAAAACi1L/0gDGEAAAEAAAACAAAAAwAAAAAAAP////8AAAAA', 'base64')),
('a DictionaryBatch message', decode('/////3gAAAAQAAAAAAAKAAwABgAFAAgACgAAAAABBAAMAAAACAAIAAAABAAIAAAABAAAAAEAAAAUAAAAEAAUAAgABgAHAAwAAAAQABAAAAAAAAECEAAAABwAAAAEAAAAAAAAAAEAAABjAAAACAAMAAgABwAIAAAAAAAAASAAAAD/////qAAAABQAAAAAAAAADAAUAAYABQAIAAwADAAAAAACBAAUAAAAGAAAAAAAAAAIAAoAAAAEAAgAAAAQAAAAAAAKABgADAAEAAgACgAAAEwAAAAQAAAAAgAAAAAAAAAAAAAAAwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAwAAAAAAAAAEAAAAAAAAAACAAAAAAAAAAAAAAABAAAAAgAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAgAAAAAAAAB4eQAAAAAAAP////8AAAAA', 'base64')),
('pyarrow, metadata version V4', decode('/////3gAAAAQAAAAAAAKAAwABgAFAAgACgAAAAABAwAMAAAACAAIAAAABAAIAAAABAAAAAEAAAAUAAAAEAAUAAgABgAHAAwAAAAQABAAAAAAAAECEAAAABwAAAAEAAAAAAAAAAEAAABjAAAACAAMAAgABwAIAAAAAAAAASAAAAD/////iAAAABQAAAAAAAAADAAWAAYABQAIAAwADAAAAAADAwAYAAAAEAAAAAAAAAAAAAoAGAAMAAQACAAKAAAAPAAAABAAAAADAAAAAAAAAAAAAAACAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAAAAAAAAAAAAAAAAQAAAAMAAAAAAAAAAAAAAAAAAAABAAAAAgAAAAMAAAAAAAAA/////wAAAAA=', 'base64')),
('pyarrow, the format before 0.15', decode('fAAAABAAAAAAAAoADAAGAAUACAAKAAAAAAEEAAwAAAAIAAgAAAAEAAgAAAAEAAAAAQAAABQAAAAQABQACAAGAAcADAAAABAAEAAAAAAAAQIQAAAAHAAAAAQAAAAAAAAAAQAAAGMAAAAIAAwACAAHAAgAAAAAAAABIAAAAAAAAACMAAAAFAAAAAAAAAAMABYABgAFAAgADAAMAAAAAAMEABgAAAAQAAAAAAAAAAAACgAYAAwABAAIAAoAAAA8AAAAEAAAAAMAAAAAAAAAAAAAAAIAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAMAAAAAAAAAAAAAAABAAAAAwAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAgAAAAMAAAAAAAAAAAAAAA==', 'base64'));

SELECT name, ipc_try(stream) AS outcome, ipc_try(stream, false) = ipc_try(stream) AS same_unaligned
FROM ipc_case ORDER BY name;

DROP TABLE ipc_case;
DROP TABLE ipc_small;
DROP FUNCTION ipc_try(bytea, bool);
