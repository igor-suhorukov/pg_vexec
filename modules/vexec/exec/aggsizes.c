/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * aggsizes.c
 *	  The groups each VecAgg's table reached, kept in shared memory to size
 *	  the table the next run of the same node begins with (C6 of
 *	  pg_vector_executor.md §3.19).
 *
 * VecAgg sizes its table by the planner's estimate of the groups, and the
 * estimates of ClickBench's GROUP BYs are 4 to 215 times low (§3.19): a
 * table begun too small grows as it fills, a rehash of every group each
 * time it doubles, and costs 43% to 48% more a row for Q15's and Q32's
 * shapes.  ClickHouse keeps the sizes its tables reached, by query and
 * aggregation, and sizes the next run's by them
 * (collect_hash_table_stats_during_aggregation,
 * CH:src/Interpreters/HashTablesStatistics.h).  So does vexec: a slot of a
 * fixed array in shared memory a node, keyed by the statement's query id --
 * or, where PostgreSQL computes none (compute_query_id off, or auto with no
 * module asking for one), the hash of its text -- and the node's plan id;
 * the most groups a participant's table held.
 *
 * A slot is a hint: two nodes whose keys share it take turns, a write racing
 * another may pair one's key with the other's count, and a statement whose
 * groups vary with its parameters is sized for the most it had.  The size
 * changes how soon a table grows, never what it holds.  Off
 * (vexec.remember_agg_sizes), nothing is read or kept.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"

#include "vexec.h"
#include "exec/exec.h"

#define VEXEC_AGGSIZE_SLOTS		8192

typedef struct AggSizeSlot
{
	pg_atomic_uint64 key;
	pg_atomic_uint64 groups;
} AggSizeSlot;

static AggSizeSlot *slots = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;

static void
aggsize_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestAddinShmemSpace(mul_size(VEXEC_AGGSIZE_SLOTS, sizeof(AggSizeSlot)));
}

static void
aggsize_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	slots = ShmemInitStruct("vexec aggregation sizes",
							mul_size(VEXEC_AGGSIZE_SLOTS, sizeof(AggSizeSlot)), &found);
	if (!found)
	{
		for (int i = 0; i < VEXEC_AGGSIZE_SLOTS; i++)
		{
			pg_atomic_init_u64(&slots[i].key, 0);
			pg_atomic_init_u64(&slots[i].groups, 0);
		}
	}
	LWLockRelease(AddinShmemInitLock);
}

/* From _PG_init: the slots are made with the postmaster's shared memory. */
void
vexec_aggsize_install(void)
{
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = aggsize_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = aggsize_shmem_startup;
}

/* A node's key: its statement's query id, or its text's hash, and its plan id. */
uint64
vexec_aggsize_key(EState *estate, int plan_node_id)
{
	uint64		q = estate->es_plannedstmt != NULL ? estate->es_plannedstmt->queryId : 0;

	if (q == 0 && estate->es_sourceText != NULL)
		q = hash_bytes_extended((const unsigned char *) estate->es_sourceText,
								(int) strlen(estate->es_sourceText), 0);
	/* 0 stands for an empty slot */
	return hash_combine64(q, (uint64) (uint32) plan_node_id) | 1;
}

/* The most groups the node's table held, as far as its slot says; 0: none. */
int64
vexec_aggsize_get(uint64 key)
{
	AggSizeSlot *s;

	if (slots == NULL || !vexec_remember_agg_sizes)
		return 0;
	s = &slots[key % VEXEC_AGGSIZE_SLOTS];
	if (pg_atomic_read_u64(&s->key) != key)
		return 0;
	return (int64) pg_atomic_read_u64(&s->groups);
}

/* The groups a table of the node held, kept where they are the most. */
void
vexec_aggsize_put(uint64 key, int64 groups)
{
	AggSizeSlot *s;

	if (slots == NULL || groups <= 0 || !vexec_remember_agg_sizes)
		return;
	s = &slots[key % VEXEC_AGGSIZE_SLOTS];
	if (pg_atomic_read_u64(&s->key) == key &&
		pg_atomic_read_u64(&s->groups) >= (uint64) groups)
		return;
	pg_atomic_write_u64(&s->key, key);
	pg_atomic_write_u64(&s->groups, (uint64) groups);
}
