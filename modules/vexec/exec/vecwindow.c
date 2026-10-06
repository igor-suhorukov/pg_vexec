/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vecwindow.c
 *	  VecWindowHashAgg: ORCA's hashed window, its partitions hashed rather
 *	  than sorted, under PostgreSQL's WindowAgg (pg_vector_executor.md
 *	  §3.3.4, §3.8, V5).
 *
 * ORCA's hashed window (CPhysicalHashSequenceProject, offered under
 * create_vectorization_plan) asks its input for no order: it brings each
 * of its partitions' rows together itself.  No node of PostgreSQL's does
 * that, so gp_orca's translator lowers it to a WindowAgg over a Sort of its
 * input by the partition's columns and then the window's order
 * (TranslateDXLWindowHashAgg()), and offers that pair to vexec
 * (gp_orca_vec.h, build_window), which puts this node in the Sort's place
 * (plan/window.c).  The WindowAgg then reads each partition's rows
 * together, each partition in the window's order -- all a WindowAgg needs
 * of its input -- and computes the window functions as it computes them
 * over any input, PostgreSQL's own: the partitions come in no order, which
 * ORCA, taking the hashed window's input to have none, does not ask for.
 *
 *	the input	the child's rows, read to their end the first time a row is
 *				asked for: a vector child's batches, its rows still to be
 *				decided resolved in row order through the child
 *				(vexec_resolve_row()); a row child's rows.  Each row's
 *				partition is found by its keys in a hash table
 *				(lib/simplehash.h), hashed and compared as the window's
 *				equality operators compare -- integers and the like by
 *				their bits, text of a deterministic collation and bytea by
 *				their bytes, any other type through its hash and equality
 *				functions, as VecAgg compares its keys (vecagg.c) -- NULLs
 *				together, as a WindowAgg partitions them; and the row is
 *				kept there, a MinimalTuple, in the order it came;
 *	the output	the partitions, in the table's order, each partition's rows
 *				sorted by the window's order with SortSupport where it has
 *				one, else in the order they came: a row a call.
 *
 * The table and its rows are held to get_hash_memory_limit(), measured with
 * MemoryContextMemAllocated().  Past it, every row kept so far and every
 * row still to come goes to the one of 32 files on disk that five bits of
 * its hash choose, and the files are read back one at a time, each a table
 * of its own.  A file that does not fit either goes to 32 more by the next
 * five bits, three levels deep; past the last, its rows are sorted by the
 * lowering's keys -- the partition's columns, then the window's order --
 * through tuplesort, which spills, as the lowering would have sorted them.
 * A partition's rows have one hash, so no partition is split between files.
 *
 * Its scan tuple, which custom_scan_tlist describes, is its child's row,
 * and its target list reads it as INDEX_VAR, as VecSort's does: the
 * WindowAgg above reads the columns it read of the Sort.
 *
 * Rescans: the partitions are given out again, sorted as they were, where
 * nothing spilled and the child has no changed parameter; otherwise the
 * child is read again.  It runs neither backward nor marks.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/htup_details.h"
#include "catalog/pg_collation.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "port/pg_bitutils.h"
#include "storage/buffile.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ruleutils.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"
#include "utils/typcache.h"
#include "varatt.h"

#include "cb_explain.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"

/* How a partition key is hashed and compared, as VecAgg's keys are. */
typedef enum WinKeyKind
{
	WKEY_BITS,					/* a by-value type equal where its bits are */
	WKEY_BYTES,					/* text of a deterministic collation, bytea */
	WKEY_FMGR					/* the type's hash and equality functions */
} WinKeyKind;

typedef struct WinKey
{
	int			col;			/* the child's column, from 0 */
	WinKeyKind	kind;
	int16		typlen;
	bool		typbyval;
	Oid			collation;
	FmgrInfo	hash;
	FmgrInfo	eq;
} WinKey;

/* A row's key, as it is looked up. */
typedef struct WinProbe
{
	Datum		value;
	const char *bytes;			/* WKEY_BYTES */
	int32		len;
	bool		isnull;
} WinProbe;

/* A partition: its keys, as its first row had them, and its rows. */
typedef struct WinPart
{
	uint32		hash;
	Datum	   *keys;
	bool	   *nulls;
	MinimalTuple *rows;			/* in the order they came; once given out,
								 * in the window's order */
	int64		nrows;
	int64		maxrows;
	bool		sorted;			/* rows is in the window's order */
} WinPart;

typedef struct WinEntry
{
	WinPart    *part;
	uint32		hash;
	char		status;
} WinEntry;

/* A file of spilled rows, and the level they were spilled at. */
typedef struct WinFile
{
	BufFile    *file;
	int64		rows;
	int			depth;
} WinFile;

#define WIN_FILE_BITS	5
#define WIN_FILES		(1 << WIN_FILE_BITS)
#define WIN_MAX_DEPTH	3

typedef struct VexecWindowState VexecWindowState;

static bool part_equal(VexecWindowState *s, const WinPart *part);

#define SH_PREFIX vwin
#define SH_ELEMENT_TYPE WinEntry
#define SH_KEY_TYPE WinPart *
#define SH_KEY part
#define SH_HASH_KEY(tb, key) ((key)->hash)
#define SH_EQUAL(tb, a, b) part_equal((VexecWindowState *) (tb)->private_data, (a))
#define SH_SCOPE static inline
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) ((a)->hash)
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

