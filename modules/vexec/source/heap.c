/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * heap.c
 *	  Heap's page reader: a heap table's visible tuples deformed straight
 *	  into a batch's columns, a page at a time (pg_vector_executor.md
 *	  §3.5.2, V4).
 *
 * vexec begins the scan through table_beginscan() or
 * table_beginscan_parallel(), so heap keeps the snapshot's registration,
 * the predicate lock, the buffer strategy, synchronized scans and a
 * parallel scan's share of the blocks.  heap_beginscan() gives a sequential
 * scan its read stream (READ_STREAM_SEQUENTIAL | READ_STREAM_USE_BATCHING),
 * whose callback picks the blocks, serially or by the parallel scan's
 * chunks (PG19:src/backend/access/heap/heapam.c:255-315, 1168-1330).  The
 * reader takes each page from that stream as heap_fetch_next_buffer() does
 * (heapam.c:711-748), and has it prepared as heapgettup_pagemode() has it
 * (heapam.c:1074-1160): heap_prepare_pagescan() prunes it, takes the share
 * lock, runs PostgreSQL 19's batch MVCC test (HeapTupleSatisfiesMVCCBatch)
 * and the SERIALIZABLE check, and leaves the visible tuples' offsets in
 * rs_vistuples[] (heapam.c:619-703).  PostgreSQL's own TABLESAMPLE path
 * sets rs_cbuf and calls heap_prepare_pagescan() the same way
 * (PG19:src/backend/access/heap/heapam_handler.c:2215-2220).  So
 * visibility is never decided here: the distributed snapshot the port
 * builds into the executor's snapshot (pg19/modules/gp_core/gp_dtx.c:46-60)
 * reaches heap's own test.
 *
 * Then, for each visible tuple, the columns the scan reads are deformed
 * into the batch, as heap_deform_tuple() deforms a tuple
 * (PG19:src/backend/access/common/heaptuple.c:1254-1366): the attributes
 * before the first that has no fixed offset read at their attcacheoff, the
 * rest walked as align_fetch_then_add() walks them (access/tupmacs.h),
 * NULLs from the tuple's bitmap, and the attributes a tuple lacks -- added
 * by ALTER TABLE after it was written -- from their missing values
 * (getmissingattr()).  A value is written into its column's build shape
 * (batch.h) without passing through a Datum and a slot:
 *
 *	fixed-width, bool	its bytes, at the column's stride: the tuple holds
 *						them as a slot's Datum would;
 *	varlena				for the types whose functions take a short header
 *						as it is, a Datum pointing into the page, which the
 *						batch keeps pinned (below); for the others, and past
 *						the pins, a copy in the batch's arena with the
 *						header those functions want (rows.c).
 *
 * The pins (§3.5.2, "late copying").  A page whose values a batch points
 * into stays pinned until the batch's consumer is done with it -- the next
 * batch, a rescan or the end -- the lifetime a batch's arena has, which is
 * what a consumer that keeps a value copies it for (§3.4.5).  A batch pins
 * at most VEXEC_HEAP_PINS pages so: the read stream's look-ahead is
 * bounded by the pins a backend holds (bufmgr.c, GetAdditionalPinLimit()),
 * and a page past them has its values copied.  External TOAST pointers are
 * pointed to, or copied, as pointers, never fetched (§3.5.2).
 *
 * A scan that reads no column -- count(*) -- deforms nothing: a page's
 * rows are rs_ntuples, which heap_prepare_pagescan() counted (H1, §3.14).
 *
 * A VecSort's running bound (H6, §3.14; vecsort.c), where the scan checks
 * it before its quals: a tuple whose bound key is strictly past the bound
 * is passed over before anything of it is deformed -- the key read as
 * heap_getattr() reads it, at its attcacheoff where it has one -- and never
 * enters the batch.
 *
 * Interrupts are checked once a page, and the rows a batch hands are
 * counted in the table's statistics once a batch, as heap_getnextslot()
 * counts each row it returns (pgstat_count_heap_getnext()).
 *
 * A bitmap heap scan's pages (H9, §3.14; VecBitmapHeapScan).  A scan
 * table_beginscan_bm() began, its iterator over a bitmap set, gives its
 * pages through heap's own preparation of a bitmap page,
 * BitmapHeapScanNextBlock() (PG19:src/backend/access/heap/
 * heapam_handler.c:2500): an exact page's offsets the bitmap holds, each
 * followed along its HOT chain to the version the snapshot sees, a lossy
 * page's every visible tuple, and the page's need of a recheck.  That
 * function is static; the access method's callback,
 * table_scan_bitmap_next_tuple(), returns a page's first tuple and leaves
 * the page prepared as page mode leaves one -- its visible tuples' offsets
 * in rs_vistuples[], their number in rs_ntuples -- so the reader takes the
 * first through it, and the rest of the page as it takes a sequential
 * scan's.  The rows are counted as fetched, as that callback counts them
 * (pgstat_count_heap_fetch()).
 *
 * It leans on heap's internals: HeapScanDescData's fields and the page-mode
 * protocol (access/heapam.h).  A release that changes them is checked here
 * (§7); the slot path stays for a scan the reader does not take: a
 * snapshot that is not MVCC's, where heap reads no page at a time.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/tableam.h"
