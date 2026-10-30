-- Every type class a table of its own files keeps, through the plain scan:
-- the Arrow type each column is written as, and each value read back as it
-- was written
CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy');
CREATE DOMAIN posint AS int4 CHECK (VALUE > 0);
CREATE TYPE pair AS (x int4, y text);

CREATE TABLE scalars_heap (
	b bool, i2 int2, i4 int4, i8 int8, f4 float4, f8 float8, o oid, c "char",
	d date, t time, ts timestamp, tz timestamptz, tt timetz, iv interval,
	n numeric(20,4), nbig numeric(38,10), nany numeric, nneg numeric(5,-2),
	tx text, vc varchar(10), bc char(5), by bytea, u uuid, j json, jb jsonb,
	nm name, mo money, ip inet, cid cidr, mac macaddr, mac8 macaddr8,
	bt bit(8), vb varbit, lsn pg_lsn, x8 xid8, en mood, dm posint
) USING heap;
INSERT INTO scalars_heap VALUES
 (true, 1, 2, 3, 1.5, 2.5, 7, 'a',
  '2024-02-29', '12:34:56.789', '2024-02-29 12:34:56.789', '2024-02-29 12:34:56.789+03', '12:00:00+05', '1 year 2 mons 3 days 04:05:06.789',
  12345.6789, 1234567890.0123456789, 'NaN', 12300,
  'héllo wörld ✓', 'short', 'ab', '\xdeadbeef', 'a0eebc99-9c0b-4ef8-bb6d-6bb9bd380a11', '{"a": 1}', '{"b": [1, 2]}',
  'pg_name', 12.34, '192.168.1.5/24', '10.0.0.0/8', '08:00:2b:01:02:03', '08:00:2b:01:02:03:04:05',
  B'10101010', B'101', '16/B374D848', '42', 'happy', 42),
 (false, -32768, -2147483648, -9223372036854775808, '-Infinity', 'NaN', 4294967295, 'z',
  '-infinity', '00:00:00', 'infinity', '-infinity', '00:00:00+00', '-1 day',
  -9999999999999999.9999, -9999999999999999999999999999.9999999999, 'Infinity', -9999900,
  '', '', '', '\x', '00000000-0000-0000-0000-000000000000', '[]', 'null',
  '', -1, '::1', '::/0', '00:00:00:00:00:00', '00:00:00:00:00:00:00:00',
  B'00000000', B'', '0/0', '0', 'sad', 1),
 (NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL,
  NULL, NULL, NULL, NULL, NULL, NULL),
 (true, 32767, 2147483647, 9223372036854775807, 3.4e38, 1.7e308, 0, ' ',
  '4713-01-01 BC', '24:00:00', '4713-01-01 00:00:00 BC', '290000-12-31 00:00:00+00', '23:59:59.999999-12', '178000000 years',
  0.0001, 0, 123456789012345678901234567890.123456789, 0,
  repeat('x', 3000), 'tenchars!!', 'abcde', decode(repeat('ff', 1000), 'hex'), 'ffffffff-ffff-ffff-ffff-ffffffffffff', '"str"', '{"deep": {"x": [true, false, null]}}',
  'x', 92233720368547758.07, '2001:db8::1/64', '2001:db8::/32', 'ff:ff:ff:ff:ff:ff', 'ff:ff:ff:ff:ff:ff:ff:ff',
  B'11111111', repeat('1', 100)::varbit, 'FFFFFFFF/FFFFFFFF', '18446744073709551615', 'ok', 2147483647);
CREATE TABLE scalars (LIKE scalars_heap) USING vexec_parquet;
INSERT INTO scalars SELECT * FROM scalars_heap;
-- json has no equality: compared as text
CREATE VIEW scalars_cmp AS SELECT b, i2, i4, i8, f4, f8, o, c, d, t, ts, tz, tt, iv, n, nbig, nany, nneg,
	tx, vc, bc, by, u, j::text AS j, jb, nm, mo, ip, cid, mac, mac8, bt, vb, lsn, x8, en, dm FROM scalars;
