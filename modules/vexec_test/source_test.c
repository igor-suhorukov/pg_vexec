/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * source_test.c
 *	  vexec_test_am, a table access method of the tests, and its batch
 *	  source and sink with every member of their contracts (vexec_source.h
 *	  at minor 3, vexec_sink.h), so that vexec's suite tests the contracts
 *	  on vanilla PostgreSQL, where no storage module registers either
 *	  (ch_table_engines.md §3, VC9).
 *
 * The access method is heap's: its routine is a copy of heap's, so its
 * tables store, scan and vacuum as heap's do, and only the routine's
 * address tells them apart -- which is what vexec finds a source by.  Its
 * TOAST tables are heap's own; an index of its is not built, heap's build
 * reading with heap_getnext(), which refuses another routine.
 *
 * The source reads the scan vexec began, heap's, through
 * table_scan_getnextslot(), a unit of UNIT_ROWS rows at a time read ahead
 * into memory, and keeps statistics of each unit as it reads it: of each
 * column, the values that are not NULL, and for int2, int4 and int8 their
 * least, greatest and sum.  From them:
 *
 *	next()			a unit's rows cut into batches of the spec's max_rows: a
 *					fixed-length type FIXED at its width, bool BYTE_BOOL, any
 *					other type DATUM -- or, a varlena one, OFFSETS, int32
 *					offsets into its values' bytes, as ch_table_engines'
 *					MergeTree hands its strings, under
 *					vexec_test.source_text_offsets (VC14); the TIDs heap
 *					gave; each batch's arrays its own, written over by the
 *					next unless retained.  Under
 *					vexec_test.source_dictionaries, where vexec asks for
 *					dictionaries, a varlena column is DICT: a dictionary of
 *					the unit's distinct values (their detoasted bytes, in
 *					their bytes' order), as DATUM or OFFSETS -- the last
 *					unit's, where it has the same values (VC5)
 *	dictionary_id()	a dictionary's, the same for every batch of every unit
 *					that has it
 *	aggregate()		count(*), count(x), and min(x), max(x) and sum(x) of the
 *					integers, from the next unit's statistics, which is then
 *					passed over as read
 *	set_keys()		and the spec's keys: a unit none of whose rows is within
 *					them, by its least and greatest values, is passed over
 *	estimate()		the relation's tuples, and the needed attributes' widths
 *					over them, a fifth where the quals are not empty
 *	estimate_ex()	estimate()'s, the units a serial scan reads, and
 *					VEXEC_EST_AGG_EXACT where aggregate() answers each: no qual,
 *					no TIDs, every column an integer
 *	supports()		every attribute but one of type xml, which keeps a scan
 *					reading it on the slot path
 *
 * A unit a retained batch points into stays until the last such batch is
 * released.  The sink takes every attribute but an xml one -- FIXED for a
 * fixed-length type, BYTE_BOOL for bool, DATUM for the rest -- and writes the
 * rows with heap's table_multi_insert(), giving their TIDs.
 *
 * vexec_test.source_calls() counts each member's calls since the backend
 * began, for the suite to check which vexec makes.  Registered from _PG_init
 * while the postmaster preloads vexec_test, after vexec.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/heapam.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"
#include "varatt.h"

#include "vexec_sink.h"
#include "vexec_source.h"

#define UNIT_ROWS 1000

PG_FUNCTION_INFO_V1(vexec_test_am_handler);
PG_FUNCTION_INFO_V1(vexec_test_source_calls);

static TableAmRoutine test_am;
static bool test_am_made = false;

/* Each member's calls, in this backend */
typedef enum Member
{
	M_SUPPORTS, M_BEGIN, M_NEXT, M_RETAIN, M_RELEASE, M_RESCAN, M_END, M_ESTIMATE, M_AGGREGATE,
	M_AGGREGATE_ANSWERED, M_SET_KEYS, M_UNITS_PASSED, M_SINK_SUPPORTS, M_SINK_BEGIN, M_SINK_PUT,
	M_SINK_END, M_DICTIONARY_ID, M_DICTIONARIES, M_ESTIMATE_EX, NMEMBERS
} Member;

static const char *const member_names[NMEMBERS] = {
	"supports", "begin", "next", "retain", "release", "rescan", "end", "estimate", "aggregate",
	"aggregate answered", "set_keys", "units passed by keys", "sink supports", "sink begin",
	"sink put", "sink end", "dictionary_id", "dictionaries made", "estimate_ex"
};

static int64 calls[NMEMBERS];

/* vexec_test.source_text_offsets: varlena columns handed as OFFSETS */
static bool text_offsets = false;

/* vexec_test.source_dictionaries: varlena columns handed as DICT where asked */
static bool dictionaries = false;

/* The last dictionary's identity, in this backend */
static uint64 last_dictionary = 0;

