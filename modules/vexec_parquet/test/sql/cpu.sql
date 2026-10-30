-- pg_vector_executor.md §3.37: the CPU's reads of a table's files --
-- Arrow's large blocks from the engine's pool (PQ8), another writer's text
-- checked a batch at once (PQ9) and once a column chunk (PQ14), a scan's
-- reader over a footer of its columns alone (PQ11), row groups cut into
-- ranges of rows where a parallel scan keeps few (PQ12), a later step's
-- columns read by the pages its batches need (PQ13) -- each answer the same
-- as with them off, and vexec_parquet.scan_stats() counting each.
\getenv data VPQ_TEST_DATA
SET vexec_parquet.sync = off;
-- the counters are the backend's: its scans serial but where a parallel one is asked for
SET max_parallel_workers_per_gather = 0;
-- rows in four row groups of 150,000, pages of 1 kB: k orders them, so that
-- a quals' range of k keeps some row groups; s a short text, w a wider one
CREATE TABLE cpu_own (k int8, s text, w text, n int4) USING vexec_parquet
	WITH (row_group_rows = 150000, page_bytes = 1024, codec = 'snappy');
INSERT INTO cpu_own SELECT i, 's' || (i % 1000), repeat(chr(97 + i % 26), 20 + i % 30) || i, i % 97
FROM generate_series(1, 600000) i;
SELECT regexp_replace(min(path), '/[^/]+$', '') AS cpu_dir FROM vexec_parquet.files('cpu_own') \gset
-- the same files read as another's: their text checked, their footers' facts and indexes shared
CREATE TABLE cpu_ext (k int8, s text, w text, n int4) USING vexec_parquet WITH (location = :'cpu_dir');
SET vexec.mode = force;

-- PQ8: the later row groups' large blocks the earlier ones', kept
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(length(w)) FROM cpu_ext;
SELECT pool_mapped > 0 AS mapped, pool_reused > 0 AS reused FROM vexec_parquet.scan_stats();
-- given back as the transaction ends
SELECT pool_kept_bytes FROM vexec_parquet.scan_stats();
-- none kept: each large block given back as it is freed, the answers the same
SET vexec_parquet.read_memory_mb = 0;
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(length(w)) FROM cpu_ext;
SELECT pool_reused FROM vexec_parquet.scan_stats();
RESET vexec_parquet.read_memory_mb;

-- PQ9: another writer's text checked a batch at once, own files' not at all
-- (PQ14's marks off: each read checks it)
SET vexec_parquet.enable_text_marks = off;
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*) FROM cpu_ext WHERE w LIKE '%z%';
SELECT text_batches > 0 AS checked_at_once, text_values FROM vexec_parquet.scan_stats();
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*) FROM cpu_own WHERE w LIKE '%z%';
SELECT text_batches, text_values FROM vexec_parquet.scan_stats();
-- a character cut between two values is no value's whole: the value's own error, as a value at a time raised it
SELECT :'data' || '/cut_utf8.parquet' AS cut_file, :'data' || '/nul_utf8.parquet' AS nul_file \gset
CREATE TABLE cpu_cut (id int8, t text) USING vexec_parquet WITH (location = :'cut_file');
SELECT count(*) FROM cpu_cut WHERE t <> '';
SELECT count(*) FROM cpu_cut WHERE id = 1 AND t <> '';
CREATE TABLE cpu_nul (id int8, t text) USING vexec_parquet WITH (location = :'nul_file');
SELECT count(*) FROM cpu_nul WHERE t <> '';
SET vexec.mode = off;
SELECT count(*) FROM cpu_cut WHERE t <> '';
SET vexec.mode = force;
RESET vexec_parquet.enable_text_marks;

-- PQ13: a later step's column read by the pages its batches need -- k's
-- quals first, w then for the batches with a row left
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(length(w)), min(w), max(w) FROM cpu_ext WHERE k % 50000 < 3;
SELECT step_batches > 0 AS by_pages, pages_passed > 0 AS passed, pages_decoded > 0 AS decoded FROM vexec_parquet.scan_stats();
SET vexec_parquet.enable_step_pages = off;
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(length(w)), min(w), max(w) FROM cpu_ext WHERE k % 50000 < 3;
SELECT step_batches, pages_passed FROM vexec_parquet.scan_stats();
RESET vexec_parquet.enable_step_pages;
-- every batch asking: the same rows as the chunk read whole
SELECT count(*), sum(length(w)), count(DISTINCT s) FROM cpu_ext WHERE k > 10;

-- PQ12: a parallel scan whose quals keep two row groups of 150,000 rows
-- reads them as ranges -- its leader alone here, no worker to be had
SET max_parallel_workers_per_gather = 2;
SET max_parallel_workers = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(k), sum(length(w)), count(DISTINCT n) FROM cpu_ext WHERE k > 300000;
SELECT units_cut FROM vexec_parquet.scan_stats();
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(k), sum(length(w)), count(DISTINCT n) FROM cpu_own WHERE k > 300000 AND k % 7 = 0;
SELECT units_cut FROM vexec_parquet.scan_stats();
SET vexec_parquet.enable_row_ranges = off;
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(k), sum(length(w)), count(DISTINCT n) FROM cpu_ext WHERE k > 300000;
SELECT count(*), sum(k), sum(length(w)), count(DISTINCT n) FROM cpu_own WHERE k > 300000 AND k % 7 = 0;
SELECT units_cut FROM vexec_parquet.scan_stats();
RESET vexec_parquet.enable_row_ranges;
-- no quals: aggregate() answers each unit from its statistics, none cut
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*) FROM cpu_ext;
SELECT units_cut FROM vexec_parquet.scan_stats();
SET max_parallel_workers_per_gather = 0;
RESET max_parallel_workers;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

