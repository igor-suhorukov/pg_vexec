/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * device.h
 *	  A vector node's device task: its program, the chunks it gathers from
 *	  its batches, and the answers it reads back, over the device contract
 *	  (vexec_device.h; pg_vector_executor.md §3.11, V8).
 *
 * The provider -- vexec_gpu, an extension of its own -- owns the device
 * and the memory both sides share; this is vexec's side of a task, shared
 * by VecAgg's, VecHashJoin's and VecSort's device strategies:
 *
 *	a task		begun with a program; its requests one at a time, each
 *				waited for, interrupts checked; ended at the node's end, or
 *				by the resource owner after an error
 *	a chunk		rows of the program's columns in the device's layouts,
 *				written from a batch's selected rows into blocks the task
 *				claims: a column's values, its validity, a string column's
 *				offsets and bytes, each a stream of pieces within blocks.
 *				A chunk the device has taken is kept until an answer covers
 *				it, so that a lost executor's work can be redone on the CPU
 *				from it
 *	an answer	a page of columns in blocks the task gives
 *
 * Nothing here raises for the device's sake: a task that cannot be had, a
 * request answered other than OK, a layout without a copy here, all say
 * so, and the node runs on the CPU.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_DEVICE_EXEC_H
#define VEXEC_DEVICE_EXEC_H

#include "vexec_device.h"
#include "vexec_source.h"

#include "batch/batch.h"

/* A buffer of a chunk being written: pieces within the chunk's blocks. */
typedef struct VexecDevStream
{
	VexecDevPiece *pieces;
	int			npieces;
	int			maxpieces;
	uint64		length;
	char	   *ptr;			/* where the next bytes go */
	uint32		room;			/* and how many fit before the piece's end */
	int			next;			/* the piece written next once room is out */
} VexecDevStream;

typedef struct VexecDevFillCol
{
	uint8		type;			/* VexecDevType */
	int16		scale;
	uint8		width;			/* fixed width: bytes a value */
	VexecDevStream values;		/* fixed width: the values; STRING: bytes */
	VexecDevStream validity;	/* uint64 words */
	VexecDevStream offsets;		/* STRING: int32 */
	uint64		vword;			/* validity bits not yet written */
	int			vbits;
	bool		nulls;			/* a NULL among its rows */
	uint32		strlen;			/* STRING: bytes so far, its next offset */
} VexecDevFillCol;

struct VexecDevTask;

/* A chunk: being filled, in flight, or kept until an answer covers it. */
typedef struct VexecDevFill
{
	struct VexecDevTask *task;
	int			ncols;
	VexecDevFillCol *cols;
	uint32		nrows;
	uint32		cap;			/* the rows its fixed-width buffers hold */
	uint32	   *blocks;			/* the blocks it holds */
	int			nblocks;
	int			maxblocks;
	int			bump;			/* the block of blocks[] bytes are cut
								 * from next, */
	uint32		bump_off;		/* and where in it */
	int			spare;			/* blocks claimed for strings' bytes, at
								 * the end of blocks[], not yet taken */
	uint64		first_row;		/* its first row's ordinal among the task's */
} VexecDevFill;

/* What a task did, for EXPLAIN ANALYZE. */
typedef struct VexecDevFigures
{
	int64		chunks;
	int64		rows;
	int64		bytes;			/* the chunks' buffers' */
	int64		answers;		/* answer pages read */
	int64		answer_rows;
	int64		answer_bytes;
	int64		requests;
	double		wait_ms;		/* waiting for answers */
	double		device_ms;		/* the executor's time on the requests */
	double		gpu_ms;			/* of it, the GPU's, by CUDA events (OT10) */
} VexecDevFigures;

typedef struct VexecDevTask
{
	const VexecDeviceRoutine *r;
	VexecDeviceTask *task;
	MemoryContext mcxt;
	int			op;
	uint32		block_size;
	int			share;			/* the blocks the task may hold */
	int			held;			/* and holds now */
	uint32	   *unit_blocks;	/* a units request's deletions', until it is answered */
	int			nunit_blocks;
	uint64		rows_given;		/* rows of its chunks so far */
	int			last_status;
	char		message[256];	/* the last answer's other than OK */
	VexecDevFigures fig;
} VexecDevTask;