/*
 * A TOAST table of the access method's is heap's own: heap's callback gives
 * the parent's (heapam_handler.c), and the TOAST index's build reads it with
 * heap_getnext(), which refuses a routine but heap's.
 */
static Oid
test_toast_am(Relation rel)
{
	return HEAP_TABLE_AM_OID;
}

static const TableAmRoutine *
routine(void)
{
	if (!test_am_made)
	{
		test_am = *GetHeapamTableAmRoutine();
		test_am.relation_toast_am = test_toast_am;
		test_am_made = true;
	}
	return &test_am;
}

Datum
vexec_test_am_handler(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(routine());
}

/* ---- the source ---- */

/* A column's statistics in a unit */
typedef struct ColStats
{
	int64		count;			/* values that are not NULL */
	bool		ints;			/* int2, int4 or int8: min, max and sum kept */
	int64		min;
	int64		max;
	int128		sum;
} ColStats;

/*
 * A dictionary of a varlena column: a unit's distinct values in their bytes'
 * order, in the source's memory, for as many units as have those values
 */
typedef struct UnitDict
{
	VexecColumn values;			/* DATUM or OFFSETS, no NULL */
	int			nvalues;
	char	  **bytes;			/* each value's, and its length */
	int		   *lens;
	uint64		id;
} UnitDict;

/* A unit read ahead: its rows' values of the spec's columns, and their TIDs */
typedef struct Unit
{
	MemoryContext cxt;
	int			nrows;
	Datum	   *values;			/* [row * ncols + col] */
	bool	   *nulls;
	ItemPointerData *tids;
	ColStats   *stats;
	UnitDict  **dicts;			/* per column, where it is handed as DICT */
	int32	  **codes;			/* and the rows' codes into it; a NULL row's 0 */
	int			refs;			/* retained batches pointing into it */
	bool		current;		/* the source's own, still being handed */
} Unit;

/* One batch's arrays */
typedef struct Out
{
	MemoryContext cxt;
	VexecColumn *columns;
	ItemPointerData *tids;
	Unit	   *unit;			/* what its DATUM columns point into */
	bool		retained;
} Out;

typedef struct SrcState
{
	TableScanDesc scan;
	Relation	rel;
	TupleDesc	desc;
	MemoryContext cxt;
	TupleTableSlot *slot;
	int			ncols;
	AttrNumber *attnums;
	uint8	   *layouts;		/* what each is handed in: FIXED, BYTE_BOOL or DATUM */
	bool	   *dict;			/* per column: handed as DICT */
	UnitDict  **last_dict;		/* per column: the last unit's dictionary */
	bool		tids;
	bool		compact;
	int			max_rows;
	Unit	   *unit;			/* the unit being handed, or NULL */
	int			pos;
	bool		done;			/* the heap scan is at its end */
	Out		   *out;
	int			nkeys;			/* the spec's and set_keys()'s, ANDed */
	ScanKeyData *keys;
	int			nspec_keys;
	MemoryContext answers;
} SrcState;

static bool
src_supports(Relation rel, AttrNumber attnum)
{
	calls[M_SUPPORTS]++;
	if (attnum == SelfItemPointerAttributeNumber)
		return true;
	if (attnum < 1 || attnum > RelationGetNumberOfAttributes(rel))
		return false;
	return TupleDescAttr(RelationGetDescr(rel), attnum - 1)->atttypid != XMLOID;
}

static void *
src_begin(TableScanDesc scan, const VexecSourceSpec *spec)
{
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext, "vexec_test source", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	SrcState   *st = palloc0(sizeof(SrcState));

	calls[M_BEGIN]++;
	st->scan = scan;
	st->rel = scan->rs_rd;
	st->desc = RelationGetDescr(st->rel);
	st->cxt = cxt;
	st->answers = AllocSetContextCreate(cxt, "vexec_test answers", ALLOCSET_SMALL_SIZES);
	st->slot = table_slot_create(st->rel, NULL);
	st->max_rows = spec->max_rows > 0 ? spec->max_rows : UNIT_ROWS;
	st->tids = (spec->flags & VEXEC_SRC_TIDS) != 0;
	st->compact = (spec->flags & VEXEC_SRC_COMPACT) != 0;
	st->attnums = palloc(sizeof(AttrNumber) * Max(spec->ncolumns, 1));
	st->layouts = palloc(Max(spec->ncolumns, 1));
	st->dict = palloc0(sizeof(bool) * Max(spec->ncolumns, 1));
	st->last_dict = palloc0(sizeof(UnitDict *) * Max(spec->ncolumns, 1));
	for (int i = 0; i < spec->ncolumns; i++)
	{
		Form_pg_attribute att = TupleDescAttr(st->desc, spec->attnums[i] - 1);

		st->attnums[i] = spec->attnums[i];
		st->layouts[i] = att->atttypid == BOOLOID ? VEXEC_BYTE_BOOL :
			att->attlen > 0 ? VEXEC_FIXED :
			att->attlen == -1 && text_offsets ? VEXEC_OFFSETS : VEXEC_DATUM;
		st->dict[i] = dictionaries && (spec->flags & VEXEC_SRC_DICT) != 0 && att->attlen == -1;
	}
	st->ncols = spec->ncolumns;
	if (spec->nkeys > 0)
	{
		st->keys = palloc(sizeof(ScanKeyData) * spec->nkeys);
		memcpy(st->keys, spec->keys, sizeof(ScanKeyData) * spec->nkeys);
		st->nkeys = st->nspec_keys = spec->nkeys;
	}
	MemoryContextSwitchTo(old);
	return st;
}

