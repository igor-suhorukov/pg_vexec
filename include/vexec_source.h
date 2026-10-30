/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_source.h
 *	  The batch-source contract: how a table access method hands vexec its
 *	  rows a batch of columns at a time (pg_vector_executor.md §3.5.1).
 *
 * A storage module -- PAX, gp_ao, or any access method outside the port --
 * fills a VexecSourceRoutine and registers it from its _PG_init.  vexec
 * finds it by the relation's rd_tableam when it plans and runs a scan.
 * Neither links the other: the registry is reached through a rendezvous
 * variable, and everything in this header is a type, a macro or an inline
 * function, so a module that includes it needs nothing of vexec's at load
 * time, and there is no order between the modules in
 * "shared_preload_libraries".  Without vexec a registration is an unused
 * entry; without the storage module there are no such tables.
 *
 * Versions.  The major version is in the rendezvous name: a change that an
 * older module cannot read makes a new name, and the old one stays empty.
 * Minor additions go at the end of a struct, whose "size" says how much of
 * it the module that filled it was built with; a member past that size
 * reads as absent.
 *
 * Every call runs on the backend's main thread and may ereport.  MVCC,
 * pruning and deletes stay inside the storage module (§3.1, principle 6).
 *
 * vexec installs this header with itself, into the server's include
 * directory under extension/vexec/, so that a module built elsewhere can
 * implement it.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_SOURCE_H
#define VEXEC_SOURCE_H

#include "access/relscan.h"
#include "access/skey.h"
#include "access/tableam.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "storage/itemptr.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapshot.h"

#define VEXEC_SOURCE_RENDEZVOUS	"vexec/source_v1"	/* the major version is
													 * in the name */
#define VEXEC_SOURCE_MINOR		19	/* 1: aggregate; 2: set_keys; 3:
									 * dictionary_id, estimate_ex; 4:
									 * fetch_columns, ordering, a fetch's
									 * price; 5: columns in steps; 6:
									 * set_join_keys; 7: next_unit,
									 * read_unit; 8: MergeTree's units,
									 * read_unit_bytes; 9: unit_tids; 10:
									 * a column's flags, granules passed
									 * over; 11: a unit's granules' rows and
									 * its part's identity; 12: a unit of
									 * several ranges of granules, a sole
									 * claimer's units; 13: a unit's bytes,
									 * integers of days and of seconds; 14:
									 * a fetch's threads; 15: the nested
									 * layouts, LIST and MAP; 16: a scan's
									 * and a planning's figures; 17:
									 * set_keys() by granules; 18: an
									 * estimate's raw bytes and parallel
									 * units; 19: count(*) under a scan's
									 * qual by aggregate() */

/*
 * How a column holds its values (§3.4).  A format -- PostgreSQL's or
 * Arrow's -- is a choice of layouts per type; a source fills each column in
 * its storage's own layout, or in the one VexecSourceSpec.layouts asks for
 * when it can do so without copying the bytes, and names the layout it
 * used.  vexec converts the rest.
 */
typedef enum VexecLayout
{
	VEXEC_FIXED,				/* the type's width a row, or PostgreSQL's
								 * array stride */
	VEXEC_BYTE_BOOL,			/* a byte a row */
	VEXEC_BIT_BOOL,				/* a bit a row, least significant first */
	VEXEC_SCALED,				/* numeric as int64 or int128, at the
								 * typmod's scale */
	VEXEC_DATUM,				/* a Datum a row, pointing at a headered
								 * value */
	VEXEC_VIEW,					/* Arrow's binary view: 16 bytes a row, and
								 * data buffers */
	VEXEC_OFFSETS,				/* Arrow's binary: int32 offsets, and the
								 * bytes */
	VEXEC_LIST,					/* from minor version 15: Arrow's list,
								 * int32 offsets into its elements */
	VEXEC_MAP					/* from minor version 15: Arrow's map, int32
								 * offsets into its keys and values */
} VexecLayout;

#define VEXEC_NUM_LAYOUTS	(VEXEC_MAP + 1)

/* How a column's rows map to its values (§3.4.3). */
typedef enum VexecEncoding
{
	VEXEC_FLAT,					/* a value a row */
	VEXEC_CONST,				/* one value for every row */
	VEXEC_DICT					/* codes into a dictionary column */
} VexecEncoding;

