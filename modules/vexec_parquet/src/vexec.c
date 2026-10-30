/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec.c
 *	  vexec's batch source and batch sink for vexec_parquet's tables -- the
 *	  contracts of pg_accel's vexec_source.h and vexec_sink.h, at the source's
 *	  minor version 12, as ch_table_engines' parquet access method serves
 *	  them over ClickHouse's readers (its src/vexec.c), here over Arrow's.
 *
 *	  The source.  vexec begins the scan itself, through table_beginscan(),
 *	  and hands it to begin(): the units -- row groups -- come from that
 *	  scan's listing and claims (scan.c), a parallel scan's shared among its
 *	  participants, each pruned by its statistics against the spec's quals
 *	  and keys, set_keys()'s and set_join_keys()'s (filter.c).  A unit's
 *	  columns are read by Arrow's reader on the calling thread -- the first
 *	  step's with the unit, a later step's only where next_rest() asks for
 *	  it -- and cut into batches of max_rows rows, each column pointing into
 *	  Arrow's own buffers where vexec's layout is Arrow's:
 *
 *		Arrow's column				vexec's layout			copies
 *		int16/32/64, float32/64,	FIXED, the width		none
 *		 uint32 (oid), int8 ("char")
 *		date32, timestamp[us]		FIXED, Arrow's epoch	none
 *		time64[us]					FIXED					none
 *		decimal128(p,s), s the		SCALED, 16 bytes		none
 *		 column's, p within its
 *		decimal64(p,s)				SCALED, 8 bytes			none
 *		string, binary				OFFSETS					none
 *		string_view, binary_view	VIEW					none
 *		fixed_size_binary(16) uuid	FIXED, 16				none
 *		an own file's image of		FIXED, its length		none
 *		 another fixed-length type
 *		boolean						BYTE_BOOL				the bits as bytes
 *		narrower integers, floats,	FIXED					converted
 *		 other units
 *		large_string, dictionaries	OFFSETS					the offsets, or
 *																the bytes
 *		anything else				DATUM, or FIXED for a	every value
 *									 by-value type
 *		a column a file lacks		CONST, its missing value	one value
 *
 *	  and the validity bitmap where the batch begins at a word, else copied.
 *	  An external file's values are checked as the slot path checks them
 *	  (read.c): text's encoding, NOT NULL, a date's and timestamp's range; a
 *	  text column's values are vouched whole (VEXEC_COL_TEXT_WHOLE), as they
 *	  were checked so here or as they were written.  A batch's arrays are
 *	  written over by the next batch; a retained one keeps them and the
 *	  unit's columns until released.
 *
 *	  aggregate() answers a unit from its statistics -- count(*), count, min
 *	  and max where they are exact and no row of it is deleted; estimate()
 *	  and estimate_ex() price the spec's columns' bytes in the units its
 *	  quals keep, and a fetch by TID as a data page of each column (scan.c's
 *	  point reads); fetch_columns() names the columns such a fetch reads.
 *	  next_unit() hands a unit of a local file to vexec's GPU executor where
 *	  every column is one it decodes as the CPU's reader does -- text from
 *	  elsewhere in a UTF-8 database and of no varchar's length, dates and
 *	  timestamps the row group's statistics show finite and in range, no data
 *	  page of version 2 under snappy or LZ4 -- its rows the snapshot does
 *	  not see as its deleted bits; any other unit is the CPU's, read by the
 *	  next batches alone.
 *
 *	  The sink.  VecInsert's batches go into the write buffer by column, in
 *	  the layouts its Parquet columns have (write.c).
 *
 *	  Both are registered while the postmaster preloads the library;
 *	  without vexec they are unused entries of its registries.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <unistd.h>

#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "datatype/timestamp.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "varatt.h"

#include "vexec_sink.h"
#include "vexec_source.h"

#include "vexec_parquet.h"
#include "convert.h"

#define MAX_STEPS VEXEC_SRC_MAX_STEPS

/* One batch's arrays; a retained one keeps the unit's columns too */
/* PQ21 (pg_vector_executor.md §3.40): a nested column of a batch, as its source's Datums are made of it */
typedef struct SrcNested
{
	VpqReadCol *rc;
	const vpq_array *a;
	int64		pos;
} SrcNested;

typedef struct SrcOut
{
	MemoryContext cxt;
	VexecColumn *columns;
	const VexecColumn **children;	/* PQ21: per column two, a nested one's; NULL until one is */
	SrcNested  *nested;
	ItemPointerData *tids;
	uint64	   *visible;
	bool		retained;
	vpq_chunk  *chunks[MAX_STEPS];
	vpq_chunk  *rest[MAX_STEPS];	/* PQ13: a later step's columns of the batch's rows alone */
	int64		pos;			/* its first row's place in its unit */
	int			n;
} SrcOut;

/* PQ12: a range of a unit's rows, a parallel scan's unit where few are kept */
typedef struct SrcPart
{
	int			unit;
	int64		lo;
	int64		hi;
} SrcPart;

typedef struct SrcState
{
	TableScanDesc scan;
	Relation	rel;
	VpqTable   *t;
	VpqListing *listing;
	MemoryContext cxt;
	MemoryContext file_cxt;		/* the open file's read modes */
	MemoryContext unit_cxt;		/* the unit's deleted rows */
	MemoryContext dev_cxt;		/* the last device unit's arrays */
	MemoryContext answer_cxt;	/* aggregate()'s by-reference answers */
	int			ncols;
	AttrNumber *attnums;
	uint8	   *want;
	bool		any_nested;		/* PQ21: a nested layout asked for */
	int			nsteps;			/* 1 without VEXEC_SRC_STEPS */
	int		   *col_step;
	bool		tids;
	uint32		max_rows;
	VpqPruner  *pruner;
	/* the open file */
	int			file;
	vpq_file   *handle;
	vpq_reader *reader;			/* opened as a unit is first read on the CPU (PQ2) */
	int			stat_file;		/* the file last stat()ed for a device unit, -1 none */
	int64		stat_size;
	int64		stat_mtime;
	VpqReadCol *rcols;			/* per spec column */
	int		   *chunk_pos;		/* per spec column: its index in its step's chunk, -1 none */
	int		   *step_fcols[MAX_STEPS];
	int			step_nfcols[MAX_STEPS];
	/* the current unit */
	int			unit;			/* -1 none */
	int64		lo;				/* PQ12: its first row in its row group */
	int64		base[MAX_STEPS];	/* each step's chunk's row of the unit's first */
	vpq_range  *ranges[MAX_STEPS];	/* PQ13: a later step's columns read by their pages */
	bool		whole[MAX_STEPS];	/* a step's columns read whole, not by pages */
	int64		rows;
	uint64		first_rowid;
	int64		pos;
	vpq_chunk  *chunks[MAX_STEPS];
	uint64	   *dead;
	int			ndead;
	int			deadpos;
	bool		one_unit;		/* the unit's rows alone, then false */
	SrcOut	   *out;
	int64		pruned;
	int64		units_read;		/* OT5: units read here, again where a device lost them */
	int64		units_device;	/* units handed to the device to read itself */
	bool		ended;
	/* PQ12: the scan's units cut into ranges of rows, or none (nparts -1 not yet asked) */
	VpqPruner  *cut_pruner;		/* the spec's quals alone, as every participant's */
	SrcPart    *parts;
	int			nparts;
	/* PQ14: per spec column, its chunk's text found UTF-8 whole before; its rows checked in order, -1 not */
	bool	   *text_known;
	int64	   *text_next;
} SrcState;

static uint64 null_row = 0;		/* a constant's validity: its one value NULL */

VpqScanStats vpq_scan_stats;
static const char zeros[16] = {0};

static bool
src_supports(Relation rel, AttrNumber attnum)
{
	VpqTable   *t;

	if (attnum == SelfItemPointerAttributeNumber)
		return true;
	t = vpq_table(rel);
	return attnum >= 1 && attnum <= t->natts && t->cols[attnum - 1].kind != VPQ_K_DROPPED;
}

static bool text_kind(VpqKind kind);
static void mark_text(SrcState *st);

static void
release_unit(SrcState *st)
{
	/* PQ14: not as the scan is cleaned up, an error's perhaps */
	if (!st->ended)
		mark_text(st);
	for (int s = 0; s < MAX_STEPS; s++)
	{
		if (st->chunks[s])
			vpq_chunk_release(st->chunks[s]);
		st->chunks[s] = NULL;
		if (st->ranges[s])
			vpq_range_close(st->ranges[s]);
		st->ranges[s] = NULL;
		st->whole[s] = false;
		st->base[s] = 0;
	}
	st->unit = -1;
	st->lo = 0;
	st->rows = 0;
	st->pos = 0;
	st->dead = NULL;
	st->ndead = 0;
	st->deadpos = 0;
	MemoryContextReset(st->unit_cxt);
}

static void
release_file(SrcState *st)
{
	release_unit(st);
	if (st->reader)
		vpq_reader_close(st->reader);
	if (st->handle)
		vpq_file_close(st->handle);
	st->reader = NULL;
	st->handle = NULL;
	st->file = -1;
	MemoryContextReset(st->file_cxt);
}

static void
src_cleanup(void *arg)
{
	SrcState   *st = arg;

	if (st->ended)
		return;
	st->ended = true;
	release_file(st);
	if (st->out)
		for (int s = 0; s < MAX_STEPS; s++)
		{
			if (st->out->retained && st->out->chunks[s])
				vpq_chunk_release(st->out->chunks[s]);
			if (st->out->rest[s])
				vpq_chunk_release(st->out->rest[s]);
			st->out->rest[s] = NULL;
		}
}