struct VexecWindowState
{
	VexecNode	node;			/* first */
	VexecWindowPlan plan;
	PlanState  *child;
	TupleDesc	desc;			/* the child's rows */
	bool		child_batches;	/* the child hands batches up */
	TupleTableSlot *put_slot;	/* a vector child's row */
	TupleTableSlot *read_slot;	/* a row read back, or a partition's row */
	TupleTableSlot *scan_slot;	/* the node's scan tuple, given out */

	/* the partition's keys, and a row's, as it is looked up */
	int			nkeys;
	WinKey	   *keys;
	WinProbe   *probe;
	WinPart		probe_part;		/* simplehash's key: the hash alone */

	/* the window's order, within a partition */
	int			nord;
	AttrNumber	ord_maxcol;
	SortSupport ord;

	/* the table */
	MemoryContext tablecxt;		/* the table, the partitions, their arrays */
	MemoryContext rowcxt;		/* the rows */
	MemoryContext tmpcxt;		/* a row's detoasted keys; a sort's arrays */
	MemoryContext readcxt;		/* a row read back from a file */
	Size		memlimit;
	vwin_hash  *table;
	int64		ntable_rows;
	Size		bytes_unchecked;	/* rows' bytes kept since memory was
									 * last measured */

	/* the output */
	bool		filled;			/* the input, or a file, read into the table */
	WinPart   **parts;			/* the table's partitions, in its order */
	int64		nparts;
	int64		cur_part;
	int64		cur_row;
	bool		done;

	/* past memory */
	int			depth;			/* the level the table's rows came from */
	bool		spilling;		/* rows go to the level's files */
	WinFile    *files;			/* the level's */
	List	   *pending;		/* WinFile, to be read back */
	WinFile    *reading;		/* the one read back now, or NULL */
	Tuplesortstate *lastsort;	/* past the last level: the lowering's sort */
	bool		lastsort_done;
	bool		ever_spilled;

	/* EXPLAIN ANALYZE */
	int64		rows_in;
	int64		partitions;
	int64		spilled_rows;
	int64		spill_files;
	int			max_depth;
	int64		lastsort_rows;
	Size		peak_memory;
};

static void window_begin(CustomScanState *css, EState *estate, int eflags);
static TupleTableSlot *window_exec(CustomScanState *css);
static void window_end(CustomScanState *css);
static void window_rescan(CustomScanState *css);
static void window_explain(CustomScanState *css, List *ancestors, ExplainState *es);

static const CustomExecMethods window_exec_methods = {
	.CustomName = VEXEC_WINDOW_NAME,
	.BeginCustomScan = window_begin,
	.ExecCustomScan = window_exec,
	.EndCustomScan = window_end,
	.ReScanCustomScan = window_rescan,
	.ExplainCustomScan = window_explain,
};

Node *
vexec_create_window_state(CustomScan *cscan)
{
	VexecWindowState *s = palloc0(sizeof(VexecWindowState));

	NodeSetTag(s, T_CustomScanState);
	s->node.css.methods = &window_exec_methods;
	/* the scan tuple holds the rows as they are kept */
	s->node.css.slotOps = &TTSOpsMinimalTuple;
	s->node.kind = VEXEC_NODE_WINDOW;
	return (Node *) s;
}

/* ---------------------------------------------------------------------
 * The plan: custom_private
 * ---------------------------------------------------------------------
 */

/*
 * The partition's keys -- the child's columns from 1, the window's
 * equality operators, collations -- the window's order within a partition
 * -- columns, ordering operators, collations, NULLS FIRST -- and the
 * lowering's Sort's keys, the same four, for the rows past the last level.
 */
List *
vexec_window_plan_encode(const VexecWindowPlan *plan)
{
	List	   *ordnulls = NIL;
	List	   *sortnulls = NIL;
	ListCell   *lc;

	foreach(lc, plan->ordnullsfirst)
		ordnulls = lappend_int(ordnulls, lfirst_int(lc) ? 1 : 0);
	foreach(lc, plan->sortnullsfirst)
		sortnulls = lappend_int(sortnulls, lfirst_int(lc) ? 1 : 0);
	return list_make3(list_make3(list_copy(plan->partcols), list_copy(plan->parteqops),
								 list_copy(plan->partcollations)),
					  list_make4(list_copy(plan->ordcols), list_copy(plan->ordops),
								 list_copy(plan->ordcollations), ordnulls),
					  list_make4(list_copy(plan->sortcols), list_copy(plan->sortops),
								 list_copy(plan->sortcollations), sortnulls));
}

