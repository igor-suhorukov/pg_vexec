/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * write.c
 *	  The write buffer: rows inserted into a table of its own files, kept in
 *	  Arrow's layouts until they become a Parquet file -- and the files'
 *	  deletions at the ends of transactions.  After ch_table_engines'
 *	  buffer.c.
 *
 *	  A buffer holds the rows of one relation, one subtransaction and one
 *	  command id; a row of another command or subtransaction makes the file
 *	  first.  It becomes a file as the query that wrote ends (ExecutorFinish),
 *	  as COPY, CREATE TABLE AS and a rewrite end (finish_bulk_insert, the
 *	  utility hook), before commit and PREPARE, before its subtransaction
 *	  commits, as a scan begins whose snapshot would see its rows, and at
 *	  vexec_parquet.buffer_rows or vexec_parquet.buffer_mb.  The file's row
 *	  of vexec_parquet.files is inserted under the buffer's command id.
 *
 *	  Each row gets its row id, its TID, as it enters: from a range taken
 *	  from the storage's counter, in chunks that grow to the buffer's limit.
 *	  A file's rows are of consecutive ids: a new range that does not follow
 *	  the last makes the file first.  Ids a subtransaction's abort took rows
 *	  of are never given again: an index may name them.
 *
 *	  vexec's sink's batches go into the same buffers, their columns copied
 *	  in at once (vpq_buffer_put_batch()).
 *
 *	  A file a transaction writes is deleted where it aborts, and the
 *	  directories of the storages it drops where it commits; whatever a
 *	  crash leaves VACUUM finds (am.c).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/detoast.h"
#include "access/xact.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "varatt.h"

#include "vexec_parquet.h"
#include "convert.h"
#include "stats.h"
#include "vexec_sink.h"

typedef struct ColBuf
{
	VpqColumn	col;			/* a copy: the buffer outlives no relcache entry it needs */
	int			width;			/* fixed: bytes a value; 0: offsets and bytes; -1: bits */
	bool		image;			/* a numeric(p,s) of this file kept as its values' images: a NaN came */
	uint8	   *validity;
	char	   *values;
	Size		values_cap;
	char	   *data;
	Size		data_len;
	Size		data_cap;
	int64		nulls;
} ColBuf;

typedef struct WriteBuffer
{
	Oid			relid;
	VpqStorage	storage;
	char	   *dir;
	bool		s3;
	SubTransactionId subid;
	CommandId	cid;
	MemoryContext cxt;
	int			ncols;
	ColBuf	   *cols;
	int64		nrows;
	int64		cap;			/* rows the column buffers hold */
	Size		bytes;
	uint64		first_rowid;	/* of the buffered rows */
	uint64		next_rowid;		/* the taken range, [next_rowid, end_rowid) */
	uint64		end_rowid;
	uint64		chunk;
	char	   *codec;
	int			level;
	int			row_group_rows;
	int			page_bytes;
	bool		dir_made;
	bool		utf8;
} WriteBuffer;

typedef struct PendingDelete
{
	char	   *path;
	bool		dir;
	bool		at_commit;
	SubTransactionId subid;
} PendingDelete;

static List *buffers = NIL;		/* in TopTransactionContext */
static List *pending = NIL;		/* in TopTransactionContext */
static List *new_storages = NIL;	/* VpqStorage *, made by the running transaction */
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;

static void buffer_flush(WriteBuffer *b);

static int
kind_width(const VpqColumn *c)
{
	switch (c->kind)
	{
		case VPQ_K_BOOL:
			return -1;
		case VPQ_K_CHAR:
			return 1;
		case VPQ_K_INT2:
			return 2;
		case VPQ_K_INT4:
		case VPQ_K_FLOAT4:
		case VPQ_K_OID:
		case VPQ_K_DATE:
			return 4;
		case VPQ_K_INT8:
		case VPQ_K_FLOAT8:
		case VPQ_K_TIME:
		case VPQ_K_TIMESTAMP:
		case VPQ_K_TIMESTAMPTZ:
			return 8;
		case VPQ_K_NUMERIC:
		case VPQ_K_UUID:
			return 16;
		case VPQ_K_FIXED:
			return c->typlen;
		default:
			return 0;
	}
}

static WriteBuffer *
find_buffer(Oid relid)
{
	ListCell   *lc;

	foreach(lc, buffers)
	{
		WriteBuffer *b = lfirst(lc);

		if (b->relid == relid)
			return b;
	}
	return NULL;
}

static WriteBuffer *
make_buffer(Relation rel, CommandId cid)
{
	VpqTable   *t = vpq_table(rel);
	MemoryContext cxt;
	MemoryContext old;
	WriteBuffer *b;
	int			n = 0;

	if (t->external)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a vexec_parquet table that reads files is read-only: \"%s\"",
						RelationGetRelationName(rel)),
				 errhint("Make a table of its own files, without location, to write into.")));
	cxt = AllocSetContextCreate(TopTransactionContext, "vexec_parquet write buffer", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(cxt);
	b = palloc0(sizeof(WriteBuffer));
	b->cxt = cxt;
	b->relid = RelationGetRelid(rel);
	b->storage = t->storage;
	b->dir = pstrdup(t->dir);
	b->s3 = vpq_is_s3(t->dir);
	b->subid = GetCurrentSubTransactionId();
	b->cid = cid;
	b->utf8 = t->utf8;
	b->codec = pstrdup(t->opts.codec ? t->opts.codec : vpq_default_codec);
	b->level = t->opts.compression_level;
	b->row_group_rows = t->opts.row_group_rows > 0 ? t->opts.row_group_rows : vpq_default_row_group_rows;
	b->page_bytes = t->opts.page_bytes > 0 ? t->opts.page_bytes : VPQ_PAGE_BYTES;
	b->chunk = 1024;
	b->cols = palloc0(sizeof(ColBuf) * Max(t->natts, 1));
	for (int i = 0; i < t->natts; i++)
	{
		ColBuf	   *c;

		if (t->cols[i].kind == VPQ_K_DROPPED)
			continue;
		c = &b->cols[n++];
		c->col = t->cols[i];
		c->col.name = pstrdup(t->cols[i].name);
		c->width = kind_width(&c->col);
	}
	b->ncols = n;
	MemoryContextSwitchTo(old);

	old = MemoryContextSwitchTo(TopTransactionContext);
	buffers = lappend(buffers, b);
	MemoryContextSwitchTo(old);
	return b;
}