static void *
src_begin(TableScanDesc scan, const VexecSourceSpec *spec)
{
	Relation	rel = scan->rs_rd;
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "vexec_parquet source",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SrcState   *st = palloc0(sizeof(SrcState));
	uint32		flags = spec->size >= offsetof(VexecSourceSpec, flags) + sizeof(spec->flags) ? spec->flags : 0;
	MemoryContextCallback *cb;

	st->scan = scan;
	st->rel = rel;
	st->t = vpq_scan_table(scan);
	st->listing = vpq_scan_listing(scan);
	st->cxt = cxt;
	st->file_cxt = AllocSetContextCreate(cxt, "vexec_parquet source file", ALLOCSET_SMALL_SIZES);
	st->unit_cxt = AllocSetContextCreate(cxt, "vexec_parquet source unit", ALLOCSET_SMALL_SIZES);
	st->dev_cxt = AllocSetContextCreate(cxt, "vexec_parquet device unit", ALLOCSET_SMALL_SIZES);
	st->answer_cxt = AllocSetContextCreate(cxt, "vexec_parquet answers", ALLOCSET_SMALL_SIZES);
	st->file = -1;
	st->stat_file = -1;
	st->unit = -1;
	st->ncols = spec->ncolumns;
	st->attnums = palloc(sizeof(AttrNumber) * Max(st->ncols, 1));
	st->want = palloc(Max(st->ncols, 1));
	st->col_step = palloc0(sizeof(int) * Max(st->ncols, 1));
	st->text_known = palloc0(sizeof(bool) * Max(st->ncols, 1));
	st->text_next = palloc0(sizeof(int64) * Max(st->ncols, 1));
	for (int i = 0; i < st->ncols; i++)
	{
		st->attnums[i] = spec->attnums[i];
		st->want[i] = spec->layouts ? spec->layouts[i] : VEXEC_FIXED;
		/* PQ21 (§3.40): a nested layout asked for: lists and maps handed as Arrow holds them */
		if (st->want[i] == VEXEC_LIST || st->want[i] == VEXEC_MAP)
			st->any_nested = true;
	}
	st->tids = (flags & VEXEC_SRC_TIDS) != 0;
	st->max_rows = spec->max_rows > 0 ? (uint32) spec->max_rows : 1024;
	st->nsteps = 1;
	if ((flags & VEXEC_SRC_STEPS) && spec->size >= offsetof(VexecSourceSpec, step_ends) + sizeof(spec->step_ends) &&
		spec->nsteps >= 2)
	{
		int			s = 0;

		st->nsteps = Min(spec->nsteps, MAX_STEPS);
		for (int i = 0; i < st->ncols; i++)
		{
			while (s < st->nsteps - 1 && i >= spec->step_ends[s])
				s++;
			st->col_step[i] = s;
		}
	}
	st->pruner = vpq_pruner_make(st->t, spec->quals, spec->nkeys, spec->keys);
	st->cut_pruner = vpq_pruner_make(st->t, spec->quals, spec->nkeys, spec->keys);
	st->nparts = -1;
	cb = palloc0(sizeof(MemoryContextCallback));
	cb->func = src_cleanup;
	cb->arg = st;
	MemoryContextRegisterResetCallback(cxt, cb);
	MemoryContextSwitchTo(old);
	return st;
}

/*
 * The read modes of the spec's columns in a file, and each step's file
 * columns -- of its facts where the listing has them, no footer read
 * (PQ1, PQ4); Arrow's reader is opened as a unit is first read (PQ2)
 */
static void
open_file(SrcState *st, int file)
{
	VpqFileEntry *f = &st->listing->files[file];
	MemoryContext old;
	int			n[MAX_STEPS] = {0};

	if (st->file == file)
		return;
	release_file(st);
	st->handle = vpq_open_file(f);
	st->file = file;
	old = MemoryContextSwitchTo(st->file_cxt);
	st->rcols = palloc0(sizeof(VpqReadCol) * Max(st->ncols, 1));
	st->chunk_pos = palloc(sizeof(int) * Max(st->ncols, 1));
	for (int s = 0; s < st->nsteps; s++)
		st->step_fcols[s] = palloc(sizeof(int) * Max(st->ncols, 1));
	for (int i = 0; i < st->ncols; i++)
	{
		VpqColumn  *col = &st->t->cols[st->attnums[i] - 1];
		int			s = st->col_step[i];

		vpq_read_col_setup(st->t, col, st->handle, &st->rcols[i], !st->t->external);
		st->chunk_pos[i] = -1;
		if (st->rcols[i].fcol >= 0)
		{
			st->step_fcols[s][n[s]] = st->rcols[i].fcol;
			st->chunk_pos[i] = n[s]++;
		}
	}
	for (int s = 0; s < st->nsteps; s++)
		st->step_nfcols[s] = n[s];
	MemoryContextSwitchTo(old);
}

/*
 * Arrow's reader of the open file, as a unit is first read on the CPU (PQ2):
 * of the scan's columns alone, over a footer of them, where the shared
 * footer cache has the file's footer's index (PQ11, pg_vector_executor.md
 * §3.37) -- else over its footer whole, whose index is kept then for the
 * next processes
 */
static void
open_reader(SrcState *st)
{
	VpqFileEntry *f = &st->listing->files[st->file];
	const char *index = NULL;
	Size		len = 0;

	if (st->reader != NULL)
		return;
	if (vpq_enable_projected_footers && f->mtime >= 0 && !vpq_file_has_footer(st->handle))
		index = vpq_file_index_get(f, &len);
	if (index != NULL)
	{
		int		   *cols = palloc(sizeof(int) * Max(st->ncols, 1));
		int			n = 0;

		for (int s = 0; s < st->nsteps; s++)
			for (int c = 0; c < st->step_nfcols[s]; c++)
				cols[n++] = st->step_fcols[s][c];
		VPQ_CHECK(vpq_reader_open_cols(st->handle, NULL, cols, n, index, len, &st->reader),
				  psprintf("opening \"%s\"", f->path));
		pfree(cols);
		pfree((char *) index);
		return;
	}
	VPQ_CHECK(vpq_reader_open(st->handle, NULL, &st->reader), psprintf("opening \"%s\"", f->path));
	if (vpq_enable_projected_footers && f->mtime >= 0)
		vpq_file_index_keep(f, st->handle);
}

/* A step's columns of the unit's row group, read whole as first asked for */
static void
read_step(SrcState *st, int step)
{
	VpqUnitRef *ref = &st->listing->units[st->unit];
	VpqFileEntry *f = &st->listing->files[ref->file];

	if (st->chunks[step] != NULL)
		return;
	open_reader(st);
	VPQ_CHECK(vpq_reader_read(st->reader, ref->rg, st->step_fcols[step], st->step_nfcols[step],
							  &st->chunks[step]),
			  psprintf("reading row group %d of \"%s\"", ref->rg, f->path));
	if (vpq_chunk_rows(st->chunks[step]) != f->unit_rows[ref->rg])
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("row group %d of \"%s\" has " INT64_FORMAT " rows where its footer says " INT64_FORMAT,
						ref->rg, f->path, vpq_chunk_rows(st->chunks[step]), f->unit_rows[ref->rg])));
	/* a range of the row group's rows the unit's (PQ12): its rows from its first */
	st->base[step] = st->lo;
	st->whole[step] = true;
}

/*
 * PQ12 and PQ13 (pg_vector_executor.md §3.37): rows [lo, lo + n) of the
 * unit's row group's columns of a step, read forward by the pages that
 * hold them -- the others passed over -- as a chunk of their own; NULL
 * where the step's columns are not read so (vpq_range_open()), its whole
 * column chunks then read as before.  A local file's alone.
 */
static vpq_chunk *
read_range(SrcState *st, int step, int64 lo, int64 n)
{
	VpqUnitRef *ref = &st->listing->units[st->unit];
	VpqFileEntry *f = &st->listing->files[ref->file];
	vpq_chunk  *chunk = NULL;
	int			rc;

	if (st->whole[step] || st->step_nfcols[step] == 0 || f->path[0] != '/')
		return NULL;
	open_reader(st);
	if (st->ranges[step] == NULL)
	{
		rc = vpq_range_open(st->reader, ref->rg, st->step_fcols[step], st->step_nfcols[step], &st->ranges[step]);
		if (rc == VPQ_NOT_FOUND)
		{
			st->whole[step] = true;
			return NULL;
		}
		VPQ_CHECK(rc, psprintf("reading row group %d of \"%s\"", ref->rg, f->path));
	}
	rc = vpq_range_read(st->ranges[step], lo, n, &chunk);
	if (rc == VPQ_NOT_FOUND)
	{
		vpq_range_close(st->ranges[step]);
		st->ranges[step] = NULL;
		st->whole[step] = true;
		return NULL;
	}
	VPQ_CHECK(rc, psprintf("reading row group %d of \"%s\"", ref->rg, f->path));
	return chunk;
}

/* A unit made the current one -- rows [lo, hi) of its row group, all of them where hi is -1 -- its first step read */
static void
begin_unit_rows(SrcState *st, int unit, int64 lo, int64 hi)
{
	VpqUnitRef *ref = &st->listing->units[unit];
	VpqFileEntry *f = &st->listing->files[ref->file];
	int64		rg_rows = f->unit_rows[ref->rg];
	MemoryContext old;

	st->units_read++;

	release_unit(st);
	open_file(st, ref->file);
	if (hi < 0)
		hi = rg_rows;
	st->unit = unit;
	st->lo = lo;
	st->rows = hi - lo;
	st->first_rowid = f->first_rowid + (uint64) f->unit_first[ref->rg] + (uint64) lo;
	st->pos = 0;
	/* PQ14: each column's text, whether a reader found its chunk UTF-8 whole before */
	for (int i = 0; i < st->ncols; i++)
	{
		VpqReadCol *rc = &st->rcols[i];

		st->text_next[i] = 0;
		st->text_known[i] = vpq_enable_text_marks && !rc->own && rc->fcol >= 0 && text_kind(rc->col->kind) &&
			vpq_file_text_checked(st->handle, ref->rg, rc->fcol);
	}
	if (lo > 0 || hi < rg_rows)
	{
		/* PQ12: a range of the row group's rows, its first step's pages alone where they can be */
		vpq_scan_stats.units_cut++;
		st->chunks[0] = read_range(st, 0, lo, hi - lo);
		if (st->ranges[0] != NULL)
		{
			vpq_range_close(st->ranges[0]);
			st->ranges[0] = NULL;
		}
		st->whole[0] = false;
	}
	if (st->chunks[0] == NULL)
		read_step(st, 0);
	old = MemoryContextSwitchTo(st->unit_cxt);
	st->dead = vpq_scan_dead_rows(st->scan, f, st->first_rowid, st->first_rowid + (uint64) st->rows - 1,
								  &st->ndead);
	MemoryContextSwitchTo(old);
	st->deadpos = 0;
}

typedef struct StatArg
{
	VpqTable   *t;
	VpqFileEntry *f;
	int			rg;
} StatArg;

static const VpqStat *
unit_stat(void *arg, AttrNumber attnum)
{
	StatArg    *a = arg;

	return vpq_unit_stat(a->t, a->f, a->rg, attnum);
}

static bool
unit_may_match(SrcState *st, int unit)
{
	VpqUnitRef *ref = &st->listing->units[unit];
	StatArg		a;

	if (!vpq_pruner_active(st->pruner))
		return true;
	a.t = st->t;
	a.f = &st->listing->files[ref->file];
	a.rg = ref->rg;
	return vpq_pruner_may_match(st->pruner, unit_stat, &a, a.f->unit_rows[ref->rg]);
}

/* A unit made the current one, its first step read */
static void
begin_unit(SrcState *st, int unit)
{
	begin_unit_rows(st, unit, 0, -1);
}

/*
 * PQ12 (pg_vector_executor.md §3.37): a parallel scan's units cut into
 * ranges of a row group's rows where the units its quals keep are fewer
 * than twice the participants a gather may have -- ClickBench's Q36 to Q42
 * at 100M rows keep 3 row groups, read by 3 of 17 processes -- so that each
 * participant decodes a range's pages alone (read_range()).  Every
 * participant cuts them alike: by the listing, which a parallel scan's
 * leader hands its workers, the spec's quals and keys, its settings; its
 * runtime keys prune the ranges it claims, not the cut.  Not where a device
 * may read the scan's units -- vexec.gpu on, with device units -- whose
 * units are row groups whole; not a serial scan's.
 */
#define PART_LEAST_ROWS 65536