void
vexec_window_plan_decode(CustomScan *cscan, VexecWindowPlan *plan)
{
	List	   *p = cscan->custom_private;
	List	   *part;
	List	   *ord;
	List	   *sort;

	if (list_length(p) != 3)
		elog(ERROR, "vexec: a VecWindowHashAgg's plan of %d parts", list_length(p));
	part = linitial(p);
	ord = lsecond(p);
	sort = lthird(p);
	if (list_length(part) != 3 || list_length(ord) != 4 || list_length(sort) != 4)
		elog(ERROR, "vexec: a VecWindowHashAgg's plan of the wrong shape");
	plan->partcols = linitial(part);
	plan->parteqops = lsecond(part);
	plan->partcollations = lthird(part);
	plan->ordcols = linitial(ord);
	plan->ordops = lsecond(ord);
	plan->ordcollations = lthird(ord);
	plan->ordnullsfirst = lfourth(ord);
	plan->sortcols = linitial(sort);
	plan->sortops = lsecond(sort);
	plan->sortcollations = lthird(sort);
	plan->sortnullsfirst = lfourth(sort);
	if (list_length(plan->partcols) == 0 ||
		list_length(plan->parteqops) != list_length(plan->partcols) ||
		list_length(plan->partcollations) != list_length(plan->partcols) ||
		list_length(plan->ordops) != list_length(plan->ordcols) ||
		list_length(plan->ordcollations) != list_length(plan->ordcols) ||
		list_length(plan->ordnullsfirst) != list_length(plan->ordcols) ||
		list_length(plan->sortcols) == 0 ||
		list_length(plan->sortops) != list_length(plan->sortcols) ||
		list_length(plan->sortcollations) != list_length(plan->sortcols) ||
		list_length(plan->sortnullsfirst) != list_length(plan->sortcols))
		elog(ERROR, "vexec: a VecWindowHashAgg of %d partition keys and %d operators",
			 list_length(plan->partcols), list_length(plan->parteqops));
}

/* ---------------------------------------------------------------------
 * The keys, as VecAgg hashes and compares its own (vecagg.c)
 * ---------------------------------------------------------------------
 */

/* How a partition key's equality operator lets its values be hashed and compared. */
static WinKeyKind
key_kind(Oid eqop, Oid collation, Form_pg_attribute att)
{
	switch (get_opcode(eqop))
	{
		case F_INT2EQ:
		case F_INT4EQ:
		case F_INT8EQ:
		case F_OIDEQ:
		case F_BOOLEQ:
		case F_CHAREQ:
		case F_DATE_EQ:
		case F_TIME_EQ:
		case F_TIMESTAMP_EQ:
		case F_TIMESTAMPTZ_EQ:
		case F_CASH_EQ:
			if (att->attbyval)
				return WKEY_BITS;
			break;
		case F_BYTEAEQ:
			return WKEY_BYTES;
		case F_TEXTEQ:
			if (!OidIsValid(collation) || get_collation_isdeterministic(collation))
				return WKEY_BYTES;
			break;
		default:
			break;
	}
	return WKEY_FMGR;
}

/*
 * A by-value key's bits: its type's bytes only, as a Datum built from a
 * tuple and one built by a function may extend a narrower value's sign
 * otherwise.
 */
static inline Datum
key_bits(Datum d, int16 typlen)
{
	switch (typlen)
	{
		case 1:
			return d & 0xFF;
		case 2:
			return d & 0xFFFF;
		case 4:
			return d & UINT64CONST(0xFFFFFFFF);
		default:
			return d;
	}
}

/* The probe's keys, from a row, and their hash, combined as nodeAgg.c does. */
static uint32
probe_row(VexecWindowState *s, TupleTableSlot *slot)
{
	uint32		h = 0;
	int			i;

	for (i = 0; i < s->nkeys; i++)
	{
		WinKey	   *k = &s->keys[i];
		WinProbe   *p = &s->probe[i];
		Datum		d = slot->tts_values[k->col];

		h = pg_rotate_left32(h, 1);
		p->isnull = slot->tts_isnull[k->col];
		if (p->isnull)
			continue;
		switch (k->kind)
		{
			case WKEY_BITS:
				p->value = key_bits(d, k->typlen);
				h ^= (uint32) murmurhash64((uint64) p->value);
				break;
			case WKEY_BYTES:
				{
					varlena    *vl = (varlena *) DatumGetPointer(d);

					if (VARATT_IS_EXTENDED(vl))
					{
						MemoryContext old = MemoryContextSwitchTo(s->tmpcxt);

						vl = pg_detoast_datum_packed(vl);
						MemoryContextSwitchTo(old);
					}
					p->bytes = VARDATA_ANY(vl);
					p->len = VARSIZE_ANY_EXHDR(vl);
					h ^= hash_bytes((const unsigned char *) p->bytes, p->len);
				}
				break;
			case WKEY_FMGR:
				p->value = d;
				h ^= DatumGetUInt32(FunctionCall1Coll(&k->hash, k->collation, d));
				break;
		}
	}
	return murmurhash32(h);
}

/* Whether a partition's keys are the probe's: NULLs together. */
static bool
part_equal(VexecWindowState *s, const WinPart *part)
{
	int			i;

	for (i = 0; i < s->nkeys; i++)
	{
		WinKey	   *k = &s->keys[i];
		WinProbe   *p = &s->probe[i];

		if (part->nulls[i] != p->isnull)
			return false;
		if (p->isnull)
			continue;
		switch (k->kind)
		{
			case WKEY_BITS:
				if (key_bits(part->keys[i], k->typlen) != p->value)
					return false;
				break;
			case WKEY_BYTES:
				{
					varlena    *vl = (varlena *) DatumGetPointer(part->keys[i]);

					if (VARSIZE(vl) - VARHDRSZ != (Size) p->len ||
						memcmp(VARDATA(vl), p->bytes, p->len) != 0)
						return false;
				}
				break;
			case WKEY_FMGR:
				if (!DatumGetBool(FunctionCall2Coll(&k->eq, k->collation,
													part->keys[i], p->value)))
					return false;
				break;
		}
	}
	return true;
}