CREATE VIEW scalars_heap_cmp AS SELECT b, i2, i4, i8, f4, f8, o, c, d, t, ts, tz, tt, iv, n, nbig, nany, nneg,
	tx, vc, bc, by, u, j::text AS j, jb, nm, mo, ip, cid, mac, mac8, bt, vb, lsn, x8, en, dm FROM scalars_heap;
SELECT count(*) AS differ FROM ((TABLE scalars_cmp EXCEPT ALL TABLE scalars_heap_cmp)
	UNION ALL (TABLE scalars_heap_cmp EXCEPT ALL TABLE scalars_cmp)) x;
SELECT b, i2, i4, i8, f4, f8, o, c, d, t, ts, tz, tt, iv FROM scalars ORDER BY i4 NULLS LAST;
SELECT n, nbig, nany, nneg, left(tx, 20) AS tx, length(tx) AS len, vc, bc || '|' AS bc, length(by) AS bylen, u FROM scalars ORDER BY i4 NULLS LAST;
SELECT j, jb, nm, mo, ip, cid, mac, mac8, bt, length(vb) AS vblen, lsn, x8, en, dm FROM scalars ORDER BY i4 NULLS LAST;
-- the Arrow types: numeric(p,s) as decimal128, uuid and json as Arrow's
-- extension types, a fixed-length type's bytes as fixed_size_binary, every
-- other type's own binary image
SELECT s.name, s.field_id, s.arrow_type, s.nullable
FROM vexec_parquet.files('scalars') f, vexec_parquet.file_schema(f.path) s ORDER BY s.field_id;

