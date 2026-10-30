-- Writes into a table of its own files: inserts, updates, deletes, upserts,
-- MERGE, COPY, TRUNCATE, indexes, constraints and triggers, as heap's
CREATE TABLE items (id int4 PRIMARY KEY, name text NOT NULL, qty int4 CHECK (qty >= 0), note text)
	USING vexec_parquet;
INSERT INTO items VALUES (1, 'apple', 10, NULL), (2, 'pear', 5, 'green');
INSERT INTO items SELECT i, 'item ' || i, i % 7, NULL FROM generate_series(3, 1000) i;
COPY items FROM stdin;
1001	copied	3	\N
1002	copied too	4	from COPY
\.
SELECT count(*), sum(id), sum(qty) FROM items;
-- constraints
INSERT INTO items VALUES (1, 'dup', 1, NULL);
INSERT INTO items VALUES (2000, NULL, 1, NULL);
INSERT INTO items VALUES (2000, 'neg', -1, NULL);
-- UPDATE and DELETE, with RETURNING
UPDATE items SET qty = qty + 100, note = 'more' WHERE id <= 3 RETURNING id, name, qty, note;
DELETE FROM items WHERE id BETWEEN 10 AND 19 RETURNING id;
UPDATE items SET name = upper(name) WHERE id = 1001 RETURNING *;
SELECT count(*), sum(id), sum(qty) FROM items;
SELECT * FROM items WHERE id IN (1, 2, 3, 10, 1001, 1002) ORDER BY id;
-- a row updated twice in a transaction, and in one statement through a join
BEGIN;
UPDATE items SET qty = 1 WHERE id = 4;
UPDATE items SET qty = qty + 1 WHERE id = 4;
UPDATE items i SET qty = i.qty + v.d FROM (VALUES (5, 1), (5, 2)) v(id, d) WHERE i.id = v.id;
SELECT id, qty FROM items WHERE id IN (4, 5) ORDER BY id;
COMMIT;
SELECT id, qty FROM items WHERE id IN (4, 5) ORDER BY id;

-- upserts: RETURNING xmax = 0 an insert, else an update, as heap's
INSERT INTO items VALUES (1, 'apple', 1, NULL), (3000, 'new', 1, NULL)
	ON CONFLICT (id) DO UPDATE SET qty = items.qty + excluded.qty
	RETURNING id, qty, xmax = 0 AS inserted;
INSERT INTO items VALUES (2, 'pear', 1, NULL), (3001, 'newer', 1, NULL)
	ON CONFLICT (id) DO NOTHING RETURNING id;
INSERT INTO items VALUES (2, 'pear', 1, NULL) ON CONFLICT (id) DO UPDATE SET qty = 0 WHERE items.qty > 1000
	RETURNING id;
-- a row a command would affect twice
INSERT INTO items VALUES (3002, 'a', 1, NULL), (3002, 'b', 1, NULL)
	ON CONFLICT (id) DO UPDATE SET name = excluded.name;
-- DELETE's RETURNING xmax: the deleter's
BEGIN;
DELETE FROM items WHERE id = 3001 RETURNING id, xmax = pg_current_xact_id()::xid AS xmax_ours;
ROLLBACK;

-- MERGE
CREATE TABLE changes (id int4, qty int4) USING vexec_parquet;
INSERT INTO changes VALUES (1, 0), (2, 7), (4000, 1);
MERGE INTO items i USING changes c ON i.id = c.id
	WHEN MATCHED AND c.qty = 0 THEN DELETE
	WHEN MATCHED THEN UPDATE SET qty = c.qty
	WHEN NOT MATCHED THEN INSERT VALUES (c.id, 'merged', c.qty, NULL)
	RETURNING merge_action(), i.id, i.qty;
INSERT INTO changes VALUES (2, 8);
MERGE INTO items i USING changes c ON i.id = c.id WHEN MATCHED THEN UPDATE SET qty = c.qty;

-- secondary indexes, read by index, bitmap and index-only scans
CREATE INDEX items_name ON items (name);
CREATE INDEX items_qty ON items USING hash (qty);
CREATE INDEX items_note ON items (note) WHERE note IS NOT NULL;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT id, name FROM items WHERE name = 'item 500';
SELECT id, name FROM items WHERE name = 'item 500';
SET enable_indexscan = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM items WHERE qty = 6;
SELECT count(*) FROM items WHERE qty = 6;
RESET enable_indexscan;
SELECT id, note FROM items WHERE note IS NOT NULL ORDER BY note, id;
UPDATE items SET name = 'renamed' WHERE id = 500;
SELECT id, name FROM items WHERE name IN ('item 500', 'renamed');
DELETE FROM items WHERE name = 'renamed';
SELECT id, name FROM items WHERE name IN ('item 500', 'renamed');
RESET enable_seqscan;
-- a unique index on rows already in files, a deleted row's value given
-- again, and one refused
INSERT INTO items VALUES (10, 'item 10', 3, NULL);
CREATE UNIQUE INDEX items_name_u ON items (name);
CREATE UNIQUE INDEX items_qty_u ON items (qty);
INSERT INTO items VALUES (6000, 'item 10', 3, NULL);
-- concurrent index builds read no consistent set of files: refused
CREATE INDEX CONCURRENTLY items_qty2 ON items (qty);
REINDEX TABLE CONCURRENTLY items;
REINDEX TABLE items;
SELECT count(*) FROM items WHERE name LIKE 'item 9%';

