# vexec_parquet

A PostgreSQL 19 table access method, `vexec_parquet`, of Parquet files read
and written by [Apache Arrow C++](https://arrow.apache.org/docs/cpp/) alone
(26.0, its Parquet and S3 libraries), on the local filesystem or S3, with
[vexec](../../README.md)'s batch source and batch sink.  It serves vexec the
same internal contracts as ch_table_engines' `parquet` access method does
(`include/vexec_source.h` at minor 12, `include/vexec_sink.h` at minor 0),
without ClickHouse: no engine library, no ClickHouse code, no ClickHouse
setting.

Two kinds of tables:

- **A table of its own files** (no `location`), read and written: INSERT,
  COPY, UPDATE, DELETE, MERGE, ON CONFLICT, TRUNCATE, indexes, constraints,
  foreign keys, triggers, savepoints, prepared transactions, SERIALIZABLE,
  as heap's.  Its rows are written as Parquet files under a storage root of
  the local filesystem or S3.
- **A table that reads files** (`location`): a file, a directory (every
  `*.parquet` under it), or a glob, local or S3, written by any writer:
  read-only, its columns matched to the files' by name.

```sql
CREATE EXTENSION vexec_parquet;

CREATE TABLE events (id int8, at timestamptz, kind text, amount numeric(12,2))
	USING vexec_parquet WITH (codec = 'zstd', row_group_rows = 131072);
INSERT INTO events SELECT ...;

-- a table over another writer's files: its columns, as CREATE TABLE names them
SELECT vexec_parquet.column_list('s3://bucket/events/');
CREATE TABLE events_lake (id int8, at timestamptz, kind text)
	USING vexec_parquet WITH (location = 's3://bucket/events/*.parquet');
```

The library must be preloaded, with vexec where vexec is to read its tables:
`shared_preload_libraries = 'vexec,vexec_parquet'`.  The table options
reach no parser of PostgreSQL's (19 has none for a table access method's
options), and the end of a statement writes its buffered rows: both are the
library's hooks.

## How vexec reads and writes it

vexec finds a table's batch source and sink by its access method, in its
registries, which a library fills while the postmaster preloads it
(`vexec_register_source()`, `vexec_register_sink()`).  ch_table_engines'
`parquet` access method fills them from `src/vexec.c`, over ClickHouse's
Parquet reader; this module fills them from its own `src/vexec.c`, over
Arrow's:

| The contract | ch_table_engines' `parquet` | vexec_parquet |
|---|---|---|
| begin/next/retain/release/rescan/end | ClickHouse's columns cut into batches | Arrow's arrays cut into batches of `max_rows`, each column pointing into Arrow's buffers where vexec's layout is Arrow's (FIXED, SCALED, OFFSETS, VIEW), the rest converted |
| units | ClickHouse's parts and granules | row groups, from the listing a scan takes as it begins; a parallel scan's shared by an atomic counter |
| `estimate`, `estimate_ex` | parts' rows and bytes | row groups' rows and the spec's columns' compressed bytes, in the units its quals keep |
| `aggregate` | part statistics | count(*), count, min and max from a row group's statistics, where exact and no row of it is deleted |
| `set_keys`, `set_join_keys` | granule pruning | row groups whose min/max no key meets passed over |
| steps (`next_rest`) | columns in steps | a later step's columns read for a batch only where a row is left |
| `fetch_columns`, `estimate_ex`'s `fetch` | a fetch's columns decoded alone (VC16), a row read where it lies (VC34) | a fetch's named columns alone, each from the data page holding its row -- found by the offset index, or by the pages' headers -- that page kept for the next fetch in it; priced as a page of each column |
| `next_unit`, `read_unit`, `unit_tids` | the GPU tier's units | a local file's row group as `VEXEC_UNIT_PARQUET`, its deleted rows as bits, where the GPU's executor makes its values as the CPU's reader does (below) |
| the sink | VecInsert into ClickHouse's columns | VecInsert into the write buffer by column, in its files' layouts |

Each row group is pruned by its statistics against the spec's quals:
comparisons of a column with a constant (commuted too, and across the types
of a btree family), `= ANY`, `IS [NOT] NULL`, boolean columns, AND and OR;
text by its bytes under a deterministic collation, and ranges of text under
C's only.  Without vexec the plain scan reads every row group.