static void
make_parts(SrcState *st)
{
	int			target = 2 * (max_parallel_workers_per_gather + 1);
	int			kept = 0;
	int64		rows = 0;
	int64		part_rows;
	int			n = 0;
	bool	   *keep;

	st->nparts = 0;
	/* nor a scan whose quals keep every unit: aggregate() may answer its units from their statistics */
	if (!vpq_enable_row_ranges || !vpq_scan_is_parallel(st->scan) || max_parallel_workers_per_gather < 1 ||
		!vpq_pruner_active(st->cut_pruner) || vpq_device_may_read())
		return;
	keep = palloc0(sizeof(bool) * Max(st->listing->nunits, 1));
	for (int u = 0; u < st->listing->nunits; u++)
	{
		VpqUnitRef *ref = &st->listing->units[u];
		StatArg		a;

		a.t = st->t;
		a.f = &st->listing->files[ref->file];
		a.rg = ref->rg;
		if (vpq_pruner_active(st->cut_pruner) &&
			!vpq_pruner_may_match(st->cut_pruner, unit_stat, &a, a.f->unit_rows[ref->rg]))
			continue;
		keep[u] = true;
		kept++;
		rows += a.f->unit_rows[ref->rg];
		if (kept >= target)
		{
			pfree(keep);
			return;
		}
	}
	part_rows = Max(PART_LEAST_ROWS, (rows + target - 1) / target);
	for (int u = 0; u < st->listing->nunits; u++)
		if (keep[u])
		{
			VpqUnitRef *ref = &st->listing->units[u];
			int64		r = st->listing->files[ref->file].unit_rows[ref->rg];

			n += (int) Max(1, r / part_rows);
		}
	if (n <= kept)
	{
		pfree(keep);
		return;
	}
	st->parts = MemoryContextAlloc(st->cxt, sizeof(SrcPart) * n);
	for (int u = 0; u < st->listing->nunits; u++)
		if (keep[u])
		{
			VpqUnitRef *ref = &st->listing->units[u];
			int64		r = st->listing->files[ref->file].unit_rows[ref->rg];
			int			k = (int) Max(1, r / part_rows);

			for (int j = 0; j < k; j++)
			{
				SrcPart    *p = &st->parts[st->nparts++];

				p->unit = u;
				p->lo = r * j / k;
				p->hi = r * (j + 1) / k;
			}
		}
	pfree(keep);
}

/* The scan's next unit its statistics do not rule out -- with *lo and *hi its rows, -1 all (PQ12) */
static bool
claim_unit(SrcState *st, int *unit, int64 *lo, int64 *hi)
{
	*lo = 0;
	*hi = -1;
	if (st->nparts < 0)
		make_parts(st);
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (st->nparts > 0)
		{
			int			slot;

			if (!vpq_scan_claim_slot(st->scan, st->nparts, &slot))
				return false;
			*unit = st->parts[slot].unit;
			*lo = st->parts[slot].lo;
			*hi = st->parts[slot].hi;
		}
		else if (!vpq_scan_claim_unit(st->scan, unit))
			return false;
		if (unit_may_match(st, *unit))
			return true;
		st->pruned++;
	}
}

static SrcOut *
next_out(SrcState *st)
{
	SrcOut	   *out = st->out;

	if (out != NULL && !out->retained)
	{
		for (int s = 0; s < MAX_STEPS; s++)
		{
			if (out->rest[s])
				vpq_chunk_release(out->rest[s]);
			out->rest[s] = NULL;
		}
		MemoryContextReset(out->cxt);
		return out;
	}
	out = MemoryContextAllocZero(st->cxt, sizeof(SrcOut));
	out->cxt = AllocSetContextCreate(st->cxt, "vexec_parquet batch", ALLOCSET_DEFAULT_SIZES);
	st->out = out;
	return out;
}

static void
set_shape(VexecColumn *vc, uint8 layout, int width, int scale)
{
	vc->layout = layout;
	vc->encoding = VEXEC_FLAT;
	vc->arrow_values = false;
	vc->width = width;
	vc->stride = width;
	vc->scale = (int16) scale;
}

static bool
bit_at(const uint8 *bits, int64 i)
{
	return (bits[i >> 3] >> (i & 7)) & 1;
}

/* A batch's validity: Arrow's own bits where the batch begins at a word, else copied */
static const uint64 *
validity_of(SrcOut *out, const vpq_array *a, int64 pos, int n)
{
	int64		start;
	uint64	   *w;

	if (a == NULL || a->null_count == 0 || a->validity == NULL)
		return NULL;
	start = a->offset + pos;
	if (start % 64 == 0 && ((uintptr_t) (a->validity + start / 8)) % sizeof(uint64) == 0)
		return (const uint64 *) (a->validity + start / 8);
	w = MemoryContextAllocZero(out->cxt, sizeof(uint64) * ((n + 63) / 64 + 1));
	for (int k = 0; k < n; k++)
		if (bit_at(a->validity, start + k))
			w[k >> 6] |= UINT64CONST(1) << (k & 63);
	return w;
}

/*
 * PQ21 (§3.40): whether a list or a map is handed in the nested layout vexec
 * asked for, its children where Arrow holds them: a list of int32 offsets
 * whose elements are read without a check that may fail -- a fixed width of
 * the element type's own, text of no typmod -- a map of strings as hstore
 */
static bool
nested_as_it_lies(SrcState *st, int i, VpqReadCol *rc, const vpq_array *a)
{
	int			want = st->want[i];

	if (rc->own || a == NULL || a->child == NULL || rc->kids == NULL)
		return false;
	if (want == VEXEC_LIST && rc->mode == VPQ_RM_LIST && a->type.id == VPQ_T_LIST)
	{
		VpqReadCol *k = &rc->kids[0];

		if (k->mode == VPQ_RM_FIXED)
			return k->col->typlen > 0;
		return k->mode == VPQ_RM_STRING && k->col->kind == VPQ_K_TEXT && k->col->typmod < 0;
	}
	if (want == VEXEC_MAP && rc->mode == VPQ_RM_MAP_HSTORE && a->type.id == VPQ_T_MAP && a->child2 != NULL)
		return rc->kids[0].mode == VPQ_RM_STRING && rc->kids[1].mode == VPQ_RM_STRING;
	return false;
}

/*
 * A nested column's child as Arrow holds it, from the batch's first element
 * -- lo of the row group's -- its rows those of the batch's offsets, count of them
 */
static VexecColumn *
nested_child(SrcOut *out, VpqReadCol *k, const vpq_array *c, int32 lo, int32 count)
{
	VexecColumn *cc = MemoryContextAllocZero(out->cxt, sizeof(VexecColumn));

	if (k->mode == VPQ_RM_FIXED)
	{
		set_shape(cc, VEXEC_FIXED, k->col->typlen, 0);
		cc->values = c->values + (c->offset + lo) * k->col->typlen;
	}
	else
	{
		set_shape(cc, VEXEC_OFFSETS, 0, 0);
		cc->values = c->values + (c->offset + lo) * sizeof(int32);
		cc->buffers = (const void *const *) MemoryContextAlloc(out->cxt, sizeof(void *));
		((const void **) cc->buffers)[0] = c->data ? c->data : (const uint8 *) zeros;
		cc->buffer_sizes = MemoryContextAlloc(out->cxt, sizeof(int64));
		((int64 *) cc->buffer_sizes)[0] = c->data_size;
		cc->nbuffers = 1;
	}
	cc->validity = validity_of(out, c, lo, count);
	return cc;
}

/* PQ21: a nested column in its layout -- its offsets from the batch's first row, its children -- and its Datums' maker */
static void
nested_column(SrcState *st, SrcOut *out, VexecColumn *vc, int i, VpqReadCol *rc, const vpq_array *a,
			  int64 pos, int n)
{
	const int32 *aoff = (const int32 *) a->values + (a->offset + pos);
	int32		lo = aoff[0];
	int32		count = aoff[n] - lo;
	int32	   *off = MemoryContextAlloc(out->cxt, sizeof(int32) * (n + 1));

	/* the batch's offsets from its first element: its children begin there, a batch's elements alone */
	for (int k = 0; k <= n; k++)
		off[k] = aoff[k] - lo;
	if (out->children == NULL)
	{
		out->children = MemoryContextAllocZero(out->cxt, sizeof(VexecColumn *) * 2 * Max(st->ncols, 1));
		out->nested = MemoryContextAllocZero(out->cxt, sizeof(SrcNested) * Max(st->ncols, 1));
	}
	memset(vc, 0, sizeof(*vc));
	set_shape(vc, (uint8) st->want[i], 0, 0);
	vc->values = off;
	vc->validity = validity_of(out, a, pos, n);
	out->children[2 * i] = nested_child(out, &rc->kids[0], a->child, lo, count);
	if (st->want[i] == VEXEC_MAP)
		out->children[2 * i + 1] = nested_child(out, &rc->kids[1], a->child2, lo, count);
	out->nested[i].rc = rc;
	out->nested[i].a = a;
	out->nested[i].pos = pos;
}

/* PQ21: row `row` of nested column `column` of a batch as a Datum of its type: the row path's value (nested.c) */
static Datum
src_nested_datum(void *owner, int column, int row, bool *isnull)
{
	SrcOut	   *out = owner;
	SrcNested  *r = &out->nested[column];

	return vpq_value_at(r->rc, r->a, r->pos + row, isnull);
}

/* A constant column of one value, as a CONST of the column's own layout */
static void
const_column(SrcOut *out, VexecColumn *vc, VpqColumn *col, Datum value, bool isnull)
{
	memset(vc, 0, sizeof(*vc));
	if (col->typlen > 0)
	{
		char	   *v = MemoryContextAllocZero(out->cxt, Max(col->typlen, (int) sizeof(Datum)));

		if (!isnull)
		{
			if (col->typbyval)
				store_att_byval(v, value, col->typlen);
			else
				memcpy(v, DatumGetPointer(value), col->typlen);
		}
		set_shape(vc, col->kind == VPQ_K_BOOL ? VEXEC_BYTE_BOOL : VEXEC_FIXED, col->typlen, 0);
		vc->values = v;
	}
	else
	{
		Datum	   *d = MemoryContextAlloc(out->cxt, sizeof(Datum));

		*d = isnull ? (Datum) 0 : value;
		set_shape(vc, VEXEC_DATUM, 0, 0);
		vc->values = d;
	}
	vc->encoding = VEXEC_CONST;
	vc->validity = isnull ? &null_row : NULL;
}

