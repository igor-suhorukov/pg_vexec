-- SPDX-License-Identifier: Apache-2.0
--
-- Batches across Motions as Arrow IPC frames (pg_vector_executor.md §3.10,
-- step A; §5 V7): every type of the layouts' corpus (§6.2), in both formats
-- and with each per-structure setting, sent as a Redistribute sends it --
-- dealt among 1, 3 or 7 segments, a frame each -- and decoded value for
-- value, from aligned and from unaligned bytes; and a malformed frame
-- refused, never read outside its bytes.  vexec_test runs the checks
-- (frame_test.c); the frames across a cluster's Motions, and the vector
-- cdbhash against gp_core's, are the cluster leg's (test/vexec/cluster.sh).

\i sql/vexec_corpus_setup.sql

-- every column, in each format: the shapes its frames carried
SELECT * FROM vexec_test.frames('SELECT * FROM corpus ORDER BY id', 'postgres');
SELECT * FROM vexec_test.frames('SELECT * FROM corpus ORDER BY id', 'arrow', parts => 7);

-- each per-structure setting changed alone, in each format, dealt to 3
SELECT f.format, v.varlena, b.bool, t.temporal, n.numeric,
       count(*) AS columns, sum(e.rows) AS rows, sum(e.nulls) AS nulls,
       max(e.frames) AS frames, max(e.schemas) AS schemas
FROM (VALUES ('postgres'), ('arrow')) f(format),
     (VALUES ('format'), ('datum'), ('view'), ('offsets')) v(varlena),
     (VALUES ('format'), ('byte'), ('bit')) b(bool),
     (VALUES ('format'), ('postgres'), ('arrow')) t(temporal),
     (VALUES ('format'), ('scaled'), ('varlena')) n(numeric),
     LATERAL vexec_test.frames('SELECT * FROM corpus ORDER BY id', f.format,
                               v.varlena, b.bool, t.temporal, n.numeric, 3) e
WHERE (v.varlena = 'format')::int + (b.bool = 'format')::int +
      (t.temporal = 'format')::int + (n.numeric = 'format')::int >= 3
GROUP BY 1, 2, 3, 4, 5 ORDER BY 1, 2, 3, 4, 5;

-- no rows: no frame
SELECT col, rows, frames, schemas FROM vexec_test.frames('SELECT * FROM corpus WHERE false')
WHERE col <= 2;

-- a malformed frame, refused with its SQLSTATE
CREATE FUNCTION frame_try(at int4, value int4, cut int4 DEFAULT NULL) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
	detail text;
BEGIN
	RETURN vexec_test.frame_patch('SELECT i4, tx, n10_2, d FROM corpus ORDER BY id', at, value, cut);
EXCEPTION WHEN OTHERS THEN
	GET STACKED DIAGNOSTICS detail = PG_EXCEPTION_DETAIL;
	RETURN SQLSTATE || ': ' || SQLERRM || coalesce(' -- ' || nullif(detail, ''), '');
END $$;
SELECT frame_try(-100000, 0) AS as_written;
SELECT frame_try(0, 0) AS magic;
SELECT frame_try(4, 1) AS reserved;
SELECT frame_try(8, 1) AS schema_hash;
SELECT frame_try(-100000, 0, 12) AS cut_in_header;
SELECT frame_try(-100000, 0, 40) AS cut_in_metadata;
SELECT frame_try(-100000, 0, -8) AS cut_in_body;
SELECT frame_try(20, 255) AS metadata_length;
DROP FUNCTION frame_try(int4, int4, int4);

DROP TABLE corpus;
DROP FUNCTION corpus_null(int4, int4);