/*
 * A column of a source's batch.  The pointers are the source's: vexec only
 * reads through them, until the batch is released.
 *
 * validity is a bitmap of uint64 words, least significant bit first, 1 =
 * valid, as Arrow's and as a heap tuple's null bitmap; NULL when the column
 * has no NULLs.  A row the source has deleted is never NULL here: it is left
 * out of VexecSourceBatch.visible.
 *
 * What each layout reads, beside values:
 *
 *	FIXED	width and stride, and arrow_values for date, timestamp[tz] and
 *			interval.  A by-value type at its width (stride = width); a
 *			by-reference one of fixed length -- uuid, interval, name -- at
 *			its width or PostgreSQL's array stride (its length aligned).
 *	SCALED	width and scale: width 8 for a typmod of 18 digits or fewer
 *			(precision, and -scale for a negative scale), 16 for up to 38,
 *			stride = width.  Another width is converted, a value at a time.
 *	OFFSETS	nvalues + 1 int32 offsets into buffers[0], whose size is
 *			buffer_sizes[0]; they need not start at 0, and a NULL row's
 *			pair must still lie in the buffer, as Arrow's does.
 *	VIEW	the views, and buffers and buffer_sizes for the bytes past 12.
 *	LIST, MAP (from minor version 15, pg_vector_executor.md §3.40's PQ21) --
 *			an array's value as Arrow's list holds it, hstore's or jsonb's as
 *			its map does: nvalues + 1 int32 offsets into the column's
 *			children (VexecSourceBatch.children), a row's of them those of
 *			[offsets[row], offsets[row + 1]).  A source hands one only where
 *			VexecSourceSpec.layouts asks for it, and gives each row's value
 *			as a Datum of the column's type too (nested_datum): vexec reads
 *			the children in its kernels, and the Datum wherever else a row's
 *			value is read.
 *
 * A DICT column (VEXEC_SRC_DICT asks for one) has a code a row, each in
 * [0, ndictionary), and validity a bit a row; its dictionary is FLAT, in
 * one of the layouts above, and holds no NULL: a NULL row is one whose
 * validity bit is clear, its code any of the dictionary's.  vexec checks
 * both.
 *
 * vexec ignores the fields a layout does not read: a source may leave them
 * as it likes.
 *
 * From minor version 10, flags, in what was the padding byte after
 * arrow_values, so that the struct's size and members stay where they were:
 * vexec reads them only of a source of minor version 10 or later, which
 * sets them or leaves them 0.
 */
typedef struct VexecColumn
{
	uint8		layout;			/* VexecLayout */
	uint8		encoding;		/* VexecEncoding */
	bool		arrow_values;	/* date, timestamp[tz], interval: Arrow's
								 * epoch and struct */
	uint8		flags;			/* from minor version 10: VEXEC_COL_* */
	int16		scale;			/* SCALED: digits after the point */
	int32		width;			/* FIXED: bytes a value; SCALED: 8 or 16 */
	int32		stride;			/* FIXED: bytes from one value to the next */
	const uint64 *validity;		/* NULL: no NULLs; else 1 = valid */
	const void *values;			/* the values, Datums, views or offsets */
	const void *const *buffers; /* VIEW, OFFSETS: the bytes */
	const int64 *buffer_sizes;	/* VIEW: each buffer's size in bytes */
	int			nbuffers;
	const Datum *datums;		/* VIEW: the Datums kept beside the views, or
								 * NULL */
	const int32 *codes;			/* DICT: indexes into the dictionary */
	const struct VexecColumn *dictionary;	/* DICT: its values */
	int			ndictionary;	/* DICT: how many */
} VexecColumn;

/*
 * VexecColumn.flags, from minor version 10.
 *
 * VEXEC_COL_TEXT_WHOLE: a text column's values -- a DICT column's
 * dictionary's -- each the database encoding's whole, no NUL, no truncated
 * or invalid character, as the source vouches: written through
 * PostgreSQL's input functions, as every Datum a table access method's row
 * is, and kept as they were.  vexec checks none of them again, as
 * PostgreSQL checks none of a stored value's (pg_vector_executor.md §3.28,
 * F5); a source that cannot say so leaves it clear, and vexec checks them.
 */
#define VEXEC_COL_TEXT_WHOLE	0x01
/*
 * From minor version 19: an OFFSETS column asked for by VexecSourceSpec's
 * length_columns, its offsets counting each value's characters, no byte behind
 * them; NOT NULL
 */
#define VEXEC_COL_LENGTHS		0x02

/* VexecSourceSpec.flags */
#define VEXEC_SRC_COMPACT	0x0001	/* hand only visible rows */
#define VEXEC_SRC_TIDS		0x0002	/* fill VexecSourceBatch.tids */
#define VEXEC_SRC_DICT		0x0004	/* dictionary columns are welcome: a
									 * column of a type's bytes, its
									 * dictionary in the layout asked */
#define VEXEC_SRC_ORDERED	0x0008	/* from minor version 4: a serial scan
									 * hands its rows in ordering()'s order
									 * where it can */
#define VEXEC_SRC_STEPS		0x0010	/* from minor version 5: the columns
									 * come in steps, the first from next(),
									 * each later one from a call of
									 * next_rest() (nsteps, step_ends) */
#define VEXEC_SRC_COUNT_QUALS 0x0020	/* from minor version 19: aggregate()
										 * may be asked count(*) under the
										 * spec's quals */

/* At most this many steps */
#define VEXEC_SRC_MAX_STEPS	8

