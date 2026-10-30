/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec_device.h
 *	  The device contract: how vexec's nodes hand an operator to a device
 *	  provider, an extension of its own that owns a device and the
 *	  process that runs it (pg_vector_executor.md §3.11, V8).
 *
 * GPU execution is a second strategy of VecAgg, VecHashJoin and VecSort,
 * over input the CPU has decoded and filtered.  vexec keeps the strategy's
 * planning and its node's side -- which nodes may take it, the chunks it
 * gathers from its batches, the answers merged back, every fallback to its
 * CPU strategy -- and knows nothing of CUDA.  A provider (vexec_gpu,
 * modules/vexec_gpu) keeps the rest: the arena of memory both sides map,
 * the executor that owns the device, its requests and their answers.  So a
 * server without a provider, or with one whose executor is not running,
 * runs every node on the CPU, with no other plan.
 *
 * What a request and an answer say is vexec_device_format.h's, a layout
 * the provider's executor reads without PostgreSQL's headers.  A task's
 * process writes its request into the task's request area and calls
 * submit(); wait() returns the answer's status once the answer is in the
 * task's answer area.  The memory a chunk's columns and an answer's pages
 * are written into is the provider's blocks, claimed and released by the
 * task: block_size bytes each, at the same block numbers in every process.
 *
 * The provider's rules:
 *
 *	- ready() and begin() never raise: they answer false and NULL where no
 *	  executor serves, or no task can be had, and the node runs on the CPU;
 *	- wait() checks for interrupts while it waits, and so may raise a
 *	  cancel; a lost executor is an answer, VEXEC_DEV_LOST, never an error;
 *	- a task is the current resource owner's: an error that ends the
 *	  statement ends its tasks -- its requests cancelled, its blocks
 *	  released once the executor has let them go -- as end() would;
 *	- end() may be called once more after the resource owner's cleanup,
 *	  and does nothing then.
 *
 * Registration.  The provider registers from its _PG_init while the
 * postmaster preloads libraries, before or after vexec: whichever comes
 * first makes the rendezvous variable, and vexec reads it as a node needs
 * it.  One provider a server; the routine and what it points to are
 * static data, valid for the life of the process.
 *
 * Versions: the major version is in the rendezvous name; minor additions
 * go at the end of VexecDeviceRoutine, whose size says how much of it the
 * provider was built with.
 *
 * vexec installs this header and vexec_device_format.h with itself, into
 * the server's include directory under extension/vexec/.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_DEVICE_H
#define VEXEC_DEVICE_H

#include "fmgr.h"

#include "vexec_device_format.h"

#define VEXEC_DEVICE_RENDEZVOUS		"vexec/device_v1"	/* the major version
														 * is in the name */
#define VEXEC_DEVICE_MINOR			5	/* 1: kept_rows (W7); 2: claim_answers (PQ5);
										 * 3: free_bytes (W38); 4: a GROUPBY's
										 * HAVING applied to an answer only
										 * where its pages ask for it,
										 * VEXEC_DEV_PAGES_HAVING (W46); 5:
										 * cache_bytes (W50) */
#define VEXEC_DEVICE_ROUTINE_MAGIC	0x56584431	/* "VXD1" */

/* A provider's task: one operator of one node of this process. */
typedef struct VexecDeviceTask VexecDeviceTask;

