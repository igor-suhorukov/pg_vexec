# Concurrent updates and deletes of a row of a table of its own files: the
# second writer waits for the first; READ COMMITTED's goes on with the
# row's newest version, REPEATABLE READ's fails, as heap's

setup
{
	CREATE TABLE accounts (id int4 PRIMARY KEY, balance int4) USING vexec_parquet;
	INSERT INTO accounts VALUES (1, 100), (2, 200);
}

teardown
{
	DROP TABLE accounts;
}

session s1
setup		{ BEGIN; }
step s1u	{ UPDATE accounts SET balance = balance + 10 WHERE id = 1; }
step s1d	{ DELETE FROM accounts WHERE id = 1; }
step s1c	{ COMMIT; }
step s1a	{ ROLLBACK; }

session s2
setup		{ BEGIN; }
step s2rr	{ SET TRANSACTION ISOLATION LEVEL REPEATABLE READ; }
step s2u	{ UPDATE accounts SET balance = balance * 2 WHERE id = 1 RETURNING balance; }
step s2s	{ SELECT * FROM accounts ORDER BY id; }
step s2c	{ COMMIT; }

permutation s1u s2u s1c s2s s2c
permutation s1u s2u s1a s2s s2c
permutation s1d s2u s1c s2s s2c
permutation s2rr s2s s1u s1c s2u s2c
permutation s2rr s2s s1u s2u s1a s2s s2c
