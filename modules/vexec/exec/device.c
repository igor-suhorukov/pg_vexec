/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * device.c
 *	  A vector node's device task, its chunks and its answers
 *	  (exec/device.h; pg_vector_executor.md §3.11, V8).
 *
 * What crosses is each column in the device's layout (§3.11, "What
 * crosses"): by-value numbers as they are; bool a byte a value, Arrow's
 * bits unpacked as the chunk is filled; dates and timestamps in
 * PostgreSQL's epoch, an Arrow batch's shifted as they are copied, so that
 * a value read back is a Datum's bits; scaled numerics as DECIMAL64 or
 * DECIMAL128 at the typmod's scale; text, varchar and bytea as offsets and
 * bytes, a compressed or external value detoasted first, so that equal
 * values are equal bytes; a dictionary column's values at its codes; a
 * constant's value at every row.  A layout with no copy here keeps its
 * batch on the CPU, which costs time, never an answer.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/cmptype.h"
#include "access/detoast.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_type.h"
#include "datatype/timestamp.h"
#include "lib/stringinfo.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "parser/scansup.h"
#include "portability/instr_time.h"
#include "port/pg_bitutils.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "varatt.h"

#include "vexec.h"
#include "exec/device.h"
#include "expr/kernel.h"

/* A piece list's first allocation, and the bytes a buffer starts at. */
#define STREAM_PIECES	8
#define STREAM_ALIGN	64

/* ---------------------------------------------------------------------
 * The provider
 * ---------------------------------------------------------------------
 */

const VexecDeviceRoutine *
vexec_device_ready(void)
{
	const VexecDeviceRoutine *r = vexec_device_provider();

	if (r == NULL || !r->ready())
		return NULL;
	return r;
}

/* W7: a provider of minor version 1 says what its device keeps; an older one, nothing. */
double
vexec_device_kept_rows(Oid relation, AttrNumber attnum)
{
	const VexecDeviceRoutine *r = vexec_device_ready();

	if (r == NULL || r->minor < 1 || r->size <= offsetof(VexecDeviceRoutine, kept_rows) ||
		r->kept_rows == NULL)
		return 0;
	return r->kept_rows(relation, (int16) attnum);
}

/* W38: a provider of minor version 3 says what memory its tasks have not reserved; an older one, nothing. */
uint64
vexec_device_request_bytes(void)
{
	const VexecDeviceRoutine *r = vexec_device_ready();

	if (vexec_gpu_request_share <= 0 || r == NULL || r->minor < 3 ||
		r->size <= offsetof(VexecDeviceRoutine, free_bytes) || r->free_bytes == NULL)
		return PG_UINT64_MAX;
	return (uint64) Max((double) r->free_bytes() * vexec_gpu_request_share, 1.0);
}

uint64
vexec_device_plan_bytes(void)
{
	const VexecDeviceRoutine *r = vexec_device_ready();

	if (r == NULL || r->minor < 3 || r->size <= offsetof(VexecDeviceRoutine, free_bytes) || r->free_bytes == NULL)
		return PG_UINT64_MAX;
	return r->free_bytes();
}

/*
 * W46: a provider of minor version 4 applies a GROUPBY's HAVING to an answer
 * only where its pages ask for it; an older one to each, which no HAVING is
 * then given to.
 */
bool
vexec_device_having_asked(void)
{
	const VexecDeviceRoutine *r = vexec_device_ready();

	return r != NULL && r->minor >= 4;
}

/* W50: a provider of minor version 5 says what its cache keeps columns within; an older one, nothing. */
uint64
vexec_device_cache_bytes(void)
{
	const VexecDeviceRoutine *r = vexec_device_ready();

	if (r == NULL || r->minor < 5 || r->size <= offsetof(VexecDeviceRoutine, cache_bytes) || r->cache_bytes == NULL)
		return PG_UINT64_MAX;
	return r->cache_bytes();
}

double
vexec_devunit_row_bytes(Oid relation, int n, const int16 *attnums)
{
	double		bytes = 0;

	for (int c = 0; c < n; c++)
	{
		Oid			typid;
		int32		typmod;
		Oid			collid;
		int32		w;

		/* a column the device computes or makes (W10, W13): no bytes read */
		if (attnums[c] <= 0)
			continue;
		get_atttypetypmodcoll(relation, attnums[c], &typid, &typmod, &collid);
		w = get_attavgwidth(relation, attnums[c]);
		if (w <= 0)
			w = get_typavgwidth(typid, typmod);
		bytes += w + (get_typlen(typid) < 0 ? sizeof(int32) : 0);
	}
	return bytes;
}

uint64
vexec_devunit_bytes(const VexecSourceUnit *u, double row_bytes)
{
	double		w = (double) u->rows * row_bytes;

	return Max(u->bytes, (uint64) Min(w, (double) PG_UINT64_MAX / 2));
}

/*
 * Whether a node may take its device strategy as it begins (§3.11,
 * "Planned, then decided at the node's start"): vexec.gpu not off, the
 * node kind's vexec.enable_gpu_*, planned so or forced, and a provider
 * whose executor serves.  A cached plan follows a later SET this way.
 */
bool
vexec_device_wanted(bool planned, bool enabled, const char **why)
{
	const VexecDeviceRoutine *r;

	if (vexec_gpu == VEXEC_GPU_OFF)
	{
		*why = planned ? "vexec.gpu is off" : NULL;
		return false;
	}
	if (!enabled)
	{
		*why = "its vexec.enable_gpu_* is off";
		return false;
	}
	if (!planned && vexec_gpu != VEXEC_GPU_FORCE)
	{
		*why = NULL;
		return false;
	}
	r = vexec_device_provider();
	if (r == NULL)
	{
		*why = "no device provider: vexec_gpu is not loaded";
		return false;
	}
	if (!r->ready())
	{
		*why = "the device's executor does not serve";
		return false;
	}
	*why = NULL;
	return true;
}

int
vexec_dev_type_of(const VexecType *type, int16 *scale)
{
	*scale = 0;
	switch (type->basetype)
	{
		case BOOLOID:
			return VEXEC_DEV_BOOL8;
		case CHAROID:
			return VEXEC_DEV_INT8;
		case INT2OID:
			return VEXEC_DEV_INT16;
		case INT4OID:
		case DATEOID:
			return VEXEC_DEV_INT32;
		case OIDOID:
			return VEXEC_DEV_UINT32;
		case INT8OID:
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case CASHOID:
			return VEXEC_DEV_INT64;
		case FLOAT4OID:
			return VEXEC_DEV_FLOAT32;
		case FLOAT8OID:
			return VEXEC_DEV_FLOAT64;
		case NUMERICOID:
			*scale = type->scale;
			if (type->scaled_width == 8)
				return VEXEC_DEV_DECIMAL64;
			if (type->scaled_width == 16)
				return VEXEC_DEV_DECIMAL128;
			return VEXEC_DEV_NONE;
		case TEXTOID:
		case VARCHAROID:
		case BYTEAOID:
			return VEXEC_DEV_STRING;
		default:
			return VEXEC_DEV_NONE;
	}
}

const char *
vexec_dev_type_name(int type)
{
	static const char *const names[] = {
		"none", "bool8", "int8", "int16", "int32", "int64", "uint32", "float32",
		"float64", "decimal64", "decimal128", "string"
	};

	return type >= 0 && type < VEXEC_DEV_NTYPES ? names[type] : "?";
}

const char *
vexec_dev_status_name(int status)
{
	switch (status)
	{
		case VEXEC_DEV_OK:
			return "ok";
		case VEXEC_DEV_MORE:
			return "more";
		case VEXEC_DEV_PENDING:
			return "pending";
		case VEXEC_DEV_REFUSED:
			return "refused by admission";
		case VEXEC_DEV_NOMEM:
			return "out of device memory";
		case VEXEC_DEV_UNSUPPORTED:
			return "unsupported";
		case VEXEC_DEV_FAILED:
			return "failed";
		case VEXEC_DEV_LOST:
			return "the executor was lost";
		case VEXEC_DEV_CANCELLED:
			return "cancelled";
	}
	return "?";
}

/* ---------------------------------------------------------------------
 * Tasks
 * ---------------------------------------------------------------------
 */

static int
task_wait(VexecDevTask *t)
{
	instr_time	start,
				end;
	int			status;
	const VexecDevAnswer *a;

	INSTR_TIME_SET_CURRENT(start);
	status = t->r->wait(t->task, -1);
	INSTR_TIME_SET_CURRENT(end);
	INSTR_TIME_SUBTRACT(end, start);
	t->fig.wait_ms += INSTR_TIME_GET_MILLISEC(end);
	t->fig.requests++;
	a = t->r->answer(t->task);
	t->fig.device_ms += (double) a->device_ns / 1e6;
	t->fig.gpu_ms += (double) a->gpu_ns / 1e6;
	t->last_status = status;
	if (status != VEXEC_DEV_OK && status != VEXEC_DEV_MORE)
		strlcpy(t->message, a->message, sizeof(t->message));
	return status;
}

VexecDevTask *
vexec_devtask_begin(int op, const void *program, Size len, MemoryContext mcxt, const char **why)
{
	const VexecDeviceRoutine *r = vexec_device_ready();
	VexecDeviceTask *task;
	VexecDevRequest *req;
	VexecDevTask *t;
	Size		size;
	int			status;

	if (r == NULL)
	{
		*why = "the device's executor does not serve";
		return NULL;
	}
	task = r->begin();
	if (task == NULL)
	{
		*why = "the device had no free task slot";
		return NULL;
	}
	req = r->request_area(task, &size);
	if (sizeof(VexecDevRequest) + len > size)
	{
		r->end(task);
		*why = "its program is larger than a request";
		return NULL;
	}
	memset(req, 0, sizeof(VexecDevRequest));
	req->kind = VEXEC_DEV_REQ_BEGIN;
	req->op = (uint16) op;
	req->length = (uint32) (sizeof(VexecDevRequest) + len);
	memcpy(req + 1, program, len);

	t = MemoryContextAllocZero(mcxt, sizeof(VexecDevTask));
	t->r = r;
	t->task = task;
	t->mcxt = mcxt;
	t->op = op;
	t->block_size = r->block_size();
	r->submit(task);
	status = task_wait(t);
	if (status != VEXEC_DEV_OK)
	{
		*why = psprintf("the device did not take the task: %s%s%s", vexec_dev_status_name(status),
						t->message[0] ? ": " : "", t->message);
		r->end(task);
		pfree(t);
		return NULL;
	}
	t->share = r->task_share();
	return t;
}

void
vexec_devtask_end(VexecDevTask *t)
{
	if (t == NULL || t->task == NULL)
		return;
	t->r->end(t->task);
	t->task = NULL;
	t->held = 0;
	t->nunit_blocks = 0;
}