#include "access/tupmacs.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/bufmgr.h"
#include "storage/read_stream.h"
#include "utils/rel.h"
#include "varatt.h"

#include "vexec.h"
#include "batch/batch.h"
#include "exec/exec.h"
#include "source/source.h"

/* What a column's value is written as, per column. */
typedef enum HeapColKind
{
	HC_BOOL,					/* BYTE_BOOL */
	HC_FIXED,					/* FIXED: its bytes at the stride */
	HC_VARLENA_PAGE,			/* DATUM, pointing into the page where it may */
	HC_COPY						/* DATUM, copied into the arena */
} HeapColKind;

struct VexecHeapReader
{
	HeapScanDesc scan;
	bool		bitmap;			/* a bitmap heap scan's pages */
	TupleTableSlot *slot;		/* bitmap: the callback's, for a page's first
								 * tuple */
	uint64		exact_pages;	/* bitmap: as the callback counts them */
	uint64		lossy_pages;
	Relation	rel;
	TupleDesc	desc;
	int			ncols;			/* attributes read, attnos ascending */
	const AttrNumber *attrs;
	uint8	   *kinds;			/* HeapColKind, per attribute read */
	int			maxattr;		/* the highest attno read */
	bool		need_tid;		/* the batch's last column is ctid */
	bool		any_page_values;	/* some column may point into pages */

	bool		on_page;		/* rs_cbuf holds a prepared page */
	uint32		index;			/* its next tuple, into rs_vistuples[] */
	bool		page_values;	/* the batch being filled points into it */
	bool		done;

	Buffer		pins[VEXEC_HEAP_PINS];	/* pages the current batch points
										 * into, past the current one */
	int			npins;

	VexecSortBound *bound;		/* a VecSort's running bound, or NULL */
	int			bound_checks;	/* since its context was last reset */

	int64		pages;			/* for EXPLAIN ANALYZE */
};

/*
 * Whether the page reader can read a relation: heap's, under a snapshot
 * heap reads a page at a time.
 */
bool
vexec_heap_reader_possible(Relation rel)
{
	return rel->rd_tableam == GetHeapamTableAmRoutine();
}

/*
 * The reader of a scan table_beginscan() or table_beginscan_parallel()
 * began on a heap table, for the attributes attrs (ascending, none
 * dropped) and ctid where need_tid: the batch's columns are those, in that
 * order.  NULL where heap does not read the scan a page at a time: its
 * snapshot is not MVCC's, or it has no read stream.
 */
static void set_kinds(VexecHeapReader *hr, VexecBatch *batch);

VexecHeapReader *
vexec_heap_reader_begin(TableScanDesc sscan, VexecBatch *batch, int nattrs,
						const AttrNumber *attrs, bool need_tid)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;
	VexecHeapReader *hr;

	if (!vexec_heap_reader_possible(sscan->rs_rd) ||
		(sscan->rs_flags & SO_ALLOW_PAGEMODE) == 0 ||
		(sscan->rs_flags & SO_TYPE_SEQSCAN) == 0 ||
		scan->rs_read_stream == NULL || sscan->rs_nkeys != 0)
		return NULL;

	hr = palloc0(sizeof(VexecHeapReader));
	hr->scan = scan;
	hr->rel = sscan->rs_rd;
	hr->desc = RelationGetDescr(hr->rel);
	hr->ncols = nattrs;
	hr->attrs = attrs;
	hr->need_tid = need_tid;
	hr->kinds = palloc(Max(nattrs, 1));
	hr->maxattr = nattrs > 0 ? attrs[nattrs - 1] : 0;
	set_kinds(hr, batch);
	return hr;
}