/* Every value of the batch through read.c: FIXED for a fixed-length type, DATUM for a varlena */
static void
generic_column(SrcState *st, SrcOut *out, VexecColumn *vc, VpqReadCol *rc, const vpq_array *a,
			   int64 pos, int n)
{
	VpqColumn  *col = rc->col;
	uint64	   *w = NULL;
	MemoryContext old = MemoryContextSwitchTo(out->cxt);

	memset(vc, 0, sizeof(*vc));
	if (col->typlen > 0)
	{
		char	   *values = palloc0((Size) n * col->typlen);

		for (int k = 0; k < n; k++)
		{
			bool		isnull;
			Datum		d = vpq_value_at(rc, a, pos + k, &isnull);

			if (isnull)
			{
				if (w == NULL)
				{
					w = palloc(sizeof(uint64) * ((n + 63) / 64 + 1));
					memset(w, 0xff, sizeof(uint64) * ((n + 63) / 64 + 1));
				}
				w[k >> 6] &= ~(UINT64CONST(1) << (k & 63));
				continue;
			}
			if (col->typbyval)
				store_att_byval(values + (Size) k * col->typlen, d, col->typlen);
			else
				memcpy(values + (Size) k * col->typlen, DatumGetPointer(d), col->typlen);
		}
		set_shape(vc, col->kind == VPQ_K_BOOL ? VEXEC_BYTE_BOOL : VEXEC_FIXED, col->typlen, 0);
		vc->values = values;
	}
	else
	{
		Datum	   *datums = palloc0(sizeof(Datum) * Max(n, 1));

		for (int k = 0; k < n; k++)
		{
			bool		isnull;

			datums[k] = vpq_value_at(rc, a, pos + k, &isnull);
			if (isnull)
			{
				if (w == NULL)
				{
					w = palloc(sizeof(uint64) * ((n + 63) / 64 + 1));
					memset(w, 0xff, sizeof(uint64) * ((n + 63) / 64 + 1));
				}
				w[k >> 6] &= ~(UINT64CONST(1) << (k & 63));
				datums[k] = (Datum) 0;
			}
		}
		set_shape(vc, VEXEC_DATUM, 0, 0);
		vc->values = datums;
	}
	vc->validity = w;
	MemoryContextSwitchTo(old);
}

/* An external NOT NULL column's NULLs in a batch: the slot path's error */
static void
check_not_null(VpqReadCol *rc, const vpq_array *a, int64 pos, int n)
{
	if (rc->own || !rc->col->notnull || a == NULL)
		return;
	if (a->type.id == VPQ_T_NULL && n > 0)
		(void) vpq_value_at(rc, a, pos, &(bool) {false});
	if (a->null_count == 0 || a->validity == NULL)
		return;
	for (int k = 0; k < n; k++)
		if (vpq_array_null(a, pos + k))
		{
			bool		isnull;

			(void) vpq_value_at(rc, a, pos + k, &isnull);
		}
}

static bool
text_kind(VpqKind kind)
{
	return kind == VPQ_K_TEXT || kind == VPQ_K_BPCHAR || kind == VPQ_K_JSON;
}

/*
 * PQ9 (pg_vector_executor.md §3.37): whether n strings, the bytes
 * [off(0), off(n)) of base, are each UTF-8 whole -- their bytes checked
 * together, once, and each one's first byte none of a character's
 * continuation bytes, which with that check makes each string whole:
 * nothing invalid, no NUL, no character cut at a string's edge.  False
 * where not -- the caller then checks each alone, for its own error -- or
 * where they are too many bytes for one check.  A value one at a time took
 * 9% of ClickBench's Q20 at 100M rows on the CPU.
 */
static bool
text_whole(const char *base, const int32 *off32, const int64 *off64, int n)
{
	int64		lo = off32 ? off32[0] : off64[0];
	int64		hi = off32 ? off32[n] : off64[n];

	if (hi - lo > PG_INT32_MAX / 2 || hi < lo)
		return false;
	if (!pg_verify_mbstr(PG_UTF8, base + lo, (int) (hi - lo), true))
		return false;
	for (int k = 0; k < n; k++)
	{
		int64		b = off32 ? off32[k] : off64[k];
		int64		e = off32 ? off32[k + 1] : off64[k + 1];

		if (e > b && (((const uint8 *) base)[b] & 0xC0) == 0x80)
			return false;
	}
	return true;
}

/*
 * PQ14 (pg_vector_executor.md §3.37): whether column i's text in the unit
 * is of a chunk a reader found UTF-8 whole before, its check passed over
 */
static bool
text_known(SrcState *st, int i)
{
	if (!st->text_known[i])
		return false;
	vpq_scan_stats.text_known++;
	return true;
}

/* PQ14: column i's rows [upos, upos + n) of the unit checked -- counted on where they follow those before */
static void
text_checked(SrcState *st, int i, int64 upos, int n)
{
	if (st->text_next[i] == upos)
		st->text_next[i] = upos + n;
	else if (st->text_next[i] < upos)
		st->text_next[i] = -1;
}

/*
 * PQ14: the unit's chunks of another writer's text whose every value was
 * checked, in order -- the unit its row group whole -- marked so in the
 * file's facts in the shared footer cache, for each later reader to pass
 * their check over.  The check of a value a time took 17% of ClickBench's
 * Q20 and Q22 at 100M rows on the CPU after PQ9, and found the same bytes
 * whole at every query.
 */
static void
mark_text(SrcState *st)
{
	VpqUnitRef *ref;
	VpqFileEntry *f;

	if (!vpq_enable_text_marks || st->unit < 0 || st->lo != 0 || st->rows <= 0)
		return;
	ref = &st->listing->units[st->unit];
	f = &st->listing->files[ref->file];
	if (st->rows != f->unit_rows[ref->rg])
		return;
	for (int i = 0; i < st->ncols; i++)
	{
		uint8		byte;
		int64		at;

		if (st->text_known[i] || st->text_next[i] != st->rows)
			continue;
		at = vpq_file_text_mark(st->handle, ref->rg, st->rcols[i].fcol, &byte);
		if (at >= 0 && vpq_footers_set_byte(f->path, f->size, f->mtime, (Size) at, (char) byte))
			vpq_scan_stats.text_marked++;
	}
}

/*
 * Whether a string column of a batch is handed as Arrow's bytes, its values
 * checked where they come from elsewhere -- rows [pos, pos + n) of the
 * array, upos their place in the unit
 */
static bool
strings_as_they_lie(SrcState *st, int i, const vpq_array *a, int64 pos, int64 upos, int n)
{
	VpqReadCol *rc = &st->rcols[i];
	VpqColumn  *col = rc->col;

	if (rc->own)
		return true;
	if (col->kind == VPQ_K_BYTEA)
		return true;
	/* text from elsewhere: the database's encoding, and no typmod to check */
	if (GetDatabaseEncoding() != PG_UTF8 || col->kind == VPQ_K_BPCHAR ||
		(col->basetype == VARCHAROID && col->typmod >= 0) || col->kind == VPQ_K_JSON)
		return false;
	/* the other layouts gathered into Arrow's string layout by the caller, and checked there */
	if (rc->mode != VPQ_RM_STRING && rc->mode != VPQ_RM_VIEW)
		return true;
	if (text_known(st, i))
		return true;
	/* PQ9: a run of strings checked at once */
	if (n > 0 && rc->mode == VPQ_RM_STRING &&
		text_whole((const char *) a->data, (const int32 *) a->values + a->offset + pos, NULL, n))
	{
		vpq_scan_stats.text_batches++;
		text_checked(st, i, upos, n);
		return true;
	}
	for (int k = 0; k < n; k++)
	{
		int			len;
		const char *bytes;

		if (vpq_array_null(a, pos + k))
			continue;
		bytes = vpq_bytes_at(a, rc->mode, a->offset + pos + k, &len);
		(void) pg_verify_mbstr(PG_UTF8, bytes, len, false);
	}
	vpq_scan_stats.text_values += n;
	text_checked(st, i, upos, n);
	return true;
}