int
vexec_devtask_wait(VexecDevTask *t)
{
	return task_wait(t);
}

const VexecDevAnswer *
vexec_devtask_answer(VexecDevTask *t)
{
	return t->r->answer(t->task);
}

int
vexec_devtask_claim_answers(VexecDevTask *t, int n, uint32 *blocks)
{
	const VexecDeviceRoutine *r = t->r;

	if (r->minor >= 2 && r->size > offsetof(VexecDeviceRoutine, claim_answers) && r->claim_answers != NULL)
		return r->claim_answers(t->task, n, blocks);
	return r->claim(t->task, n, blocks);
}

int
vexec_devtask_answer_page(VexecDevTask *t, int npages, const uint32 *pages, uint32 flags)
{
	Size		size;
	VexecDevRequest *req = t->r->request_area(t->task, &size);
	VexecDevPages *p = (VexecDevPages *) (req + 1);
	int			status;

	memset(req, 0, sizeof(VexecDevRequest) + sizeof(VexecDevPages));
	req->kind = VEXEC_DEV_REQ_ANSWER;
	req->op = (uint16) t->op;
	p->npages = npages;
	p->flags = flags;
	memcpy(vexec_dev_pages_blocks(p), pages, sizeof(uint32) * npages);
	req->length = (uint32) (sizeof(VexecDevRequest) + sizeof(VexecDevPages) + sizeof(uint32) * npages);
	t->r->submit(t->task);
	status = task_wait(t);
	if (status == VEXEC_DEV_OK || status == VEXEC_DEV_MORE)
	{
		const VexecDevAnswer *a = t->r->answer(t->task);

		t->fig.answers++;
		t->fig.answer_rows += a->nrows;
		for (int i = 0; i < a->ncols; i++)
		{
			const VexecDevAnsCol *c = &vexec_dev_answer_cols((VexecDevAnswer *) a)[i];

			t->fig.answer_bytes += c->values.length + c->validity.length + c->offsets.length;
		}
	}
	return status;
}

const char *
vexec_devans_values(VexecDevTask *t, const VexecDevAnsCol *c)
{
	return c->values.length == 0 ? NULL : t->r->block(c->values.block) + c->values.offset;
}

const uint64 *
vexec_devans_validity(VexecDevTask *t, const VexecDevAnsCol *c)
{
	return c->validity.length == 0 ? NULL :
		(const uint64 *) (t->r->block(c->validity.block) + c->validity.offset);
}

const int32 *
vexec_devans_offsets(VexecDevTask *t, const VexecDevAnsCol *c)
{
	return c->offsets.length == 0 ? NULL :
		(const int32 *) (t->r->block(c->offsets.block) + c->offsets.offset);
}

/* ---------------------------------------------------------------------
 * Chunks: streams of pieces within the chunk's blocks
 * ---------------------------------------------------------------------
 */

static void
stream_init(VexecDevFill *f, VexecDevStream *s)
{
	s->maxpieces = STREAM_PIECES;
	s->pieces = MemoryContextAlloc(f->task->mcxt, sizeof(VexecDevPiece) * s->maxpieces);
	s->npieces = 0;
	s->length = 0;
	s->ptr = NULL;
	s->room = 0;
	s->next = 0;
}

static void
stream_add_piece(VexecDevFill *f, VexecDevStream *s, uint32 block, uint32 off, uint32 len)
{
	VexecDevPiece *p;

	if (s->npieces == s->maxpieces)
	{
		s->maxpieces *= 2;
		s->pieces = repalloc(s->pieces, sizeof(VexecDevPiece) * s->maxpieces);
	}
	p = &s->pieces[s->npieces++];
	p->block = block;
	p->offset = off;
	p->length = len;
	p->pad = 0;
}

/* More blocks for the chunk, within its task's share: how many it got. */
static int
fill_claim(VexecDevFill *f, int n)
{
	VexecDevTask *t = f->task;
	int			got;

	n = Min(n, t->share - t->held);
	if (n <= 0)
		return 0;
	if (f->nblocks + n > f->maxblocks)
	{
		f->maxblocks = Max(f->maxblocks * 2, f->nblocks + n);
		f->blocks = repalloc(f->blocks, sizeof(uint32) * f->maxblocks);
	}
	got = t->r->claim(t->task, n, f->blocks + f->nblocks);
	f->nblocks += got;
	t->held += got;
	return got;
}


/*
 * len bytes cut from the chunk's blocks for the stream, as its pieces,
 * the first at an aligned offset: false where the blocks are out.
 */
static bool
fill_cut(VexecDevFill *f, VexecDevStream *s, uint64 len, bool align)
{
	uint32		bs = f->task->block_size;

	if (align && f->bump_off % STREAM_ALIGN != 0)
	{
		f->bump_off = TYPEALIGN(STREAM_ALIGN, f->bump_off);
		if (f->bump_off >= bs)
		{
			f->bump++;
			f->bump_off = 0;
		}
	}
	while (len > 0)
	{
		uint32		take;

		if (f->bump >= f->nblocks - f->spare)
			return false;
		take = (uint32) Min((uint64) (bs - f->bump_off), len);
		stream_add_piece(f, s, f->blocks[f->bump], f->bump_off, take);
		f->bump_off += take;
		len -= take;
		if (f->bump_off == bs)
		{
			f->bump++;
			f->bump_off = 0;
		}
	}
	return true;
}

/*
 * A string column's bytes take whole blocks of their own, from the spare
 * blocks vexec_devfill_room() claimed ahead of a batch, so that no write
 * runs out of room part way.
 */
static void
stream_grow(VexecDevFill *f, VexecDevStream *s)
{
	uint32		b;

	if (f->spare == 0)
		elog(ERROR, "vexec: a device chunk's string bytes past the room claimed for them");
	b = f->blocks[f->nblocks - f->spare];
	f->spare--;
	stream_add_piece(f, s, b, 0, f->task->block_size);
}

static void
stream_write(VexecDevFill *f, VexecDevStream *s, const void *src, uint64 len, bool grow)
{
	const char *p = src;

	while (len > 0)
	{
		uint32		take;

		if (s->room == 0)
		{
			VexecDevPiece *pc;

			if (s->next >= s->npieces)
			{
				if (!grow)
					elog(ERROR, "vexec: a device chunk's buffer written past its pieces");
				stream_grow(f, s);
			}
			pc = &s->pieces[s->next++];
			s->ptr = f->task->r->block(pc->block) + pc->offset;
			s->room = pc->length;
		}
		take = (uint32) Min((uint64) s->room, len);
		memcpy(s->ptr, p, take);
		s->ptr += take;
		s->room -= take;
		s->length += take;
		p += take;
		len -= take;
	}
}

/* The validity bit of the next row. */
static inline void
validity_push(VexecDevFill *f, VexecDevFillCol *c, bool valid)
{
	if (valid)
		c->vword |= UINT64CONST(1) << c->vbits;
	else
		c->nulls = true;
	if (++c->vbits == 64)
	{
		stream_write(f, &c->validity, &c->vword, sizeof(uint64), false);
		c->vword = 0;
		c->vbits = 0;
	}
}

VexecDevFill *
vexec_devfill_begin(VexecDevTask *t, int ncols, const VexecDevColType *types, uint32 cap, int reserve)
{
	MemoryContext old = MemoryContextSwitchTo(t->mcxt);
	VexecDevFill *f = palloc0(sizeof(VexecDevFill));
	uint64		row_bytes = 0;
	uint64		fixed;
	uint64		bs = t->block_size;
	int			avail = t->share - t->held - reserve;
	int			need;
	bool		strings = false;

	f->task = t;
	f->ncols = ncols;
	f->cols = palloc0_array(VexecDevFillCol, Max(ncols, 1));
	for (int i = 0; i < ncols; i++)
	{
		VexecDevFillCol *c = &f->cols[i];

		c->type = types[i].type;
		c->scale = types[i].scale;
		c->width = (uint8) vexec_dev_type_width(c->type);
		row_bytes += c->type == VEXEC_DEV_STRING ? sizeof(int32) : c->width;
		strings |= c->type == VEXEC_DEV_STRING;
	}

	/*
	 * The rows the blocks the share leaves can hold: each buffer's bytes,
	 * its validity's, each at an aligned start; a block more for strings'
	 * bytes, which grow as they come.
	 */
	for (;;)
	{
		fixed = (row_bytes + (uint64) ncols / 8 + 1) * cap + (uint64) ncols * 4 * STREAM_ALIGN + 8;
		need = (int) ((fixed + bs - 1) / bs) + (strings ? 1 : 0);
		if (need <= avail || cap <= VEXEC_BATCH_ROWS)
			break;
		cap = Max((uint32) VEXEC_BATCH_ROWS,
				  (uint32) ((double) cap * Max(avail - (strings ? 1 : 0), 0) * bs / fixed) / VEXEC_BATCH_ROWS * VEXEC_BATCH_ROWS);
	}
	f->maxblocks = Max(need, 4);
	f->blocks = palloc(sizeof(uint32) * f->maxblocks);
	if (need > avail || fill_claim(f, need) < need)
	{
		MemoryContextSwitchTo(old);
		vexec_devfill_free(f);
		return NULL;
	}
	f->cap = cap;
	f->first_row = t->rows_given;
	for (int i = 0; i < ncols; i++)
	{
		VexecDevFillCol *c = &f->cols[i];
		bool		ok = true;

		stream_init(f, &c->values);
		stream_init(f, &c->validity);
		stream_init(f, &c->offsets);
		if (c->type == VEXEC_DEV_STRING)
		{
			int32		zero = 0;

			ok = fill_cut(f, &c->offsets, ((uint64) cap + 1) * sizeof(int32), true);
			if (ok)
				stream_write(f, &c->offsets, &zero, sizeof(int32), false);
		}
		else
			ok = fill_cut(f, &c->values, (uint64) cap * c->width, true);
		if (ok)
			ok = fill_cut(f, &c->validity, ((uint64) cap + 63) / 64 * 8, true);
		if (!ok)
		{
			MemoryContextSwitchTo(old);
			vexec_devfill_free(f);
			return NULL;
		}
	}
	MemoryContextSwitchTo(old);
	return f;
}

void
vexec_devfill_free(VexecDevFill *f)
{
	if (f == NULL)
		return;
	if (f->nblocks > 0 && f->task->task != NULL)
	{
		f->task->r->release(f->task->task, f->nblocks, f->blocks);
		f->task->held -= f->nblocks;
	}
	f->nblocks = 0;
	for (int i = 0; i < f->ncols; i++)
	{
		VexecDevFillCol *c = &f->cols[i];

		if (c->values.pieces)
			pfree(c->values.pieces);
		if (c->validity.pieces)
			pfree(c->validity.pieces);
		if (c->offsets.pieces)
			pfree(c->offsets.pieces);
	}
	pfree(f->cols);
	pfree(f->blocks);
	pfree(f);
}