static void
unit_free(Unit *u)
{
	MemoryContextDelete(u->cxt);
}

/* The source is done with its unit: freed, unless a retained batch points into it */
static void
unit_done(SrcState *st)
{
	if (st->unit == NULL)
		return;
	st->unit->current = false;
	if (st->unit->refs == 0)
		unit_free(st->unit);
	st->unit = NULL;
}

/* Whether a unit may hold a row within the keys: a key on an integer column, by its least and greatest */
static bool
within_keys(SrcState *st, Unit *u)
{
	for (int k = 0; k < st->nkeys; k++)
	{
		ScanKeyData *key = &st->keys[k];
		int			col = -1;
		ColStats   *cs;
		Datum		bound;
		Form_pg_attribute att;

		for (int i = 0; i < st->ncols; i++)
			if (st->attnums[i] == key->sk_attno)
				col = i;
		if (col < 0 || (key->sk_flags & SK_ISNULL))
			continue;
		cs = &u->stats[col];
		if (cs->count == 0)
			return false;		/* every value NULL, which no key keeps */
		if (!cs->ints)
			continue;
		att = TupleDescAttr(st->desc, key->sk_attno - 1);
		if (key->sk_strategy == BTLessStrategyNumber || key->sk_strategy == BTLessEqualStrategyNumber)
			bound = cs->min;
		else if (key->sk_strategy == BTGreaterStrategyNumber || key->sk_strategy == BTGreaterEqualStrategyNumber)
			bound = cs->max;
		else
			continue;
		bound = att->atttypid == INT2OID ? Int16GetDatum((int16) bound) :
			att->atttypid == INT4OID ? Int32GetDatum((int32) bound) : Int64GetDatum((int64) bound);
		if (!DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation, bound, key->sk_argument)))
			return false;
	}
	return true;
}

