-- SPDX-License-Identifier: Apache-2.0
--
-- Kernel packs (pg_vector_executor.md §3.17, VK).  A call of another
-- extension's function that a pack declares binds to the declaration -- by
-- the extension, its version, the function's name and signature, and its C
-- symbol -- and runs a batch's rows at a time, so that a qual or a target
-- over it compiles eager, where it was lazy, row by row, before: the
-- function itself through fmgr where it never raises or where its pack's
-- check passes, the prefilter's answer where it decides.  The other rows go
-- to PostgreSQL's evaluator, which raises the function's error where
-- PostgreSQL would.
--
-- vexec_testpack, a pack of pg_accel's tests preloaded before vexec,
-- declares functions of its own extension (modules/vexec_testpack): tp_mix
-- never raises; tp_div and tp_bytes have checks; tp_le has a prefilter;
-- tp_alias's declaration names a C symbol it is not created with.  Each
-- query's answer is PostgreSQL's in force mode in both formats
-- (vexec_check), errors included; EXPLAIN VERBOSE names the declared calls.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
CREATE EXTENSION vexec_testpack;
CREATE EXTENSION IF NOT EXISTS dblink;
RESET client_min_messages;
SET search_path = public, vexec_testpack;

-- the packs registered in this server
SELECT pack, extension, versions, declarations FROM vexec.kernel_packs();

-- what binds here: not tp_alias, created under a symbol of its own, nor
-- tp_mix(int4), whose signature no declaration has, nor tp_lazy
SELECT function, extension, version, pack, declaration
FROM vexec.declared_calls() ORDER BY function;

-- a query's answer in one configuration: its rows as text, sorted -- a
-- long one as its count and md5 -- or its error
CREATE OR REPLACE FUNCTION vexec_run(q text, m text, f text) RETURNS text
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
CREATE OR REPLACE FUNCTION vexec_check(q text) RETURNS text
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

-- how force mode's vector node runs a query: its kind, its quals and
-- targets eager or row by row, its steps, its declared calls; with ANALYZE,
-- the rows sent to PostgreSQL's evaluator and those declared calls answered
CREATE FUNCTION vexec_how(q text, with_analyze bool DEFAULT false) RETURNS SETOF text
LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	PERFORM set_config('vexec.mode', 'force', true);
	FOR l IN EXECUTE format('EXPLAIN (VERBOSE, COSTS OFF%s) %s',
							CASE WHEN with_analyze THEN ', ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF' ELSE '' END, q) LOOP
		IF l ~ '^\s*(->\s+)?Vec ' OR
		   l ~ '(Vector Quals|Row-by-Row|Kernel Steps|Fallback Steps|Declared Calls|Rows Sent to PostgreSQL|Rows Through Declared Calls|Input:)' THEN
			RETURN NEXT regexp_replace(l, '^\s+(->\s+)?', '');
		END IF;
	END LOOP;
END
$$;

-- 3000 rows over three batches: NULLs; b of 0, of -1 under a of INT64_MIN,
-- and negative, where tp_div and tp_le raise; empty bytea values, where
-- tp_bytes raises, and values stored out of line
CREATE TABLE tp (id int8, a int8, b int8, x bytea);
ALTER TABLE tp ALTER COLUMN x SET STORAGE EXTERNAL;
INSERT INTO tp
SELECT g,
	   CASE WHEN g % 1000 = 500 THEN -9223372036854775808 ELSE g * 7919 % 20011 - 10005 END,
	   CASE WHEN g % 97 = 0 THEN 0 WHEN g % 1000 = 500 THEN -1 WHEN g % 211 = 3 THEN -g
			ELSE g * 104729 % 9973 END,
	   CASE WHEN g % 503 = 7 THEN ''::bytea WHEN g % 100 = 1 THEN convert_to(repeat(chr(65 + g % 26), 3000), 'UTF8')
			ELSE convert_to('v' || g, 'UTF8') END
FROM generate_series(1, 3000) g;
UPDATE tp SET a = NULL WHERE id % 64 = 0;
UPDATE tp SET b = NULL WHERE id % 64 = 1;
UPDATE tp SET x = NULL WHERE id % 64 = 2;
ANALYZE tp;
SELECT count(*) AS out_of_line FROM tp WHERE pg_column_toast_chunk_id(x) IS NOT NULL;

-- never raises: eager, and a batch at a time
SELECT * FROM vexec_how('SELECT id, tp_mix(a) FROM tp WHERE tp_mix(a) % 5 = 0');
SELECT vexec_check('SELECT id, tp_mix(a) FROM tp WHERE tp_mix(a) % 5 = 0');

