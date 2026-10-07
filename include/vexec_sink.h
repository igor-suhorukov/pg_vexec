/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_sink.h
 *	  The batch-sink contract: how a table access method takes the rows of an
 *	  INSERT from vexec a batch of columns at a time (pg_vector_executor.md
 *	  §3.16).
 *
 * The mirror of vexec_source.h's sources.  A storage module -- PAX, gp_ao,
 * or any access method outside the port -- fills a VexecSinkRoutine and
 * registers it from its _PG_init.  vexec's VecInsert finds it by the
 * relation's rd_tableam and writes each batch's columns through it, where
 * it would otherwise form rows for table_multi_insert().  Neither links the
 * other: the registry is reached through a rendezvous variable, and this
 * header holds only types, macros and inline functions.
 *
 * The calls, for one relation in one statement:
 *
 *	supports()	for each of the relation's attributes that is not dropped,
 *				the layout the sink takes its values in -- vexec_source.h's
 *				VexecLayout, with the width, stride and scale FIXED and SCALED
 *				read -- or false: the sink takes no batch of this relation,
 *				and VecInsert forms rows for table_multi_insert() instead;
 *	begin()		the sink's state for the statement's writes into the
 *				relation, in the memory context current at the call;
 *	put()		a batch: nrows rows, a column per attribute in attribute
 *				order, each flat, in the layout supports() gave, at
 *				PostgreSQL's epochs (arrow_values false); a dropped
 *				attribute's column is all NULL.  The rows are written in the
 *				current transaction under the spec's command id, as
 *				table_multi_insert() writes them, and the module's own end of
 *				statement flushes them.  With VEXEC_SINK_TIDS, tids[] gets
 *				each row's TID, as tts_tid holds it after
 *				table_tuple_insert(), for the indexes, and the rows are where
 *				an index's uniqueness check finds them.  put() counts its rows
 *				for pgstat;
 *	end()		the statement is done with the sink.
 *
 * NOT NULL and CHECK constraints, defaults, generated columns, partitions
 * and index entries stay VecInsert's: a sink only stores.  It keeps its
 * module's own limits -- groups, files, blocks, row numbers -- and splits a
 * batch where they fall.  OFFSETS are nvalues + 1 int32 offsets into
 * buffers[0], starting anywhere; a NULL row's pair is empty.  DATUM values
 * are Datums of headered values, never external or compressed.
 *
 * Every call runs on the backend's main thread and may ereport; an error
 * aborts the transaction, whose callbacks release what the module's writer
 * held, as for an INSERT that failed.
 *
 * Versions: as vexec_source.h's.  The major version is in the rendezvous
 * name; minor additions go at the end of a struct, whose "size" says how
 * much of it the module that filled it was built with.
 *
 * vexec installs this header with itself, beside vexec_source.h.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_SINK_H
#define VEXEC_SINK_H

#include "vexec_source.h"		/* VexecLayout, VexecColumn */

#define VEXEC_SINK_RENDEZVOUS	"vexec/sink_v1" /* the major version is in the
												 * name */
#define VEXEC_SINK_MINOR		0

/* The layout a sink takes an attribute's values in. */
typedef struct VexecSinkLayout
{
	uint8		layout;			/* VexecLayout: FIXED, BYTE_BOOL, BIT_BOOL,
								 * SCALED, DATUM or OFFSETS */
	int16		scale;			/* SCALED: the typmod's scale */
	int32		width;			/* FIXED: value bytes; SCALED: 8 or 16 */
	int32		stride;			/* FIXED: bytes from one value to the next */
} VexecSinkLayout;

/* What a statement's writes into a relation are. */
typedef struct VexecSinkSpec
{
	Size		size;			/* sizeof as the caller was built */
	int			natts;			/* the relation's attributes, dropped ones
								 * included */
	const VexecSinkLayout *layouts; /* per attribute, as supports() gave */
	CommandId	cid;
	int			options;		/* table_multi_insert()'s TABLE_INSERT_* */
	uint32		flags;			/* VEXEC_SINK_TIDS */
} VexecSinkSpec;

#define VEXEC_SINK_TIDS		0x0001	/* put() gives each row's TID */

