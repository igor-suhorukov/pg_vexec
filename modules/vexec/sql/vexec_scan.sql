-- SPDX-License-Identifier: Apache-2.0
--
-- VecScan's semantics (pg_vector_executor.md §3.7, §3.8, §6.2): each query
-- is run with vexec.mode off, and in force mode in the PostgreSQL and the
-- Arrow format, and vexec_check() prints PostgreSQL's answer -- its rows
-- sorted, or its error's SQLSTATE and message -- when every run gives it,
-- and which run differs otherwise.  So the expected output is PostgreSQL's
-- answers, and a vector plan passes only where it gives them.
--
-- The cases: batch edges (1023, 1024 and 1025 rows; filters that keep 63,
-- 64 and 65 rows of a word); NULLs, three-valued logic, IS DISTINCT FROM,
-- ANY and ALL with NULLs; errors inside a batch and at its edge, and none
-- from rows PostgreSQL never evaluates; floats' NaN, -0 and infinities;
-- collations; bpchar; numerics at their layout's edges; dates and
-- timestamps at their limits; LIKE; parameters and InitPlans; rescans;
-- security quals; a volatile function's calls; the system columns.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
RESET client_min_messages;

-- a query's answer in one configuration: its rows as text, sorted -- a
-- long one as its count and md5 -- or its error
CREATE FUNCTION vexec_run(q text, m text, f text) RETURNS text
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