### The GPU tier

With vexec_gpu preloaded and `vexec.gpu` on, VecAgg's device strategy and a
scan's own device read take a scan's row groups from `next_unit()`, and
vexec_gpu's executor reads them from their files with libcudf; the rows the
snapshot does not see go with each as its deleted bits, and its TIDs come
from its rows' places (`unit_tids()`).  A row group goes to the device only
where it is a local file's and the executor makes each of its columns'
values as `src/read.c` does; any other is the CPU's, read by the next
batches:

- each column of the file's own type of the table's -- an integer widened
  where every value of its type is one of the column's (a signed one no
  wider, an unsigned one narrower), no dictionary-typed strings -- and a
  decimal at the column's scale;
- text from elsewhere only in a UTF-8 database and of no varchar's length:
  the CPU's reader makes it the database's encoding, or cuts it to the
  length, as input does;
- dates and timestamps the row group's statistics show finite -- the
  executor keeps no infinity -- and, from elsewhere, within PostgreSQL's
  range, of seconds, milliseconds or microseconds; and integers of days or
  of seconds since 1970 declared dates and timestamps (below), their
  statistics within PostgreSQL's range;
- no chunk under snappy or LZ4 whose first data page is of version 2, as
  its header says (Arrow's writer counts every data page as of version 1 in
  the footer): libcudf 25.06 never finished reading pyarrow's
  dictionary-encoded ones under snappy, holding the device until its
  executor was killed, and fails to decompress LZ4's.

### What a scan reads of a footer

A process's first query of a table reads no footer it need not
(pg_vector_executor.md §3.34, PQ1 to PQ4):

- **An own file's facts in its catalog row.**  As a file is written its
  facts -- its columns as Arrow's reader makes them, their names and field
  ids, its codec, its data pages of version 1, which the writer makes -- go
  into its `vexec_parquet.files` row beside its statistics, and a scan opens
  the file of them: planning and pruning read the catalog's statistics, a
  device unit is handed from the facts and the statistics of its dates and
  timestamps, and only a unit read on the CPU, or a fetch, reads the footer
  -- as Arrow's reader opens on the file.  `vexec_parquet.verify_facts()`
  checks them against the footer read again.  A file written before them
  is read by its footer.
- **Arrow's reader opened as a unit is first read on the CPU**, not as its
  file is opened.
- **An own table's listing kept for its snapshot**: the planner's estimates
  and the scan's start read the catalog once where their snapshots see the
  same rows of it -- of the transaction, its command, the same transactions
  in progress -- and the process wrote none in between; a column's
  statistics are decoded as they are first asked for.
- **External files' facts in shared memory**, every chunk's statistics among
  them: the first process that reads a file's footer keeps them under its
  location, size and modification time, and the next plans, prunes and
  hands device units without reading it.  `vexec_parquet.footer_cache_size`
  of the server's shared memory, the oldest entries going first.
- **A footer of the scan's columns alone** (pg_vector_executor.md §3.37,
  PQ11): beside an external file's facts its footer's index -- where each
  schema element, row group and column chunk lies in the footer's bytes,
  walked once by the process that reads the footer -- so that the next
  process opens Arrow's reader for a unit on the CPU over a footer of the
  file's row groups and the scan's columns, made of the footer's bytes by
  the index, rather than parsing the whole footer: each participant of a
  parallel scan of ClickBench's own file of 226 row groups by 105 columns
  parsed 2.4 MB of it, about 16 ms.  The key-value metadata is left out of
  it, an Arrow schema there naming every column: a column whose type Arrow
  makes otherwise of its Parquet type alone -- a large string, a
  dictionary -- has its file read through the footer whole.  So is a file
  of a nested schema, an encrypted one, or one whose index the cache does
  not hold.

### How a scan reads on the CPU

pg_vector_executor.md §3.37 (PQ8, PQ9, PQ12 to PQ14):

- **Arrow's buffers from a pool of the engine's own:** a block of 256 kB or
  more is mapped, grown and shrunk by `mremap()` -- a column chunk's
  strings grow by doubling as Parquet's decoder appends to them, and shrink
  to fit as the builder finishes, which glibc's allocator made a copy into
  new pages each time -- and a freed one is kept for the process's next
  reads, `vexec_parquet.read_memory_mb` of them, all given back as the
  transaction ends.
