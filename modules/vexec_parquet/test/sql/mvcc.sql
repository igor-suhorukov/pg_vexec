-- What a statement sees of a table of its own files: its own transaction's
-- rows by command, savepoints, cursors' snapshots, CTEs, prepared
-- transactions.  Sessions that run together are the isolation specs'.
CREATE TABLE m (k int4, v text) USING vexec_parquet;
INSERT INTO m VALUES (1, 'one'), (2, 'two');

-- rows of a rolled-back transaction are no one's
BEGIN;
INSERT INTO m VALUES (3, 'three');
UPDATE m SET v = 'ONE' WHERE k = 1;
DELETE FROM m WHERE k = 2;
SELECT * FROM m ORDER BY k;
ROLLBACK;
SELECT * FROM m ORDER BY k;

-- savepoints: a subtransaction's rows go with it, its parent's stay
BEGIN;
INSERT INTO m VALUES (10, 'ten');
SAVEPOINT a;
INSERT INTO m VALUES (11, 'eleven');
UPDATE m SET v = 'TEN' WHERE k = 10;
DELETE FROM m WHERE k = 1;
SELECT * FROM m ORDER BY k;
ROLLBACK TO SAVEPOINT a;
SELECT * FROM m ORDER BY k;
SAVEPOINT b;
INSERT INTO m VALUES (12, 'twelve');
RELEASE SAVEPOINT b;
SAVEPOINT c;
INSERT INTO m VALUES (13, 'thirteen');
SAVEPOINT d;
DELETE FROM m WHERE k = 12;
ROLLBACK TO SAVEPOINT c;
SELECT * FROM m ORDER BY k;
COMMIT;
SELECT * FROM m ORDER BY k;
-- a failed statement in a subtransaction
BEGIN;
INSERT INTO m VALUES (20, 'twenty');
SAVEPOINT e;
INSERT INTO m SELECT 21, (1 / 0)::text;
ROLLBACK TO SAVEPOINT e;
INSERT INTO m VALUES (22, 'twenty-two');
COMMIT;
SELECT * FROM m WHERE k >= 20 ORDER BY k;
-- a PL/pgSQL block's exception handler is a subtransaction
DO $$
BEGIN
	INSERT INTO m VALUES (30, 'thirty');
	BEGIN
		INSERT INTO m VALUES (31, 'thirty-one');
		PERFORM 1 / 0;
	EXCEPTION WHEN division_by_zero THEN
		INSERT INTO m VALUES (32, 'thirty-two');
	END;
END $$;
SELECT * FROM m WHERE k >= 30 ORDER BY k;

-- a statement sees no row it writes itself
INSERT INTO m SELECT k + 100, v FROM m;
SELECT count(*) FROM m;
UPDATE m SET k = k + 1000 WHERE k > 100;
SELECT count(*), min(k), max(k) FROM m WHERE k > 100;
DELETE FROM m WHERE k > 100;
-- a CTE's writes are not the main query's to see
WITH ins AS (INSERT INTO m VALUES (40, 'forty') RETURNING k)
SELECT (SELECT count(*) FROM m WHERE k = 40) AS seen, (SELECT count(*) FROM ins) AS inserted;
WITH del AS (DELETE FROM m WHERE k = 40 RETURNING k)
SELECT (SELECT count(*) FROM m WHERE k = 40) AS still_seen, (SELECT count(*) FROM del) AS deleted;
SELECT count(*) FROM m WHERE k = 40;
-- a volatile function sees the commands before its own
CREATE FUNCTION m_count() RETURNS int8 LANGUAGE sql VOLATILE AS 'SELECT count(*) FROM m';
BEGIN;
INSERT INTO m VALUES (50, 'fifty');
SELECT m_count();
INSERT INTO m SELECT 51, m_count()::text;
SELECT v FROM m WHERE k = 51;
ROLLBACK;

-- a cursor reads the snapshot it was opened with
BEGIN;
DECLARE cur CURSOR FOR SELECT k FROM m ORDER BY k;
INSERT INTO m VALUES (60, 'sixty');
DELETE FROM m WHERE k = 1;
FETCH ALL FROM cur;
SELECT k FROM m ORDER BY k;
ROLLBACK;
-- and a holdable one keeps its rows past its transaction
BEGIN;
DECLARE hold CURSOR WITH HOLD FOR SELECT k, v FROM m ORDER BY k;
COMMIT;
DELETE FROM m WHERE k = 2;
FETCH 2 FROM hold;
CLOSE hold;
INSERT INTO m VALUES (2, 'two');

-- REPEATABLE READ: one snapshot for the transaction
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM m;
INSERT INTO m VALUES (70, 'seventy');
SELECT count(*) FROM m;
COMMIT;

-- prepared transactions: their rows wait for COMMIT PREPARED
BEGIN;
INSERT INTO m VALUES (80, 'eighty');
DELETE FROM m WHERE k = 70;
PREPARE TRANSACTION 'vpq_prepared';
SELECT k FROM m WHERE k IN (70, 80) ORDER BY k;
COMMIT PREPARED 'vpq_prepared';
SELECT k FROM m WHERE k IN (70, 80) ORDER BY k;
BEGIN;
INSERT INTO m VALUES (81, 'eighty-one');
PREPARE TRANSACTION 'vpq_prepared2';
ROLLBACK PREPARED 'vpq_prepared2';
SELECT k FROM m WHERE k = 81;

-- a table made, written and dropped in a transaction, and one dropped and rolled back
BEGIN;
CREATE TABLE gone (a int4) USING vexec_parquet;
INSERT INTO gone VALUES (1);
DROP TABLE gone;
COMMIT;
BEGIN;
DROP TABLE m;
ROLLBACK;
SELECT count(*) FROM m;
-- a table made in a rolled-back transaction leaves no storage
BEGIN;
CREATE TABLE never (a int4) USING vexec_parquet;
INSERT INTO never SELECT generate_series(1, 1000);
SELECT count(*) FROM never;
ROLLBACK;
SELECT count(*) AS storages_left FROM vexec_parquet.storages s
WHERE NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.relfilenode = s.relnumber);

-- the xmin of a row this transaction wrote is its own
BEGIN;
INSERT INTO m VALUES (90, 'ninety');
SELECT xmin = pg_current_xact_id()::xid AS ours FROM m WHERE k = 90;
COMMIT;
BEGIN;
SELECT xmin <> pg_current_xact_id()::xid AS anothers FROM m WHERE k = 1;
COMMIT;
