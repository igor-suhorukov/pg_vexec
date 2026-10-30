-- Text of another writer's files in databases not UTF-8: made the
-- database's encoding as input would make it -- converted in LATIN1's,
-- checked in SQL_ASCII's -- an empty string among the values, which a
-- conversion hands back as it was given; and the extension's own files of
-- the database's text.  The rows through the plain executor and vexec's.
\getenv data VPQ_TEST_DATA
SELECT :'data' || '/texts.parquet' AS texts_file, current_database() AS regdb \gset
CREATE DATABASE regress_vpq_latin1 ENCODING 'LATIN1' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;
CREATE DATABASE regress_vpq_sqlascii ENCODING 'SQL_ASCII' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;
\c regress_vpq_latin1
SET client_encoding = 'UTF8';
CREATE EXTENSION vexec_parquet;
CREATE TABLE texts (id int8, t text) USING vexec_parquet WITH (location = :'texts_file');
SELECT t, octet_length(t), count(*) FROM texts GROUP BY t ORDER BY t NULLS LAST;
SET vexec.mode = force;
SELECT t, octet_length(t), count(*) FROM texts GROUP BY t ORDER BY t NULLS LAST;
RESET vexec.mode;
CREATE TABLE own_texts (id int8, t text) USING vexec_parquet;
INSERT INTO own_texts SELECT * FROM texts;
SELECT t, octet_length(t), count(*) FROM own_texts GROUP BY t ORDER BY t NULLS LAST;
\c regress_vpq_sqlascii
SET client_encoding = 'UTF8';
CREATE EXTENSION vexec_parquet;
CREATE TABLE texts (id int8, t text) USING vexec_parquet WITH (location = :'texts_file');
SELECT t, octet_length(t), count(*) FROM texts GROUP BY t ORDER BY t NULLS LAST;
SET vexec.mode = force;
SELECT t, octet_length(t), count(*) FROM texts GROUP BY t ORDER BY t NULLS LAST;
RESET vexec.mode;
CREATE TABLE own_texts (id int8, t text) USING vexec_parquet;
INSERT INTO own_texts SELECT * FROM texts;
SELECT t, octet_length(t), count(*) FROM own_texts GROUP BY t ORDER BY t NULLS LAST;
\c :regdb
DROP DATABASE regress_vpq_latin1;
DROP DATABASE regress_vpq_sqlascii;