typedef struct VexecSinkBatch
{
	int			nrows;
	const VexecColumn *columns; /* natts of them, flat, attribute order */
	ItemPointerData *tids;		/* VEXEC_SINK_TIDS: filled, nrows of them */
} VexecSinkBatch;

typedef struct VexecSinkRoutine
{
	Size		size;			/* later members read as absent for an older
								 * module */
	int			minor;
	const TableAmRoutine *am;	/* the key: rel->rd_tableam */
	const char *name;			/* EXPLAIN's */
	bool		(*supports) (Relation rel, AttrNumber attnum, VexecSinkLayout *layout);
	void	   *(*begin) (Relation rel, const VexecSinkSpec *spec);
	void		(*put) (void *state, VexecSinkBatch *batch);
	void		(*end) (void *state);
} VexecSinkRoutine;

/*
 * The registry the rendezvous variable points at, made by whichever module
 * comes first while the postmaster loads libraries; never freed.
 */
#define VEXEC_SINK_MAGIC		0x56585331	/* "VXS1" */
#define VEXEC_SINK_MAX			64

typedef struct VexecSinkRegistry
{
	uint32		magic;
	int			nsinks;
	const VexecSinkRoutine *sinks[VEXEC_SINK_MAX];
} VexecSinkRegistry;

/* The registry, made if no module has made it yet. */
static inline VexecSinkRegistry *
vexec_sink_registry(void)
{
	VexecSinkRegistry **rv;

	rv = (VexecSinkRegistry **) find_rendezvous_variable(VEXEC_SINK_RENDEZVOUS);
	if (*rv == NULL)
	{
		VexecSinkRegistry *reg;

		reg = (VexecSinkRegistry *)
			MemoryContextAllocZero(TopMemoryContext, sizeof(VexecSinkRegistry));
		reg->magic = VEXEC_SINK_MAGIC;
		*rv = reg;
	}
	else if ((*rv)->magic != VEXEC_SINK_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("the rendezvous variable \"%s\" does not hold vexec's sink registry",
						VEXEC_SINK_RENDEZVOUS)));
	return *rv;
}

/*
 * Register a sink, from a storage module's _PG_init, only while the
 * postmaster preloads libraries, as sources are.  The routine must stay
 * valid for the life of the process.
 */
static inline void
vexec_register_sink(const VexecSinkRoutine *routine)
{
	VexecSinkRegistry *reg;
	int			i;

	if (!process_shared_preload_libraries_in_progress)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a vexec batch sink can only be registered while \"shared_preload_libraries\" are loaded")));
	if (routine == NULL || routine->am == NULL ||
		routine->size < offsetof(VexecSinkRoutine, end) + sizeof(routine->end))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("a vexec batch sink must name its table access method")));

	reg = vexec_sink_registry();
	for (i = 0; i < reg->nsinks; i++)
		if (reg->sinks[i]->am == routine->am)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("a vexec batch sink is already registered for this table access method")));
	if (reg->nsinks >= VEXEC_SINK_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("too many vexec batch sinks"),
				 errdetail("At most %d can be registered.", VEXEC_SINK_MAX)));
	reg->sinks[reg->nsinks++] = routine;
}

/* The sink registered for an access method, or NULL. */
static inline const VexecSinkRoutine *
vexec_find_sink(const TableAmRoutine *am)
{
	VexecSinkRegistry **rv;
	VexecSinkRegistry *reg;
	int			i;

	rv = (VexecSinkRegistry **) find_rendezvous_variable(VEXEC_SINK_RENDEZVOUS);
	reg = *rv;
	if (reg == NULL || reg->magic != VEXEC_SINK_MAGIC)
		return NULL;
	for (i = 0; i < reg->nsinks; i++)
		if (reg->sinks[i]->am == am)
			return reg->sinks[i];
	return NULL;
}

/* Whether a routine has a member: it was built with a struct that long. */
#define VEXEC_SINK_HAS(routine, member) \
	((routine)->size >= offsetof(VexecSinkRoutine, member) + sizeof((routine)->member) && \
	 (routine)->member != NULL)

#endif							/* VEXEC_SINK_H */