/* A new partition, the probe's keys copied into it. */
static WinPart *
part_create(VexecWindowState *s, uint32 hash)
{
	MemoryContext old = MemoryContextSwitchTo(s->tablecxt);
	WinPart    *part = palloc0(sizeof(WinPart));
	int			i;

	part->hash = hash;
	part->keys = palloc0(sizeof(Datum) * s->nkeys);
	part->nulls = palloc0(sizeof(bool) * s->nkeys);
	for (i = 0; i < s->nkeys; i++)
	{
		WinKey	   *k = &s->keys[i];
		WinProbe   *p = &s->probe[i];

		part->nulls[i] = p->isnull;
		if (p->isnull)
			continue;
		if (k->kind == WKEY_BYTES)
		{
			varlena    *vl = palloc(VARHDRSZ + p->len);

			SET_VARSIZE(vl, VARHDRSZ + p->len);
			memcpy(VARDATA(vl), p->bytes, p->len);
			part->keys[i] = PointerGetDatum(vl);
		}
		else if (k->kind == WKEY_BITS)
			part->keys[i] = p->value;
		else
			part->keys[i] = datumCopy(p->value, k->typbyval, k->typlen);
	}
	part->maxrows = 8;
	part->rows = palloc(sizeof(MinimalTuple) * part->maxrows);
	MemoryContextSwitchTo(old);
	s->partitions++;
	return part;
}

/* ---------------------------------------------------------------------
 * The table, and past memory
 * ---------------------------------------------------------------------
 */

static Size
table_memory(VexecWindowState *s)
{
	Size		m = MemoryContextMemAllocated(s->tablecxt, true) +
		MemoryContextMemAllocated(s->rowcxt, true);

	s->peak_memory = Max(s->peak_memory, m);
	return m;
}

/* An empty table, for the input and for each file read back. */
static void
table_reset(VexecWindowState *s)
{
	MemoryContextReset(s->tablecxt);
	MemoryContextReset(s->rowcxt);
	s->table = vwin_create(s->tablecxt, 1024, s);
	s->ntable_rows = 0;
	s->bytes_unchecked = 0;
	s->parts = NULL;
	s->nparts = 0;
	s->cur_part = 0;
	s->cur_row = 0;
	s->spilling = false;
}

/* A row into the file the hash's bits at the next level choose. */
static void
spill_tuple(VexecWindowState *s, uint32 hash, MinimalTuple tup)
{
	int			shift = 32 - WIN_FILE_BITS * (s->depth + 1);
	WinFile    *f = &s->files[(hash >> shift) & (WIN_FILES - 1)];

	if (f->file == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(s->node.mcxt);

		f->file = BufFileCreateTemp(false);
		MemoryContextSwitchTo(old);
		s->spill_files++;
	}
	BufFileWrite(f->file, tup, tup->t_len);
	f->rows++;
	s->spilled_rows++;
}

/* Past memory at this level: every row of the table to the files. */
static void
spill_begin(VexecWindowState *s)
{
	vwin_iterator it;
	WinEntry   *e;
	int			i;

	s->files = MemoryContextAllocZero(s->node.mcxt, sizeof(WinFile) * WIN_FILES);
	for (i = 0; i < WIN_FILES; i++)
		s->files[i].depth = s->depth + 1;
	s->max_depth = Max(s->max_depth, s->depth + 1);
	vwin_start_iterate(s->table, &it);
	while ((e = vwin_iterate(s->table, &it)) != NULL)
	{
		int64		r;

		for (r = 0; r < e->part->nrows; r++)
			spill_tuple(s, e->part->hash, e->part->rows[r]);
	}
	table_reset(s);
	s->spilling = true;
	s->ever_spilled = true;
}

