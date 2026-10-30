/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_parquet.h
 *	  vexec_parquet: a table access method of Parquet files, read and
 *	  written by Apache Arrow C++ alone, on the local filesystem or S3, with
 *	  vexec's batch source and sink (vexec_source.h, vexec_sink.h) -- the
 *	  contracts ch_table_engines' parquet access method implements over
 *	  ClickHouse's readers, implemented here without ClickHouse.
 *
 * Two kinds of table, fixed at CREATE TABLE:
 *
 *	external	WITH (location = '...'): the Parquet files of a path, a
 *				directory or a glob, local or s3://, read-only, listed as
 *				each statement first reads them; rows outside MVCC, TIDs a
 *				file's place in the listing and a row's in its file.
 *	own			without location: files the table writes itself under its
 *				storage directory -- $PGDATA's database directory, or the
 *				`storage` option's root, local or s3:// -- under MVCC:
 *				each file a row of vexec_parquet.files, visible as that row
 *				is; DELETE and UPDATE rows of vexec_parquet.tombstones, as
 *				ch_table_engines' tables of their own files keep them.
 *
 * Row ids: a file of an own table holds rows of consecutive ids, its first
 * the catalog's first_rowid, taken from the storage's counter, which never
 * goes back; a TID is a row id, 291 rows a block.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_PARQUET_H
#define VEXEC_PARQUET_H

#include "access/relscan.h"
#include "access/skey.h"
#include "access/tableam.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "storage/itemptr.h"
#include "storage/relfilelocator.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

#include "engine.h"

/* TIDs from row ids: 291 = MaxHeapTuplesPerPage, a TID bitmap's limit */
#define VPQ_ROWS_PER_BLOCK		291
#define VPQ_ROWID_TO_BLOCK(id)	((BlockNumber) ((id) / VPQ_ROWS_PER_BLOCK))
#define VPQ_ROWID_TO_OFFSET(id) ((OffsetNumber) ((id) % VPQ_ROWS_PER_BLOCK + 1))
#define VPQ_TID_TO_ROWID(tid) \
	((uint64) ItemPointerGetBlockNumberNoCheck(tid) * VPQ_ROWS_PER_BLOCK + \
	 ItemPointerGetOffsetNumberNoCheck(tid) - 1)
#define VPQ_MAX_ROWID			((uint64) (MaxBlockNumber - 1) * VPQ_ROWS_PER_BLOCK)

static inline void
vpq_set_tid(ItemPointer tid, uint64 rowid)
{
	ItemPointerSet(tid, VPQ_ROWID_TO_BLOCK(rowid), VPQ_ROWID_TO_OFFSET(rowid));
}

/* A TID's row id, where it names one */
static inline bool
vpq_tid_rowid(ItemPointer tid, uint64 *rowid)
{
	if (!ItemPointerIsValid(tid) || ItemPointerGetOffsetNumberNoCheck(tid) > VPQ_ROWS_PER_BLOCK)
		return false;
	*rowid = VPQ_TID_TO_ROWID(tid);
	return true;
}

/*
 * Settings (module.c)
 */
extern char *vpq_storage_root;
extern char *vpq_default_codec;
extern int	vpq_default_row_group_rows;
extern int	vpq_buffer_rows;
extern int	vpq_buffer_mb;
extern bool vpq_sync;
/* The bytes of a data page the extension's writer makes: Arrow's, uncompressed */
#define VPQ_PAGE_BYTES		(1024 * 1024)

extern bool vpq_enable_pruning;
extern bool vpq_enable_device_units;
extern bool vpq_enable_point_reads;
extern bool vpq_enable_fetch_columns;
extern int	vpq_fetch_threads;
extern int	vpq_read_memory_mb;
extern bool vpq_enable_row_ranges;
extern bool vpq_enable_step_pages;
extern bool vpq_enable_text_marks;
extern bool vpq_enable_projected_footers;
extern char *vpq_s3_endpoint;
extern char *vpq_s3_region;
extern char *vpq_s3_access_key_id;
extern char *vpq_s3_secret_access_key;
extern char *vpq_s3_session_token;
extern char *vpq_s3_scheme;
extern bool vpq_s3_virtual_addressing;
extern bool vpq_s3_anonymous;
extern bool vpq_s3_prefetch;
extern int	vpq_s3_io_threads;
extern bool vpq_preloaded;