/* How each column of the batch is written (HeapColKind). */
static void
set_kinds(VexecHeapReader *hr, VexecBatch *batch)
{
	int			i;

	for (i = 0; i < hr->ncols; i++)
	{
		const VexecType *type = batch->types[i];

		switch (type->tclass)
		{
			case VEXEC_TC_BOOL:
				hr->kinds[i] = HC_BOOL;
				break;
			case VEXEC_TC_FIXED:
			case VEXEC_TC_DATE:
			case VEXEC_TC_TIMESTAMP:
			case VEXEC_TC_INTERVAL:
			case VEXEC_TC_BYREF:
				hr->kinds[i] = HC_FIXED;
				break;
			case VEXEC_TC_VARLENA:

				/*
				 * The types whose functions read a short header as it is
				 * point into the page; the others would copy a short header
				 * on every call, so they get one copy, with the header they
				 * take (§3.4.2).
				 */
				if (type->short_ok && type->typlen == -1)
				{
					hr->kinds[i] = HC_VARLENA_PAGE;
					hr->any_page_values = true;
				}
				else
					hr->kinds[i] = HC_COPY;
				break;
			default:			/* numeric, cstring */
				hr->kinds[i] = HC_COPY;
				break;
		}
	}
}

/*
 * The reader of a bitmap heap scan table_beginscan_bm() began on a heap
 * table, whose iterator over its bitmap is set (above).  slot is a slot of
 * the table's for the callback, which the reader clears.
 */
VexecHeapReader *
vexec_heap_reader_begin_bitmap(TableScanDesc sscan, VexecBatch *batch, int nattrs,
							   const AttrNumber *attrs, bool need_tid, TupleTableSlot *slot)
{
	HeapScanDesc scan = (HeapScanDesc) sscan;
	VexecHeapReader *hr;

	/* heap prepares a bitmap page as page mode prepares one */
	if (!vexec_heap_reader_possible(sscan->rs_rd) ||
		(sscan->rs_flags & SO_TYPE_BITMAPSCAN) == 0 || sscan->rs_nkeys != 0)
		return NULL;
	hr = palloc0(sizeof(VexecHeapReader));
	hr->scan = scan;
	hr->rel = sscan->rs_rd;
	hr->desc = RelationGetDescr(hr->rel);
	hr->ncols = nattrs;
	hr->attrs = attrs;
	hr->need_tid = need_tid;
	hr->kinds = palloc(Max(nattrs, 1));
	hr->maxattr = nattrs > 0 ? attrs[nattrs - 1] : 0;
	hr->bitmap = true;
	hr->slot = slot;
	set_kinds(hr, batch);
	return hr;
}

/* Let go of the pages the last batch pointed into. */
static void
release_pins(VexecHeapReader *hr)
{
	int			i;

	for (i = 0; i < hr->npins; i++)
		ReleaseBuffer(hr->pins[i]);
	hr->npins = 0;
}

/*
 * The scan's next page, prepared: as heap_fetch_next_buffer() and
 * heapgettup_pagemode() take it, forward.  The page before it is let go,
 * or its pin kept for the batch that points into it.  False at the end.
 */
static bool
next_bitmap_page(VexecHeapReader *hr);

