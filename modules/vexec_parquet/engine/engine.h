/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * engine.h
 *	  vexec_parquet's engine: Apache Arrow C++ and its Parquet library behind
 *	  a C interface, the only code of the extension that is C++.
 *
 * The extension's C code -- the table access method, vexec's source and
 * sink -- never sees a C++ type or exception.  Every call here returns
 * VPQ_OK, or VPQ_ERROR with vpq_error() saying why (VPQ_NOT_FOUND where a
 * file is missing), and the caller raises the error from its own C frame:
 * no C++ frame is ever unwound by PostgreSQL's longjmp, and no exception
 * crosses into C.
 *
 * Threads.  Arrow reads and writes on the caller's thread, but for what
 * cannot be done there: S3's client starts the AWS SDK's event loop thread,
 * and Arrow's I/O thread pool serves S3's listings, its prefetched ranges
 * and its multipart uploads.  Every call blocks the process's asynchronous
 * signals on the calling thread while it runs, so that a thread it starts
 * inherits them blocked and PostgreSQL's handlers -- SIGINT's cancel,
 * SIGTERM, SIGUSR1's latch -- run on the backend's own thread alone, once
 * the call returns.  No thread calls into PostgreSQL.
 *
 * Memory.  Arrow allocates its own: a block of 256 kB or more from the
 * engine's pool -- mapped, grown and shrunk by mremap(), a freed one kept
 * for the process's next reads until the transaction ends (PQ8) -- a smaller
 * one from the system's allocator; a handle holds what it read until the C
 * code releases it, which the extension ties to a memory context's reset,
 * so that an error frees it too.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VPQ_ENGINE_H
#define VPQ_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define VPQ_OK			0
#define VPQ_ERROR		(-1)
#define VPQ_NOT_FOUND	(-2)

/* The last error of this thread, until the next call */
extern const char *vpq_error(void);

/*
 * S3's settings, the extension's GUCs: an empty string leaves Arrow's own
 * (the URI's query, the environment's AWS_* variables, the SDK's chain of
 * credentials).  Filesystems made before a change are dropped.
 */
typedef struct vpq_s3_config
{
	const char *endpoint;		/* host[:port] */
	const char *region;
	const char *access_key;
	const char *secret_key;
	const char *session_token;
	const char *scheme;			/* http or https */
	bool		virtual_addressing;
	bool		anonymous;
	double		connect_timeout;	/* seconds; <= 0 the SDK's */
	double		request_timeout;
	bool		prefetch;		/* coalesced, concurrent range reads of S3 */
	int			io_threads;		/* Arrow's I/O pool: S3's concurrency */
} vpq_s3_config;

extern int	vpq_configure(const vpq_s3_config *config);

/*
 * The waits of the process's own thread on storage (pg_vector_executor.md
 * §3.41, OT4): the engine calls the hook as a file's read begins
 * (VPQ_WAIT_READ), or a wait for the point reads' threads (VPQ_WAIT_FETCH),
 * and as it ends (VPQ_WAIT_NONE) -- on that thread alone, never on a thread
 * of Arrow's or its own -- so that the C code puts the backend under a wait
 * event of its own; the hook neither allocates nor raises.  NULL: none.
 * Arrow's asynchronous reads, S3's prefetched ranges, are not among them.
 */
#define VPQ_WAIT_NONE	0
#define VPQ_WAIT_READ	1
#define VPQ_WAIT_FETCH	2

typedef void (*vpq_wait_hook) (int what);

extern void vpq_set_wait_hook(vpq_wait_hook hook);

/* Whether a location names S3 ("s3://...") */
extern bool vpq_is_s3(const char *location);

/* The engine's process state gone: S3's client, its event loop thread */
extern void vpq_shutdown(void);

/*
 * Files.  A location is an absolute path, a file:// URI or an s3://bucket/key
 * URI.  A listing is of a file, of a directory's *.parquet files at any
 * depth, or of a glob -- '*' and '?' within a name, '**' across directories
 * -- in path order.
 */
typedef struct vpq_entry
{
	char	   *path;			/* the location's own form: a path or a URI */
	int64_t		size;
	int64_t		mtime_ns;		/* nanoseconds since 1970 */
} vpq_entry;