-- a check: the function on the rows it passes ...
SELECT * FROM vexec_how('SELECT id, tp_div(a, b) FROM tp WHERE b > 0 AND tp_div(a, b) > 2');
SELECT vexec_check('SELECT id, tp_div(a, b) FROM tp WHERE b > 0 AND tp_div(a, b) > 2');
SELECT vexec_check('SELECT id, a #/# b FROM tp WHERE b <> 0 AND id % 1000 <> 500');
-- ... and PostgreSQL's evaluator on the others, which raises its error
-- where PostgreSQL does: in row order, at the first row it reaches
SELECT vexec_check('SELECT count(*) FROM tp WHERE tp_div(a, b) > 0');
SELECT vexec_check('SELECT count(*) FROM tp WHERE b <> 0 AND tp_div(a, b) > 0');
SELECT vexec_check('SELECT id, tp_div(a, b) FROM tp ORDER BY id');
-- and nowhere PostgreSQL does not: rows a qual before it removed, rows past
-- a LIMIT, NULLs of a strict function
SELECT vexec_check('SELECT id FROM tp WHERE id < 90 AND tp_div(a, b) > 0');
SELECT vexec_check('SELECT id, tp_div(a, b) FROM tp WHERE id < 200 LIMIT 90');
SELECT vexec_check('SELECT count(*) FROM tp WHERE tp_div(a, b) IS NULL AND b IS NULL');
-- a lazy qual PostgreSQL evaluates before the call -- tp_div's cost puts it
-- last -- rejects the rows the check failed, which reach PostgreSQL's
-- evaluator, and raise nothing there
ALTER FUNCTION tp_div(int8, int8) COST 1000;
SELECT * FROM vexec_how('SELECT id FROM tp WHERE id % 1000 <> 500 AND tp_lazy(b * b) > 1 AND tp_div(a, b) > 0', true);
SELECT vexec_check('SELECT id FROM tp WHERE id % 1000 <> 500 AND tp_lazy(b * b) > 1 AND tp_div(a, b) > 0');
ALTER FUNCTION tp_div(int8, int8) COST 1;

-- a check's varlena arguments are detoasted before it: of tp_bytes's rows,
-- those stored out of line and those whose short headers the heap page
-- holds, none goes to PostgreSQL's evaluator -- tp_bytes's check fails a
-- value handed to it toasted
SELECT vexec_check('SELECT id, tp_bytes(x) FROM tp WHERE x <> ''''::bytea');
SELECT * FROM vexec_how('SELECT id, tp_bytes(x) FROM tp WHERE x <> ''''::bytea', true);
SELECT vexec_check('SELECT sum(tp_bytes(x)) FROM tp');

-- a prefilter: its answers where it decides, PostgreSQL's evaluator
-- elsewhere, which raises on a negative bound in row order
SELECT * FROM vexec_how('SELECT id FROM tp WHERE b >= 0 AND tp_le(a, b)', true);
SELECT vexec_check('SELECT id FROM tp WHERE b >= 0 AND tp_le(a, b)');
SELECT vexec_check('SELECT id, a #<=# b FROM tp WHERE b >= 0');
SELECT vexec_check('SELECT count(*) FROM tp WHERE tp_le(a, b)');
SELECT vexec_check('SELECT id FROM tp WHERE id < 3 AND tp_le(a, b)');

-- calls of several declarations in one node, each named once
SELECT * FROM vexec_how('SELECT tp_mix(a), tp_div(a, b), tp_mix(b) FROM tp WHERE b > 0 AND tp_le(a, b)');
SELECT vexec_check('SELECT tp_mix(a), tp_div(a, b), tp_mix(b) FROM tp WHERE b > 0 AND tp_le(a, b)');

-- declared calls over batches: a vector sort's keys, an aggregate's
-- arguments, each read from the scan's batches
SELECT * FROM vexec_how('SELECT id FROM tp WHERE b > 0 ORDER BY tp_div(a, b), id LIMIT 5');
SELECT vexec_check('SELECT id FROM tp WHERE b > 0 ORDER BY tp_div(a, b), id LIMIT 5');
SELECT * FROM vexec_how('SELECT b % 7, sum(tp_mix(a) % 1000), count(tp_div(a, b)) FROM tp WHERE b > 0 GROUP BY 1');
SELECT vexec_check('SELECT b % 7, sum(tp_mix(a) % 1000), count(tp_div(a, b)) FROM tp WHERE b > 0 GROUP BY 1');

-- a call that never raises may run in PostgreSQL's evaluator ahead of its
-- order too, in a fallback step; one with a check may not
SELECT * FROM vexec_how('SELECT CASE WHEN a > 0 THEN tp_mix(a) ELSE 0 END FROM tp');
SELECT * FROM vexec_how('SELECT CASE WHEN b > 0 THEN tp_div(a, b) ELSE 0 END FROM tp');
SELECT vexec_check('SELECT CASE WHEN b > 0 THEN tp_div(a, b) ELSE 0 END FROM tp');

-- what binds to nothing stays row by row: a function declared nowhere, a
-- declaration whose C symbol is another, an overload no declaration has
SELECT * FROM vexec_how('SELECT id FROM tp WHERE tp_lazy(a) > 100');
SELECT vexec_check('SELECT id FROM tp WHERE tp_lazy(a) > 100');
SELECT * FROM vexec_how('SELECT tp_alias(a), tp_mix(id::int4) FROM tp');
SELECT vexec_check('SELECT tp_alias(a) = tp_mix(a), tp_mix(id::int4) FROM tp');