static bool
next_page(VexecHeapReader *hr)
{
	HeapScanDesc scan = hr->scan;

	if (hr->bitmap)
		return next_bitmap_page(hr);
	if (BufferIsValid(scan->rs_cbuf))
	{
		if (hr->page_values)
		{
			Assert(hr->npins < VEXEC_HEAP_PINS);
			hr->pins[hr->npins++] = scan->rs_cbuf;
		}
		else
			ReleaseBuffer(scan->rs_cbuf);
		scan->rs_cbuf = InvalidBuffer;
	}
	hr->page_values = false;
	hr->on_page = false;

	/* once a page, as heap_fetch_next_buffer() checks */
	CHECK_FOR_INTERRUPTS();

	Assert(ScanDirectionIsForward(scan->rs_dir));
	scan->rs_cbuf = read_stream_next_buffer(scan->rs_read_stream, NULL);
	if (!BufferIsValid(scan->rs_cbuf))
	{
		/* the end of the scan, as heapgettup_pagemode() leaves it */
		scan->rs_cblock = InvalidBlockNumber;
		scan->rs_prefetch_block = InvalidBlockNumber;
		scan->rs_ctup.t_data = NULL;
		scan->rs_inited = false;
		return false;
	}
	scan->rs_cblock = BufferGetBlockNumber(scan->rs_cbuf);
	heap_prepare_pagescan(&scan->rs_base);
	hr->on_page = true;
	hr->index = 0;
	hr->pages++;
	return true;
}

/*
 * A bitmap heap scan's next page, prepared by heap: the callback returns its
 * first tuple, which the slot lets go of, and leaves its visible tuples'
 * offsets in rs_vistuples[].  The page before is let go by heap as it moves
 * on, so where the batch points into it the reader pins it once more, for
 * the batch.  False at the end of the bitmap.
 */
static bool
next_bitmap_page(VexecHeapReader *hr)
{
	HeapScanDesc scan = hr->scan;
	bool		recheck;

	if (BufferIsValid(scan->rs_cbuf) && hr->page_values)
	{
		Assert(hr->npins < VEXEC_HEAP_PINS);
		IncrBufferRefCount(scan->rs_cbuf);
		hr->pins[hr->npins++] = scan->rs_cbuf;
	}
	hr->page_values = false;
	hr->on_page = false;

	CHECK_FOR_INTERRUPTS();

	/* past the page's last tuple, so that the callback moves to the next page */
	scan->rs_cindex = scan->rs_ntuples;
	if (!table_scan_bitmap_next_tuple(&scan->rs_base, hr->slot, &recheck,
									  &hr->lossy_pages, &hr->exact_pages))
	{
		ExecClearTuple(hr->slot);
		return false;
	}
	ExecClearTuple(hr->slot);
	Assert(scan->rs_cindex == 1 && scan->rs_ntuples >= 1);
	hr->index = 0;
	hr->on_page = true;
	hr->pages++;

	/* the callback counted the first as fetched; the rest are counted here */
	if (scan->rs_ntuples > 1 && pgstat_should_count_relation(hr->rel))
		hr->rel->pgstat_info->counts.tuples_fetched += scan->rs_ntuples - 1;
	return true;
}

/* A value the tuple holds at p, written into row `row` of column c. */
static inline void
store_value(VexecHeapReader *hr, VexecBatch *in, int c, int row, const char *p,
			bool page_values)
{
	VexecVec   *v = &in->cols[c];

	if (v->validity)
		vexec_bit_set(v->validity, row);
	switch (hr->kinds[c])
	{
		case HC_BOOL:
			((uint8 *) v->values)[row] = *(const uint8 *) p != 0;
			break;
		case HC_FIXED:
			memcpy((char *) v->values + (Size) row * v->shape.stride, p, v->type->typlen);
			break;
		case HC_VARLENA_PAGE:
			if (page_values)
			{
				((Datum *) v->values)[row] = PointerGetDatum(p);
				break;
			}
			pg_fallthrough;
		case HC_COPY:
			((Datum *) v->values)[row] = vexec_varlena_copy(in, v->type, PointerGetDatum(p));
			break;
	}
}

/* A NULL in row `row` of column c. */
static inline void
store_null(VexecBatch *in, int c, int row)
{
	VexecVec   *v = &in->cols[c];

	vexec_vec_set_null(in, v, row);
	if (v->shape.layout == VEXEC_DATUM)
		((Datum *) v->values)[row] = (Datum) 0;
}