/* What a scan asks of a source. */
typedef struct VexecSourceSpec
{
	Size		size;			/* sizeof as the caller was built */
	int			ncolumns;
	const AttrNumber *attnums;	/* 1..natts, ascending -- within each step,
								 * with VEXEC_SRC_STEPS: a TID is asked for
								 * with VEXEC_SRC_TIDS, never as a column */
	const uint8 *layouts;		/* per column: the layout the batch format
								 * asks for */
	List	   *quals;			/* ANDed Exprs over the relation's own Vars:
								 * pruning only */
	int			nkeys;
	ScanKeyData *keys;			/* runtime filters, as scan keys */
	int			max_rows;		/* 0: the source's own unit */
	uint32		flags;			/* VEXEC_SRC_* */

	/*
	 * From minor version 5, with VEXEC_SRC_STEPS: the columns in nsteps
	 * steps, 2 to VEXEC_SRC_MAX_STEPS, step s the columns [step_ends[s - 1],
	 * step_ends[s]) -- step 0 from column 0, the last ending at ncolumns,
	 * none empty
	 */
	int			nsteps;
	const int  *step_ends;

	/*
	 * From minor version 10 (VC30; pg_vector_executor.md §3.28, F7), vexec's
	 * condition cache.  Where set, a source whose units are granules of
	 * storage that never changes while it lives -- a MergeTree part's marks
	 * -- calls it as it selects a unit's granules, with the unit's identity
	 * (VexecSourceBatch's unit_id), its granules' count, and those of them
	 * it is to read, [begin, end): NULL passes none over, else a bit a
	 * granule of the unit's, set for one no row of which is true of the
	 * scan's quals, which the source then leaves unread.  The bits are valid
	 * until the next call.  Each batch then names its rows' granule, so that
	 * vexec learns the granules none of whose rows its quals passed.
	 */
	const uint64 *(*granules_passed) (void *arg, const uint64 *unit_id, uint32 ngranules, uint32 begin,
									  uint32 end);
	void	   *granules_arg;

	/*
	 * From minor version 12 (W26, pg_vector_executor.md §3.28): whether vexec
	 * takes a VEXEC_UNIT_MERGETREE unit of several ranges of its part's
	 * granules (VexecSourceUnit's stream_rows), so that a source cuts no unit
	 * at the granules granules_passed passes over -- a unit costs a device far
	 * more than a granule's rows do -- but leaves them out of its ranges
	 */
	bool		unit_ranges;

	/*
	 * From minor version 17 (F25, pg_vector_executor.md §3.38): set_keys()'s
	 * keys may pass over the granules of a unit the source reads, not only
	 * whole units (set_keys(), below)
	 */
	bool		granule_keys;

	/*
	 * From minor version 19 (F35 and F37, §3.38): NULL, or per column whether
	 * vexec reads its values only for their length in characters and whether
	 * they are empty.  The source may then hand the column as
	 * VEXEC_COL_LENGTHS, from something it keeps beside the values; a source
	 * that keeps none hands the values as ever.
	 */
	const uint8 *length_columns;
} VexecSourceSpec;

/* One batch from a source. */
typedef struct VexecSourceBatch
{
	int			nrows;
	const uint64 *visible;		/* NULL: every row; else nrows bits, 1 =
								 * visible */
	const ItemPointerData *tids;	/* when VEXEC_SRC_TIDS */
	VexecColumn *columns;		/* the layouts of §3.4 */
	void	   *owner;			/* what retain/release name */

	/*
	 * From minor version 10, where the spec has granules_passed: the unit the
	 * batch's rows are of, by an identity no other unit has while vexec may
	 * keep it -- zeros for none -- the granule of the unit every one of them
	 * is of, -1 where the source cannot say, and whether the batch holds the
	 * granule's last row, a granule coming in one batch or more.  vexec
	 * zeroes them before each next(), and reads them of a source of minor
	 * version 10 or later; a source writes them only where the spec, of
	 * that size, has granules_passed set.
	 */
	uint64		unit_id[2];
	int64		granule;
	bool		granule_last;

	/*
	 * From minor version 15, where a column is of a nested layout: its
	 * children -- VEXEC_LIST's elements at children[2 * j], VEXEC_MAP's keys
	 * there and its values at children[2 * j + 1], j the column's place in
	 * the spec -- each FLAT, of the layouts above, its rows those the
	 * offsets name, from 0; and the value of row `row` of column j as a
	 * Datum of the column's type, made in the caller's memory context,
	 * valid while the batch is.  vexec zeroes them before each next(), and
	 * reads them of a source of minor version 15 or later.
	 */
	const VexecColumn *const *children;
	Datum		(*nested_datum) (void *owner, int column, int row, bool *isnull);
} VexecSourceBatch;

/*
 * An aggregate a source may answer from the statistics it keeps of a unit
 * of its own -- a file, a group of rows -- in place of the unit's rows
 * (pg_vector_executor.md §3.14, H2): aggregate(), from minor version 1.
 */
typedef enum VexecSourceAggKind
{
	VEXEC_SRC_AGG_ROWS,			/* the unit's rows: count(*) */
	VEXEC_SRC_AGG_COUNT,		/* its values of attnum that are not NULL */
	VEXEC_SRC_AGG_MIN,			/* the least of them, by the btree ordering
								 * of the type's min() */
	VEXEC_SRC_AGG_MAX,			/* the greatest */
	VEXEC_SRC_AGG_SUM			/* their sum, as the type's sum() returns it */
} VexecSourceAggKind;

typedef struct VexecSourceAgg
{
	uint8		kind;			/* VexecSourceAggKind */
	AttrNumber	attnum;			/* 0 for ROWS */
	Oid			type;			/* the answer's: int8 for ROWS and COUNT,
								 * the column's for MIN and MAX, sum()'s
								 * result for SUM */
	Oid			collation;		/* MIN and MAX: the aggregate's */
} VexecSourceAgg;

