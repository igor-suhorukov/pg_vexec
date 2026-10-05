-- SPDX-License-Identifier: Apache-2.0
--
-- The layouts' semantics corpus (pg_vector_executor.md §6.2, "Layouts"): a
-- table of every kind of type vexec holds, with the values at each layout's
-- edges, over 2,600 rows -- three batches, the last short -- with NULLs at
-- the edges of the bitmaps' words and of the batches.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
CREATE EXTENSION IF NOT EXISTS vexec_test;
RESET client_min_messages;

CREATE TABLE corpus (
	id int4,
	b bool, i2 int2, i4 int4, i8 int8, f4 float4, f8 float8,
	o oid, m money, ch "char",
	d date, t time, ts timestamp, tstz timestamptz, iv interval, tz timetz,
	n numeric, n10_2 numeric(10,2), n18_0 numeric(18,0), n38_5 numeric(38,5),
	n5_m2 numeric(5,-2), n3_10 numeric(3,10), n40_2 numeric(40,2),
	tx text, vc varchar(200), bp char(5), bt bytea, js json, jb jsonb,
	u uuid, nm name, mac macaddr, mac8 macaddr8, ip inet, pt point,
	a4 int4[], at text[], k int4, big text
);
-- big's long values are stored out of line, uncompressed
ALTER TABLE corpus ALTER COLUMN big SET STORAGE EXTERNAL;

-- a row's value is NULL where its id is at a word's edge, or every 7th
CREATE FUNCTION corpus_null(id int4, col int4) RETURNS bool
LANGUAGE sql IMMUTABLE AS
$$ SELECT (id - 1) % 64 IN (0, 63) OR (id + col) % 7 = 0 OR id IN (1024, 1025, 2048) $$;