- **Another writer's text checked a batch at once:** a batch's strings, one
  run of bytes in Arrow's string layout, checked as the database's UTF-8
  once, each one's first byte none of a character's continuation bytes --
  which makes each string whole -- and where that fails each string alone,
  for its own error.
- **And once a column chunk:** a scan that checked every value of a column
  chunk, its row group read whole, marks the chunk so in the file's facts
  in the shared footer cache (`vexec_parquet.footer_cache_size`), and a
  later reader -- the next process, which takes the facts from there --
  passes its check over.  A chunk with a value that fails is never marked;
  the mark goes with the file's facts, its path, size and modification
  time.
- **A later step's columns by the pages its batches need:** where vexec
  reads a scan's columns in steps, a later step's are read forward through
  the unit by one page reader a column -- its dictionary decoded once, the
  data pages wholly before a batch with a row left passed over
  undecompressed, a batch's rows read as its own arrays -- not the column
  chunk decoded whole as its first batch asks.  A local file's columns that
  are not repeated, nor read as dictionaries, nor INT96; else whole.
- **Row groups cut into ranges of rows where few are kept:** a parallel
  scan whose quals keep fewer row groups than twice the participants a
  gather may have reads them as ranges of 64K rows or more, each
  participant decoding a range's pages alone -- all of them cut alike, by
  the listing the leader hands its workers, the quals and the settings.
  Not where a device may read the scan's units (`vexec.gpu` on, with device
  units), whose units stay row groups; not where the quals keep every row
  group, whose units `aggregate()` may answer from their statistics.

## Options and settings

| Option | Of | |
|---|---|---|
| `location` | a table that reads files | a path, a `file://` or an `s3://` URI: a file, a directory, or a glob (`*`, `?`, `[...]`, `**`); set again by ALTER TABLE, its files checked as CREATE TABLE checks them; never reset |
| `storage` | a table of its own files | the root of its files: a directory or an `s3://` URI; changed only while the table has no file |
| `codec` | " | `zstd` (default), `lz4`, `snappy`, `gzip`, `brotli`, `none` |
| `compression_level` | " | the codec's level |
| `row_group_rows` | " | the rows of a row group, 1024 to 67108864 (131072) |
| `page_bytes` | " | a data page's uncompressed bytes, 1024 to 67108864 (1048576): a fetch by TID reads one page of each column it reads |

| Setting | |
|---|---|
| `vexec_parquet.storage` | the root of the files of tables without `storage` (superuser): empty, each database's own directory |
| `vexec_parquet.codec`, `.row_group_rows` | the defaults of the options |
| `vexec_parquet.buffer_rows`, `.buffer_mb` | the rows and memory a table's buffered rows take before they are written as a file (1048576 rows, 256 MB) |
| `vexec_parquet.sync` | whether a local file is synced before its row commits (on) |
| `vexec_parquet.enable_pruning` | whether a scan passes over row groups by their statistics (on) |
| `vexec_parquet.enable_device_units` | whether vexec's GPU tier may read a scan's local files itself (on) |
| `vexec_parquet.enable_point_reads` | whether a fetch by TID reads each column's data page holding its row alone (on) |
| `vexec_parquet.enable_fetch_columns` | whether a fetch by TID reads only the columns vexec names for its slot -- a bounded sort's late columns (on) |
| `vexec_parquet.fetch_threads` | the threads a fetch by TID reads its columns' data pages on, at once, the engine's own; 1, a page at a time (16) |
| `vexec_parquet.footer_cache_size` | the shared memory keeping external files' facts, so that a process reads no footer another has read (64 MB; 0 none; at server start) |
| `vexec_parquet.enable_footer_cache` | whether a scan reads and fills it (on) |
| `vexec_parquet.enable_projected_footers` | whether a scan reads an external file's row groups through a footer of its columns alone, of the footer's index in the shared footer cache (on) |
| `vexec_parquet.read_memory_mb` | the memory of Arrow's freed large buffers a process keeps for its next reads, given back as the transaction ends; 0 keeps none (256 MB) |
| `vexec_parquet.enable_step_pages` | whether a scan's later steps decode only the data pages their batches need (on) |
| `vexec_parquet.enable_row_ranges` | whether a parallel scan that keeps few row groups reads them as ranges of rows, where no device may read its units (on) |
| `vexec_parquet.enable_text_marks` | whether another writer's text a scan found UTF-8 whole is marked so in the shared footer cache, a column chunk at a time, and not checked again (on) |
| `vexec_parquet.s3_endpoint`, `.s3_region`, `.s3_scheme`, `.s3_virtual_addressing` | S3's endpoint (or `AWS_ENDPOINT_URL`), region (or `AWS_REGION`), `https`, path-style addressing |
| `vexec_parquet.s3_access_key_id`, `.s3_secret_access_key`, `.s3_session_token`, `.s3_anonymous` | credentials, superuser-only, the secrets never shown; empty for the AWS SDK's chain |
| `vexec_parquet.s3_io_threads`, `.s3_prefetch`, `.s3_connect_timeout`, `.s3_request_timeout` | Arrow's I/O threads, a row group's column chunks fetched as coalesced concurrent ranges, timeouts |

