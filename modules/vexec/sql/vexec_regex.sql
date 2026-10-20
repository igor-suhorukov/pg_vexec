-- SPDX-License-Identifier: Apache-2.0
--
-- regexp_replace(text, text, text)'s kernel (expr/k_regex.c, H10 of
-- pg_vector_executor.md §3.14): patterns anchored at both ends, of
-- literals, optional literals, classes and groups, matched by a greedy
-- matcher, against PostgreSQL's engine.  Differential fuzzing: random
-- patterns of those items -- and of items outside them, which keep
-- PostgreSQL's function -- over values made of the same pieces, multibyte
-- characters and newlines among them; every answer compared, off and in
-- force mode.  Values the encoding does not take whole keep PostgreSQL's
-- function.

SET vexec.mode = force;

-- ClickBench's Q28: a kernel, no fallback step, the answers PostgreSQL's
CREATE TABLE rx (id int, v text);
INSERT INTO rx VALUES
  (1, 'https://www.example.com/a/b'), (2, 'http://example.com/'), (3, 'http://www.x/'),
  (4, 'https://example.com'), (5, 'ftp://example.com/x'), (6, ''), (7, 'http:///x'),
  (8, 'https://www./x'), (9, 'http://ünï.cödé/päth'), (10, E'http://a\nb/c\nd'),
  (11, 'https://www.www.x/'), (12, 'HTTP://x/y'), (13, NULL);
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT regexp_replace(v, '^https?://(?:www\.)?([^/]+)/.*$', '\1') FROM rx;
SELECT id, regexp_replace(v, '^https?://(?:www\.)?([^/]+)/.*$', '\1') FROM rx ORDER BY id;
SET vexec.mode = off;
SELECT id, regexp_replace(v, '^https?://(?:www\.)?([^/]+)/.*$', '\1') FROM rx ORDER BY id;
SET vexec.mode = force;

-- the replacement: groups, the match, backslashes, a group past the pattern's
SELECT id, regexp_replace(v, '^(h)t(t)p.*$', E'<\\2\\1\\&\\\\\\q\\9\\') FROM rx ORDER BY id;
SET vexec.mode = off;
SELECT id, regexp_replace(v, '^(h)t(t)p.*$', E'<\\2\\1\\&\\\\\\q\\9\\') FROM rx ORDER BY id;
SET vexec.mode = force;