extern int	vpq_list(const char *location, vpq_entry **entries, int64_t *n);
extern void vpq_list_free(vpq_entry *entries, int64_t n);
extern int	vpq_stat(const char *path, vpq_entry *entry);	/* VPQ_NOT_FOUND */
extern int	vpq_make_dir(const char *path); /* a local directory and its parents */
extern int	vpq_delete_file(const char *path);	/* a missing file is no error */
extern int	vpq_delete_dir(const char *path);	/* and everything under it */
extern int	vpq_copy_file(const char *from, const char *to);
/* The files directly in a directory (S3: under a prefix), any name */
extern int	vpq_list_dir(const char *dir, vpq_entry **entries, int64_t *n);

/*
 * Arrow's types, as the engine names them to C.  Parquet's are read as
 * Arrow's reader makes them of the file's schema.
 */
typedef enum vpq_type_id
{
	VPQ_T_NULL = 0,
	VPQ_T_BOOL,
	VPQ_T_INT8,
	VPQ_T_INT16,
	VPQ_T_INT32,
	VPQ_T_INT64,
	VPQ_T_UINT8,
	VPQ_T_UINT16,
	VPQ_T_UINT32,
	VPQ_T_UINT64,
	VPQ_T_FLOAT16,
	VPQ_T_FLOAT32,
	VPQ_T_FLOAT64,
	VPQ_T_DATE32,				/* days since 1970 */
	VPQ_T_DATE64,				/* milliseconds since 1970 */
	VPQ_T_TIME32,				/* unit s or ms */
	VPQ_T_TIME64,				/* unit us or ns */
	VPQ_T_TIMESTAMP,			/* unit, and a time zone or none */
	VPQ_T_DECIMAL32,
	VPQ_T_DECIMAL64,
	VPQ_T_DECIMAL128,
	VPQ_T_DECIMAL256,
	VPQ_T_STRING,				/* int32 offsets */
	VPQ_T_LARGE_STRING,			/* int64 offsets */
	VPQ_T_STRING_VIEW,
	VPQ_T_BINARY,
	VPQ_T_LARGE_BINARY,
	VPQ_T_BINARY_VIEW,
	VPQ_T_FIXED_BINARY,			/* byte_width */
	VPQ_T_DICTIONARY,			/* value_id: the values' type, index_width */
	VPQ_T_OTHER,				/* nested of other shapes, intervals, ...: not read */
	/*
	 * PQ19 (pg_vector_executor.md §3.40): nested types of flat children,
	 * which vpq_file_column_children() names -- a list's elements, a map's
	 * keys and values
	 */
	VPQ_T_LIST,					/* int32 offsets; child its elements */
	VPQ_T_LARGE_LIST,			/* int64 offsets */
	VPQ_T_MAP					/* int32 offsets; child its keys, child2 its values */
} vpq_type_id;

/* VPQ_T_TIMESTAMP's, VPQ_T_TIME32's, VPQ_T_TIME64's unit */
#define VPQ_UNIT_S	0
#define VPQ_UNIT_MS	1
#define VPQ_UNIT_US	2
#define VPQ_UNIT_NS	3

typedef struct vpq_type
{
	int			id;				/* vpq_type_id */
	int			unit;			/* VPQ_UNIT_* */
	bool		has_tz;			/* a timestamp adjusted to UTC */
	int			precision;		/* decimals */
	int			scale;
	int			byte_width;		/* fixed binary */
	int			value_id;		/* a dictionary's values' vpq_type_id */
	int			index_width;	/* a dictionary's indexes' bytes */
	bool		uuid;			/* Parquet's UUID logical type */
	char		name[64];		/* Arrow's ToString(), for messages */
} vpq_type;

/*
 * A Parquet file: its footer, read once in the process by path, size and
 * modification time, and its schema as Arrow's reader makes it.  Refcounted
 * in the engine: vpq_file_close() lets the handle go, the footer staying in
 * the process's cache while it fits.
 */
typedef struct vpq_file vpq_file;