/* A detoasted varlena's bytes, the work batch's memory holding them. */
static inline void
plain_bytes(Datum d, const char **p, Size *len)
{
	varlena    *vl = (varlena *) DatumGetPointer(d);

	if (VARATT_IS_EXTERNAL(vl) || VARATT_IS_COMPRESSED(vl))
		vl = (varlena *) detoast_attr(vl);
	*p = VARDATA_ANY(vl);
	*len = VARSIZE_ANY_EXHDR(vl);
}

/* Value i's bytes of a string column of any layout, detoasted. */
static inline void
string_value(const VexecVec *v, int i, const char **p, Size *len)
{
	if (v->shape.layout == VEXEC_DATUM)
		plain_bytes(((const Datum *) v->values)[i], p, len);
	else
		vexec_vec_value_bytes(v, i, p, len);
}

/* A value's bytes as a STRING column takes them, its raw size for a toasted one. */
static inline Size
string_size(const VexecVec *v, int i)
{
	if (v->shape.layout == VEXEC_DATUM)
	{
		varlena    *vl = (varlena *) DatumGetPointer(((const Datum *) v->values)[i]);

		if (VARATT_IS_EXTERNAL(vl) || VARATT_IS_COMPRESSED(vl))
			return toast_raw_datum_size(PointerGetDatum(vl)) - VARHDRSZ;
		return VARSIZE_ANY_EXHDR(vl);
	}
	else
	{
		const char *p;
		Size		len;

		vexec_vec_value_bytes(v, i, &p, &len);
		return len;
	}
}

uint64
vexec_devfill_string_bytes(const VexecVec *v, const uint64 *sel, int nrows)
{
	uint64		n = 0;

	if (v->encoding == VEXEC_CONST)
		return vexec_vec_isnull(v, 0) ? 0 :
			(uint64) string_size(v, 0) * vexec_bits_count(sel, nrows);
	VEXEC_FOREACH_ROW(sel, nrows, i)
	{
		if (vexec_vec_isnull(v, i))
			continue;
		if (v->encoding == VEXEC_DICT)
			n += string_size(v->dictionary, v->codes[i]);
		else
			n += string_size(v, i);
	}
	return n;
}

bool
vexec_devfill_room(VexecDevFill *f, int n, const uint64 *strings)
{
	uint64		bs = f->task->block_size;
	int			need = 0;

	if (f->nrows + (uint32) n > f->cap)
		return false;
	if (strings == NULL)
		return true;
	for (int i = 0; i < f->ncols; i++)
	{
		VexecDevFillCol *c = &f->cols[i];
		uint64		room;

		if (c->type != VEXEC_DEV_STRING)
			continue;
		/* offsets are int32: a chunk's bytes below 2 GB */
		if ((uint64) c->strlen + strings[i] >= (uint64) PG_INT32_MAX)
			return false;
		room = c->values.room;
		for (int k = c->values.next; k < c->values.npieces; k++)
			room += c->values.pieces[k].length;
		if (strings[i] > room)
			need += (int) ((strings[i] - room + bs - 1) / bs);
	}
	if (need > f->spare)
	{
		int			want = need - f->spare;
		int			got = fill_claim(f, want);

		f->spare += got;
		if (got < want)
			return false;
	}
	return true;
}

/* Whether a vector's layout is one the column's type is copied from here. */
bool
vexec_devfill_can_append(const VexecDevFill *f, int col, const VexecVec *v)
{
	const VexecDevFillCol *c = &f->cols[col];
	const VexecVec *d = v->encoding == VEXEC_DICT ? v->dictionary : v;

	switch (c->type)
	{
		case VEXEC_DEV_BOOL8:
			return d->shape.layout == VEXEC_BYTE_BOOL || d->shape.layout == VEXEC_BIT_BOOL;
		case VEXEC_DEV_DECIMAL64:
		case VEXEC_DEV_DECIMAL128:
			return d->shape.layout == VEXEC_SCALED && d->shape.width == c->width &&
				d->shape.stride == c->width && d->shape.scale == c->scale;
		case VEXEC_DEV_STRING:
			return (d->shape.layout == VEXEC_DATUM || d->shape.layout == VEXEC_VIEW ||
					d->shape.layout == VEXEC_OFFSETS) && d->type->typlen == -1;
		case VEXEC_DEV_NONE:
			return false;
		default:
			return d->shape.layout == VEXEC_FIXED && d->shape.width == c->width &&
				d->shape.stride == c->width &&
				(!d->shape.arrow_values || d->type->tclass == VEXEC_TC_DATE ||
				 d->type->tclass == VEXEC_TC_TIMESTAMP);
	}
}

/* A fixed-width value of row i of d into dst, in the device's layout. */
static inline void
fixed_value(const VexecDevFillCol *c, const VexecVec *d, int i, char *dst)
{
	const char *src = (const char *) d->values + (Size) i * d->shape.stride;

	if (c->type == VEXEC_DEV_BOOL8)
	{
		*dst = d->shape.layout == VEXEC_BIT_BOOL ? (char) vexec_bit((const uint64 *) d->values, i) :
			(char) (*src != 0);
		return;
	}
	memcpy(dst, src, c->width);
	if (d->shape.arrow_values)
	{
		/* an Arrow date's or timestamp's epoch made PostgreSQL's */
		if (d->type->tclass == VEXEC_TC_DATE)
		{
			int32		v;

			memcpy(&v, dst, 4);
			if (!DATE_NOT_FINITE(v))
				v -= (int32) VEXEC_EPOCH_DAYS;
			memcpy(dst, &v, 4);
		}
		else
		{
			int64		v;

			memcpy(&v, dst, 8);
			if (!TIMESTAMP_NOT_FINITE(v))
				v -= VEXEC_EPOCH_USECS;
			memcpy(dst, &v, 8);
		}
	}
}

bool
vexec_devfill_append(VexecDevFill *f, int col, VexecBatch *work, const VexecVec *v,
					 const uint64 *sel, int nrows, int nsel)
{
	VexecDevFillCol *c = &f->cols[col];
	const VexecVec *d = v->encoding == VEXEC_DICT ? v->dictionary : v;
	char		buf[VEXEC_BATCH_ROWS * 16];

	(void) work;
	if (!vexec_devfill_can_append(f, col, v))
		return false;
	if (c->type == VEXEC_DEV_STRING)
	{
		VEXEC_FOREACH_ROW(sel, nrows, i)
		{
			bool		isnull = vexec_vec_isnull(v, i);

			if (!isnull)
			{
				const char *p;
				Size		len;
				int			at = v->encoding == VEXEC_CONST ? 0 :
					v->encoding == VEXEC_DICT ? v->codes[i] : i;

				string_value(d, at, &p, &len);
				stream_write(f, &c->values, p, len, true);
				c->strlen += (uint32) len;
			}
			stream_write(f, &c->offsets, &c->strlen, sizeof(int32), false);
			validity_push(f, c, !isnull);
		}
		return true;
	}

	/* fixed width: gathered a batch at a time, then written at once */
	if (v->encoding == VEXEC_FLAT && d->shape.layout == VEXEC_FIXED && !d->shape.arrow_values &&
		nsel == nrows)
		stream_write(f, &c->values, d->values, (uint64) nrows * c->width, false);
	else
	{
		int			n = 0;

		VEXEC_FOREACH_ROW(sel, nrows, i)
		{
			int			at = v->encoding == VEXEC_CONST ? 0 :
				v->encoding == VEXEC_DICT ? v->codes[i] : i;

			if (vexec_vec_isnull(v, i))
				memset(buf + n * c->width, 0, c->width);
			else
				fixed_value(c, d, at, buf + n * c->width);
			n++;
		}
		Assert(n == nsel);
		stream_write(f, &c->values, buf, (uint64) n * c->width, false);
	}
	if (v->validity == NULL)
	{
		for (int k = 0; k < nsel; k++)
			validity_push(f, c, true);
	}
	else
	{
		VEXEC_FOREACH_ROW(sel, nrows, i)
			validity_push(f, c, !vexec_vec_isnull(v, i));
	}
	return true;
}

bool
vexec_devfill_append_true(VexecDevFill *f, int col, const VexecVec *v, const uint64 *sel,
						  int nrows, int nsel)
{
	VexecDevFillCol *c = &f->cols[col];
	char		buf[VEXEC_BATCH_ROWS] = {0};
	int			n = 0;

	if (c->type != VEXEC_DEV_BOOL8 ||
		(v->shape.layout != VEXEC_BIT_BOOL && v->shape.layout != VEXEC_BYTE_BOOL) ||
		v->encoding == VEXEC_DICT)
		return false;
	VEXEC_FOREACH_ROW(sel, nrows, i)
	{
		int			at = v->encoding == VEXEC_CONST ? 0 : i;
		bool		t = !vexec_vec_isnull(v, i) &&
			(v->shape.layout == VEXEC_BIT_BOOL ? vexec_bit((const uint64 *) v->values, at) :
			 ((const char *) v->values)[at] != 0);

		buf[n++] = t ? 1 : 0;
	}
	Assert(n == nsel);
	stream_write(f, &c->values, buf, n, false);
	for (int k = 0; k < nsel; k++)
		validity_push(f, c, true);
	return true;
}

void
vexec_devfill_put(VexecDevFill *f, int col, const void *p, uint32 len, bool isnull)
{
	VexecDevFillCol *c = &f->cols[col];

	if (c->type == VEXEC_DEV_STRING)
	{
		if (!isnull && len > 0)
		{
			stream_write(f, &c->values, p, len, true);
			c->strlen += len;
		}
		stream_write(f, &c->offsets, &c->strlen, sizeof(int32), false);
	}
	else
	{
		static const char zero[16] = {0};

		stream_write(f, &c->values, isnull ? zero : p, c->width, false);
	}
	validity_push(f, c, !isnull);
}

void
vexec_devfill_rows(VexecDevFill *f, int n)
{
	f->nrows += n;
}

/* A stream's pieces into the request's, its first `length` bytes alone. */
static void
put_buf(const VexecDevStream *s, uint64 length, VexecDevBuf *b, VexecDevPiece *pieces, uint32 *np)
{
	uint64		left = length;

	b->first = *np;
	b->npieces = 0;
	b->length = length;
	for (int i = 0; i < s->npieces && left > 0; i++)
	{
		VexecDevPiece p = s->pieces[i];

		if (p.length > left)
			p.length = (uint32) left;
		pieces[(*np)++] = p;
		b->npieces++;
		left -= p.length;
	}
}

void
vexec_devfill_submit(VexecDevFill *f)
{
	vexec_devfill_submit_side(f, 0);
}