/* The engine configured with the settings, before a call that may touch S3 */
extern void vpq_engine_ready(void);
/* An engine call's failure as an error */
pg_noreturn extern void vpq_engine_error(int rc, const char *what);
#define VPQ_CHECK(call, what) \
	do { \
		int vpq_rc_ = (call); \
		if (vpq_rc_ != VPQ_OK) \
			vpq_engine_error(vpq_rc_, (what)); \
	} while (0)

/*
 * A slot of the access method's rows: a virtual one, its row's writer where
 * known, and its xmax as heap's tuple would show it, looked up as it is
 * first asked for where not known
 */
typedef struct VpqSlot
{
	VirtualTupleTableSlot base;
	TransactionId xmin;
	TransactionId xmax;
	bool		xmax_known;
} VpqSlot;

/* The handler's routine, the key of the source and the sink */
extern const TableAmRoutine *vpq_routine(void);
extern void vpq_register_vexec(void);
extern void vpq_register_otel(void);
extern void vpq_slot_set_xmin(TupleTableSlot *slot, TransactionId xmin);
extern void vpq_slot_set_xmax(TupleTableSlot *slot, TransactionId xmax);

/*
 * Options (options.c): from pg_class.reloptions, which the ProcessUtility
 * hook writes, or from the running CREATE's before they are written
 */
typedef struct VpqOptions
{
	char	   *location;		/* external: where it reads */
	char	   *storage;		/* own: the files' root, NULL for the default */
	char	   *codec;			/* own: the next files' */
	int			compression_level;	/* INT32_MIN: the codec's default */
	int			row_group_rows;
	int			page_bytes;		/* 0: VPQ_PAGE_BYTES */
} VpqOptions;

extern void vpq_options_init(void);
extern void vpq_relation_options(Relation rel, VpqOptions *opts);
extern bool vpq_is_vpq_rel(Relation rel);

/*
 * A storage: the catalogs' key of an own table's files, its relfilenumber's
 * incarnation, so that TRUNCATE and rewrites are a new storage
 */
typedef struct VpqStorage
{
	Oid			spc;
	RelFileNumber relnumber;
	int			backend;		/* ProcNumber of a temporary table's, -1 */
	char		persistence;	/* RELPERSISTENCE_*: the catalogs' twin */
} VpqStorage;

/*
 * A column, by the attribute's base type
 */
typedef enum VpqKind
{
	VPQ_K_DROPPED,
	VPQ_K_BOOL,
	VPQ_K_INT2,
	VPQ_K_INT4,
	VPQ_K_INT8,
	VPQ_K_FLOAT4,
	VPQ_K_FLOAT8,
	VPQ_K_OID,					/* uint32 */
	VPQ_K_CHAR,					/* "char", int8 */
	VPQ_K_DATE,
	VPQ_K_TIME,
	VPQ_K_TIMESTAMP,
	VPQ_K_TIMESTAMPTZ,
	VPQ_K_NUMERIC,				/* numeric(p,s), p <= 38, 0 <= s <= p */
	VPQ_K_NUMERIC_ANY,			/* other numerics: own files keep their image */
	VPQ_K_TEXT,					/* text, varchar */
	VPQ_K_BPCHAR,
	VPQ_K_BYTEA,
	VPQ_K_UUID,
	VPQ_K_JSON,
	VPQ_K_FIXED,				/* any other fixed length: its image */
	VPQ_K_VARLENA				/* any other varlena: its image */
} VpqKind;

typedef struct VpqColumn
{
	AttrNumber	attnum;
	char	   *name;
	Oid			typid;			/* the attribute's */
	Oid			basetype;		/* a domain's base */
	int32		typmod;			/* the base type's */
	int16		typlen;
	bool		typbyval;
	char		typalign;
	Oid			collation;
	bool		notnull;
	VpqKind		kind;
	int			precision;		/* numeric(p,s) */
	int			scale;
	vpq_type	ftype;			/* an own file's type of it */
	bool		json;
	Datum		missing;		/* its value in a file that lacks it */
	bool		missing_null;
	bool		recv;			/* PQ15: its type has a binary receive function */
	int			geo;			/* PQ16: PostGIS's geometry or geography (VPQ_GEO_*), else VPQ_GEO_NONE */
	Oid			elemtype;		/* PQ20: an array's element type, else InvalidOid */
	bool		hstore;			/* PQ20: hstore's type */
} VpqColumn;