static void
drop_buffer(WriteBuffer *b)
{
	buffers = list_delete_ptr(buffers, b);
	MemoryContextDelete(b->cxt);
}

/* Room for rows more rows in every column */
static void
grow(WriteBuffer *b, int64 rows)
{
	int64		need = b->nrows + rows;
	int64		cap;

	if (need <= b->cap)
		return;
	cap = Max(b->cap * 2, Max(need, 1024));
	for (int i = 0; i < b->ncols; i++)
	{
		ColBuf	   *c = &b->cols[i];
		Size		vbytes = (cap + 7) / 8 + 8;
		Size		oldv = c->validity ? (b->cap + 7) / 8 + 8 : 0;
		Size		values;

		c->validity = c->validity ? repalloc_huge(c->validity, vbytes) :
			MemoryContextAllocHuge(b->cxt, vbytes);
		memset(c->validity + oldv, 0, vbytes - oldv);
		if (c->width > 0)
			values = (Size) cap * c->width;
		else if (c->width < 0)
			values = (cap + 7) / 8 + 8;
		else
			values = (Size) (cap + 1) * sizeof(int32);
		values = Max(values, c->values_cap);
		if (c->values == NULL)
		{
			c->values = MemoryContextAllocHuge(b->cxt, values);
			memset(c->values, 0, values);
		}
		else
		{
			Size		oldsize = c->values_cap;

			c->values = repalloc_huge(c->values, values);
			memset(c->values + oldsize, 0, values - oldsize);
		}
		c->values_cap = values;
	}
	b->cap = cap;
}

static void
data_append(WriteBuffer *b, ColBuf *c, const char *bytes, Size len)
{
	if (c->data_len + len > c->data_cap)
	{
		Size		cap = Max(c->data_cap * 2, Max(c->data_len + len, 4096));

		c->data = c->data ? repalloc_huge(c->data, cap) : MemoryContextAllocHuge(b->cxt, cap);
		c->data_cap = cap;
	}
	if (len > 0)
		memcpy(c->data + c->data_len, bytes, len);
	c->data_len += len;
	b->bytes += len;
}

static inline void
set_valid(ColBuf *c, int64 row)
{
	c->validity[row >> 3] |= (uint8) (1 << (row & 7));
}

/* A row's offsets in a variable-length column: the value's end */
static inline void
end_value(ColBuf *c, int64 row)
{
	((int32 *) c->values)[row + 1] = (int32) c->data_len;
}

static void
check_data_size(ColBuf *c)
{
	if (c->data_len > (Size) PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("the buffered values of column \"%s\" pass 2 GB", c->col.name)));
}

/*
 * A numeric(p,s) column whose file must keep a NaN or an infinity, which a
 * Parquet decimal cannot: its file keeps every value's image instead, the
 * decimals buffered so far made numerics again
 */
