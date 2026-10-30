# SERIALIZABLE over tables of their own files: write skew -- each
# transaction reads what the other writes -- fails one of them

setup
{
	CREATE TABLE doctors (name text, oncall bool) USING vexec_parquet;
	INSERT INTO doctors VALUES ('alice', true), ('bob', true);
}

teardown
{
	DROP TABLE doctors;
}

session s1
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1r	{ SELECT count(*) FROM doctors WHERE oncall; }
step s1w	{ UPDATE doctors SET oncall = false WHERE name = 'alice'; }
step s1c	{ COMMIT; }

session s2
setup		{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2r	{ SELECT count(*) FROM doctors WHERE oncall; }
step s2w	{ UPDATE doctors SET oncall = false WHERE name = 'bob'; }
step s2c	{ COMMIT; }

permutation s1r s2r s1w s2w s1c s2c
permutation s1r s1w s1c s2r s2w s2c