An `s3://` URI's query may name its own `region`, `endpoint_override` and
`scheme`; credentials in a URI are refused.  In an `s3://` URI `?` begins
the query, never a wildcard: a glob there uses `*` and `[...]`.

## Tables of their own files

- **Writes.**  A statement's rows go into a per-backend write buffer, one per
  table, and become a file when the statement ends, before a scan whose
  snapshot would see them, and at the buffer's limits.  A file's rows take
  consecutive row ids; a row's TID is its row id (291 to a block number).
  A local file is written to a temporary name, synced, renamed and its
  directory synced before the row naming it commits.
- **MVCC by catalogs.**  The extension's heap catalogs say what exists:
  `vexec_parquet.files` (a file, its first row id, its rows, its row
  groups' statistics), `vexec_parquet.tombstones` (a deleted, updated or
  locked row), `vexec_parquet.storages` (each storage's next row id and
  file), with UNLOGGED twins for UNLOGGED and temporary tables.  A file's
  rows are visible where its catalog row is; a row is deleted where a
  visible tombstone names it.  An update is a tombstone that names the new
  version's row id and the new row; a row lock is a tombstone of the
  running transaction, deleted before it commits.  Writers of one row
  serialize on a tuple lock and wait for each other's transactions as
  heap's do: EvalPlanQual follows an update to the newest version,
  REPEATABLE READ fails with a serialization error, SERIALIZABLE takes
  relation-level predicate locks.  Every update takes the strongest row
  lock: a `FOR KEY SHARE` lock makes it wait, as it would not heap's no-key
  update.
- **System columns.**  `ctid` the row id's TID; `xmin` the transaction
  that wrote the row's file (frozen for a table that reads files); `xmax` the
  deleting, updating or locking transaction, as heap's shows it, so that an
  upsert's `RETURNING xmax = 0` tells an insert from an update; `cmin` and
  `cmax` 0.