void
vexec_devfill_submit_side(VexecDevFill *f, int side)
{
	VexecDevTask *t = f->task;
	Size		size;
	VexecDevRequest *req = t->r->request_area(t->task, &size);
	VexecDevChunk *chunk = (VexecDevChunk *) (req + 1);
	VexecDevCol *cols;
	VexecDevPiece *pieces;
	uint32		np = 0;
	uint64		maxpieces;
	uint64		bytes = 0;

	/* the validity bits of the last word */
	for (int i = 0; i < f->ncols; i++)
	{
		VexecDevFillCol *c = &f->cols[i];

		if (c->vbits > 0)
		{
			stream_write(f, &c->validity, &c->vword, sizeof(uint64), false);
			c->vword = 0;
			c->vbits = 0;
		}
	}

	memset(req, 0, sizeof(VexecDevRequest) + sizeof(VexecDevChunk));
	chunk->nrows = f->nrows;
	chunk->ncols = (uint16) f->ncols;
	chunk->side = (uint16) side;
	chunk->first_row = f->first_row;
	cols = vexec_dev_chunk_cols(chunk);
	pieces = (VexecDevPiece *) (cols + f->ncols);
	maxpieces = (size - ((char *) pieces - (char *) req)) / sizeof(VexecDevPiece);
	{
		uint64		want = 0;

		for (int i = 0; i < f->ncols; i++)
			want += f->cols[i].values.npieces + f->cols[i].validity.npieces + f->cols[i].offsets.npieces;
		if (want > maxpieces)
			elog(ERROR, "vexec: a device chunk of %llu pieces, more than a request's %llu",
				 (unsigned long long) want, (unsigned long long) maxpieces);
	}
	for (int i = 0; i < f->ncols; i++)
	{
		VexecDevFillCol *c = &f->cols[i];
		VexecDevCol *dc = &cols[i];

		memset(dc, 0, sizeof(VexecDevCol));
		dc->type = c->type;
		dc->scale = c->scale;
		if (c->type == VEXEC_DEV_STRING)
		{
			put_buf(&c->values, c->values.length, &dc->values, pieces, &np);
			put_buf(&c->offsets, ((uint64) f->nrows + 1) * sizeof(int32), &dc->offsets, pieces, &np);
		}
		else
			put_buf(&c->values, (uint64) f->nrows * c->width, &dc->values, pieces, &np);
		if (c->nulls)
			put_buf(&c->validity, ((uint64) f->nrows + 63) / 64 * 8, &dc->validity, pieces, &np);
		bytes += dc->values.length + dc->validity.length + dc->offsets.length;
	}
	chunk->npieces = np;
	req->kind = VEXEC_DEV_REQ_CHUNK;
	req->op = (uint16) t->op;
	req->length = (uint32) ((char *) (pieces + np) - (char *) req);
	t->r->submit(t->task);
	t->rows_given += f->nrows;
	t->fig.chunks++;
	t->fig.rows += f->nrows;
	t->fig.bytes += bytes;
}

/*
 * V8e: units for the executor to read from their files itself, as a chunk
 * of the program's columns (VEXEC_DEV_REQ_UNITS): each unit's file, its
 * parts, its rows and its columns' names in the program's order, the
 * columns' checks, written into the request area and submitted.  False,
 * nothing submitted, where they do not fit in it: fewer at a time.
 */
bool
vexec_devtask_units(VexecDevTask *t, int ncols, int nqcols, const uint8 *flags,
					const VexecDevColType *qtypes, Oid relation, const int16 *attnums,
					const void *filter, Size filter_len, const void *exprs, Size exprs_len,
					int nunits, VexecDevUnitDesc *const *units, uint64 first_row,
					VexecDevUnitBytes fill, void *fill_arg, uint32 req_flags)
{
	/* W5: a probe names the units and their columns alone, no deletion and no stream */
	bool		probe = (req_flags & VEXEC_DEV_UNITS_PROBE) != 0;
	Size		size;
	VexecDevRequest *req = t->r->request_area(t->task, &size);
	VexecDevUnits *u = (VexecDevUnits *) (req + 1);
	uint64		nparts = 0;
	uint64		strings = 0;
	uint64		rows = 0;
	uint64		len;
	VexecDevUnit *list;
	int32	   *parts;
	char	   *sp;
	uint32		at = 0;
	uint32		pat = 0;
	uint64		dbytes = 0;
	uint64		sbytes = 0;
	uint64		npieces = 0;
	int			nblocks = 0;
	int			nstreams = 0;
	uint32		sat = 0;
	VexecDevPiece *pieces;
	VexecDevPartStream *dstreams;

	Assert(t->nunit_blocks == 0);
	for (int i = 0; i < nunits; i++)
	{
		/* W21: a unit's granules' rows after its granules in the parts area */
		nparts += (uint64) units[i]->nparts * (units[i]->granule_rows != NULL ? 2 : 1);
		strings += strlen(units[i]->path) + 1;
		for (int c = 0; c < ncols + nqcols; c++)
			strings += strlen(units[i]->names[c]) + 1;
		/* a unit's deletions, a bit a row, one bitmap after another in blocks, a piece where a block ends */
		if (units[i]->deleted != NULL && !probe)
		{
			uint64		n = (units[i]->rows + 7) / 8;

			npieces += (dbytes % t->block_size + n + t->block_size - 1) / t->block_size;
			dbytes += n;
		}
		/* W6: a MERGETREE unit's streams' bytes, after them in the blocks, the same way */
		for (int s = 0; s < units[i]->nstreams && !probe; s++)
		{
			uint64		n = units[i]->streams[s].length;

			if (units[i]->streams[s].kept)
				continue;
			npieces += (dbytes % t->block_size + n + t->block_size - 1) / t->block_size;
			dbytes += n;
			nstreams++;
		}
	}
	len = sizeof(VexecDevRequest) + vexec_dev_units_size(ncols, nqcols, nunits, (int) nparts, (int) npieces,
														 strings, filter_len, exprs_len, nstreams);
	if (nunits == 0 || len > size || nparts > PG_UINT32_MAX || strings > PG_UINT32_MAX)
		return false;
	if (dbytes > 0)
	{
		nblocks = (int) ((dbytes + t->block_size - 1) / t->block_size);
		if (nblocks > t->share - t->held)
			return false;
		t->unit_blocks = t->unit_blocks ? repalloc(t->unit_blocks, sizeof(uint32) * nblocks) :
			MemoryContextAlloc(t->mcxt, sizeof(uint32) * nblocks);
		t->nunit_blocks = t->r->claim(t->task, nblocks, t->unit_blocks);
		t->held += t->nunit_blocks;
		if (t->nunit_blocks < nblocks)
		{
			vexec_devtask_units_done(t);
			return false;
		}
	}
	memset(req, 0, len);
	u->nunits = (uint32) nunits;
	u->ncols = (uint16) ncols;
	u->nqcols = (uint16) nqcols;
	u->nparts = (uint32) nparts;
	u->npieces = (uint32) npieces;
	u->strings_length = (uint32) strings;
	u->filter_length = (uint32) filter_len;
	u->expr_length = (uint32) exprs_len;
	u->nstreams = (uint32) nstreams;
	u->flags = req_flags;
	u->first_row = first_row;
	u->database = OidIsValid(relation) ? MyDatabaseId : InvalidOid;
	u->relation = relation;
	memcpy(vexec_dev_units_flags(u), flags, ncols + nqcols);
	if (nqcols > 0)
		memcpy(vexec_dev_units_qtypes(u), qtypes, sizeof(VexecDevColType) * nqcols);
	if (attnums != NULL)
		memcpy(vexec_dev_units_attnums(u), attnums, sizeof(int16) * (ncols + nqcols));
	if (filter_len > 0)
		memcpy(vexec_dev_units_filter(u), filter, filter_len);
	/* W10: the computed columns' expressions, a filter's program each */
	if (exprs_len > 0)
		memcpy(vexec_dev_units_exprs(u), exprs, exprs_len);
	list = vexec_dev_units_list(u);
	parts = vexec_dev_units_parts(u);
	pieces = vexec_dev_units_pieces(u);
	sp = vexec_dev_units_strings(u);
	dstreams = vexec_dev_units_streams(u);
	npieces = 0;
	dbytes = 0;
	for (int i = 0; i < nunits; i++)
	{
		const VexecDevUnitDesc *x = units[i];
		VexecDevUnit *du = &list[i];
		Size		n;

		du->format = (uint8) x->format;
		du->nparts = (uint32) x->nparts;
		du->parts = pat;
		memcpy(parts + pat, x->parts, sizeof(int32) * x->nparts);
		pat += (uint32) x->nparts;
		/* W21: each granule's rows, for the executor's word of the granules none of whose rows the filter kept */
		if (x->granule_rows != NULL)
		{
			du->granule_rows = pat;
			memcpy(parts + pat, x->granule_rows, sizeof(int32) * x->nparts);
			pat += (uint32) x->nparts;
		}
		else
			du->granule_rows = VEXEC_DEV_UNIT_NO_GRANULES;
		du->path = at;
		n = strlen(x->path) + 1;
		memcpy(sp + at, x->path, n);
		at += (uint32) n;
		du->names = at;
		for (int c = 0; c < ncols + nqcols; c++)
		{
			n = strlen(x->names[c]) + 1;
			memcpy(sp + at, x->names[c], n);
			at += (uint32) n;
		}
		du->file_size = x->file_size;
		du->file_mtime = x->file_mtime;
		du->rows = x->rows;
		rows += x->rows;
		if (x->deleted != NULL && !probe)
		{
			const char *bits = (const char *) x->deleted;
			uint64		left = (x->rows + 7) / 8;

			du->deleted.first = (uint32) npieces;
			du->deleted.length = left;
			while (left > 0)
			{
				uint32		block = t->unit_blocks[dbytes / t->block_size];
				uint32		off = (uint32) (dbytes % t->block_size);
				uint32		take = (uint32) Min(left, (uint64) (t->block_size - off));
				VexecDevPiece *p = &pieces[npieces++];

				memcpy(t->r->block(block) + off, bits, take);
				p->block = block;
				p->offset = off;
				p->length = take;
				p->pad = 0;
				bits += take;
				left -= take;
				dbytes += take;
				du->deleted.npieces++;
			}
		}
		/* W6: its streams' bytes, filled by the caller's source into the blocks a piece at a time */
		du->first_stream = sat;
		du->nstreams = 0;
		for (int s = 0; s < x->nstreams && !probe; s++)
		{
			const VexecDevUnitStream *xs = &x->streams[s];
			VexecDevPartStream *ds;
			uint64		left = xs->length;
			uint64		done = 0;

			if (xs->kept)
				continue;
			ds = &dstreams[sat++];
			du->nstreams++;

			ds->column = (uint16) xs->column;
			ds->kind = (uint8) xs->kind;
			ds->width = (uint8) xs->width;
			ds->rows = xs->rows;
			ds->skip = xs->skip;
			ds->bytes.first = (uint32) npieces;
			ds->bytes.length = xs->length;
			while (left > 0)
			{
				uint32		block = t->unit_blocks[dbytes / t->block_size];
				uint32		off = (uint32) (dbytes % t->block_size);
				uint32		take = (uint32) Min(left, (uint64) (t->block_size - off));
				VexecDevPiece *p = &pieces[npieces++];

				fill(fill_arg, x, s, done, take, t->r->block(block) + off);
				p->block = block;
				p->offset = off;
				p->length = take;
				p->pad = 0;
				done += take;
				left -= take;
				dbytes += take;
				ds->bytes.npieces++;
			}
			sbytes += xs->length;
		}
	}
	req->kind = VEXEC_DEV_REQ_UNITS;
	req->op = (uint16) t->op;
	req->length = (uint32) len;
	t->r->submit(t->task);
	if (probe)
		return true;
	t->rows_given += rows;
	t->fig.chunks++;
	t->fig.rows += rows;
	/* the streams' bytes cross to the device, as a chunk's do; a file's the executor reads are its own */
	t->fig.bytes += sbytes;
	return true;
}