typedef struct VpqTable
{
	Oid			relid;
	bool		external;
	VpqOptions	opts;
	VpqStorage	storage;		/* own */
	char	   *dir;			/* own: the storage's directory, a location */
	char	   *root;			/* own: the storage's root */
	int			natts;
	VpqColumn  *cols;			/* per attribute */
	bool		utf8;			/* the database's encoding: text kept as Parquet's strings */
	MemoryContext cxt;
} VpqTable;

/* table.c */
extern VpqTable *vpq_table(Relation rel);
extern bool vpq_desc_matches(Relation rel, TupleDesc desc);
extern VpqTable *vpq_table_for_desc(Relation rel, TupleDesc desc);
extern void vpq_table_init(void);
extern void vpq_tables_xact_end(void);
extern void vpq_storage_of(Relation rel, const RelFileLocator *locator, VpqStorage *st);
extern char *vpq_storage_root_of(const VpqOptions *opts, const VpqStorage *st);
extern char *vpq_storage_dir(const VpqStorage *st, const char *root, Oid dboid);
extern char *vpq_file_path(const char *dir, int64 file_id, uint64 fxid);
extern bool vpq_file_name_parse(const char *path, int64 *file_id, uint64 *fxid);
extern void vpq_column_kind(VpqColumn *col, bool utf8);
extern bool vpq_type_has_receive(Oid typid);
extern const char *vpq_type_sql(const vpq_type *t);

/*
 * Statistics of a unit -- a row group -- for pruning and aggregate()
 */
typedef struct VpqStat
{
	int64		nulls;			/* -1: unknown */
	bool		has_minmax;
	bool		bytewise;		/* min and max as bytes: a string's, a binary's */
	bool		may_nan;		/* a float column whose NaNs the range leaves out */
	bool		exact;			/* min and max are values of the unit */
	Datum		min;			/* of the column's base type; bytewise: its bytes as bytea */
	Datum		max;
	bool		has_box;		/* PQ18: a geometry column's X and Y bounds (geo.c) */
	double		box[4];			/* xmin, xmax, ymin, ymax */
} VpqStat;

/*
 * A data file of a scan
 */
typedef struct VpqStatsIndex VpqStatsIndex;

typedef struct VpqFileEntry
{
	char	   *path;			/* its location */
	int64		size;
	int64		mtime;			/* -1: not checked (own files never change) */
	int64		file_id;		/* own: the catalog's; external: its place in the listing */
	uint64		first_rowid;
	int64		rows;
	int			nunits;			/* its row groups */
	int64	   *unit_rows;
	int64	   *unit_first;		/* each row group's first row's place in the file */
	int			nstats;			/* columns with statistics */
	uint8	   *stat_attnums;	/* per attribute: VPQ_STAT_* */
	VpqStat    *stats;			/* [unit * nstats + c], NULL until decoded */
	int64	   *col_bytes;		/* [unit * nstats + c] compressed bytes, or NULL */
	bytea	   *stats_raw;		/* own: the catalog's, decoded on demand */
	VpqStatsIndex *stats_index; /* own: its entries' places, made as first asked */
	const char *facts;			/* what a scan asks of its footer (engine.h), or NULL */
	Size		facts_len;
	bool		has_dead;		/* own: a tombstone the snapshot sees is in its range */
	TransactionId xmin;			/* own: its rows' writer; external: none */
} VpqFileEntry;

/* VpqFileEntry.stat_attnums: an attribute's statistics */
#define VPQ_STAT_UNREAD		0	/* not decoded yet */
#define VPQ_STAT_READ		1	/* decoded: unknown where the file has none */
#define VPQ_STAT_ABSENT		2	/* own: the file lacks the column */

typedef struct VpqUnitRef
{
	int			file;
	int			rg;
} VpqUnitRef;

typedef struct VpqListing
{
	MemoryContext cxt;
	int			refs;
	int			nfiles;
	VpqFileEntry *files;
	int			nunits;
	VpqUnitRef *units;
	int64		rows;
	int64		bytes;
	bool		any_dead;
} VpqListing;

/* listing.c */
typedef struct VpqListingStats
{
	int64		listings;		/* own tables' listings read from the catalog */
	int64		stat_columns;	/* a file's column's statistics decoded */
} VpqListingStats;