/* A new unit's arrays and statistics, empty */
static Unit *
unit_make(SrcState *st)
{
	Unit	   *u = MemoryContextAllocZero(st->cxt, sizeof(Unit));
	MemoryContext old;

	u->cxt = AllocSetContextCreate(st->cxt, "vexec_test unit", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(u->cxt);
	u->values = palloc(sizeof(Datum) * UNIT_ROWS * Max(st->ncols, 1));
	u->nulls = palloc(sizeof(bool) * UNIT_ROWS * Max(st->ncols, 1));
	u->tids = palloc(sizeof(ItemPointerData) * UNIT_ROWS);
	u->stats = palloc0(sizeof(ColStats) * Max(st->ncols, 1));
	MemoryContextSwitchTo(old);
	for (int i = 0; i < st->ncols; i++)
	{
		Oid			t = TupleDescAttr(st->desc, st->attnums[i] - 1)->atttypid;

		u->stats[i].ints = t == INT2OID || t == INT4OID || t == INT8OID;
		u->stats[i].min = PG_INT64_MAX;
		u->stats[i].max = PG_INT64_MIN;
	}
	return u;
}

/* The slot's row into the unit, its values copied, its statistics counted */
static void
unit_add(SrcState *st, Unit *u, TupleTableSlot *slot)
{
	int			r = u->nrows++;
	MemoryContext old = MemoryContextSwitchTo(u->cxt);

	slot_getallattrs(slot);
	for (int i = 0; i < st->ncols; i++)
	{
		Form_pg_attribute att = TupleDescAttr(st->desc, st->attnums[i] - 1);
		bool		isnull = slot->tts_isnull[st->attnums[i] - 1];
		Datum		v = slot->tts_values[st->attnums[i] - 1];
		ColStats   *cs = &u->stats[i];

		u->nulls[r * st->ncols + i] = isnull;
		u->values[r * st->ncols + i] = isnull ? (Datum) 0 : datumCopy(v, att->attbyval, att->attlen);
		if (isnull)
			continue;
		cs->count++;
		if (cs->ints)
		{
			int64		x = att->atttypid == INT2OID ? DatumGetInt16(v) :
				att->atttypid == INT4OID ? DatumGetInt32(v) : DatumGetInt64(v);

			cs->min = Min(cs->min, x);
			cs->max = Max(cs->max, x);
			cs->sum += x;
		}
	}
	u->tids[r] = slot->tts_tid;
	MemoryContextSwitchTo(old);
}

/* Two values' bytes in their order: memcmp's, then the shorter first */
static int
bytes_cmp(const char *a, int alen, const char *b, int blen)
{
	int			c = memcmp(a, b, Min(alen, blen));

	return c != 0 ? c : (alen > blen) - (alen < blen);
}

static int
plain_cmp(const void *x, const void *y, void *arg)
{
	varlena   **plain = arg;
	const varlena *a = plain[*(const int *) x];
	const varlena *b = plain[*(const int *) y];

	return bytes_cmp(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a), VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
}

/*
 * A unit's dictionary of column i and its rows' codes: its distinct values,
 * matched by their detoasted bytes, in their bytes' order -- the last
 * unit's dictionary where it holds the same values; NULL where every row is
 * NULL.
 */
static UnitDict *
unit_dict(SrcState *st, Unit *u, int i, int32 **codes_out)
{
	MemoryContext old = MemoryContextSwitchTo(u->cxt);
	UnitDict   *d;
	UnitDict   *last = st->last_dict[i];
	int			nslots = 2048;
	int		   *slots = palloc(sizeof(int) * nslots);
	int		   *first = palloc(sizeof(int) * Max(u->nrows, 1));
	int		   *rank = palloc(sizeof(int) * Max(u->nrows, 1));
	varlena   **plain = palloc0(sizeof(varlena *) * Max(u->nrows, 1));
	int32	   *codes = palloc0(sizeof(int32) * Max(u->nrows, 1));
	int			nvals = 0;
	bool		same;

	Assert(u->nrows <= UNIT_ROWS && nslots >= 2 * UNIT_ROWS);
	memset(slots, -1, sizeof(int) * nslots);
	for (int r = 0; r < u->nrows; r++)
	{
		uint32		h;
		int			k;

		if (u->nulls[r * st->ncols + i])
			continue;
		plain[r] = pg_detoast_datum_packed((varlena *) DatumGetPointer(u->values[r * st->ncols + i]));
		h = hash_bytes((const unsigned char *) VARDATA_ANY(plain[r]), VARSIZE_ANY_EXHDR(plain[r]));
		for (k = h & (nslots - 1);; k = (k + 1) & (nslots - 1))
		{
			int			f = slots[k];

			if (f < 0)
			{
				slots[k] = r;
				first[nvals] = r;
				codes[r] = nvals++;
				break;
			}
			if (bytes_cmp(VARDATA_ANY(plain[f]), VARSIZE_ANY_EXHDR(plain[f]),
						  VARDATA_ANY(plain[r]), VARSIZE_ANY_EXHDR(plain[r])) == 0)
			{
				codes[r] = codes[f];
				break;
			}
		}
	}
	*codes_out = codes;
	if (nvals == 0)
	{
		MemoryContextSwitchTo(old);
		return NULL;
	}

	/* the values in their bytes' order, the codes their places in it */
	{
		int		   *order = palloc(sizeof(int) * nvals);

		for (int v = 0; v < nvals; v++)
			order[v] = first[v];
		qsort_arg(order, nvals, sizeof(int), plain_cmp, plain);
		for (int v = 0; v < nvals; v++)
		{
			rank[codes[order[v]]] = v;
			first[v] = order[v];
		}
		for (int r = 0; r < u->nrows; r++)
			codes[r] = u->nulls[r * st->ncols + i] ? 0 : rank[codes[r]];
	}

	same = last != NULL && last->nvalues == nvals;
	for (int v = 0; v < nvals && same; v++)
		same = bytes_cmp(last->bytes[v], last->lens[v], VARDATA_ANY(plain[first[v]]),
						 VARSIZE_ANY_EXHDR(plain[first[v]])) == 0;
	if (same)
	{
		MemoryContextSwitchTo(old);
		return last;
	}

	/* a new dictionary, in the source's memory: later units may share it */
	MemoryContextSwitchTo(st->cxt);
	d = palloc0(sizeof(UnitDict));
	d->nvalues = nvals;
	d->bytes = palloc(sizeof(char *) * nvals);
	d->lens = palloc(sizeof(int) * nvals);
	for (int v = 0; v < nvals; v++)
	{
		d->lens[v] = VARSIZE_ANY_EXHDR(plain[first[v]]);
		d->bytes[v] = palloc(Max(d->lens[v], 1));
		memcpy(d->bytes[v], VARDATA_ANY(plain[first[v]]), d->lens[v]);
	}
	d->values.layout = st->layouts[i];
	d->values.encoding = VEXEC_FLAT;
	if (st->layouts[i] == VEXEC_OFFSETS)
	{
		int32	   *off = palloc(sizeof(int32) * (nvals + 1));
		const void **buffers = palloc(sizeof(void *));
		int64	   *sizes = palloc(sizeof(int64));
		StringInfoData all;

		initStringInfo(&all);
		off[0] = 0;
		for (int v = 0; v < nvals; v++)
		{
			appendBinaryStringInfo(&all, d->bytes[v], d->lens[v]);
			off[v + 1] = all.len;
		}
		buffers[0] = all.data;
		sizes[0] = all.len;
		d->values.values = off;
		d->values.buffers = buffers;
		d->values.buffer_sizes = sizes;
		d->values.nbuffers = 1;
	}
	else
	{
		Datum	   *vals = palloc(sizeof(Datum) * nvals);

		for (int v = 0; v < nvals; v++)
		{
			varlena    *vl = palloc(VARHDRSZ + d->lens[v]);

			SET_VARSIZE(vl, VARHDRSZ + d->lens[v]);
			memcpy(VARDATA(vl), d->bytes[v], d->lens[v]);
			vals[v] = PointerGetDatum(vl);
		}
		d->values.values = vals;
	}
	d->id = ++last_dictionary;
	calls[M_DICTIONARIES]++;
	st->last_dict[i] = d;
	MemoryContextSwitchTo(old);
	return d;
}

/* The next unit read ahead, with its statistics, past those the keys rule out; false at the scan's end */
static bool
load_unit(SrcState *st)
{
	Assert(st->unit == NULL);
	while (!st->done)
	{
		Unit	   *u = unit_make(st);

		while (u->nrows < UNIT_ROWS)
		{
			if (!table_scan_getnextslot(st->scan, ForwardScanDirection, st->slot))
			{
				st->done = true;
				break;
			}
			unit_add(st, u, st->slot);
		}
		ExecClearTuple(st->slot);
		if (u->nrows == 0)
		{
			unit_free(u);
			break;
		}
		if (st->nkeys > 0 && !within_keys(st, u))
		{
			calls[M_UNITS_PASSED]++;
			unit_free(u);
			continue;
		}
		u->current = true;
		u->dicts = MemoryContextAllocZero(u->cxt, sizeof(UnitDict *) * Max(st->ncols, 1));
		u->codes = MemoryContextAllocZero(u->cxt, sizeof(int32 *) * Max(st->ncols, 1));
		for (int i = 0; i < st->ncols; i++)
			if (st->dict[i])
				u->dicts[i] = unit_dict(st, u, i, &u->codes[i]);
		st->unit = u;
		st->pos = 0;
		return true;
	}
	return false;
}

static Out *
next_out(SrcState *st)
{
	if (st->out == NULL || st->out->retained)
	{
		st->out = MemoryContextAllocZero(st->cxt, sizeof(Out));
		st->out->cxt = AllocSetContextCreate(st->cxt, "vexec_test batch", ALLOCSET_SMALL_SIZES);
	}
	else
		MemoryContextReset(st->out->cxt);
	return st->out;
}

static bool
src_next(void *arg, VexecSourceBatch *batch)
{
	SrcState   *st = arg;
	Out		   *out;
	Unit	   *u;
	int			n;
	MemoryContext old;

	calls[M_NEXT]++;
	CHECK_FOR_INTERRUPTS();
	if (st->unit && st->pos >= st->unit->nrows)
		unit_done(st);
	if (st->unit == NULL && !load_unit(st))
		return false;
	u = st->unit;
	n = Min(st->max_rows, u->nrows - st->pos);
	out = next_out(st);
	out->unit = u;
	old = MemoryContextSwitchTo(out->cxt);
	out->columns = palloc0(sizeof(VexecColumn) * Max(st->ncols, 1));
	for (int i = 0; i < st->ncols; i++)
	{
		Form_pg_attribute att = TupleDescAttr(st->desc, st->attnums[i] - 1);
		VexecColumn *c = &out->columns[i];
		uint64	   *validity = NULL;

		c->layout = st->layouts[i];
		c->encoding = VEXEC_FLAT;
		for (int r = 0; r < n; r++)
			if (u->nulls[(st->pos + r) * st->ncols + i])
			{
				if (validity == NULL)
				{
					validity = palloc(sizeof(uint64) * ((n + 63) / 64));
					memset(validity, 0xFF, sizeof(uint64) * ((n + 63) / 64));
				}
				validity[r / 64] &= ~(UINT64CONST(1) << (r % 64));
			}
		c->validity = validity;
		if (u->dicts[i] != NULL)
		{
			/* the unit's dictionary, and the rows' codes into it */
			*c = u->dicts[i]->values;
			c->encoding = VEXEC_DICT;
			c->validity = validity;
			c->values = NULL;
			c->buffers = NULL;
			c->buffer_sizes = NULL;
			c->nbuffers = 0;
			c->codes = u->codes[i] + st->pos;
			c->dictionary = &u->dicts[i]->values;
			c->ndictionary = u->dicts[i]->nvalues;
		}
		else if (c->layout == VEXEC_DATUM)
		{
			Datum	   *d = palloc(sizeof(Datum) * Max(n, 1));

			for (int r = 0; r < n; r++)
				d[r] = u->values[(st->pos + r) * st->ncols + i];
			c->values = d;
		}
		else if (c->layout == VEXEC_OFFSETS)
		{
			/* each value's bytes end to end, detoasted, without its header */
			int32	   *off = palloc(sizeof(int32) * (n + 1));
			const void **buffers = palloc(sizeof(void *));
			int64	   *sizes = palloc(sizeof(int64));
			StringInfoData bytes;

			initStringInfo(&bytes);
			off[0] = 0;
			for (int r = 0; r < n; r++)
			{
				if (!u->nulls[(st->pos + r) * st->ncols + i])
				{
					varlena    *p = (varlena *) DatumGetPointer(u->values[(st->pos + r) * st->ncols + i]);
					varlena    *plain = pg_detoast_datum_packed(p);

					appendBinaryStringInfo(&bytes, VARDATA_ANY(plain), VARSIZE_ANY_EXHDR(plain));
					if (plain != p)
						pfree(plain);
				}
				off[r + 1] = bytes.len;
			}
			buffers[0] = bytes.data;
			sizes[0] = bytes.len;
			c->values = off;
			c->buffers = buffers;
			c->buffer_sizes = sizes;
			c->nbuffers = 1;
		}
		else
		{
			char	   *v = palloc0((Size) att->attlen * Max(n, 1));

			c->width = att->attlen;
			c->stride = att->attlen;
			for (int r = 0; r < n; r++)
			{
				Datum		d = u->values[(st->pos + r) * st->ncols + i];

				if (u->nulls[(st->pos + r) * st->ncols + i])
					continue;
				if (att->attbyval)
					store_att_byval(v + (Size) r * att->attlen, d, att->attlen);
				else
					memcpy(v + (Size) r * att->attlen, DatumGetPointer(d), att->attlen);
			}
			c->values = v;
		}
	}
	if (st->tids)
	{
		out->tids = palloc(sizeof(ItemPointerData) * Max(n, 1));
		memcpy(out->tids, &u->tids[st->pos], sizeof(ItemPointerData) * n);
	}
	MemoryContextSwitchTo(old);
	st->pos += n;
	batch->nrows = n;
	batch->visible = NULL;
	batch->tids = st->tids ? out->tids : NULL;
	batch->columns = out->columns;
	batch->owner = out;
	return true;
}

static void
src_retain(void *arg, void *owner)
{
	Out		   *out = owner;

	calls[M_RETAIN]++;
	if (out->retained)
		return;
	out->retained = true;
	out->unit->refs++;
}

static void
src_release(void *arg, void *owner)
{
	SrcState   *st = arg;
	Out		   *out = owner;

	calls[M_RELEASE]++;
	if (!out->retained)
		return;
	out->retained = false;
	if (--out->unit->refs == 0 && !out->unit->current)
		unit_free(out->unit);
	if (out != st->out)
	{
		MemoryContextDelete(out->cxt);
		pfree(out);
	}
}

/* After table_rescan(), which began heap's scan again */
static void
src_rescan(void *arg)
{
	SrcState   *st = arg;

	calls[M_RESCAN]++;
	unit_done(st);
	st->done = false;
	st->nkeys = st->nspec_keys;
}

/* The identity of the dictionary of a column of the last batch: its unit's (minor 3) */
static uint64
src_dictionary_id(void *arg, int column)
{
	SrcState   *st = arg;

	calls[M_DICTIONARY_ID]++;
	if (st->out == NULL || st->out->unit == NULL || column < 0 || column >= st->ncols ||
		st->out->unit->dicts == NULL || st->out->unit->dicts[column] == NULL)
		return 0;
	return st->out->unit->dicts[column]->id;
}

static void
src_end(void *arg)
{
	SrcState   *st = arg;

	calls[M_END]++;
	ExecDropSingleTupleTableSlot(st->slot);
	MemoryContextDelete(st->cxt);
}

static void
estimate_scan(Relation rel, const VexecSourceSpec *spec, double *rows, double *bytes)
{
	double		tuples = Max(rel->rd_rel->reltuples, 0);
	double		width = 0;

	for (int i = 0; i < spec->ncolumns; i++)
	{
		Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), spec->attnums[i] - 1);

		width += att->attlen > 0 ? att->attlen : 32;
	}
	if (spec->size >= offsetof(VexecSourceSpec, flags) + sizeof(spec->flags) && (spec->flags & VEXEC_SRC_TIDS))
		width += sizeof(ItemPointerData);
	*rows = spec->quals != NIL ? tuples / 5 : tuples;
	*bytes = width * *rows;
}

