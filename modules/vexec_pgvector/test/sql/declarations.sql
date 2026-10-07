-- SPDX-License-Identifier: Apache-2.0
--
-- vexec_pgvector's declarations against pgvector itself
-- (pg_vector_executor.md §3.17, VK's "done when"): over random vectors and
-- edge cases -- every dimension count from 1 to pgvector's 16,000, zero
-- vectors, negative zeros, the largest and the smallest floats, values
-- stored out of line -- each declared distance, called directly, raises
-- nothing on a pair whose check passes.  Its check being that the two
-- dimensions agree, it raises pgvector's own error on every pair the check
-- fails.  Neither vexec nor the registry takes part: this is what vexec
-- relies on, for the pgvector versions the pack names.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vector;
CREATE EXTENSION vexec_pgvector;
RESET client_min_messages;

-- the declarations, and the functions of this database they name
SELECT name, argtypes, symbol, declaration, versions FROM vexec_pgvector.declarations()
ORDER BY argtypes[1], name;
SELECT d.name, p.oid::regprocedure AS function
FROM vexec_pgvector.declarations() d
	 LEFT JOIN pg_proc p ON p.proname = d.name AND p.prosrc = d.symbol AND p.probin = '$libdir/vector'
ORDER BY d.argtypes[1], d.name;
SELECT extversion = ANY ((SELECT versions FROM vexec_pgvector.declarations() LIMIT 1)::text[]) AS named
FROM pg_extension WHERE extname = 'vector';

-- the vectors: random ones of each dimension count, each type's edges
SELECT setseed(0.1717);
CREATE TABLE vec (id serial, kind text, v vector);
INSERT INTO vec (kind, v)
SELECT 'random ' || d,
	   (SELECT array_agg((random() * 2 - 1)::real) FROM generate_series(1, d) WHERE g > 0)::vector
FROM unnest(ARRAY[1, 2, 3, 16, 128, 769, 2000, 16000]) d, generate_series(1, 3) g;
INSERT INTO vec (kind, v) VALUES
	('zero 3', '[0,0,0]'), ('negative zero 3', '[-0,-0,0]'),
	('zero 16', array_fill(0::real, ARRAY[16])::vector),
	('zero 2000', array_fill(0::real, ARRAY[2000])::vector),
	('largest 3', '[3.4028235e38,-3.4028235e38,1]'),
	('smallest 3', '[1.4e-45,-1.4e-45,1.17549435e-38]'),
	('large 16', array_fill(1e19::real, ARRAY[16])::vector);
CREATE TABLE hvec (id serial, kind text, v halfvec);
INSERT INTO hvec (kind, v)
SELECT 'random ' || d,
	   (SELECT array_agg((random() * 2 - 1)::real) FROM generate_series(1, d) WHERE g > 0)::halfvec
FROM unnest(ARRAY[1, 2, 3, 16, 128, 769, 4000, 16000]) d, generate_series(1, 3) g;
INSERT INTO hvec (kind, v) VALUES
	('zero 3', '[0,0,0]'), ('negative zero 3', '[-0,-0,0]'),
	('zero 4000', array_fill(0::real, ARRAY[4000])::halfvec),
	('largest 3', '[65504,-65504,1]'),
	('smallest 3', '[6e-8,-6e-8,6.1e-5]');

-- values stored out of line, as pgvector's types are external
SELECT count(*) FILTER (WHERE pg_column_toast_chunk_id(v) IS NOT NULL) AS out_of_line,
	   count(*) AS vectors FROM vec;
SELECT count(*) FILTER (WHERE pg_column_toast_chunk_id(v) IS NOT NULL) AS out_of_line,
	   count(*) AS halfvecs FROM hvec;

-- a call of a function on two values: 'ok', or its error
CREATE FUNCTION try_call(fn regprocedure, a anyelement, b anyelement) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	r float8;
BEGIN
	EXECUTE format('SELECT %s($1, $2)', fn::oid::regproc) INTO r USING a, b;
	RETURN 'ok';
EXCEPTION WHEN OTHERS THEN
	RETURN SQLSTATE || ' ' || SQLERRM;
END
$$;

-- every declared function on every pair of its type's values
CREATE TABLE outcome AS
SELECT p.oid::regprocedure AS fn, a.kind AS ka, b.kind AS kb,
	   vexec_pgvector.dims_check(a.v, b.v) AS passes, try_call(p.oid::regprocedure, a.v, b.v) AS result
FROM vexec_pgvector.declarations() d
	 JOIN pg_proc p ON p.proname = d.name AND p.prosrc = d.symbol AND p.probin = '$libdir/vector'
	 JOIN vec a ON d.argtypes[1] = 'vector'
	 JOIN vec b ON true
UNION ALL
SELECT p.oid::regprocedure, a.kind, b.kind,
	   vexec_pgvector.dims_check(a.v, b.v), try_call(p.oid::regprocedure, a.v, b.v)
FROM vexec_pgvector.declarations() d
	 JOIN pg_proc p ON p.proname = d.name AND p.prosrc = d.symbol AND p.probin = '$libdir/vector'
	 JOIN hvec a ON d.argtypes[1] = 'halfvec'
	 JOIN hvec b ON true;

-- none raises on a pair its check passes; each raises pgvector's error on
-- every other pair
SELECT fn, count(*) AS pairs, count(*) FILTER (WHERE passes) AS passed,
	   count(*) FILTER (WHERE passes AND result <> 'ok') AS raised_where_passed,
	   count(*) FILTER (WHERE NOT passes AND result = 'ok') AS failed_without_error,
	   count(*) FILTER (WHERE NOT passes AND result ~ '^22000 different (vector|halfvec) dimensions') AS dimension_errors
FROM outcome GROUP BY fn ORDER BY fn::text;

-- the errors, by kind
SELECT regexp_replace(result, '\d+', 'N', 'g') AS error, count(*)
FROM outcome WHERE result <> 'ok' GROUP BY 1 ORDER BY 1;

-- a zero vector under cosine distance is NaN, and no error
SELECT cosine_distance('[0,0,0]'::vector, '[1,2,3]'::vector) AS cosine_of_zero,
	   vexec_pgvector.dims_check('[0,0,0]'::vector, '[1,2,3]'::vector) AS passes;

-- the check on a value stored out of line, read through a slice, and on
-- NULLs, which a strict function is never called with
SELECT vexec_pgvector.dims_check(a.v, b.v) AS out_of_line_pair
FROM vec a, vec b WHERE a.kind = 'random 2000' AND b.kind = 'zero 2000' LIMIT 1;
SELECT vexec_pgvector.dims_check(NULL::vector, '[1]'::vector) AS with_null;

DROP TABLE outcome, vec, hvec;
DROP FUNCTION try_call(regprocedure, anyelement, anyelement);