typedef struct VexecSourceAggAnswer
{
	Datum		value;
	bool		isnull;			/* MIN, MAX, SUM: no value is not NULL */
} VexecSourceAggAnswer;

/*
 * What a scan of a spec's columns would read (from minor version 3,
 * estimate_ex()): estimate()'s two numbers, and what the planner needs
 * beyond them.
 */
typedef struct VexecSourceEstimate
{
	Size		size;			/* sizeof as the caller was built */
	double		rows;			/* rows handed, after pruning: estimate()'s */
	double		bytes;			/* the columns' bytes read: estimate()'s */
	double		units;			/* the units a serial scan reads, each of which
								 * aggregate() may answer */
	uint32		flags;			/* VEXEC_EST_* */
	double		fetch;			/* from minor version 4: a row fetched by its
								 * TID, the spec's columns alone read
								 * (fetch_columns()), in the planner's units;
								 * 0 where the source does not price it */
	double		fetch_threads;	/* from minor version 14: the threads of the
								 * source's own a fetch reads its columns on
								 * at once, no more than the machine's
								 * processors -- a parallel plan's
								 * participants sharing a sort's rows then
								 * fetch as many at once as the processors
								 * hold; 0 or 1 its caller's thread */
	double		raw;			/* from minor version 18: the columns' bytes
								 * read as they are decoded, uncompressed --
								 * the work a row makes; 0 unknown */
	double		parallel_units; /* from minor version 18: the units a
								 * parallel scan of what the pruning keeps
								 * is cut into, which no more participants
								 * than share; 0 unknown */
} VexecSourceEstimate;

/*
 * aggregate() would answer count(*), and count, min and max of every one of
 * the spec's columns, for every unit of a serial scan with no qual: its
 * statistics are exact there, and no row of the units is deleted
 */
#define VEXEC_EST_AGG_EXACT		0x0001

/*
 * Minor version 19 (pg_vector_executor.md §3.38, F36): aggregate() would answer
 * count(*) of every unit of a serial scan begun with the spec's quals
 * (VEXEC_SRC_COUNT_QUALS), each unit's rows that satisfy them
 */
#define VEXEC_EST_COUNT_QUALS	0x0002

/*
 * A unit a source hands for the GPU's executor to read itself, from its file
 * (from minor version 7, next_unit(); pg_vector_executor.md §3.25, N2):
 * some row groups of a Parquet file, some record batches of an Arrow IPC
 * file.  The file is named by its path and pinned by the identity the
 * source's listing gave it, which the executor checks before it reads; the
 * spec's columns by their names in the file, each with what the source's
 * own reader checks of its values as it reads them, which the executor
 * checks too -- a unit that fails a check is read again on the CPU, whose
 * reader raises the error.  A date is read as days, a timestamp as its
 * file's unit, both from 1970-01-01, a decimal at its file's scale; the
 * executor moves them to PostgreSQL's.
 */
typedef enum VexecUnitFormat
{
	VEXEC_UNIT_CPU,				/* not the device's: next() hands its rows */
	VEXEC_UNIT_PARQUET,
	VEXEC_UNIT_ARROW,			/* Arrow IPC's file format */
	VEXEC_UNIT_MERGETREE		/* from minor version 8: granules of a part */
} VexecUnitFormat;

/*
 * From minor version 8 (pg_vector_executor.md §3.26, W6): a unit of granules
 * of a MergeTree part, its spec's columns as the part's files hold them:
 * each column's streams, compressed blocks of ClickHouse's format -- a
 * 16-byte checksum, a method's byte, the block's and its data's sizes --
 * of its values, fixed-width little-endian or a String's bytes, of a
 * String's sizes, a uint64 each, and of a Nullable's null map, a byte each,
 * 1 a NULL; or, a column of the sparse serialization, of its offsets --
 * VarUInts, each the rows of its default before the next value, a granule's
 * last its rows of the default after them with bit 62 set -- its values'
 * streams then those of its other rows alone, the default the type's 0 or
 * empty string, or NULL (SPARSE_NULLS).  The device decompresses and decodes
 * them itself; their bytes,
 * which the source's storage alone may read -- PostgreSQL's buffers, where
 * the part's pages are the relation's -- are copied by read_unit_bytes().
 * path names the unit -- the relation, the part and its granules -- for
 * as long as the part is, which never changes, as the device's cache keys
 * it; file_size and file_mtime are 0.
 */
#define VEXEC_STREAM_VALUES		1
#define VEXEC_STREAM_SIZES		2
#define VEXEC_STREAM_NULLS		3
#define VEXEC_STREAM_SPARSE		4
#define VEXEC_STREAM_SPARSE_NULLS	5

typedef struct VexecSourceStream
{
	int			column;			/* the spec column it is of */
	int			kind;			/* VEXEC_STREAM_* */
	int			width;			/* VALUES: a value's bytes, 0 a String's */
	uint64		length;			/* its compressed blocks' bytes */
	uint64		skip;			/* their data's bytes before the unit's
								 * first row */
} VexecSourceStream;

/* VexecSourceUnit.column_flags */
#define VEXEC_UNIT_NOT_NULL		0x01	/* a NULL is an error */
#define VEXEC_UNIT_CHECK_UTF8	0x02	/* a string that is not UTF-8, or holds a
										 * zero byte, is an error */