-- PostgreSQL's answer, where force mode in both formats gives it
CREATE FUNCTION vexec_check(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	ref text := vexec_run(q, 'off', 'postgres');
	got text;
	f text;
BEGIN
	FOREACH f IN ARRAY ARRAY['postgres', 'arrow'] LOOP
		got := vexec_run(q, 'force', f);
		IF got IS DISTINCT FROM ref THEN
			RETURN format('DIFFERS in the %s format: off gives [%s], force gives [%s]', f, ref, got);
		END IF;
	END LOOP;
	RETURN ref;
END
$$;

-- the tables: n rows each, every type class
CREATE TABLE vs (
	id int4, i2 int2, i4 int4, i8 int8, f4 float4, f8 float8,
	n numeric(12,3), nb numeric(38,0), nf numeric, t text, v varchar(10), bp char(5),
	d date, ts timestamp, tz timestamptz, b bool, u uuid, o oid, c "char", iv interval
);
INSERT INTO vs
SELECT g, (g % 200 - 100)::int2, g * 37 % 1000 - 500, g::int8 * 1000000007,
	   g / 8.0, g / 16.0, (g % 997) / 7.0, 10::numeric ^ 37 + g, g * 1.5,
	   'text ' || (g % 101), 'v' || g % 17, (g % 5)::text,
	   DATE '2001-01-01' + g, TIMESTAMP '2001-01-01' + g * interval '7 hours',
	   TIMESTAMPTZ '2001-01-01 00:00:00+00' + g * interval '11 hours',
	   g % 3 = 1, md5(g::text)::uuid, g, chr(65 + g % 26)::"char", g * interval '1 minute'
FROM generate_series(1, 3000) g;
UPDATE vs SET i2 = NULL, f8 = NULL, t = NULL, b = NULL WHERE id % 64 IN (0, 1, 63);
UPDATE vs SET i4 = NULL, n = NULL, d = NULL WHERE id % 1024 IN (0, 1023);
ANALYZE vs;

CREATE TABLE edge1023 AS SELECT g AS a FROM generate_series(1, 1023) g;
CREATE TABLE edge1024 AS SELECT g AS a FROM generate_series(1, 1024) g;
CREATE TABLE edge1025 AS SELECT g AS a FROM generate_series(1, 1025) g;

-- batch edges
SELECT vexec_check('SELECT count(*), sum(a), min(a), max(a) FROM edge1023');
SELECT vexec_check('SELECT count(*), sum(a), min(a), max(a) FROM edge1024');
SELECT vexec_check('SELECT count(*), sum(a), min(a), max(a) FROM edge1025');
SELECT vexec_check('SELECT a FROM edge1025 WHERE a >= 1020');
-- filters that keep 63, 64 and 65 rows of the first word
SELECT vexec_check('SELECT count(*), sum(a) FROM edge1025 WHERE a <= 63');
SELECT vexec_check('SELECT count(*), sum(a) FROM edge1025 WHERE a <= 64');
SELECT vexec_check('SELECT count(*), sum(a) FROM edge1025 WHERE a <= 65');
SELECT vexec_check('SELECT count(*) FROM vs WHERE id % 64 = 63');
SELECT vexec_check('SELECT count(*) FROM vs');

-- NULLs to the row parent, three-valued logic, IS DISTINCT FROM
SELECT vexec_check('SELECT id, i2, t FROM vs WHERE id IN (1, 2, 63, 64, 65, 128)');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i2 > 0 AND b');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i2 > 0 OR b');
SELECT vexec_check('SELECT count(*) FROM vs WHERE NOT (i2 > 0 OR b)');
SELECT vexec_check('SELECT count(*) FROM vs WHERE (i2 > 0 AND b) IS NULL');
SELECT vexec_check('SELECT count(*) FROM vs WHERE b IS TRUE');
SELECT vexec_check('SELECT count(*) FROM vs WHERE b IS NOT FALSE');
SELECT vexec_check('SELECT count(*) FROM vs WHERE b IS UNKNOWN');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i2 IS NULL');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i2 IS NOT NULL AND t IS NOT NULL');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i2 IS DISTINCT FROM 5');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i2 IS NOT DISTINCT FROM NULL');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t IS DISTINCT FROM ''text 7''');
SELECT vexec_check('SELECT id, i2 > 0, b AND i2 > 0, b OR i2 > 0 FROM vs WHERE id <= 3 OR id = 64');

-- ANY and ALL, NULL elements, empty arrays
SELECT vexec_check('SELECT count(*) FROM vs WHERE i4 IN (1, 2, 3, NULL)');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i4 NOT IN (1, 2, 3, NULL)');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i4 NOT IN (1, 2, 3)');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i4 = ANY (''{}''::int4[])');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i4 <> ALL (''{}''::int4[])');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i4 > ALL (ARRAY[10, 20])');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i4 < ANY (ARRAY[-100, NULL])');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t IN (''text 1'', ''text 2'')');
SELECT vexec_check('SELECT count(*) FROM vs WHERE i4 = ANY (NULL::int4[])');

-- errors: inside a batch, at its edge, and PostgreSQL's own
SELECT vexec_check('SELECT i2 * i2 * i2 * i2 FROM vs WHERE id < 5');
SELECT vexec_check('SELECT i4 * 1000000 FROM vs');
SELECT vexec_check('SELECT a * 2147483 FROM edge1025 WHERE a > 1000');
SELECT vexec_check('SELECT 1000 / (a - 1024) FROM edge1025');
SELECT vexec_check('SELECT 1000 / (a - 1025) FROM edge1025');
SELECT vexec_check('SELECT (-2147483648)::int4 / (a - 1024 - 1) FROM edge1025 WHERE a = 1024');
SELECT vexec_check('SELECT a % (a - 7) FROM edge1023 WHERE a < 10');
SELECT vexec_check('SELECT (i8 * i8) FROM vs WHERE id = 3000');
SELECT vexec_check('SELECT i2::int4 * 400 FROM vs WHERE id < 4');
SELECT vexec_check('SELECT (i4 * 100)::int2 FROM vs WHERE id < 20');
SELECT vexec_check('SELECT f8 * 1e308 * 10 FROM vs WHERE id = 5');
SELECT vexec_check('SELECT f8 / 0 FROM vs WHERE id = 5');
SELECT vexec_check('SELECT f8 / 0 FROM vs WHERE id = 1');
SELECT vexec_check('SELECT (f8 * 1e-320)::float4 FROM vs WHERE id = 5');
-- and none from rows PostgreSQL never evaluates
SELECT vexec_check('SELECT a FROM edge1025 WHERE a > 1020 AND 1000 / (a - 1) > 0');
SELECT vexec_check('SELECT a FROM edge1025 WHERE a = 1 OR 1000 / (a - 1) > 900');
SELECT vexec_check('SELECT CASE WHEN a = 1 THEN 0 ELSE 1000 / (a - 1) END FROM edge1023 WHERE a < 4');
SELECT vexec_check('SELECT 1000 / (a - 1025) FROM edge1025 LIMIT 3');
SELECT vexec_check('SELECT 1000 / (a - 3) FROM edge1025 WHERE a <> 3');
SELECT vexec_check('SELECT 1000 / i2 FROM vs WHERE i2 IS NULL OR i2 <> 0');
SELECT vexec_check('SELECT 1000 / NULLIF(i2, 0) FROM vs WHERE id < 5');
-- the first error in row order, from the quals before the target list
SELECT vexec_check('SELECT 1 / (a - 5) FROM edge1023 WHERE 1 / (a - 900) > -1');

-- floats: NaN, -0, the infinities
CREATE TABLE vfl (x float8, y float4);
INSERT INTO vfl VALUES ('NaN', 'NaN'), ('-0', '-0'), ('0', '0'), ('Infinity', 'Infinity'),
	('-Infinity', '-Infinity'), (1.5, 1.5), (NULL, NULL);
SELECT vexec_check('SELECT x FROM vfl WHERE x = ''NaN''');
SELECT vexec_check('SELECT x FROM vfl WHERE x > 1e300');
SELECT vexec_check('SELECT x FROM vfl WHERE x < ''NaN''');
SELECT vexec_check('SELECT x FROM vfl WHERE x = 0');
SELECT vexec_check('SELECT x FROM vfl WHERE y >= x');
SELECT vexec_check('SELECT x + 1, x * 2, -x FROM vfl');
SELECT vexec_check('SELECT x - ''Infinity'' FROM vfl WHERE x = ''Infinity''');