/* Column i of the spec for rows [pos, pos + n) of the unit, into the batch */
static void
fill_column(SrcState *st, SrcOut *out, int i, int64 pos, int n)
{
	VpqReadCol *rc = &st->rcols[i];
	VpqColumn  *col = rc->col;
	VexecColumn *vc = &out->columns[i];
	const vpq_array *a;
	int64		start;
	int64		upos = pos;		/* PQ14: the rows' place in the unit */

	if (rc->mode == VPQ_RM_MISSING)
	{
		const_column(out, vc, col, col->missing, col->missing_null);
		return;
	}
	if (out->rest[st->col_step[i]] != NULL)
	{
		/* PQ13: the batch's own rows of a later step's columns */
		a = vpq_chunk_array(out->rest[st->col_step[i]], st->chunk_pos[i]);
		pos = 0;
	}
	else
	{
		a = vpq_chunk_array(st->chunks[st->col_step[i]], st->chunk_pos[i]);
		pos += st->base[st->col_step[i]];
	}
	if (rc->mode == VPQ_RM_NULL)
	{
		check_not_null(rc, a, pos, n);
		const_column(out, vc, col, (Datum) 0, true);
		return;
	}
	check_not_null(rc, a, pos, n);
	memset(vc, 0, sizeof(*vc));
	start = a->offset + pos;
	/* PQ21 (§3.40): a list or a map vexec asked for nested, as Arrow holds it */
	if ((st->want[i] == VEXEC_LIST || st->want[i] == VEXEC_MAP) && nested_as_it_lies(st, i, rc, a))
	{
		nested_column(st, out, vc, i, rc, a, pos, n);
		return;
	}
	switch (rc->mode)
	{
		case VPQ_RM_FIXED:
			{
				int			width = col->kind == VPQ_K_UUID ? 16 : col->typlen;

				set_shape(vc, VEXEC_FIXED, width, 0);
				vc->values = a->values + start * width;
				vc->validity = validity_of(out, a, pos, n);
				break;
			}
		case VPQ_RM_IMAGE_FIXED:
			set_shape(vc, VEXEC_FIXED, col->typlen, 0);
			vc->values = a->values + start * col->typlen;
			vc->validity = validity_of(out, a, pos, n);
			break;
		case VPQ_RM_BOOL:
			{
				uint8	   *bytes = MemoryContextAlloc(out->cxt, Max(n, 1));

				for (int k = 0; k < n; k++)
					bytes[k] = bit_at(a->values, start + k);
				set_shape(vc, VEXEC_BYTE_BOOL, 1, 0);
				vc->values = bytes;
				vc->validity = validity_of(out, a, pos, n);
				break;
			}
		case VPQ_RM_DATE32:
			if (!rc->own)
			{
				const int32 *v = (const int32 *) a->values + start;

				for (int k = 0; k < n; k++)
				{
					int64		d = (int64) v[k] - VPQ_EPOCH_DAYS;

					if (!DATE_NOT_FINITE(v[k]) && !vpq_array_null(a, pos + k) &&
						(d < PG_INT32_MIN || d > PG_INT32_MAX || !IS_VALID_DATE((DateADT) d)))
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("date out of range in column \"%s\"", col->name)));
				}
			}
			set_shape(vc, VEXEC_FIXED, 4, 0);
			vc->arrow_values = true;
			vc->values = a->values + start * 4;
			vc->validity = validity_of(out, a, pos, n);
			break;
		case VPQ_RM_INT_DAYS:
			{
				/* PQ6: PostgreSQL's dates of the file's days since 1970, each checked as read.c checks it */
				DateADT    *days = MemoryContextAlloc(out->cxt, sizeof(DateADT) * Max(n, 1));

				for (int k = 0; k < n; k++)
				{
					bool		isnull;

					days[k] = vpq_array_null(a, pos + k) ? 0 :
						DatumGetDateADT(vpq_value_at(rc, a, pos + k, &isnull));
				}
				set_shape(vc, VEXEC_FIXED, sizeof(DateADT), 0);
				vc->values = days;
				vc->validity = validity_of(out, a, pos, n);
				break;
			}
		case VPQ_RM_INT_SECONDS:
			{
				/* PQ6: PostgreSQL's timestamps of the file's seconds since 1970, each checked as read.c checks it */
				Timestamp  *ts = MemoryContextAlloc(out->cxt, sizeof(Timestamp) * Max(n, 1));

				for (int k = 0; k < n; k++)
				{
					bool		isnull;

					ts[k] = vpq_array_null(a, pos + k) ? 0 :
						DatumGetTimestamp(vpq_value_at(rc, a, pos + k, &isnull));
				}
				set_shape(vc, VEXEC_FIXED, sizeof(Timestamp), 0);
				vc->values = ts;
				vc->validity = validity_of(out, a, pos, n);
				break;
			}
		case VPQ_RM_TIMESTAMP:
			if (rc->ftype.unit == VPQ_UNIT_US)
			{
				if (!rc->own)
				{
					const int64 *v = (const int64 *) a->values + start;

					for (int k = 0; k < n; k++)
						if (!TIMESTAMP_NOT_FINITE(v[k]) && !vpq_array_null(a, pos + k) &&
							(v[k] < PG_INT64_MIN + VPQ_EPOCH_USECS ||
							 !IS_VALID_TIMESTAMP(v[k] - VPQ_EPOCH_USECS)))
							ereport(ERROR,
									(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
									 errmsg("timestamp out of range in column \"%s\"", col->name)));
				}
				set_shape(vc, VEXEC_FIXED, 8, 0);
				vc->arrow_values = true;
				vc->values = a->values + start * 8;
				vc->validity = validity_of(out, a, pos, n);
			}
			else
				generic_column(st, out, vc, rc, a, pos, n);
			break;
		case VPQ_RM_DECIMAL:
			if (col->kind == VPQ_K_NUMERIC && rc->ftype.scale == col->scale &&
				rc->ftype.precision <= col->precision &&
				(rc->ftype.id == VPQ_T_DECIMAL128 || rc->ftype.id == VPQ_T_DECIMAL64))
			{
				int			width = rc->ftype.id == VPQ_T_DECIMAL128 ? 16 : 8;

				set_shape(vc, VEXEC_SCALED, width, col->scale);
				vc->values = a->values + start * width;
				vc->validity = validity_of(out, a, pos, n);
			}
			else if (col->kind == VPQ_K_NUMERIC && rc->ftype.scale <= col->scale &&
					 rc->ftype.precision - rc->ftype.scale <= col->precision - col->scale &&
					 rc->ftype.id != VPQ_T_DECIMAL256)
			{
				/* a narrower scale or width: scaled up exactly, its digits within the column's */
				int128	   *values = MemoryContextAlloc(out->cxt, sizeof(int128) * Max(n, 1));
				int128		mul = vpq_pow10_128(col->scale - rc->ftype.scale);

				for (int k = 0; k < n; k++)
					values[k] = vpq_array_null(a, pos + k) ? 0 :
						vpq_decimal_at(a, &rc->ftype, start + k) * mul;
				set_shape(vc, VEXEC_SCALED, 16, col->scale);
				vc->values = values;
				vc->validity = validity_of(out, a, pos, n);
			}
			else
				generic_column(st, out, vc, rc, a, pos, n);
			break;
		case VPQ_RM_STRING:
			if (strings_as_they_lie(st, i, a, pos, upos, n))
			{
				set_shape(vc, VEXEC_OFFSETS, 0, 0);
				vc->values = a->values + start * sizeof(int32);
				vc->buffers = (const void *const *) MemoryContextAlloc(out->cxt, sizeof(void *));
				((const void **) vc->buffers)[0] = a->data ? a->data : (const uint8 *) zeros;
				vc->buffer_sizes = MemoryContextAlloc(out->cxt, sizeof(int64));
				((int64 *) vc->buffer_sizes)[0] = a->data_size;
				vc->nbuffers = 1;
				vc->validity = validity_of(out, a, pos, n);
			}
			else
				generic_column(st, out, vc, rc, a, pos, n);
			break;
		case VPQ_RM_VIEW:
			if (strings_as_they_lie(st, i, a, pos, upos, n))
			{
				set_shape(vc, VEXEC_VIEW, 16, 0);
				vc->values = a->values + start * 16;
				vc->buffers = (const void *const *) a->buffers;
				vc->buffer_sizes = (const int64 *) a->buffer_sizes;
				vc->nbuffers = a->nbuffers;
				vc->validity = validity_of(out, a, pos, n);
			}
			else
				generic_column(st, out, vc, rc, a, pos, n);
			break;
		case VPQ_RM_LARGE_STRING:
		case VPQ_RM_DICT:
		case VPQ_RM_FIXED_BINARY:
			if ((col->kind == VPQ_K_BYTEA || (rc->own && text_kind(col->kind)) ||
				 strings_as_they_lie(st, i, a, pos, upos, n)) && col->kind != VPQ_K_BPCHAR)
			{
				/* the values gathered into Arrow's string layout: int32 offsets and their bytes */
				int32	   *off = MemoryContextAlloc(out->cxt, sizeof(int32) * (n + 1));
				Size		total = 0;
				char	   *data;

				for (int k = 0; k < n; k++)
				{
					int			len = 0;

					if (!vpq_array_null(a, pos + k))
					{
						if (rc->mode == VPQ_RM_DICT)
						{
							const vpq_array *d = a->dictionary;
							int64		idx = vpq_dict_index(a, start + k);

							(void) vpq_bytes_at(d, d->type.id == VPQ_T_LARGE_STRING || d->type.id == VPQ_T_LARGE_BINARY ?
												VPQ_RM_LARGE_STRING : d->type.id == VPQ_T_STRING_VIEW ||
												d->type.id == VPQ_T_BINARY_VIEW ? VPQ_RM_VIEW : VPQ_RM_STRING,
												d->offset + idx, &len);
						}
						else
							(void) vpq_bytes_at(a, rc->mode, start + k, &len);
					}
					total += len;
				}
				if (total > (Size) PG_INT32_MAX)
				{
					generic_column(st, out, vc, rc, a, pos, n);
					break;
				}
				data = MemoryContextAlloc(out->cxt, Max(total, 1));
				off[0] = 0;
				for (int k = 0; k < n; k++)
				{
					int			len = 0;
					const char *bytes = NULL;

					if (!vpq_array_null(a, pos + k))
					{
						if (rc->mode == VPQ_RM_DICT)
						{
							const vpq_array *d = a->dictionary;
							int64		idx = vpq_dict_index(a, start + k);

							bytes = vpq_bytes_at(d, d->type.id == VPQ_T_LARGE_STRING || d->type.id == VPQ_T_LARGE_BINARY ?
												 VPQ_RM_LARGE_STRING : d->type.id == VPQ_T_STRING_VIEW ||
												 d->type.id == VPQ_T_BINARY_VIEW ? VPQ_RM_VIEW : VPQ_RM_STRING,
												 d->offset + idx, &len);
						}
						else
							bytes = vpq_bytes_at(a, rc->mode, start + k, &len);
					}
					if (len > 0)
						memcpy(data + off[k], bytes, len);
					off[k + 1] = off[k] + len;
				}
				if (!rc->own && text_kind(col->kind) && !text_known(st, i))
				{
					/* PQ9: checked at once where it can be, else a value at a time */
					if (n > 0 && text_whole(data, off, NULL, n))
						vpq_scan_stats.text_batches++;
					else
					{
						for (int k = 0; k < n; k++)
							(void) pg_verify_mbstr(PG_UTF8, data + off[k], off[k + 1] - off[k], false);
						vpq_scan_stats.text_values += n;
					}
					text_checked(st, i, upos, n);
				}
				set_shape(vc, VEXEC_OFFSETS, 0, 0);
				vc->values = off;
				vc->buffers = (const void *const *) MemoryContextAlloc(out->cxt, sizeof(void *));
				((const void **) vc->buffers)[0] = data;
				vc->buffer_sizes = MemoryContextAlloc(out->cxt, sizeof(int64));
				((int64 *) vc->buffer_sizes)[0] = (int64) total;
				vc->nbuffers = 1;
				vc->validity = validity_of(out, a, pos, n);
			}
			else
				generic_column(st, out, vc, rc, a, pos, n);
			break;
		default:
			generic_column(st, out, vc, rc, a, pos, n);
			break;
	}
	if (text_kind(col->kind))
		vc->flags = VEXEC_COL_TEXT_WHOLE;
}

/*
 * The batch's rows the snapshot does not see, as visible bits, or NULL where
 * it sees every one.  The bits past the batch's rows are 0: vexec counts a
 * word's bits whole.
 */
static const uint64 *
visible_of(SrcState *st, SrcOut *out, int n, int64 *nvisible)
{
	uint64		lo = st->first_rowid + (uint64) st->pos;
	uint64		hi = lo + (uint64) n;
	uint64	   *w = NULL;

	*nvisible = n;
	while (st->deadpos < st->ndead && st->dead[st->deadpos] < lo)
		st->deadpos++;
	for (int d = st->deadpos; d < st->ndead && st->dead[d] < hi; d++)
	{
		int			k = (int) (st->dead[d] - lo);

		if (w == NULL)
		{
			w = MemoryContextAllocZero(out->cxt, sizeof(uint64) * ((n + 63) / 64 + 1));
			memset(w, 0xff, sizeof(uint64) * (n / 64));
			if (n % 64 != 0)
				w[n / 64] = (UINT64CONST(1) << (n % 64)) - 1;
		}
		w[k >> 6] &= ~(UINT64CONST(1) << (k & 63));
		(*nvisible)--;
	}
	return w;
}

static bool
src_next(void *arg, VexecSourceBatch *batch)
{
	SrcState   *st = arg;
	SrcOut	   *out;
	int			n;
	int64		nvisible;

	for (;;)
	{
		int			unit;
		int64		lo;
		int64		hi;

		if (st->unit >= 0 && st->pos < st->rows)
			break;
		if (st->unit >= 0)
		{
			release_unit(st);
			if (st->one_unit)
			{
				st->one_unit = false;
				return false;
			}
		}
		if (!claim_unit(st, &unit, &lo, &hi))
			return false;
		begin_unit_rows(st, unit, lo, hi);
	}
	n = (int) Min((int64) st->max_rows, st->rows - st->pos);
	out = next_out(st);
	out->pos = st->pos;
	out->n = n;
	out->columns = MemoryContextAllocZero(out->cxt, sizeof(VexecColumn) * Max(st->ncols, 1));
	/* PQ21: the nested columns' children and Datums, every step's, where vexec asked for one */
	out->children = NULL;
	out->nested = NULL;
	if (st->any_nested)
	{
		out->children = MemoryContextAllocZero(out->cxt, sizeof(VexecColumn *) * 2 * Max(st->ncols, 1));
		out->nested = MemoryContextAllocZero(out->cxt, sizeof(SrcNested) * Max(st->ncols, 1));
	}
	for (int i = 0; i < st->ncols; i++)
		if (st->col_step[i] == 0)
			fill_column(st, out, i, st->pos, n);
	if (st->tids)
	{
		out->tids = MemoryContextAlloc(out->cxt, sizeof(ItemPointerData) * Max(n, 1));
		for (int k = 0; k < n; k++)
			vpq_set_tid(&out->tids[k], st->first_rowid + (uint64) (st->pos + k));
	}
	out->visible = (uint64 *) visible_of(st, out, n, &nvisible);
	batch->nrows = n;
	batch->visible = out->visible;
	batch->tids = out->tids;
	batch->columns = out->columns;
	batch->owner = out;
	if (st->any_nested)
	{
		batch->children = (const VexecColumn *const *) out->children;
		batch->nested_datum = src_nested_datum;
	}
	st->pos += n;
	if (pgstat_should_count_relation(st->rel))
		st->rel->pgstat_info->counts.tuples_returned += nvisible;
	return true;
}