/* size < 0: the file stat()ed first */
extern int	vpq_file_open(const char *path, int64_t size, int64_t mtime_ns, vpq_file **file);
extern void vpq_file_close(vpq_file *file);
extern const char *vpq_file_location(vpq_file *file);
extern int64_t vpq_file_size(vpq_file *file);
extern int64_t vpq_file_mtime(vpq_file *file);
extern int	vpq_file_row_groups(vpq_file *file);
extern int64_t vpq_file_rows(vpq_file *file);
extern int64_t vpq_file_row_group_rows(vpq_file *file, int rg);
extern int	vpq_file_columns(vpq_file *file);
/* A top-level column: its name, its field id (-1 none), its type; false for a nested one */
extern bool vpq_file_column(vpq_file *file, int col, const char **name, int *field_id,
							vpq_type *type, bool *nullable);
/*
 * PQ19: a nested top-level column's children -- a list's elements, a map's
 * keys and values -- their types and whether they may be NULL, into
 * children[] and nullable[] (two at most); 0 for a column of any other type
 */
extern int	vpq_file_column_children(vpq_file *file, int col, vpq_type *children, bool *nullable);
/* A column chunk's compressed bytes */
extern int64_t vpq_file_column_bytes(vpq_file *file, int rg, int col);
/* W38: its bytes before compression, about, from the facts where the footer is not read; 0 where unknown */
extern int64_t vpq_file_column_raw_bytes(vpq_file *file, int rg, int col);
/* The writer's application, Parquet's "created_by" */
extern const char *vpq_file_created_by(vpq_file *file);

/*
 * GeoParquet (pg_vector_executor.md §3.39's PQ16), from the footer or the
 * facts, the bytes the engine's until the file is closed:
 *
 * vpq_file_geo_metadata(): the footer's key "geo" -- GeoParquet's metadata,
 * its JSON's bytes, of every version: 1.x, the only sign of a column of
 * WKB in a file without Parquet's geospatial types, and 2.0 -- NULL where
 * the footer has none.
 *
 * vpq_file_column_geo(): a top-level column's geospatial logical type,
 * Parquet's GEOMETRY or GEOGRAPHY -- VPQ_GEO_NONE for any other -- and its
 * CRS as the type writes it, "" where it writes none (Parquet's default,
 * OGC:CRS84), a "projjson:<key>" one as that key's value in the footer.
 *
 * vpq_file_column_geo_bounds(): a column chunk's GeospatialStatistics, the
 * bounds of its X and Y; false where it has none, or none valid and finite.
 */
#define VPQ_GEO_NONE		0
#define VPQ_GEO_GEOMETRY	1
#define VPQ_GEO_GEOGRAPHY	2

extern const char *vpq_file_geo_metadata(vpq_file *file, size_t *len);
extern int	vpq_file_column_geo(vpq_file *file, int col, const char **crs, size_t *crs_len);
extern bool vpq_file_column_geo_bounds(vpq_file *file, int rg, int col, double *xmin, double *xmax,
									   double *ymin, double *ymax);
/* The footer's bytes, for the extension's own files' catalog */
extern int64_t vpq_file_footer_bytes(vpq_file *file);

/*
 * A file's facts (pg_vector_executor.md §3.34, PQ1 and PQ4): what a scan
 * asks of its footer -- its row groups' rows and its columns as Arrow's
 * reader makes them; with VPQ_FACTS_CHUNKS each column chunk's codec, first
 * data page's version where read, its place and bytes; with VPQ_FACTS_STATS
 * its statistics -- as bytes of the engine's own form, kept where a process
 * finds them without reading the footer: an own file's in its catalog row,
 * an external one's in shared memory.  A handle opened of them answers the
 * calls above without reading a footer -- a chunk's statistics where kept,
 * else none -- and reads it as Arrow's reader is opened over it.
 */
#define VPQ_FACTS_CHUNKS	0x01
#define VPQ_FACTS_STATS		0x02	/* implies VPQ_FACTS_CHUNKS */
#define VPQ_FACTS_PAGES_V1	0x04	/* every data page of version 1: the extension's writer's file */
#define VPQ_FACTS_GEO		0x08	/* PQ16: the footer's geospatial facts, set by the engine where it has them */
#define VPQ_FACTS_NESTED	0x10	/* PQ19: its nested columns' children, set where it has such columns */