/* A units request answered: its deletions' blocks given back */
/*
 * A unit a source handed, copied for a task's units requests (V8e; W13,
 * shared by VecAgg's and VecScan's device reads): each column read's name in
 * the unit -- nread of them, column c the source's spec column cols[c], or
 * -1 for one the executor computes or makes, its name empty -- the unit's
 * checks of each added into flags[c]; a MERGETREE unit's streams, each of a
 * column read, with the source's stream it is (W6); and the source's own
 * description, its pointers these copies', for read_unit() and unit_tids().
 */
VexecDevUnitDesc *
vexec_devunit_copy(MemoryContext mcxt, const VexecSourceUnit *u, int nread, const int *cols, uint8 *flags)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	VexecDevUnitDesc *x = palloc0(sizeof(VexecDevUnitDesc));

	x->format = u->format == VEXEC_UNIT_PARQUET ? VEXEC_DEV_UNIT_PARQUET :
		u->format == VEXEC_UNIT_MERGETREE ? VEXEC_DEV_UNIT_MERGETREE : VEXEC_DEV_UNIT_ARROW;
	x->path = pstrdup(u->path);
	x->file_size = u->file_size;
	x->file_mtime = u->file_mtime;
	x->nparts = u->nparts;
	x->parts = palloc(sizeof(int32) * Max(u->nparts, 1));
	memcpy(x->parts, u->parts, sizeof(int32) * u->nparts);
	x->rows = u->rows;
	/* CH8's parts' rows the snapshot does not see, as the source handed them */
	if (u->deleted != NULL)
	{
		Size		words = (u->rows + 63) / 64;

		x->deleted = palloc(sizeof(uint64) * Max(words, 1));
		memcpy(x->deleted, u->deleted, sizeof(uint64) * words);
	}
	/* W21 (minor 11): a MERGETREE unit's part's identity and granules' rows, for the condition cache */
	if (u->format == VEXEC_UNIT_MERGETREE && u->size >= offsetof(VexecSourceUnit, granule_rows) + sizeof(u->granule_rows) &&
		u->granule_rows != NULL && u->nparts > 0)
	{
		x->unit_id[0] = u->unit_id[0];
		x->unit_id[1] = u->unit_id[1];
		x->ngranules = u->ngranules;
		x->granule_rows = palloc(sizeof(uint32) * u->nparts);
		memcpy(x->granule_rows, u->granule_rows, sizeof(uint32) * u->nparts);
	}
	x->names = palloc(sizeof(char *) * Max(nread, 1));
	x->nnames = nread;
	for (int c = 0; c < nread; c++)
	{
		int			uc = cols[c];

		x->names[c] = pstrdup(uc >= 0 ? u->names[uc] : "");
		if (uc >= 0)
		{
			flags[c] |= u->column_flags[uc] & (VEXEC_DEV_UCOL_NOT_NULL | VEXEC_DEV_UCOL_CHECK_UTF8);
			/* PQ6: an integer of days or of seconds since 1970, a date or a timestamp */
			if (u->column_flags[uc] & VEXEC_UNIT_DAYS)
				flags[c] |= VEXEC_DEV_UCOL_DAYS;
			if (u->column_flags[uc] & VEXEC_UNIT_SECONDS)
				flags[c] |= VEXEC_DEV_UCOL_SECONDS;
		}
	}
	x->token = u->token;
	/* what read_unit() is given: the source's description, its file and parts */
	x->unit = *u;
	x->unit.path = x->path;
	x->unit.parts = x->parts;
	x->unit.names = NULL;
	x->unit.column_flags = NULL;
	x->unit.deleted = NULL;
	x->unit.streams = NULL;
	x->unit.nstreams = 0;
	/*
	 * W6: a MERGETREE unit's streams, each of a column read -- of its spec
	 * column -- and the source's streams, which read_unit_bytes() reads
	 */
	if (u->format == VEXEC_UNIT_MERGETREE && u->nstreams > 0)
	{
		VexecSourceStream *src = palloc(sizeof(VexecSourceStream) * u->nstreams);
		/* W26 (minor 12): a unit of several ranges, each stream's range's rows */
		const uint32 *srows = u->size >= offsetof(VexecSourceUnit, stream_rows) + sizeof(u->stream_rows) ?
			u->stream_rows : NULL;
		int			n = 0;

		memcpy(src, u->streams, sizeof(VexecSourceStream) * u->nstreams);
		x->unit.streams = src;
		x->unit.nstreams = u->nstreams;
		x->streams = palloc(sizeof(VexecDevUnitStream) * Max(u->nstreams * nread, 1));
		for (int c = 0; c < nread; c++)
		{
			int			uc = cols[c];

			if (uc < 0)
				continue;
			for (int k = 0; k < u->nstreams; k++)
				if (u->streams[k].column == uc)
				{
					VexecDevUnitStream *ds = &x->streams[n++];

					ds->column = c;
					ds->kind = u->streams[k].kind == VEXEC_STREAM_SIZES ? VEXEC_DEV_STREAM_SIZES :
						u->streams[k].kind == VEXEC_STREAM_NULLS ? VEXEC_DEV_STREAM_NULLS :
						u->streams[k].kind == VEXEC_STREAM_SPARSE ? VEXEC_DEV_STREAM_SPARSE :
						u->streams[k].kind == VEXEC_STREAM_SPARSE_NULLS ? VEXEC_DEV_STREAM_SPARSE_NULLS :
						VEXEC_DEV_STREAM_VALUES;
					ds->width = u->streams[k].width;
					ds->length = u->streams[k].length;
					ds->skip = u->streams[k].skip;
					ds->rows = srows ? srows[k] : 0;
					ds->source = k;
					ds->kept = false;
				}
		}
		x->nstreams = n;
	}
	MemoryContextSwitchTo(old);
	return x;
}

void
vexec_devunit_free(VexecDevUnitDesc *x)
{
	for (int c = 0; c < x->nnames; c++)
		pfree(x->names[c]);
	pfree(x->path);
	pfree(x->parts);
	pfree(x->names);
	if (x->deleted)
		pfree(x->deleted);
	if (x->streams)
		pfree(x->streams);
	if (x->unit.streams)
		pfree(unconstify(VexecSourceStream *, x->unit.streams));
	if (x->granule_rows)
		pfree(x->granule_rows);
	pfree(x);
}

/*
 * W5, W6: a probe of the columns the device keeps of MERGETREE units, before
 * their request: those it keeps sent no bytes of -- the CPU copies none of
 * their compressed blocks -- the device taking them from what it keeps.
 */
void
vexec_devunits_probe(VexecDevTask *t, int ncols, int nqcols, const uint8 *flags, const VexecDevColType *qtypes,
					 Oid relation, const int16 *attnums, VexecDevUnitDesc **arr, int n)
{
	int			nread = ncols + nqcols;
	const VexecDevAnswer *a;
	const uint8 *bits;
	bool		any = false;

	for (int i = 0; i < n && !any; i++)
		any = arr[i]->format == VEXEC_DEV_UNIT_MERGETREE && arr[i]->nstreams > 0;
	if (!any || !vexec_devtask_units(t, ncols, nqcols, flags, qtypes, relation, attnums, NULL, 0, NULL, 0, n, arr,
									 0, NULL, NULL, VEXEC_DEV_UNITS_PROBE))
		return;
	/* an answer other than OK: every stream sent, the request then answering as it answers */
	if (vexec_devtask_wait(t) != VEXEC_DEV_OK)
		return;
	a = vexec_devtask_answer(t);
	bits = (const uint8 *) vexec_dev_answer_cols(unconstify(VexecDevAnswer *, a));
	for (int i = 0; i < n; i++)
		for (int k = 0; k < arr[i]->nstreams; k++)
		{
			uint64		b = (uint64) i * nread + arr[i]->streams[k].column;

			arr[i]->streams[k].kept = (bits[b / 8] >> (b % 8)) & 1;
		}
}

void
vexec_devtask_units_done(VexecDevTask *t)
{
	if (t->nunit_blocks == 0)
		return;
	if (t->task != NULL)
		t->r->release(t->task, t->nunit_blocks, t->unit_blocks);
	t->held -= t->nunit_blocks;
	t->nunit_blocks = 0;
}

/* ---------------------------------------------------------------------
 * W4: a scan's quals as a units request's filter (§3.26)
 * ---------------------------------------------------------------------
 *
 * The forms the device's filter takes, each answering as PostgreSQL's
 * operator does, NULL where it is NULL: a column compared with a constant
 * by its btree family's =, <>, <, <=, > or >= -- integers of any width
 * among them, booleans, oids, dates, times and timestamps against their own
 * type, text and bytea by their bytes, < and > of text under C's collation
 * alone; IN and NOT IN of a list of constants, as ORs of = and ANDs of <>;
 * text's LIKE and NOT LIKE of a constant pattern whose backslashes each
 * escape a '%' or a '_'; IS NULL and IS NOT NULL of a column; a boolean
 * column; AND, OR and NOT of them.  None raises: a qual that might -- a
 * function, a cast, arithmetic -- has no form here, and the scan keeps it.
 */

typedef struct FilterBuild
{
	StringInfoData steps;
	StringInfoData data;
	int			nsteps;
	Index		varno;
	VexecDevFilterColumn column;
	void	   *arg;
	const char *why;
} FilterBuild;

#define FILTER_MAX_LIST		64		/* IN's constants */

static void
filter_step(FilterBuild *b, int op, int type, uint32 arg)
{
	VexecDevFilterStep st;

	memset(&st, 0, sizeof(st));
	st.op = (uint8) op;
	st.type = (uint8) type;
	st.arg = arg;
	appendBinaryStringInfo(&b->steps, &st, sizeof(st));
	b->nsteps++;
}