/*
 * A later step's columns of the batch next() handed last: the pages that
 * hold its rows alone, read forward through the unit (PQ13,
 * pg_vector_executor.md §3.37) -- a batch with no row left never asks, and
 * the pages wholly between two that do are passed over undecompressed --
 * else its unit's columns read whole as first asked for.  ClickBench's Q22
 * at 100M rows reads URL for a fifth of its batches, and decoded every row
 * group's whole.
 */
static void
src_next_rest(void *arg, VexecSourceBatch *batch, int step, const uint64 *selected)
{
	SrcState   *st = arg;
	SrcOut	   *out = batch->owner;

	if (step <= 0 || step >= st->nsteps)
		elog(ERROR, "vexec_parquet: no step %d of a scan of %d", step, st->nsteps);
	if (vpq_enable_step_pages && st->chunks[step] == NULL && out->rest[step] == NULL)
	{
		out->rest[step] = read_range(st, step, st->lo + out->pos, out->n);
		if (out->rest[step] != NULL)
			vpq_scan_stats.step_batches++;
	}
	if (out->rest[step] == NULL)
		read_step(st, step);
	for (int i = 0; i < st->ncols; i++)
		if (st->col_step[i] == step)
			fill_column(st, out, i, out->pos, out->n);
	if (out->rest[step] == NULL && out->retained && out->chunks[step] == NULL)
	{
		vpq_chunk_retain(st->chunks[step]);
		out->chunks[step] = st->chunks[step];
	}
}

static void
src_retain(void *arg, void *owner)
{
	SrcState   *st = arg;
	SrcOut	   *out = owner;

	if (out->retained)
		return;
	out->retained = true;
	for (int s = 0; s < MAX_STEPS; s++)
		if (st->chunks[s] != NULL && out->chunks[s] == NULL)
		{
			vpq_chunk_retain(st->chunks[s]);
			out->chunks[s] = st->chunks[s];
		}
	if (st->out == out)
		st->out = NULL;
}

static void
src_release(void *arg, void *owner)
{
	SrcOut	   *out = owner;

	if (!out->retained)
		return;
	for (int s = 0; s < MAX_STEPS; s++)
	{
		if (out->chunks[s])
			vpq_chunk_release(out->chunks[s]);
		if (out->rest[s])
			vpq_chunk_release(out->rest[s]);
	}
	MemoryContextDelete(out->cxt);
	pfree(out);
}

static void
src_rescan(void *arg)
{
	SrcState   *st = arg;

	release_unit(st);
	st->one_unit = false;
	st->stat_file = -1;
	vpq_scan_rescan(st->scan, NULL, false, false, false, false);
	vpq_pruner_set_keys(st->pruner, 2, 0, NULL);
}

static void
src_end(void *arg)
{
	SrcState   *st = arg;

	src_cleanup(st);
	MemoryContextDelete(st->cxt);
}

static void
src_set_keys(void *arg, int nkeys, const ScanKeyData *keys)
{
	SrcState   *st = arg;

	vpq_pruner_set_keys(st->pruner, 1, nkeys, keys);
}

static void
src_set_join_keys(void *arg, int nkeys, const ScanKeyData *keys)
{
	SrcState   *st = arg;

	vpq_pruner_set_keys(st->pruner, 2, nkeys, keys);
}

/*
 * Estimates: the spec's columns' compressed bytes in the units its quals
 * keep, and their rows; the units; whether aggregate() answers them all
 */
static void
estimate_units(Relation rel, Snapshot snapshot, const VexecSourceSpec *spec, double *rows, double *bytes,
			   double *units, bool *exact)
{
	VpqTable   *t = vpq_table(rel);
	VpqListing *l = vpq_listing(rel, t, snapshot);
	VpqPruner  *pruner = vpq_pruner_make(t, spec->quals, spec->nkeys, spec->keys);
	bool		all_exact = !l->any_dead;

	*rows = *bytes = *units = 0;
	PG_TRY();
	{
		for (int u = 0; u < l->nunits; u++)
		{
			VpqUnitRef *ref = &l->units[u];
			VpqFileEntry *f = &l->files[ref->file];
			StatArg		a;

			a.t = t;
			a.f = f;
			a.rg = ref->rg;
			if (vpq_pruner_active(pruner) &&
				!vpq_pruner_may_match(pruner, unit_stat, &a, f->unit_rows[ref->rg]))
				continue;
			*rows += f->unit_rows[ref->rg];
			*units += 1;
			for (int i = 0; i < spec->ncolumns; i++)
			{
				AttrNumber	attnum = spec->attnums[i];

				if (attnum < 1 || attnum > t->natts)
					continue;
				*bytes += vpq_unit_bytes(t, f, ref->rg, attnum);
				if (all_exact)
				{
					const VpqStat *s = vpq_unit_stat(t, f, ref->rg, attnum);

					if (s == NULL || s->nulls < 0 || (s->nulls < f->unit_rows[ref->rg] &&
													   (!s->has_minmax || !s->exact || s->may_nan)))
						all_exact = false;
				}
			}
		}
	}
	PG_FINALLY();
	{
		vpq_listing_release(l);
	}
	PG_END_TRY();
	*exact = all_exact;
}

static void
src_estimate(Relation rel, Snapshot snapshot, const VexecSourceSpec *spec, double *rows, double *bytes)
{
	double		units;
	bool		exact;

	estimate_units(rel, snapshot, spec, rows, bytes, &units, &exact);
}

/* W55: a fetch's part of its own, in columns' parts (src_estimate_ex()) */
#define VPQ_FETCH_OWN_COLUMNS	8.0

/*
 * W55 (pg_vector_executor.md §3.36): the threads a fetch of ncols columns
 * reads their pages on at once (scan.c's fetch_pages()) -- a page a column,
 * vexec_parquet.fetch_threads at most, and no more than the machine's
 * processors, as the engine's pool takes them; 1 where one at a time
 */
static int
fetch_threads(int ncols)
{
	long		cpus = sysconf(_SC_NPROCESSORS_ONLN);
	int			n = Min(vpq_fetch_threads, ncols);

	if (!vpq_enable_point_reads || ncols < 2)
		return 1;
	if (cpus > 0)
		n = (int) Min((long) n, cpus);
	return Max(n, 1);
}

static void
src_estimate_ex(Relation rel, Snapshot snapshot, const VexecSourceSpec *spec, VexecSourceEstimate *est)
{
	bool		tids = spec->size >= offsetof(VexecSourceSpec, flags) + sizeof(spec->flags) &&
		(spec->flags & VEXEC_SRC_TIDS) != 0;
	double		rows;
	double		bytes;
	double		units;
	bool		exact;
	int			threads = fetch_threads(spec->ncolumns);

	estimate_units(rel, snapshot, spec, &rows, &bytes, &units, &exact);
	est->rows = rows;
	est->bytes = bytes;
	if (est->size >= offsetof(VexecSourceEstimate, units) + sizeof(est->units))
		est->units = units;
	if (est->size >= offsetof(VexecSourceEstimate, flags) + sizeof(est->flags))
		est->flags = exact && spec->quals == NIL && spec->nkeys == 0 && !tids ? VEXEC_EST_AGG_EXACT : 0;
	/*
	 * a row fetched by its TID (scan.c): each of the spec's columns' data
	 * page holding it, at most a page's bytes of its row group's -- the
	 * writer's page -- or, with point reads off, its row group's columns,
	 * once for the rows of one row group.  W55 (pg_vector_executor.md
	 * §3.36): the pages read at once on fetch_threads()'s threads -- a
	 * fetch's time a part of its own, its row group's reader, and a part a
	 * column, the columns' parts shared by the threads: on 9, 16 and 105
	 * columns of two writers' files the part of its own about 8 columns'
	 * (VPQ_FETCH_OWN_COLUMNS), 1 to 16 threads 1.9, 2.6 and 9.2 times as fast,
	 * as (8 + n) / (8 + ceil(n / t)) has them (§3.36, "Universal?")
	 */
	if (est->size >= offsetof(VexecSourceEstimate, fetch) + sizeof(est->fetch))
	{
		double		read = units > 0 ? bytes / units : 0;

		if (vpq_enable_point_reads)
		{
			VpqTable   *t = vpq_table(rel);
			int			page = !t->external && t->opts.page_bytes > 0 ? t->opts.page_bytes : VPQ_PAGE_BYTES;

			read = Min(read, (double) Max(spec->ncolumns, 1) * page);
		}
		est->fetch = read / BLCKSZ * random_page_cost / 8.0;
		if (threads > 1)
		{
			double		n = Max(spec->ncolumns, 1);

			est->fetch *= (VPQ_FETCH_OWN_COLUMNS + ceil(n / threads)) / (VPQ_FETCH_OWN_COLUMNS + n);
		}
	}
	/* W55: those threads the source's own (minor 14) */
	if (est->size >= offsetof(VexecSourceEstimate, fetch_threads) + sizeof(est->fetch_threads))
		est->fetch_threads = threads > 1 ? threads : 0;
}

/*
 * fetch_columns() (minor 4): the columns a fetch by TID into the slot reads
 * (scan.c), the slot's others left NULL -- vexec's late columns
 */
static void
src_fetch_columns(Relation rel, TupleTableSlot *slot, int n, const AttrNumber *attnums)
{
	vpq_fetch_set_columns(rel, slot, n, attnums);
}

/*
 * aggregate(): the requests answered from the statistics of the next unit,
 * passed over as read; false leaves the unit to next()
 */
static bool
answer_from_stats(SrcState *st, VpqFileEntry *f, int rg, const VexecSourceAgg *req,
				  VexecSourceAggAnswer *answer)
{
	VpqTable   *t = st->t;
	int64		rows = f->unit_rows[rg];
	VpqColumn  *col;
	const VpqStat *s;

	if (req->kind == VEXEC_SRC_AGG_ROWS)
	{
		answer->value = Int64GetDatum(rows);
		answer->isnull = false;
		return true;
	}
	if (req->attnum < 1 || req->attnum > t->natts)
		return false;
	col = &t->cols[req->attnum - 1];
	if (col->kind == VPQ_K_DROPPED)
		return false;
	s = vpq_unit_stat(t, f, rg, req->attnum);
	if (s == NULL || s->nulls < 0)
		return false;
	if (req->kind == VEXEC_SRC_AGG_COUNT)
	{
		answer->value = Int64GetDatum(rows - s->nulls);
		answer->isnull = false;
		return true;
	}
	if (req->kind != VEXEC_SRC_AGG_MIN && req->kind != VEXEC_SRC_AGG_MAX)
		return false;
	if (req->type != col->typid)
		return false;
	if (s->nulls >= rows)
	{
		answer->value = (Datum) 0;
		answer->isnull = true;
		return true;
	}
	if (!s->has_minmax || !s->exact || s->may_nan)
		return false;
	if (s->bytewise)
	{
		bytea	   *b = DatumGetByteaPP(req->kind == VEXEC_SRC_AGG_MIN ? s->min : s->max);

		/* bytewise is a text's order under "C" alone */
		if (col->kind == VPQ_K_TEXT)
		{
			if (OidIsValid(req->collation) && !pg_newlocale_from_collation(req->collation)->collate_is_c)
				return false;
			answer->value = PointerGetDatum(cstring_to_text_with_len(VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b)));
		}
		else if (col->kind == VPQ_K_BYTEA)
			answer->value = PointerGetDatum(b);
		else if (col->kind == VPQ_K_UUID && VARSIZE_ANY_EXHDR(b) == 16)
		{
			char	   *u = palloc(16);

			memcpy(u, VARDATA_ANY(b), 16);
			answer->value = PointerGetDatum(u);
		}
		else
			return false;
		answer->isnull = false;
		return true;
	}
	answer->value = datumCopy(req->kind == VEXEC_SRC_AGG_MIN ? s->min : s->max, col->typbyval, col->typlen);
	answer->isnull = false;
	return true;
}

