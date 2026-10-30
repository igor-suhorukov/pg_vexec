# Upserts racing for a key, and a foreign key's parent deleted while a child
# that names it is inserted, in tables of their own files

setup
{
	CREATE TABLE kv (k int4 PRIMARY KEY, v text) USING vexec_parquet;
	CREATE TABLE parent (id int4 PRIMARY KEY) USING vexec_parquet;
	CREATE TABLE child (pid int4 REFERENCES parent) USING vexec_parquet;
	INSERT INTO parent VALUES (1), (2);
}

teardown
{
	DROP TABLE kv, child, parent;
}

session s1
setup		{ BEGIN; }
step s1ins	{ INSERT INTO kv VALUES (1, 'a') ON CONFLICT (k) DO UPDATE SET v = kv.v || excluded.v; }
step s1child	{ INSERT INTO child VALUES (1); }
step s1c	{ COMMIT; }
step s1a	{ ROLLBACK; }

session s2
step s2ins	{ INSERT INTO kv VALUES (1, 'b') ON CONFLICT (k) DO UPDATE SET v = kv.v || excluded.v RETURNING v, xmax = 0 AS inserted; }
step s2nothing	{ INSERT INTO kv VALUES (1, 'c') ON CONFLICT DO NOTHING RETURNING v; }
step s2delp	{ DELETE FROM parent WHERE id = 1; }
step s2sel	{ SELECT * FROM kv ORDER BY k; }

permutation s1ins s2ins s1c s2sel
permutation s1ins s2ins s1a s2sel
permutation s1ins s2nothing s1c s2sel
permutation s1child s2delp s1c
permutation s1child s2delp s1a