/* A value given as a Datum -- an attribute's missing value -- into row `row`. */
static void
store_datum(VexecBatch *in, int c, int row, Datum d, bool isnull)
{
	VexecVec   *v = &in->cols[c];
	const VexecType *type = v->type;

	if (isnull)
	{
		store_null(in, c, row);
		return;
	}
	if (v->validity)
		vexec_bit_set(v->validity, row);
	switch (v->shape.layout)
	{
		case VEXEC_BYTE_BOOL:
			((uint8 *) v->values)[row] = DatumGetBool(d) ? 1 : 0;
			break;
		case VEXEC_FIXED:
			{
				char	   *p = (char *) v->values + (Size) row * v->shape.stride;

				if (type->typbyval)
					store_att_byval(p, d, type->typlen);
				else
					memcpy(p, DatumGetPointer(d), type->typlen);
				break;
			}
		default:
			((Datum *) v->values)[row] = vexec_varlena_copy(in, type, d);
			break;
	}
}

/*
 * The columns a tuple gives, into row `row`: as heap_deform_tuple() reads
 * them, but only those read, and each written into its column.
 */
static void
deform(VexecHeapReader *hr, VexecBatch *in, int row, HeapTupleHeader tup,
	   bool page_values)
{
	TupleDesc	desc = hr->desc;
	bool		hasnulls = (tup->t_infomask & HEAP_HASNULL) != 0;
	const uint8 *bp = tup->t_bits;
	const char *tp = (const char *) tup + tup->t_hoff;
	int			natts = Min((int) HeapTupleHeaderGetNatts(tup), desc->natts);
	int			limit = Min(natts, hr->maxattr);
	int			cached = Min(desc->firstNonCachedOffsetAttr, limit);
	int			first_null = limit;
	int			c = 0;
	int			attnum;
	uint32		off = 0;

	if (hasnulls && limit > 0)
	{
		first_null = first_null_attr(bp, limit);
		cached = Min(cached, first_null);
	}

	/* the attributes at fixed offsets: read where they are */
	while (c < hr->ncols && hr->attrs[c] <= cached)
	{
		CompactAttribute *cattr = TupleDescCompactAttr(desc, hr->attrs[c] - 1);

		store_value(hr, in, c, row, tp + cattr->attcacheoff, page_values);
		c++;
	}

	/* the rest, walked from the first without a fixed offset */
	if (c < hr->ncols && hr->attrs[c] <= limit)
	{
		if (cached > 0)
		{
			CompactAttribute *last = TupleDescCompactAttr(desc, cached - 1);

			off = last->attcacheoff + last->attlen;
		}
		for (attnum = cached; attnum < limit && c < hr->ncols; attnum++)
		{
			CompactAttribute *cattr = TupleDescCompactAttr(desc, attnum);
			bool		wanted = hr->attrs[c] == attnum + 1;

			if (attnum >= first_null && att_isnull(attnum, bp))
			{
				if (wanted)
				{
					store_null(in, c, row);
					c++;
				}
				continue;
			}

			/* align, as align_fetch_then_add() does */
			if (cattr->attlen != -1 || !VARATT_IS_SHORT(tp + off))
				off = TYPEALIGN(cattr->attalignby, off);
			if (wanted)
			{
				store_value(hr, in, c, row, tp + off, page_values);
				c++;
			}
			if (cattr->attlen > 0)
				off += cattr->attlen;
			else if (cattr->attlen == -1)
				off += VARSIZE_ANY(tp + off);
			else
				off += strlen(tp + off) + 1;
		}
	}

	/* the attributes the tuple lacks: their missing values */
	for (; c < hr->ncols; c++)
	{
		bool		isnull;
		Datum		d = getmissingattr(desc, hr->attrs[c], &isnull);

		store_datum(in, c, row, d, isnull);
	}
}

/* A VecSort's running bound, checked before a tuple is deformed (H6). */
void
vexec_heap_reader_set_bound(VexecHeapReader *hr, VexecSortBound *bound)
{
	hr->bound = bound;
}

/* Whether a tuple's bound key is strictly past the running bound. */
static bool
past_bound(VexecHeapReader *hr, HeapTupleHeader tup, uint32 len)
{
	VexecSortBound *b = hr->bound;
	HeapTupleData htup;
	MemoryContext old;
	bool		isnull;
	Datum		d;
	bool		past;

	htup.t_data = tup;
	htup.t_len = len;
	ItemPointerSetInvalid(&htup.t_self);
	htup.t_tableOid = RelationGetRelid(hr->rel);
	old = MemoryContextSwitchTo(b->tmpcxt);
	d = heap_getattr(&htup, b->attno, hr->desc, &isnull);
	past = ApplySortComparator(d, isnull, b->value, b->isnull, &b->ssup) > 0;
	MemoryContextSwitchTo(old);
	if (++hr->bound_checks >= VEXEC_BATCH_ROWS)
	{
		MemoryContextReset(b->tmpcxt);
		hr->bound_checks = 0;
	}
	if (past)
		b->removed++;
	return past;
}