static bool
src_aggregate(void *arg, int nreqs, const VexecSourceAgg *reqs, VexecSourceAggAnswer *answers, int64 *nrows)
{
	SrcState   *st = arg;
	MemoryContext old;
	int			unit;
	VpqUnitRef *ref;
	VpqFileEntry *f;
	bool		ok = true;
	int64		lo;
	int64		hi;

	if (st->unit >= 0 && st->pos < st->rows)
		return false;			/* inside a unit: its rows come first */
	if (st->unit >= 0)
		release_unit(st);
	if (!claim_unit(st, &unit, &lo, &hi))
		return false;
	ref = &st->listing->units[unit];
	f = &st->listing->files[ref->file];
	if (hi >= 0)
	{
		/* a range of a row group's rows (PQ12), which its statistics do not tell of: read by next() */
		begin_unit_rows(st, unit, lo, hi);
		return false;
	}
	if (st->listing->any_dead)
	{
		int			n;
		uint64		first = f->first_rowid + (uint64) f->unit_first[ref->rg];

		(void) vpq_scan_dead_rows(st->scan, f, first, first + (uint64) f->unit_rows[ref->rg] - 1, &n);
		ok = n == 0;
	}
	MemoryContextReset(st->answer_cxt);
	old = MemoryContextSwitchTo(st->answer_cxt);
	for (int i = 0; i < nreqs && ok; i++)
		ok = answer_from_stats(st, f, ref->rg, &reqs[i], &answers[i]);
	MemoryContextSwitchTo(old);
	if (!ok)
	{
		/* the claimed unit is read by next() */
		begin_unit(st, unit);
		return false;
	}
	*nrows = f->unit_rows[ref->rg];
	if (pgstat_should_count_relation(st->rel))
		st->rel->pgstat_info->counts.tuples_returned += *nrows;
	return true;
}

/*
 * Device units (the contract's minor 7, 9): a unit of a local file whose
 * every column vexec's GPU executor decodes as read.c does -- the file's
 * own type of the column's, a date as days, a timestamp in its unit, a
 * decimal at its scale -- handed for the executor to read itself, the rows
 * the snapshot does not see as its deleted bits; any other unit the CPU's,
 * read by the next batches alone
 */
typedef struct DevToken
{
	int			unit;
} DevToken;

static bool
device_mode(VpqReadCol *rc)
{
	VpqColumn  *col = rc->col;

	switch (rc->mode)
	{
		case VPQ_RM_FIXED:
		case VPQ_RM_BOOL:
		case VPQ_RM_DATE32:
		case VPQ_RM_TIMESTAMP:
			return true;
		case VPQ_RM_INT:
			{
				/*
				 * PQ6 (pg_vector_executor.md §3.35): an integer the executor
				 * widens -- each value of its type one of the column's, so
				 * that none is the CPU reader's range error: a signed one no
				 * wider, an unsigned one narrower, or the oid's unsigned 32
				 * bits
				 */
				bool		is_unsigned;
				int			w = vpq_int_width(rc->ftype.id, &is_unsigned);
				int			cw = col->kind == VPQ_K_INT2 ? 2 : col->kind == VPQ_K_INT4 || col->kind == VPQ_K_OID ? 4 : 8;

				if (w == 0)
					return false;
				if (col->kind == VPQ_K_OID)
					return is_unsigned && w <= 4;
				return is_unsigned ? w < cw : w <= cw;
			}
		case VPQ_RM_INT_DAYS:
			{
				/* PQ6: days since 1970 the executor makes dates, of 32 bits signed or fewer; device_values() their range */
				bool		is_unsigned;
				int			w = vpq_int_width(rc->ftype.id, &is_unsigned);

				return w > 0 && (w < 4 || (w == 4 && !is_unsigned));
			}
		case VPQ_RM_INT_SECONDS:
			{
				/* PQ6: seconds since 1970 the executor makes timestamps; device_values() their range */
				bool		is_unsigned;
				int			w = vpq_int_width(rc->ftype.id, &is_unsigned);

				return w > 0 && !(w == 8 && is_unsigned);
			}
		case VPQ_RM_DECIMAL:
			return col->kind == VPQ_K_NUMERIC && rc->ftype.precision <= col->precision &&
				rc->ftype.scale == col->scale;
		case VPQ_RM_STRING:
		case VPQ_RM_LARGE_STRING:
		case VPQ_RM_VIEW:
			return col->kind == VPQ_K_TEXT || col->kind == VPQ_K_BYTEA ||
				(col->kind == VPQ_K_JSON && rc->own);
		default:
			return false;
	}
}

/* A date's days from 1970 the executor moves as fill_column() does: finite, and from elsewhere PostgreSQL's */
static bool
device_date(int64 v, bool own)
{
	int64		d = v - VPQ_EPOCH_DAYS;

	if (v <= PG_INT32_MIN || v >= PG_INT32_MAX)
		return false;			/* an infinity, which the executor would move */
	return own || (d >= PG_INT32_MIN && d <= PG_INT32_MAX && IS_VALID_DATE((DateADT) d));
}

/* A timestamp of a unit from 1970 the executor moves as read.c does: finite, and from elsewhere PostgreSQL's */
static bool
device_timestamp(int64 v, int unit, bool own)
{
	int64		us;

	switch (unit)
	{
		case VPQ_UNIT_S:
			if (pg_mul_s64_overflow(v, INT64CONST(1000000), &us))
				return false;
			break;
		case VPQ_UNIT_MS:
			if (pg_mul_s64_overflow(v, INT64CONST(1000), &us))
				return false;
			break;
		case VPQ_UNIT_US:
			us = v;
			break;
		default:
			return false;		/* nanoseconds, which the executor refuses */
	}
	if (TIMESTAMP_NOT_FINITE(us))
		return false;			/* an infinity, which the executor would move */
	return own || (us >= PG_INT64_MIN + VPQ_EPOCH_USECS && IS_VALID_TIMESTAMP(us - VPQ_EPOCH_USECS));
}

/*
 * Whether the executor reads column i's chunk of row group rg and makes its
 * values as read.c does, past their type.  libcudf 25.06 never finished
 * reading a chunk of pyarrow's dictionary-encoded data pages of version 2
 * under snappy -- the device held at its whole until the executor was
 * killed -- and failed to decompress LZ4's: a chunk under either codec is
 * the device's only where its first data page's header says version 1.  The executor checks
 * text from elsewhere as UTF-8 and no more -- it makes it neither the
 * database's encoding nor a varchar's length -- and moves a date's days and
 * a timestamp's units from 1970, keeping no infinity and checking no range.
 * So a row group's dates and timestamps go to it where its statistics show
 * them finite -- and from elsewhere within PostgreSQL's range -- as they
 * show the bounds its pruning trusts; else the unit is the CPU's.
 */
static bool
device_values(SrcState *st, int i, int file, int rg, int64 rows)
{
	VpqReadCol *rc = &st->rcols[i];
	VpqColumn  *col = rc->col;
	int64_t		nulls;
	bool		has_minmax;
	vpq_value	lo;
	vpq_value	hi;
	bool		lo_exact;
	bool		hi_exact;
	int			codec;
	int			version;
	int64_t		facts_at;

	if (!vpq_file_column_pages_at(st->handle, rg, rc->fcol, &codec, &version, &facts_at))
		return false;
	/* PQ7 (§3.35): a version read now kept in the file's facts in shared memory, for the next process */
	if (facts_at >= 0 && version != 0 && !rc->own)
	{
		VpqFileEntry *f = &st->listing->files[file];

		vpq_footers_set_byte(f->path, f->size, f->mtime, (Size) facts_at, (char) version);
	}
	if (codec != VPQ_CODEC_OTHER && version != 1)
		return false;
	if (!rc->own && text_kind(col->kind) &&
		(GetDatabaseEncoding() != PG_UTF8 || (col->basetype == VARCHAROID && col->typmod >= 0)))
		return false;
	if (rc->mode == VPQ_RM_INT_DAYS || rc->mode == VPQ_RM_INT_SECONDS)
	{
		/* PQ6: the file's integers of days or seconds within PostgreSQL's range, as their statistics show */
		if (!vpq_file_column_stats(st->handle, rg, rc->fcol, &nulls, &has_minmax, &lo, &hi, &lo_exact, &hi_exact))
			return false;
		if (!has_minmax)
			return nulls >= rows;	/* every value NULL */
		if (rc->mode == VPQ_RM_INT_DAYS)
			return device_date(lo.i, false) && device_date(hi.i, false);
		return device_timestamp(lo.i, VPQ_UNIT_S, false) && device_timestamp(hi.i, VPQ_UNIT_S, false);
	}
	if (rc->mode != VPQ_RM_DATE32 && rc->mode != VPQ_RM_TIMESTAMP)
		return true;
	if (rc->own)
	{
		/*
		 * An own file's from the catalog's statistics, as its footer's say
		 * them: the least and greatest in PostgreSQL's units, an infinity as
		 * PostgreSQL holds it either way -- so that its facts need none (PQ1)
		 */
		const VpqStat *s = vpq_unit_stat(st->t, &st->listing->files[file], rg, col->attnum);

		if (s == NULL || s->nulls < 0)
			return false;
		if (!s->has_minmax)
			return s->nulls >= rows;	/* every value NULL */
		if (rc->mode == VPQ_RM_DATE32)
			return !DATE_NOT_FINITE(DatumGetDateADT(s->min)) && !DATE_NOT_FINITE(DatumGetDateADT(s->max));
		return !TIMESTAMP_NOT_FINITE(DatumGetTimestamp(s->min)) && !TIMESTAMP_NOT_FINITE(DatumGetTimestamp(s->max));
	}
	if (!vpq_file_column_stats(st->handle, rg, rc->fcol, &nulls, &has_minmax, &lo, &hi, &lo_exact, &hi_exact))
		return false;
	if (!has_minmax)
		return nulls >= rows;	/* every value NULL */
	if (rc->mode == VPQ_RM_DATE32)
		return device_date(lo.i, rc->own) && device_date(hi.i, rc->own);
	return device_timestamp(lo.i, rc->ftype.unit, rc->own) && device_timestamp(hi.i, rc->ftype.unit, rc->own);
}