CREATE TABLE composites_heap (
	id int4, pt point, bx box, ln line, sg lseg, pa path, pg polygon, ci circle,
	arr int4[], txa text[], r int4range, mr int4multirange, tsv tsvector, pr pair, prs pair[], ens mood[]
) USING heap;
INSERT INTO composites_heap VALUES
 (1, '(1.5,2.5)', '((0,0),(1,1))', '{1,2,3}', '[(0,0),(1,1)]', '((0,0),(1,1),(2,0))', '((0,0),(1,1),(1,0))', '<(0,0),5>',
  '{1,2,NULL}', '{"x","y z",NULL}', '[1,10)', '{[1,3), [5,7)}', 'a fat cat sat', ROW(1, 'one'), ARRAY[ROW(2, 'two')::pair, NULL], '{happy,sad}'),
 (2, '(-1e300,1e300)', '((-1,-1),(1,1))', '{-1,0,0}', '[(1,2),(3,4)]', '[(0,0),(5,5)]', '((0,0))', '<(1,1),0>',
  '{}', '{}', 'empty', '{}', '', ROW(NULL, NULL), '{}', '{}'),
 (3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
CREATE TABLE composites (LIKE composites_heap) USING vexec_parquet;
INSERT INTO composites SELECT * FROM composites_heap;
-- the geometric types have no equality: compared as text
SELECT count(*) AS differ FROM (
	(SELECT id, pt::text, bx::text, ln::text, sg::text, pa::text, pg::text, ci::text, arr, txa, r, mr, tsv, pr, prs, ens FROM composites
	 EXCEPT ALL
	 SELECT id, pt::text, bx::text, ln::text, sg::text, pa::text, pg::text, ci::text, arr, txa, r, mr, tsv, pr, prs, ens FROM composites_heap)
	UNION ALL
	(SELECT id, pt::text, bx::text, ln::text, sg::text, pa::text, pg::text, ci::text, arr, txa, r, mr, tsv, pr, prs, ens FROM composites_heap
	 EXCEPT ALL
	 SELECT id, pt::text, bx::text, ln::text, sg::text, pa::text, pg::text, ci::text, arr, txa, r, mr, tsv, pr, prs, ens FROM composites)) x;
SELECT * FROM composites ORDER BY id;
SELECT s.name, s.arrow_type
FROM vexec_parquet.files('composites') f, vexec_parquet.file_schema(f.path) s ORDER BY s.field_id;

-- numeric(p,s)'s NaN has no decimal: a file with one keeps the column's
-- binary images, another file its decimals
CREATE TABLE nums (n numeric(10,2)) USING vexec_parquet;
INSERT INTO nums VALUES (1.5), (-2.25);
INSERT INTO nums VALUES ('NaN'), (3);
SELECT n FROM nums ORDER BY n;
SELECT f.file_id, s.arrow_type FROM vexec_parquet.files('nums') f, vexec_parquet.file_schema(f.path) s ORDER BY 1;

-- values TOASTed in heap, whole in a file
CREATE TABLE big (id int4, tx text, by bytea) USING vexec_parquet;
INSERT INTO big SELECT i, repeat(md5(i::text), 32768), decode(repeat(md5(i::text), 16384), 'hex') FROM generate_series(1, 3) i;
SELECT id, length(tx), md5(tx) = md5(repeat(md5(id::text), 32768)) AS tx_ok,
	length(by), md5(by) = md5(decode(repeat(md5(id::text), 16384), 'hex')) AS by_ok FROM big ORDER BY id;

-- row groups: the table's row_group_rows
CREATE TABLE groups (id int8, v text) USING vexec_parquet WITH (row_group_rows = 1024);
INSERT INTO groups SELECT i, 'v' || i FROM generate_series(1, 10000) i;
SELECT g.row_group, g.rows FROM vexec_parquet.files('groups') f, vexec_parquet.file_row_groups(f.path) g ORDER BY 1;
SELECT count(*), sum(id), min(v), max(v) FROM groups;

-- every codec
CREATE TABLE codecs (codec text, id int4, v text) USING vexec_parquet;
DO $$
DECLARE c text;
BEGIN
	FOREACH c IN ARRAY ARRAY['zstd', 'lz4', 'snappy', 'gzip', 'brotli', 'none'] LOOP
		EXECUTE format('SET LOCAL vexec_parquet.codec = %L', c);
		EXECUTE format('INSERT INTO codecs SELECT %L, i, repeat(%L, i %% 50) FROM generate_series(1, 2000) i', c, c);
		-- each codec's rows its own file
		EXECUTE 'SELECT 1 FROM codecs LIMIT 1';
	END LOOP;
END $$;
SELECT codec, count(*), sum(id), sum(length(v)) FROM codecs GROUP BY codec ORDER BY codec;
SELECT count(*) AS files FROM vexec_parquet.files('codecs');

-- a timestamp past Arrow's microseconds since 1970 has no file to go into
CREATE TABLE far (ts timestamp) USING vexec_parquet;
INSERT INTO far VALUES ('294270-01-01 00:00:00');
SELECT count(*) FROM far;

-- a table of no columns keeps its rows' count
CREATE TABLE nocols () USING vexec_parquet;
INSERT INTO nocols DEFAULT VALUES;
INSERT INTO nocols SELECT FROM generate_series(1, 9);
SELECT count(*) FROM nocols;
SELECT s.name, s.field_id, s.arrow_type FROM vexec_parquet.files('nocols') f, vexec_parquet.file_schema(f.path) s ORDER BY 1, 2;

-- columns added, dropped and retyped: older files read by field id
CREATE TABLE evolve (a int4, b text, c int4) USING vexec_parquet;
INSERT INTO evolve VALUES (1, 'one', 10), (2, 'two', 20);
ALTER TABLE evolve ADD COLUMN d text DEFAULT 'dflt';
ALTER TABLE evolve ADD COLUMN e int8;
INSERT INTO evolve VALUES (3, 'three', 30, 'given', 300);
ALTER TABLE evolve DROP COLUMN b;
INSERT INTO evolve VALUES (4, 40, NULL, 400);
SELECT * FROM evolve ORDER BY a;
ALTER TABLE evolve ALTER COLUMN c TYPE int8, ALTER COLUMN a TYPE numeric(10,1);
SELECT * FROM evolve ORDER BY a;
SELECT s.name, s.field_id, s.arrow_type FROM vexec_parquet.files('evolve') f, vexec_parquet.file_schema(f.path) s ORDER BY 2;
-- a column renamed: still its attribute's, by field id
ALTER TABLE evolve RENAME COLUMN d TO dd;
SELECT a, dd FROM evolve ORDER BY a;
