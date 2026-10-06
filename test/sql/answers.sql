-- SPDX-License-Identifier: Apache-2.0
--
-- Same answers with or without the pack (pg_vector_executor.md §3.17, VK's
-- "done when"): each query over pgvector's distances gives PostgreSQL's
-- answer -- its rows, or its error -- in vexec's force mode, in the
-- PostgreSQL and the Arrow format, as vexec_check() compares.  test/run.sh
-- runs this file on a server with vexec_pgvector preloaded, where the
-- distances are declared calls, and on one without it, where they are
-- calls row by row: its expected output is the same, PostgreSQL's.
--
-- The cases: a nearest-neighbour search without an index and with a
-- filter; each distance in a qual, a target, a sort key and an aggregate;
-- halfvec's; vectors stored out of line; zero vectors under cosine
-- distance; vectors of other dimensions, which raise pgvector's error in
-- row order, and nowhere a qual before has removed them or a LIMIT has
-- stopped short of them; batch edges.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
CREATE EXTENSION IF NOT EXISTS vector;
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

-- 6000 rows of 16 dimensions, a category each; every 500th a zero vector
SELECT setseed(0.42);
CREATE TABLE items (id int, category int, embedding vector(16), half halfvec(16));
INSERT INTO items
SELECT g, g % 10,
	   CASE WHEN g % 500 = 0 THEN array_fill(0::real, ARRAY[16])
			ELSE (SELECT array_agg((random() * 2 - 1)::real) FROM generate_series(1, 16) WHERE g > 0) END::vector,
	   NULL
FROM generate_series(1, 6000) g;
UPDATE items SET half = embedding::halfvec;
UPDATE items SET embedding = NULL, half = NULL WHERE id % 777 = 0;
ANALYZE items;

-- 1025 rows of 1536 dimensions, stored out of line
CREATE TABLE wide (id int, e vector(1536));
INSERT INTO wide
SELECT g, (SELECT array_agg(sin(g * 0.37 + i)::real) FROM generate_series(1, 1536) i)::vector
FROM generate_series(1, 1025) g;
ANALYZE wide;
SELECT count(*) FILTER (WHERE pg_column_toast_chunk_id(e) IS NOT NULL) AS out_of_line FROM wide;

-- 3000 rows of 3 dimensions but every 1000th, of 2
CREATE TABLE mixed (id int, e vector);
INSERT INTO mixed
SELECT g, CASE WHEN g % 1000 = 0 THEN ARRAY[g, 1]::real[] ELSE ARRAY[g % 7, g % 11, g % 13]::real[] END::vector
FROM generate_series(1, 3000) g;
ANALYZE mixed;

-- the query vectors
CREATE TABLE q AS
SELECT (SELECT array_agg(cos(i)::real) FROM generate_series(1, 16) i)::vector AS v16,
	   (SELECT array_agg(cos(i)::real) FROM generate_series(1, 16) i)::halfvec AS h16,
	   (SELECT array_agg(cos(i * 0.5)::real) FROM generate_series(1, 1536) i)::vector AS v1536;

-- a nearest-neighbour search without an index, with a filter and without
SELECT vexec_check(format('SELECT id FROM items WHERE category = 3 ORDER BY embedding <-> %L, id LIMIT 10', v16)) FROM q;
SELECT vexec_check(format('SELECT id, embedding <-> %L AS d FROM items ORDER BY d, id LIMIT 10', v16)) FROM q;
SELECT vexec_check(format('SELECT id FROM items WHERE category IN (1, 2) ORDER BY embedding <=> %L, id LIMIT 7', v16)) FROM q;
SELECT vexec_check(format('SELECT id FROM items ORDER BY embedding <#> %L, id LIMIT 5', v16)) FROM q;
SELECT vexec_check(format('SELECT id FROM items ORDER BY embedding <+> %L, id LIMIT 5', v16)) FROM q;