- **Storage.**  `$PGDATA/base/<db>/vexec_parquet/<relfilenumber>` (or the
  tablespace's), or `<root>/<system identifier>/<db>/<spc>_<relfilenumber>`
  under a `storage` root; each file `<file id>_<FullTransactionId>.parquet`;
  a temporary table's directory `_t<backend>`.  TRUNCATE, VACUUM FULL,
  CLUSTER and a rewriting ALTER TABLE make a new directory and drop the old
  one when the transaction commits; an aborted transaction's files go when
  it aborts, and VACUUM deletes a file no catalog row names whose writer
  has ended -- a crash's.
- **Columns by field id.**  A file's columns are named by the attributes'
  names and carry their attribute numbers as Parquet field ids, so that a
  column renamed, added (its default as the missing value of older files),
  dropped or retyped (by a rewrite) reads older files right.

| PostgreSQL | Arrow, in the files a table writes |
|---|---|
| bool, int2, int4, int8, float4, float8 | bool, int16, int32, int64, float, double |
| oid, "char" | uint32, int8 |
| date, time, timestamp, timestamptz | date32, time64[us], timestamp[us], timestamp[us, UTC] |
| numeric(p,s), p ≤ 38, 0 ≤ s ≤ p | decimal128(p,s); a file with a NaN keeps the column's binary images |
| text, varchar, bpchar | string; binary in a database of another encoding |
| bytea | binary |
| uuid, json | Arrow's `arrow.uuid` and `arrow.json` extension types |
| any other fixed-length type (interval, timetz, money, name, macaddr, point, an enum, ...) | fixed_size_binary(its length): its bytes |
| any other variable-length type (numeric otherwise, jsonb, arrays, composites, ranges, inet, ...) | binary: its varlena image |
| a domain | its base type's |

The images are this access method's to read, in this database: an enum's
or a composite's values name its OIDs.  A table that reads files reads
another writer's types: every integer width, signed and unsigned (to the
column's range), half and single floats, decimal32 to 256, every time unit,
dates of both widths, string, large string, string view and dictionary
columns, fixed_size_binary(16) as uuid, any string column through the
column type's input function, any binary column through its receive
function, as COPY BINARY reads a value (pg_vector_executor.md §3.39, PQ15),
and lists and maps of flat children as arrays, hstore and jsonb (§3.40,
below); any other nested column is refused.  A timestamp
without a time zone read into timestamptz is taken as UTC.  An integer
column declared `date` counts days since 1970, and one declared `timestamp`
or `timestamptz` seconds since 1970 -- ClickHouse's Date and DateTime, as
ClickBench's own Parquet file holds EventDate and EventTime -- read so by the
CPU and by the GPU's executor, a value past PostgreSQL's range an error, and
their statistics prune as a date's and a timestamp's do (pg_vector_executor.md
§3.35, PQ6):

```sql
CREATE TABLE hits (..., eventtime timestamp NOT NULL, eventdate date NOT NULL, ...)
  USING vexec_parquet WITH (location = '/data/hits.parquet');   -- int64 seconds, uint16 days
```

### GeoParquet: PostGIS's geometry

A column of WKB declared PostGIS's `geometry` or `geography` is read through
the type's receive function, with the column's typmod (pg_vector_executor.md
§3.39, PQ15-PQ18).  A file column is one of geometries where GeoParquet's
metadata names it -- the footer's key `geo`, of every version, the only sign
of one in GeoParquet 1.x's files ("V1", DuckDB's default) -- or where its
type is Parquet's GEOMETRY or GEOGRAPHY (GeoParquet 2.0's, and DuckDB's
"BOTH" and "V2"); the key is kept in the file's facts with the types' CRSs
and each row group's GeospatialStatistics, so that no process reads a
footer for them.  For `geometry`:

- **The SRID is the file's CRS's**: the logical type's where it writes one,
  else GeoParquet's `crs` of the column, else -- neither names one --
  OGC:CRS84, both formats' default, SRID 4326.  EPSG's codes (`EPSG:<n>`, a
  PROJJSON's `id`), Parquet's `srid:<n>` and OGC:CRS84 are known; GeoParquet's
  `null`, and any other CRS, are unknown.
- **A typmod with an SRID** reads no file of another known SRID: an error as
  the file is opened, at CREATE TABLE for its first file.  **A column without
  one** gives the file's SRID to every value of WKB without an SRID of its
  own; an EWKB's own SRID stays.  An unknown CRS changes nothing: the typmod's
  SRID, or 0, as COPY BINARY gives.
- **A value PostGIS refuses** -- another geometry type than the typmod's, a
  line of one point, WKB that does not parse, an EWKB of another SRID -- is
  PostGIS's own error, as COPY BINARY's is, with the column and the file it
  was read from.
- **Spherical edges** (Parquet's GEOGRAPHY, GeoParquet's `edges: "spherical"`)
  are not a geometry's: declared `geometry` they are an error; `geography`
  reads them.
- **`&&` against a constant prunes row groups** where vexec reads the table
  (`vexec.mode` auto or force): by the row group's GeospatialStatistics, else
  by GeoParquet's `bbox` of the file's column -- V1's, the whole file's.  Both
  boxes are rounded outward to floats, as PostGIS's `&&` compares them.
- `vexec_parquet.columns()` and `column_list()` declare such a column
  `geometry`, of GeoParquet's `geometry_types` and the SRID where they are
  known, wherever PostGIS is installed; `bytea` for spherical edges.

```sql
CREATE EXTENSION postgis;
SELECT vexec_parquet.column_list('/data/ways/');   -- ..., linestring geometry(LineString,4326), ...
CREATE TABLE ways (id int8, h33 int2, linestring geometry(LineString, 4326))
  USING vexec_parquet WITH (location = '/data/ways/');
SELECT count(*) FROM ways WHERE linestring && ST_MakeEnvelope(100.50, 13.70, 100.52, 13.72, 4326);
```

Without PostGIS a column of WKB is `bytea`, or any type with a receive
function of its own.

### Lists and maps

A list (and a large list) of flat elements is read as an array of the
declared element type, one-dimensional, of lower bound 1: each element as
a column of that type reads it -- `bigint[]` of int64's, `smallint[]` or
`"char"[]` of int8's, `text[]` or `varchar(n)[]` of strings (their encoding
checked, a typmod applied), a string element of another type through that
type's input function -- its NULLs kept, an empty list an empty array.  A map
of strings to strings is read as `hstore`, made by hstore's own constructor,
`hstore(text[], text[])`, which keeps one of a key given twice; any map of
flat keys and values -- or of strings, declared so -- as a `jsonb` object:
its keys as text, its values JSON's own (numbers, booleans, strings, null;
dates and times in ISO 8601, a float's NaN and infinities as text, as
`to_jsonb()` makes them), a key given twice its last value (§3.40, PQ19 and
PQ20).  A list of lists or of structs, and a map of nested values, are
refused.  The file's children's types are kept in its facts with the rest.

```sql
CREATE EXTENSION hstore;
SELECT vexec_parquet.column_list('/data/ways/');  -- ..., tags hstore, "pointIdxs" bigint[], "h38Indexes" integer[], ...
CREATE TABLE ways (id int8, tags hstore, "pointIdxs" int8[]) USING vexec_parquet WITH (location = '/data/ways/');
SELECT count(*) FROM ways WHERE tags -> 'highway' = 'residential';
```

Their values are made a row at a time, and their operators -- hstore's
`->` and `?`, `cardinality()`, `= ANY` -- run as PostgreSQL runs them, in
vexec's batches too: hstore's functions are not leakproof, so vexec runs a
qual of them lazily.  The plain scan makes every column of a row, a
nested one among them; `vexec.mode` auto or force reads only those a query
asks for.

## Functions

| Function | |
|---|---|
| `vexec_parquet.columns(location)` | the columns of a location's first file, as a table would declare them -- a GeoParquet column `geometry(type,srid)` where PostGIS is installed, a list an array of its elements' type, a map of strings `hstore` where hstore is installed, any other map `jsonb` -- and their Arrow types |
| `vexec_parquet.column_list(location)` | the same, as CREATE TABLE's column list |
| `vexec_parquet.files(regclass)` | the files a scan of the table now reads: id, path, rows, bytes, row groups, first row id |
| `vexec_parquet.file_schema(path)`, `vexec_parquet.file_row_groups(path)` | a file's columns as Arrow's reader makes them; its row groups' rows, bytes and writer |
| `vexec_parquet.fetch_stats(reset)` | the process's fetches by TID: rows, the columns read for them, data pages read alone and those of them the offset index found, columns a kept page or chunk answered, column chunks read whole; reset where asked |
| `vexec_parquet.read_stats(reset)` | what the process read to plan and scan: footers read from files, files opened of their facts, footers read later for Arrow's reader, own tables' listings read from the catalog, columns' statistics decoded; the shared footer cache's entries, bytes, finds, misses and entries kept |
| `vexec_parquet.scan_stats(reset)` | how the process's scans read (§3.37): Arrow's large blocks mapped, reused and moved, the bytes kept; text checked a batch at once or a value at a time, batches of text found UTF-8 whole before and column chunks marked so; readers over footers of their columns, and those refused for a type; units cut into ranges of rows; batches whose later steps were read by pages, and the data pages decoded and passed over |
| `vexec_parquet.verify_facts(regclass)` | an own table's files: whether the catalog keeps each one's facts, whether they are its footer's read again, whether every data page is of version 1 |