/* The device type a filter compares a value of this type as, or VEXEC_DEV_NONE. */
int
vexec_devfilter_type(Oid typid)
{
	switch (getBaseType(typid))
	{
		case BOOLOID:
			return VEXEC_DEV_BOOL8;
		case INT2OID:
			return VEXEC_DEV_INT16;
		case INT4OID:
		case DATEOID:
			return VEXEC_DEV_INT32;
		case OIDOID:
			return VEXEC_DEV_UINT32;
		case INT8OID:
		case TIMEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return VEXEC_DEV_INT64;
		case TEXTOID:
		case VARCHAROID:
		case BYTEAOID:
			return VEXEC_DEV_STRING;
		default:
			return VEXEC_DEV_NONE;
	}
}

static Expr *
filter_strip(Expr *e)
{
	while (e != NULL && IsA(e, RelabelType))
		e = ((RelabelType *) e)->arg;
	return e;
}

static bool
filter_fail(FilterBuild *b, const char *why)
{
	if (b->why == NULL)
		b->why = why;
	return false;
}

/* A column of the relation pushed, its type and collation given back. */
static bool
filter_column(FilterBuild *b, Expr *e, Oid *typid, Oid *collid)
{
	Var		   *v = (Var *) filter_strip(e);
	int			col;

	if (v == NULL || !IsA(v, Var) || v->varno != b->varno || v->varattno <= 0 || v->varlevelsup != 0)
		return filter_fail(b, "a qual over something other than the relation's columns");
	*typid = v->vartype;
	*collid = v->varcollid;
	if (vexec_devfilter_type(v->vartype) == VEXEC_DEV_NONE)
		return filter_fail(b, psprintf("a qual over a column of type %s", format_type_be(v->vartype)));
	col = b->column(b->arg, v->varattno, v->vartype, v->varcollid);
	if (col < 0)
		return filter_fail(b, "a qual over a column the units do not hold");
	filter_step(b, VEXEC_DEV_FILTER_COLUMN, 0, (uint32) col);
	return true;
}

/* A constant pushed, in the device's form of its own type. */
static bool
filter_const_value(FilterBuild *b, Datum value, Oid typid)
{
	int			type = vexec_devfilter_type(typid);
	uint32		at;

	if (type == VEXEC_DEV_NONE)
		return filter_fail(b, psprintf("a constant of type %s", format_type_be(typid)));
	/* every constant at an offset of 8 */
	while (b->data.len % 8 != 0)
		appendStringInfoChar(&b->data, '\0');
	at = (uint32) b->data.len;
	if (type == VEXEC_DEV_STRING)
	{
		varlena    *vl = pg_detoast_datum_packed((varlena *) DatumGetPointer(value));
		int32		n = (int32) VARSIZE_ANY_EXHDR(vl);

		appendBinaryStringInfo(&b->data, &n, sizeof(n));
		appendBinaryStringInfo(&b->data, VARDATA_ANY(vl), n);
	}
	else
	{
		switch (type)
		{
			case VEXEC_DEV_BOOL8:
				{
					uint8		v = DatumGetBool(value) ? 1 : 0;

					appendBinaryStringInfo(&b->data, &v, 1);
				}
				break;
			case VEXEC_DEV_INT16:
				{
					int16		v = DatumGetInt16(value);

					appendBinaryStringInfo(&b->data, &v, 2);
				}
				break;
			case VEXEC_DEV_INT32:
				{
					int32		v = DatumGetInt32(value);

					appendBinaryStringInfo(&b->data, &v, 4);
				}
				break;
			case VEXEC_DEV_UINT32:
				{
					uint32		v = DatumGetObjectId(value);

					appendBinaryStringInfo(&b->data, &v, 4);
				}
				break;
			default:
				{
					int64		v = DatumGetInt64(value);

					appendBinaryStringInfo(&b->data, &v, 8);
				}
				break;
		}
	}
	filter_step(b, VEXEC_DEV_FILTER_CONST, type, at);
	return true;
}

/*
 * The comparison an operator between these types is, as the device makes
 * it, or 0: by its btree family's interpretation -- integers of any width,
 * booleans, oids, dates, times, timestamps and bytea against their own
 * type, text's = and <> under a deterministic collation and the rest under
 * C's.
 */
static int
filter_cmp_kind(Oid opno, Oid ltype, Oid rtype, Oid collid)
{
	List	   *interps = get_op_index_interpretation(opno);
	ListCell   *lc;
	Oid			lt = getBaseType(ltype);
	Oid			rt = getBaseType(rtype);

	foreach(lc, interps)
	{
		OpIndexInterpretation *oi = lfirst(lc);
		bool		ints = (lt == INT2OID || lt == INT4OID || lt == INT8OID) &&
			(rt == INT2OID || rt == INT4OID || rt == INT8OID);
		int			kind;

		switch (oi->cmptype)
		{
			case COMPARE_EQ:
				kind = VEXEC_DEV_CMP_EQ;
				break;
			case COMPARE_NE:
				kind = VEXEC_DEV_CMP_NE;
				break;
			case COMPARE_LT:
				kind = VEXEC_DEV_CMP_LT;
				break;
			case COMPARE_LE:
				kind = VEXEC_DEV_CMP_LE;
				break;
			case COMPARE_GT:
				kind = VEXEC_DEV_CMP_GT;
				break;
			case COMPARE_GE:
				kind = VEXEC_DEV_CMP_GE;
				break;
			default:
				continue;
		}
		if (oi->opfamily_id == INTEGER_BTREE_FAM_OID && ints)
			return kind;
		if (lt != rt)
			continue;
		if ((oi->opfamily_id == BOOL_BTREE_FAM_OID && lt == BOOLOID) ||
			(oi->opfamily_id == OID_BTREE_FAM_OID && lt == OIDOID) ||
			(oi->opfamily_id == BYTEA_BTREE_FAM_OID && lt == BYTEAOID))
			return kind;
		if (lt == DATEOID || lt == TIMEOID || lt == TIMESTAMPOID || lt == TIMESTAMPTZOID)
			return kind;
		if (oi->opfamily_id == TEXT_BTREE_FAM_OID && lt == TEXTOID)
		{
			bool		c = OidIsValid(collid) && pg_newlocale_from_collation(collid)->collate_is_c;

			if (kind == VEXEC_DEV_CMP_EQ || kind == VEXEC_DEV_CMP_NE ?
				!OidIsValid(collid) || get_collation_isdeterministic(collid) : c)
				return kind;
		}
	}
	return 0;
}

/* Whether a LIKE pattern's backslashes each escape a '%' or a '_', as the device's LIKE takes them. */
static bool
filter_like_pattern_ok(Datum pattern)
{
	varlena    *vl = pg_detoast_datum_packed((varlena *) DatumGetPointer(pattern));
	const char *p = VARDATA_ANY(vl);
	int			n = VARSIZE_ANY_EXHDR(vl);

	for (int i = 0; i < n; i++)
		if (p[i] == '\\')
		{
			if (i + 1 >= n || (p[i + 1] != '%' && p[i + 1] != '_'))
				return false;
			i++;
		}
	return true;
}

static bool filter_expr(FilterBuild *b, Expr *e);
static bool filter_value(FilterBuild *b, Expr *e, int *type);

static bool
filter_opexpr(FilterBuild *b, OpExpr *op)
{
	Expr	   *l;
	Expr	   *r;
	Oid			opno = op->opno;
	Oid			ltype;
	Oid			lcoll;
	int			kind;

	if (list_length(op->args) != 2)
		return filter_fail(b, "a qual of an operator of one argument");
	l = filter_strip(linitial(op->args));
	r = filter_strip(lsecond(op->args));
	if (op->opfuncid == F_TEXTLIKE || op->opfuncid == F_TEXTNLIKE)
	{
		/* text LIKE a constant pattern */
		if (!IsA(r, Const) || ((Const *) r)->constisnull || !filter_like_pattern_ok(((Const *) r)->constvalue))
			return filter_fail(b, "a LIKE of a pattern the device does not match as PostgreSQL does");
		if (OidIsValid(op->inputcollid) && !get_collation_isdeterministic(op->inputcollid))
			return filter_fail(b, "a LIKE under a nondeterministic collation");
		if (!filter_column(b, l, &ltype, &lcoll) || vexec_devfilter_type(ltype) != VEXEC_DEV_STRING ||
			getBaseType(ltype) == BYTEAOID ||
			!filter_const_value(b, ((Const *) r)->constvalue, TEXTOID))
			return filter_fail(b, "a LIKE over other than a text column");
		filter_step(b, VEXEC_DEV_FILTER_LIKE, 0, op->opfuncid == F_TEXTNLIKE ? 1 : 0);
		return true;
	}
	/* a constant first: the commuted operator */
	if (IsA(l, Const) && !IsA(r, Const))
	{
		Expr	   *t = l;

		l = r;
		r = t;
		opno = get_commutator(opno);
		if (!OidIsValid(opno))
			return filter_fail(b, "a comparison with no commutator");
	}
	if (!IsA(r, Const) || ((Const *) r)->constisnull)
		return filter_fail(b, "a qual comparing other than a column with a constant");
	kind = filter_cmp_kind(opno, exprType((Node *) l), ((Const *) r)->consttype, op->inputcollid);
	if (kind == 0)
		return filter_fail(b, "a qual of an operator the device does not compare as PostgreSQL does");
	/* W10: a column, or a value the machine makes */
	if (IsA(l, Var))
	{
		if (!filter_column(b, l, &ltype, &lcoll))
			return false;
	}
	else
	{
		int			t;

		if (!filter_value(b, l, &t))
			return false;
	}
	if (!filter_const_value(b, ((Const *) r)->constvalue, ((Const *) r)->consttype))
		return false;
	filter_step(b, VEXEC_DEV_FILTER_CMP, 0, (uint32) kind);
	return true;
}

/* ---------------------------------------------------------------------
 * W10 (§3.27): values the device's machine makes, as the units' computed
 * columns -- a key, an aggregate's argument -- and a comparison's operand
 * ---------------------------------------------------------------------
 *
 * Each answers as PostgreSQL's function does over the row, or makes the
 * device refuse the unit, which the CPU then reads, whose function raises
 * or answers: integer arithmetic out of its result's range, extract() of a
 * timestamp not finite.  extract()'s fields of an integer -- its numeric of
 * scale 0 -- come as INT64, which a grouping's key takes as the numeric
 * (vexec_devexpr_int_extract()); length() counts a UTF-8 text's characters,
 * the column checked as it is read.
 */

