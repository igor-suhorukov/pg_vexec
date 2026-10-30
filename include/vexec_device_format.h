/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_device_format.h
 *	  What crosses to a device and back: a task's program, its chunks of
 *	  columns, and its answers, as vexec's nodes write and read them
 *	  (pg_vector_executor.md §3.11, V8).
 *
 * The device contract (vexec_device.h) is two halves.  How a request gets to
 * a device and its answer back -- the memory, the process that owns the
 * device, the signals -- is the provider's, an extension of its own
 * (vexec_gpu).  What a request says is this file's, so that vexec's nodes and
 * a provider's executor, which links nothing of PostgreSQL's, read one
 * layout: plain C99, fixed widths, no pointers.
 *
 * Memory.  A provider hands a task blocks of a shared arena, each
 * block_size bytes, which the task's process and the executor both map, at
 * different addresses: so everything here names a block by its number and
 * a place in it by its offset.  A chunk's buffer -- a column's values, its
 * validity, a string column's offsets -- is a byte stream laid in pieces,
 * each within one block, in order; the executor joins a buffer's pieces in
 * the device's memory.  A fixed-width value never straddles two pieces,
 * the block size being a multiple of every width; a string's bytes may.
 *
 * A task.  One operator of one node of one process: a program, written
 * once with the request VEXEC_DEV_REQ_BEGIN; then its chunks, each a
 * request of its own; then its answer, asked for a page at a time into
 * blocks the task gives; then END.  One request at a time: the next is
 * written once the last is answered.  The executor holds the task's state
 * on the device between requests -- a grouping's table, a join's build
 * side -- and answers each request in the task's answer area with a status
 * and, for an answer page, its columns.
 *
 * Columns, as the device holds them (§3.11, "What crosses"):
 *
 *	BOOL8		a byte a value, 0 or 1: cuDF's BOOL8, and the PostgreSQL
 *				format's bool
 *	INT8..64	two's complement, little-endian: "char", int2, int4, int8,
 *				money, time, date's days and the timestamps' microseconds
 *				in the epoch the batch holds them in
 *	UINT32		oid
 *	FLOAT32/64	IEEE 754
 *	DECIMAL64/128	a scaled numeric: the value times 10^scale
 *	STRING		int32 offsets, nrows + 1 of them from 0, and the values'
 *				bytes: text, varchar and bytea, compared by their bytes
 *
 * A validity buffer is uint64 words, a bit a row from the least significant,
 * 1 where the row is not NULL; a column without one has no NULLs.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_DEVICE_FORMAT_H
#define VEXEC_DEVICE_FORMAT_H

#include <stdint.h>

#define VEXEC_DEV_MAGIC			0x56584452u	/* "VXDR" */
#define VEXEC_DEV_FORMAT		11	/* this file's layouts: 2, GROUPBY's rank; 3, the units' filter;
									 * 4, MergeTree's units; 5, GROUPBY's distinct count;
									 * 6, the units' relation and attributes; 7, the
									 * units' computed columns; 8, SCAN; 9, the filter's
									 * REGEXP, GROUPBY's HAVING (W17) and
									 * the units' granule rows (W21); 10, a
									 * MergeTree unit's ranges' streams' rows
									 * (W26); 11, the HAVING asked of an answer
									 * (W46) */

/* The parts that follow a header, at 8-aligned offsets. */
#define VEXEC_DEV_ALIGN(n)	(((n) + 7) & ~(uint64_t) 7)

/* The types a column crosses as. */
typedef enum VexecDevType
{
	VEXEC_DEV_NONE = 0,
	VEXEC_DEV_BOOL8,
	VEXEC_DEV_INT8,
	VEXEC_DEV_INT16,
	VEXEC_DEV_INT32,
	VEXEC_DEV_INT64,
	VEXEC_DEV_UINT32,
	VEXEC_DEV_FLOAT32,
	VEXEC_DEV_FLOAT64,
	VEXEC_DEV_DECIMAL64,
	VEXEC_DEV_DECIMAL128,
	VEXEC_DEV_STRING,
	VEXEC_DEV_NTYPES
} VexecDevType;

/* A fixed-width type's bytes a value; 0 for STRING and NONE. */
static inline int
vexec_dev_type_width(int type)
{
	switch (type)
	{
		case VEXEC_DEV_BOOL8:
		case VEXEC_DEV_INT8:
			return 1;
		case VEXEC_DEV_INT16:
			return 2;
		case VEXEC_DEV_INT32:
		case VEXEC_DEV_UINT32:
		case VEXEC_DEV_FLOAT32:
			return 4;
		case VEXEC_DEV_INT64:
		case VEXEC_DEV_FLOAT64:
		case VEXEC_DEV_DECIMAL64:
			return 8;
		case VEXEC_DEV_DECIMAL128:
			return 16;
		default:
			return 0;
	}
}

/* What a task runs. */
typedef enum VexecDevOp
{
	VEXEC_DEV_OP_NOOP = 1,		/* nothing: a request's round trip */
	VEXEC_DEV_OP_SUM,			/* the self-test's: the sum of a chunk's one
								 * INT64 column, computed on the device */
	VEXEC_DEV_OP_GROUPBY,		/* a VecAgg's first stage (V8b) */
	VEXEC_DEV_OP_JOIN,			/* a VecHashJoin's matches (V8c) */
	VEXEC_DEV_OP_SORT,			/* a VecSort's order (V8d) */
	VEXEC_DEV_OP_SCAN			/* a VecScan's rows from its units (W13) */
} VexecDevOp;

/* A request's kind. */
typedef enum VexecDevReqKind
{
	VEXEC_DEV_REQ_BEGIN = 1,	/* the program: VexecDevProgram follows */
	VEXEC_DEV_REQ_CHUNK,		/* a chunk: VexecDevChunk follows */
	VEXEC_DEV_REQ_ANSWER,		/* the next page of the answer: VexecDevPages
								 * follows */
	VEXEC_DEV_REQ_END,			/* the task is over: its device state freed */
	VEXEC_DEV_REQ_UNITS			/* rows the executor reads from files, as a
								 * chunk: VexecDevUnits follows (V8e) */
} VexecDevReqKind;