extern VpqListingStats vpq_listing_stats;
extern VpqListing *vpq_listing(Relation rel, VpqTable *t, Snapshot snapshot);
extern VpqListing *vpq_listing_all(VpqTable *t);
extern void vpq_listing_release(VpqListing *l);
extern void vpq_listings_xact_end(void);
extern Size vpq_listing_serialized_size(VpqListing *l);
extern void vpq_listing_serialize(VpqListing *l, char *out);
extern VpqListing *vpq_listing_deserialize(Relation rel, VpqTable *t, const char *in, Snapshot snapshot);
extern const VpqStat *vpq_unit_stat(VpqTable *t, VpqFileEntry *f, int rg, AttrNumber attnum);
extern int64 vpq_unit_bytes(VpqTable *t, VpqFileEntry *f, int rg, AttrNumber attnum);
extern vpq_file *vpq_open_file(VpqFileEntry *f);
extern int	vpq_file_entry_of_row(VpqListing *l, uint64 rowid);
extern const char *vpq_file_index_get(VpqFileEntry *f, Size *len);
extern void vpq_file_index_keep(VpqFileEntry *f, vpq_file *h);
extern bool vpq_device_may_read(void);

/* footers.c: external files' facts in shared memory (pg_vector_executor.md §3.34, PQ4) */
extern int	vpq_footer_cache_size;
extern bool vpq_enable_footer_cache;
extern void vpq_footers_install(void);
extern bool vpq_footers_enabled(void);
extern char *vpq_footers_get(const char *path, int64 size, int64 mtime, Size *len);
extern void vpq_footers_put(const char *path, int64 size, int64 mtime, const char *facts, Size facts_len);
extern void vpq_footers_stats(int64 *entries, int64 *bytes, int64 *hits, int64 *misses, int64 *kept);

/* stats.c: own files' statistics, as the catalog keeps them */
extern VpqStatsIndex *vpq_stats_index(bytea *raw);
extern int	vpq_stats_index_column(VpqStatsIndex *ix, AttrNumber attnum);
extern int	vpq_stats_index_row_groups(VpqStatsIndex *ix);
extern void vpq_stats_decode(VpqTable *t, bytea *raw, VpqStatsIndex *ix, int c, int rg, VpqStat *s,
							 int64 *bytes);
extern const char *vpq_stats_facts(bytea *raw, Size *len);
extern bool vpq_stat_from_footer(VpqTable *t, VpqColumn *col, vpq_file *file, int fcol, int rg,
								 VpqStat *out);
extern int	vpq_bytes_cmp(const char *a, int alen, const char *b, int blen);

/* filter.c: pruning */
typedef struct VpqPruner VpqPruner;
typedef const VpqStat *(*VpqStatFn) (void *arg, AttrNumber attnum);
extern VpqPruner *vpq_pruner_make(VpqTable *t, List *quals, int nkeys, const ScanKeyData *keys);
extern void vpq_pruner_set_keys(VpqPruner *p, int which, int nkeys, const ScanKeyData *keys);
extern bool vpq_pruner_active(VpqPruner *p);
extern bool vpq_pruner_may_match(VpqPruner *p, VpqStatFn stat, void *arg, int64 rows);

/*
 * Reading (read.c): a column of a row group as Arrow holds it, made a
 * PostgreSQL Datum, or vexec's column
 */
typedef enum VpqReadMode
{
	VPQ_RM_NULL,				/* every value NULL: the file's null type */
	VPQ_RM_MISSING,				/* the file lacks the column: its missing value */
	VPQ_RM_FIXED,				/* the PostgreSQL value's bytes, as its width */
	VPQ_RM_INT,					/* an integer to a wider one, or unsigned */
	VPQ_RM_BOOL,				/* Arrow's bits */
	VPQ_RM_FLOAT,				/* float32 or float16 to a wider float */
	VPQ_RM_DATE32,
	VPQ_RM_DATE64,
	VPQ_RM_TIME,
	VPQ_RM_TIMESTAMP,
	VPQ_RM_DECIMAL,
	VPQ_RM_INT_NUMERIC,			/* an integer to numeric */
	VPQ_RM_STRING,				/* int32 offsets */
	VPQ_RM_LARGE_STRING,		/* int64 offsets */
	VPQ_RM_VIEW,
	VPQ_RM_DICT,				/* dictionary codes of strings */
	VPQ_RM_FIXED_BINARY,		/* bytea, uuid of a fixed binary */
	VPQ_RM_IMAGE_FIXED,			/* an own file's image of a fixed-length value */
	VPQ_RM_IMAGE_VARLENA,		/* an own file's image of a varlena */
	VPQ_RM_TEXT_INPUT,			/* a string through the type's input function */
	VPQ_RM_INT_DAYS,			/* PQ6: an integer of days since 1970, a date */
	VPQ_RM_INT_SECONDS,			/* PQ6: an integer of seconds since 1970, a
								 * timestamp */
	VPQ_RM_RECV,				/* PQ15: a binary through the type's receive
								 * function */
	VPQ_RM_LIST,				/* PQ20: a list as an array (nested.c) */
	VPQ_RM_MAP_HSTORE,			/* a map of strings as hstore */
	VPQ_RM_MAP_JSONB			/* a map as a jsonb object */
} VpqReadMode;