-- collations: bytes, varstr_cmp, and a nondeterministic one left to fmgr
CREATE COLLATION vexec_und (provider = icu, locale = 'und');
CREATE COLLATION vexec_ci (provider = icu, locale = 'und-u-ks-level2', deterministic = false);
CREATE TABLE vco (s text);
INSERT INTO vco VALUES ('a'), ('B'), ('b'), ('A'), ('ä'), ('ab'), (''), (NULL), ('Ab');
SELECT vexec_check('SELECT s FROM vco WHERE s < ''b'' COLLATE "C"');
SELECT vexec_check('SELECT s FROM vco WHERE s < ''b'' COLLATE "POSIX"');
SELECT vexec_check('SELECT s FROM vco WHERE s < ''b'' COLLATE vexec_und');
SELECT vexec_check('SELECT s FROM vco WHERE s = ''a'' COLLATE vexec_und');
SELECT vexec_check('SELECT s FROM vco WHERE s = ''a'' COLLATE vexec_ci');
SELECT vexec_check('SELECT s FROM vco WHERE s >= ''A'' COLLATE vexec_ci');
SELECT vexec_check('SELECT s FROM vco WHERE s LIKE ''a%'' COLLATE vexec_und');
SELECT vexec_check('SELECT s FROM vco WHERE s <> ''''');

-- bpchar's trailing blanks
CREATE TABLE vbp (c char(6), v varchar(6));
INSERT INTO vbp VALUES ('ab', 'ab'), ('ab  ', 'ab  '), ('ab c', 'ab c'), ('', ''), (NULL, NULL);
SELECT vexec_check('SELECT c, v FROM vbp WHERE c = ''ab''');
SELECT vexec_check('SELECT c, v FROM vbp WHERE c = ''ab    ''');
SELECT vexec_check('SELECT c, v FROM vbp WHERE v = ''ab''');
SELECT vexec_check('SELECT c, v FROM vbp WHERE c < ''ab c''');
SELECT vexec_check('SELECT c, v FROM vbp WHERE c LIKE ''ab''');
SELECT vexec_check('SELECT c, v FROM vbp WHERE c LIKE ''ab%''');
SELECT vexec_check('SELECT c, v FROM vbp WHERE c LIKE ''%  ''');