/* An answer's status. */
typedef enum VexecDevStatus
{
	VEXEC_DEV_OK = 0,
	VEXEC_DEV_MORE,				/* an answer page, and more pages to come */
	VEXEC_DEV_PENDING,			/* not answered yet (a provider's wait) */
	VEXEC_DEV_REFUSED,			/* admission: no room on the device in time */
	VEXEC_DEV_NOMEM,			/* the device's memory ran out: this request
								 * not taken, the task's state as before it */
	VEXEC_DEV_UNSUPPORTED,		/* the program asks for what the executor
								 * does not do */
	VEXEC_DEV_FAILED,			/* the executor's error: message says */
	VEXEC_DEV_LOST,				/* the executor that had the task is gone */
	VEXEC_DEV_CANCELLED			/* the task's process gave the request up */
} VexecDevStatus;

/* A piece of a buffer: bytes [offset, offset + length) of a block. */
typedef struct VexecDevPiece
{
	uint32_t	block;
	uint32_t	offset;
	uint32_t	length;
	uint32_t	pad;
} VexecDevPiece;

/* A buffer: pieces [first, first + npieces) of its request, in order. */
typedef struct VexecDevBuf
{
	uint32_t	first;
	uint32_t	npieces;
	uint64_t	length;			/* the pieces' bytes together */
} VexecDevBuf;

/* A chunk's column. */
typedef struct VexecDevCol
{
	uint8_t		type;			/* VexecDevType */
	uint8_t		flags;
	int16_t		scale;			/* DECIMAL64, DECIMAL128 */
	uint32_t	pad;
	VexecDevBuf values;			/* fixed width: nrows values; STRING: bytes */
	VexecDevBuf validity;		/* length 0: no NULLs */
	VexecDevBuf offsets;		/* STRING: nrows + 1 int32 */
} VexecDevCol;

/* Every request begins so; its kind's part follows. */
typedef struct VexecDevRequest
{
	uint32_t	magic;			/* VEXEC_DEV_MAGIC */
	uint16_t	kind;			/* VexecDevReqKind */
	uint16_t	op;				/* the task's VexecDevOp */
	uint32_t	length;			/* the request's bytes, this header included */
	uint32_t	flags;
	uint64_t	seq;			/* the task's request number, which the
								 * answer echoes */
} VexecDevRequest;

/*
 * A program (VEXEC_DEV_REQ_BEGIN): its chunks' columns, then its
 * operator's part -- VexecDevGroupBy for GROUPBY, nothing for NOOP and SUM.
 */
#define VEXEC_DEV_PROG_FAST		0x0001	/* vexec.precision = fast: the
										 * operator may take what only the
										 * fast mode allows */
#define VEXEC_DEV_PROG_SORT_RANK	0x0002	/* W37 (vexec.gpu_sort_rank): a GROUPBY
										 * ranked by its count(*) may group by
										 * sort, its tables kept to its answer */
/* W37, tests (vexec.debug_gpu_rank_hash_bits): the sort's keys' hashes cut to so many bits, 0 all 64 */
#define VEXEC_DEV_PROG_HASH_BITS_SHIFT	24
#define VEXEC_DEV_PROG_HASH_BITS(flags)	(((flags) >> VEXEC_DEV_PROG_HASH_BITS_SHIFT) & 0x7f)

typedef struct VexecDevColType
{
	uint8_t		type;			/* VexecDevType */
	uint8_t		pad;
	int16_t		scale;
} VexecDevColType;

/* admission_wait_ms: the provider's own setting's, filled in as it submits */
#define VEXEC_DEV_WAIT_DEFAULT	UINT32_MAX

typedef struct VexecDevProgram
{
	uint32_t	ncols;			/* every chunk's columns, these types */
	uint32_t	flags;			/* VEXEC_DEV_PROG_* */
	uint64_t	memory;			/* the bytes its state is expected to take
								 * on the device, for admission */
	uint32_t	admission_wait_ms;	/* how long admission may make it wait */
	uint32_t	pad;
	/* VexecDevColType cols[ncols]; then the operator's part, 8-aligned */
} VexecDevProgram;

/*
 * GROUPBY: the keys, chunk columns, NULLs one group, values grouped by
 * their bits -- a STRING's bytes -- and the aggregates.  Each aggregate takes
 * the rows its filter, a BOOL8 column, has 1 at, or every row; its
 * argument's NULLs are passed over.  Its result is one column of the
 * answer: NULL where it took no value, but COUNT_ALL's, COUNT's and
 * COUNT_DISTINCT's, which are never NULL.
 *
 * COUNT_DISTINCT (W1, pg_vector_executor.md §3.26) is a program's one
 * aggregate, of no filter: the distinct values of its argument a group's,
 * counted from every row of the task's, so its answer is asked once, at the
 * input's end, and is final.  Its keys may be none: one group, a plain
 * aggregation's.
 */
typedef enum VexecDevAggKind
{
	VEXEC_DEV_AGG_COUNT_ALL = 1,	/* the rows: INT64 */
	VEXEC_DEV_AGG_COUNT,		/* the rows whose argument is not NULL: INT64 */
	VEXEC_DEV_AGG_SUM,			/* into result's type: INT64 wraps as
								 * two's complement does; DECIMAL128 is exact;
								 * FLOAT32 and FLOAT64 add in the device's
								 * order (VEXEC_DEV_PROG_FAST alone) */
	VEXEC_DEV_AGG_MIN,			/* the argument's type; a float's NaN above
								 * every other value (VEXEC_DEV_PROG_FAST
								 * alone for floats) */
	VEXEC_DEV_AGG_MAX,
	VEXEC_DEV_AGG_ALL,			/* bool_and: BOOL8 */
	VEXEC_DEV_AGG_ANY,			/* bool_or: BOOL8 */
	VEXEC_DEV_AGG_M2,			/* the sum of the squares of a FLOAT64's
								 * deviations from its mean (FAST alone) */
	VEXEC_DEV_AGG_COUNT_DISTINCT	/* the distinct values not NULL: INT64 */
} VexecDevAggKind;

typedef struct VexecDevAgg
{
	uint8_t		kind;			/* VexecDevAggKind */
	uint8_t		result;			/* the result's VexecDevType */
	int16_t		arg;			/* a chunk column; -1 for COUNT_ALL */
	int16_t		filter;			/* a BOOL8 chunk column, or -1 */
	int16_t		scale;			/* result DECIMAL*: its scale */
} VexecDevAgg;