static void
column_to_image(WriteBuffer *b, ColBuf *c, int64 rows)
{
	int128	   *decimals = (int128 *) c->values;
	Size		cap = (Size) (b->cap + 1) * sizeof(int32);
	int32	   *off = MemoryContextAllocHuge(b->cxt, Max(cap, (Size) (rows + 1) * sizeof(int32)));

	off[0] = 0;
	c->values = (char *) off;
	c->values_cap = Max(cap, (Size) (rows + 1) * sizeof(int32));
	c->width = 0;
	c->image = true;
	c->data_len = 0;
	for (int64 r = 0; r < rows; r++)
	{
		if (c->validity[r >> 3] & (1 << (r & 7)))
		{
			struct varlena *v = (struct varlena *) DatumGetPointer(vpq_int128_to_numeric(decimals[r], c->col.scale));

			data_append(b, c, VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
			pfree(v);
		}
		off[r + 1] = (int32) c->data_len;
	}
	pfree(decimals);
	memset(&c->col.ftype, 0, sizeof(c->col.ftype));
	c->col.ftype.id = VPQ_T_BINARY;
}

/* A file's image column made the column's decimal again, for the next file */
static void
column_from_image(WriteBuffer *b, ColBuf *c)
{
	Size		values = (Size) Max(b->cap, 1) * 16;

	pfree(c->values);
	c->values = MemoryContextAllocHuge(b->cxt, values);
	memset(c->values, 0, values);
	c->values_cap = values;
	c->width = 16;
	c->image = false;
	memset(&c->col.ftype, 0, sizeof(c->col.ftype));
	c->col.ftype.id = VPQ_T_DECIMAL128;
	c->col.ftype.precision = c->col.precision;
	c->col.ftype.scale = c->col.scale;
	c->col.ftype.byte_width = 16;
}

/* A Datum of a column's base type into the buffer at row */
static void
put_datum(WriteBuffer *b, ColBuf *c, int64 row, Datum d)
{
	VpqColumn  *col = &c->col;
	char	   *at;
	int128		dec = 0;

	/* a numeric(p,s) value no decimal holds: the file's column its images */
	if (col->kind == VPQ_K_NUMERIC && !c->image && !vpq_numeric_to_int128(d, col->scale, &dec))
		column_to_image(b, c, row);
	at = c->width > 0 ? c->values + (Size) row * c->width : NULL;
	set_valid(c, row);
	switch (c->image ? VPQ_K_NUMERIC_ANY : col->kind)
	{
		case VPQ_K_BOOL:
			if (DatumGetBool(d))
				((uint8 *) c->values)[row >> 3] |= (uint8) (1 << (row & 7));
			break;
		case VPQ_K_CHAR:
			*(int8 *) at = (int8) DatumGetChar(d);
			break;
		case VPQ_K_INT2:
			*(int16 *) at = DatumGetInt16(d);
			break;
		case VPQ_K_INT4:
			*(int32 *) at = DatumGetInt32(d);
			break;
		case VPQ_K_OID:
			*(uint32 *) at = DatumGetObjectId(d);
			break;
		case VPQ_K_FLOAT4:
			*(float4 *) at = DatumGetFloat4(d);
			break;
		case VPQ_K_INT8:
			*(int64 *) at = DatumGetInt64(d);
			break;
		case VPQ_K_FLOAT8:
			*(float8 *) at = DatumGetFloat8(d);
			break;
		case VPQ_K_TIME:
			*(int64 *) at = DatumGetInt64(d);
			break;
		case VPQ_K_DATE:
			{
				DateADT		v = DatumGetDateADT(d);

				*(int32 *) at = DATE_NOT_FINITE(v) ? v : v + VPQ_EPOCH_DAYS;
			}
			break;
		case VPQ_K_TIMESTAMP:
		case VPQ_K_TIMESTAMPTZ:
			{
				Timestamp	v = DatumGetTimestamp(d);

				if (!TIMESTAMP_NOT_FINITE(v))
				{
					if (v > PG_INT64_MAX - VPQ_EPOCH_USECS)
						ereport(ERROR,
								(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
								 errmsg("timestamp out of range for a Parquet file of column \"%s\"",
										col->name)));
					v += VPQ_EPOCH_USECS;
				}
				*(int64 *) at = v;
			}
			break;
		case VPQ_K_NUMERIC:
			memcpy(at, &dec, sizeof(int128));
			break;
		case VPQ_K_UUID:
			memcpy(at, DatumGetPointer(d), 16);
			break;
		case VPQ_K_FIXED:
			if (col->typbyval)
			{
				Datum		tmp = d;

				store_att_byval(at, tmp, col->typlen);
			}
			else
				memcpy(at, DatumGetPointer(d), col->typlen);
			break;
		default:
			{
				/* a varlena's bytes, detoasted, without its header */
				struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

				/*
				 * text checked in the database's encoding, as the sink checks
				 * a batch's: a Parquet string is UTF-8, and a scan vouches for
				 * its files' text whole (VEXEC_COL_TEXT_WHOLE)
				 */
				if (col->kind == VPQ_K_TEXT || col->kind == VPQ_K_BPCHAR || col->kind == VPQ_K_JSON)
					(void) pg_verifymbstr(VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v), false);
				data_append(b, c, VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
				check_data_size(c);
				end_value(c, row);
				if ((Pointer) v != DatumGetPointer(d))
					pfree(v);
			}
			return;
	}
}

static void
put_null(ColBuf *c, int64 row)
{
	c->nulls++;
	if (c->width == 0)
		end_value(c, row);
}

/*
 * The next row id the buffer gives, a range taken first where it has none
 * left: a range that does not follow the buffered rows' makes their file
 * first
 */
static uint64
take_rowids(WriteBuffer *b, int64 want)
{
	if (b->next_rowid == b->end_rowid)
	{
		uint64		first;
		uint64		first_file;
		uint64		n = Max(b->chunk, (uint64) want);

		vpq_counters_take(&b->storage, n, 0, &first, &first_file);
		if (b->nrows > 0 && first != b->next_rowid)
			buffer_flush(b);
		if (b->nrows == 0)
			b->first_rowid = first;
		b->next_rowid = first;
		b->end_rowid = first + n;
		b->chunk = Min(b->chunk * 4, (uint64) vpq_buffer_rows);
	}
	return b->next_rowid;
}

/* The buffer of a relation for rows of the command, the other rows made a file first */
static WriteBuffer *
buffer_for(Relation rel, CommandId cid)
{
	WriteBuffer *b = find_buffer(RelationGetRelid(rel));
	SubTransactionId subid = GetCurrentSubTransactionId();

	/* another storage's rows -- before a TRUNCATE or a rewrite -- went with it */
	if (b != NULL && (b->storage.relnumber != rel->rd_locator.relNumber ||
					  b->storage.spc != rel->rd_locator.spcOid))
	{
		drop_buffer(b);
		b = NULL;
	}
	if (b != NULL && (b->subid != subid || b->cid != cid))
	{
		buffer_flush(b);
		b->subid = subid;
		b->cid = cid;
	}
	if (b != NULL)
	{
		/* the relation's live attributes now: rows of other columns make their file first */
		int			live = 0;
		TupleDesc	desc = RelationGetDescr(rel);

		for (int i = 0; i < desc->natts; i++)
			if (!TupleDescAttr(desc, i)->attisdropped)
				live++;
		if (live != b->ncols)
		{
			buffer_flush(b);
			drop_buffer(b);
			b = NULL;
		}
	}
	if (b == NULL)
		b = make_buffer(rel, cid);
	return b;
}

static void
after_rows(WriteBuffer *b)
{
	if (b->nrows >= vpq_buffer_rows || b->bytes >= (Size) vpq_buffer_mb * 1024 * 1024)
		buffer_flush(b);
}

uint64
vpq_buffer_next_rowid(Relation rel, CommandId cid)
{
	WriteBuffer *b = buffer_for(rel, cid);

	return take_rowids(b, 1);
}

/*
 * A row into the write buffer, its TID set in the slot; counted as an
 * insert unless it is another row's new version (an update's, a rewrite's)
 */
void
vpq_buffer_insert(Relation rel, TupleTableSlot *slot, CommandId cid, bool counted)
{
	WriteBuffer *b = buffer_for(rel, cid);
	uint64		rowid;
	int64		row;
	TupleDesc	desc = RelationGetDescr(rel);
	MemoryContext old;

	rowid = take_rowids(b, 1);
	slot_getallattrs(slot);
	grow(b, 1);
	row = b->nrows;
	old = MemoryContextSwitchTo(b->cxt);
	for (int i = 0, k = 0; i < desc->natts; i++)
	{
		ColBuf	   *c;

		if (TupleDescAttr(desc, i)->attisdropped)
			continue;
		c = &b->cols[k++];
		if (slot->tts_isnull[i])
			put_null(c, row);
		else
			put_datum(b, c, row, slot->tts_values[i]);
	}
	MemoryContextSwitchTo(old);
	b->nrows++;
	b->next_rowid++;
	b->bytes += 16;
	vpq_set_tid(&slot->tts_tid, rowid);
	slot->tts_tableOid = RelationGetRelid(rel);
	/* RETURNING's xmin: the inserting (sub)transaction's, as heap's tuple has it */
	vpq_slot_set_xmin(slot, GetCurrentTransactionId());
	vpq_slot_set_xmax(slot, InvalidTransactionId);
	if (counted)
		pgstat_count_heap_insert(rel, 1);
	after_rows(b);
}

/*
 * The columns' statistics of each row group of the buffered rows, as the
 * catalog keeps them (stats.c): the least and greatest by the type's own
 * order -- a string's and binary's bytewise -- and NULLs counted
 */
static void
column_stats(ColBuf *c, int64 from, int64 to, VpqRawStat *s)
{
	VpqColumn  *col = &c->col;
	int64		n = 0;

	memset(s, 0, sizeof(*s));
	s->klass = c->image ? VPQ_STATS_NONE : vpq_stats_class(col->kind);
	for (int64 r = from; r < to; r++)
		if (!(c->validity[r >> 3] & (1 << (r & 7))))
			s->nulls++;
	if (s->klass == VPQ_STATS_NONE)
		return;
	for (int64 r = from; r < to; r++)
	{
		const char *at;

		if (!(c->validity[r >> 3] & (1 << (r & 7))))
			continue;
		at = c->width > 0 ? c->values + (Size) r * c->width : NULL;
		switch (s->klass)
		{
			case VPQ_STATS_INT:
				{
					int64		v;

					switch (col->kind)
					{
						case VPQ_K_BOOL:
							v = (((uint8 *) c->values)[r >> 3] >> (r & 7)) & 1;
							break;
						case VPQ_K_CHAR:
							v = *(const int8 *) at;
							break;
						case VPQ_K_INT2:
							v = *(const int16 *) at;
							break;
						case VPQ_K_INT4:
							v = *(const int32 *) at;
							break;
						case VPQ_K_OID:
							v = *(const uint32 *) at;
							break;
						case VPQ_K_DATE:
							{
								int32		d = *(const int32 *) at;

								v = DATE_NOT_FINITE(d) ? d : d - VPQ_EPOCH_DAYS;
							}
							break;
						case VPQ_K_TIMESTAMP:
						case VPQ_K_TIMESTAMPTZ:
							{
								int64		t = *(const int64 *) at;

								v = TIMESTAMP_NOT_FINITE(t) ? t : t - VPQ_EPOCH_USECS;
							}
							break;
						default:
							v = *(const int64 *) at;
							break;
					}
					if (n == 0 || v < s->imin)
						s->imin = v;
					if (n == 0 || v > s->imax)
						s->imax = v;
				}
				break;
			case VPQ_STATS_FLOAT:
				{
					double		v = col->kind == VPQ_K_FLOAT4 ? (double) *(const float4 *) at : *(const float8 *) at;

					if (isnan(v))
					{
						s->may_nan = true;
						continue;
					}
					if (n == 0 || v < s->dmin)
						s->dmin = v;
					if (n == 0 || v > s->dmax)
						s->dmax = v;
				}
				break;
			case VPQ_STATS_DECIMAL:
				{
					int128		v;

					memcpy(&v, at, sizeof(int128));
					if (n == 0 || v < s->nmin)
						s->nmin = v;
					if (n == 0 || v > s->nmax)
						s->nmax = v;
				}
				break;
			case VPQ_STATS_BYTES:
				{
					const char *p;
					int			len;

					if (c->width > 0)
					{
						p = at;
						len = c->width;
					}
					else
					{
						int32		o0 = ((int32 *) c->values)[r];
						int32		o1 = ((int32 *) c->values)[r + 1];

						p = c->data + o0;
						len = o1 - o0;
					}
					if (n == 0 || vpq_bytes_cmp(p, len, s->bmin, s->bminlen) < 0)
					{
						s->bmin = p;
						s->bminlen = len;
					}
					if (n == 0 || vpq_bytes_cmp(p, len, s->bmax, s->bmaxlen) > 0)
					{
						s->bmax = p;
						s->bmaxlen = len;
					}
				}
				break;
			case VPQ_STATS_NONE:
				break;
		}
		n++;
	}
	s->has_minmax = n > 0;
}

/*
 * The buffered rows made a file: written, its row inserted under the
 * buffer's command id, the buffer emptied
 */
static void
buffer_flush(WriteBuffer *b)
{
	int64		nrows = b->nrows;
	int			nrg;
	int64	   *rg_rows;
	int64	   *chunk_bytes;
	VpqRawStat *raw;
	vpq_column_spec *specs;
	vpq_column_data *data;
	vpq_write_options opts;
	uint64		first_rowid;
	uint64		file_id;
	uint64		fxid;
	int64		written;
	char	   *path;
	char	   *facts_c = NULL;
	char	   *facts = NULL;
	size_t		facts_len = 0;
	VpqFileRow	row;
	MemoryContext old;
	MemoryContext tmp;

	if (nrows == 0)
		return;
	tmp = AllocSetContextCreate(CurrentMemoryContext, "vexec_parquet flush", ALLOCSET_DEFAULT_SIZES);
	old = MemoryContextSwitchTo(tmp);

	vpq_engine_ready();
	vpq_counters_take(&b->storage, 0, 1, &first_rowid, &file_id);
	fxid = U64FromFullTransactionId(GetTopFullTransactionId());
	path = vpq_file_path(b->dir, (int64) file_id, fxid);

	if (!b->dir_made)
	{
		ListCell   *lc;

		if (!b->s3)
		{
			bool		ours = false;

			VPQ_CHECK(vpq_make_dir(b->dir), psprintf("making the directory \"%s\"", b->dir));
			foreach(lc, new_storages)
			{
				VpqStorage *s = lfirst(lc);

				if (s->spc == b->storage.spc && s->relnumber == b->storage.relnumber &&
					s->backend == b->storage.backend)
					ours = true;
			}
			/* a storage this transaction made: its directory goes where it aborts */
			if (ours)
				vpq_pending_delete(b->dir, true, false);
		}
		/* the database's directory of the extension: CREATE DATABASE's template check sees it */
		{
			char	   *dbpath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);
			char	   *marker = psprintf("%s/%s/vexec_parquet", DataDir, dbpath);

			VPQ_CHECK(vpq_make_dir(marker), "making the database's vexec_parquet directory");
		}
		b->dir_made = true;
	}

	nrg = (int) ((nrows + b->row_group_rows - 1) / b->row_group_rows);
	rg_rows = palloc(sizeof(int64) * nrg);
	for (int g = 0; g < nrg; g++)
		rg_rows[g] = Min((int64) b->row_group_rows, nrows - (int64) g * b->row_group_rows);
	chunk_bytes = palloc0(sizeof(int64) * nrg * Max(b->ncols, 1));
	raw = palloc0(sizeof(VpqRawStat) * nrg * Max(b->ncols, 1));
	specs = palloc0(sizeof(vpq_column_spec) * Max(b->ncols, 1));
	data = palloc0(sizeof(vpq_column_data) * Max(b->ncols, 1));
	for (int i = 0; i < b->ncols; i++)
	{
		ColBuf	   *c = &b->cols[i];

		specs[i].name = c->col.name;
		specs[i].field_id = c->col.attnum;
		specs[i].type = c->col.ftype;
		specs[i].nullable = !c->col.notnull || c->nulls > 0;
		specs[i].json = c->col.json;
		data[i].null_count = c->nulls;
		data[i].validity = c->nulls > 0 ? c->validity : NULL;
		data[i].values = (const uint8_t *) c->values;
		data[i].values_size = c->width > 0 ? nrows * c->width :
			c->width < 0 ? (nrows + 7) / 8 : (nrows + 1) * (int64) sizeof(int32);
		data[i].data = (const uint8_t *) c->data;
		data[i].data_size = (int64) c->data_len;
		for (int g = 0; g < nrg; g++)
		{
			int64		from = (int64) g * b->row_group_rows;

			column_stats(c, from, from + rg_rows[g], &raw[g * b->ncols + i]);
		}
	}
	/*
	 * A table of no column: a Parquet row group of no column keeps no rows,
	 * so the file gets a column of NULLs no attribute is, field id 0
	 */
	if (b->ncols == 0)
	{
		Size		bits = (nrows + 7) / 8 + 8;
		uint8	   *none = palloc0(bits);

		specs = palloc0(sizeof(vpq_column_spec));
		data = palloc0(sizeof(vpq_column_data));
		specs[0].name = "_vpq_rows";
		specs[0].field_id = 0;
		specs[0].type.id = VPQ_T_BOOL;
		specs[0].nullable = true;
		data[0].null_count = nrows;
		data[0].validity = none;
		data[0].values = none;
		data[0].values_size = (nrows + 7) / 8;
	}
	memset(&opts, 0, sizeof(opts));
	opts.codec = b->codec;
	opts.level = b->level;
	opts.row_group_rows = b->row_group_rows;
	opts.page_bytes = b->page_bytes;
	opts.dictionary = true;
	opts.page_index = true;
	opts.sync = vpq_sync;

	/* where the transaction aborts, its file goes with it */
	vpq_pending_delete(path, false, false);
	VPQ_CHECK(vpq_write_file(path, Max(b->ncols, 1), specs, data, nrows, &opts, &written, chunk_bytes,
							 &facts_c, &facts_len),
			  psprintf("writing \"%s\"", path));
	if (facts_c != NULL)
	{
		facts = palloc(Max(facts_len, 1));
		memcpy(facts, facts_c, facts_len);
		free(facts_c);
	}

	memset(&row, 0, sizeof(row));
	row.file_id = (int64) file_id;
	row.first_rowid = b->first_rowid;
	row.rows = nrows;
	row.bytes = written;
	row.fxid = (int64) fxid;
	{
		AttrNumber *attnums = palloc(sizeof(AttrNumber) * Max(b->ncols, 1));
		VpqKind    *kinds = palloc(sizeof(VpqKind) * Max(b->ncols, 1));

		for (int i = 0; i < b->ncols; i++)
		{
			attnums[i] = b->cols[i].col.attnum;
			kinds[i] = b->cols[i].image ? VPQ_K_NUMERIC_ANY : b->cols[i].col.kind;
		}
		/* and the facts a device unit asks of the file's footer, which none need read (PQ1) */
		row.stats = vpq_stats_build(nrg, rg_rows, b->ncols, attnums, kinds, raw, chunk_bytes,
									facts, facts_len);
	}
	vpq_file_insert(&b->storage, &row, b->cid);

	MemoryContextSwitchTo(old);
	MemoryContextDelete(tmp);

	/* emptied: the next rows' ids follow on in the range taken */
	for (int i = 0; i < b->ncols; i++)
	{
		ColBuf	   *c = &b->cols[i];
		Size		vbytes = (b->cap + 7) / 8 + 8;

		if (c->image)
			column_from_image(b, c);
		memset(c->validity, 0, vbytes);
		memset(c->values, 0, c->values_cap);
		c->data_len = 0;
		c->nulls = 0;
	}
	b->nrows = 0;
	b->bytes = 0;
	b->first_rowid = b->next_rowid;
}

