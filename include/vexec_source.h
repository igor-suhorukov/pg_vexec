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
#define VEXEC_SOURCE_MINOR		2	/* 1: aggregate; 2: set_keys */

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
	VEXEC_OFFSETS				/* Arrow's binary: int32 offsets, and the
								 * bytes */
} VexecLayout;

#define VEXEC_NUM_LAYOUTS	(VEXEC_OFFSETS + 1)

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
 *
 * vexec ignores the fields a layout does not read: a source may leave them
 * as it likes.
 */
typedef struct VexecColumn
{
	uint8		layout;			/* VexecLayout */
	uint8		encoding;		/* VexecEncoding */
	bool		arrow_values;	/* date, timestamp[tz], interval: Arrow's
								 * epoch and struct */
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

/* VexecSourceSpec.flags */
#define VEXEC_SRC_COMPACT	0x0001	/* hand only visible rows */
#define VEXEC_SRC_TIDS		0x0002	/* fill VexecSourceBatch.tids */
#define VEXEC_SRC_DICT		0x0004	/* dictionary columns are welcome */

/* What a scan asks of a source. */
typedef struct VexecSourceSpec
{
	Size		size;			/* sizeof as the caller was built */
	int			ncolumns;
	const AttrNumber *attnums;	/* 1..natts, ascending: a TID is asked for
								 * with VEXEC_SRC_TIDS, never as a column */
	const uint8 *layouts;		/* per column: the layout the batch format
								 * asks for */
	List	   *quals;			/* ANDed Exprs over the relation's own Vars:
								 * pruning only */
	int			nkeys;
	ScanKeyData *keys;			/* runtime filters, as scan keys */
	int			max_rows;		/* 0: the source's own unit */
	uint32		flags;			/* VEXEC_SRC_* */
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
	 * with no quals and no keys; a source answers only at the start of a
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
	 * satisfies them, and never a row of a unit it reads.  vexec gives them
	 * only where passing a row over unread raises no error PostgreSQL would
	 * raise, and only under a NULLS order a key's NULLs never satisfy.
	 * Called between batches; the source copies what it keeps of them.
	 */
	void		(*set_keys) (void *state, int nkeys, const ScanKeyData *keys);
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