/* An open file's facts of the flags' forms, malloc()ed for the caller to free() */
extern int	vpq_file_facts(vpq_file *file, int flags, char **facts, size_t *len);
/*
 * A file opened of its facts, of the file at path, size and mtime_ns (-1 an
 * own file's); the footer if the process holds it already.  VPQ_NOT_FOUND
 * where the facts are not of this engine's form: the caller opens the file.
 */
extern int	vpq_file_open_facts(const char *path, int64_t size, int64_t mtime_ns, const char *facts,
								size_t len, vpq_file **file);
/* Whether a handle holds its file's footer, read or found in the process */
extern bool vpq_file_has_footer(vpq_file *file);
/* A column chunk's first data page's version as its header has it, whatever its codec: 1, 2, -1 not told */
extern int	vpq_file_page_version(vpq_file *file, int rg, int col);

/* The process's footers read from files, handles opened of facts, and those of them whose footer was read later */
typedef struct vpq_engine_stats
{
	int64_t		footers_read;
	int64_t		facts_opened;
	int64_t		footers_late;
	int64_t		versions_read;	/* PQ7: data pages' headers read for their versions */
	/* PQ8: Arrow's large blocks mapped afresh, taken from those kept, moved by mremap(); the bytes kept now */
	int64_t		pool_mapped;
	int64_t		pool_reused;
	int64_t		pool_moved;
	int64_t		pool_kept_bytes;
	/* PQ11: CPU readers opened over a footer of their columns; those whose types differed, opened whole */
	int64_t		footers_projected;
	int64_t		projections_refused;
	/* PQ12, PQ13: data pages a range of rows decoded, and those it passed over undecompressed */
	int64_t		pages_decoded;
	int64_t		pages_passed;
} vpq_engine_stats;

extern void vpq_engine_stats_get(vpq_engine_stats *out, bool reset);
/* OT5 (pg_vector_executor.md §3.41): footers_read and facts_opened alone, for a planning's figures */
extern void vpq_engine_footer_counts(int64_t *footers_read, int64_t *facts_opened);
/* the pool's, PQ11's and the range reads' counters alone set to 0 */
extern void vpq_engine_stats_reset_scans(void);

/*
 * PQ8 (pg_vector_executor.md §3.37): the bytes of Arrow's freed large blocks
 * the process keeps for its next reads (vexec_parquet.read_memory_mb), and
 * every kept one given back -- as a transaction ends
 */
extern void vpq_pool_set_keep(int64_t bytes);
extern void vpq_pool_release(void);

/*
 * A value of a column's statistics, in the column's Arrow type: integers,
 * dates, times, timestamps and decimals' unscaled values in i (decimals
 * past 64 bits in i128_hi:i128_lo), floats in d, strings and binaries in
 * bytes.
 */
typedef struct vpq_value
{
	int64_t		i;
	uint64_t	i128_lo;
	int64_t		i128_hi;
	double		d;
	const char *bytes;			/* the engine's, until the file is closed */
	int32_t		len;
} vpq_value;

/*
 * A column chunk's statistics: false where the footer has none the reader
 * trusts (ColumnChunkMetaData::is_stats_set()); *has_minmax false where it
 * has a NULL count alone.  min_exact and max_exact: not truncated.
 */
extern bool vpq_file_column_stats(vpq_file *file, int rg, int col, int64_t *null_count,
								  bool *has_minmax, vpq_value *min, vpq_value *max,
								  bool *min_exact, bool *max_exact);

/* A column chunk's codec, of those vpq_file_column_pages() names */
#define VPQ_CODEC_OTHER		0
#define VPQ_CODEC_SNAPPY	1
#define VPQ_CODEC_LZ4		2		/* LZ4_RAW, or Hadoop's framing */

/*
 * A column chunk's codec and, where it is snappy or LZ4, the version of its
 * first data page -- 1 or 2, as its header has it -- or 0 where the header
 * does not tell; false where the chunk is not one this tells of.
 */