typedef struct VexecDevGroupBy
{
	uint16_t	nkeys;
	uint16_t	naggs;
	uint16_t	nrank;			/* the rank's keys; 0: none */
	uint16_t	rank_flags;		/* VEXEC_DEV_RANK_* */
	uint32_t	having_length;	/* W17: its HAVING filter's bytes after the rank, 8-aligned; 0: none */
	uint32_t	pad;
	/*
	 * int16_t keys[nkeys], padded to 8; VexecDevAgg aggs[naggs], padded to 8;
	 * VexecDevRank where nrank > 0; its VexecDevFilter, 8-aligned (W17)
	 */
} VexecDevGroupBy;

/*
 * A GROUPBY's rank (W2, pg_vector_executor.md §3.26): the order a bounded
 * sort above puts the answer's groups in, by a prefix of its keys -- each an
 * answer column, a key or an aggregate whose value is the sort's, ascending
 * or descending, its NULLs first or last as PostgreSQL's operators place
 * them -- and its bound.  Asked of the task's last answer alone
 * (VEXEC_DEV_PAGES_RANK), that answer is then the bound's first groups in
 * that order and, where the rank is a prefix of the sort's keys
 * (VEXEC_DEV_RANK_TIES), every group tying with the last of them on the
 * rank's keys: the sort's own answer is among them, whichever of the rows
 * tying at its bound it keeps.  Its value is the groups before the rank.
 *
 * A GROUPBY's HAVING (W17, pg_vector_executor.md §3.28): a filter of the
 * units' machine (VexecDevFilter, below) over the answer's columns -- the
 * keys, then the aggregates' results, numbered so -- applied before its rank
 * and its pages to an answer asked with VEXEC_DEV_PAGES_HAVING, the task's
 * only one, of every row of its input (W46, §3.35): the groups it does not
 * keep never cross, as the HAVING above drops them, which still runs over
 * the rest.  An answer before the input's end holds a part of each group's
 * rows, and so does the last after one: every group of theirs crosses.
 * Only where every aggregate it reads has its result in one device
 * aggregate's value -- a count, an integer's sum, a minimum, a maximum.
 */
#define VEXEC_DEV_RANK_TIES			0x0001

typedef struct VexecDevRankKey
{
	int16_t		col;			/* an answer column: a key's, or nkeys + an
								 * aggregate's */
	uint8_t		descending;
	uint8_t		nulls_first;
} VexecDevRankKey;

typedef struct VexecDevRank
{
	uint64_t	k;				/* the bound */
	/* VexecDevRankKey keys[nrank] */
} VexecDevRank;

/*
 * SORT (VecSort's, V8d): its keys, the program's first nkeys columns, each
 * ascending or descending, its NULLs first or last; and the column after
 * them, where the program has one, INT32 and never NULL, each row's id.
 * The chunks are the rows in the order the node read them; the answer is
 * every row's id -- its number among the task's rows where there is no id
 * column -- in the keys' order, a page at a time, INT32, unless two rows
 * tie on every key (two NULLs, two NaNs, a -0 and a 0, the rest equal):
 * tuplesort gives tied rows in an order of its own, so the answer then says
 * so (VEXEC_DEV_ANSF_TIES) and holds no row, and the node sorts on the CPU.
 */
typedef struct VexecDevSortKey
{
	uint8_t		descending;
	uint8_t		nulls_first;
	uint16_t	pad;
} VexecDevSortKey;

typedef struct VexecDevSort
{
	uint32_t	nkeys;			/* the program's first columns */
	uint32_t	pad;
	/* VexecDevSortKey keys[nkeys] */
} VexecDevSort;

static inline VexecDevSortKey *
vexec_dev_sort_keys(VexecDevSort *s)
{
	return (VexecDevSortKey *) (s + 1);
}

static inline uint64_t
vexec_dev_sort_size(int nkeys)
{
	return sizeof(VexecDevSort) + sizeof(VexecDevSortKey) * nkeys;
}

/*
 * SCAN (W13, pg_vector_executor.md §3.27): a VecScan's rows, which the
 * executor reads from the scan's units itself (VEXEC_DEV_REQ_UNITS alone,
 * no chunk), its filter (W4) applied: the program's columns -- the scan's,
 * and, where the scan's rows' TIDs are asked, their ordinals among the
 * task's units' rows (VEXEC_DEV_UCOL_ORDINAL), by which the node has the
 * source give their TIDs (vexec_source.h's minor 9) -- of the rows the
 * filter keeps.
 * With a rank (nrank > 0) -- the order a bounded VecSort above puts them
 * in, by program columns, each ascending or descending, its NULLs first or
 * last as PostgreSQL's operators place them, and its bound -- the answer is
 * the bound's first rows in that order and, with VEXEC_DEV_RANK_TIES,
 * every row tying with the last of them on the rank's keys, up to
 * VEXEC_DEV_SCAN_TIES_MAX past the bound: the sort's own rows are among
 * them, whichever of the rows tying at its bound it keeps.  Without a rank
 * it is every row kept, in the units' order.  A unit's answer's value is
 * the rows the filter kept so far; the answer's, the rows before the rank.
 */
#define VEXEC_DEV_SCAN_TIES_MAX		65536

typedef struct VexecDevScan
{
	uint64_t	k;				/* the rank's bound; 0: no rank */
	uint16_t	nrank;
	uint16_t	rank_flags;		/* VEXEC_DEV_RANK_TIES */
	uint32_t	pad;
	/* VexecDevRankKey keys[nrank], col a program column */
} VexecDevScan;

static inline VexecDevRankKey *
vexec_dev_scan_keys(VexecDevScan *s)
{
	return (VexecDevRankKey *) (s + 1);
}

static inline uint64_t
vexec_dev_scan_size(int nrank)
{
	return sizeof(VexecDevScan) + sizeof(VexecDevRankKey) * nrank;
}

/*
 * A chunk (VEXEC_DEV_REQ_CHUNK): nrows rows of the program's columns, in
 * the order the task read them; its columns' descriptions, then the
 * pieces their buffers name.
 */