/* The level's files, queued to be read back. */
static void
spill_finish(VexecWindowState *s)
{
	MemoryContext old;
	int			i;

	if (s->files == NULL)
		return;
	old = MemoryContextSwitchTo(s->node.mcxt);
	for (i = 0; i < WIN_FILES; i++)
	{
		WinFile    *f = &s->files[i];
		WinFile    *queued;

		if (f->file == NULL)
			continue;
		if (BufFileSeek(f->file, 0, 0, SEEK_SET) != 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not rewind a VecWindowHashAgg spill file")));
		queued = palloc_object(WinFile);
		*queued = *f;
		s->pending = lappend(s->pending, queued);
	}
	pfree(s->files);
	s->files = NULL;
	MemoryContextSwitchTo(old);
}

/*
 * Past the last level: the lowering's sort of the file's rows, by the
 * partition's columns and then the window's order, which tuplesort spills.
 */
static void
lastsort_begin(VexecWindowState *s)
{
	int			n = list_length(s->plan.sortcols);
	AttrNumber *cols = palloc(sizeof(AttrNumber) * n);
	Oid		   *ops = palloc(sizeof(Oid) * n);
	Oid		   *colls = palloc(sizeof(Oid) * n);
	bool	   *nulls = palloc(sizeof(bool) * n);
	vwin_iterator it;
	WinEntry   *e;
	int			i;

	for (i = 0; i < n; i++)
	{
		cols[i] = (AttrNumber) list_nth_int(s->plan.sortcols, i);
		ops[i] = list_nth_oid(s->plan.sortops, i);
		colls[i] = list_nth_oid(s->plan.sortcollations, i);
		nulls[i] = list_nth_int(s->plan.sortnullsfirst, i) != 0;
	}
	s->lastsort = tuplesort_begin_heap(s->desc, n, cols, ops, colls, nulls,
									   work_mem, NULL, TUPLESORT_NONE);
	s->lastsort_done = false;
	vwin_start_iterate(s->table, &it);
	while ((e = vwin_iterate(s->table, &it)) != NULL)
	{
		int64		r;

		for (r = 0; r < e->part->nrows; r++)
		{
			ExecStoreMinimalTuple(e->part->rows[r], s->read_slot, false);
			tuplesort_puttupleslot(s->lastsort, s->read_slot);
			s->lastsort_rows++;
		}
	}
	ExecClearTuple(s->read_slot);
	table_reset(s);
}

/* A row of the input into its partition, a file, or the last sort. */
static void
add_row(VexecWindowState *s, TupleTableSlot *slot)
{
	MemoryContext old;
	WinEntry   *e;
	MinimalTuple tup;
	uint32		hash;
	bool		found;

	s->rows_in++;
	if (s->lastsort != NULL)
	{
		tuplesort_puttupleslot(s->lastsort, slot);
		s->lastsort_rows++;
		return;
	}
	slot_getallattrs(slot);
	MemoryContextReset(s->tmpcxt);
	hash = probe_row(s, slot);
	if (s->spilling)
	{
		old = MemoryContextSwitchTo(s->tmpcxt);
		tup = heap_form_minimal_tuple(s->desc, slot->tts_values, slot->tts_isnull, 0);
		MemoryContextSwitchTo(old);
		spill_tuple(s, hash, tup);
		return;
	}

	s->probe_part.hash = hash;
	e = vwin_insert_hash(s->table, &s->probe_part, hash, &found);
	if (!found)
		e->part = part_create(s, hash);
	if (e->part->nrows == e->part->maxrows)
	{
		old = MemoryContextSwitchTo(s->tablecxt);
		e->part->maxrows *= 2;
		e->part->rows = repalloc_huge(e->part->rows, sizeof(MinimalTuple) * e->part->maxrows);
		MemoryContextSwitchTo(old);
	}
	old = MemoryContextSwitchTo(s->rowcxt);
	tup = heap_form_minimal_tuple(s->desc, slot->tts_values, slot->tts_isnull, 0);
	MemoryContextSwitchTo(old);
	e->part->rows[e->part->nrows++] = tup;
	s->ntable_rows++;
	s->bytes_unchecked += tup->t_len;

	/*
	 * Past the memory the table may hold: to the files, or the last sort.
	 * Measured every 256 rows, and after every 64kB of rows kept, so that
	 * wide rows do not run far past it.
	 */
	if ((s->ntable_rows & 255) != 0 && s->bytes_unchecked < 65536)
		return;
	s->bytes_unchecked = 0;
	if (table_memory(s) > s->memlimit)
	{
		if (s->depth < WIN_MAX_DEPTH)
			spill_begin(s);
		else
			lastsort_begin(s);
	}
}

/* A vector child's batch, its rows in order. */
static void
add_batch(VexecWindowState *s, VexecNode *child, VexecBatch *b)
{
	int			row;

	for (row = 0; row < b->nrows; row++)
	{
		TupleTableSlot *slot;

		if ((child->redo != NULL && vexec_bit(child->redo, row)) ||
			(child->child_redo != NULL && vexec_bit(child->child_redo, row)))
		{
			/* a row the child's kernels left: its own evaluator decides it */
			slot = vexec_resolve_row(child, row);
			if (slot == NULL)
				continue;
		}
		else if (b->selection == NULL || vexec_bit(b->selection, row))
		{
			vexec_batch_store_row(b, row, s->put_slot);
			slot = s->put_slot;
		}
		else
			continue;
		add_row(s, slot);
	}
}

/*
 * The table filled: from the child, the first time, or from the file read
 * back.  Then its partitions, in the table's order, to be given out.
 */
static void
fill(VexecWindowState *s)
{
	EState	   *estate = s->node.css.ss.ps.state;
	ScanDirection dir = estate->es_direction;

	estate->es_direction = ForwardScanDirection;
	if (s->reading != NULL)
	{
		BufFile    *f = s->reading->file;

		for (;;)
		{
			uint32		len;
			MinimalTuple tup;

			CHECK_FOR_INTERRUPTS();
			MemoryContextReset(s->readcxt);
			if (BufFileReadMaybeEOF(f, &len, sizeof(uint32), true) == 0)
				break;
			tup = (MinimalTuple) MemoryContextAlloc(s->readcxt, len);
			tup->t_len = len;
			BufFileReadExact(f, (char *) tup + sizeof(uint32), len - sizeof(uint32));
			ExecStoreMinimalTuple(tup, s->read_slot, false);
			add_row(s, s->read_slot);
			ExecClearTuple(s->read_slot);
		}
		MemoryContextReset(s->readcxt);
		BufFileClose(f);
		s->reading = NULL;
	}
	else if (s->child_batches)
	{
		VexecNode  *child = (VexecNode *) s->child;
		VexecBatch *b;

		while ((b = vexec_next_batch(s->child)) != NULL)
			add_batch(s, child, b);
	}
	else
	{
		for (;;)
		{
			TupleTableSlot *slot = ExecProcNode(s->child);

			if (TupIsNull(slot))
				break;
			add_row(s, slot);
		}
	}
	estate->es_direction = dir;
	s->filled = true;

	if (s->lastsort != NULL)
	{
		tuplesort_performsort(s->lastsort);
		return;
	}
	if (s->spilling)
	{
		/* every row went to the files: the table is empty */
		spill_finish(s);
		return;
	}

	/* the partitions, in the table's order */
	{
		vwin_iterator it;
		WinEntry   *e;
		int64		n = 0;

		s->parts = MemoryContextAlloc(s->tablecxt,
									  sizeof(WinPart *) * Max(s->table->members, 1));
		vwin_start_iterate(s->table, &it);
		while ((e = vwin_iterate(s->table, &it)) != NULL)
			s->parts[n++] = e->part;
		s->nparts = n;
	}
	table_memory(s);
}

/* ---------------------------------------------------------------------
 * A partition in the window's order
 * ---------------------------------------------------------------------
 */

typedef struct WinSortRow
{
	MinimalTuple tup;
	Datum	   *values;
	bool	   *nulls;
} WinSortRow;

static int
sort_row_compare(const void *a, const void *b, void *arg)
{
	VexecWindowState *s = (VexecWindowState *) arg;
	const WinSortRow *x = (const WinSortRow *) a;
	const WinSortRow *y = (const WinSortRow *) b;
	int			i;

	for (i = 0; i < s->nord; i++)
	{
		int			c = ApplySortComparator(x->values[i], x->nulls[i],
											y->values[i], y->nulls[i], &s->ord[i]);

		if (c != 0)
			return c;
	}
	return 0;
}

/*
 * A partition's rows sorted by the window's order, in place.  Its keys are
 * read once a row; a by-reference value points into the row, which lives
 * as long as the table.
 */
static void
sort_part(VexecWindowState *s, WinPart *part)
{
	MemoryContext old = MemoryContextSwitchTo(s->tmpcxt);
	WinSortRow *rows = palloc_array(WinSortRow, part->nrows);
	Datum	   *values = palloc_array(Datum, (Size) part->nrows * s->nord);
	bool	   *nulls = palloc_array(bool, (Size) part->nrows * s->nord);
	int64		r;
	int			i;

	for (r = 0; r < part->nrows; r++)
	{
		rows[r].tup = part->rows[r];
		rows[r].values = &values[r * s->nord];
		rows[r].nulls = &nulls[r * s->nord];
		ExecStoreMinimalTuple(part->rows[r], s->read_slot, false);
		slot_getsomeattrs(s->read_slot, s->ord_maxcol);
		for (i = 0; i < s->nord; i++)
		{
			AttrNumber	col = s->ord[i].ssup_attno;

			rows[r].values[i] = s->read_slot->tts_values[col - 1];
			rows[r].nulls[i] = s->read_slot->tts_isnull[col - 1];
		}
		ExecClearTuple(s->read_slot);
	}
	qsort_arg(rows, part->nrows, sizeof(WinSortRow), sort_row_compare, s);
	for (r = 0; r < part->nrows; r++)
		part->rows[r] = rows[r].tup;
	part->sorted = true;
	MemoryContextSwitchTo(old);
	MemoryContextReset(s->tmpcxt);
}

/* ---------------------------------------------------------------------
 * Exec
 * ---------------------------------------------------------------------
 */

/* The next row given out, in the node's scan tuple; NULL at the end. */
static TupleTableSlot *
next_row(VexecWindowState *s)
{
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (s->done)
			return NULL;
		if (!s->filled)
			fill(s);

		if (s->lastsort != NULL)
		{
			if (!s->lastsort_done &&
				tuplesort_gettupleslot(s->lastsort, true, false, s->scan_slot, NULL))
				return s->scan_slot;
			s->lastsort_done = true;
			tuplesort_end(s->lastsort);
			s->lastsort = NULL;
		}
		else if (s->cur_part < s->nparts)
		{
			WinPart    *part = s->parts[s->cur_part];

			if (s->cur_row == 0 && !part->sorted && s->nord > 0 && part->nrows > 1)
				sort_part(s, part);
			if (s->cur_row < part->nrows)
				return ExecStoreMinimalTuple(part->rows[s->cur_row++], s->scan_slot, false);
			s->cur_part++;
			s->cur_row = 0;
			continue;
		}

		/* the table is out: the next file, if any */
		if (s->pending == NIL)
		{
			s->done = true;
			continue;
		}
		s->reading = linitial(s->pending);
		s->pending = list_delete_first(s->pending);
		s->depth = s->reading->depth;
		table_reset(s);
		s->filled = false;
	}
}