extern bool vpq_file_column_pages(vpq_file *file, int rg, int col, int *codec, int *version);

/*
 * PQ7 (pg_vector_executor.md §3.35): as vpq_file_column_pages(), and into
 * *facts_at, where the version was read now from the page's header of a
 * file opened of its facts, the place of its byte in the facts -- so that
 * the caller keeps it where the next process finds them -- else -1.
 */
extern bool vpq_file_column_pages_at(vpq_file *file, int rg, int col, int *codec, int *version,
									 int64_t *facts_at);

/*
 * PQ14 (pg_vector_executor.md §3.37): whether a reader found the text of
 * column col's chunk of row group rg UTF-8 whole, every value, as the facts
 * the file was opened of say -- false where it was opened of its footer.
 * vpq_file_text_mark(): that chunk's byte in the file's facts and, into
 * *byte, its value marked so -- for the caller to write where the next
 * process finds them -- else -1; the facts this handle holds left as they
 * are.
 */
extern bool vpq_file_text_checked(vpq_file *file, int rg, int col);
extern int64_t vpq_file_text_mark(vpq_file *file, int rg, int col, uint8_t *byte);

/*
 * An array of a row group's column, as Arrow holds it: the buffers' own
 * pointers, valid while the chunk it came from is.  validity is a bitmap,
 * least significant bit first, 1 = valid, NULL where no value is NULL;
 * offset is the array's first value's place in its buffers, in values (in
 * bits for the validity bitmap and a boolean's values).
 */
typedef struct vpq_array
{
	vpq_type	type;
	int64_t		length;
	int64_t		offset;
	int64_t		null_count;
	const uint8_t *validity;
	const uint8_t *values;		/* fixed widths; offsets; views; indexes */
	const uint8_t *data;		/* the bytes of a binary or string */
	int64_t		data_size;
	int			nbuffers;		/* views: their data buffers */
	const uint8_t *const *buffers;
	const int64_t *buffer_sizes;
	const struct vpq_array *dictionary; /* VPQ_T_DICTIONARY's values */
	const struct vpq_array *child;	/* PQ19: a list's elements, a map's keys, which its offsets index */
	const struct vpq_array *child2; /* a map's values */
} vpq_array;

/* Some columns of one row group, read; refcounted */
typedef struct vpq_chunk vpq_chunk;

extern int64_t vpq_chunk_rows(vpq_chunk *chunk);
extern const vpq_array *vpq_chunk_array(vpq_chunk *chunk, int i);
extern void vpq_chunk_retain(vpq_chunk *chunk);
extern void vpq_chunk_release(vpq_chunk *chunk);

/* A file open for reading row groups */
typedef struct vpq_reader vpq_reader;

/* dictionaries: per file column, whether a string column is read as a dictionary, or NULL */
extern int	vpq_reader_open(vpq_file *file, const bool *dictionaries, vpq_reader **reader);
extern void vpq_reader_close(vpq_reader *reader);
/* The columns cols[0 .. ncols) of row group rg, in that order */
extern int	vpq_reader_read(vpq_reader *reader, int rg, const int *cols, int ncols, vpq_chunk **chunk);

/*
 * PQ11 (pg_vector_executor.md §3.37): a reader of the columns cols[] alone --
 * over a footer of the file's row groups and those columns, made of the
 * footer's index (vpq_file_index()) and the footer's bytes, where the
 * handle is of facts and its columns are of the types its file's own reader
 * makes -- else as vpq_reader_open()
 */
extern int	vpq_reader_open_cols(vpq_file *file, const bool *dictionaries, const int *cols, int ncols,
								 const char *index, size_t index_len, vpq_reader **reader);

/*
 * PQ11: the index of a file's footer -- where its schema elements, row
 * groups and column chunks lie -- walked from its bytes, malloc()ed for the
 * caller to free(), for the shared footer cache; VPQ_NOT_FOUND where it is
 * not one a footer is projected of: a schema not flat, an encrypted file
 */
extern int	vpq_file_index(vpq_file *file, char **index, size_t *len);