/*
 * From minor version 13 (PQ6, pg_vector_executor.md §3.35): the file's
 * integer counts days since 1970, the column a date -- or seconds since
 * 1970, a timestamp -- which the source's reader makes so; its values within
 * the type's range, as the source found them
 */
#define VEXEC_UNIT_DAYS			0x04
#define VEXEC_UNIT_SECONDS		0x08

typedef struct VexecSourceUnit
{
	Size		size;			/* sizeof as the caller was built */
	uint8		format;			/* VexecUnitFormat */
	const char *path;			/* the file, an absolute path */
	uint64		file_size;		/* its identity, as the source listed it */
	int64		file_mtime;		/* nanoseconds since 1970 */
	int			nparts;			/* its row groups or record batches: */
	const int32 *parts;			/* their indexes in the file, ascending */
	uint64		rows;			/* their rows, deleted ones among them */
	const char *const *names;	/* per spec column: the file's name of it */
	const uint8 *column_flags;	/* per spec column: VEXEC_UNIT_* */
	const uint64 *deleted;		/* rows bits, 1 = a row the snapshot does not
								 * see; NULL: it sees every one */
	void	   *token;			/* the source's own: what read_unit() names */

	/* from minor version 8, VEXEC_UNIT_MERGETREE's: its columns' streams */
	int			nstreams;
	const VexecSourceStream *streams;

	/*
	 * From minor version 9, a unit of a scan of TIDs: the column of its
	 * rows' ids in its file, an unsigned 64-bit integer each, whose TIDs
	 * row_tids() makes; NULL where unit_tids() gives them by their places
	 */
	const char *rowid_name;

	/*
	 * From minor version 11 (W21, pg_vector_executor.md §3.28), a
	 * VEXEC_UNIT_MERGETREE unit's place in its part, for vexec's condition cache
	 * (minor 10's granules_passed): the part's identity, as VexecSourceBatch's
	 * unit_id has it -- zeros for none -- its granules' count, and each of the
	 * unit's nparts granules' rows, in their order, so that the device says of
	 * each granule whether a row of it passed the scan's quals; NULL where the
	 * source does not say.  A unit of granules the cache passes over is handed
	 * without them: a source cuts its units at them.
	 */
	uint64		unit_id[2];
	uint32		ngranules;
	const uint32 *granule_rows;

	/*
	 * From minor version 12 (W26, pg_vector_executor.md §3.28), where the
	 * spec has unit_ranges: a VEXEC_UNIT_MERGETREE unit of several ranges of
	 * its part's granules -- its parts and granule_rows those of each range in
	 * turn -- its streams each range's in turn, a column's again for each:
	 * each stream's range's rows, so that the device decodes each range alone
	 * and puts them together; NULL where the unit is one range, each stream
	 * the unit's rows'
	 */
	const uint32 *stream_rows;

	/*
	 * From minor version 13 (W38, pg_vector_executor.md §3.35): the bytes of
	 * the unit's spec columns before their compression, as its files say --
	 * the device's decoding of them takes about these, and their values'
	 * width once more where a dictionary encodes them -- 0 where the source
	 * does not say.  What vexec sizes a units request by, with its columns'
	 * widths.
	 */
	uint64		bytes;
} VexecSourceUnit;

/*
 * From minor version 16 (pg_vector_executor.md §3.41, OT5): a figure of the
 * source's own -- what a scan of it read, or what planning its scans read --
 * for pg_otel_probe's spans: a key under the source's prefix,
 * "vexec_parquet.units_read", valid for the life of the process, and a count.
 */
typedef struct VexecSourceFigure
{
	const char *key;
	int64		value;
} VexecSourceFigure;

/* The most figures of a scan, and of a planning, one source gives */
#define VEXEC_SOURCE_FIGURES_MAX	4

/*
 * A source.  vexec begins the scan itself through the generic wrappers
 * (table_beginscan, table_beginscan_parallel), so the access method keeps
 * the snapshot's registration, predicate locks, parallel units, rescan and
 * end; begin() is handed that scan.  Columns are named by attribute number,
 * never by a PlanState.  next() counts the visible rows it hands in the
 * statistics, as the access method's getnextslot counts each row it
 * returns (pgstat_count_heap_getnext), so that a table's figures do not
 * depend on the executor that read it.
 */
