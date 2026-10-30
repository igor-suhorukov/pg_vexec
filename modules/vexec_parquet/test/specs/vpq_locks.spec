# Row locks of a table of its own files: shared locks share, an update waits
# for every other transaction's lock -- each update a key's, so FOR KEY SHARE
# makes it wait too, where heap's no-key update would not -- and NOWAIT and
# SKIP LOCKED see a lock

setup
{
	CREATE TABLE things (id int4 PRIMARY KEY, v text) USING vexec_parquet;
	INSERT INTO things VALUES (1, 'one'), (2, 'two'), (3, 'three');
}

teardown
{
	DROP TABLE things;
}

session s1
setup			{ BEGIN; }
step s1share	{ SELECT id FROM things WHERE id = 1 FOR SHARE; }
step s1keyshare	{ SELECT id FROM things WHERE id = 1 FOR KEY SHARE; }
step s1update	{ SELECT id FROM things WHERE id = 1 FOR UPDATE; }
step s1c		{ COMMIT; }

session s2
setup			{ BEGIN; }
step s2share	{ SELECT id FROM things WHERE id = 1 FOR SHARE; }
step s2upd		{ UPDATE things SET v = 'changed' WHERE id = 1; }
step s2nowait	{ SELECT id FROM things WHERE id = 1 FOR UPDATE NOWAIT; }
step s2skip		{ SELECT id FROM things ORDER BY id FOR UPDATE SKIP LOCKED; }
step s2sel		{ SELECT * FROM things ORDER BY id; }
step s2c		{ COMMIT; }

permutation s1share s2share s1c s2c
permutation s1share s2upd s1c s2sel s2c
permutation s1keyshare s2upd s1c s2c
permutation s1update s2nowait s1c s2c
permutation s1update s2skip s1c s2c