-- PQ11: a process that finds the files' facts and footers' indexes in shared
-- memory reads their row groups through footers of its scan's columns alone
\c
SET vexec.mode = force;
SET max_parallel_workers_per_gather = 0;
SELECT * FROM vexec_parquet.read_stats(true) \gset
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(length(w)), count(DISTINCT s) FROM cpu_ext WHERE k > 10;
SELECT footers_projected > 0 AS projected, projections_refused FROM vexec_parquet.scan_stats();
SELECT footers_read FROM vexec_parquet.read_stats();
-- with them off, the footers read whole and the same answer
\c
SET vexec.mode = force;
SET max_parallel_workers_per_gather = 0;
SET vexec_parquet.enable_projected_footers = off;
SELECT * FROM vexec_parquet.read_stats(true) \gset
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(length(w)), count(DISTINCT s) FROM cpu_ext WHERE k > 10;
SELECT footers_projected FROM vexec_parquet.scan_stats();
SELECT footers_read > 0 AS read_whole FROM vexec_parquet.read_stats();
RESET vexec_parquet.enable_projected_footers;
-- pyarrow's files, whose footers' Arrow schema names their columns' types,
-- which a footer of some columns leaves out: read through one where those
-- columns' types are what Arrow makes of their Parquet types -- not a large
-- string's nor a dictionary's, whose files are read through their whole
-- footers -- each answer the same as the whole footer's
SELECT :'data' || '/all.parquet' AS all_file \gset
CREATE TABLE cpu_arrow (id int8, s text, f64 float8, d32 date, dec9 numeric(9,2)) USING vexec_parquet
	WITH (location = :'all_file');
CREATE TABLE cpu_large (id int8, ls text) USING vexec_parquet WITH (location = :'all_file');
CREATE TABLE cpu_dict (id int8, dict text) USING vexec_parquet WITH (location = :'all_file');
SET vexec.mode = off;
SELECT count(*), sum(id), count(s), max(s), sum(f64) FILTER (WHERE f64 = f64), max(d32), sum(dec9) FROM cpu_arrow;
SELECT count(*), sum(length(ls)) FROM cpu_large WHERE ls LIKE 'large 9%';
SELECT dict, count(*) FROM cpu_dict GROUP BY 1 ORDER BY 1 NULLS LAST;
\c
SET vexec.mode = force;
SET max_parallel_workers_per_gather = 0;
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(id), count(s), max(s), sum(f64) FILTER (WHERE f64 = f64), max(d32), sum(dec9) FROM cpu_arrow;
SELECT footers_projected > 0 AS projected, projections_refused FROM vexec_parquet.scan_stats();
\c
SET vexec.mode = force;
SET max_parallel_workers_per_gather = 0;
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(length(ls)) FROM cpu_large WHERE ls LIKE 'large 9%';
SELECT footers_projected, projections_refused > 0 AS refused FROM vexec_parquet.scan_stats();
\c
SET vexec.mode = force;
SET max_parallel_workers_per_gather = 0;
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT dict, count(*) FROM cpu_dict GROUP BY 1 ORDER BY 1 NULLS LAST;
SELECT footers_projected, projections_refused > 0 AS refused FROM vexec_parquet.scan_stats();

-- PQ14: a column chunk of another writer's text whose every value a scan
-- checked, its row group read whole, marked so in the file's facts in the
-- shared footer cache -- here new files' three row groups -- and the next
-- process, which takes the facts from there, checks none of them
SET vexec_parquet.sync = off;
CREATE TABLE cpu_own2 (k int8, w text) USING vexec_parquet WITH (row_group_rows = 10000, codec = 'snappy');
INSERT INTO cpu_own2 SELECT i, repeat(chr(97 + i % 26), 5 + i % 30) || i FROM generate_series(1, 30000) i;
SELECT regexp_replace(min(path), '/[^/]+$', '') AS cpu_dir2 FROM vexec_parquet.files('cpu_own2') \gset
CREATE TABLE cpu_ext2 (k int8, w text) USING vexec_parquet WITH (location = :'cpu_dir2');
\c
SET vexec.mode = force;
SET max_parallel_workers_per_gather = 0;
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(length(w)) FROM cpu_ext2 WHERE w LIKE '%z%';
SELECT text_batches > 0 AS checked, text_known, text_marked FROM vexec_parquet.scan_stats();
\c
SET vexec.mode = force;
SET max_parallel_workers_per_gather = 0;
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(length(w)) FROM cpu_ext2 WHERE w LIKE '%z%';
SELECT text_batches, text_values FROM vexec_parquet.scan_stats();
SELECT text_known > 0 AS known, text_marked FROM vexec_parquet.scan_stats();
-- with them off, checked again, the same answer
SET vexec_parquet.enable_text_marks = off;
SELECT * FROM vexec_parquet.scan_stats(true) \gset
SELECT count(*), sum(length(w)) FROM cpu_ext2 WHERE w LIKE '%z%';
SELECT text_batches > 0 AS checked, text_known FROM vexec_parquet.scan_stats();
RESET vexec_parquet.enable_text_marks;
-- a chunk with a value that fails is never marked: the next process's error the same
SELECT count(*) FROM cpu_cut WHERE t <> '';
SELECT count(*) FROM cpu_nul WHERE t <> '';

DROP TABLE cpu_ext, cpu_cut, cpu_nul, cpu_arrow, cpu_large, cpu_dict, cpu_ext2;
DROP TABLE cpu_own, cpu_own2;