/* The device that serves, or NULL: a provider registered whose executor is ready. */
extern const VexecDeviceRoutine *vexec_device_ready(void);

/*
 * W7: of the rows of a relation's attribute, those whose column the device
 * keeps between statements (vexec_device.h's kept_rows), 0 where none is or
 * the provider says nothing of it.
 */
extern double vexec_device_kept_rows(Oid relation, AttrNumber attnum);

/*
 * W38 (pg_vector_executor.md §3.35): the bytes a units request's columns are
 * gathered within -- vexec.gpu_request_share of the device's memory its tasks
 * have not reserved (vexec_device.h's free_bytes) -- PG_UINT64_MAX where the
 * share is 0 or the provider says nothing of its memory.  A unit's bytes, about,
 * as the device decodes them: its source's figure, or its rows times its read
 * columns' widths (vexec_devunit_row_bytes(): their attributes' average
 * widths, a variable-length value's offset beside it), whichever is more.
 */
extern uint64 vexec_device_request_bytes(void);

/*
 * W42 (pg_vector_executor.md §3.35): the device's memory its tasks have not
 * reserved, as the provider publishes it -- what the planner holds a device
 * grouping's groups against -- PG_UINT64_MAX where it says nothing of it.
 */
extern uint64 vexec_device_plan_bytes(void);

/*
 * W46 (pg_vector_executor.md §3.35): whether the provider applies a
 * GROUPBY's HAVING only to the answers whose pages ask for it
 * (VEXEC_DEV_PAGES_HAVING): one that applies it to each is given none.
 */
extern bool vexec_device_having_asked(void);

/*
 * W50 (pg_vector_executor.md §3.36): the bytes the provider keeps columns
 * within between statements -- its cache's limit -- as it publishes them;
 * PG_UINT64_MAX where it says nothing of it.
 */
extern uint64 vexec_device_cache_bytes(void);
extern double vexec_devunit_row_bytes(Oid relation, int n, const int16 *attnums);
extern uint64 vexec_devunit_bytes(const VexecSourceUnit *u, double row_bytes);

/*
 * W20 (devasked.c): a token of a statement's planning, and the rows of a
 * column to price as the device keeps them, the statement's ask of it kept;
 * W50, its bytes as the device decodes them, its trial within the cache.
 */
extern uint64 vexec_device_asked_token(void);
extern double vexec_device_priced_kept(Oid relation, AttrNumber attnum, uint64 statement, double kept,
									   double all, double bytes);

/* Whether a node may take the device's strategy now: vexec.gpu and the provider. */
extern bool vexec_device_wanted(bool planned, bool enabled, const char **why);

/* A column's type as the device holds it, or VEXEC_DEV_NONE. */
extern int	vexec_dev_type_of(const VexecType *type, int16 *scale);
extern const char *vexec_dev_type_name(int type);
extern const char *vexec_dev_status_name(int status);

/*
 * A task begun with its program: the program's bytes, which begin with a
 * VexecDevProgram, written into the request area and answered.  NULL with
 * *why where it is not.
 */
extern VexecDevTask *vexec_devtask_begin(int op, const void *program, Size len,
										 MemoryContext mcxt, const char **why);
extern void vexec_devtask_end(VexecDevTask *t);

/* The status of the request in flight, once it is answered. */
extern int	vexec_devtask_wait(VexecDevTask *t);
extern const VexecDevAnswer *vexec_devtask_answer(VexecDevTask *t);

/*
 * A chunk of ncols columns of these types for up to cap rows, its
 * fixed-width buffers and validity claimed at once, within the task's
 * share less `reserve` blocks; NULL where not even a batch's rows fit.
 */
extern VexecDevFill *vexec_devfill_begin(VexecDevTask *t, int ncols, const VexecDevColType *types,
										 uint32 cap, int reserve);

/*
 * Whether n more rows fit, strings[c] more bytes for each STRING column c
 * (NULL: none), claiming blocks for the strings where they must.
 */
extern bool vexec_devfill_room(VexecDevFill *f, int n, const uint64 *strings);

/* The bytes a batch's selected rows of a STRING column take. */
extern uint64 vexec_devfill_string_bytes(const VexecVec *v, const uint64 *sel, int nrows);