/* Whether the transaction has buffered rows to make files of */
bool
vpq_write_pending(void)
{
	ListCell   *lc;

	foreach(lc, buffers)
		if (((WriteBuffer *) lfirst(lc))->nrows > 0)
			return true;
	return false;
}

void
vpq_buffer_flush_rel(Oid relid)
{
	WriteBuffer *b = find_buffer(relid);

	if (b != NULL)
		buffer_flush(b);
}

void
vpq_buffer_flush_all(void)
{
	ListCell   *lc;

	foreach(lc, buffers)
		buffer_flush((WriteBuffer *) lfirst(lc));
}

/* A scan's snapshot would see buffered rows of the relation: their file first */
void
vpq_buffer_flush_visible(Relation rel, Snapshot snapshot)
{
	WriteBuffer *b = find_buffer(RelationGetRelid(rel));

	if (b == NULL || b->nrows == 0)
		return;
	if (snapshot == NULL || !IsMVCCSnapshot(snapshot) || snapshot->curcid > b->cid)
		buffer_flush(b);
}

void
vpq_buffer_discard_rel(Oid relid)
{
	WriteBuffer *b = find_buffer(relid);

	if (b != NULL)
		drop_buffer(b);
}

/* Whether a row is in the buffer, and the command id it was inserted under */
bool
vpq_buffer_row_cid(Relation rel, uint64 rowid, CommandId *cid)
{
	WriteBuffer *b = find_buffer(RelationGetRelid(rel));

	if (b == NULL || b->nrows == 0 || rowid < b->first_rowid || rowid >= b->first_rowid + (uint64) b->nrows)
		return false;
	*cid = b->cid;
	return true;
}