/*
 * The scan's next batch into `in`, whose columns are the reader's, in
 * their build shapes: up to VEXEC_BATCH_ROWS visible rows, from as many
 * pages as they take.  False at the end.
 */
bool
vexec_heap_reader_next(VexecHeapReader *hr, VexecBatch *in)
{
	HeapScanDesc scan = hr->scan;
	bool		deforms = hr->ncols > 0 || hr->need_tid;
	int			row = 0;
	int			i;

	/* the last batch is done with: its pages go, the current one stays */
	release_pins(hr);
	hr->page_values = false;

	vexec_batch_reset(in);
	if (hr->done)
		return false;
	if (deforms)
		vexec_batch_begin_rows(in);

	while (row < VEXEC_BATCH_ROWS)
	{
		uint32		take;
		Page		page;

		if (!hr->on_page || hr->index >= scan->rs_ntuples)
		{
			/*
			 * The page before is kept for the batch where the batch points
			 * into it, which it does only while it holds fewer pins than it
			 * may (below).
			 */
			if (!next_page(hr))
			{
				hr->done = true;
				break;
			}
			continue;			/* a page with no visible tuple is passed */
		}

		take = Min(scan->rs_ntuples - hr->index, (uint32) (VEXEC_BATCH_ROWS - row));
		if (deforms)
		{
			bool		page_values = hr->any_page_values &&
				(hr->page_values || hr->npins < VEXEC_HEAP_PINS);
			bool		bounded = hr->bound != NULL && hr->bound->active;
			int			out = row;

			page = BufferGetPage(scan->rs_cbuf);
			for (i = 0; i < (int) take; i++)
			{
				OffsetNumber lineoff = scan->rs_vistuples[hr->index + i];
				ItemId		lpp = PageGetItemId(page, lineoff);
				HeapTupleHeader tup = (HeapTupleHeader) PageGetItem(page, lpp);

				Assert(ItemIdIsNormal(lpp));
				if (bounded && past_bound(hr, tup, ItemIdGetLength(lpp)))
					continue;
				deform(hr, in, out, tup, page_values);
				if (hr->need_tid)
				{
					VexecVec   *v = &in->cols[hr->ncols];

					ItemPointerSet((ItemPointer) ((char *) v->values +
												  (Size) out * v->shape.stride),
								   scan->rs_cblock, lineoff);
				}
				out++;
			}
			if (page_values && out > row)
				hr->page_values = true;
			row = out;
		}
		else
			row += take;
		hr->index += take;
	}

	if (row == 0)
		return false;
	in->nrows = row;
	for (i = 0; i < in->ncols; i++)
		in->cols[i].nvalues = row;

	/*
	 * the rows handed, in the table's statistics, as getnextslot counts them;
	 * a bitmap scan's are counted as fetched, as their pages are taken
	 */
	if (!hr->bitmap && pgstat_should_count_relation(hr->rel))
		hr->rel->pgstat_info->counts.tuples_returned += row;
	return true;
}

/* The scan was rescanned (table_rescan): start again from its first page. */
void
vexec_heap_reader_rescan(VexecHeapReader *hr)
{
	release_pins(hr);
	hr->on_page = false;
	hr->page_values = false;
	hr->index = 0;
	hr->done = false;
}

/* The scan ends: let go of every page but the scan's own, which it ends. */
void
vexec_heap_reader_end(VexecHeapReader *hr)
{
	release_pins(hr);
}

/* Pages read, for EXPLAIN ANALYZE. */
int64
vexec_heap_reader_pages(VexecHeapReader *hr)
{
	return hr->pages;
}

/* A bitmap scan's exact and lossy pages, as heap's callback counted them. */
void
vexec_heap_reader_bitmap_pages(VexecHeapReader *hr, uint64 *exact, uint64 *lossy)
{
	*exact = hr->exact_pages;
	*lossy = hr->lossy_pages;
}