typedef struct VpqReadCol
{
	VpqColumn  *col;
	int			fcol;			/* the file's column, -1 missing */
	VpqReadMode mode;
	vpq_type	ftype;
	Datum		missing;		/* VPQ_RM_MISSING */
	bool		missing_null;
	FmgrInfo	input;			/* VPQ_RM_TEXT_INPUT, VPQ_RM_RECV's receive function */
	Oid			ioparam;
	bool		own;			/* an own file's: its values checked as they were written */
	int32		geo_srid;		/* PQ17: the file's SRID, given to a geometry's WKB that has none; 0 none */
	const char *fname;			/* VPQ_RM_RECV: the file's column's name and the file, for its errors' context */
	const char *location;
	/* PQ20: a nested column's children -- a list's elements, a map's keys and values -- read as columns */
	struct VpqReadCol *kids;
	Oid			elemtype;		/* VPQ_RM_LIST: the array's */
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	FmgrInfo	output;			/* VPQ_RM_MAP_JSONB's children: their type's output, for text */
} VpqReadCol;

extern void vpq_read_col_setup(VpqTable *t, VpqColumn *col, vpq_file *file, VpqReadCol *rc,
							   bool own);
extern void vpq_read_col_own(VpqColumn *col, VpqReadCol *rc);
extern bool vpq_read_child_setup(VpqReadCol *rc, VpqColumn *col, int fcol, const vpq_type *ft);
extern Datum vpq_value_at(VpqReadCol *rc, const vpq_array *a, int64 row, bool *isnull);

/*
 * PQ7 (pg_vector_executor.md §3.35): a byte of a file's facts in shared
 * memory written in place -- a chunk's first data page's version, read since
 * they were kept, or its text found UTF-8 whole (PQ14, §3.37) -- nothing
 * where the file's entry is gone (footers.c): whether it was written
 */
extern bool vpq_footers_set_byte(const char *path, int64 size, int64 mtime, Size at, char value);

/*
 * GeoParquet (geo.c; pg_vector_executor.md §3.39, PQ16-PQ18): a file
 * column's geometries as GeoParquet's metadata and Parquet's geospatial
 * logical types tell of them
 */
typedef struct VpqGeoFacts
{
	bool		geo;			/* a column of geometries the file names */
	bool		spherical;		/* Parquet's GEOGRAPHY, or GeoParquet's edges "spherical" */
	bool		wkb;			/* of WKB: Parquet's types, GeoParquet's encoding "WKB" */
	int32		srid;			/* its CRS's SRID; 0 unknown */
	bool		has_bbox;		/* GeoParquet's bbox of the file's column */
	double		bbox[4];		/* xmin, xmax, ymin, ymax */
	const char *typmod_type;	/* GeoParquet's geometry_types as PostGIS's typmod names them, or NULL */
} VpqGeoFacts;

extern int	vpq_geo_type(Oid typid);
extern void vpq_geo_facts(vpq_file *file, int fcol, const char *name, VpqGeoFacts *g);
extern void vpq_geo_read_setup(VpqReadCol *rc, vpq_file *file, const char *fname);
extern int	vpq_geo_wkb_with_srid(const char *wkb, int len, int32 srid, char *out);
extern void vpq_geo_stats(vpq_file *file, int fcol, int nunits, VpqStat *stats, int stride);
extern bool vpq_geo_overlaps_func(Oid funcid);
extern bool vpq_geo_const_box(Datum geom, Oid geomtype, double box[4]);
extern bool vpq_geo_boxes_overlap(const double a[4], const double b[4]);
extern char *vpq_geo_type_sql(vpq_file *file, int fcol, const char *name);

