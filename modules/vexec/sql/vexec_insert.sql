-- SPDX-License-Identifier: Apache-2.0
--
-- VecInsert (pg_vector_executor.md §3.16; §5 VI): an INSERT's rows written
-- a batch at a time, in place of ModifyTable's row loop, through the
-- target's sink or table_multi_insert().  On a vanilla server no access
-- method has a sink: heap's rows go through table_multi_insert(), as COPY
-- writes them.  The tables it writes equal ModifyTable's, its errors are
-- PostgreSQL's, and where ModifyTable's work is needed the planner keeps
-- ModifyTable, with the reason.  The port's sinks are the sinks leg's.

\i sql/vexec_corpus_setup.sql

-- every type of the corpus, in both formats, against ModifyTable's load
CREATE TABLE ins_off (LIKE corpus);
CREATE TABLE ins_vec (LIKE corpus);
SET vexec.mode = off;
INSERT INTO ins_off SELECT * FROM corpus;
SET vexec.mode = force;
EXPLAIN (COSTS OFF) INSERT INTO ins_vec SELECT * FROM corpus;
DO $$
DECLARE
	f text;
	n bigint;
BEGIN
	FOREACH f IN ARRAY ARRAY['postgres', 'arrow'] LOOP
		PERFORM set_config('vexec.batch_format', f, true);
		TRUNCATE ins_vec;
		INSERT INTO ins_vec SELECT * FROM corpus;
		-- as text: json has no equality
		SELECT count(*) INTO n FROM (
			(SELECT rv::text FROM ins_vec rv EXCEPT ALL SELECT ro::text FROM ins_off ro)
			UNION ALL
			(SELECT ro::text FROM ins_off ro EXCEPT ALL SELECT rv::text FROM ins_vec rv)) x;
		RAISE NOTICE 'format %: % rows, % differ', f, (SELECT count(*) FROM ins_vec), n;
	END LOOP;
END $$;
RESET vexec.batch_format;

-- rows a row node gives, gathered into batches; the count in the tag
EXPLAIN (COSTS OFF) INSERT INTO ins_vec SELECT * FROM corpus ORDER BY id LIMIT 10;
INSERT INTO ins_vec (id, tx) VALUES (-1, 'one'), (-2, 'two');
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  INSERT INTO ins_vec (id, i4) SELECT g, g FROM generate_series(1, 2500) g;

-- what the target computes: defaults, identity, generated columns, domains
CREATE DOMAIN ins_pos AS int4 CHECK (VALUE > 0);
CREATE SEQUENCE ins_seq;
CREATE TABLE ins_comp (
	id int4 GENERATED ALWAYS AS IDENTITY,
	s int8 DEFAULT nextval('ins_seq'),
	k ins_pos,
	t text DEFAULT 'none',
	g int4 GENERATED ALWAYS AS (k * 2) STORED,
	v int4 GENERATED ALWAYS AS (id + 1) VIRTUAL);
INSERT INTO ins_comp (k) SELECT g FROM generate_series(1, 3000) g;
SELECT count(*), sum(id), sum(s), sum(g), sum(v), count(*) FILTER (WHERE t = 'none') FROM ins_comp;
INSERT INTO ins_comp (k) SELECT g - 5 FROM generate_series(1, 3000) g;

-- constraints: NOT NULL and CHECK, PostgreSQL's errors, the first failing
-- row's; a unique index's violation; nothing written
CREATE TABLE ins_chk (id int4 NOT NULL, a int4 CHECK (a < 2000), u int4 UNIQUE);
INSERT INTO ins_chk SELECT g, g % 1000, g FROM generate_series(1, 3000) g;
INSERT INTO ins_chk SELECT CASE WHEN g = 2222 THEN NULL ELSE g END, 1, -g FROM generate_series(1, 3000) g;
INSERT INTO ins_chk SELECT g, CASE WHEN g = 1777 THEN 5000 ELSE 1 END, -g FROM generate_series(1, 3000) g;
INSERT INTO ins_chk SELECT g, 1, 10 + g % 2000 FROM generate_series(1, 3000) g;
SELECT count(*), sum(id) FROM ins_chk;
SET vexec.mode = off;
INSERT INTO ins_chk SELECT CASE WHEN g = 2222 THEN NULL ELSE g END, 1, -g FROM generate_series(1, 3000) g;
INSERT INTO ins_chk SELECT g, CASE WHEN g = 1777 THEN 5000 ELSE 1 END, -g FROM generate_series(1, 3000) g;
INSERT INTO ins_chk SELECT g, 1, 10 + g % 2000 FROM generate_series(1, 3000) g;
SET vexec.mode = force;