typedef struct VexecSourceRoutine
{
	Size		size;			/* later members read as absent for an older
								 * module */
	int			minor;			/* VEXEC_SOURCE_MINOR it was built with */
	const TableAmRoutine *am;	/* the key: rel->rd_tableam */
	const char *name;			/* for EXPLAIN and errors */
	bool		(*supports) (Relation rel, AttrNumber attnum);
	void	   *(*begin) (TableScanDesc scan, const VexecSourceSpec *spec);
	bool		(*next) (void *state, VexecSourceBatch *out);	/* false at the end */
	void		(*retain) (void *state, void *owner);	/* keep a batch past
														 * next() */
	void		(*release) (void *state, void *owner);
	void		(*rescan) (void *state);
	void		(*end) (void *state);
	/* optional: the needed columns' bytes after pruning, for the cost model */
	void		(*estimate) (Relation rel, Snapshot snapshot,
							 const VexecSourceSpec *spec,
							 double *rows, double *bytes);

	/*
	 * Optional, from minor version 1: every request answered from the
	 * statistics of the scan's next unit, which is then passed over as read
	 * -- true -- or the unit left to next(), which hands its rows as it
	 * would have -- false.  Called only between batches, of a scan begun
	 * with no quals and no keys -- or under count_under_quals() (minor 19);
	 * a source answers only at the start of a
	 * unit, for one none of whose rows is deleted, where its statistics are
	 * exact.  *nrows is the unit's rows, which it counts in the statistics
	 * as next() counts the rows it hands.  A by-reference answer lives
	 * until the next call.
	 */
	bool		(*aggregate) (void *state, int nreqs, const VexecSourceAgg *reqs,
							  VexecSourceAggAnswer *answers, int64 *nrows);

	/*
	 * Optional, from minor version 2: keys the scan's rows are bounded by
	 * as it runs, ANDed with VexecSourceSpec.keys and replacing the last
	 * given -- a VecSort's running bound, "the first key at most the N-th
	 * row's" (pg_vector_executor.md §3.14, H6), as a btree scan key of
	 * BTLessEqualStrategyNumber or BTGreaterEqualStrategyNumber.  Pruning
	 * only: from the next unit the source begins -- a file, a group -- it
	 * may pass over one whose statistics show that none of its rows
	 * satisfies them, and never a row of a unit it reads; from minor
	 * version 17, where the spec's granule_keys is set, a granule of a unit
	 * it reads too, one it has not begun, whose statistics show the same --
	 * a granule's rows passed over whole or not at all, so that vexec's
	 * condition cache sees no granule in part.  vexec gives them
	 * only where passing a row over unread raises no error PostgreSQL would
	 * raise, and only under a NULLS order a key's NULLs never satisfy.
	 * Called between batches; the source copies what it keeps of them.
	 */
	void		(*set_keys) (void *state, int nkeys, const ScanKeyData *keys);

	/*
	 * Optional, from minor version 3: the identity of the dictionary of
	 * column `column` (0..ncolumns-1 of the spec) of the batch next() last
	 * handed, a VEXEC_DICT column.  Two batches of the scan whose
	 * dictionaries have one nonzero identity have the same values in them,
	 * at the same codes -- a dictionary the storage shares between its
	 * batches, as ClickHouse's LowCardinality shares one between a part's
	 * granules -- so that what vexec computed from a dictionary's values
	 * holds for every batch that comes with it.  An identity is never given
	 * to another dictionary while the process runs; 0 is none, and the
	 * dictionary is taken as new.  Called after next(), before the next
	 * call.
	 */
	uint64		(*dictionary_id) (void *state, int column);

	/*
	 * Optional, from minor version 3: estimate()'s rows and bytes, and the
	 * units and VEXEC_EST_* flags beside them, asked in place of estimate()
	 * where a source has it.  est->size is the caller's; the source fills
	 * the members that size holds.
	 */
	void		(*estimate_ex) (Relation rel, Snapshot snapshot,
								const VexecSourceSpec *spec, VexecSourceEstimate *est);

	/*
	 * Optional, from minor version 4: the columns a fetch of a row by its
	 * TID into `slot` reads -- table_tuple_fetch_row_version() or an index
	 * fetch, attnums[0 .. n), ascending -- the slot's other attributes
	 * left NULL, which no one reads: a bounded VecSort's late columns (H6)
	 * name theirs, where a column store would decode every column of the
	 * row.  Given before the first fetch into the slot, which then lives
	 * until its memory context is reset, as an executor's slots do; a fetch
	 * into any other slot reads every column.  VexecSourceEstimate.fetch
	 * prices such a fetch.
	 */
	void		(*fetch_columns) (Relation rel, TupleTableSlot *slot, int n,
								  const AttrNumber *attnums);

	/*
	 * Optional, from minor version 4: an order a serial scan hands its rows
	 * in -- the attributes it is by, attnums[0 .. k), at most max of them,
	 * the count returned -- in which, for every j up to k, the rows whose
	 * first j of them are equal come one after another: equal as each
	 * type's default equality has them, a NULL equal to a NULL.  Nothing is
	 * said of which comes first.  With `state` NULL, the planner's
	 * question: the order a serial scan of rel begun with VEXEC_SRC_ORDERED
	 * now would hand its rows in.  With a scan's state -- begun with
	 * VEXEC_SRC_ORDERED, before its first batch -- the order this scan's
	 * rows come in, which may be shorter, or none (0): the storage's units
	 * may have changed since the plan was made.  A parallel scan's is none.
	 */
	int			(*ordering) (Relation rel, void *state, int max, AttrNumber *attnums);

	/*
	 * Optional, from minor version 5: the columns of step `step` -- 1 to
	 * nsteps - 1 of a scan begun with VEXEC_SRC_STEPS -- of the batch next()
	 * handed last, filled into batch->columns, the batch next() filled.  For
	 * at least the rows of `selected`, nrows bits, 1 = a row vexec still
	 * reads, of which there is one; at another row a column may hold any
	 * value its layout can, NULL among them.  So a source decodes a later
	 * step's columns only in the units of its own -- a granule -- where a
	 * row is selected, vexec having evaluated the quals over the columns
	 * before them: a step whose columns no row needs is never asked for, nor
	 * any after it.  Called in step order, each step at most once a batch,
	 * before the next call of next(); vexec reads none of a step's columns
	 * before.  A batch retained keeps the steps handed by then.  A source
	 * that cannot skip may decode every column in next() and hand the later
	 * steps from it.
	 */
	void		(*next_rest) (void *state, VexecSourceBatch *batch, int step,
							  const uint64 *selected);

	/*
	 * Optional, from minor version 6: a hash join's inner keys, lent to the
	 * scan on its outer side (ch_table_engines.md §3, VC3) -- keys ANDed,
	 * replacing the last given, none where nkeys is 0 -- each on an
	 * attribute of the spec, under its type's default btree operator
	 * family, in one of three forms:
	 *
	 *	attnum >= v		BTGreaterEqualStrategyNumber: the inner keys' least
	 *	attnum <= v		BTLessEqualStrategyNumber: their greatest
	 *	attnum = ANY (a)	BTEqualStrategyNumber with SK_SEARCHARRAY: an
	 *					array of the attribute's type, its elements the
	 *					inner keys' distinct values, ascending, none NULL
	 *
	 * A row whose attribute is NULL satisfies none.  Pruning only, as
	 * set_keys(): from the next unit the source begins it may pass over
	 * one -- or the smaller units of its own it would read of it, granules
	 * -- whose statistics or index show that none of its rows satisfies
	 * them, and never a row of what it reads: vexec checks every row
	 * itself.  vexec gives them only to the outer side of an inner, semi or
	 * right join, which drops an outer row no inner row meets, and only
	 * where passing a row over unread raises no error PostgreSQL would
	 * raise.  Called between batches -- before the first, where the join's
	 * table is built before the scan begins -- and again where the table
	 * is built anew; a rescan forgets them.  The source copies what it
	 * keeps.
	 */
	void		(*set_join_keys) (void *state, int nkeys, const ScanKeyData *keys);

	/*
	 * Optional, from minor version 7: the scan's next unit, claimed as next()
	 * claims its units -- a parallel scan's among its participants, the
	 * spec's quals pruning as they prune -- between batches, for the GPU's
	 * executor to read itself (pg_vector_executor.md §3.25, N2): true with
	 * *unit filled, false at the scan's end.  A unit of a device's format the
	 * scan passes over, its rows counted in the statistics as handed; the
	 * executor reads them from the file.  A unit the source cannot hand so --
	 * a file of another kind, a column the executor's reader may not decode
	 * as the source's does -- comes as VEXEC_UNIT_CPU: next() then hands its
	 * rows, and returns false at its end, and the scan goes on at the next
	 * call of next_unit().  Called only for a scan begun with no keys, its
	 * columns in one step, without TIDs -- or, from minor version 9, with
	 * them (VEXEC_SRC_TIDS) where the source has unit_tids(), which then
	 * gives a unit's rows' TIDs -- or row_tids(), from the ids of a column
	 * of the unit's file it names (rowid_name).  The unit's pointers are the
	 * source's until
	 * the next call of next_unit() or read_unit(), or the scan's end; its
	 * token, until the scan's end.
	 */
	bool		(*next_unit) (void *state, VexecSourceUnit *unit);

	/*
	 * Optional, from minor version 7: a unit next_unit() handed for the
	 * device, read again by next() -- its rows, then false -- for an executor
	 * that lost them, or a unit whose values failed a check there, so that
	 * the source's reader raises its error.  Its rows are counted again.
	 */
	void		(*read_unit) (void *state, const VexecSourceUnit *unit);

	/*
	 * Optional, from minor version 8: bytes [offset, offset + length) of
	 * stream `stream` of a VEXEC_UNIT_MERGETREE unit next_unit() handed, into
	 * dest -- the device's memory a task shares with it -- before the unit's
	 * request goes; called between batches, as often as the caller asks.
	 */
	void		(*read_unit_bytes) (void *state, const VexecSourceUnit *unit, int stream,
									uint64 offset, uint64 length, void *dest);

	/*
	 * Optional, from minor version 9 (pg_vector_executor.md §3.27, W13): the
	 * TIDs of n rows of a unit next_unit() handed a scan begun with TIDs,
	 * each by its place among the unit's rows, deleted ones counted, in any
	 * order -- into tids, as next() would have handed them: a fetch of the
	 * row by its TID under the scan's snapshot finds it.  Called between
	 * batches, for any of the scan's units, until its end.  A unit whose
	 * rows' TIDs the source cannot give so comes as VEXEC_UNIT_CPU.
	 */
	void		(*unit_tids) (void *state, const VexecSourceUnit *unit, int n, const uint64 *rows,
							  ItemPointerData *tids);

	/*
	 * Optional, from minor version 9: the TIDs of n rows of the relation by
	 * their ids, as a unit's rowid_name column holds them (above).
	 */
	void		(*row_tids) (Relation rel, int n, const uint64 *ids, ItemPointerData *tids);

	/*
	 * Optional, from minor version 12 (W32, pg_vector_executor.md §3.29): the
	 * participant of a parallel scan whose state this is alone claims the
	 * scan's units from now on -- the device owner of W13's parallel twin,
	 * which reads every unit -- so that the source may hand it a part's
	 * adjacent units at once, as a serial scan's: a parallel scan's unit is a
	 * sixty-fourth of the table, ClickBench's twenty granules, each else one
	 * device unit.  Called once, before the participant asks for its first.
	 * A unit another participant claimed meanwhile stays its own.
	 */
	void		(*claim_alone) (void *state);

	/*
	 * Optional, from minor version 16 (pg_vector_executor.md §3.41, OT5): the
	 * scan's own figures so far -- at most VEXEC_SOURCE_FIGURES_MAX, into
	 * figures -- and how many it filled, which pg_otel_probe puts on the span
	 * of the node that reads it.  Called as the statement's executor ends,
	 * and on a failed statement's error path, with the state as the error left
	 * it: it reads the state alone, and neither allocates nor raises.
	 */
	int			(*scan_figures) (void *state, VexecSourceFigure *figures);

	/*
	 * Optional, from minor version 16: the process's reads to plan the
	 * source's scans since it started -- footers, listings, statistics --
	 * at most VEXEC_SOURCE_FIGURES_MAX of them, the same keys in the same
	 * order at every call.  vexec takes them as a statement's planning
	 * begins and ends, and keeps what grew for the statement's span.
	 */
	int			(*planning_figures) (VexecSourceFigure *figures);

	/*
	 * Optional, from minor version 19 (pg_vector_executor.md §3.38, F36):
	 * whether aggregate() counts this scan's units under the qual it was begun
	 * with, a scan begun with VEXEC_SRC_COUNT_QUALS.  Its *nrows then each
	 * unit's rows that satisfy the qual; a unit it cannot so answer it leaves
	 * to next(), whose rows the scan judges by the qual as ever.
	 */
	bool		(*count_under_quals) (void *state);
} VexecSourceRoutine;