-- a function called row by row is called in the row's memory, as
-- PostgreSQL's evaluator calls it, never in its own fn_mcxt, the query's:
-- in a lazy qual, a lazy target, and an aggregate's lazy argument
SELECT vexec_check('SELECT id FROM tp WHERE tp_context(id) % 7 = 0 AND tp_context(a) IS NOT NULL');
SELECT vexec_check('SELECT tp_context(a) FROM tp');
SELECT vexec_check('SELECT b % 5, sum(tp_context(a)) FROM tp GROUP BY 1');

-- the oracle counts a declared call as it will run
SET vexec.mode = explain;
SET vexec.min_rows = 0;
EXPLAIN (VEXEC, COSTS OFF) SELECT id, tp_mix(a) FROM tp WHERE tp_div(a, b) > 0 AND b > 0;
RESET vexec.min_rows;
RESET vexec.mode;

-- a declaration holds only for vexec's own evaluation: tp_mix is no more
-- leakproof than it was
SELECT proleakproof FROM pg_proc WHERE oid = 'tp_mix(int8)'::regprocedure;

-- a function fmgr calls in a way of its own binds to nothing: security
-- definer, with settings of its own; altered back, it binds again
ALTER FUNCTION tp_mix(int8) SECURITY DEFINER;
SELECT function FROM vexec.declared_calls() ORDER BY 1;
SELECT * FROM vexec_how('SELECT tp_mix(a) FROM tp');
ALTER FUNCTION tp_mix(int8) SECURITY INVOKER;
ALTER FUNCTION tp_mix(int8) SET work_mem = '4MB';
SELECT function FROM vexec.declared_calls() ORDER BY 1;
ALTER FUNCTION tp_mix(int8) RESET ALL;
SELECT function FROM vexec.declared_calls() ORDER BY 1;

-- another version of the extension, which the pack does not name, binds
-- nothing -- updated by another session, in this one too -- and gives the
-- same answers; back at the pack's version, the calls bind again
SELECT * FROM vexec_how('SELECT id, tp_div(a, b) FROM tp WHERE b > 0 AND tp_le(a, b)');
SELECT dblink_exec(format('host=%s port=%s dbname=%s',
						  current_setting('unix_socket_directories'), current_setting('port'),
						  current_database()),
				   'ALTER EXTENSION vexec_testpack UPDATE TO ''1.1''');
SELECT extversion FROM pg_extension WHERE extname = 'vexec_testpack';
SELECT count(*) AS bound FROM vexec.declared_calls();
SELECT * FROM vexec_how('SELECT id, tp_div(a, b) FROM tp WHERE b > 0 AND tp_le(a, b)');
SELECT vexec_check('SELECT id, tp_div(a, b) FROM tp WHERE b > 0 AND tp_le(a, b)');
SELECT vexec_check('SELECT count(*) FROM tp WHERE tp_div(a, b) > 0');
ALTER EXTENSION vexec_testpack UPDATE TO '1.0';
SELECT count(*) AS bound FROM vexec.declared_calls();
SELECT * FROM vexec_how('SELECT id, tp_div(a, b) FROM tp WHERE b > 0 AND tp_le(a, b)');

-- dropped and made again, the extension's functions have new OIDs, which
-- bind as the old ones did
DROP EXTENSION vexec_testpack CASCADE;
SELECT count(*) AS bound FROM vexec.declared_calls();
CREATE EXTENSION vexec_testpack;
SELECT function, declaration FROM vexec.declared_calls() ORDER BY 1;

-- a function of the same name, signature and symbol that is not the
-- extension's member binds to nothing.  Made a member, it is the
-- extension's C function, and binds -- in a session that binds anew: ADD
-- FUNCTION changes pg_depend alone, which no syscache callback hears of, so
-- a session keeps what it found until pg_proc or pg_extension changes
CREATE FUNCTION public.tp_div(int8, int8) RETURNS int8
AS '$libdir/vexec_testpack', 'tp_div' LANGUAGE C IMMUTABLE STRICT;
SELECT function FROM vexec.declared_calls() WHERE function LIKE '%tp_div%' ORDER BY 1;
ALTER EXTENSION vexec_testpack ADD FUNCTION public.tp_div(int8, int8);
SELECT function FROM vexec.declared_calls() WHERE function LIKE '%tp_div%' ORDER BY 1;
SELECT f FROM dblink(format('host=%s port=%s dbname=%s',
							current_setting('unix_socket_directories'), current_setting('port'),
							current_database()),
					 $$SELECT function FROM vexec.declared_calls() WHERE function LIKE '%tp_div%'$$) AS t(f text)
ORDER BY 1;
ALTER EXTENSION vexec_testpack DROP FUNCTION public.tp_div(int8, int8);
DROP FUNCTION public.tp_div(int8, int8);

RESET vexec.mode;
DROP TABLE tp;
DROP FUNCTION vexec_how(text, bool);
DROP EXTENSION vexec_testpack;