/* Whether a row is in the buffer, the running transaction's, and the snapshot sees its command */
bool
vpq_buffer_has_row(Relation rel, uint64 rowid, Snapshot snapshot)
{
	WriteBuffer *b = find_buffer(RelationGetRelid(rel));

	if (b == NULL || b->nrows == 0 || rowid < b->first_rowid || rowid >= b->first_rowid + (uint64) b->nrows)
		return false;
	return !(snapshot != NULL && IsMVCCSnapshot(snapshot) && snapshot->curcid <= b->cid);
}

/*
 * A row of the buffer by its row id, as the snapshot would see it: the
 * running transaction's rows of a command before the snapshot's, or any
 * snapshot that is not MVCC's
 */
bool
vpq_buffer_fetch(Relation rel, uint64 rowid, Snapshot snapshot, TupleTableSlot *slot)
{
	WriteBuffer *b = find_buffer(RelationGetRelid(rel));
	TupleDesc	desc = RelationGetDescr(rel);
	Datum	   *values;
	bool	   *nulls;
	int64		row;
	HeapTuple	tuple;

	if (b == NULL || b->nrows == 0 || rowid < b->first_rowid || rowid >= b->first_rowid + (uint64) b->nrows)
		return false;
	if (snapshot != NULL && IsMVCCSnapshot(snapshot) && snapshot->curcid <= b->cid)
		return false;
	row = (int64) (rowid - b->first_rowid);
	values = palloc0(sizeof(Datum) * Max(desc->natts, 1));
	nulls = palloc(sizeof(bool) * Max(desc->natts, 1));
	memset(nulls, true, sizeof(bool) * Max(desc->natts, 1));
	for (int i = 0, k = 0; i < desc->natts; i++)
	{
		ColBuf	   *c;
		vpq_array	a;
		VpqReadCol	rc;

		if (TupleDescAttr(desc, i)->attisdropped)
			continue;
		c = &b->cols[k++];
		memset(&a, 0, sizeof(a));
		a.type = c->col.ftype;
		a.length = b->nrows;
		a.null_count = c->nulls;
		a.validity = c->nulls > 0 ? c->validity : NULL;
		a.values = (const uint8_t *) c->values;
		a.data = (const uint8_t *) c->data;
		a.data_size = (int64) c->data_len;
		vpq_read_col_own(&c->col, &rc);
		values[i] = vpq_value_at(&rc, &a, row, &nulls[i]);
	}
	tuple = heap_form_tuple(desc, values, nulls);
	ExecForceStoreHeapTuple(tuple, slot, true);
	vpq_set_tid(&slot->tts_tid, rowid);
	slot->tts_tableOid = RelationGetRelid(rel);
	vpq_slot_set_xmin(slot, GetCurrentTransactionIdIfAny());
	return true;
}