typedef struct VexecDevChunk
{
	uint32_t	nrows;
	uint16_t	ncols;
	uint16_t	side;			/* JOIN: 0 the build side, 1 the probe side */
	uint32_t	npieces;
	uint32_t	pad;
	uint64_t	first_row;		/* the ordinal of its first row among the
								 * task's rows of its side */
	/* VexecDevCol cols[ncols]; VexecDevPiece pieces[npieces] */
} VexecDevChunk;

/*
 * Units (VEXEC_DEV_REQ_UNITS, V8e): a chunk of the program's columns the
 * executor reads from files itself, in place of one the node wrote
 * (pg_vector_executor.md §3.25, N1 to N4).  A unit is some row groups of a
 * Parquet file, or some record batches of an Arrow IPC file, as a source
 * handed them (vexec_source.h, next_unit()); each program column is a
 * column of the unit's file, by its name there.  The file is named by its
 * path and pinned by its size and modification time, which the executor
 * checks before it reads.  A unit's rows the snapshot does not see are a
 * bitmap in the request's blocks, a bit a row of its parts' rows in order.
 *
 * The executor reads each column in its file's type and makes it the
 * program's: an integer widened, a decimal moved to the program's scale, a
 * date's days and a timestamp's microseconds moved to PostgreSQL's epoch.
 * The answer is a chunk's, its value the rows read.  A unit the executor
 * cannot read so -- a file changed since it was listed, a type it does not
 * make the program's, a NULL in a column of VEXEC_DEV_UCOL_NOT_NULL, a
 * string that is not UTF-8 in one of VEXEC_DEV_UCOL_CHECK_UTF8 -- refuses
 * the request (VEXEC_DEV_REFUSED, its message saying which): the node then
 * has the units read on the CPU, whose reader raises an error that is one.
 */
#define VEXEC_DEV_UNIT_PARQUET		1
#define VEXEC_DEV_UNIT_ARROW		2
#define VEXEC_DEV_UNIT_MERGETREE	3	/* W6: granules of a MergeTree part */
#define VEXEC_DEV_UNIT_NO_GRANULES	UINT32_MAX	/* W21: a unit's granule_rows: none given */

#define VEXEC_DEV_UCOL_NOT_NULL		0x01
#define VEXEC_DEV_UCOL_CHECK_UTF8	0x02
#define VEXEC_DEV_UCOL_EXPR			0x04	/* W10: computed, not read (below) */
#define VEXEC_DEV_UCOL_ROWID		0x10	/* W13: the unit's rows' ids, an
											 * unsigned 64-bit integer of its
											 * file's each, read as INT64 */
#define VEXEC_DEV_UCOL_DAYS			0x20	/* PQ6: an integer of days since
											 * 1970, made a date: INT32 */
#define VEXEC_DEV_UCOL_SECONDS		0x40	/* PQ6: an integer of seconds since
											 * 1970, made a timestamp: INT64 */
#define VEXEC_DEV_UCOL_ORDINAL		0x08	/* W13: made, not read: each row's
											 * ordinal among the task's units'
											 * rows -- the request's first_row
											 * and its place among the
											 * request's units' rows, deleted
											 * ones counted -- INT64, its name
											 * empty */

typedef struct VexecDevUnit
{
	uint8_t		format;			/* VEXEC_DEV_UNIT_* */
	uint8_t		pad;
	uint16_t	pad2;
	uint32_t	nparts;			/* its row groups, its record batches: */
	uint32_t	parts;			/* the first of them in the parts area */
	uint32_t	path;			/* its file's path: an offset in the strings */
	uint32_t	names;			/* the program's columns' names in the file,
								 * one after another, each ending in a zero
								 * byte: an offset in the strings */
	uint64_t	file_size;
	int64_t		file_mtime;		/* nanoseconds since 1970 */
	uint64_t	rows;			/* its parts' rows, deleted ones among them */
	VexecDevBuf deleted;		/* rows bits, 1 = not seen; length 0: none */
	uint32_t	first_stream;	/* MERGETREE's: its streams in the request's */
	uint32_t	nstreams;
	/*
	 * W21 (pg_vector_executor.md §3.28): a MERGETREE unit's granules' rows --
	 * nparts int32 entries in the parts area, the first of them here, each the
	 * rows of the granule at the same place among the unit's parts -- so that
	 * the executor says of each granule whether a row of it passed the filter
	 * (VEXEC_DEV_ANSF_GRANULES); VEXEC_DEV_UNIT_NO_GRANULES where none is given
	 */
	uint32_t	granule_rows;
	uint32_t	pad3;
} VexecDevUnit;

/*
 * W6 (pg_vector_executor.md §3.26): a MERGETREE unit's columns as its part's
 * files hold them, each stream compressed blocks of ClickHouse's -- a 16-byte
 * checksum, a method's byte (0x02 none, 0x82 LZ4, 0x90 zstd), the block's
 * size with its 9 header bytes and its data's -- the bytes of its data before
 * the unit's first row skipped: a column's values, fixed-width little-endian
 * or a String's bytes; a String's sizes, a uint64 each; a Nullable's null
 * map, a byte each, 1 a NULL; a sparse serialization's offsets, VarUInts --
 * each the rows of the default before the next value, a granule's last its
 * rows of the default after them, bit 62 set -- the values' streams then
 * those of the other rows alone, the default the type's 0 or empty string,
 * or NULL (SPARSE_NULLS).  Each column read -- the program's, then the
 * filter's -- has its values, and its sizes, null map or offsets where it
 * has them;
 * its path is the unit's name for the device's cache (W5), file_size and
 * file_mtime 0, nothing checked against a file.
 */
#define VEXEC_DEV_STREAM_VALUES		1
#define VEXEC_DEV_STREAM_SIZES		2
#define VEXEC_DEV_STREAM_NULLS		3
#define VEXEC_DEV_STREAM_SPARSE		4
#define VEXEC_DEV_STREAM_SPARSE_NULLS	5