/*
 * The registry the rendezvous variable points at.  Whichever module comes
 * first while the postmaster loads libraries makes it; it is never freed.
 * Its layout is fixed for the major version.
 */
#define VEXEC_SOURCE_MAGIC		0x56584331	/* "VXC1" */
#define VEXEC_SOURCE_MAX		64

typedef struct VexecSourceRegistry
{
	uint32		magic;
	int			nsources;
	const VexecSourceRoutine *sources[VEXEC_SOURCE_MAX];
} VexecSourceRegistry;

/* The registry, made if no module has made it yet. */
static inline VexecSourceRegistry *
vexec_source_registry(void)
{
	VexecSourceRegistry **rv;

	rv = (VexecSourceRegistry **) find_rendezvous_variable(VEXEC_SOURCE_RENDEZVOUS);
	if (*rv == NULL)
	{
		VexecSourceRegistry *reg;

		reg = (VexecSourceRegistry *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(VexecSourceRegistry));
		reg->magic = VEXEC_SOURCE_MAGIC;
		*rv = reg;
	}
	else if ((*rv)->magic != VEXEC_SOURCE_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("the rendezvous variable \"%s\" does not hold vexec's source registry",
						VEXEC_SOURCE_RENDEZVOUS)));
	return *rv;
}

/*
 * Register a source, from a storage module's _PG_init.  Only while the
 * postmaster preloads libraries, as the port's own registries refuse
 * otherwise: a backend that loaded the module later would have a source its
 * siblings lack.  The routine must stay valid for the life of the process.
 */