## Privileges

Setting a table's `location` takes the privileges of `pg_read_server_files`,
its `storage` those of `pg_write_server_files`, as file_fdw's `filename`
does; so do `columns()`, `column_list()`, `file_schema()`,
`file_row_groups()` and `verify_facts()`, which PUBLIC may not execute.
`files()` answers for a table its caller may read, the paths only to those
who may read the server's files.  The catalogs are the access method's
alone.

## Limitations

- **Not in WAL.**  A local table's files are not WAL-logged: a standby, a
  logical decoder, point-in-time recovery past a base backup and pg_upgrade
  see no row of them.  Move such tables with dump and restore.  A database
  with them is no template for CREATE DATABASE, and moves to no other
  tablespace.
- **No compaction.**  A deleted row stays in its file; VACUUM takes the
  index entries of rows dead to every snapshot and reports the others, and
  VACUUM FULL or CLUSTER rewrites the table -- not MVCC-safe for a snapshot
  older than it, as heap's.
- **Refused:** CREATE INDEX CONCURRENTLY and REINDEX CONCURRENTLY (a
  concurrent build reads no consistent set of files); writes, indexes and
  partitions of tables that read files (COPY TO of one returns no rows and
  names the query form; TRUNCATE of one leaves its files).
- **No pages.**  No TOAST table, `pg_relation_size()` 0, no buffer reads or
  hits in `pg_statio_*` or `pg_stat_io`; TABLESAMPLE SYSTEM samples blocks
  of 291 row ids.
- **Timestamps** past 294247 AD, which Arrow's microseconds since 1970 do not
  reach, are refused as a row is written; so is text not valid in the
  database's encoding, which only a C function can make -- a Parquet string
  is UTF-8, and a scan vouches for its files' text to vexec whole.
- **A table of no columns** keeps its rows' count in a hidden column,
  `_vpq_rows`.

## Building and testing

```sh
make PG_CONFIG=/usr/local/pgsql/bin/pg_config        # Arrow C++ by pkg-config: arrow parquet arrow-s3
make PG_CONFIG=... install
make PG_CONFIG=... installcheck                      # a server with vexec and vexec_parquet preloaded
test/vexec/run.sh parquet                            # the leg, in docker/Dockerfile.parquet's image
```

`engine/` is the only C++, behind a C interface (`engine/engine.h`); it
catches every exception and blocks signals while Arrow's and the AWS SDK's
threads may start, so that they inherit masks PostgreSQL's handlers need.
Arrow's pools start in each process as it first reads or writes, never in
the postmaster.

The leg (`test/vexec/parquet.sh`):

- `suite` -- vexec's own regression suite, vexec_parquet preloaded beside it.
- `parquet` -- the module's suite (`test/sql`): `types` (every type class
  round trip and the Arrow type it is written as), `dml`, `mvcc`, `options`
  (with privileges), `external` (over its own files), `vexec` (force mode
  in both batch formats against the plain executor, statistics, pruning,
  parallel units, the sink), `fetch` (fetches by TID: point reads, kept
  pages, vexec's late columns), `external_arrow` (pyarrow's files,
  `test/data/make_files.py`), `encodings` (their text in LATIN1 and
  SQL_ASCII databases), `geo` (GeoParquet's files of every form read as
  PostGIS's types, their CRSs, refused values, && pruning; where the image
  has PostGIS, as docker/Dockerfile.parquet's does), `nested` (lists of
  every flat element type and maps read as arrays, hstore and jsonb, the
  shapes refused, their operators, vexec's source in both formats), `s3`
  (moto's S3 server on the loopback), and
  the isolation specs (`test/specs`): concurrent updates, row locks,
  upserts and foreign keys, SERIALIZABLE.
- `differential` -- the differential runner's sessions over tables of their
  own files: PostgreSQL's schedule and vexec's corpus with
  `default_table_access_method = vexec_parquet`, every force session
  answering as off but where a difference is kept, reviewed
  (`test/vexec/differential/kept/pg_parquet`: plans handed as rows, ties'
  order, a case-insensitive group's representative).

## Licence

Apache-2.0, as Apache Arrow C++ is.