typedef struct VexecDevPartStream
{
	uint16_t	column;			/* the column read it is of */
	uint8_t		kind;			/* VEXEC_DEV_STREAM_* */
	uint8_t		width;			/* VALUES: a value's bytes, 0 a String's */
	uint32_t	rows;			/* W26: its range's rows, a unit of several
								 * ranges' streams each range's in turn, a
								 * column's again for each; 0: the unit's */
	uint64_t	skip;
	VexecDevBuf bytes;			/* its compressed blocks, pieces of the request's */
} VexecDevPartStream;

/*
 * A filter of the units' rows (W4, pg_vector_executor.md §3.26): the scan's
 * quals, which the executor applies to the rows it read, past the rows the
 * snapshot does not see, before the operator takes them.  It reads the
 * program's columns and nqcols more of its own, read from the units past
 * the program's -- their names after the program's in each unit's names,
 * their checks after the program's in the column flags, their types in
 * qtypes -- which the operator never sees.  It is a stack machine's steps:
 *
 *	COLUMN		push column arg: the program's from 0, then the filter's own
 *	CONST		push the constant of type `type` (`scale`) at offset arg of
 *				the filter's data: a fixed width's value, or a STRING's
 *				int32 length and bytes
 *	CMP			pop the constant, then the column; push their comparison, arg
 *				a VEXEC_DEV_CMP_*: integers as integers of the wider width,
 *				strings by their bytes; NULL where the column's value is
 *	AND, OR		pop arg values, push their AND or OR, NULL as SQL has it
 *	NOT			pop one, push its NOT; NULL stays NULL
 *	ISNULL, ISNOTNULL	pop a column, push whether its value is NULL, or not
 *	LIKE		pop the pattern, a STRING constant, then the column; push its
 *				LIKE -- '%' any characters, '_' one, '\' before either of
 *				them that one itself -- arg 1 for NOT LIKE
 *
 * The last value left is the filter: a row is kept where it is true, not
 * where it is false or NULL, as WHERE keeps it.
 *
 * W10 (pg_vector_executor.md §3.27): a program column the executor computes
 * from the columns read, in place of reading it -- VEXEC_DEV_UCOL_EXPR in
 * its flags, its name in each unit's names empty.  Each such column, in the
 * program's order, has an expression after the filter: a VexecDevFilter of
 * the same machine over the columns read, the program's that are read and
 * the filter's own, numbered as the filter's are, whose last value is the
 * column, of the program's type -- computed over the rows the filter keeps.
 * Beside the filter's, its steps make values:
 *
 *	ARITH		pop b, then a; push a op b, arg a VEXEC_DEV_ARITH_*, integers
 *				of the result's type `type`: one out of its range refuses the
 *				unit, which the CPU then reads, its operator raising
 *	WIDEN		pop an integer; push it as the wider integer `type`
 *	EXTRACT		pop a timestamp, INT64 microseconds from PostgreSQL's epoch;
 *				push its field arg (VEXEC_DEV_FIELD_*), INT64 -- a timestamp
 *				not finite refuses the unit
 *	TRUNC		pop a timestamp; push it truncated to the unit arg
 *				(VEXEC_DEV_FIELD_SECOND to _DAY), its infinities as they are
 *	LENGTH		pop a STRING; push its characters' count, INT32: arg 1 its
 *				bytes' (bytea's)
 *	CASE		pop the ELSE value, then arg pairs of a condition and its
 *				value pushed in order; push the value of the first condition
 *				true, else the ELSE value
 *
 *	REGEXP		pop a STRING; push regexp_replace() of it by the constant
 *				pattern and replacement at offset arg of the filter's data,
 *				a VexecDevRegex (W17, below): STRING, NULL where the value is
 *	SECONDS		pop an integer of seconds since 1970; push to_timestamp() of
 *				it, INT64 microseconds from PostgreSQL's epoch, as its float8
 *				makes it -- exactly, the seconds within 2^53 microseconds'
 *				(W43): one past them refuses the unit, which the CPU then
 *				reads
 *
 * A comparison's first operand may be a value made so.
 */
typedef enum VexecDevFilterOp
{
	VEXEC_DEV_FILTER_COLUMN = 1,
	VEXEC_DEV_FILTER_CONST,
	VEXEC_DEV_FILTER_CMP,
	VEXEC_DEV_FILTER_AND,
	VEXEC_DEV_FILTER_OR,
	VEXEC_DEV_FILTER_NOT,
	VEXEC_DEV_FILTER_ISNULL,
	VEXEC_DEV_FILTER_ISNOTNULL,
	VEXEC_DEV_FILTER_LIKE,
	VEXEC_DEV_FILTER_ARITH,		/* W10 */
	VEXEC_DEV_FILTER_WIDEN,
	VEXEC_DEV_FILTER_EXTRACT,
	VEXEC_DEV_FILTER_TRUNC,
	VEXEC_DEV_FILTER_LENGTH,
	VEXEC_DEV_FILTER_CASE,
	VEXEC_DEV_FILTER_REGEXP,	/* W17 (§3.28): regexp_replace(), below */
	VEXEC_DEV_FILTER_SECONDS	/* W43 (§3.35): to_timestamp() of an integer */
} VexecDevFilterOp;

typedef enum VexecDevArith
{
	VEXEC_DEV_ARITH_ADD = 1,
	VEXEC_DEV_ARITH_SUB,
	VEXEC_DEV_ARITH_MUL
} VexecDevArith;

/* EXTRACT's fields, as PostgreSQL's extract() names them; TRUNC's units the first four */
typedef enum VexecDevField
{
	VEXEC_DEV_FIELD_SECOND = 1,	/* TRUNC alone: extract()'s second has a fraction */
	VEXEC_DEV_FIELD_MINUTE,
	VEXEC_DEV_FIELD_HOUR,
	VEXEC_DEV_FIELD_DAY,
	VEXEC_DEV_FIELD_MONTH,
	VEXEC_DEV_FIELD_YEAR,
	VEXEC_DEV_FIELD_DOW,
	VEXEC_DEV_FIELD_ISODOW,
	VEXEC_DEV_FIELD_DOY,
	VEXEC_DEV_FIELD_QUARTER
} VexecDevField;

typedef enum VexecDevCmp
{
	VEXEC_DEV_CMP_EQ = 1,
	VEXEC_DEV_CMP_NE,
	VEXEC_DEV_CMP_LT,
	VEXEC_DEV_CMP_LE,
	VEXEC_DEV_CMP_GT,
	VEXEC_DEV_CMP_GE
} VexecDevCmp;