/* A file's nested columns as arrays, hstore and jsonb (nested.c; pg_vector_executor.md §3.40, PQ20) */
extern bool vpq_nested_is_hstore(Oid typid);
extern Oid	vpq_nested_array_element(Oid typid);
extern void vpq_nested_setup(VpqTable *t, VpqReadCol *rc, vpq_file *file, const char *fname);
extern Datum vpq_nested_value(VpqReadCol *rc, const vpq_array *a, int64 i);
extern char *vpq_nested_type_sql(vpq_file *file, int fcol, const vpq_type *type);

/* PQ6: a file's integer type's bytes and signedness -- 0 for another type */
extern int	vpq_int_width(int id, bool *is_unsigned);
extern bool vpq_array_null(const vpq_array *a, int64 row);
extern int128 vpq_decimal_at(const vpq_array *a, const vpq_type *t, int64 i);
extern int64 vpq_to_usecs(int64 v, int unit);
extern const char *vpq_bytes_at(const vpq_array *a, VpqReadMode mode, int64 i, int *len);
extern int64 vpq_dict_index(const vpq_array *a, int64 i);

/*
 * Catalogs (catalog.c)
 */
typedef struct VpqFileRow
{
	int64		file_id;
	uint64		first_rowid;
	int64		rows;
	int64		bytes;
	int64		fxid;			/* the writer's FullTransactionId, in the file's name */
	bytea	   *stats;
	TransactionId xmin;			/* the catalog row's: its rows' writer, a subtransaction's own */
} VpqFileRow;

typedef enum VpqWriterState
{
	VPQ_WRITER_ABORTED,
	VPQ_WRITER_US,
	VPQ_WRITER_IN_PROGRESS,
	VPQ_WRITER_COMMITTED
} VpqWriterState;

extern uint64 vpq_catalog_writes;
extern void vpq_catalog_init(void);
extern Snapshot vpq_catalog_snapshot(bool *registered);
extern void vpq_catalog_snapshot_done(Snapshot snapshot, bool registered);
extern Relation vpq_catalog_open(const char *name, char persistence, LOCKMODE lockmode);
extern Oid	vpq_catalog_index(Relation catalog);
extern int	vpq_catalog_storage_keys(const VpqStorage *st, ScanKeyData *keys);
extern void vpq_catalog_insert(Relation catalog, HeapTuple tuple, CommandId cid);
extern VpqWriterState vpq_writer_state(HeapTupleHeader h);
extern bool vpq_row_deleted(HeapTupleHeader h);
extern void vpq_storage_create(const VpqStorage *st);
extern void vpq_storage_forget(const VpqStorage *st);
extern void vpq_storage_truncate(const VpqStorage *st);
extern bool vpq_storage_exists(const VpqStorage *st);
extern void vpq_counters_take(const VpqStorage *st, uint64 nrowids, uint64 nfiles, uint64 *first_rowid,
							  uint64 *first_file);
extern void vpq_counters_xact_callback(XactEvent event);
extern bool vpq_counters_pending(void);
extern bool vpq_tombstones_pending(void);
extern bool vpq_write_pending(void);
extern List *vpq_files_visible(const VpqStorage *st, Snapshot snapshot);
extern List *vpq_files_all(const VpqStorage *st);
extern bool vpq_file_of_row(const VpqStorage *st, uint64 rowid, Snapshot snapshot, VpqFileRow *out);
extern void vpq_file_insert(const VpqStorage *st, const VpqFileRow *row, CommandId cid);
extern void vpq_files_copy(const VpqStorage *from, const VpqStorage *to, const char *from_dir,
						   const char *to_dir);
extern void vpq_totals(const VpqStorage *st, Snapshot snapshot, int64 *rows, int64 *bytes,
					   int64 *files);

/*
 * Tombstones (tombstones.c): a row's deletion, update -- naming the new
 * version's row id -- or lock, as ch_table_engines' are
 */
#define VPQ_TOMB_DELETED	'd'
#define VPQ_TOMB_UPDATED	'u'
#define VPQ_TOMB_LOCKED		'l'

typedef enum VpqRowStatus
{
	VPQ_ROW_LIVE,
	VPQ_ROW_BEING_MODIFIED,
	VPQ_ROW_SELF_MODIFIED,
	VPQ_ROW_MODIFIED
} VpqRowStatus;

typedef struct VpqRowState
{
	VpqRowStatus status;
	TransactionId xid;
	CommandId	cid;
	bool		updated;
	bool		lock_only;
	bool		locked_by_us;	/* the running transaction holds a lock at least as strong */
	bool		lock_ours;		/* the running transaction holds a lock of any mode */
	uint64		successor;
} VpqRowState;

