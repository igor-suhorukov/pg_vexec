-- SPDX-License-Identifier: Apache-2.0
--
-- The batch layer (pg_vector_executor.md §3.4, §5 V0): batches of every
-- type of the layouts' corpus (§6.2) convert between every pair of layouts
-- and back unchanged, out through a slot in both formats, and compacted;
-- and both formats' exports pass the export check.  vexec_test, a module of
-- the tests alone, runs them; vexec_corpus_setup made the corpus.

\i sql/vexec_corpus_setup.sql

-- every column of three batches through every shape and back
SELECT * FROM vexec_test.roundtrip('SELECT * FROM corpus ORDER BY id');

-- the export check in each format: nanoarrow's full validation, vexec's
-- own check of views and of UTF-8, and every value read back
SELECT * FROM vexec_test.export('SELECT * FROM corpus ORDER BY id', 'postgres');
SELECT * FROM vexec_test.export('SELECT * FROM corpus ORDER BY id', 'arrow');

-- without the values Arrow's types cannot mean -- the infinities, time's
-- 24:00:00, timestamps past the Unix epoch's int64 -- the Arrow format
-- exports Arrow's own date32, timestamp[us], time64[us] and month_day_nano
SELECT col, type, layout, arrow, extension FROM vexec_test.export(
	'SELECT d, t, ts, tstz, iv FROM corpus WHERE id % 50 NOT BETWEEN 15 AND 29 ORDER BY id', 'arrow');
SELECT col, type, layout, arrow, extension FROM vexec_test.export(
	'SELECT d, t, ts, tstz, iv FROM corpus WHERE id % 50 NOT BETWEEN 15 AND 29 ORDER BY id', 'postgres');

-- and with the per-structure settings changed one at a time, and with
-- every column dictionary-encoded
SELECT v.varlena, b.bool, t.temporal, n.numeric, d.dict,
       count(*) AS columns, sum(e.nulls) AS nulls, min(e.rows) AS rows
FROM (VALUES ('format'), ('datum'), ('view'), ('offsets')) v(varlena),
     (VALUES ('format'), ('byte'), ('bit')) b(bool),
     (VALUES ('format'), ('postgres'), ('arrow')) t(temporal),
     (VALUES ('format'), ('scaled'), ('varlena')) n(numeric),
     (VALUES (false), (true)) d(dict),
     LATERAL vexec_test.export('SELECT * FROM corpus ORDER BY id', 'arrow',
                               v.varlena, b.bool, t.temporal, n.numeric, d.dict) e
WHERE (v.varlena = 'format')::int + (b.bool = 'format')::int +
      (t.temporal = 'format')::int + (n.numeric = 'format')::int >= 3
GROUP BY 1, 2, 3, 4, 5 ORDER BY 1, 2, 3, 4, 5;

-- the batch's row count at its edges: 1,023, 1,024 and 1,025 rows
SELECT n, (SELECT string_agg(DISTINCT batches::text || ' batches of ' || rows::text || ' rows', '; ')
           FROM vexec_test.roundtrip(format('SELECT * FROM corpus WHERE id <= %s ORDER BY id', n)))
FROM unnest(ARRAY[1, 63, 64, 65, 1023, 1024, 1025, 2048, 2049]) n;
SELECT * FROM vexec_test.roundtrip('SELECT * FROM corpus WHERE false');

-- numeric's scaled form at its edges: 18 and 38 digits, a value of another
-- display scale, NaN and the infinities, and zero of any scale
SELECT v, s, w, vexec_test.scaled(v, s, w)
FROM (VALUES (999999999999999999::numeric, 0, 8), (-999999999999999999, 0, 8),
             (9999999999999999999, 0, 8), (9223372036854775807, 0, 16),
             (99999999999999999999999999999999999999, 0, 16),
             (-99999999999999999999999999999999999999, 0, 16),
             (999999999999999999999999999999999999999, 0, 16),
             (0.00000000000000000000000000000000000001, 38, 16),
             (999999999999999999999999999999999.99999, 5, 16),
             (123.45, 2, 8), (123.4, 2, 8), (123.450, 3, 8), (123.45, 3, 8),
             (0.0001, 4, 8), (0.00010, 5, 8), (-0.5, 1, 8), (0.000, 3, 8), (0, 0, 8),
             (12300, 0, 8), (1e20, 0, 16), ('NaN', 0, 16), ('Infinity', 0, 16),
             ('-Infinity', 0, 16)) x(v, s, w);
-- a scale past the scaled layout's 38 has no scaled form, zero's included
SELECT vexec_test.scaled(round(0::numeric, 2000), 2000) AS zero_at_2000,
       vexec_test.scaled(round(0.5, 39), 39) AS half_at_39;

-- a numeric of another display scale than its column's typmod, as a table
-- access method storing Datums could hold: the scaled layout refuses it,
-- and the column keeps PostgreSQL's
SELECT col, type, layouts, pairs, kept FROM vexec_test.roundtrip(
	'SELECT x FROM (VALUES (1.50), (1.505), (2.5), (NULL)) v(x)',
	ARRAY[(SELECT atttypmod FROM pg_attribute WHERE attrelid = 'corpus'::regclass AND attname = 'n10_2')]);
SELECT col, type, layout, arrow FROM vexec_test.export(
	'SELECT x::numeric(10,2) FROM (VALUES (1.50), (1.505), (NULL)) v(x)', 'arrow');

-- text in a database whose encoding is not UTF8: Arrow's binary, not utf8
SELECT current_database() AS suite_db \gset
CREATE DATABASE vexec_latin1 TEMPLATE template0 ENCODING 'LATIN1' LOCALE 'C';
\c vexec_latin1
SET client_min_messages = warning;
CREATE EXTENSION vexec;
CREATE EXTENSION vexec_test;
RESET client_min_messages;
SELECT col, type, layout, arrow, extension, rows FROM vexec_test.export(
	$$SELECT convert_from('\xe4f6fc'::bytea, 'LATIN1') AS t, 'x'::varchar AS v,
	         '{"a": 1}'::json AS j, repeat('l', 20) AS long$$, 'arrow');
SELECT col, type, layout, arrow, extension, rows FROM vexec_test.export(
	$$SELECT convert_from('\xe4f6fc'::bytea, 'LATIN1') AS t, repeat('l', 20) AS long$$,
	'arrow', 'offsets');
\c :suite_db
DROP DATABASE vexec_latin1;

DROP TABLE corpus;
DROP FUNCTION corpus_null(int4, int4);