/*
 * Column col's selected rows of v -- sel's rows of the batch's nrows, the
 * rows of the batch being appended -- written in the chunk's layout; false,
 * nothing written, where v's layout has no copy here.  The chunk's row
 * count moves with vexec_devfill_rows().
 */
extern bool vexec_devfill_append(VexecDevFill *f, int col, VexecBatch *work, const VexecVec *v,
								 const uint64 *sel, int nrows, int nsel);
/* A BOOL8 column of the rows where a boolean vector is true and not NULL. */
extern bool vexec_devfill_append_true(VexecDevFill *f, int col, const VexecVec *v,
									  const uint64 *sel, int nrows, int nsel);
extern void vexec_devfill_rows(VexecDevFill *f, int n);

/*
 * A value of a row into column col, as the device holds it: a fixed width's
 * bytes, a STRING's len bytes; the room for a STRING's asked for first.
 */
extern void vexec_devfill_put(VexecDevFill *f, int col, const void *p, uint32 len, bool isnull);

/* Whether every column's layout has a copy here, for a batch about to be appended. */
extern bool vexec_devfill_can_append(const VexecDevFill *f, int col, const VexecVec *v);

/* The chunk to the executor: its request written and submitted; a JOIN's of a side. */
extern void vexec_devfill_submit(VexecDevFill *f);
extern void vexec_devfill_submit_side(VexecDevFill *f, int side);

/* Its blocks released, and itself. */
extern void vexec_devfill_free(VexecDevFill *f);

/*
 * V8e: a unit the executor reads from its file itself (vexec_source.h's
 * VexecSourceUnit), its columns those of a task's program: each one's name
 * in the file, in the program's order.
 */
typedef struct VexecDevUnitDesc
{
	int			format;			/* VEXEC_DEV_UNIT_* */
	char	   *path;
	uint64		file_size;
	int64		file_mtime;
	int			nparts;
	int32	   *parts;
	uint64		rows;
	uint64	   *deleted;		/* rows bits, 1 = a row the snapshot does
								 * not see; NULL: it sees every one */
	char	  **names;			/* per program column, then per filter column (W4) */
	int			nnames;
	void	   *token;			/* the source's, for read_unit() */
	VexecSourceUnit unit;		/* the source's description, its pointers
								 * these copies' */
	/* W6: a MERGETREE unit's streams, each a column read's, and its source stream */
	int			nstreams;
	struct VexecDevUnitStream *streams;
	/*
	 * W21 (§3.28): a MERGETREE unit's part's identity and its granules' rows,
	 * nparts of them, as the source gave them (vexec_source.h's minor 11), for
	 * the condition cache; granule_rows NULL where it did not
	 */
	uint64		unit_id[2];
	uint32		ngranules;
	uint32	   *granule_rows;
} VexecDevUnitDesc;

typedef struct VexecDevUnitStream
{
	int			column;			/* the column read: the program's, then the filter's */
	int			kind;			/* VEXEC_DEV_STREAM_* */
	int			width;
	uint64		length;			/* its compressed bytes */
	uint64		skip;
	uint32		rows;			/* W26: its range's rows; 0, the unit's */
	int			source;			/* the source's stream it is, for its bytes */
	bool		kept;			/* W5: its column the device keeps, not sent */
} VexecDevUnitStream;

/* W6: bytes [offset, offset + length) of a unit's stream into dest, from its source */
typedef void (*VexecDevUnitBytes) (void *arg, const VexecDevUnitDesc *unit, int stream,
								   uint64 offset, uint64 length, void *dest);

/*
 * W4: a scan's quals -- an implicit AND, of Vars of varno, as clauses or
 * RestrictInfos -- as a units request's filter (vexec_device_format.h,
 * VexecDevFilter): column(arg, attnum, typid, collid) gives an attribute's
 * filter column, -1 where the units do not hold it.  The filter's bytes, in
 * the current context, or NULL with *why where a qual has no form the
 * device's filter takes.
 */
typedef int (*VexecDevFilterColumn) (void *arg, AttrNumber attnum, Oid typid, Oid collid);
extern int	vexec_devfilter_type(Oid typid);
extern char *vexec_devfilter_build(List *quals, Index varno, VexecDevFilterColumn column, void *arg,
								   Size *len, const char **why);