static TupleTableSlot *
window_exec(CustomScanState *css)
{
	VexecWindowState *s = (VexecWindowState *) css;
	ExprContext *econtext = css->ss.ps.ps_ExprContext;
	TupleTableSlot *slot;

	s->node.ran = true;
	slot = next_row(s);
	if (slot == NULL)
		return ExecClearTuple(css->ss.ps.ps_ResultTupleSlot);
	if (css->ss.ps.ps_ProjInfo == NULL)
		return slot;
	ResetExprContext(econtext);
	econtext->ecxt_scantuple = slot;
	return ExecProject(css->ss.ps.ps_ProjInfo);
}

/* ---------------------------------------------------------------------
 * Begin, rescan, end
 * ---------------------------------------------------------------------
 */

static void
window_begin(CustomScanState *css, EState *estate, int eflags)
{
	VexecWindowState *s = (VexecWindowState *) css;
	VexecNode  *node = &s->node;
	CustomScan *cscan = (CustomScan *) css->ss.ps.plan;
	TupleDesc	scandesc = css->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	int			i;

	vexec_node_begin(node, estate);
	node->label = "Vec Window Hash Agg";
	vexec_window_plan_decode(cscan, &s->plan);

	/* the child, read once to its end: it needs neither to rewind nor to mark */
	s->child = ExecInitNode(outerPlan(cscan), estate,
							eflags & ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK));
	outerPlanState(css) = s->child;
	s->desc = ExecGetResultType(s->child);
	if (scandesc->natts != s->desc->natts)
		elog(ERROR, "vexec: a VecWindowHashAgg's scan tuple of %d columns, its child's %d",
			 scandesc->natts, s->desc->natts);
	s->child_batches = vexec_is_vector_state(s->child) &&
		vexec_node_batchable((VexecNode *) s->child);
	s->put_slot = ExecInitExtraTupleSlot(estate, s->desc, &TTSOpsVirtual);
	s->read_slot = ExecInitExtraTupleSlot(estate, s->desc, &TTSOpsMinimalTuple);
	s->scan_slot = css->ss.ss_ScanTupleSlot;

	/* the partition's keys */
	s->nkeys = list_length(s->plan.partcols);
	s->keys = palloc0_array(WinKey, s->nkeys);
	s->probe = palloc0_array(WinProbe, s->nkeys);
	for (i = 0; i < s->nkeys; i++)
	{
		WinKey	   *k = &s->keys[i];
		int			col = list_nth_int(s->plan.partcols, i);
		Oid			eqop = list_nth_oid(s->plan.parteqops, i);
		Form_pg_attribute att;
		Oid			lhash;
		Oid			rhash;

		if (col < 1 || col > s->desc->natts)
			elog(ERROR, "vexec: a VecWindowHashAgg's partition key is not a column of its child");
		att = TupleDescAttr(s->desc, col - 1);
		k->col = col - 1;
		k->typlen = att->attlen;
		k->typbyval = att->attbyval;
		k->collation = list_nth_oid(s->plan.partcollations, i);
		k->kind = key_kind(eqop, k->collation, att);
		if (!get_op_hash_functions(eqop, &lhash, &rhash) || !OidIsValid(lhash))
			elog(ERROR, "vexec: a VecWindowHashAgg's partition key of an operator with no hash function");
		fmgr_info(lhash, &k->hash);
		fmgr_info(get_opcode(eqop), &k->eq);
	}

	/* the window's order within a partition */
	s->nord = list_length(s->plan.ordcols);
	s->ord = palloc0_array(SortSupportData, Max(s->nord, 1));
	for (i = 0; i < s->nord; i++)
	{
		SortSupport ssup = &s->ord[i];

		ssup->ssup_cxt = CurrentMemoryContext;
		ssup->ssup_collation = list_nth_oid(s->plan.ordcollations, i);
		ssup->ssup_nulls_first = list_nth_int(s->plan.ordnullsfirst, i) != 0;
		ssup->ssup_attno = (AttrNumber) list_nth_int(s->plan.ordcols, i);
		ssup->abbreviate = false;
		if (ssup->ssup_attno < 1 || ssup->ssup_attno > s->desc->natts)
			elog(ERROR, "vexec: a VecWindowHashAgg's order key is not a column of its child");
		PrepareSortSupportFromOrderingOp(list_nth_oid(s->plan.ordops, i), ssup);
		s->ord_maxcol = Max(s->ord_maxcol, ssup->ssup_attno);
	}

	s->memlimit = get_hash_memory_limit();
	s->tablecxt = AllocSetContextCreate(node->mcxt, "VecWindowHashAgg partitions",
										ALLOCSET_DEFAULT_SIZES);
	s->rowcxt = BumpContextCreate(node->mcxt, "VecWindowHashAgg rows",
								  ALLOCSET_DEFAULT_SIZES);
	s->tmpcxt = AllocSetContextCreate(node->mcxt, "VecWindowHashAgg keys",
									  ALLOCSET_DEFAULT_SIZES);
	s->readcxt = AllocSetContextCreate(node->mcxt, "VecWindowHashAgg rows read back",
									   ALLOCSET_DEFAULT_SIZES);
	table_reset(s);
}