/*
 * vexec's sink: the layout each attribute's values come in
 */
bool
vpq_sink_layout(VpqTable *t, AttrNumber attnum, VexecSinkLayout *layout)
{
	VpqColumn  *col = &t->cols[attnum - 1];

	memset(layout, 0, sizeof(*layout));
	switch (col->kind)
	{
		case VPQ_K_DROPPED:
			layout->layout = VEXEC_FIXED;
			layout->width = layout->stride = 1;
			return true;
		case VPQ_K_BOOL:
			layout->layout = VEXEC_BYTE_BOOL;
			layout->width = layout->stride = 1;
			return true;
		case VPQ_K_CHAR:
		case VPQ_K_INT2:
		case VPQ_K_INT4:
		case VPQ_K_INT8:
		case VPQ_K_FLOAT4:
		case VPQ_K_FLOAT8:
		case VPQ_K_OID:
		case VPQ_K_DATE:
		case VPQ_K_TIME:
		case VPQ_K_TIMESTAMP:
		case VPQ_K_TIMESTAMPTZ:
		case VPQ_K_UUID:
		case VPQ_K_FIXED:
			layout->layout = VEXEC_FIXED;
			layout->width = layout->stride = col->typlen;
			return true;
		case VPQ_K_NUMERIC:
			layout->layout = VEXEC_SCALED;
			layout->width = layout->stride = 16;
			layout->scale = col->scale;
			return true;
		case VPQ_K_TEXT:
		case VPQ_K_BPCHAR:
		case VPQ_K_BYTEA:
		case VPQ_K_JSON:
			layout->layout = VEXEC_OFFSETS;
			return true;
		case VPQ_K_NUMERIC_ANY:
		case VPQ_K_VARLENA:
			layout->layout = VEXEC_DATUM;
			return true;
	}
	return false;
}