-- an error in the middle of the statement, and one in the middle of a
-- batch: the table as it was
BEGIN;
INSERT INTO ins_chk SELECT g, 1, 100000 + g FROM generate_series(1, 2000) g;
INSERT INTO ins_chk SELECT g, 1, 200000 + g + 0 * (g / (g - 1500)) FROM generate_series(1, 2000) g;
ROLLBACK;
SELECT count(*), sum(id) FROM ins_chk;

-- char(n) and varchar(n) from text: the length coercions' kernels, padding,
-- cutting what is blank, explicit casts cutting, PostgreSQL's errors
CREATE TABLE ins_len (id int4, c5 char(5), v5 varchar(5), c2 char(2));
CREATE TABLE ins_len_off (LIKE ins_len);
CREATE TEMP TABLE ins_strs AS
SELECT g AS id,
	CASE g % 7 WHEN 0 THEN NULL WHEN 1 THEN 'ab' WHEN 2 THEN 'abcde' WHEN 3 THEN 'abc  '
		WHEN 4 THEN 'äöü' WHEN 5 THEN 'ab     ' ELSE '日本' END AS t
FROM generate_series(1, 3000) g;
EXPLAIN (COSTS OFF, VERBOSE) INSERT INTO ins_len SELECT id, t, t, left(t, 2)::char(2) FROM ins_strs;
INSERT INTO ins_len SELECT id, t, t, t::char(2) FROM ins_strs;
SET vexec.mode = off;
INSERT INTO ins_len_off SELECT id, t, t, t::char(2) FROM ins_strs;
SET vexec.mode = force;
SELECT count(*) AS differ FROM (
	(SELECT * FROM ins_len EXCEPT ALL SELECT * FROM ins_len_off)
	UNION ALL
	(SELECT * FROM ins_len_off EXCEPT ALL SELECT * FROM ins_len)) x;
SELECT id, c5, octet_length(c5), v5, c2 FROM ins_len WHERE id <= 7 ORDER BY id;
INSERT INTO ins_len (id, c2) SELECT id, t FROM ins_strs;
INSERT INTO ins_len (id, v5) SELECT id, t || 'x' FROM ins_strs;

-- where ModifyTable stays, and why
CREATE TABLE ins_trg (a int4);
CREATE FUNCTION ins_trg_fn() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;
CREATE TRIGGER ins_trg_t BEFORE INSERT ON ins_trg FOR EACH ROW EXECUTE FUNCTION ins_trg_fn();
CREATE TABLE ins_ref (a int4 PRIMARY KEY);
CREATE TABLE ins_fk (a int4 REFERENCES ins_ref);
CREATE TABLE ins_def (a int4 UNIQUE DEFERRABLE);
CREATE VIEW ins_view AS SELECT a FROM ins_ref WHERE a > 0 WITH CHECK OPTION;
EXPLAIN (COSTS OFF, VEXEC) INSERT INTO ins_trg SELECT g FROM generate_series(1, 10) g;
EXPLAIN (COSTS OFF, VEXEC) INSERT INTO ins_fk SELECT g FROM generate_series(1, 10) g;
EXPLAIN (COSTS OFF, VEXEC) INSERT INTO ins_def SELECT g FROM generate_series(1, 10) g;
EXPLAIN (COSTS OFF, VEXEC) INSERT INTO ins_ref SELECT g FROM generate_series(1, 10) g RETURNING a;
EXPLAIN (COSTS OFF, VEXEC) INSERT INTO ins_ref SELECT g FROM generate_series(1, 10) g ON CONFLICT DO NOTHING;
EXPLAIN (COSTS OFF, VEXEC) INSERT INTO ins_view SELECT g FROM generate_series(1, 10) g;
SET vexec.enable_insert = off;
EXPLAIN (COSTS OFF, VEXEC) INSERT INTO ins_ref SELECT g FROM generate_series(1, 10) g;
RESET vexec.enable_insert;
SET vexec.mode = auto;
EXPLAIN (COSTS OFF, VEXEC) INSERT INTO ins_ref VALUES (1);
SET vexec.mode = force;

-- partitions: each row routed as COPY routes it, the partition's own
-- checks, a partition of another column order, a sub-partitioned level,
-- hash partitions every batch's rows spread among, PostgreSQL's errors
CREATE TABLE ins_pr (id int4 NOT NULL, d date NOT NULL, t text) PARTITION BY RANGE (d);
CREATE TABLE ins_pr_a PARTITION OF ins_pr FOR VALUES FROM ('2024-01-01') TO ('2024-04-01');
CREATE TABLE ins_pr_b (t text, junk int4, d date NOT NULL, id int4 NOT NULL);
ALTER TABLE ins_pr_b DROP COLUMN junk;
ALTER TABLE ins_pr ATTACH PARTITION ins_pr_b FOR VALUES FROM ('2024-04-01') TO ('2024-07-01');
CREATE TABLE ins_pr_c PARTITION OF ins_pr FOR VALUES FROM ('2024-07-01') TO ('2025-01-01')
	PARTITION BY LIST ((id % 2));