typedef struct VexecDeviceRoutine
{
	uint32		magic;			/* VEXEC_DEVICE_ROUTINE_MAGIC */
	Size		size;			/* sizeof as the provider was built */
	int			minor;			/* VEXEC_DEVICE_MINOR it was built with */
	const char *name;			/* the provider's, for EXPLAIN: "vexec_gpu" */

	/* Whether an executor serves now: what the planner and a node ask. */
	bool		(*ready) (void);

	/* The device that serves, for EXPLAIN: its name, or NULL. */
	const char *(*device_name) (void);

	/* The bytes of a block. */
	uint32		(*block_size) (void);

	/*
	 * The blocks a task may hold at once: its share of the arena among the
	 * tasks that hold blocks now and this one.
	 */
	int			(*task_share) (void);

	/* A task, or NULL where none can be had now. */
	VexecDeviceTask *(*begin) (void);

	/* The task's request area, where its process writes a request. */
	void	   *(*request_area) (VexecDeviceTask *task, Size *size);

	/* The task's answer area, which an answered request has filled. */
	const VexecDevAnswer *(*answer) (VexecDeviceTask *task);

	/* Up to n free blocks for the task; how many, 0 where none is free. */
	int			(*claim) (VexecDeviceTask *task, int n, uint32 *blocks);

	/* Blocks the task holds, given back. */
	void		(*release) (VexecDeviceTask *task, int n, const uint32 *blocks);

	/* A block's address in this process. */
	char	   *(*block) (uint32 block);

	/* The request area's request, to the executor. */
	void		(*submit) (VexecDeviceTask *task);

	/*
	 * The status of the request last submitted, once it is answered, or
	 * VEXEC_DEV_PENDING after timeout_ms; -1 waits until it is answered.
	 */
	int			(*wait) (VexecDeviceTask *task, int timeout_ms);

	/* The task over: its device state freed, its blocks released. */
	void		(*end) (VexecDeviceTask *task);

	/*
	 * From minor version 1 (W7, pg_vector_executor.md §3.27): of the rows of
	 * attribute attnum of relation in this database, those whose column the
	 * device keeps between statements, as its executor publishes them -- 0
	 * where it keeps none, or no executor serves.  The planner's price; it
	 * never raises.
	 */
	double		(*kept_rows) (Oid relation, int16 attnum);

	/*
	 * From minor version 2 (pg_vector_executor.md §3.34's PQ5): up to n free
	 * blocks for the task's answers, which the executor writes and this
	 * process only reads -- not faulted in for writing as claim()'s are, a
	 * block of a fresh process's 4 MB of page tables made for a few bytes
	 * of answer.  As claim() otherwise.
	 */
	int			(*claim_answers) (VexecDeviceTask *task, int n, uint32 *blocks);

	/*
	 * From minor version 3 (W38, pg_vector_executor.md §3.35): the bytes of
	 * the executor's device memory its tasks have not reserved -- the columns
	 * it keeps among them, which it gives back before it refuses a request --
	 * as it publishes them; 0 where no executor serves.  What a units request
	 * is sized by; it never raises.
	 */
	uint64		(*free_bytes) (void);

	/*
	 * From minor version 5 (W50, pg_vector_executor.md §3.36): the bytes the
	 * executor keeps columns within between statements -- its cache's limit
	 * -- as it publishes them; 0 where no executor serves.  What the planner
	 * holds the columns it prices as kept on trial against (W20); it never
	 * raises.
	 */
	uint64		(*cache_bytes) (void);
} VexecDeviceRoutine;

/* The provider's registration, from its _PG_init. */
static inline void
vexec_device_register(const VexecDeviceRoutine *routine)
{
	const VexecDeviceRoutine **rv;

	rv = (const VexecDeviceRoutine **) find_rendezvous_variable(VEXEC_DEVICE_RENDEZVOUS);
	if (*rv != NULL && *rv != routine)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a device provider, \"%s\", is already registered", (*rv)->name),
				 errdetail("A server has one device provider.")));
	*rv = routine;
}

/* The provider, or NULL: vexec's side. */
static inline const VexecDeviceRoutine *
vexec_device_provider(void)
{
	const VexecDeviceRoutine **rv;

	rv = (const VexecDeviceRoutine **) find_rendezvous_variable(VEXEC_DEVICE_RENDEZVOUS);
	if (*rv == NULL || (*rv)->magic != VEXEC_DEVICE_ROUTINE_MAGIC)
		return NULL;
	return *rv;
}

#endif							/* VEXEC_DEVICE_H */