static inline bool
batch_valid(const VexecColumn *vc, int i)
{
	return vc->validity == NULL || (vc->validity[i >> 6] >> (i & 63)) & 1;
}

/* rows [from, from + n) of a sink's batch into the buffer at its row `row` */
static void
put_batch_rows(WriteBuffer *b, Relation rel, const VexecColumn *columns, int from, int n)
{
	TupleDesc	desc = RelationGetDescr(rel);
	int64		row0 = b->nrows;

	grow(b, n);
	for (int i = 0, k = 0; i < desc->natts; i++)
	{
		const VexecColumn *vc = &columns[i];
		ColBuf	   *c;
		VpqColumn  *col;

		if (TupleDescAttr(desc, i)->attisdropped)
			continue;
		c = &b->cols[k++];
		col = &c->col;
		for (int j = 0; j < n; j++)
		{
			int			src = from + j;
			int64		row = row0 + j;
			char	   *at = c->width > 0 ? c->values + (Size) row * c->width : NULL;

			if (!batch_valid(vc, src))
			{
				put_null(c, row);
				continue;
			}
			set_valid(c, row);
			switch (col->kind)
			{
				case VPQ_K_BOOL:
					if (((const uint8 *) vc->values)[src])
						((uint8 *) c->values)[row >> 3] |= (uint8) (1 << (row & 7));
					break;
				case VPQ_K_DATE:
					{
						int32		v = ((const int32 *) vc->values)[src];

						*(int32 *) at = DATE_NOT_FINITE(v) ? v : v + VPQ_EPOCH_DAYS;
					}
					break;
				case VPQ_K_TIMESTAMP:
				case VPQ_K_TIMESTAMPTZ:
					{
						int64		v = ((const int64 *) vc->values)[src];

						if (!TIMESTAMP_NOT_FINITE(v))
						{
							if (v > PG_INT64_MAX - VPQ_EPOCH_USECS)
								ereport(ERROR,
										(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
										 errmsg("timestamp out of range for a Parquet file of column \"%s\"",
												col->name)));
							v += VPQ_EPOCH_USECS;
						}
						*(int64 *) at = v;
					}
					break;
				case VPQ_K_NUMERIC:
					if (c->image)
					{
						int128		v;

						memcpy(&v, (const char *) vc->values + (Size) src * 16, 16);
						put_datum(b, c, row, vpq_int128_to_numeric(v, col->scale));
					}
					else
						memcpy(at, (const char *) vc->values + (Size) src * 16, 16);
					break;
				case VPQ_K_TEXT:
				case VPQ_K_BPCHAR:
				case VPQ_K_BYTEA:
				case VPQ_K_JSON:
					{
						const int32 *off = (const int32 *) vc->values;
						const char *bytes = (const char *) vc->buffers[0] + off[src];
						int			len = off[src + 1] - off[src];

						/* text a sink hands is checked as PostgreSQL's input checks it */
						if (col->kind != VPQ_K_BYTEA)
							(void) pg_verifymbstr(bytes, len, false);
						data_append(b, c, bytes, len);
						check_data_size(c);
						end_value(c, row);
					}
					break;
				case VPQ_K_NUMERIC_ANY:
				case VPQ_K_VARLENA:
					put_datum(b, c, row, ((const Datum *) vc->values)[src]);
					break;
				default:
					memcpy(at, (const char *) vc->values + (Size) src * vc->stride, c->width);
					break;
			}
		}
	}
	b->nrows += n;
	b->bytes += (Size) n * 16;
}

void
vpq_buffer_put_batch(Relation rel, CommandId cid, int nrows, const VexecColumn *columns,
					 ItemPointerData *tids)
{
	WriteBuffer *b = buffer_for(rel, cid);
	int			done = 0;

	while (done < nrows)
	{
		uint64		first = take_rowids(b, nrows - done);
		int			n = (int) Min((uint64) (nrows - done), b->end_rowid - b->next_rowid);

		put_batch_rows(b, rel, columns, done, n);
		if (tids)
			for (int j = 0; j < n; j++)
				vpq_set_tid(&tids[done + j], first + j);
		b->next_rowid += n;
		done += n;
		after_rows(b);
	}
	pgstat_count_heap_insert(rel, nrows);
}