/*
 * W17 (pg_vector_executor.md §3.28): REGEXP, regexp_replace() of a constant
 * pattern and replacement over a STRING, for the patterns H10's kernel
 * (vexec's expr/k_regex.c) matches as PostgreSQL's engine does: anchored
 * at both ends, no alternation, every quantifier greedy.  The step pops a
 * STRING and pushes the STRING replaced -- NULL where the value is, the
 * value as it is where the pattern does not match -- and its arg is the
 * offset in the filter's data, 8-aligned, of a VexecDevRegex: the pattern's
 * items as the kernel compiles them, in order -- a literal's bytes; an
 * optional run of literals, present first; a class, once or under ?, * or
 * +, its members ASCII, a byte past ASCII in a negated class and in no
 * other; a group's start and end, nine groups at most, not nested -- the
 * replacement's parts, each a literal's bytes or a group's text (ref 0 the
 * match's, 1 to 9 a group's), then the items' literal bytes and the
 * replacement's.  The executor's matcher is the kernel's, greedy, its
 * backtracking bounded by max_steps: a row past it refuses the unit, which
 * the CPU then reads, and PostgreSQL's function answers that row.
 */
#define VEXEC_DEV_REGEX_MAX_ITEMS	64
#define VEXEC_DEV_REGEX_MAX_GROUPS	9

typedef enum VexecDevRegexKind
{
	VEXEC_DEV_REGEX_LIT = 0,	/* bytes, as they are */
	VEXEC_DEV_REGEX_OPT,		/* bytes, or none: greedy */
	VEXEC_DEV_REGEX_CLASS,		/* min to max characters of a class: greedy */
	VEXEC_DEV_REGEX_OPEN,		/* a group's start */
	VEXEC_DEV_REGEX_CLOSE		/* and its end */
} VexecDevRegexKind;

typedef struct VexecDevRegexItem
{
	uint8_t		kind;			/* VexecDevRegexKind */
	uint8_t		any;			/* CLASS: . */
	uint8_t		negate;			/* CLASS: [^...] */
	uint8_t		pad;
	int32_t		min;			/* CLASS: 0 or 1 */
	int32_t		max;			/* CLASS: 1, or -1 for no bound */
	int32_t		excl;			/* CLASS: a negated class's one member, or -1 */
	int32_t		group;			/* OPEN, CLOSE: from 1 */
	int32_t		off;			/* LIT, OPT: its bytes in the literals */
	int32_t		len;
	int32_t		pad2;
	uint64_t	member[2];		/* CLASS: its ASCII members, a bit each */
} VexecDevRegexItem;

typedef struct VexecDevRegexPart
{
	int32_t		ref;			/* -1: bytes of the replacement; 0: the match; 1 to 9: a group */
	int32_t		off;			/* the bytes', in the replacement's */
	int32_t		len;
	int32_t		pad;
} VexecDevRegexPart;

typedef struct VexecDevRegex
{
	uint32_t	nitems;
	uint32_t	ngroups;
	uint32_t	nparts;
	uint32_t	lits_len;
	uint32_t	repl_len;
	uint32_t	max_steps;		/* a row's matching gives up past this many steps */
	uint8_t		utf8;			/* the values UTF-8; else of a single-byte encoding */
	uint8_t		pad[3];
	uint32_t	pad2;
	/*
	 * VexecDevRegexItem items[nitems]; VexecDevRegexPart parts[nparts];
	 * char lits[lits_len], padded to 8; char repl[repl_len], padded to 8
	 */
} VexecDevRegex;

static inline VexecDevRegexItem *
vexec_dev_regex_items(VexecDevRegex *r)
{
	return (VexecDevRegexItem *) (r + 1);
}

static inline VexecDevRegexPart *
vexec_dev_regex_parts(VexecDevRegex *r)
{
	return (VexecDevRegexPart *) (vexec_dev_regex_items(r) + r->nitems);
}

static inline char *
vexec_dev_regex_lits(VexecDevRegex *r)
{
	return (char *) (vexec_dev_regex_parts(r) + r->nparts);
}

static inline char *
vexec_dev_regex_repl(VexecDevRegex *r)
{
	return vexec_dev_regex_lits(r) + VEXEC_DEV_ALIGN(r->lits_len);
}

static inline uint64_t
vexec_dev_regex_size(uint32_t nitems, uint32_t nparts, uint32_t lits_len, uint32_t repl_len)
{
	return sizeof(VexecDevRegex) + sizeof(VexecDevRegexItem) * nitems + sizeof(VexecDevRegexPart) * nparts +
		VEXEC_DEV_ALIGN(lits_len) + VEXEC_DEV_ALIGN(repl_len);
}

typedef struct VexecDevFilterStep
{
	uint8_t		op;				/* VexecDevFilterOp */
	uint8_t		type;			/* CONST: its VexecDevType */
	int16_t		scale;
	uint32_t	arg;
} VexecDevFilterStep;

typedef struct VexecDevFilter
{
	uint32_t	nsteps;
	uint32_t	data_length;
	/* VexecDevFilterStep steps[nsteps]; char data[data_length] */
} VexecDevFilter;

static inline VexecDevFilterStep *
vexec_dev_filter_steps(VexecDevFilter *f)
{
	return (VexecDevFilterStep *) (f + 1);
}

static inline char *
vexec_dev_filter_data(VexecDevFilter *f)
{
	return (char *) (vexec_dev_filter_steps(f) + f->nsteps);
}

static inline uint64_t
vexec_dev_filter_size(int nsteps, uint64_t data_length)
{
	return sizeof(VexecDevFilter) + sizeof(VexecDevFilterStep) * nsteps + data_length;
}