static void
src_estimate(Relation rel, Snapshot snapshot, const VexecSourceSpec *spec, double *rows, double *bytes)
{
	calls[M_ESTIMATE]++;
	estimate_scan(rel, spec, rows, bytes);
}

static void
src_estimate_ex(Relation rel, Snapshot snapshot, const VexecSourceSpec *spec, VexecSourceEstimate *est)
{
	bool		tids = spec->size >= offsetof(VexecSourceSpec, flags) + sizeof(spec->flags) &&
		(spec->flags & VEXEC_SRC_TIDS) != 0;
	bool		ints = true;

	calls[M_ESTIMATE_EX]++;
	estimate_scan(rel, spec, &est->rows, &est->bytes);
	for (int i = 0; i < spec->ncolumns && ints; i++)
	{
		Oid			type = TupleDescAttr(RelationGetDescr(rel), spec->attnums[i] - 1)->atttypid;

		ints = type == INT2OID || type == INT4OID || type == INT8OID;
	}
	est->units = ceil(Max(rel->rd_rel->reltuples, 0) / UNIT_ROWS);
	est->flags = spec->quals == NIL && !tids && ints ? VEXEC_EST_AGG_EXACT : 0;
}

static bool
answer(SrcState *st, Unit *u, const VexecSourceAgg *req, VexecSourceAggAnswer *a)
{
	int			col = -1;
	ColStats   *cs;
	Oid			type;

	if (req->kind == VEXEC_SRC_AGG_ROWS)
	{
		a->value = Int64GetDatum(u->nrows);
		a->isnull = false;
		return true;
	}
	for (int i = 0; i < st->ncols; i++)
		if (st->attnums[i] == req->attnum)
			col = i;
	if (col < 0)
		return false;
	cs = &u->stats[col];
	type = TupleDescAttr(st->desc, req->attnum - 1)->atttypid;
	switch (req->kind)
	{
		case VEXEC_SRC_AGG_COUNT:
			a->value = Int64GetDatum(cs->count);
			a->isnull = false;
			return true;
		case VEXEC_SRC_AGG_MIN:
		case VEXEC_SRC_AGG_MAX:
			{
				int64		x = req->kind == VEXEC_SRC_AGG_MIN ? cs->min : cs->max;

				if (!cs->ints || req->type != type)
					return false;
				a->isnull = cs->count == 0;
				a->value = type == INT2OID ? Int16GetDatum((int16) x) :
					type == INT4OID ? Int32GetDatum((int32) x) : Int64GetDatum(x);
				return true;
			}
		case VEXEC_SRC_AGG_SUM:
			/* sum(int2) and sum(int4) are int8 */
			if (!cs->ints || type == INT8OID || req->type != INT8OID)
				return false;
			a->isnull = cs->count == 0;
			a->value = Int64GetDatum((int64) cs->sum);
			return true;
		default:
			return false;
	}
}

