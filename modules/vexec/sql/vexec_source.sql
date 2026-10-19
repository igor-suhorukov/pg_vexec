-- SPDX-License-Identifier: Apache-2.0
--
-- The batch-source and batch-sink contracts (vexec_source.h at minor 5,
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
-- the scan keeps (VC14).  Under vexec_test.source_dictionaries, with
-- vexec.batch_dictionary on, it hands text as dictionaries -- a unit's
-- distinct values, shared by the units that have the same ones and named by
-- dictionary_id() -- and the subtrees over one such column are evaluated
-- over the dictionary's values, once for every batch with it, and an
-- aggregation hashes its keys' values once (VC5).  Under
-- vexec_test.source_ordering it says its rows come in an order, which a
-- serial grouping by a prefix of it takes as they come (VC4); its access
-- method's fetch by TID leaves the columns fetch_columns() did not name
-- NULL, and vexec_test.source_fetch_cost prices a fetch (VC16).  It hands
-- a scan's columns in the steps vexec asks for, a later step's through
-- next_rest() at the rows vexec selects, every other row NULL (VC2).

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

-- estimate_ex() (VC13): an aggregation whose every aggregate the units'
-- statistics answer is planned in one process, priced at its units, where
-- in parallel each participant's units are its own share; a min of text,
-- which they do not keep, a sum, which VEXEC_EST_AGG_EXACT does not promise,
-- and a qual keep the Gather.  The rows the estimate leaves after a qual --
-- a fifth here -- size the scan's workers
SET vexec.mode = auto;
SET max_parallel_workers_per_gather = 4;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET vexec.scan_rows_per_worker = 1000;
EXPLAIN (COSTS OFF) SELECT count(*), count(a), min(a), max(b) FROM src;
EXPLAIN (COSTS OFF) SELECT count(*), min(c) FROM src;
EXPLAIN (COSTS OFF) SELECT count(*), sum(g) FROM src;
EXPLAIN (COSTS OFF) SELECT count(*), max(a) FROM src WHERE b > 7;
SELECT source_check('SELECT count(*), count(a), min(a), max(b) FROM src');
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET vexec.scan_rows_per_worker;
RESET vexec.mode;

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

-- dictionaries (VC5): m of 7 values and NULLs, one dictionary for every
-- unit; t of a value a row, a dictionary a unit; both through quals, targets,
-- aggregations, sorts, joins and PostgreSQL's evaluator, in both formats,
-- with the text as Datums and as offsets
CREATE TABLE srcd (k int4, m text, t text, n int4) USING vexec_test_am;
INSERT INTO srcd
SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE 'm' || (i % 7) END,
	   CASE WHEN i % 17 = 0 THEN NULL ELSE 't' || i END, i % 100
FROM generate_series(1, 10000) i;
ANALYZE srcd;
SET vexec.batch_dictionary = on;
SET vexec_test.source_dictionaries = on;
SET vexec.mode = force;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT k, upper(m) FROM srcd WHERE m <> 'm3' AND m LIKE 'm%' AND k % 1000 = 1;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT m, count(*) FROM srcd GROUP BY m;
RESET vexec.mode;
SELECT member, calls > 0 AS called FROM vexec_test.source_calls()
WHERE member IN ('dictionary_id', 'dictionaries made') ORDER BY member;
SELECT source_check('SELECT k, m, t FROM srcd WHERE k % 997 = 1');
-- the TIDs beside them: after the columns, not over a dictionary's
SELECT source_check('SELECT ctid, k, m FROM srcd WHERE k % 1999 = 1');
SELECT source_check('SELECT m, t FROM srcd WHERE m <> ''m1'' ORDER BY k LIMIT 7');
SELECT source_check('SELECT count(*) FROM srcd WHERE m <> ''m3''');
SELECT source_check('SELECT count(*) FROM srcd WHERE m IS NULL OR m IN (''m1'', ''m5'')');
SELECT source_check('SELECT count(*) FROM srcd WHERE m IS DISTINCT FROM ''m2''');
SELECT source_check('SELECT count(*) FROM srcd WHERE NOT (m LIKE ''%4'') AND n > 50');
SELECT source_check('SELECT k, upper(m), length(t), t || ''!'' FROM srcd WHERE k % 1001 = 0');
SELECT source_check('SELECT m, count(*), min(t), max(k) FROM srcd GROUP BY m');
SELECT source_check('SELECT m, n % 3, count(*) FROM srcd GROUP BY m, n % 3');
SELECT source_check('SELECT upper(m), count(*) FROM srcd GROUP BY upper(m)');
SELECT source_check('SELECT count(DISTINCT m), count(DISTINCT t), count(m) FROM srcd');
SELECT source_check('SELECT t, count(*) FROM srcd WHERE t LIKE ''t99%'' GROUP BY t');
SELECT source_check('SELECT k, m FROM srcd ORDER BY m DESC NULLS FIRST, k LIMIT 5');
SELECT source_check('SELECT a.k, b.m FROM srcd a JOIN srcd b ON a.m = b.m AND a.k = b.k + 7 WHERE a.k < 300');
SELECT source_check('SELECT k, regexp_replace(m, ''m(.)'', ''x\1'') FROM srcd WHERE k % 503 = 0');
SELECT source_check('SELECT count(*) FROM srcd WHERE substr(m, 2)::int4 > 3');
SELECT source_check('SELECT k FROM srcd WHERE coalesce(m, ''none'') = ''none'' AND k < 200');
SET vexec_test.source_text_offsets = on;
SELECT source_check('SELECT count(*) FROM srcd WHERE m <> ''m3''');
SELECT source_check('SELECT m, count(*), min(t), max(k) FROM srcd GROUP BY m');
SELECT source_check('SELECT k, upper(m), length(t), t || ''!'' FROM srcd WHERE k % 1001 = 0');
SELECT source_check('SELECT k, regexp_replace(m, ''m(.)'', ''x\1'') FROM srcd WHERE k % 503 = 0');
SELECT source_check('SELECT t, count(*) FROM srcd WHERE t LIKE ''t99%'' GROUP BY t');
SET vexec.enable_source_layouts = off;
SELECT source_check('SELECT m, count(*), min(t) FROM srcd WHERE m > ''m2'' GROUP BY m');
RESET vexec.enable_source_layouts;
RESET vexec_test.source_text_offsets;
-- not asked for, none is handed: the same dictionaries made as before
SET vexec.batch_dictionary = off;
SELECT calls AS made FROM vexec_test.source_calls() WHERE member = 'dictionaries made' \gset
SELECT source_check('SELECT m, count(*), min(t) FROM srcd GROUP BY m');
SELECT calls = :made AS none_made FROM vexec_test.source_calls() WHERE member = 'dictionaries made';
RESET vexec_test.source_dictionaries;
RESET vexec.batch_dictionary;

-- groups in their keys' order (VC4): where the source says its rows come
-- in an order the keys are a prefix of (ordering(), minor 4) -- so_rows is
-- loaded in (k, t)'s order, k's groups of 2,500 rows across batches, then
-- k's NULLs -- a serial grouping makes each group as its rows come and
-- hands it up as the next begins: no table, no spill, with 64 kB of
-- work_mem too.  Where the scan, as it begins, says its rows do not come so
-- -- as a storage whose units changed since the plan was made would -- it
-- hashes.  The keys' order among themselves is free; a key past the
-- order's prefix keeps the grouping hashed
CREATE TABLE so_rows (k int4, t text, v int8, n numeric(12,2)) USING vexec_test_am;
INSERT INTO so_rows
SELECT i / 2500, 't' || (i % 2500 / 1000), i, i / 3.0 FROM generate_series(0, 49999) i
UNION ALL SELECT NULL, 'tn', i, NULL FROM generate_series(1, 700) i;
ANALYZE so_rows;
SET vexec_test.source_ordering = '1,2';
SET vexec.mode = auto;
SET max_parallel_workers_per_gather = 0;
EXPLAIN (COSTS OFF) SELECT k, count(*), sum(v) FROM so_rows GROUP BY k;
EXPLAIN (COSTS OFF) SELECT t, k, count(*) FROM so_rows GROUP BY t, k;
EXPLAIN (COSTS OFF) SELECT t, count(*) FROM so_rows GROUP BY t;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT k, count(*) FROM so_rows GROUP BY k;
SELECT source_check('SELECT k, count(*), sum(v), min(t), avg(n), string_agg(v::text, '','' ORDER BY v DESC) FROM so_rows GROUP BY k');
SELECT source_check('SELECT k, t, count(*), max(v), array_agg(v ORDER BY v)::text FROM so_rows GROUP BY k, t');
SELECT source_check('SELECT k, count(*) FROM so_rows GROUP BY k HAVING sum(v) > 100000000');
SELECT source_check('SELECT k, sum(v) FROM so_rows WHERE v % 3 = 0 GROUP BY k');
SET work_mem = '64kB';
SELECT source_check('SELECT k, t, count(*), sum(n) FROM so_rows GROUP BY k, t');
SELECT source_check('SELECT v / 7 AS g, count(*) FROM so_rows GROUP BY k, v / 7');
RESET work_mem;
-- the scan says otherwise: hashed, the same answers
SET vexec_test.source_ordered_scans = off;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT k, count(*) FROM so_rows GROUP BY k;
SELECT source_check('SELECT k, count(*), sum(v) FROM so_rows GROUP BY k');
RESET vexec_test.source_ordered_scans;
-- a grouping for each outer row: rescanned, its groups made again
SELECT source_check('SELECT o.k, (SELECT count(*) FROM (SELECT k, t FROM so_rows WHERE so_rows.k = o.k GROUP BY k, t) g) FROM (VALUES (0), (7), (19)) o(k)');
SET vexec.enable_sorted_agg = off;
EXPLAIN (COSTS OFF) SELECT k, count(*) FROM so_rows GROUP BY k;
RESET vexec.enable_sorted_agg;
RESET vexec_test.source_ordering;
RESET max_parallel_workers_per_gather;
RESET vexec.mode;

-- a bounded VecSort's late columns (H6) with a source that names them
-- (fetch_columns(), minor 4, VC16): its fetches by TID read only the
-- columns the sort's rows take, the access method leaving the others NULL
-- -- a column read but not named would show as NULL; and where the source
-- prices a fetch (estimate_ex()'s fetch), a serial sort is made late only
-- where that saves
SET vexec.mode = force;
EXPLAIN (COSTS OFF) SELECT a, b, c, d, f FROM src ORDER BY d DESC, a LIMIT 5;
SELECT source_check('SELECT a, b, c, d, f FROM src ORDER BY d DESC, a LIMIT 5');
SELECT source_check('SELECT u, c, a FROM src WHERE e ORDER BY b DESC LIMIT 7');
SET vexec_test.source_fetch_cost = 1000000;
EXPLAIN (COSTS OFF) SELECT a, b, c, d, f FROM src ORDER BY d DESC, a LIMIT 5;
SELECT source_check('SELECT a, b, c, d, f FROM src ORDER BY d DESC, a LIMIT 5');
RESET vexec_test.source_fetch_cost;
RESET vexec.mode;

-- columns in steps (VC2; next_rest(), minor 5): the first vector qual's
-- columns first, then each later qual's new ones, the rest last -- the TIDs
-- with the first step -- and a step read only for a batch where the quals
-- before it left a row, which the source hands, and no other row of it:
-- every answer as with vexec off, through quals that keep few rows, none,
-- or all, the TIDs, rows a kernel cannot compute, which PostgreSQL's
-- evaluator reads whole, lazy quals and targets, an aggregation, a running
-- bound, dictionaries and text as offsets in a later step; and with
-- vexec.enable_source_steps off, every column at once
SET vexec.mode = force;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT a, b, d, u FROM src WHERE a % 100 = 7 AND c LIKE 'v%7' AND g > 0;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT a, b, c FROM src WHERE a > 9990;
RESET vexec.mode;
SELECT calls AS steps_before FROM vexec_test.source_calls() WHERE member = 'next_rest' \gset
SELECT source_check('SELECT a, b, d, u FROM src WHERE a % 100 = 7 AND c LIKE ''v%7'' AND g > 0');
SELECT source_check('SELECT a, b, c FROM src WHERE a > 9990');
SELECT source_check('SELECT a, b, c FROM src WHERE a > 20000');
SELECT source_check('SELECT a, b, f FROM src WHERE a > 0');
SELECT source_check('SELECT ctid, a, f FROM src WHERE b % 1000 = 7');
SELECT source_check('SELECT a, f, u FROM src WHERE b % 7 = 0 AND (CASE WHEN a % 50 = 0 THEN 0 ELSE 100 / (a % 50) END) > 3');
SELECT source_check('SELECT a, f, u FROM src WHERE a % 10 = 0 AND 100 / (a % 50) > 3');
SELECT source_check('SELECT a, regexp_replace(c, ''v'', ''w''), d FROM src WHERE a % 1000 = 1 AND regexp_replace(c, ''v'', '''') <> ''x''');
SELECT source_check('SELECT d, count(*), sum(f), min(u::text) FROM src WHERE e AND a % 3 = 0 GROUP BY d ORDER BY d LIMIT 5');
SELECT source_check('SELECT a, b, c FROM src WHERE g > 400 ORDER BY b LIMIT 4');
SELECT source_check('SELECT a, b, c FROM src WHERE g > 400 ORDER BY b DESC LIMIT 4');
SELECT calls > :steps_before AS steps_read FROM vexec_test.source_calls() WHERE member = 'next_rest';
SET vexec_test.source_text_offsets = on;
SET vexec_test.source_dictionaries = on;
SET vexec.batch_dictionary = on;
SELECT source_check('SELECT k, m, t FROM srcd WHERE n = 7 AND k % 3 = 1');
SELECT source_check('SELECT m, count(*), min(t) FROM srcd WHERE n < 10 AND k % 2 = 0 GROUP BY m');
SELECT source_check('SELECT k, upper(m), t || ''!'' FROM srcd WHERE k % 1001 = 0');
RESET vexec.batch_dictionary;
RESET vexec_test.source_dictionaries;
RESET vexec_test.source_text_offsets;
SET vexec.enable_source_steps = off;
SET vexec.mode = force;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  SELECT a, b, d, u FROM src WHERE a % 100 = 7 AND c LIKE 'v%7' AND g > 0;
RESET vexec.mode;
SELECT source_check('SELECT a, b, d, u FROM src WHERE a % 100 = 7 AND c LIKE ''v%7'' AND g > 0');
RESET vexec.enable_source_steps;

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

-- which members vexec called: every one but retain and release, which it never asks for,
-- and estimate, which estimate_ex replaces (minor 3)
SELECT member, calls > 0 AS called FROM vexec_test.source_calls();

DROP TABLE src, src_outer, src_copy, src_xml, so_rows;
DROP FUNCTION source_check(text), source_run(text, text, text);