/* extract()'s and date_trunc()'s field named by a constant, or 0 */
static int
value_field(Expr *e, bool trunc)
{
	static const struct
	{
		const char *name;
		int			field;
		bool		trunc;
		bool		extract;
	}			fields[] = {
		{"second", VEXEC_DEV_FIELD_SECOND, true, false},
		{"minute", VEXEC_DEV_FIELD_MINUTE, true, true},
		{"hour", VEXEC_DEV_FIELD_HOUR, true, true},
		{"day", VEXEC_DEV_FIELD_DAY, true, true},
		{"month", VEXEC_DEV_FIELD_MONTH, false, true},
		{"year", VEXEC_DEV_FIELD_YEAR, false, true},
		{"dow", VEXEC_DEV_FIELD_DOW, false, true},
		{"isodow", VEXEC_DEV_FIELD_ISODOW, false, true},
		{"doy", VEXEC_DEV_FIELD_DOY, false, true},
		{"quarter", VEXEC_DEV_FIELD_QUARTER, false, true},
	};
	char	   *name;

	e = filter_strip(e);
	if (e == NULL || !IsA(e, Const) || ((Const *) e)->constisnull || ((Const *) e)->consttype != TEXTOID)
		return 0;
	name = TextDatumGetCString(((Const *) e)->constvalue);
	name = downcase_truncate_identifier(name, strlen(name), false);
	for (int i = 0; i < (int) lengthof(fields); i++)
		if (strcmp(name, fields[i].name) == 0 && (trunc ? fields[i].trunc : fields[i].extract))
			return fields[i].field;
	return 0;
}

/*
 * Whether e is extract() of a timestamp's field of an integer: INT64 on the
 * device -- W43, a timestamptz's under UTC.
 */
bool
vexec_devexpr_int_extract(Expr *e)
{
	FuncExpr   *f = (FuncExpr *) filter_strip(e);

	if (f == NULL || !IsA(f, FuncExpr) || list_length(f->args) != 2 || value_field(linitial(f->args), false) == 0)
		return false;
	if (f->funcid == F_EXTRACT_TEXT_TIMESTAMP)
		return exprType((Node *) lsecond(f->args)) == TIMESTAMPOID;
	return f->funcid == F_EXTRACT_TEXT_TIMESTAMPTZ && exprType((Node *) lsecond(f->args)) == TIMESTAMPTZOID &&
		vexec_session_utc();
}

/* An integer operator's arithmetic, or 0 */
static int
value_arith(Oid opfuncid)
{
	switch (opfuncid)
	{
		case F_INT2PL:
		case F_INT4PL:
		case F_INT8PL:
		case F_INT24PL:
		case F_INT42PL:
		case F_INT48PL:
		case F_INT84PL:
		case F_INT28PL:
		case F_INT82PL:
			return VEXEC_DEV_ARITH_ADD;
		case F_INT2MI:
		case F_INT4MI:
		case F_INT8MI:
		case F_INT24MI:
		case F_INT42MI:
		case F_INT48MI:
		case F_INT84MI:
		case F_INT28MI:
		case F_INT82MI:
			return VEXEC_DEV_ARITH_SUB;
		case F_INT2MUL:
		case F_INT4MUL:
		case F_INT8MUL:
		case F_INT24MUL:
		case F_INT42MUL:
		case F_INT48MUL:
		case F_INT84MUL:
		case F_INT28MUL:
		case F_INT82MUL:
			return VEXEC_DEV_ARITH_MUL;
		default:
			return 0;
	}
}

static bool
value_integer(int type)
{
	return type == VEXEC_DEV_INT16 || type == VEXEC_DEV_INT32 || type == VEXEC_DEV_INT64;
}

static bool
filter_value(FilterBuild *b, Expr *e, int *type)
{
	e = filter_strip(e);
	if (e == NULL)
		return filter_fail(b, "an empty value");
	/* a condition: the filter's own steps, a BOOL8 -- a FILTER's, a CASE's */
	if (exprType((Node *) e) == BOOLOID && !IsA(e, Var) && !IsA(e, Const))
	{
		*type = VEXEC_DEV_BOOL8;
		return filter_expr(b, e);
	}
	switch (nodeTag(e))
	{
		case T_Var:
			{
				Oid			t;
				Oid			c;

				if (!filter_column(b, e, &t, &c))
					return false;
				*type = vexec_devfilter_type(t);
				return true;
			}
		case T_Const:
			if (((Const *) e)->constisnull)
				return filter_fail(b, "a NULL constant as a value");
			*type = vexec_devfilter_type(((Const *) e)->consttype);
			return filter_const_value(b, ((Const *) e)->constvalue, ((Const *) e)->consttype);
		case T_OpExpr:
			{
				OpExpr	   *op = (OpExpr *) e;
				int			kind = list_length(op->args) == 2 ? value_arith(get_opcode(op->opno)) : 0;
				int			ta;
				int			tb;

				if (kind == 0)
					return filter_fail(b, "an operator the device's machine does not compute");
				if (!filter_value(b, linitial(op->args), &ta) || !filter_value(b, lsecond(op->args), &tb))
					return false;
				*type = vexec_devfilter_type(op->opresulttype);
				if (!value_integer(ta) || !value_integer(tb) || !value_integer(*type))
					return filter_fail(b, "arithmetic of other than integers");
				filter_step(b, VEXEC_DEV_FILTER_ARITH, *type, (uint32) kind);
				return true;
			}
		case T_FuncExpr:
			{
				FuncExpr   *f = (FuncExpr *) e;
				int			t;
				int			field;

				switch (f->funcid)
				{
					case F_INT4_INT2:
					case F_INT8_INT4:
					case F_INT8_INT2:
						if (list_length(f->args) != 1 || !filter_value(b, linitial(f->args), &t) || !value_integer(t))
							return filter_fail(b, "a cast the device's machine does not compute");
						*type = vexec_devfilter_type(f->funcresulttype);
						filter_step(b, VEXEC_DEV_FILTER_WIDEN, *type, 0);
						return true;
					case F_TO_TIMESTAMP_FLOAT8:
						{
							/*
							 * W43 (§3.35): to_timestamp() of an integer's
							 * seconds, through its float8 -- exact on the
							 * device where its microseconds are within 2^53
							 * (SECONDS), past them the CPU's
							 */
							FuncExpr   *cast = list_length(f->args) == 1 ? (FuncExpr *) filter_strip(linitial(f->args)) : NULL;

							if (cast == NULL || !IsA(cast, FuncExpr) || list_length(cast->args) != 1 ||
								(cast->funcid != F_FLOAT8_INT8 && cast->funcid != F_FLOAT8_INT4 &&
								 cast->funcid != F_FLOAT8_INT2))
								return filter_fail(b, "to_timestamp() of other than an integer's seconds");
							if (!filter_value(b, linitial(cast->args), &t))
								return false;
							if (!value_integer(t))
								return filter_fail(b, "to_timestamp() of other than an integer's seconds");
							*type = VEXEC_DEV_INT64;
							filter_step(b, VEXEC_DEV_FILTER_SECONDS, VEXEC_DEV_INT64, 0);
							return true;
						}
					case F_EXTRACT_TEXT_TIMESTAMP:
					case F_DATE_TRUNC_TEXT_TIMESTAMP:
					case F_EXTRACT_TEXT_TIMESTAMPTZ:
					case F_DATE_TRUNC_TEXT_TIMESTAMPTZ:
						{
							/* W43: a timestamptz's, under UTC, its UTC time's -- a timestamp's */
							bool		tz = f->funcid == F_EXTRACT_TEXT_TIMESTAMPTZ ||
								f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMPTZ;

							field = list_length(f->args) == 2 ?
								value_field(linitial(f->args), f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMP ||
											f->funcid == F_DATE_TRUNC_TEXT_TIMESTAMPTZ) : 0;
							if (field == 0 || exprType((Node *) lsecond(f->args)) != (tz ? TIMESTAMPTZOID : TIMESTAMPOID))
								return filter_fail(b, "a field extract() or date_trunc() takes on the CPU alone");
							if (tz && !vexec_session_utc())
								return filter_fail(b, "a timestamptz's field under a time zone not UTC's");
						}
						if (!filter_value(b, lsecond(f->args), &t))
							return false;
						if (f->funcid == F_EXTRACT_TEXT_TIMESTAMP || f->funcid == F_EXTRACT_TEXT_TIMESTAMPTZ)
						{
							*type = VEXEC_DEV_INT64;
							filter_step(b, VEXEC_DEV_FILTER_EXTRACT, 0, (uint32) field);
						}
						else
						{
							*type = VEXEC_DEV_INT64;
							filter_step(b, VEXEC_DEV_FILTER_TRUNC, 0, (uint32) field);
						}
						return true;
					case F_TEXTLEN:
					case F_LENGTH_TEXT:
					case F_CHAR_LENGTH_TEXT:
					case F_CHARACTER_LENGTH_TEXT:
					case F_OCTET_LENGTH_TEXT:
					case F_LENGTH_BYTEA:
						if (list_length(f->args) != 1 || !filter_value(b, linitial(f->args), &t) ||
							t != VEXEC_DEV_STRING)
							return filter_fail(b, "a length of other than text or bytea");
						*type = VEXEC_DEV_INT32;
						filter_step(b, VEXEC_DEV_FILTER_LENGTH, 0,
									f->funcid == F_OCTET_LENGTH_TEXT || f->funcid == F_LENGTH_BYTEA ? 1 : 0);
						return true;
					case F_REGEXP_REPLACE_TEXT_TEXT_TEXT:
						{
							/*
							 * W17 (§3.28): regexp_replace() of a constant pattern and
							 * replacement, for the patterns H10's kernel matches as
							 * PostgreSQL's engine does (k_regex.c): ClickBench's Q28's key
							 */
							Expr	   *pat;
							Expr	   *rep;
							Oid			argtype;
							char	   *prog;
							Size		plen;
							uint32		at;

							if (list_length(f->args) != 3)
								return filter_fail(b, "a regexp_replace() of other than three arguments");
							pat = filter_strip(lsecond(f->args));
							rep = filter_strip(lthird(f->args));
							if (!IsA(pat, Const) || ((Const *) pat)->constisnull || !IsA(rep, Const) || ((Const *) rep)->constisnull)
								return filter_fail(b, "a regexp_replace() whose pattern or replacement is not a constant");
							if (OidIsValid(f->inputcollid) && !get_collation_isdeterministic(f->inputcollid))
								return filter_fail(b, "a regexp_replace() under a nondeterministic collation");
							argtype = getBaseType(exprType(linitial(f->args)));
							if ((argtype != TEXTOID && argtype != VARCHAROID) || !filter_value(b, linitial(f->args), &t) ||
								t != VEXEC_DEV_STRING)
								return filter_fail(b, "a regexp_replace() of other than text");
							prog = vexec_regex_device_program(((Const *) pat)->constvalue, ((Const *) rep)->constvalue, &plen);
							if (prog == NULL)
								return filter_fail(b, "a regexp_replace() of a pattern the device's matcher does not take as PostgreSQL's engine does");
							while (b->data.len % 8 != 0)
								appendStringInfoChar(&b->data, '\0');
							at = (uint32) b->data.len;
							appendBinaryStringInfo(&b->data, prog, plen);
							pfree(prog);
							*type = VEXEC_DEV_STRING;
							filter_step(b, VEXEC_DEV_FILTER_REGEXP, VEXEC_DEV_STRING, at);
							return true;
						}
					default:
						return filter_fail(b, psprintf("a function the device's machine does not compute: %s",
													   get_func_name(f->funcid)));
				}
			}
		case T_CaseExpr:
			{
				CaseExpr   *c = (CaseExpr *) e;
				ListCell   *lc;
				int			t = VEXEC_DEV_NONE;

				if (c->arg != NULL || c->defresult == NULL)
					return filter_fail(b, "a CASE of a test value, or of no ELSE");
				foreach(lc, c->args)
				{
					CaseWhen   *w = lfirst_node(CaseWhen, lc);
					int			wt;

					if (!filter_expr(b, w->expr) || !filter_value(b, w->result, &wt))
						return false;
					if (t != VEXEC_DEV_NONE && wt != t)
						return filter_fail(b, "a CASE of results of two types");
					t = wt;
				}
				{
					int			et;

					if (!filter_value(b, c->defresult, &et) || et != t)
						return filter_fail(b, "a CASE whose ELSE is another type");
				}
				*type = t;
				filter_step(b, VEXEC_DEV_FILTER_CASE, 0, (uint32) list_length(c->args));
				return true;
			}
		default:
			return filter_fail(b, "a value of a form the device's machine does not make");
	}
}

