-- SPDX-License-Identifier: Apache-2.0
--
-- The batch-source and batch-sink contracts (vexec_source.h at minor 2,
-- vexec_sink.h) on vanilla PostgreSQL, through vexec_test's access method,
-- heap's with a source and a sink of every member
-- (modules/vexec_test/source_test.c): each query run with vexec.mode off,
-- and in force mode in both formats, and source_check() printing
-- PostgreSQL's answer where every run gives it, as vexec_scan's
-- vexec_check() does.  Then which of the members vexec calls.
--
-- The source reads units of 1,000 rows ahead and keeps their statistics:
-- vexec's aggregates over a scan with no qual are answered from them, a
-- running bound passes the units it rules out over, and estimate() prices
-- the scan.  A column of type xml it does not support: a scan reading one
-- stays on the slot path.  The sink takes every type but xml.  Under
-- vexec_test.source_text_offsets the source hands text as offsets, which
-- the scan keeps (VC14).

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
CREATE EXTENSION IF NOT EXISTS vexec_test;
RESET client_min_messages;

-- the source and the sink, with their members
SELECT * FROM vexec.sources() WHERE source = 'vexec_test';
SELECT * FROM vexec.sinks() WHERE sink = 'vexec_test';

CREATE FUNCTION source_run(q text, m text, f text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r text;
	n int8;
BEGIN
	PERFORM set_config('vexec.mode', m, true);
	PERFORM set_config('vexec.batch_format', f, true);
	BEGIN
		EXECUTE format('SELECT coalesce(string_agg(x::text, %L ORDER BY x::text COLLATE "C"), %L), count(*) FROM (%s) x',
					   ' | ', '(none)', q) INTO r, n;
		IF length(r) > 200 THEN
			r := format('%s rows, md5 %s', n, md5(r));
		END IF;
	EXCEPTION WHEN OTHERS THEN
		r := 'ERROR ' || SQLSTATE || ': ' || SQLERRM;
	END;
	RETURN r;
END
$$;

CREATE FUNCTION source_check(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ref text := source_run(q, 'off', 'postgres');
	got text;
	f text;
BEGIN
	FOREACH f IN ARRAY ARRAY['postgres', 'arrow'] LOOP
		got := source_run(q, 'force', f);
		IF got IS DISTINCT FROM ref THEN
			RETURN format('DIFFERS in the %s format: off gives [%s], force gives [%s]', f, ref, got);
		END IF;
	END LOOP;
	RETURN ref;
END
$$;

CREATE TABLE src (a int4, b int8, c text, d date, e bool, f numeric(10,2), g int2, u uuid, x xml)
  USING vexec_test_am;
INSERT INTO src
SELECT i, i * 7, CASE WHEN i % 5 = 0 THEN NULL ELSE 'v' || i END, DATE '2024-01-01' + i % 365,
	   i % 2 = 0, CASE WHEN i % 11 = 0 THEN NULL ELSE i / 3.0 END, (i % 1000 - 500)::int2,
	   md5(i::text)::uuid, ('<x>' || i || '</x>')::xml
FROM generate_series(1, 10000) i;
ANALYZE src;
SET vexec.min_rows = 0;

-- the plan reads through the source, and with an xml column through the slot path
SET vexec.mode = force;
EXPLAIN (COSTS OFF, VERBOSE) SELECT a, c FROM src WHERE a % 10 = 3;
EXPLAIN (COSTS OFF, VERBOSE) SELECT a, x FROM src WHERE a < 3;
RESET vexec.mode;

-- every column's batches, the TIDs, quals
SELECT source_check('SELECT a, b, c, d, e, f, g, u FROM src');
SELECT source_check('SELECT ctid, a FROM src WHERE a % 997 = 1');
SELECT source_check('SELECT a, c FROM src WHERE c LIKE ''v1%'' AND e');
SELECT source_check('SELECT count(*), sum(f), min(c), max(u::text) FROM src WHERE d > DATE ''2024-06-01''');
SELECT source_check('SELECT a, x::text FROM src WHERE a < 3');

-- aggregates answered from the units' statistics
SET vexec.mode = force;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT count(*), count(c), min(a), max(b), sum(g) FROM src;
RESET vexec.mode;
SELECT source_check('SELECT count(*), count(c), min(a), max(b), sum(g) FROM src');
-- not all of them: the units read
SELECT source_check('SELECT count(*), sum(f) FROM src');

-- a running bound passes over the units none of whose rows is within it
SET vexec.mode = force;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT a, c FROM src ORDER BY a LIMIT 3;
RESET vexec.mode;
SELECT source_check('SELECT a, c FROM src ORDER BY a LIMIT 3');
SELECT source_check('SELECT a, c FROM src ORDER BY a DESC LIMIT 3');

-- text as the source hands it (VC14): int32 offsets into its bytes here, as
-- MergeTree's source hands its strings, read so by the scan's kernels and
-- by its parents -- an aggregation, a sort, a row node -- and made the
-- format's only where PostgreSQL's evaluator reads it, a lazy target or
-- qual's, at the rows past the vector quals; with
-- vexec.enable_source_layouts off, as each batch arrives
SET vexec_test.source_text_offsets = on;
SET vexec.mode = force;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT a, regexp_replace(c, 'v(.)', 'w\1') FROM src WHERE c LIKE 'v1%' AND a % 7 = 0;
RESET vexec.mode;
SELECT source_check('SELECT a, c FROM src WHERE c LIKE ''v1%'' AND e');
SELECT source_check('SELECT count(*) FROM src WHERE c LIKE ''%99%''');
SELECT source_check('SELECT c, count(*) FROM src WHERE a % 3 = 0 GROUP BY c ORDER BY c LIMIT 5');
SELECT source_check('SELECT a, length(c), c > ''v5'' FROM src WHERE c <> ''v7'' AND a < 300');
SELECT source_check('SELECT a, regexp_replace(c, ''v(.)'', ''w\1'') FROM src WHERE a % 97 = 0');
SELECT source_check('SELECT a, c FROM src WHERE regexp_replace(c, ''v'', '''') = ''123'' OR a = 7');
SELECT source_check('SELECT min(c), max(c), count(c), count(DISTINCT c) FROM src WHERE a < 5000');
SELECT source_check('SELECT a, c FROM src ORDER BY c DESC NULLS LAST, a LIMIT 3');
SET vexec.enable_source_layouts = off;
SELECT source_check('SELECT a, c FROM src WHERE c LIKE ''v1%'' AND e');
SELECT source_check('SELECT a, regexp_replace(c, ''v(.)'', ''w\1'') FROM src WHERE a % 97 = 0');
RESET vexec.enable_source_layouts;
RESET vexec_test.source_text_offsets;

-- rescans: a subquery's scan, for each outer row
CREATE TABLE src_outer (k int4);
INSERT INTO src_outer VALUES (3), (5000), (9999);
SELECT source_check('SELECT k, (SELECT c FROM src WHERE a = k) FROM src_outer');

-- the sink: every type but xml; a table with an xml column is written by rows
CREATE TABLE src_copy (a int4, b int8, c text, d date, e bool, f numeric(10,2), g int2, u uuid)
  USING vexec_test_am;
SET vexec.mode = force;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  INSERT INTO src_copy SELECT a, b, c, d, e, f, g, u FROM src;
RESET vexec.mode;
SELECT (SELECT md5(string_agg(t::text, '|' ORDER BY a)) FROM src_copy t) =
	   (SELECT md5(string_agg(t::text, '|' ORDER BY a)) FROM (SELECT a, b, c, d, e, f, g, u FROM src) t) AS copied;
CREATE TABLE src_xml (LIKE src) USING vexec_test_am;
SET vexec.mode = force;
EXPLAIN (COSTS OFF) INSERT INTO src_xml SELECT * FROM src;
INSERT INTO src_xml SELECT * FROM src;
RESET vexec.mode;
SELECT count(*) FROM src_xml;

-- which members vexec called: every one but retain and release, which it never asks for
SELECT member, calls > 0 AS called FROM vexec_test.source_calls();

DROP TABLE src, src_outer, src_copy, src_xml;
DROP FUNCTION source_check(text), source_run(text, text, text);
