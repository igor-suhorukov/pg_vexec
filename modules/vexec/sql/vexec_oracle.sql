-- SPDX-License-Identifier: Apache-2.0
--
-- The capability oracle for types (pg_vector_executor.md §3.3.2, §3.4):
-- every type has a layout in each format, and every layout it can be held
-- in is one the conversions take it between.  And the batch-source
-- registry (§3.5.1), which no storage module fills before V1.

SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS vexec;
RESET client_min_messages;

SELECT t AS type, (vexec.type_layouts(t)).*
FROM unnest(ARRAY[
	'bool', '"char"', 'int2', 'int4', 'int8', 'oid', 'float4', 'float8', 'money',
	'time', 'date', 'timestamp', 'timestamptz', 'interval',
	'numeric', 'numeric(10,2)', 'numeric(18,0)', 'numeric(19,0)', 'numeric(38,5)',
	'numeric(39,0)', 'numeric(5,-2)', 'numeric(17,-2)', 'numeric(3,10)', 'numeric(2,40)',
	'text', 'varchar(20)', 'char(5)', 'bytea', 'json', 'jsonb', 'xml', 'cstring',
	'uuid', 'name', 'tid', 'point', 'macaddr', 'macaddr8', 'timetz', 'inet',
	'int4[]', 'text[]', 'int4range', 'regclass', 'pg_lsn', 'xid8', 'oid8'
]) t;

-- a domain is held as its base type
CREATE DOMAIN vexec_price AS numeric(12,2) CHECK (VALUE >= 0);
CREATE TYPE vexec_mood AS ENUM ('sad', 'ok', 'happy');
CREATE TYPE vexec_pair AS (a int4, b text);
SELECT t AS type, (vexec.type_layouts(t)).*
FROM unnest(ARRAY['vexec_price', 'vexec_mood', 'vexec_pair', 'vexec_pair[]']) t;
DROP TYPE vexec_pair;
DROP TYPE vexec_mood;
DROP DOMAIN vexec_price;

-- the sources storage modules registered: none in this build
SELECT * FROM vexec.sources();