-- patterns that keep PostgreSQL's function: no $, alternation, a range, a
-- class name, an escape class, a back-reference, a bound, a non-greedy
-- quantifier, options, nested groups
CREATE FUNCTION rx_fallback(p text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r record;
	plan text := '';
BEGIN
	FOR r IN EXECUTE format('EXPLAIN (VERBOSE, COSTS OFF) SELECT regexp_replace(v, %L, %L) FROM rx',
							p, '\1') LOOP
		plan := plan || r."QUERY PLAN" || E'\n';
	END LOOP;
	RETURN CASE WHEN plan ~ 'Kernel Steps: [1-9]' THEN 'kernel' ELSE 'fallback' END;
END $$;
SELECT p, rx_fallback(p) FROM (VALUES ('^https?://(?:www\.)?([^/]+)/.*$'), ('^h(t+)p'), ('^(a|b).*$'),
  ('^[a-z]+.*$'), ('^[[:alpha:]]+.*$'), ('^\w+.*$'), ('^(h)\1.*$'), ('^h{2}.*$'), ('^h.*?$'),
  ('(?i)^h.*$'), ('^((h)t).*$'), ('***=^h.*$'), ('^h.*\$')) v(p);
DROP FUNCTION rx_fallback(text);

-- the fuzzing: patterns of random items, values of random pieces
CREATE TABLE rx_values (id int, v text);
CREATE FUNCTION rx_piece() RETURNS text LANGUAGE sql AS $$
  SELECT (ARRAY['a', 'b', '/', '.', 'w', 'x', ':', 'ab', 'www.', 'é', 'ü/', E'\n', 'http', '//'])
         [1 + floor(random() * 14)::int]
$$;
CREATE FUNCTION rx_atom() RETURNS text LANGUAGE sql AS $$
  SELECT (ARRAY['a', 'b', '\/', '\.', 'w', 'é', ':', '.', '[ab]', '[^/]', '[^a.]', '[]a]', '[a-]', 'x'])
         [1 + floor(random() * 14)::int]
$$;
CREATE FUNCTION rx_item() RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	r float8 := random();
	a text := rx_atom();
BEGIN
	IF r < 0.12 THEN
		RETURN '(?:' || rx_piece_lit() || ')?';
	END IF;
	IF a IN ('.', '[ab]', '[^/]', '[^a.]', '[]a]', '[a-]') OR random() < 0.3 THEN
		RETURN a || (ARRAY['', '?', '*', '+'])[1 + floor(random() * 4)::int];
	END IF;
	RETURN a;
END $$;
CREATE FUNCTION rx_piece_lit() RETURNS text LANGUAGE sql AS $$
  SELECT (ARRAY['www\.', 'a', 'ab', 'é', '\/\/', 'x:'])[1 + floor(random() * 6)::int]
$$;
CREATE FUNCTION rx_pattern() RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	p text := '^';
	n int := 1 + floor(random() * 6)::int;
	g int := 0;
BEGIN
	FOR i IN 1 .. n LOOP
		IF random() < 0.3 AND g < 3 THEN
			p := p || '(' || rx_item() || CASE WHEN random() < 0.5 THEN rx_item() ELSE '' END || ')';
			g := g + 1;
		ELSE
			p := p || rx_item();
		END IF;
	END LOOP;
	-- now and then one that is not anchored at its end, or that alternates
	IF random() < 0.05 THEN
		RETURN p;
	ELSIF random() < 0.03 THEN
		RETURN p || '|a$';
	END IF;
	RETURN p || '$';
END $$;
CREATE FUNCTION rx_fuzz(npatterns int, nvalues int) RETURNS text LANGUAGE plpgsql AS $f$
DECLARE
	pat text;
	rep text;
	off_md5 text;
	on_md5 text;
	plan text;
	r record;
	differ int := 0;
	kernel int := 0;
	matched bigint := 0;
	changed bigint;
BEGIN
	PERFORM setseed(0.42);
	FOR k IN 1 .. npatterns LOOP
		pat := rx_pattern();
		rep := (ARRAY['\1', '[\1]', '\2-\1', '\&', 'x', '\\', '\q', '', '\9', '\3\2\1'])
			   [1 + floor(random() * 10)::int];
		TRUNCATE rx_values;
		INSERT INTO rx_values
			SELECT i, (SELECT string_agg(rx_piece(), '') FROM generate_series(1, floor(random() * 10)::int + i % 2) s)
			FROM generate_series(1, nvalues) i;
		-- values that match the pattern's items, and their neighbours
		INSERT INTO rx_values
			SELECT nvalues + i, regexp_replace(regexp_replace(pat, '^\^|\$$', '', 'g'),
											   '[\\()?*+\[\]^:]', '', 'g') ||
					CASE WHEN i % 3 = 0 THEN rx_piece() ELSE '' END
			FROM generate_series(1, 20) i;
		PERFORM set_config('vexec.mode', 'off', true);
		EXECUTE format('SELECT md5(string_agg(coalesce(regexp_replace(v, %L, %L), ''~''), ''|'' ORDER BY id)), '
					   'count(*) FILTER (WHERE regexp_replace(v, %L, %L) IS DISTINCT FROM v) FROM rx_values',
					   pat, rep, pat, rep)
			INTO off_md5, changed;
		matched := matched + changed;
		PERFORM set_config('vexec.mode', 'force', true);
		EXECUTE format('SELECT md5(string_agg(coalesce(regexp_replace(v, %L, %L), ''~''), ''|'' ORDER BY id)) '
					   'FROM rx_values', pat, rep) INTO on_md5;
		plan := '';
		FOR r IN EXECUTE format('EXPLAIN (VERBOSE, COSTS OFF) SELECT regexp_replace(v, %L, %L) FROM rx_values',
								pat, rep) LOOP
			plan := plan || r."QUERY PLAN" || E'\n';
		END LOOP;
		IF plan ~ 'Kernel Steps: [1-9]' THEN
			kernel := kernel + 1;
		END IF;
		IF off_md5 IS DISTINCT FROM on_md5 THEN
			differ := differ + 1;
			RAISE NOTICE 'differs: pattern %, replacement %', pat, rep;
		END IF;
	END LOOP;
	RETURN format('%s patterns, %s of them by the kernel, %s values changed, %s patterns differ',
				  npatterns, kernel, matched, differ);
END $f$;
SELECT rx_fuzz(400, 200) AS fuzz;

-- patterns built with values that match them, choice by choice, and those
-- values with a character inserted or removed
CREATE TABLE rx_cases (id int, v text);
CREATE FUNCTION rx_case(OUT pat text, OUT vals text[]) LANGUAGE plpgsql AS $f$
DECLARE
	nitems int := 1 + floor(random() * 7)::int;
	ninst int := 12;
	inst text[] := array_fill(''::text, ARRAY[ninst]);
	kind float8;
	lit text;
	cls text;
	members text[];
	neg boolean;
	q text;
	n int;
	grp int := 0;
	ingroup boolean := false;
	pieces text[] := ARRAY['a', 'b', '/', '.', 'w', 'x', ':', 'é', E'\n', ']', '-', 'ü'];
	c text;
BEGIN
	pat := '^';
	FOR it IN 1 .. nitems LOOP
		IF NOT ingroup AND grp < 4 AND random() < 0.25 THEN
			pat := pat || '(';
			ingroup := true;
			grp := grp + 1;
		END IF;
		kind := random();
		IF kind < 0.3 THEN
			-- a literal, maybe optional
			lit := pieces[1 + floor(random() * 12)::int];
			IF random() < 0.3 THEN
				pat := pat || CASE WHEN lit IN ('/', '.', ']', '-', ':') THEN '\' || lit WHEN lit = E'\n' THEN E'\n' ELSE lit END || '?';
				FOR k IN 1 .. ninst LOOP
					IF random() < 0.6 THEN inst[k] := inst[k] || lit; END IF;
				END LOOP;
			ELSE
				pat := pat || CASE WHEN lit IN ('/', '.', ']', '-', ':') THEN '\' || lit ELSE lit END;
				FOR k IN 1 .. ninst LOOP inst[k] := inst[k] || lit; END LOOP;
			END IF;
		ELSIF kind < 0.42 THEN
			lit := (ARRAY['www.', 'ab', 'é', '//', 'x:', 'a'])[1 + floor(random() * 6)::int];
			pat := pat || '(?:' || replace(replace(lit, '.', '\.'), '/', '\/') || ')?';
			FOR k IN 1 .. ninst LOOP
				IF random() < 0.6 THEN inst[k] := inst[k] || lit; END IF;
			END LOOP;
		ELSE
			-- a class with a quantifier
			IF random() < 0.3 THEN
				cls := '.'; members := NULL; neg := false;
			ELSE
				members := string_to_array((ARRAY['a b', '/ x', 'a .', '] a', 'a -', 'w . :'])[1 + floor(random() * 6)::int], ' ');
				neg := random() < 0.5;
				cls := '[' || CASE WHEN neg THEN '^' ELSE '' END ||
					   CASE WHEN members[1] = ']' THEN ']' || array_to_string(members[2:], '') 
							WHEN members[2] = '-' THEN members[1] || '-' ELSE array_to_string(members, '') END || ']';
			END IF;
			q := (ARRAY['', '?', '*', '+'])[1 + floor(random() * 4)::int];
			pat := pat || cls || q;
			FOR k IN 1 .. ninst LOOP
				n := CASE q WHEN '' THEN 1 WHEN '?' THEN floor(random() * 2)::int
						   WHEN '*' THEN floor(random() * 5)::int ELSE 1 + floor(random() * 4)::int END;
				FOR j IN 1 .. n LOOP
					LOOP
						c := pieces[1 + floor(random() * 12)::int];
						EXIT WHEN cls = '.' OR (neg AND NOT c = ANY (members)) OR (NOT neg AND c = ANY (members));
					END LOOP;
					inst[k] := inst[k] || c;
				END LOOP;
			END LOOP;
		END IF;
		IF ingroup AND random() < 0.5 THEN
			pat := pat || ')';
			ingroup := false;
		END IF;
	END LOOP;
	IF ingroup THEN pat := pat || ')'; END IF;
	pat := pat || '$';
	vals := inst;
	-- mutations: a character inserted, removed, changed
	FOR k IN 1 .. ninst LOOP
		n := 1 + floor(random() * greatest(length(inst[k]), 1))::int;
		vals := vals || (substr(inst[k], 1, n - 1) || pieces[1 + floor(random() * 12)::int] || substr(inst[k], n));
		vals := vals || (substr(inst[k], 1, n - 1) || substr(inst[k], n + 1));
	END LOOP;
END $f$;
CREATE FUNCTION rx_fuzz_matching(ncases int, seed float8) RETURNS text LANGUAGE plpgsql AS $f$
DECLARE
	cs record;
	rep text;
	off_md5 text;
	on_md5 text;
	plan text;
	r record;
	kernel int := 0;
	differ int := 0;
	matched bigint := 0;
	changed bigint;
BEGIN
	PERFORM setseed(seed);
	FOR k IN 1 .. ncases LOOP
		SELECT * INTO cs FROM rx_case();
		rep := (ARRAY['\1', '[\1]', '\2-\1', '\&', 'x', '\\', '\q', '', '\9', '\3\2\1', '<\4>'])[1 + floor(random() * 11)::int];
		TRUNCATE rx_cases;
		INSERT INTO rx_cases SELECT i, cs.vals[i] FROM generate_series(1, array_length(cs.vals, 1)) i;
		PERFORM set_config('vexec.mode', 'off', true);
		EXECUTE format('SELECT md5(string_agg(coalesce(regexp_replace(v, %L, %L), ''~''), ''|'' ORDER BY id)), '
					   'count(*) FILTER (WHERE regexp_replace(v, %L, %L) IS DISTINCT FROM v) FROM rx_cases',
					   cs.pat, rep, cs.pat, rep) INTO off_md5, changed;
		matched := matched + changed;
		PERFORM set_config('vexec.mode', 'force', true);
		EXECUTE format('SELECT md5(string_agg(coalesce(regexp_replace(v, %L, %L), ''~''), ''|'' ORDER BY id)) '
					   'FROM rx_cases', cs.pat, rep) INTO on_md5;
		plan := '';
		FOR r IN EXECUTE format('EXPLAIN (VERBOSE, COSTS OFF) SELECT regexp_replace(v, %L, %L) FROM rx_cases',
								cs.pat, rep) LOOP
			plan := plan || r."QUERY PLAN" || E'\n';
		END LOOP;
		IF plan ~ 'Kernel Steps: [1-9]' THEN kernel := kernel + 1; END IF;
		IF off_md5 IS DISTINCT FROM on_md5 THEN
			differ := differ + 1;
			RAISE NOTICE 'differs: pattern % replacement %', cs.pat, rep;
		END IF;
	END LOOP;
	RETURN format('%s cases, %s by the kernel, %s values changed, %s differ', ncases, kernel, matched, differ);
END $f$;
SELECT rx_fuzz_matching(300, 0.42) AS fuzz;
DROP FUNCTION rx_fuzz_matching(int, float8);
DROP FUNCTION rx_case();
DROP TABLE rx_cases;

-- a pattern whose backtracking passes the bound: its rows PostgreSQL's
INSERT INTO rx VALUES (14, repeat('a', 300) || 'b'), (15, repeat('a', 30) || 'c');
SELECT id, regexp_replace(v, '^a*a*a*a*a*c$', 'x') = v AS unchanged FROM rx WHERE id >= 14 ORDER BY id;

-- values the encoding does not take whole, made as PostgreSQL's encoding
-- test makes them: its engine matches their wide characters, which end at
-- a NUL and drop a truncated last character, so the kernel leaves them to
-- PostgreSQL's function and keeps the valid values beside them.  Their
-- answers' bytes, in force mode and off: the kernel's answers made below
-- the row function that shows them
SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec_test CASCADE;
RESET client_min_messages;
CREATE TABLE rx_bad (id int, v text);
INSERT INTO rx_bad VALUES
  (1, 'café'),
  (2, 'caf' || vexec_test.text_of_bytes('\xc3')),
  (3, 'café' || vexec_test.text_of_bytes('\x00') || 'dcba'),
  (4, 'caf' || vexec_test.text_of_bytes('\xc300') || 'dcba'),
  (5, vexec_test.text_of_bytes('\xff') || 'caf'),
  (6, 'caf' || vexec_test.text_of_bytes('\xe282')),
  (7, 'cafx');
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, vexec_test.raw(one) AS one, vexec_test.raw(dcba) AS dcba, vexec_test.raw(rot) AS rot
FROM (SELECT id, regexp_replace(v, '^caf(.)$', '\1') AS one, regexp_replace(v, '^caf(.)dcba$', '\1') AS dcba,
             regexp_replace(v, '^(.)(.*)$', '\2<\1>') AS rot
      FROM rx_bad OFFSET 0) r
ORDER BY id;
SELECT id, vexec_test.raw(one) AS one, vexec_test.raw(dcba) AS dcba, vexec_test.raw(rot) AS rot
FROM (SELECT id, regexp_replace(v, '^caf(.)$', '\1') AS one, regexp_replace(v, '^caf(.)dcba$', '\1') AS dcba,
             regexp_replace(v, '^(.)(.*)$', '\2<\1>') AS rot
      FROM rx_bad OFFSET 0) r
ORDER BY id;
SET vexec.mode = off;
SELECT id, vexec_test.raw(one) AS one, vexec_test.raw(dcba) AS dcba, vexec_test.raw(rot) AS rot
FROM (SELECT id, regexp_replace(v, '^caf(.)$', '\1') AS one, regexp_replace(v, '^caf(.)dcba$', '\1') AS dcba,
             regexp_replace(v, '^(.)(.*)$', '\2<\1>') AS rot
      FROM rx_bad OFFSET 0) r
ORDER BY id;
SET vexec.mode = force;
-- the same in each layout: a batch's values as offsets -- one check where
-- their bytes are whole and cut between characters, else each value's --
-- as views and as Datums, each value's; and in each, a character split
-- between two values, whose bytes together are whole, and a batch of whole
-- values, multibyte ones among them, which the kernel answers whole
CREATE FUNCTION rx_same(q text) RETURNS text LANGUAGE plpgsql AS $f$
DECLARE
	ref text;
	got text;
	lay text;
	res text := '';
BEGIN
	PERFORM set_config('vexec.mode', 'off', true);
	EXECUTE format('SELECT string_agg(x::text, %L ORDER BY x::text) FROM (%s) x', '|', q) INTO ref;
	FOREACH lay IN ARRAY ARRAY['datum', 'view', 'offsets'] LOOP
		PERFORM set_config('vexec.mode', 'force', true);
		PERFORM set_config('vexec.batch_varlena_layout', lay, true);
		EXECUTE format('SELECT string_agg(x::text, %L ORDER BY x::text) FROM (%s) x', '|', q) INTO got;
		res := res || format('%s %s; ', lay, CASE WHEN got IS NOT DISTINCT FROM ref THEN 'same' ELSE 'DIFFERS: ' || got || ' off ' || ref END);
	END LOOP;
	RETURN res;
END $f$;
CREATE TABLE rx_split (id int, v text);
INSERT INTO rx_split VALUES
  (1, 'caf' || vexec_test.text_of_bytes('\xc3')), (2, vexec_test.text_of_bytes('\xa9') || 'caf'), (3, 'café');
CREATE TABLE rx_ok (id int, v text);
INSERT INTO rx_ok VALUES (1, 'café'), (2, 'naïve'), (3, 'http://ünï.cödé/päth'), (4, 'caf€'), (5, 'caf😀'), (6, '');
SELECT t, rx_same(format($q$SELECT id, vexec_test.raw(one), vexec_test.raw(dcba), vexec_test.raw(rot)
  FROM (SELECT id, regexp_replace(v, '^caf(.)$', '\1') AS one, regexp_replace(v, '^caf(.)dcba$', '\1') AS dcba,
               regexp_replace(v, '^(.)(.*)$', '\2<\1>') AS rot FROM %I OFFSET 0) r$q$, t))
FROM unnest(ARRAY['rx_bad', 'rx_split', 'rx_ok']) t;
DROP FUNCTION rx_same(text);
DROP TABLE rx_bad, rx_split, rx_ok;

DROP FUNCTION rx_fuzz(int, int);
DROP FUNCTION rx_pattern();
DROP FUNCTION rx_piece_lit();
DROP FUNCTION rx_item();
DROP FUNCTION rx_atom();
DROP FUNCTION rx_piece();
DROP TABLE rx_values;
DROP TABLE rx;