/* Whether two row lock modes conflict, as heap's (heapam.c, tupleLockExtraInfo's MultiXact rules) */
static inline bool
vpq_lock_modes_conflict(int held, int wanted)
{
	/* LockTupleKeyShare 0, LockTupleShare 1, LockTupleNoKeyExclusive 2, LockTupleExclusive 3 */
	static const bool conflicts[4][4] = {
		{false, false, false, true},
		{false, false, true, true},
		{false, true, true, true},
		{true, true, true, true},
	};

	return conflicts[held & 3][wanted & 3];
}

extern void vpq_row_state(const VpqStorage *st, uint64 rowid, int mode, VpqRowState *state);
extern TransactionId vpq_row_xmax(const VpqStorage *st, uint64 rowid);
extern void vpq_tombstone_insert(const VpqStorage *st, uint64 rowid, char kind, uint64 successor,
								 CommandId cid);
extern uint64 *vpq_tombstones_visible(const VpqStorage *st, Snapshot snapshot, uint64 lo, uint64 hi,
									  int *n);
extern bool vpq_tombstones_visible_any(const VpqStorage *st, Snapshot snapshot);
extern bool vpq_tombstone_visible(const VpqStorage *st, Snapshot snapshot, uint64 rowid,
								  uint64 *successor);
extern void vpq_tombstones_xact_callback(XactEvent event);
extern void vpq_tombstones_copy(const VpqStorage *from, const VpqStorage *to);
extern bool vpq_row_survives(const VpqStorage *st, uint64 rowid, Snapshot snapshot);
extern void vpq_conflict_out(Relation rel, const VpqStorage *st, Snapshot snapshot);

/* A deleted row an index build hands as not alive */
typedef struct VpqDeadRow
{
	uint64		rowid;
} VpqDeadRow;

extern VpqDeadRow *vpq_tombstones_all(const VpqStorage *st, int *n);
extern uint64 *vpq_tombstones_dead_to_all(const VpqStorage *st, TransactionId horizon, int *n,
										  int64 *recently_dead);

/* dml.c */
extern TM_Result vpq_tuple_delete(Relation rel, ItemPointer tid, CommandId cid, uint32 options,
								  Snapshot snapshot, Snapshot crosscheck, bool wait,
								  TM_FailureData *tmfd);
extern TM_Result vpq_tuple_update(Relation rel, ItemPointer otid, TupleTableSlot *slot,
								  CommandId cid, uint32 options, Snapshot snapshot,
								  Snapshot crosscheck, bool wait, TM_FailureData *tmfd,
								  LockTupleMode *lockmode, TU_UpdateIndexes *update_indexes);
extern TM_Result vpq_tuple_lock(Relation rel, ItemPointer tid, Snapshot snapshot,
								TupleTableSlot *slot, CommandId cid, LockTupleMode mode,
								LockWaitPolicy wait_policy, uint8 flags, TM_FailureData *tmfd);
extern void vpq_tuple_get_latest_tid(TableScanDesc scan, ItemPointer tid);

/*
 * Writes (write.c): the write buffer, files written, deletions at the end
 * of transactions
 */
extern void vpq_write_init(void);
extern void vpq_buffer_insert(Relation rel, TupleTableSlot *slot, CommandId cid, bool counted);
extern uint64 vpq_buffer_next_rowid(Relation rel, CommandId cid);
extern void vpq_buffer_flush_rel(Oid relid);
extern void vpq_buffer_flush_all(void);
extern void vpq_buffer_flush_visible(Relation rel, Snapshot snapshot);
extern bool vpq_buffer_fetch(Relation rel, uint64 rowid, Snapshot snapshot, TupleTableSlot *slot);
extern bool vpq_buffer_has_row(Relation rel, uint64 rowid, Snapshot snapshot);
extern bool vpq_buffer_row_cid(Relation rel, uint64 rowid, CommandId *cid);
extern void vpq_buffer_discard_rel(Oid relid);
extern void vpq_pending_delete(const char *path, bool dir, bool at_commit);
extern void vpq_note_new_storage(const VpqStorage *st);
extern void vpq_storage_drop_later(const VpqStorage *st, const char *dir);