/* x IN (...), x NOT IN (...): an OR of =, an AND of <>, a step each */
static bool
filter_saop(FilterBuild *b, ScalarArrayOpExpr *sa)
{
	Expr	   *l = filter_strip(linitial(sa->args));
	Const	   *r = (Const *) filter_strip(lsecond(sa->args));
	ArrayType  *arr;
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	int			kind;
	Oid			ltype;
	Oid			lcoll;

	if (!IsA(l, Var) || !IsA(r, Const) || r->constisnull)
		return filter_fail(b, "an IN of other than a column and a list of constants");
	arr = DatumGetArrayTypeP(r->constvalue);
	kind = filter_cmp_kind(sa->opno, ((Var *) l)->vartype, ARR_ELEMTYPE(arr), sa->inputcollid);
	if (kind != (sa->useOr ? VEXEC_DEV_CMP_EQ : VEXEC_DEV_CMP_NE))
		return filter_fail(b, "an ANY or ALL of other than = or <>");
	get_typlenbyvalalign(ARR_ELEMTYPE(arr), &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arr, ARR_ELEMTYPE(arr), elmlen, elmbyval, elmalign, &elems, &nulls, &n);
	if (n == 0 || n > FILTER_MAX_LIST)
		return filter_fail(b, "an IN of no constant, or of too many");
	for (int i = 0; i < n; i++)
	{
		if (nulls[i])
			return filter_fail(b, "an IN of a NULL");
		if (!filter_column(b, l, &ltype, &lcoll) || !filter_const_value(b, elems[i], ARR_ELEMTYPE(arr)))
			return false;
		filter_step(b, VEXEC_DEV_FILTER_CMP, 0, (uint32) kind);
	}
	if (n > 1)
		filter_step(b, sa->useOr ? VEXEC_DEV_FILTER_OR : VEXEC_DEV_FILTER_AND, 0, (uint32) n);
	return true;
}

static bool
filter_expr(FilterBuild *b, Expr *e)
{
	e = filter_strip(e);
	if (e == NULL)
		return filter_fail(b, "an empty qual");
	switch (nodeTag(e))
	{
		case T_Var:
			{
				Oid			t;
				Oid			c;

				if (getBaseType(((Var *) e)->vartype) != BOOLOID)
					return filter_fail(b, "a column as a qual that is not a boolean");
				return filter_column(b, e, &t, &c);
			}
		case T_OpExpr:
			return filter_opexpr(b, (OpExpr *) e);
		case T_ScalarArrayOpExpr:
			return filter_saop(b, (ScalarArrayOpExpr *) e);
		case T_NullTest:
			{
				NullTest   *nt = (NullTest *) e;
				Oid			t;
				Oid			c;

				if (nt->argisrow || !filter_column(b, nt->arg, &t, &c))
					return filter_fail(b, "a null test of other than a column");
				filter_step(b, nt->nulltesttype == IS_NULL ? VEXEC_DEV_FILTER_ISNULL : VEXEC_DEV_FILTER_ISNOTNULL,
							0, 0);
				return true;
			}
		case T_BoolExpr:
			{
				BoolExpr   *be = (BoolExpr *) e;
				ListCell   *lc;

				foreach(lc, be->args)
					if (!filter_expr(b, lfirst(lc)))
						return false;
				if (be->boolop == NOT_EXPR)
				{
					if (list_length(be->args) != 1)
						return filter_fail(b, "a NOT of other than one argument");
					filter_step(b, VEXEC_DEV_FILTER_NOT, 0, 0);
				}
				else if (list_length(be->args) > 1)
					filter_step(b, be->boolop == AND_EXPR ? VEXEC_DEV_FILTER_AND : VEXEC_DEV_FILTER_OR,
								0, (uint32) list_length(be->args));
				return true;
			}
		default:
			return filter_fail(b, "a qual of a form the device's filter does not take");
	}
}

/*
 * W10: an expression as a computed column's program: its steps, the last
 * value its device type into *type; NULL with *why where it has no form the
 * machine takes.
 */
char *
vexec_devexpr_build(Expr *e, Index varno, VexecDevFilterColumn column, void *arg, int *type, Size *len,
					const char **why)
{
	FilterBuild b;
	VexecDevFilter hdr;
	char	   *out;

	memset(&b, 0, sizeof(b));
	initStringInfo(&b.steps);
	initStringInfo(&b.data);
	b.varno = varno;
	b.column = column;
	b.arg = arg;
	*len = 0;
	*why = NULL;
	if (!filter_value(&b, e, type) || *type == VEXEC_DEV_NONE)
	{
		*why = b.why ? b.why : "a value of no device type";
		return NULL;
	}
	hdr.nsteps = (uint32) b.nsteps;
	hdr.data_length = (uint32) b.data.len;
	*len = vexec_dev_filter_size(b.nsteps, b.data.len);
	out = palloc(*len);
	memcpy(out, &hdr, sizeof(hdr));
	memcpy(vexec_dev_filter_steps((VexecDevFilter *) out), b.steps.data, b.steps.len);
	memcpy(vexec_dev_filter_data((VexecDevFilter *) out), b.data.data, b.data.len);
	pfree(b.steps.data);
	pfree(b.data.data);
	return out;
}

char *
vexec_devfilter_build(List *quals, Index varno, VexecDevFilterColumn column, void *arg, Size *len,
					  const char **why)
{
	FilterBuild b;
	ListCell   *lc;
	VexecDevFilter hdr;
	char	   *out;

	memset(&b, 0, sizeof(b));
	initStringInfo(&b.steps);
	initStringInfo(&b.data);
	b.varno = varno;
	b.column = column;
	b.arg = arg;
	*len = 0;
	*why = NULL;
	if (quals == NIL)
	{
		*why = "no qual";
		return NULL;
	}
	foreach(lc, quals)
	{
		Expr	   *q = lfirst(lc);

		if (IsA(q, RestrictInfo))
			q = ((RestrictInfo *) q)->clause;
		if (!filter_expr(&b, q))
		{
			*why = b.why;
			return NULL;
		}
	}
	if (list_length(quals) > 1)
		filter_step(&b, VEXEC_DEV_FILTER_AND, 0, (uint32) list_length(quals));
	hdr.nsteps = (uint32) b.nsteps;
	hdr.data_length = (uint32) b.data.len;
	*len = vexec_dev_filter_size(b.nsteps, b.data.len);
	out = palloc(*len);
	memcpy(out, &hdr, sizeof(hdr));
	memcpy(vexec_dev_filter_steps((VexecDevFilter *) out), b.steps.data, b.steps.len);
	memcpy(vexec_dev_filter_data((VexecDevFilter *) out), b.data.data, b.data.len);
	pfree(b.steps.data);
	pfree(b.data.data);
	return out;
}

/* ---------------------------------------------------------------------
 * Chunks read back
 * ---------------------------------------------------------------------
 */

void
vexec_devcursor_init(VexecDevCursor *c, const VexecDevFill *f, const VexecDevStream *s)
{
	c->f = f;
	c->s = s;
	c->piece = 0;
	c->off = 0;
	c->pos = 0;
}

const char *
vexec_devcursor_next(VexecDevCursor *c, uint32 len, char *buf)
{
	const VexecDevStream *s = c->s;
	const VexecDeviceRoutine *r = c->f->task->r;
	uint32		done = 0;
	const char *whole = NULL;

	while (c->piece < s->npieces && c->off == s->pieces[c->piece].length)
	{
		c->piece++;
		c->off = 0;
	}
	if (c->piece < s->npieces && s->pieces[c->piece].length - c->off >= len)
	{
		whole = r->block(s->pieces[c->piece].block) + s->pieces[c->piece].offset + c->off;
		c->off += len;
		c->pos += len;
		return whole;
	}
	while (done < len)
	{
		uint32		take;

		if (c->piece >= s->npieces)
			elog(ERROR, "vexec: a device chunk read past its bytes");
		take = Min(len - done, s->pieces[c->piece].length - c->off);
		memcpy(buf + done, r->block(s->pieces[c->piece].block) + s->pieces[c->piece].offset + c->off, take);
		done += take;
		c->off += take;
		if (c->off == s->pieces[c->piece].length)
		{
			c->piece++;
			c->off = 0;
		}
	}
	c->pos += len;
	return buf;
}

bool
vexec_devfill_isnull(const VexecDevFill *f, int col, uint32 r)
{
	const VexecDevFillCol *c = &f->cols[col];
	const VexecDevStream *s = &c->validity;
	uint64		off = (uint64) (r / 64) * 8;
	uint64		word;

	if (!c->nulls)
		return false;
	for (int i = 0; i < s->npieces; i++)
	{
		if (off < s->pieces[i].length)
		{
			memcpy(&word, f->task->r->block(s->pieces[i].block) + s->pieces[i].offset + off, 8);
			return ((word >> (r % 64)) & 1) == 0;
		}
		off -= s->pieces[i].length;
	}
	elog(ERROR, "vexec: a device chunk's row %u past its validity", r);
	return true;
}