-- LIKE: literal, prefix, suffix, substring, the general pattern, escapes
SELECT vexec_check('SELECT count(*) FROM vs WHERE t LIKE ''text 1''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t LIKE ''text 1%''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t LIKE ''%0''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t LIKE ''%xt 9%''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t LIKE ''t_xt _''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t NOT LIKE ''%1%''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t LIKE ''%''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t LIKE ''%%%''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t LIKE ''text\_1'' ESCAPE ''\''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t LIKE ''te#%%'' ESCAPE ''#''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t LIKE ''text 1\''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE t LIKE ''xyz\''');

-- numerics: the scaled layout's edges, and what it cannot hold
CREATE TABLE vnum (n numeric(18,0), m numeric(38,2), k numeric(10,4), f numeric);
INSERT INTO vnum VALUES (999999999999999999, 999999999999999999999999999999999999.99, 123456.7891, 1.5),
	(-999999999999999999, -1, 'NaN', 'NaN'), (0, 0.01, 0, '-Infinity'), (NULL, NULL, NULL, NULL);
SELECT vexec_check('SELECT n, m, k, f FROM vnum WHERE n > 0 OR m < 1');
SELECT vexec_check('SELECT n + 1, m * 2, k - 1 FROM vnum');
SELECT vexec_check('SELECT n * n FROM vnum');
SELECT vexec_check('SELECT count(*) FROM vnum WHERE k = ''NaN'' OR k > 100');
SELECT vexec_check('SELECT count(*) FROM vnum WHERE f > 1');
SELECT vexec_check('SELECT n = 999999999999999999.0, k = 123456.78910 FROM vnum');
SELECT vexec_check('SELECT count(*) FROM vs WHERE n > 100.5 AND n <= 120.25');
SELECT vexec_check('SELECT sum(n * 2), sum(nb - 10::numeric ^ 37), sum(nf + i4) FROM vs');
SELECT vexec_check('SELECT i4::numeric, i8::numeric, i2::numeric FROM vs WHERE id < 3');

-- dates and timestamps: comparisons, their limits, and their fields
CREATE TABLE vdt (d date, ts timestamp);
INSERT INTO vdt VALUES ('infinity', 'infinity'), ('-infinity', '-infinity'),
	('4713-11-24 BC', '4713-11-24 00:00:00 BC'), ('5874897-12-31', '294276-12-31 23:59:59.999999'),
	('294247-01-10', '294247-01-10 04:00:54.775807'), ('2000-01-01', '2000-01-01'),
	('1970-01-01', '1970-01-01'), (NULL, NULL);
SELECT vexec_check('SELECT d, ts FROM vdt WHERE d > ''2000-01-01''');
SELECT vexec_check('SELECT d, ts FROM vdt WHERE ts < ''1999-12-31''');
SELECT vexec_check('SELECT d FROM vdt WHERE d < ts');
SELECT vexec_check('SELECT d FROM vdt WHERE d = ''2000-01-01''::timestamp');
SELECT vexec_check('SELECT d::timestamp FROM vdt');
SELECT vexec_check('SELECT ts::date FROM vdt');
SELECT vexec_check('SELECT extract(year FROM d), extract(dow FROM d), extract(julian FROM d) FROM vdt');
SELECT vexec_check('SELECT extract(year FROM ts), extract(second FROM ts), extract(epoch FROM ts) FROM vdt');
SELECT vexec_check('SELECT date_part(''month'', ts), date_part(''epoch'', ts), date_part(''decade'', ts) FROM vdt');
SELECT vexec_check('SELECT extract(hour FROM d) FROM vdt');
SELECT vexec_check('SELECT count(*) FROM vs WHERE d BETWEEN ''2002-01-01'' AND ''2003-01-01'' AND ts > ''2001-06-01''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE tz < ''2002-01-01 00:00:00+00''');
SELECT vexec_check('SELECT extract(month FROM d), extract(doy FROM ts), extract(isodow FROM ts) FROM vs WHERE id IN (1, 500, 1024, 2999)');

-- other types: bool, uuid, oid, "char", interval
SELECT vexec_check('SELECT count(*) FROM vs WHERE b = true AND c > ''M''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE u > ''80000000-0000-0000-0000-000000000000''');
SELECT vexec_check('SELECT count(*) FROM vs WHERE o < 100 OR o >= 2990');
SELECT vexec_check('SELECT iv, b, u, c FROM vs WHERE id IN (1, 2, 63)');

-- InitPlans, and an InitPlan no row reaches
SELECT vexec_check('SELECT count(*) FROM vs WHERE i4 > (SELECT avg(i4) FROM vs)');
SELECT vexec_check('SELECT count(*) FROM vs WHERE id < 0 AND i4 > (SELECT 1 / (count(*) - count(*))::int4 FROM edge1023)');
SELECT vexec_check('SELECT count(*) FROM vs WHERE id < 3 AND i4 > (SELECT 1 / (count(*) - count(*))::int4 FROM edge1023)');
SELECT vexec_check('SELECT id, (SELECT count(*) FROM edge1023 WHERE a < vs.id) FROM vs WHERE id < 4');

-- rescans: a lateral scan, rescanned for each outer row
SELECT vexec_check('SELECT e.a, s.n FROM edge1023 e, LATERAL (SELECT count(*) AS n FROM vs WHERE vs.id < e.a) s WHERE e.a < 5');
SELECT vexec_check('SELECT e.a FROM edge1023 e WHERE EXISTS (SELECT 1 FROM vs WHERE vs.id = e.a * 1000)');

-- the system columns and a whole row
SELECT vexec_check('SELECT ctid, tableoid::regclass FROM edge1023 WHERE a IN (1, 1023)');
SELECT vexec_check('SELECT e FROM edge1023 e WHERE a < 3');
SELECT vexec_check('SELECT row_to_json(r) FROM vbp r WHERE c IS NOT NULL');

-- a volatile function: every call PostgreSQL makes, in its order, each of
-- the three runs making as many: here the planner puts the cheap qual first
CREATE SEQUENCE vseq;
SELECT vexec_check('SELECT count(*) FROM edge1023 WHERE nextval(''vseq'') > 0 AND a < 10');
SELECT nextval('vseq') - 1 AS calls;
-- and here a dear one second, so that nextval() is called for every row
CREATE FUNCTION vexec_dear(int4) RETURNS bool LANGUAGE plpgsql IMMUTABLE COST 1000 AS
$$ BEGIN RETURN $1 < 10; END $$;
SELECT vexec_check('SELECT count(*) FROM edge1023 WHERE nextval(''vseq'') > 0 AND vexec_dear(a)');
SELECT nextval('vseq') - 28 - 1 AS calls;

-- security: a qual that would fail on a hidden row never sees it
CREATE TABLE vsec (a int4, secret int4);
INSERT INTO vsec SELECT g, g % 4 FROM generate_series(1, 2000) g;
CREATE FUNCTION vexec_leak(int4) RETURNS bool LANGUAGE plpgsql COST 0.0000001 AS
$$ BEGIN IF $1 = 0 THEN RAISE EXCEPTION 'saw a hidden row'; END IF; RETURN true; END $$;
CREATE VIEW vsec_view WITH (security_barrier) AS SELECT * FROM vsec WHERE secret > 0;
SELECT vexec_check('SELECT count(*) FROM vsec_view WHERE vexec_leak(secret)');
ALTER TABLE vsec ENABLE ROW LEVEL SECURITY;
ALTER TABLE vsec FORCE ROW LEVEL SECURITY;
CREATE POLICY vsec_p ON vsec USING (secret > 0);
CREATE ROLE regress_vexec_rls;
GRANT SELECT ON vsec TO regress_vexec_rls;
GRANT EXECUTE ON FUNCTION vexec_check(text), vexec_run(text, text, text), vexec_leak(int4) TO regress_vexec_rls;
SET ROLE regress_vexec_rls;
SELECT vexec_check('SELECT count(*) FROM vsec WHERE vexec_leak(secret)');
RESET ROLE;

-- long and compressed values, and the functions that read their stored form
CREATE TABLE vlong (id int4, s text);
INSERT INTO vlong SELECT g, repeat(md5(g::text), 2000) FROM generate_series(1, 30) g;
INSERT INTO vlong SELECT g, repeat('x', g) FROM generate_series(31, 40) g;
SELECT vexec_check('SELECT id, length(s), pg_column_compression(s) IS NOT NULL FROM vlong WHERE s LIKE ''%a%'' OR id > 35');
SELECT vexec_check('SELECT count(*) FROM vlong WHERE s = repeat(md5(''7''), 2000)');

-- dropped columns, and one added with a default
ALTER TABLE vbp ADD COLUMN w int4 DEFAULT 7;
ALTER TABLE vbp DROP COLUMN v;
SELECT vexec_check('SELECT c, w FROM vbp WHERE w = 7');

-- a kernel's failure on a row the quals removed is no row of PostgreSQL's
-- evaluator: EXPLAIN ANALYZE counts none sent to it
CREATE FUNCTION vexec_sent(q text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
	r text := 'none sent';
BEGIN
	PERFORM set_config('vexec.mode', 'force', true);
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP
		IF l LIKE '%Rows Sent to PostgreSQL%' THEN
			r := btrim(l);
		END IF;
	END LOOP;
	RETURN r;
END
$$;
SELECT vexec_check('SELECT id, i2 + 32700::int2 FROM vs WHERE i2 < 50 AND id < 400');
SELECT vexec_sent('SELECT id, i2 + 32700::int2 FROM vs WHERE i2 < 50 AND id < 400');
SELECT vexec_sent('SELECT id FROM vs WHERE i2 < 50 AND 1000 / (i2 - 70) > -100 AND id < 400');

DROP POLICY vsec_p ON vsec;
REVOKE ALL ON vsec FROM regress_vexec_rls;
REVOKE ALL ON FUNCTION vexec_check(text), vexec_run(text, text, text), vexec_leak(int4) FROM regress_vexec_rls;
DROP ROLE regress_vexec_rls;
DROP VIEW vsec_view;
DROP TABLE vs, edge1023, edge1024, edge1025, vfl, vco, vbp, vnum, vdt, vsec, vlong;
DROP COLLATION vexec_und;
DROP COLLATION vexec_ci;
DROP SEQUENCE vseq;
DROP FUNCTION vexec_leak(int4);
DROP FUNCTION vexec_dear(int4);
DROP FUNCTION vexec_sent(text);
DROP FUNCTION vexec_check(text);
DROP FUNCTION vexec_run(text, text, text);