typedef struct VexecDevUnits
{
	uint32_t	nunits;
	uint16_t	ncols;			/* the program's */
	uint16_t	nqcols;			/* W4: its filter's own, past the program's */
	uint32_t	nparts;			/* the parts area's int32 entries */
	uint32_t	npieces;
	uint32_t	strings_length;
	uint32_t	filter_length;	/* W4: its filter's bytes; 0: none */
	uint32_t	nstreams;		/* W6: MERGETREE units' */
	uint32_t	flags;			/* VEXEC_DEV_UNITS_* */
	uint64_t	first_row;		/* its first row's ordinal among the task's */
	uint32_t	database;		/* W7: the units' relation, its database's */
	uint32_t	relation;		/* and its own oid, 0 for none */
	uint32_t	expr_length;	/* W10: its computed columns' expressions' bytes */
	uint32_t	pad;

	/*
	 * uint8_t column_flags[ncols + nqcols], padded to 8; VexecDevColType
	 * qtypes[nqcols], padded to 8; int16_t attnums[ncols + nqcols], padded
	 * to 8 (W7: each column read's attribute of the relation, 0 for none);
	 * VexecDevUnit units[nunits]; int32_t parts[nparts], padded to 8;
	 * VexecDevPiece pieces[npieces]; char strings[strings_length], padded to
	 * 8; its VexecDevFilter, padded to 8; its computed columns' expressions,
	 * a VexecDevFilter each, padded to 8 (W10); VexecDevPartStream
	 * streams[nstreams]
	 */
} VexecDevUnits;

/*
 * A probe (W5, W6): the units' columns the executor keeps from an earlier
 * read, a bit a column read of each unit in turn -- bit unit * (ncols +
 * nqcols) + column -- past the answer's header; nothing read, nothing taken
 * by the operator, the streams' bytes none.  A request after it then holds
 * the streams of the columns not kept alone, which the executor takes from
 * what it keeps -- refusing the unit where it has since given one back.
 */
#define VEXEC_DEV_UNITS_PROBE		0x0001

static inline uint8_t *
vexec_dev_units_flags(VexecDevUnits *u)
{
	return (uint8_t *) (u + 1);
}

static inline VexecDevColType *
vexec_dev_units_qtypes(VexecDevUnits *u)
{
	return (VexecDevColType *) ((char *) (u + 1) + VEXEC_DEV_ALIGN((uint64_t) u->ncols + u->nqcols));
}

static inline int16_t *
vexec_dev_units_attnums(VexecDevUnits *u)
{
	return (int16_t *) ((char *) vexec_dev_units_qtypes(u) +
						VEXEC_DEV_ALIGN(sizeof(VexecDevColType) * u->nqcols));
}

static inline VexecDevUnit *
vexec_dev_units_list(VexecDevUnits *u)
{
	return (VexecDevUnit *) ((char *) vexec_dev_units_attnums(u) +
							 VEXEC_DEV_ALIGN(sizeof(int16_t) * ((uint64_t) u->ncols + u->nqcols)));
}

static inline int32_t *
vexec_dev_units_parts(VexecDevUnits *u)
{
	return (int32_t *) (vexec_dev_units_list(u) + u->nunits);
}

static inline VexecDevPiece *
vexec_dev_units_pieces(VexecDevUnits *u)
{
	return (VexecDevPiece *) ((char *) vexec_dev_units_parts(u) +
							  VEXEC_DEV_ALIGN(sizeof(int32_t) * u->nparts));
}

static inline char *
vexec_dev_units_strings(VexecDevUnits *u)
{
	return (char *) (vexec_dev_units_pieces(u) + u->npieces);
}

static inline VexecDevFilter *
vexec_dev_units_filter(VexecDevUnits *u)
{
	return u->filter_length == 0 ? NULL :
		(VexecDevFilter *) (vexec_dev_units_strings(u) + VEXEC_DEV_ALIGN(u->strings_length));
}

/* W10: the first computed column's expression, the next one 8-aligned past it; NULL: none */
static inline VexecDevFilter *
vexec_dev_units_exprs(VexecDevUnits *u)
{
	return u->expr_length == 0 ? NULL :
		(VexecDevFilter *) (vexec_dev_units_strings(u) + VEXEC_DEV_ALIGN(u->strings_length) +
							VEXEC_DEV_ALIGN(u->filter_length));
}

static inline VexecDevPartStream *
vexec_dev_units_streams(VexecDevUnits *u)
{
	return (VexecDevPartStream *) (vexec_dev_units_strings(u) + VEXEC_DEV_ALIGN(u->strings_length) +
							   VEXEC_DEV_ALIGN(u->filter_length) + VEXEC_DEV_ALIGN(u->expr_length));
}

static inline uint64_t
vexec_dev_units_size(int ncols, int nqcols, int nunits, int nparts, int npieces,
					 uint64_t strings_length, uint64_t filter_length, uint64_t expr_length, int nstreams)
{
	return sizeof(VexecDevUnits) + VEXEC_DEV_ALIGN((uint64_t) ncols + nqcols) +
		VEXEC_DEV_ALIGN(sizeof(VexecDevColType) * nqcols) +
		VEXEC_DEV_ALIGN(sizeof(int16_t) * ((uint64_t) ncols + nqcols)) + sizeof(VexecDevUnit) * nunits +
		VEXEC_DEV_ALIGN(sizeof(int32_t) * nparts) + sizeof(VexecDevPiece) * npieces +
		VEXEC_DEV_ALIGN(strings_length) + VEXEC_DEV_ALIGN(filter_length) + VEXEC_DEV_ALIGN(expr_length) +
		sizeof(VexecDevPartStream) * nstreams;
}

/*
 * The next answer page (VEXEC_DEV_REQ_ANSWER): the blocks the executor may
 * write it into.  A GROUPBY's answer is its groups so far, after which its
 * table starts again empty; it may take several pages.  Its first page's
 * flags may ask for the answer ranked (VEXEC_DEV_PAGES_RANK): the task's
 * last answer, which the program's rank cuts; and for its HAVING
 * (VEXEC_DEV_PAGES_HAVING, W46): the task's only answer, which the
 * program's HAVING cuts before its rank.
 */
#define VEXEC_DEV_PAGES_RANK		0x0001
#define VEXEC_DEV_PAGES_HAVING		0x0002

typedef struct VexecDevPages
{
	uint32_t	npages;
	uint32_t	flags;			/* VEXEC_DEV_PAGES_* */
	/* uint32_t blocks[npages] */
} VexecDevPages;

/* An answer column's buffer: bytes of one page block, contiguous. */
typedef struct VexecDevSpan
{
	uint32_t	block;
	uint32_t	offset;
	uint64_t	length;			/* 0: none */
} VexecDevSpan;