-- foreign keys, both ways
CREATE TABLE orders (oid int4 PRIMARY KEY, item int4 REFERENCES items ON DELETE CASCADE, n int4)
	USING vexec_parquet;
INSERT INTO orders VALUES (1, 3, 1), (2, 4, 1), (3, 21, 5);
INSERT INTO orders VALUES (4, 999999, 1);
DELETE FROM items WHERE id = 21;
SELECT * FROM orders ORDER BY oid;
UPDATE items SET id = 9999 WHERE id = 3;
CREATE TABLE refs (id int4 REFERENCES orders ON UPDATE CASCADE) USING heap;
INSERT INTO refs VALUES (1);
UPDATE orders SET oid = 100 WHERE oid = 1;
SELECT * FROM refs;
DELETE FROM orders WHERE oid = 100;

-- triggers see the rows heap's would
CREATE TABLE audit (op text, old_id int4, new_id int4, new_qty int4) USING vexec_parquet;
CREATE FUNCTION items_audit() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	IF TG_OP = 'INSERT' THEN
		NEW.qty := NEW.qty * 10;
		INSERT INTO audit VALUES (TG_OP, NULL, NEW.id, NEW.qty);
		RETURN NEW;
	ELSIF TG_OP = 'UPDATE' THEN
		INSERT INTO audit VALUES (TG_OP, OLD.id, NEW.id, NEW.qty);
		RETURN NEW;
	ELSE
		INSERT INTO audit VALUES (TG_OP, OLD.id, NULL, NULL);
		RETURN OLD;
	END IF;
END $$;
CREATE TRIGGER items_audit BEFORE INSERT OR UPDATE OR DELETE ON items FOR EACH ROW EXECUTE FUNCTION items_audit();
CREATE FUNCTION items_after() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
	INSERT INTO audit SELECT 'AFTER ' || TG_OP, (SELECT count(*) FROM old_rows), (SELECT count(*) FROM new_rows), NULL;
	RETURN NULL;
END $$;
CREATE TRIGGER items_after AFTER UPDATE ON items REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows
	FOR EACH STATEMENT EXECUTE FUNCTION items_after();
INSERT INTO items VALUES (5000, 'triggered', 2, NULL);
UPDATE items SET qty = qty + 1 WHERE id IN (5000, 6);
DELETE FROM items WHERE id = 5000;
SELECT * FROM audit ORDER BY op, old_id NULLS FIRST, new_id;
DROP TRIGGER items_audit ON items;
DROP TRIGGER items_after ON items;

-- TRUNCATE, rolled back and kept
BEGIN;
TRUNCATE orders, items CASCADE;
SELECT count(*) FROM items;
ROLLBACK;
SELECT count(*) FROM items;
CREATE TABLE scratch (a int4) USING vexec_parquet;
INSERT INTO scratch SELECT generate_series(1, 100);
BEGIN;
TRUNCATE scratch;
INSERT INTO scratch VALUES (7);
COMMIT;
SELECT * FROM scratch;

-- CTAS, SELECT INTO, INSERT ... SELECT from the table itself
CREATE TABLE copy1 USING vexec_parquet AS SELECT id, name FROM items WHERE id < 100;
SELECT id, name INTO TEMP copy2 FROM items WHERE id < 50;
INSERT INTO copy1 SELECT * FROM copy1;
SELECT count(*), count(DISTINCT id) FROM copy1;
SELECT count(*) FROM copy2;