static inline void
vexec_register_source(const VexecSourceRoutine *routine)
{
	VexecSourceRegistry *reg;
	int			i;

	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a vexec batch source can only be registered while \"shared_preload_libraries\" are loaded")));
	if (routine == NULL || routine->am == NULL ||
		routine->size < offsetof(VexecSourceRoutine, estimate))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a vexec batch source must name its table access method")));

	reg = vexec_source_registry();
	for (i = 0; i < reg->nsources; i++)
		if (reg->sources[i]->am == routine->am)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("a vexec batch source is already registered for this table access method")));
	if (reg->nsources >= VEXEC_SOURCE_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many vexec batch sources"),
				 errdetail("At most %d can be registered.", VEXEC_SOURCE_MAX)));
	reg->sources[reg->nsources++] = routine;
}

/* The source registered for an access method, or NULL. */
static inline const VexecSourceRoutine *
vexec_find_source(const TableAmRoutine *am)
{
	VexecSourceRegistry **rv;
	VexecSourceRegistry *reg;
	int			i;

	rv = (VexecSourceRegistry **) find_rendezvous_variable(VEXEC_SOURCE_RENDEZVOUS);
	reg = *rv;
	if (reg == NULL || reg->magic != VEXEC_SOURCE_MAGIC)
		return NULL;
	for (i = 0; i < reg->nsources; i++)
		if (reg->sources[i]->am == am)
			return reg->sources[i];
	return NULL;
}

/* Whether a routine has a member: it was built with a struct that long. */
#define VEXEC_SOURCE_HAS(routine, member) \
	((routine)->size >= offsetof(VexecSourceRoutine, member) + sizeof((routine)->member) && \
	 (routine)->member != NULL)

#endif							/* VEXEC_SOURCE_H */