/*
 * PQ12, PQ13: a row group's columns cols[] read forward in ranges of rows,
 * each column by one page reader -- the data pages before a range passed
 * over undecompressed -- each range's rows as arrays of their own: rows
 * [lo, lo + n), lo at or past the last range's end.  VPQ_NOT_FOUND, from
 * either, where a column is not read so: the caller reads it whole.
 */
typedef struct vpq_range vpq_range;

extern int	vpq_range_open(vpq_reader *reader, int rg, const int *cols, int ncols, vpq_range **range);
extern int	vpq_range_read(vpq_range *range, int64_t lo, int64_t n, vpq_chunk **chunk);
extern void vpq_range_close(vpq_range *range);

/*
 * A point read (pg_vector_executor.md §3.34, as ch_table_engines' VC34 reads
 * a row where it lies): of row group rg's column col, the data page holding
 * its row `row`, read and decoded alone -- that page's bytes and its
 * dictionary page's by the file's offset index (*indexed), else the pages'
 * headers walked to it, the others' bytes neither decompressed nor decoded --
 * its rows [*first, *first + *nrows) of the row group as a chunk of one
 * column, of the Arrow type vpq_reader_read() makes of it.  VPQ_NOT_FOUND
 * where the column or its type is not one read so: the caller reads the
 * column chunk whole.
 */
extern int	vpq_reader_read_page(vpq_reader *reader, int rg, int col, int64_t row, vpq_chunk **chunk,
								 int64_t *first, int64_t *nrows, bool *indexed);

/*
 * W55 (pg_vector_executor.md §3.36): the point reads of n columns cols[] at
 * once, each as vpq_reader_read_page() reads one, on `threads` threads of the
 * engine's own -- chunks[i], firsts[i], nrows[i], indexed[i] and rcs[i] its
 * results, NULL chunks where none -- VPQ_ERROR where one of them failed, its
 * message vpq_error()'s, the chunks read by the others the caller's to
 * release.
 */
extern int	vpq_reader_read_pages(vpq_reader *reader, int rg, int n, const int *cols, int64_t row, int threads,
								  vpq_chunk **chunks, int64_t *firsts, int64_t *nrows, bool *indexed, int *rcs);

/*
 * Writing a file of the extension's own: the columns' buffers in Arrow's
 * layouts, borrowed for the call; the file written whole, its local form
 * through a temporary name, synced and renamed, and S3's by an upload that
 * makes the object at its end.
 */
typedef struct vpq_column_spec
{
	const char *name;
	int			field_id;		/* the attribute's number */
	vpq_type	type;			/* id, and unit/has_tz/precision/scale/byte_width */
	bool		nullable;
	bool		json;			/* a string as Parquet's JSON */
} vpq_column_spec;

typedef struct vpq_column_data
{
	int64_t		null_count;
	const uint8_t *validity;	/* NULL where null_count is 0 */
	const uint8_t *values;		/* fixed widths, booleans' bits, or int32 offsets */
	int64_t		values_size;
	const uint8_t *data;		/* a binary's or string's bytes */
	int64_t		data_size;
} vpq_column_data;

typedef struct vpq_write_options
{
	const char *codec;			/* zstd, lz4, snappy, gzip, brotli, none */
	int			level;			/* the codec's level; INT32_MIN its default */
	int64_t		row_group_rows;
	int64_t		page_bytes;
	bool		dictionary;
	bool		page_index;
	bool		sync;			/* fsync a local file and its directory */
} vpq_write_options;

/*
 * chunk_bytes, where given: each row group's columns' compressed bytes,
 * [rg * ncols + col]; facts, where given: the file's (VPQ_FACTS_PAGES_V1),
 * malloc()ed for the caller to free().  The file's footer is kept in the
 * process, as one read would be.
 */
extern int	vpq_write_file(const char *path, int ncols, const vpq_column_spec *specs,
						   const vpq_column_data *cols, int64_t nrows,
						   const vpq_write_options *options, int64_t *bytes_written,
						   int64_t *chunk_bytes, char **facts, size_t *facts_len);

#ifdef __cplusplus
}
#endif

#endif							/* VPQ_ENGINE_H */