/*
 * Deletions at the transaction's end
 */
void
vpq_pending_delete(const char *path, bool dir, bool at_commit)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
	PendingDelete *p = palloc(sizeof(PendingDelete));

	p->path = pstrdup(path);
	p->dir = dir;
	p->at_commit = at_commit;
	p->subid = GetCurrentSubTransactionId();
	pending = lappend(pending, p);
	MemoryContextSwitchTo(old);
}

/* A storage this transaction made */
void
vpq_note_new_storage(const VpqStorage *st)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);
	VpqStorage *copy = palloc(sizeof(VpqStorage));

	*copy = *st;
	new_storages = lappend(new_storages, copy);
	MemoryContextSwitchTo(old);
}

/* A storage dropped: its rows now, its directory where the transaction commits */
void
vpq_storage_drop_later(const VpqStorage *st, const char *dir)
{
	vpq_storage_forget(st);
	if (dir != NULL)
		vpq_pending_delete(dir, true, true);
}

static void
do_delete(PendingDelete *p)
{
	int			rc;

	/* an error here is a warning: the files are left to VACUUM */
	PG_TRY();
	{
		vpq_engine_ready();
		rc = p->dir ? vpq_delete_dir(p->path) : vpq_delete_file(p->path);
		if (rc != VPQ_OK && rc != VPQ_NOT_FOUND)
			ereport(WARNING,
					(errmsg("vexec_parquet: could not delete \"%s\": %s", p->path, vpq_error())));
		/* a database directory's vexec_parquet directory left empty goes too: DROP TABLESPACE needs it gone */
		if (p->dir && p->path[0] == '/')
		{
			char	   *parent = pstrdup(p->path);
			char	   *slash = strrchr(parent, '/');

			if (slash != NULL && slash != parent)
			{
				*slash = '\0';
				if (strlen(parent) > 14 && strcmp(parent + strlen(parent) - 14, "/vexec_parquet") == 0)
					(void) rmdir(parent);
			}
			pfree(parent);
		}
	}
	PG_CATCH();
	{
		FlushErrorState();
		ereport(WARNING,
				(errmsg("vexec_parquet: could not delete \"%s\"", p->path)));
	}
	PG_END_TRY();
}

static void
run_pending(bool committed)
{
	ListCell   *lc;
	List	   *todo = pending;

	pending = NIL;
	foreach(lc, todo)
	{
		PendingDelete *p = lfirst(lc);

		if (p->at_commit == committed)
			do_delete(p);
	}
}

static void
write_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
		case XACT_EVENT_PARALLEL_PRE_COMMIT:
		case XACT_EVENT_PRE_PREPARE:
			/* module.c's callback makes the files, under a snapshot */
			break;
		case XACT_EVENT_COMMIT:
		case XACT_EVENT_PARALLEL_COMMIT:
			run_pending(true);
			buffers = NIL;
			new_storages = NIL;
			break;
		case XACT_EVENT_ABORT:
		case XACT_EVENT_PARALLEL_ABORT:
			run_pending(false);
			buffers = NIL;
			new_storages = NIL;
			break;
		case XACT_EVENT_PREPARE:
			/* a prepared transaction's files stay: COMMIT PREPARED or ROLLBACK PREPARED leaves them to VACUUM */
			pending = NIL;
			buffers = NIL;
			new_storages = NIL;
			break;
	}
}

static void
write_subxact_callback(SubXactEvent event, SubTransactionId mySubid, SubTransactionId parentSubid,
					   void *arg)
{
	ListCell   *lc;

	switch (event)
	{
		case SUBXACT_EVENT_PRE_COMMIT_SUB:
			foreach(lc, buffers)
			{
				WriteBuffer *b = lfirst(lc);

				if (b->subid == mySubid)
					buffer_flush(b);
			}
			break;
		case SUBXACT_EVENT_COMMIT_SUB:
			foreach(lc, buffers)
			{
				WriteBuffer *b = lfirst(lc);

				if (b->subid == mySubid)
					b->subid = parentSubid;
			}
			foreach(lc, pending)
			{
				PendingDelete *p = lfirst(lc);

				if (p->subid == mySubid)
					p->subid = parentSubid;
			}
			break;
		case SUBXACT_EVENT_ABORT_SUB:
			{
				List	   *keep = NIL;

				/* its rows gone, and their ids never given again */
				foreach(lc, buffers)
				{
					WriteBuffer *b = lfirst(lc);

					if (b->subid == mySubid && b->nrows > 0)
					{
						for (int i = 0; i < b->ncols; i++)
						{
							ColBuf	   *c = &b->cols[i];

							memset(c->validity, 0, (b->cap + 7) / 8 + 8);
							memset(c->values, 0, c->values_cap);
							c->data_len = 0;
							c->nulls = 0;
						}
						b->nrows = 0;
						b->bytes = 0;
						b->first_rowid = b->next_rowid;
					}
				}
				foreach(lc, pending)
				{
					PendingDelete *p = lfirst(lc);

					if (p->subid != mySubid)
					{
						MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

						keep = lappend(keep, p);
						MemoryContextSwitchTo(old);
					}
					else if (!p->at_commit)
						do_delete(p);
				}
				pending = keep;
			}
			break;
		case SUBXACT_EVENT_START_SUB:
			break;
	}
}

static void
vpq_ExecutorFinish(QueryDesc *queryDesc)
{
	if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);
	/* the query's rows become files as it ends */
	if (buffers != NIL)
		vpq_buffer_flush_all();
}

void
vpq_write_init(void)
{
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = vpq_ExecutorFinish;
	RegisterXactCallback(write_xact_callback, NULL);
	RegisterSubXactCallback(write_subxact_callback, NULL);
}