/* Every file closed, and the last sort ended. */
static void
release_all(VexecWindowState *s)
{
	ListCell   *lc;

	foreach(lc, s->pending)
	{
		WinFile    *f = lfirst(lc);

		if (f->file != NULL)
			BufFileClose(f->file);
	}
	s->pending = NIL;
	if (s->files != NULL)
	{
		for (int i = 0; i < WIN_FILES; i++)
			if (s->files[i].file != NULL)
				BufFileClose(s->files[i].file);
		pfree(s->files);
		s->files = NULL;
	}
	if (s->reading != NULL && s->reading->file != NULL)
		BufFileClose(s->reading->file);
	s->reading = NULL;
	if (s->lastsort != NULL)
		tuplesort_end(s->lastsort);
	s->lastsort = NULL;
}

/*
 * As ExecReScanSort() keeps a sort: the partitions are given out again where
 * they are all in the table, and the child has no changed parameter;
 * otherwise the child is read again.
 */
static void
window_rescan(CustomScanState *css)
{
	VexecWindowState *s = (VexecWindowState *) css;

	vexec_node_rescan(&s->node);
	ExecClearTuple(s->scan_slot);
	if (s->filled && !s->ever_spilled && s->child->chgParam == NULL)
	{
		s->cur_part = 0;
		s->cur_row = 0;
		s->done = false;
		return;
	}
	release_all(s);
	s->depth = 0;
	s->ever_spilled = false;
	s->done = false;
	s->filled = false;
	table_reset(s);
	if (s->child->chgParam == NULL)
		ExecReScan(s->child);
}