-- partitions and inheritance
CREATE TABLE parted (k int4, v text) PARTITION BY RANGE (k);
CREATE TABLE parted_1 PARTITION OF parted FOR VALUES FROM (0) TO (100) USING vexec_parquet;
CREATE TABLE parted_2 PARTITION OF parted FOR VALUES FROM (100) TO (200) USING heap;
INSERT INTO parted SELECT i, 'p' || i FROM generate_series(0, 199) i;
UPDATE parted SET k = k + 100 WHERE k BETWEEN 90 AND 94;
UPDATE parted SET k = k - 150 WHERE k BETWEEN 190 AND 192;
SELECT tableoid::regclass, count(*), sum(k) FROM parted GROUP BY 1 ORDER BY 1;
CREATE TABLE inh_parent (a int4) USING vexec_parquet;
CREATE TABLE inh_child (b text) INHERITS (inh_parent) USING vexec_parquet;
INSERT INTO inh_parent VALUES (1);
INSERT INTO inh_child VALUES (2, 'two');
SELECT tableoid::regclass, * FROM inh_parent ORDER BY a;
UPDATE inh_parent SET a = a * 10;
SELECT tableoid::regclass, * FROM inh_parent ORDER BY a;

-- temporary and unlogged tables
CREATE TEMP TABLE tmp (a int4, b text) USING vexec_parquet;
INSERT INTO tmp SELECT i, 't' || i FROM generate_series(1, 10) i;
UPDATE tmp SET b = 'u' WHERE a > 5;
DELETE FROM tmp WHERE a % 2 = 0;
SELECT * FROM tmp ORDER BY a;
CREATE TEMP TABLE tmp_commit (a int4) USING vexec_parquet ON COMMIT DELETE ROWS;
BEGIN;
INSERT INTO tmp_commit VALUES (1);
SELECT count(*) FROM tmp_commit;
COMMIT;
SELECT count(*) FROM tmp_commit;
CREATE UNLOGGED TABLE unl (a int4) USING vexec_parquet;
INSERT INTO unl VALUES (1), (2);
ALTER TABLE unl SET LOGGED;
INSERT INTO unl VALUES (3);
SELECT * FROM unl ORDER BY a;

-- generated columns, identity, defaults
CREATE TABLE gen (id int4 GENERATED ALWAYS AS IDENTITY, a int4, b int4 GENERATED ALWAYS AS (a * 2) STORED,
	c int4 GENERATED ALWAYS AS (a * 3) VIRTUAL, d timestamptz DEFAULT '2020-01-01') USING vexec_parquet;
INSERT INTO gen (a) VALUES (1), (2);
UPDATE gen SET a = 10 WHERE id = 2;
SELECT * FROM gen ORDER BY id;

-- cursors, both ways, and positioned updates
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT id FROM items WHERE id < 30 ORDER BY id;
FETCH 3 FROM c;
FETCH BACKWARD 2 FROM c;
FETCH LAST FROM c;
FETCH PRIOR FROM c;
CLOSE c;
DECLARE n NO SCROLL CURSOR FOR SELECT id, qty FROM items WHERE id = 7 FOR UPDATE;
FETCH 1 FROM n;
UPDATE items SET qty = 77 WHERE CURRENT OF n;
COMMIT;
SELECT id, qty FROM items WHERE id = 7;
BEGIN;
DECLARE s SCROLL CURSOR FOR SELECT a FROM scratch;
FETCH ALL FROM s;
FETCH BACKWARD ALL FROM s;
COMMIT;

-- VACUUM, ANALYZE, CLUSTER and VACUUM FULL keep the rows and the indexes
VACUUM items;
ANALYZE items;
SELECT reltuples > 0 AS analyzed FROM pg_class WHERE relname = 'items';
SELECT attname, null_frac FROM pg_stats WHERE tablename = 'items' AND attname IN ('id', 'note') ORDER BY 1;
CLUSTER items USING items_pkey;
SELECT count(*), sum(id), sum(qty) FROM items;
VACUUM FULL items;
SELECT count(*), sum(id), sum(qty) FROM items;
SET enable_seqscan = off;
SELECT id, name FROM items WHERE id = 777;
RESET enable_seqscan;
-- TABLESAMPLE: all of it, by rows and by blocks
SELECT count(*) FROM items TABLESAMPLE BERNOULLI (100);
SELECT count(*) FROM items TABLESAMPLE SYSTEM (100);
-- the cumulative statistics: an update counts no insert, a sequential scan is counted
CREATE TABLE counted (a int4) USING vexec_parquet;
INSERT INTO counted VALUES (1), (2), (3);
UPDATE counted SET a = a + 10 WHERE a < 3;
DELETE FROM counted WHERE a = 3;
SELECT count(*) FROM counted;
SELECT pg_stat_force_next_flush();
SELECT n_tup_ins, n_tup_upd, n_tup_del, n_live_tup, n_dead_tup, seq_scan FROM pg_stat_user_tables WHERE relname = 'counted';
VACUUM counted;
SELECT pg_stat_force_next_flush();
SELECT n_live_tup, n_dead_tup, last_vacuum IS NOT NULL AS vacuumed FROM pg_stat_user_tables WHERE relname = 'counted';