-- every distance of vector, in quals and targets
SELECT vexec_check(format('SELECT count(*) FROM items WHERE l2_distance(embedding, %L) < 3', v16)) FROM q;
SELECT vexec_check(format('SELECT id, inner_product(embedding, %L), vector_negative_inner_product(embedding, %L) FROM items WHERE id %% 13 = 0', v16, v16)) FROM q;
SELECT vexec_check(format('SELECT id, cosine_distance(embedding, %L), l1_distance(embedding, %L) FROM items WHERE id %% 11 = 0', v16, v16)) FROM q;
SELECT vexec_check(format('SELECT id, vector_l2_squared_distance(embedding, %L), vector_spherical_distance(embedding, %L) FROM items WHERE id %% 17 = 0', v16, v16)) FROM q;
SELECT vexec_check(format('SELECT category, count(*), sum(embedding <-> %L), min(embedding <=> %L) FROM items GROUP BY category', v16, v16)) FROM q;
-- zero vectors under cosine distance: NaN
SELECT vexec_check(format('SELECT id, embedding <=> %L FROM items WHERE id %% 500 = 0', v16)) FROM q;

-- every distance of halfvec
SELECT vexec_check(format('SELECT id FROM items WHERE category = 5 ORDER BY half <-> %L, id LIMIT 10', h16)) FROM q;
SELECT vexec_check(format('SELECT id, l2_distance(half, %L), inner_product(half, %L), cosine_distance(half, %L), l1_distance(half, %L) FROM items WHERE id %% 19 = 0', h16, h16, h16, h16)) FROM q;
SELECT vexec_check(format('SELECT id, halfvec_l2_squared_distance(half, %L), halfvec_negative_inner_product(half, %L), halfvec_spherical_distance(half, %L) FROM items WHERE id %% 23 = 0', h16, h16, h16)) FROM q;

-- two columns of the table, neither a constant
SELECT vexec_check('SELECT a.id, a.embedding <-> b.embedding FROM items a JOIN items b ON b.id = a.id + 1 WHERE a.id % 97 = 0');
SELECT vexec_check('SELECT count(*) FROM items WHERE embedding <-> half::vector > 0.0005');

-- vectors stored out of line, 1536 dimensions
SELECT vexec_check(format('SELECT id FROM wide ORDER BY e <-> %L, id LIMIT 10', v1536)) FROM q;
SELECT vexec_check(format('SELECT id, e <=> %L FROM wide WHERE id %% 101 = 0', v1536)) FROM q;
SELECT vexec_check(format('SELECT count(*) FROM wide WHERE e <#> %L < 0', v1536)) FROM q;

-- other dimensions: pgvector's error, at the first row PostgreSQL reaches
SELECT vexec_check('SELECT count(*) FROM mixed WHERE e <-> ''[1,2,3]'' < 5');
SELECT vexec_check('SELECT id, e <=> ''[1,2,3]'' FROM mixed ORDER BY 2, id');
SELECT vexec_check('SELECT sum(l1_distance(e, ''[1,2,3]'')) FROM mixed');
SELECT vexec_check('SELECT id FROM mixed WHERE id > 990 AND id < 1010 AND e <+> ''[1,2]'' < 1');
-- PostgreSQL makes an equality to a constant again from its equivalence
-- class, after the other quals: the distance is computed first, and raises
-- on the first such row, in vexec too
SELECT vexec_check('SELECT count(*) FROM mixed WHERE vector_dims(e) = 3 AND e <-> ''[1,2,3]'' < 5');
-- and nowhere PostgreSQL reaches no such row: a qual before it, rows past a
-- LIMIT -- a batch's eager target is computed for row 1000, and its error
-- never raised
SELECT vexec_check('SELECT count(*) FROM mixed WHERE vector_dims(e) > 2 AND e <-> ''[1,2,3]'' < 5');
SELECT vexec_check('SELECT count(*) FROM mixed WHERE id % 1000 <> 0 AND e <-> ''[1,2,3]'' < 5');
SELECT vexec_check('SELECT count(*) FROM mixed WHERE id % 1000 <> 0 AND e <=> ''[1,2,3]'' < 0.1');
SELECT vexec_check('SELECT id, e <-> ''[1,2,3]'' FROM mixed WHERE id < 1990 LIMIT 900');

-- batch edges: 1023, 1024 and 1025 rows
SELECT vexec_check(format('SELECT count(*), sum(e <-> %L) FROM wide WHERE id <= 1023', v1536)) FROM q;
SELECT vexec_check(format('SELECT count(*), sum(e <-> %L) FROM wide WHERE id <= 1024', v1536)) FROM q;
SELECT vexec_check(format('SELECT count(*), sum(e <-> %L) FROM wide', v1536)) FROM q;

DROP TABLE items, wide, mixed, q;
DROP FUNCTION vexec_check(text);
DROP FUNCTION vexec_run(text, text, text);