static void
window_end(CustomScanState *css)
{
	VexecWindowState *s = (VexecWindowState *) css;

	release_all(s);
	vexec_node_end(&s->node);
	ExecEndNode(s->child);
}

/* ---------------------------------------------------------------------
 * EXPLAIN
 * ---------------------------------------------------------------------
 */

/*
 * A key's options, as EXPLAIN gives a Sort's (explain.c,
 * show_sortorder_options(), static there): COLLATE where it is not the
 * type's, DESC or USING, and NULLS FIRST or LAST where it is not the
 * direction's default.
 */
static void
key_options(StringInfo buf, Oid keytype, Oid sortop, Oid collation, bool nullsfirst)
{
	TypeCacheEntry *typentry = lookup_type_cache(keytype, TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
	bool		reverse = false;

	if (OidIsValid(collation) && collation != get_typcollation(keytype))
	{
		char	   *collname = get_collation_name(collation);

		if (collname == NULL)
			elog(ERROR, "cache lookup failed for collation %u", collation);
		appendStringInfo(buf, " COLLATE %s", quote_identifier(collname));
	}
	if (sortop == typentry->gt_opr)
	{
		appendStringInfoString(buf, " DESC");
		reverse = true;
	}
	else if (sortop != typentry->lt_opr)
	{
		char	   *opname = get_opname(sortop);

		if (opname == NULL)
			elog(ERROR, "cache lookup failed for operator %u", sortop);
		appendStringInfo(buf, " USING %s", opname);
		(void) get_equality_op_for_ordering_op(sortop, &reverse);
	}
	if (nullsfirst && !reverse)
		appendStringInfoString(buf, " NULLS FIRST");
	else if (!nullsfirst && reverse)
		appendStringInfoString(buf, " NULLS LAST");
}

/* The child's column col, as EXPLAIN deparses it. */
static char *
column_name(VexecWindowState *s, List *context, ExplainState *es, int col)
{
	Form_pg_attribute att = TupleDescAttr(s->desc, col - 1);
	Var		   *v = makeVar(OUTER_VAR, col, att->atttypid, att->atttypmod,
							att->attcollation, 0);

	return deparse_expression((Node *) v, context, es->verbose, false);
}

static void
window_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	VexecWindowState *s = (VexecWindowState *) css;
	List	   *context = set_deparse_context_plan(es->deparse_cxt, css->ss.ps.plan, ancestors);
	List	   *keys = NIL;
	int			i;

	CbExplainRelabel(css, es, "Vec Window Hash Agg", NULL);
	for (i = 0; i < list_length(s->plan.partcols); i++)
		keys = lappend(keys, column_name(s, context, es, list_nth_int(s->plan.partcols, i)));
	ExplainPropertyList("Partition Key", keys, es);
	if (s->nord > 0)
	{
		keys = NIL;
		for (i = 0; i < s->nord; i++)
		{
			int			col = list_nth_int(s->plan.ordcols, i);
			StringInfoData buf;

			initStringInfo(&buf);
			appendStringInfoString(&buf, column_name(s, context, es, col));
			key_options(&buf, TupleDescAttr(s->desc, col - 1)->atttypid,
						list_nth_oid(s->plan.ordops, i),
						list_nth_oid(s->plan.ordcollations, i),
						list_nth_int(s->plan.ordnullsfirst, i) != 0);
			keys = lappend(keys, buf.data);
		}
		ExplainPropertyList("Order Key within Partitions", keys, es);
	}
	if (es->verbose)
		ExplainPropertyText("Input", s->child_batches ? "batches" : "rows", es);

	/* where it ran: on a cluster, not on the coordinator for a segment's */
	if (es->analyze && s->node.ran)
	{
		ExplainPropertyInteger("Partitions", NULL, s->partitions, es);
		ExplainPropertyInteger("Peak Memory Usage", "kB",
							   (int64) ((s->peak_memory + 1023) / 1024), es);
		if (s->ever_spilled || es->format != EXPLAIN_FORMAT_TEXT)
		{
			ExplainPropertyInteger("Spill Files", NULL, s->spill_files, es);
			ExplainPropertyInteger("Rows Spilled", NULL, s->spilled_rows, es);
			ExplainPropertyInteger("Spill Depth", NULL, s->max_depth, es);
		}
		if (s->lastsort_rows > 0)
			ExplainPropertyInteger("Rows Sorted Past the Last Level", NULL,
								   s->lastsort_rows, es);
		if (es->verbose)
			ExplainPropertyInteger("Rows In", NULL, s->rows_in, es);
	}
}