INSERT INTO corpus
SELECT g,
	CASE WHEN corpus_null(g, 1) THEN NULL ELSE g % 3 = 0 END,
	CASE WHEN corpus_null(g, 2) THEN NULL
		 WHEN g % 50 = 1 THEN 32767 WHEN g % 50 = 2 THEN -32768 ELSE (g * 7) % 30000 - 15000 END,
	CASE WHEN corpus_null(g, 3) THEN NULL
		 WHEN g % 50 = 3 THEN 2147483647 WHEN g % 50 = 4 THEN -2147483648 ELSE (g::int8 * 1000003 % 1000000007)::int4 END,
	CASE WHEN corpus_null(g, 4) THEN NULL
		 WHEN g % 50 = 5 THEN 9223372036854775807 WHEN g % 50 = 6 THEN -9223372036854775808
		 ELSE g::int8 * 1000000007 * 97 END,
	CASE WHEN corpus_null(g, 5) THEN NULL
		 WHEN g % 50 = 7 THEN 'NaN' WHEN g % 50 = 8 THEN '-0' WHEN g % 50 = 9 THEN '-Infinity'
		 ELSE g / 7.0 END,
	CASE WHEN corpus_null(g, 6) THEN NULL
		 WHEN g % 50 = 10 THEN 'NaN' WHEN g % 50 = 11 THEN '-0' WHEN g % 50 = 12 THEN 'Infinity'
		 WHEN g % 50 = 13 THEN 1e308 ELSE g / 3.0 END,
	CASE WHEN corpus_null(g, 7) THEN NULL WHEN g % 50 = 14 THEN 4294967295 ELSE g END,
	CASE WHEN corpus_null(g, 8) THEN NULL ELSE (g * 1.25)::money END,
	CASE WHEN corpus_null(g, 9) THEN NULL ELSE chr(33 + g % 90)::"char" END,
	-- dates: the infinities, PostgreSQL's limits
	CASE WHEN corpus_null(g, 10) THEN NULL
		 WHEN g % 50 = 15 THEN 'infinity' WHEN g % 50 = 16 THEN '-infinity'
		 WHEN g % 50 = 17 THEN '4714-11-24 BC' WHEN g % 50 = 18 THEN '5874897-12-31'
		 ELSE '2000-01-01'::date + (g * 37 - 40000) END,
	-- time, 24:00:00 included
	CASE WHEN corpus_null(g, 11) THEN NULL WHEN g % 50 = 19 THEN '24:00:00'
		 ELSE '00:00:00'::time + g * interval '17 seconds' END,
	-- timestamps: the infinities, the limits, and from 294247-01-10 on
	CASE WHEN corpus_null(g, 12) THEN NULL
		 WHEN g % 50 = 20 THEN 'infinity' WHEN g % 50 = 21 THEN '-infinity'
		 WHEN g % 50 = 22 THEN '4714-11-24 00:00:00 BC'
		 WHEN g % 50 = 23 AND g > 2000 THEN '294276-12-31 23:59:59.999999'
		 WHEN g % 50 = 24 AND g > 2000 THEN '294247-01-10 04:00:54.775808'
		 WHEN g % 50 = 25 THEN '294247-01-10 04:00:54.775807'
		 ELSE '2000-01-01'::timestamp + g * interval '1 day 3 hours 7 minutes 11.123456 seconds' END,
	CASE WHEN corpus_null(g, 13) THEN NULL
		 WHEN g % 50 = 20 THEN 'infinity' WHEN g % 50 = 21 THEN '-infinity'
		 WHEN g % 50 = 23 AND g > 2000 THEN '294276-12-30 23:59:59.999999+00'
		 ELSE '1999-12-31 00:00:00+00'::timestamptz - g * interval '13 days 5 hours 0.5 seconds' END,
	-- intervals: the infinities, and time parts on both sides of 292 years in ns
	CASE WHEN corpus_null(g, 14) THEN NULL
		 WHEN g % 50 = 26 THEN 'infinity' WHEN g % 50 = 27 AND g > 2000 THEN '-infinity'
		 WHEN g % 50 = 28 AND g > 1000 THEN '2562048 hours'
		 WHEN g % 50 = 29 THEN '2562047 hours 47 minutes 16.854775 seconds'
		 ELSE make_interval(months => g % 13, days => g % 31, secs => g * 1.000001) END,
	CASE WHEN corpus_null(g, 15) THEN NULL ELSE ('10:11:12.13'::time + g * interval '1 second')::timetz END,
	-- numerics: NaN and the infinities, and each typmod's edges
	CASE WHEN corpus_null(g, 16) THEN NULL
		 WHEN g % 50 = 30 THEN 'NaN' WHEN g % 50 = 31 THEN 'Infinity' WHEN g % 50 = 32 THEN '-Infinity'
		 WHEN g % 50 = 33 THEN 1.000 ELSE g / 7.0::numeric END,
	CASE WHEN corpus_null(g, 17) THEN NULL
		 WHEN g % 50 = 34 THEN 99999999.99 WHEN g % 50 = 35 THEN -99999999.99 WHEN g % 50 = 36 THEN 0
		 ELSE g * 1.07 - 1000 END,
	CASE WHEN corpus_null(g, 18) THEN NULL
		 WHEN g % 50 = 34 THEN 999999999999999999 WHEN g % 50 = 35 THEN -999999999999999999
		 ELSE g::int8 * 123456789 END,
	CASE WHEN corpus_null(g, 19) THEN NULL
		 WHEN g % 50 = 34 THEN 999999999999999999999999999999999.99999
		 WHEN g % 50 = 35 THEN -999999999999999999999999999999999.99999
		 WHEN g % 50 = 36 AND g > 1000 THEN 'NaN'
		 ELSE g * 1234567890123456789.12345 END,
	CASE WHEN corpus_null(g, 20) THEN NULL WHEN g % 50 = 34 THEN 9999900 ELSE g * 100 END,
	CASE WHEN corpus_null(g, 21) THEN NULL ELSE g * 0.0000000001 / 1000 END,
	CASE WHEN corpus_null(g, 22) THEN NULL ELSE g * 1.5 END,
	-- strings: 12 and 13 bytes (a view's inline edge), 126 and 127 (the short
	-- header's), compressed inline, and external
	CASE WHEN corpus_null(g, 23) THEN NULL
		 WHEN g % 50 = 37 THEN repeat('x', 12) WHEN g % 50 = 38 THEN repeat('y', 13)
		 WHEN g % 50 = 39 THEN repeat('z', 126) WHEN g % 50 = 40 THEN repeat('w', 127)
		 WHEN g % 50 = 41 THEN repeat('compressible ', 400)
		 WHEN g % 50 = 42 THEN (SELECT string_agg(md5((g * 1000 + s)::text), '') FROM generate_series(1, 300) s)
		 WHEN g % 50 = 43 THEN '' WHEN g % 50 = 44 THEN 'äöü €'
		 ELSE 'row ' || g END,
	CASE WHEN corpus_null(g, 24) THEN NULL ELSE repeat(chr(97 + g % 26), g % 200) END,
	CASE WHEN corpus_null(g, 25) THEN NULL WHEN g % 50 = 45 THEN '' ELSE substr('abcdefgh', 1 + g % 5, g % 4) END,
	CASE WHEN corpus_null(g, 26) THEN NULL
		 WHEN g % 50 = 46 THEN '\x00ff00'::bytea WHEN g % 50 = 47 THEN decode(repeat('ab', 70), 'hex')
		 ELSE int4send(g) END,
	CASE WHEN corpus_null(g, 27) THEN NULL ELSE ('{"g": ' || g || ', "s": "' || repeat('j', g % 20) || '"}')::json END,
	CASE WHEN corpus_null(g, 28) THEN NULL ELSE ('{"g": ' || g || ', "a": [1, 2, ' || g % 10 || ']}')::jsonb END,
	CASE WHEN corpus_null(g, 29) THEN NULL ELSE md5(g::text)::uuid END,
	CASE WHEN corpus_null(g, 30) THEN NULL ELSE ('name_' || g)::name END,
	CASE WHEN corpus_null(g, 31) THEN NULL ELSE ('08:00:2b:01:02:' || lpad(to_hex(g % 256), 2, '0'))::macaddr END,
	CASE WHEN corpus_null(g, 32) THEN NULL ELSE ('08:00:2b:01:02:03:04:' || lpad(to_hex(g % 256), 2, '0'))::macaddr8 END,
	CASE WHEN corpus_null(g, 33) THEN NULL ELSE ('10.' || g % 256 || '.' || g / 256 % 256 || '.1/24')::inet END,
	CASE WHEN corpus_null(g, 34) THEN NULL ELSE point(g, -g / 3.0) END,
	CASE WHEN corpus_null(g, 35) THEN NULL ELSE ARRAY[g, g + 1, NULL, g % 5] END,
	CASE WHEN corpus_null(g, 36) THEN NULL ELSE ARRAY['a' || g, NULL, repeat('t', g % 30)] END,
	42,
	CASE WHEN corpus_null(g, 37) THEN NULL WHEN g % 10 = 1 THEN repeat('external ' || g, 400)
		 ELSE 'short ' || g END
FROM generate_series(1, 2600) g;

ANALYZE corpus;