/*
 * W10 (§3.27): an expression over the relation's columns as a units
 * request's computed column (VEXEC_DEV_UCOL_EXPR): its program's bytes, its
 * device type into *type; NULL with *why where the device's machine does not
 * make it.  vexec_devexpr_int_extract(): whether it is extract() of an
 * integer field, INT64 on the device, which a key takes as the numeric.
 */
extern char *vexec_devexpr_build(Expr *e, Index varno, VexecDevFilterColumn column, void *arg, int *type,
								 Size *len, const char **why);
extern bool vexec_devexpr_int_extract(Expr *e);

/*
 * A units request (VEXEC_DEV_REQ_UNITS) written and submitted: false,
 * nothing submitted, where the units do not fit in the request area, or
 * their deletions' bitmaps in the task's share of blocks.  The bitmaps'
 * blocks are the task's until vexec_devtask_units_done(), once the request
 * is answered.  W7: relation is the units' relation, attnums each column
 * read's attribute of it, which the executor's cache publishes what it
 * keeps by (the planner's price, vexec_device_kept_rows()).
 */
extern bool vexec_devtask_units(VexecDevTask *t, int ncols, int nqcols, const uint8 *flags,
								const VexecDevColType *qtypes, Oid relation, const int16 *attnums,
								const void *filter, Size filter_len, const void *exprs, Size exprs_len,
								int nunits, VexecDevUnitDesc *const *units, uint64 first_row,
								VexecDevUnitBytes bytes, void *bytes_arg, uint32 req_flags);
extern void vexec_devtask_units_done(VexecDevTask *t);

/*
 * A unit a source handed, copied for a task's units requests: nread columns
 * read, column c the source's spec column cols[c] or -1 for one the
 * executor computes or makes, the unit's checks added into flags[c]; freed
 * by vexec_devunit_free().  vexec_devunits_probe(): the columns the device
 * keeps of MERGETREE units, marked kept, their bytes then not sent (W5, W6).
 */
extern VexecDevUnitDesc *vexec_devunit_copy(MemoryContext mcxt, const VexecSourceUnit *u, int nread,
											const int *cols, uint8 *flags);
extern void vexec_devunit_free(VexecDevUnitDesc *x);
extern void vexec_devunits_probe(VexecDevTask *t, int ncols, int nqcols, const uint8 *flags,
								 const VexecDevColType *qtypes, Oid relation, const int16 *attnums,
								 VexecDevUnitDesc **arr, int n);

/*
 * Blocks for the task's answers, which the executor writes and this process
 * reads: the provider's claim_answers() where it has one (vexec_device.h's
 * minor version 2), not faulted in for writing; else its claim()
 */
extern int	vexec_devtask_claim_answers(VexecDevTask *t, int n, uint32 *blocks);

/*
 * The next answer page, into npages blocks claimed for it and kept by the
 * caller: the status, OK for the last page, MORE before it.  flags are the
 * request's (VEXEC_DEV_PAGES_*), which its first page reads.
 */
extern int	vexec_devtask_answer_page(VexecDevTask *t, int npages, const uint32 *pages,
									  uint32 flags);

/* An answer column's buffers in this process, or NULL for none. */
extern const char *vexec_devans_values(VexecDevTask *t, const VexecDevAnsCol *c);
extern const uint64 *vexec_devans_validity(VexecDevTask *t, const VexecDevAnsCol *c);
extern const int32 *vexec_devans_offsets(VexecDevTask *t, const VexecDevAnsCol *c);

/*
 * A chunk's rows read back, column by column, row by row, as the redo of a
 * lost executor's work reads them (§3.11, "Falling back"): a cursor a column.
 */
typedef struct VexecDevCursor
{
	const VexecDevFill *f;
	int			col;
	const VexecDevStream *s;
	int			piece;
	uint32		off;			/* within the piece */
	uint64		pos;			/* within the stream */
} VexecDevCursor;

extern void vexec_devcursor_init(VexecDevCursor *c, const VexecDevFill *f, const VexecDevStream *s);
/* The next len bytes of the stream into buf, or a pointer to them where they lie whole. */
extern const char *vexec_devcursor_next(VexecDevCursor *c, uint32 len, char *buf);
/* Whether row r of a column is NULL, from its validity. */
extern bool vexec_devfill_isnull(const VexecDevFill *f, int col, uint32 r);

#endif							/* VEXEC_DEVICE_EXEC_H */