static bool
src_aggregate(void *arg, int nreqs, const VexecSourceAgg *reqs, VexecSourceAggAnswer *answers, int64 *nrows)
{
	SrcState   *st = arg;
	MemoryContext old;
	bool		ok = true;

	calls[M_AGGREGATE]++;
	/* only at the start of a unit */
	if (st->unit && st->pos > 0 && st->pos < st->unit->nrows)
		return false;
	if (st->unit && st->pos >= st->unit->nrows)
		unit_done(st);
	if (st->unit == NULL && !load_unit(st))
		return false;
	MemoryContextReset(st->answers);
	old = MemoryContextSwitchTo(st->answers);
	for (int i = 0; i < nreqs && ok; i++)
		ok = answer(st, st->unit, &reqs[i], &answers[i]);
	MemoryContextSwitchTo(old);
	if (!ok)
		return false;			/* the unit's rows are next()'s */
	calls[M_AGGREGATE_ANSWERED]++;
	*nrows = st->unit->nrows;
	unit_done(st);
	return true;
}

static void
src_set_keys(void *arg, int nkeys, const ScanKeyData *keys)
{
	SrcState   *st = arg;
	MemoryContext old = MemoryContextSwitchTo(st->cxt);
	ScanKeyData *all = palloc(sizeof(ScanKeyData) * Max(st->nspec_keys + nkeys, 1));

	calls[M_SET_KEYS]++;
	if (st->nspec_keys > 0)
		memcpy(all, st->keys, sizeof(ScanKeyData) * st->nspec_keys);
	if (nkeys > 0)
		memcpy(all + st->nspec_keys, keys, sizeof(ScanKeyData) * nkeys);
	st->keys = all;
	st->nkeys = st->nspec_keys + nkeys;
	MemoryContextSwitchTo(old);
}