/* the sink's columns: their layouts, and a batch's rows */
struct VexecSinkLayout;
struct VexecColumn;
extern bool vpq_sink_layout(VpqTable *t, AttrNumber attnum, struct VexecSinkLayout *layout);
extern void vpq_buffer_put_batch(Relation rel, CommandId cid, int nrows,
								 const struct VexecColumn *columns, ItemPointerData *tids);

/*
 * Scans (scan.c)
 */
typedef struct VpqScanDescData *VpqScanDesc;

extern TableScanDesc vpq_scan_begin(Relation rel, Snapshot snapshot, int nkeys, ScanKeyData *key,
									ParallelTableScanDesc pscan, uint32 flags);
extern void vpq_scan_end(TableScanDesc scan);
extern void vpq_scan_rescan(TableScanDesc scan, ScanKeyData *key, bool set_params, bool allow_strat,
							bool allow_sync, bool allow_pagemode);
extern bool vpq_scan_getnextslot(TableScanDesc scan, ScanDirection direction, TupleTableSlot *slot);
extern Size vpq_parallelscan_estimate(Relation rel);
extern Size vpq_parallelscan_initialize(Relation rel, ParallelTableScanDesc pscan);
extern void vpq_parallelscan_reinitialize(Relation rel, ParallelTableScanDesc pscan);
extern bool vpq_scan_analyze_next_block(TableScanDesc scan, struct ReadStream *stream);
extern bool vpq_scan_analyze_next_tuple(TableScanDesc scan, double *liverows, double *deadrows,
										TupleTableSlot *slot);
extern bool vpq_scan_bitmap_next_tuple(TableScanDesc scan, TupleTableSlot *slot, bool *recheck,
									   uint64 *lossy_pages, uint64 *exact_pages);
extern bool vpq_scan_sample_next_block(TableScanDesc scan, struct SampleScanState *scanstate);
extern bool vpq_scan_sample_next_tuple(TableScanDesc scan, struct SampleScanState *scanstate,
									   TupleTableSlot *slot);
extern bool vpq_fetch_row(Relation rel, ItemPointer tid, Snapshot snapshot, TupleTableSlot *slot);
extern void vpq_fetch_reset(void);
extern void vpq_fetch_set_columns(Relation rel, TupleTableSlot *slot, int n, const AttrNumber *attnums);

/* The process's fetches by TID (scan.c), as vexec_parquet.fetch_stats() shows them */
typedef struct VpqFetchStats
{
	int64		fetches;		/* rows read from files by their TIDs */
	int64		values;			/* the columns read for them, summed */
	int64		pages;			/* a column's data page read alone (a point read) */
	int64		indexed;		/* of those, found by the file's offset index */
	int64		kept;			/* a column read from a page or chunk a fetch before kept */
	int64		whole;			/* a column's chunk read whole */
} VpqFetchStats;

extern VpqFetchStats vpq_fetch_stats;

/*
 * The process's scans' reads (pg_vector_executor.md §3.37), beside the
 * engine's own counters: batches of external text checked at once (PQ9) and
 * values checked one by one; units read as a range of a row group's rows
 * (PQ12); batches whose later steps' columns were decoded by their pages
 * (PQ13)
 */
typedef struct VpqScanStats
{
	int64		text_batches;
	int64		text_values;
	int64		text_known;		/* PQ14: batches of chunks found UTF-8 whole before */
	int64		text_marked;	/* PQ14: chunks marked so */
	int64		units_cut;
	int64		step_batches;
} VpqScanStats;

extern VpqScanStats vpq_scan_stats;

/* the scan's listing and claims, for vexec's source (vexec.c) */
extern TableScanDesc vpq_scan_begin_all(Relation rel);
extern VpqListing *vpq_scan_listing(TableScanDesc scan);
extern VpqTable *vpq_scan_table(TableScanDesc scan);
extern bool vpq_scan_claim_unit(TableScanDesc scan, int *unit);
extern bool vpq_scan_is_parallel(TableScanDesc scan);
extern bool vpq_scan_claim_slot(TableScanDesc scan, int nslots, int *slot);
extern uint64 *vpq_scan_dead_rows(TableScanDesc scan, VpqFileEntry *f, uint64 lo, uint64 hi, int *n);

/* am.c */
extern void vpq_relation_dropped(Oid relid);
extern bool vpq_storage_has_files(Relation rel);
extern void vpq_external_check(Relation rel);
extern void vpq_sweep(Relation rel, VpqTable *t);

/* functions.c, the extension's SQL */

#endif							/* VEXEC_PARQUET_H */