typedef struct VexecDevAnsCol
{
	uint8_t		type;			/* VexecDevType */
	uint8_t		flags;
	int16_t		scale;
	uint32_t	pad;
	VexecDevSpan values;
	VexecDevSpan validity;		/* length 0: no NULLs */
	VexecDevSpan offsets;		/* STRING */
} VexecDevAnsCol;

/*
 * JOIN (VecHashJoin's, V8c): the build side's chunks (side 0) come first,
 * every row of the node's table in the order it numbered them, then probe
 * chunks (side 1), each of which the executor answers with its matching
 * pairs: an answer of two INT32 columns, the probe chunk's row and the
 * build side's, sorted by the probe row and then by the build row
 * descending -- an outer row's matches in the order the node's buckets
 * would give them.  A page ends where a probe row's pairs end, unless one
 * row's alone are more than a page holds: then its flags say so.
 */
#define VEXEC_DEV_ANSF_ROW_CONTINUES	0x0001
#define VEXEC_DEV_ANSF_TIES				0x0002	/* SORT: two rows tie */
/*
 * W21: a UNITS request's answer carries, after its header and its columns
 * (none), for each of its units that gave its granules' rows, in order,
 * (nparts + 63) / 64 words: a bit a granule of the unit's parts, set where no
 * row of it the filter kept -- the rows the snapshot does not see dropped
 * before, so a unit of deletions says nothing -- which the node keeps in its
 * condition cache.  Absent where the request has no filter, or the words
 * would not fit.
 */
#define VEXEC_DEV_ANSF_GRANULES			0x0004
/* W37: a GROUPBY's ranked answer's first page, its groups grouped by sort */
#define VEXEC_DEV_ANSF_SORTED			0x0008

/* The answer to a request, in the task's answer area. */
typedef struct VexecDevAnswer
{
	uint32_t	magic;			/* VEXEC_DEV_MAGIC */
	int32_t		status;			/* VexecDevStatus */
	uint64_t	seq;			/* the request's */
	uint32_t	nrows;			/* an answer page's rows */
	uint16_t	ncols;
	uint16_t	flags;
	uint64_t	value;			/* SUM's sum; GROUPBY's chunk: its rows' groups
								 * on the device after it */
	uint64_t	device_ns;		/* the request's time on the device */
	uint64_t	gpu_ns;			/* pg_vector_executor.md §3.41, OT10: of it,
								 * the GPU's time of the request's work on its
								 * task's stream, by CUDA events around it; 0
								 * where none was measured */
	uint32_t	cols_cached;	/* UNITS: of the columns its units asked for, */
	uint32_t	cols_asked;		/* those the executor kept from before (W5) */
	char		message[256];	/* FAILED, UNSUPPORTED: why */
	/* VexecDevAnsCol cols[ncols] */
} VexecDevAnswer;


static inline VexecDevColType *
vexec_dev_program_cols(VexecDevProgram *p)
{
	return (VexecDevColType *) (p + 1);
}

static inline void *
vexec_dev_program_op(VexecDevProgram *p)
{
	return (char *) (p + 1) + VEXEC_DEV_ALIGN(sizeof(VexecDevColType) * p->ncols);
}

static inline int16_t *
vexec_dev_groupby_keys(VexecDevGroupBy *g)
{
	return (int16_t *) (g + 1);
}

static inline VexecDevAgg *
vexec_dev_groupby_aggs(VexecDevGroupBy *g)
{
	return (VexecDevAgg *) ((char *) (g + 1) + VEXEC_DEV_ALIGN(sizeof(int16_t) * g->nkeys));
}

static inline VexecDevRank *
vexec_dev_groupby_rank(VexecDevGroupBy *g)
{
	return (VexecDevRank *) ((char *) vexec_dev_groupby_aggs(g) +
							 VEXEC_DEV_ALIGN(sizeof(VexecDevAgg) * g->naggs));
}

static inline VexecDevRankKey *
vexec_dev_rank_keys(VexecDevRank *r)
{
	return (VexecDevRankKey *) (r + 1);
}

/* W17: GROUPBY's HAVING filter, after its rank, 8-aligned; where having_length is 0, nothing */
static inline VexecDevFilter *
vexec_dev_groupby_having(VexecDevGroupBy *g)
{
	uint64_t	ranksize = g->nrank > 0 ? sizeof(VexecDevRank) + sizeof(VexecDevRankKey) * g->nrank : 0;

	return (VexecDevFilter *) ((char *) vexec_dev_groupby_rank(g) + VEXEC_DEV_ALIGN(ranksize));
}

/* GROUPBY's part: its rank's too where nrank > 0, and its HAVING's where having_length is not 0 */
static inline uint64_t
vexec_dev_groupby_size(int nkeys, int naggs, int nrank, uint64_t having_length)
{
	return sizeof(VexecDevGroupBy) + VEXEC_DEV_ALIGN(sizeof(int16_t) * nkeys) +
		VEXEC_DEV_ALIGN(sizeof(VexecDevAgg) * naggs) +
		VEXEC_DEV_ALIGN(nrank > 0 ? sizeof(VexecDevRank) + sizeof(VexecDevRankKey) * nrank : 0) +
		VEXEC_DEV_ALIGN(having_length);
}

static inline VexecDevCol *
vexec_dev_chunk_cols(VexecDevChunk *c)
{
	return (VexecDevCol *) (c + 1);
}

static inline VexecDevPiece *
vexec_dev_chunk_pieces(VexecDevChunk *c)
{
	return (VexecDevPiece *) (vexec_dev_chunk_cols(c) + c->ncols);
}

static inline uint32_t *
vexec_dev_pages_blocks(VexecDevPages *p)
{
	return (uint32_t *) (p + 1);
}

static inline VexecDevAnsCol *
vexec_dev_answer_cols(VexecDevAnswer *a)
{
	return (VexecDevAnsCol *) (a + 1);
}

/* W21: a UNITS answer's granule words, after its columns */
static inline uint64_t *
vexec_dev_answer_granules(VexecDevAnswer *a)
{
	return (uint64_t *) (vexec_dev_answer_cols(a) + a->ncols);
}

#endif							/* VEXEC_DEVICE_FORMAT_H */