/* ---- the sink ---- */

typedef struct SinkState
{
	Relation	rel;
	CommandId	cid;
	int			options;
	BulkInsertState bistate;
	TupleTableSlot **slots;
	int			nslots;
} SinkState;

static bool
sink_supports(Relation rel, AttrNumber attnum, VexecSinkLayout *layout)
{
	Form_pg_attribute att = TupleDescAttr(RelationGetDescr(rel), attnum - 1);

	calls[M_SINK_SUPPORTS]++;
	memset(layout, 0, sizeof(VexecSinkLayout));
	if (att->atttypid == XMLOID)
		return false;
	if (att->attlen > 0)
	{
		layout->layout = att->atttypid == BOOLOID ? VEXEC_BYTE_BOOL : VEXEC_FIXED;
		layout->width = att->attlen;
		layout->stride = att->attlen;
	}
	else
		layout->layout = VEXEC_DATUM;
	return true;
}

static void *
sink_begin(Relation rel, const VexecSinkSpec *spec)
{
	SinkState  *st = palloc0(sizeof(SinkState));

	calls[M_SINK_BEGIN]++;
	st->rel = rel;
	st->cid = spec->cid;
	st->options = spec->options;
	st->bistate = GetBulkInsertState();
	return st;
}

static void
sink_put(void *state, VexecSinkBatch *batch)
{
	SinkState  *st = state;
	TupleDesc	desc = RelationGetDescr(st->rel);

	calls[M_SINK_PUT]++;
	if (st->nslots < batch->nrows)
	{
		st->slots = st->slots ? repalloc(st->slots, sizeof(TupleTableSlot *) * batch->nrows) :
			palloc(sizeof(TupleTableSlot *) * batch->nrows);
		for (; st->nslots < batch->nrows; st->nslots++)
			st->slots[st->nslots] = table_slot_create(st->rel, NULL);
	}
	for (int r = 0; r < batch->nrows; r++)
	{
		TupleTableSlot *slot = st->slots[r];

		ExecClearTuple(slot);
		for (int a = 0; a < desc->natts; a++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, a);
			const VexecColumn *c = &batch->columns[a];
			bool		isnull = att->attisdropped ||
				(c->validity && !((c->validity[r / 64] >> (r % 64)) & 1));

			slot->tts_isnull[a] = isnull;
			if (isnull)
				slot->tts_values[a] = (Datum) 0;
			else if (c->layout == VEXEC_DATUM)
				slot->tts_values[a] = ((const Datum *) c->values)[r];
			else
				slot->tts_values[a] = fetch_att((const char *) c->values + (Size) r * c->stride, att->attbyval,
												c->width);
		}
		ExecStoreVirtualTuple(slot);
	}
	table_multi_insert(st->rel, st->slots, batch->nrows, st->cid, st->options, st->bistate);
	if (batch->tids)
		for (int r = 0; r < batch->nrows; r++)
			batch->tids[r] = st->slots[r]->tts_tid;
}