static bool
src_next_unit(void *arg, VexecSourceUnit *u)
{
	SrcState   *st = arg;
	int			unit;
	VpqUnitRef *ref;
	VpqFileEntry *f;
	bool		device;
	MemoryContext old;
	int64		lo;
	int64		hi;

	if (st->unit >= 0)
		release_unit(st);
	if (!claim_unit(st, &unit, &lo, &hi))
		return false;
	ref = &st->listing->units[unit];
	f = &st->listing->files[ref->file];
	MemoryContextReset(st->dev_cxt);
	if (hi >= 0)
	{
		/* a range of a row group's rows (PQ12, cut where no device reads): the CPU's */
		begin_unit_rows(st, unit, lo, hi);
		st->one_unit = true;
		u->format = VEXEC_UNIT_CPU;
		return true;
	}
	device = vpq_enable_device_units && f->path[0] == '/';
	if (device)
	{
		open_file(st, ref->file);
		for (int i = 0; i < st->ncols && device; i++)
		{
			const char *name;
			int			field_id;
			vpq_type	type;
			bool		nullable;

			if (st->rcols[i].fcol < 0 || !device_mode(&st->rcols[i]) ||
				!device_values(st, i, ref->file, ref->rg, f->unit_rows[ref->rg]))
			{
				device = false;
				break;
			}
			/* PQ6: days or seconds since 1970, which a vexec of minor version 13 tells the executor of */
			if ((st->rcols[i].mode == VPQ_RM_INT_DAYS || st->rcols[i].mode == VPQ_RM_INT_SECONDS) &&
				u->size < offsetof(VexecSourceUnit, bytes) + sizeof(u->bytes))
			{
				device = false;
				break;
			}
			(void) vpq_file_column(st->handle, st->rcols[i].fcol, &name, &field_id, &type, &nullable);
			if (strchr(name, '.') != NULL)
				device = false;
		}
	}
	if (!device)
	{
		/* the CPU's: the next batches hand its rows, then end once */
		begin_unit(st, unit);
		st->one_unit = true;
		u->format = VEXEC_UNIT_CPU;
		return true;
	}

	old = MemoryContextSwitchTo(st->dev_cxt);
	{
		const char **names = palloc0(sizeof(char *) * Max(st->ncols, 1));
		uint8	   *flags = palloc0(Max(st->ncols, 1));
		int32	   *parts = palloc(sizeof(int32));
		DevToken   *token = MemoryContextAlloc(st->cxt, sizeof(DevToken));
		int64		rows = f->unit_rows[ref->rg];
		uint64		first = f->first_rowid + (uint64) f->unit_first[ref->rg];
		int			ndead = 0;
		uint64	   *dead = vpq_scan_dead_rows(st->scan, f, first, first + (uint64) rows - 1, &ndead);
		int64		mtime = f->mtime;
		int64		size = f->size;

		/* an own file's identity, as the executor checks it: its size and time from the filesystem, once a file */
		if (mtime < 0)
		{
			if (st->stat_file != ref->file)
			{
				vpq_entry	e;

				vpq_engine_ready();
				VPQ_CHECK(vpq_stat(f->path, &e), psprintf("finding \"%s\"", f->path));
				st->stat_file = ref->file;
				st->stat_size = e.size;
				st->stat_mtime = e.mtime_ns;
			}
			mtime = st->stat_mtime;
			size = st->stat_size;
		}
		for (int i = 0; i < st->ncols; i++)
		{
			const char *name;
			int			field_id;
			vpq_type	type;
			bool		nullable;
			VpqColumn  *col = st->rcols[i].col;

			(void) vpq_file_column(st->handle, st->rcols[i].fcol, &name, &field_id, &type, &nullable);
			names[i] = pstrdup(name);
			if (col->notnull)
				flags[i] |= VEXEC_UNIT_NOT_NULL;
			if (!st->rcols[i].own && text_kind(col->kind) && GetDatabaseEncoding() == PG_UTF8)
				flags[i] |= VEXEC_UNIT_CHECK_UTF8;
			/* PQ6: the file's integer of days or of seconds since 1970 */
			if (st->rcols[i].mode == VPQ_RM_INT_DAYS)
				flags[i] |= VEXEC_UNIT_DAYS;
			if (st->rcols[i].mode == VPQ_RM_INT_SECONDS)
				flags[i] |= VEXEC_UNIT_SECONDS;
		}
		/*
		 * W38 (pg_vector_executor.md §3.35): the unit's columns' bytes before
		 * their compression, as the file says -- what vexec sizes the
		 * device's requests by, with the columns' widths
		 */
		if (u->size >= offsetof(VexecSourceUnit, bytes) + sizeof(u->bytes))
		{
			uint64		bytes = 0;

			for (int i = 0; i < st->ncols; i++)
				bytes += (uint64) Max(vpq_file_column_raw_bytes(st->handle, ref->rg, st->rcols[i].fcol), 0);
			u->bytes = bytes;
		}
		parts[0] = ref->rg;
		token->unit = unit;
		u->format = VEXEC_UNIT_PARQUET;
		u->path = pstrdup(f->path);
		u->file_size = (uint64) size;
		u->file_mtime = mtime;
		u->nparts = 1;
		u->parts = parts;
		u->rows = (uint64) rows;
		u->names = names;
		u->column_flags = flags;
		u->deleted = NULL;
		if (ndead > 0)
		{
			uint64	   *bits = palloc0(sizeof(uint64) * ((rows + 63) / 64 + 1));

			for (int d = 0; d < ndead; d++)
			{
				int64		k = (int64) (dead[d] - first);

				bits[k >> 6] |= UINT64CONST(1) << (k & 63);
			}
			u->deleted = bits;
		}
		u->token = token;
		if (u->size >= offsetof(VexecSourceUnit, nstreams) + sizeof(u->nstreams))
		{
			u->nstreams = 0;
			u->streams = NULL;
		}
		if (u->size >= offsetof(VexecSourceUnit, rowid_name) + sizeof(u->rowid_name))
			u->rowid_name = NULL;
		if (pgstat_should_count_relation(st->rel))
			st->rel->pgstat_info->counts.tuples_returned += rows - ndead;
	}
	MemoryContextSwitchTo(old);
	st->units_device++;
	return true;
}

static void
src_read_unit(void *arg, const VexecSourceUnit *u)
{
	SrcState   *st = arg;
	const DevToken *token = u->token;

	begin_unit(st, token->unit);
	st->one_unit = true;
}

static void
src_unit_tids(void *arg, const VexecSourceUnit *u, int n, const uint64 *rows, ItemPointerData *tids)
{
	SrcState   *st = arg;
	const DevToken *token = u->token;
	VpqUnitRef *ref = &st->listing->units[token->unit];
	VpqFileEntry *f = &st->listing->files[ref->file];
	uint64		first = f->first_rowid + (uint64) f->unit_first[ref->rg];

	for (int k = 0; k < n; k++)
		vpq_set_tid(&tids[k], first + rows[k]);
}

/*
 * The sink
 */
typedef struct SinkState
{
	Relation	rel;
	CommandId	cid;
	bool		tids;
} SinkState;

static bool
sink_supports(Relation rel, AttrNumber attnum, VexecSinkLayout *layout)
{
	VpqTable   *t = vpq_table(rel);

	if (t->external || attnum < 1 || attnum > t->natts)
		return false;
	return vpq_sink_layout(t, attnum, layout);
}

static void *
sink_begin(Relation rel, const VexecSinkSpec *spec)
{
	SinkState  *s = palloc0(sizeof(SinkState));

	s->rel = rel;
	s->cid = spec->cid;
	s->tids = spec->size >= offsetof(VexecSinkSpec, flags) + sizeof(spec->flags) &&
		(spec->flags & VEXEC_SINK_TIDS) != 0;
	return s;
}

static void
sink_put(void *state, VexecSinkBatch *batch)
{
	SinkState  *s = state;

	vpq_buffer_put_batch(s->rel, s->cid, batch->nrows, batch->columns, s->tids ? batch->tids : NULL);
}

static void
sink_end(void *state)
{
	pfree(state);
}

/*
 * Minor version 16 (pg_vector_executor.md §3.41, OT5): the scan's units read
 * here, those it handed the device to read itself, and those its quals'
 * statistics passed over, for pg_otel_probe's span of its VecScan.  The
 * state's fields alone.
 */
static int
src_scan_figures(void *arg, VexecSourceFigure *figures)
{
	SrcState   *st = arg;

	figures[0].key = "vexec_parquet.units_read";
	figures[0].value = st->units_read;
	figures[1].key = "vexec_parquet.units_device";
	figures[1].value = st->units_device;
	figures[2].key = "vexec_parquet.units_pruned";
	figures[2].value = st->pruned;
	return 3;
}

/*
 * Minor version 16: what the process's planning reads, since it started --
 * files' footers read, files opened of their facts without one, own tables'
 * listings read from the catalog, columns' statistics decoded -- of which
 * vexec keeps what grew while a statement was planned.
 */
static int
src_planning_figures(VexecSourceFigure *figures)
{
	int64_t		footers_read;
	int64_t		facts_opened;

	vpq_engine_footer_counts(&footers_read, &facts_opened);
	figures[0].key = "vexec_parquet.planning.footers_read";
	figures[0].value = footers_read;
	figures[1].key = "vexec_parquet.planning.facts_opened";
	figures[1].value = facts_opened;
	figures[2].key = "vexec_parquet.planning.listings";
	figures[2].value = vpq_listing_stats.listings;
	figures[3].key = "vexec_parquet.planning.stat_columns";
	figures[3].value = vpq_listing_stats.stat_columns;
	return 4;
}

static VexecSourceRoutine source = {
	.size = sizeof(VexecSourceRoutine),
	.minor = VEXEC_SOURCE_MINOR,
	.name = "vexec_parquet",
	.supports = src_supports,
	.begin = src_begin,
	.next = src_next,
	.retain = src_retain,
	.release = src_release,
	.rescan = src_rescan,
	.end = src_end,
	.estimate = src_estimate,
	.aggregate = src_aggregate,
	.set_keys = src_set_keys,
	.dictionary_id = NULL,
	.estimate_ex = src_estimate_ex,
	.fetch_columns = src_fetch_columns,
	.ordering = NULL,
	.next_rest = src_next_rest,
	.set_join_keys = src_set_join_keys,
	.next_unit = src_next_unit,
	.read_unit = src_read_unit,
	.read_unit_bytes = NULL,
	.unit_tids = src_unit_tids,
	.row_tids = NULL,
	.claim_alone = NULL,
	.scan_figures = src_scan_figures,
	.planning_figures = src_planning_figures,
};

static VexecSinkRoutine sink = {
	.size = sizeof(VexecSinkRoutine),
	.minor = VEXEC_SINK_MINOR,
	.name = "vexec_parquet",
	.supports = sink_supports,
	.begin = sink_begin,
	.put = sink_put,
	.end = sink_end,
};

void
vpq_register_vexec(void)
{
	source.am = vpq_routine();
	sink.am = vpq_routine();
	vexec_register_source(&source);
	vexec_register_sink(&sink);
}