CREATE TABLE ins_pr_c0 PARTITION OF ins_pr_c FOR VALUES IN (0);
CREATE TABLE ins_pr_c1 PARTITION OF ins_pr_c FOR VALUES IN (1);
ALTER TABLE ins_pr_c1 ADD CONSTRAINT ins_pr_c1_t CHECK (t <> 'bad');
CREATE UNIQUE INDEX ON ins_pr_a (id);
CREATE TABLE ins_pr_off (LIKE ins_pr);
CREATE TEMP TABLE ins_pr_src AS
SELECT g AS id, DATE '2024-01-01' + (g % 365) AS d, 'v' || g AS t FROM generate_series(1, 10000) g;
EXPLAIN (COSTS OFF, VEXEC) INSERT INTO ins_pr SELECT * FROM ins_pr_src;
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) INSERT INTO ins_pr SELECT * FROM ins_pr_src;
INSERT INTO ins_pr_off SELECT * FROM ins_pr_src;
SELECT tableoid::regclass, count(*), sum(id) FROM ins_pr GROUP BY 1 ORDER BY 1;
SELECT count(*) AS differ FROM (
	(SELECT * FROM ins_pr EXCEPT ALL SELECT * FROM ins_pr_off)
	UNION ALL
	(SELECT * FROM ins_pr_off EXCEPT ALL SELECT * FROM ins_pr)) x;
TRUNCATE ins_pr;
INSERT INTO ins_pr SELECT id, CASE WHEN id = 2500 THEN DATE '2030-01-01' ELSE d END, t FROM ins_pr_src;
INSERT INTO ins_pr SELECT CASE WHEN id = 2500 THEN NULL ELSE id END, d, t FROM ins_pr_src;
INSERT INTO ins_pr SELECT id, d, CASE WHEN id = 2549 THEN 'bad' ELSE t END FROM ins_pr_src;
INSERT INTO ins_pr VALUES (7, '2024-01-08', 'x'), (7, '2024-01-08', 'y');
SET vexec.mode = off;
INSERT INTO ins_pr SELECT id, CASE WHEN id = 2500 THEN DATE '2030-01-01' ELSE d END, t FROM ins_pr_src;
INSERT INTO ins_pr SELECT CASE WHEN id = 2500 THEN NULL ELSE id END, d, t FROM ins_pr_src;
INSERT INTO ins_pr SELECT id, d, CASE WHEN id = 2549 THEN 'bad' ELSE t END FROM ins_pr_src;
INSERT INTO ins_pr VALUES (7, '2024-01-08', 'x'), (7, '2024-01-08', 'y');
SET vexec.mode = force;
SELECT count(*) FROM ins_pr;
CREATE TABLE ins_ph (id int4, g int4 GENERATED ALWAYS AS (id * 2) STORED) PARTITION BY HASH (id);
SELECT format('CREATE TABLE ins_ph_%s PARTITION OF ins_ph FOR VALUES WITH (MODULUS 40, REMAINDER %s)', r, r)
FROM generate_series(0, 39) r \gexec
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  INSERT INTO ins_ph (id) SELECT g FROM generate_series(1, 10000) g;
SELECT count(*), sum(g), count(DISTINCT tableoid) FROM ins_ph;

-- a trigger made on a partition: a plan made before it is made again
PREPARE ins_prep AS INSERT INTO ins_pr SELECT * FROM ins_pr_src WHERE id <= 100;
TRUNCATE ins_pr;
EXPLAIN (COSTS OFF) EXECUTE ins_prep;
EXECUTE ins_prep;
CREATE FUNCTION ins_mark() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN NEW.t := 'marked'; RETURN NEW; END $$;
CREATE TRIGGER ins_mark_t BEFORE INSERT ON ins_pr_a FOR EACH ROW EXECUTE FUNCTION ins_mark();
TRUNCATE ins_pr;
EXPLAIN (COSTS OFF) EXECUTE ins_prep;
EXECUTE ins_prep;
SELECT count(*) FILTER (WHERE t = 'marked') AS marked, count(*) FROM ins_pr;
DEALLOCATE ins_prep;

DROP TABLE ins_off, ins_vec, ins_comp, ins_chk, ins_len, ins_len_off, ins_trg, ins_fk,
	ins_def, ins_pr, ins_pr_off, ins_ph CASCADE;
DROP TABLE ins_ref CASCADE;
DROP FUNCTION ins_trg_fn(), ins_mark();
DROP DOMAIN ins_pos;
DROP SEQUENCE ins_seq;
DROP TABLE corpus;
DROP FUNCTION corpus_null(int4, int4);