static void
sink_end(void *state)
{
	SinkState  *st = state;

	calls[M_SINK_END]++;
	for (int i = 0; i < st->nslots; i++)
		ExecDropSingleTupleTableSlot(st->slots[i]);
	FreeBulkInsertState(st->bistate);
	table_finish_bulk_insert(st->rel, st->options);
	pfree(st);
}

static VexecSourceRoutine source = {
	.size = sizeof(VexecSourceRoutine),
	.minor = VEXEC_SOURCE_MINOR,
	.name = "vexec_test",
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
	.dictionary_id = src_dictionary_id,
	.estimate_ex = src_estimate_ex,
};

static VexecSinkRoutine sink = {
	.size = sizeof(VexecSinkRoutine),
	.minor = VEXEC_SINK_MINOR,
	.name = "vexec_test",
	.supports = sink_supports,
	.begin = sink_begin,
	.put = sink_put,
	.end = sink_end,
};

void		_PG_init(void);

/* Only while the postmaster preloads the library, after vexec */
void
_PG_init(void)
{
	DefineCustomBoolVariable("vexec_test.source_text_offsets",
							 "Hands the test source's varlena columns as int32 offsets into their bytes.",
							 NULL, &text_offsets, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_test.source_dictionaries",
							 "Hands the test source's varlena columns as a unit's dictionary, where vexec asks for dictionaries.",
							 NULL, &dictionaries, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	if (!process_shared_preload_libraries_in_progress)
		return;
	source.am = routine();
	sink.am = routine();
	vexec_register_source(&source);
	vexec_register_sink(&sink);
}

/* vexec_test.source_calls(): each member's calls in this backend */
Datum
vexec_test_source_calls(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;

	InitMaterializedSRF(fcinfo, 0);
	for (int m = 0; m < NMEMBERS; m++)
	{
		Datum		values[2];
		bool		nulls[2] = {false, false};

		values[0] = CStringGetTextDatum(member_names[m]);
		values[1] = Int64GetDatum(calls[m]);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	PG_RETURN_VOID();
}
