-- vexec_parquet's suite: the extension, its access method, its settings
CREATE EXTENSION vexec_parquet;
SELECT amname, amtype, amhandler FROM pg_am WHERE amname = 'vexec_parquet';
SELECT name, setting, unit, context, vartype FROM pg_settings
WHERE name LIKE 'vexec_parquet.%' ORDER BY name;
-- the secrets are not shown to anyone
SHOW vexec_parquet.s3_secret_access_key;
-- the catalogs are the access method's alone
SELECT c.relname, c.relpersistence, a.amname
FROM pg_class c JOIN pg_am a ON a.oid = c.relam
WHERE c.relnamespace = 'vexec_parquet'::regnamespace AND c.relkind = 'r' ORDER BY 1;
SELECT has_table_privilege('public', 'vexec_parquet.files', 'SELECT') AS public_reads_files,
       has_function_privilege('public', 'vexec_parquet.file_schema(text)', 'EXECUTE') AS public_reads_schemas;
-- a statement's error, the paths it names masked: the tests' files are in
-- an instance's directory, their names hold their writers' transactions
CREATE FUNCTION vpq_error(stmt text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE
	detail text;
BEGIN
	EXECUTE stmt;
	RETURN 'no error';
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS detail = PG_EXCEPTION_DETAIL;
	RETURN regexp_replace(SQLERRM || CASE WHEN detail <> '' THEN ' / ' || detail ELSE '' END,
		'"(/|s3://)[^"]*"', '"<path>"', 'g');
END $$;
-- a role without superuser's rights, for the tests of privileges
CREATE ROLE regress_vpq_user;
GRANT CREATE ON SCHEMA public TO regress_vpq_user;
-- the codecs a setting takes
SET vexec_parquet.codec = 'lz5';
RESET vexec_parquet.codec;
