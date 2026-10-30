/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * pool.c
 *	  vexec's own long-lived parallel workers (pg_vector_executor.md §3.28,
 *	  F3; §3.38, "After VP5, read again"): background workers of a database
 *	  and a role, started once and kept, each running a parallel plan's
 *	  partial fragment for a leader in place of a parallel worker the
 *	  postmaster forks for the query -- its relation cache, the engine's
 *	  tables and the parts it loaded warm, its memory paged in.  ClickBench's
 *	  queries at 100M rows did their workers' first useful work 7 to 12 ms
 *	  into a query of 20.
 *
 * The pool is off by default: vexec.pool_workers, the postmaster's, is its
 * size, 0 none (the user's decision, 2026-10-30).  With a pool,
 * vexec.enable_pool_workers lets a session use it.
 *
 * The leader.  A parallel plan's Gather or Gather Merge makes its DSM as it
 * always does (ExecInitParallelPlan()): the fixed state, the leader's GUCs,
 * libraries, snapshots and session, the plan, its tuple queues and its
 * nodes' shared state.  As one of vexec's parallel-aware nodes in the
 * fragment initializes its part of it, the leader reserves idle pool
 * workers of its database and role for the worker numbers from 0, as many as
 * the Gather asks for and the pool has, and takes them out of PostgreSQL's
 * hands: their error queues detached, as LaunchParallelWorkers() detaches
 * those of workers it could not register, the context's workers to launch
 * none and its workers launched the pool's -- so the Gather reads their
 * tuple queues, and launches none of its own.  The reserved workers start
 * once the leader first runs a vexec node of the fragment, every node's
 * shared state then made.  A pool worker's errors are not PostgreSQL's: it
 * records the first in its slot, and the leader raises it as its executor
 * finishes (ExecutorFinish), once each of its pool workers is done with its
 * job -- the Gather has read their queues to their ends by then, or has let
 * go of them.
 *
 * Taken only where nothing of the leader's could differ in a process of its
 * own: a SELECT, its transaction without an XID -- no row the leader has
 * written, no combo CID -- not SERIALIZABLE (its predicate locks are the
 * leader's sxact's), the leader participating (it starts its workers), not
 * debug_parallel_query's single-copy Gathers, and a Gather on the main
 * plan's top chain -- under nodes that never run it again -- so that no
 * rescan makes its DSM anew while a worker of the last run may still write
 * there.  The pool is of the leader's database and authenticated role.
 *
 * The worker.  A slot of the pool's shared memory each; idle, it waits on
 * its latch.  A job: the DSM attached, the leader's start timestamps,
 * session and current users, transaction snapshot and active snapshot,
 * per-session DSM, libraries and GUCs restored as ParallelWorkerMain()
 * restores them, then ParallelQueryMain() -- PostgreSQL's own -- runs the
 * fragment; then the transaction committed, the segments detached, the
 * caches kept.  Not restored, nothing of them a transaction without an XID
 * has: combo CIDs, the relation map, the reindex state, uncommitted enums,
 * pending syncs.  A pool worker is no member of the leader's lock group: it
 * takes the fragment's relations' locks first, none waited for, and runs no
 * part of the plan where one is not had at once -- a lock another backend
 * awaits, which would wait for the leader, which waits for it -- its tuple
 * queue attached and detached, so that the leader reads none of it, the
 * other participants doing its share.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>

#include "access/parallel.h"
#include "access/session.h"
#include "access/xact.h"
#include "executor/execParallel.h"
#include "executor/executor.h"
#include "libpq/pqsignal.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "funcapi.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "nodes/readfuncs.h"
#include "optimizer/optimizer.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/dsm.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "vexec.h"
#include "exec/exec.h"

/*
 * PostgreSQL's keys of a parallel context's DSM (PG19:src/backend/access/
 * transam/parallel.c, executor/execParallel.c), which a pool worker reads as
 * ParallelWorkerMain() and ParallelQueryMain() read them
 */
#define POOL_PARALLEL_MAGIC				0x50477c7c
#define POOL_KEY_LIBRARY				UINT64CONST(0xFFFFFFFFFFFF0003)
#define POOL_KEY_GUC					UINT64CONST(0xFFFFFFFFFFFF0004)
#define POOL_KEY_TRANSACTION_SNAPSHOT	UINT64CONST(0xFFFFFFFFFFFF0006)
#define POOL_KEY_ACTIVE_SNAPSHOT		UINT64CONST(0xFFFFFFFFFFFF0007)
#define POOL_KEY_SESSION_DSM			UINT64CONST(0xFFFFFFFFFFFF000A)
#define POOL_KEY_PLANNEDSTMT			UINT64CONST(0xE000000000000002)
#define POOL_KEY_TUPLE_QUEUE			UINT64CONST(0xE000000000000005)
#define POOL_TUPLE_QUEUE_SIZE			65536

#define POOL_MAX_SLOTS		64
#define POOL_MESSAGE		256

typedef enum PoolState
{
	POOL_FREE = 0,
	POOL_STARTING,				/* registered, not yet up */
	POOL_IDLE,
	POOL_RESERVED,				/* a leader's, not yet started */
	POOL_RUNNING
} PoolState;

typedef struct PoolSlot
{
	PoolState	state;
	pid_t		pid;
	ProcNumber	procno;
	Oid			dbid;
	Oid			roleid;
	/* the job, as the leader set it */
	uint64		job;
	dsm_handle	seg;
	int			worker_number;
	pid_t		leader_pid;
	ProcNumber	leader_procno;
	TimestampTz xact_ts;
	TimestampTz stmt_ts;
	Oid			session_user_id;
	bool		session_is_superuser;
	Oid			outer_user_id;
	bool		role_is_superuser;
	Oid			current_user_id;
	int			sec_context;
	/* the worker's answer */
	uint64		cancel_job;		/* the leader's ask that the job stop */
	uint64		done_job;		/* the last job it ended, done or failed */
	bool		failed;			/* done_job's */
	bool		joined;			/* done_job's: it ran its part of the plan */
	int			sqlerrcode;
	char		message[POOL_MESSAGE];
} PoolSlot;

typedef struct PoolShared
{
	slock_t		mutex;
	uint64		next_job;
	int			nslots;
	PoolSlot	slots[FLEXIBLE_ARRAY_MEMBER];
} PoolShared;

/* A leader's job of a slot, in its transaction's memory */
typedef struct PoolJob
{
	int			slotno;
	uint64		job;
	EState	   *estate;
	bool		started;
} PoolJob;

static int	vexec_pool_workers = 0;
static bool vexec_enable_pool_workers = true;
static int	vexec_pool_idle_timeout = 300;

static PoolShared *pool = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;

/* the leader's */
static List *jobs = NIL;
int			vexec_pool_pending = 0;	/* of them, not yet started */
static List *reserved_contexts = NIL;	/* the parallel contexts given pool workers */
static int64 jobs_given = 0;	/* since the backend started, for vexec.pool() */

/* the worker's */
static int	my_slot = -1;
static volatile uint64 my_job = 0;

static Size
pool_size(void)
{
	return add_size(offsetof(PoolShared, slots), mul_size(Max(vexec_pool_workers, 0), sizeof(PoolSlot)));
}

static void
pool_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	if (vexec_pool_workers > 0)
		RequestAddinShmemSpace(pool_size());
}

static void
pool_shmem_startup(void)
{
	bool		found;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();
	if (vexec_pool_workers <= 0)
		return;
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	pool = ShmemInitStruct("vexec pool", pool_size(), &found);
	if (!found)
	{
		memset(pool, 0, pool_size());
		SpinLockInit(&pool->mutex);
		pool->nslots = vexec_pool_workers;
		pool->next_job = 1;
	}
	LWLockRelease(AddinShmemInitLock);
}

/* ---------------------------------------------------------------------
 * The leader
 * ---------------------------------------------------------------------
 */

/* Whether a plan node never runs its child again, so that a Gather beneath it is run once */
static Plan *
once_child(Plan *plan)
{
	switch (nodeTag(plan))
	{
		case T_Limit:
		case T_Sort:
		case T_IncrementalSort:
		case T_Agg:
		case T_Result:
		case T_Unique:
		case T_WindowAgg:
		case T_ProjectSet:
		case T_Material:
			return outerPlan(plan);
		case T_SubqueryScan:
			return ((SubqueryScan *) plan)->subplan;
		case T_CustomScan:
			{
				CustomScan *cs = (CustomScan *) plan;

				/*
				 * vexec's nodes of one child run it once, as these do: its
				 * child in lefttree, never custom_plans (plan/build.c)
				 */
				if (strncmp(cs->methods->CustomName, "Vec", 3) == 0 && cs->custom_plans == NIL &&
					outerPlan(plan) != NULL && innerPlan(plan) == NULL)
					return outerPlan(plan);
				return NULL;
			}
		default:
			return NULL;
	}
}

/* Whether a plan node is within a plan's tree */
static bool
plan_contains(Plan *top, Plan *node)
{
	ListCell   *lc;

	if (top == NULL)
		return false;
	if (top == node)
		return true;
	if (plan_contains(top->lefttree, node) || plan_contains(top->righttree, node))
		return true;
	switch (nodeTag(top))
	{
		case T_Append:
			foreach(lc, ((Append *) top)->appendplans)
				if (plan_contains(lfirst(lc), node))
					return true;
			break;
		case T_MergeAppend:
			foreach(lc, ((MergeAppend *) top)->mergeplans)
				if (plan_contains(lfirst(lc), node))
					return true;
			break;
		case T_CustomScan:
			foreach(lc, ((CustomScan *) top)->custom_plans)
				if (plan_contains(lfirst(lc), node))
					return true;
			break;
		case T_SubqueryScan:
			return plan_contains(((SubqueryScan *) top)->subplan, node);
		default:
			break;
	}
	return false;
}

/* Whether an expression's type is an anonymous record's, which a parallel query's session registry numbers */
static bool
record_walker(Node *node, void *context)
{
	Oid			type = InvalidOid;

	if (node == NULL)
		return false;
	/* the expressions a type is read from; an aggregate's SortGroupClauses and the like have none */
	switch (nodeTag(node))
	{
		case T_Var:
			if (((Var *) node)->varattno == 0)
				return true;	/* a whole row */
			type = ((Var *) node)->vartype;
			break;
		case T_RowExpr:
		case T_RowCompareExpr:
			return true;
		case T_Const:
			type = ((Const *) node)->consttype;
			break;
		case T_Param:
			type = ((Param *) node)->paramtype;
			break;
		case T_FuncExpr:
			type = ((FuncExpr *) node)->funcresulttype;
			break;
		case T_OpExpr:
			type = ((OpExpr *) node)->opresulttype;
			break;
		case T_Aggref:
			type = ((Aggref *) node)->aggtype;
			break;
		case T_WindowFunc:
			type = ((WindowFunc *) node)->wintype;
			break;
		case T_SubPlan:
			type = ((SubPlan *) node)->firstColType;
			break;
		case T_CoerceViaIO:
			type = ((CoerceViaIO *) node)->resulttype;
			break;
		case T_RelabelType:
			type = ((RelabelType *) node)->resulttype;
			break;
		default:
			break;
	}
	if (type == RECORDOID)
		return true;
	return expression_tree_walker(node, record_walker, context);
}

/*
 * Whether a fragment's plan has an anonymous record type in any of its
 * expressions -- a whole row, a ROW(), a function's record: a worker looks
 * such a type up in the leader's per-session registry, and PostgreSQL's type
 * cache keeps what it found there (typcache.c's RecordCacheArray, pointing
 * into the session's DSA) past the session, for a parallel worker exits with
 * it; a pool worker serves the next session, whose typmods are others
 */
static bool
plan_has_records(Plan *plan)
{
	ListCell   *lc;

	if (plan == NULL)
		return false;
	if (record_walker((Node *) plan->targetlist, NULL) || record_walker((Node *) plan->qual, NULL))
		return true;
	switch (nodeTag(plan))
	{
		case T_CustomScan:
			{
				CustomScan *cs = (CustomScan *) plan;

				if (record_walker((Node *) cs->custom_exprs, NULL) ||
					record_walker((Node *) cs->custom_scan_tlist, NULL))
					return true;
				foreach(lc, cs->custom_plans)
					if (plan_has_records(lfirst(lc)))
						return true;
			}
			break;
		case T_HashJoin:
			if (record_walker((Node *) ((HashJoin *) plan)->hashclauses, NULL) ||
				record_walker((Node *) ((HashJoin *) plan)->join.joinqual, NULL))
				return true;
			break;
		case T_NestLoop:
		case T_MergeJoin:
			if (record_walker((Node *) ((Join *) plan)->joinqual, NULL))
				return true;
			break;
		case T_Append:
			foreach(lc, ((Append *) plan)->appendplans)
				if (plan_has_records(lfirst(lc)))
					return true;
			break;
		case T_SubqueryScan:
			return plan_has_records(((SubqueryScan *) plan)->subplan);
		case T_FunctionScan:
		case T_TableFuncScan:
		case T_ValuesScan:
			return true;		/* their expressions elsewhere: left to PostgreSQL's workers */
		default:
			break;
	}
	return plan_has_records(plan->lefttree) || plan_has_records(plan->righttree);
}

/* Whether the plan node is in the fragment of the Gather on the main plan's top chain */
static bool
on_top_gather(PlannedStmt *stmt, Plan *node)
{
	Plan	   *p = stmt->planTree;

	while (p != NULL && !IsA(p, Gather) && !IsA(p, GatherMerge))
		p = once_child(p);
	if (p == NULL)
		return false;
	if (IsA(p, Gather) && ((Gather *) p)->single_copy)
		return false;
	return plan_contains(outerPlan(p), node) && !plan_has_records(outerPlan(p));
}

static bool
pool_eligible(PlanState *ps)
{
	EState	   *estate = ps->state;
	PlannedStmt *stmt = estate->es_plannedstmt;

	return pool != NULL && vexec_enable_pool_workers && !IsParallelWorker() &&
		parallel_leader_participation && debug_parallel_query == DEBUG_PARALLEL_OFF &&
		stmt != NULL && stmt->commandType == CMD_SELECT && !stmt->hasModifyingCTE &&
		!TransactionIdIsValid(GetTopTransactionIdIfAny()) && !IsolationIsSerializable() &&
		on_top_gather(stmt, ps->plan);
}

/*
 * A pool of the leader's database and role started where it has none: as
 * many workers as free slots, up to vexec.pool_workers -- registered now,
 * up for the next query
 */
static void
pool_ensure(void)
{
	int			want[POOL_MAX_SLOTS];
	int			n = 0;
	bool		have = false;
	Oid			role = GetAuthenticatedUserId();

	SpinLockAcquire(&pool->mutex);
	for (int i = 0; i < pool->nslots; i++)
		if (pool->slots[i].state != POOL_FREE && pool->slots[i].dbid == MyDatabaseId &&
			pool->slots[i].roleid == role)
			have = true;
	if (!have)
		for (int i = 0; i < pool->nslots && n < POOL_MAX_SLOTS; i++)
			if (pool->slots[i].state == POOL_FREE)
			{
				PoolSlot   *s = &pool->slots[i];

				s->state = POOL_STARTING;
				s->dbid = MyDatabaseId;
				s->roleid = role;
				s->pid = 0;
				want[n++] = i;
			}
	SpinLockRelease(&pool->mutex);
	for (int k = 0; k < n; k++)
	{
		BackgroundWorker w;
		BackgroundWorkerHandle *h;

		memset(&w, 0, sizeof(w));
		snprintf(w.bgw_name, BGW_MAXLEN, "vexec pool worker %d", want[k]);
		snprintf(w.bgw_type, BGW_MAXLEN, "vexec pool worker");
		w.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
		w.bgw_start_time = BgWorkerStart_RecoveryFinished;
		w.bgw_restart_time = BGW_NEVER_RESTART;
		snprintf(w.bgw_library_name, MAXPGPATH, "vexec");
		snprintf(w.bgw_function_name, BGW_MAXLEN, "vexec_pool_main");
		w.bgw_main_arg = Int32GetDatum(want[k]);
		memcpy(w.bgw_extra, &MyDatabaseId, sizeof(Oid));
		memcpy(w.bgw_extra + sizeof(Oid), &role, sizeof(Oid));
		w.bgw_notify_pid = 0;
		if (!RegisterDynamicBackgroundWorker(&w, &h))
		{
			/* no room: the slots left free */
			SpinLockAcquire(&pool->mutex);
			for (int j = k; j < n; j++)
				pool->slots[want[j]].state = POOL_FREE;
			SpinLockRelease(&pool->mutex);
			break;
		}
		pfree(h);
	}
}

/*
 * The leader, from a parallel-aware vexec node's InitializeDSM or
 * ReInitializeDSM: the Gather's workers the pool's idle workers of its
 * database and role, where the plan may have them (above), each reserved for
 * its worker number, the context's PostgreSQL workers none.
 */
void
vexec_pool_reserve(ParallelContext *pcxt, PlanState *ps)
{
	int			want;
	int			got = 0;
	int			slots[POOL_MAX_SLOTS];
	Oid			role;
	Oid			cur;
	int			sec;
	MemoryContext old;

	if (pool == NULL || pcxt == NULL || pcxt->nworkers_to_launch <= 0 ||
		list_member_ptr(reserved_contexts, pcxt) || !pool_eligible(ps))
		return;
	want = Min(pcxt->nworkers_to_launch, POOL_MAX_SLOTS);
	role = GetAuthenticatedUserId();
	GetUserIdAndSecContext(&cur, &sec);
	SpinLockAcquire(&pool->mutex);
	for (int i = 0; i < pool->nslots && got < want; i++)
	{
		PoolSlot   *s = &pool->slots[i];

		if (s->state != POOL_IDLE || s->dbid != MyDatabaseId || s->roleid != role)
			continue;
		s->state = POOL_RESERVED;
		s->job = pool->next_job++;
		s->seg = dsm_segment_handle(pcxt->seg);
		s->worker_number = got;
		s->leader_pid = MyProcPid;
		s->leader_procno = MyProcNumber;
		s->xact_ts = GetCurrentTransactionStartTimestamp();
		s->stmt_ts = GetCurrentStatementStartTimestamp();
		s->session_user_id = GetSessionUserId();
		s->session_is_superuser = GetSessionUserIsSuperuser();
		s->outer_user_id = GetOuterUserId();
		s->role_is_superuser = current_role_is_superuser;
		s->current_user_id = cur;
		s->sec_context = sec;
		s->cancel_job = 0;
		slots[got++] = i;
	}
	SpinLockRelease(&pool->mutex);
	if (got == 0)
	{
		pool_ensure();
		return;
	}

	/*
	 * PostgreSQL's workers none: every error queue let go of, as
	 * LaunchParallelWorkers() lets go of those of workers it could not
	 * register, the pool's workers launched in their place
	 */
	old = MemoryContextSwitchTo(TopTransactionContext);
	for (int i = 0; i < pcxt->nworkers_to_launch; i++)
	{
		if (pcxt->worker[i].error_mqh != NULL)
		{
			shm_mq_detach(pcxt->worker[i].error_mqh);
			pcxt->worker[i].error_mqh = NULL;
		}
		pcxt->worker[i].bgwhandle = NULL;
	}
	pcxt->nworkers_launched = got;
	pcxt->nworkers_to_launch = 0;
	if (pcxt->known_attached_workers != NULL)
		pfree(pcxt->known_attached_workers);
	pcxt->known_attached_workers = palloc0_array(bool, got);
	pcxt->nknown_attached_workers = 0;
	for (int k = 0; k < got; k++)
	{
		PoolJob    *j = palloc0_object(PoolJob);

		j->slotno = slots[k];
		j->job = pool->slots[slots[k]].job;
		j->estate = ps->state;
		jobs = lappend(jobs, j);
	}
	reserved_contexts = lappend(reserved_contexts, pcxt);
	vexec_pool_pending += got;
	MemoryContextSwitchTo(old);
	jobs_given += got;
}

/*
 * The leader, as it first runs a vexec node after a reservation: the
 * reserved workers started, the DSM made whole
 */
void
vexec_pool_kick_slow(void)
{
	ListCell   *lc;

	foreach(lc, jobs)
	{
		PoolJob    *j = lfirst(lc);
		PoolSlot   *s = &pool->slots[j->slotno];
		ProcNumber	procno = INVALID_PROC_NUMBER;

		if (j->started)
			continue;
		j->started = true;
		SpinLockAcquire(&pool->mutex);
		if (s->state == POOL_RESERVED && s->job == j->job)
		{
			s->state = POOL_RUNNING;
			procno = s->procno;
		}
		SpinLockRelease(&pool->mutex);
		if (procno != INVALID_PROC_NUMBER)
			SetLatch(&GetPGProcByNumber(procno)->procLatch);
	}
	vexec_pool_pending = 0;
}

/* Whether a slot is done with a job: answered, or gone with it */
static bool
job_done(PoolJob *j, bool *failed, int *code, char *message)
{
	PoolSlot   *s = &pool->slots[j->slotno];
	bool		done;

	SpinLockAcquire(&pool->mutex);
	done = s->done_job == j->job;
	*failed = false;
	if (done && s->failed)
	{
		*failed = true;
		*code = s->sqlerrcode;
		strlcpy(message, s->message, POOL_MESSAGE);
	}
	else if (!done && (s->job != j->job || s->state == POOL_FREE || s->state == POOL_STARTING))
	{
		/* the worker went before it answered */
		done = true;
		*failed = true;
		*code = ERRCODE_INTERNAL_ERROR;
		strlcpy(message, "a vexec pool worker exited before its job was done", POOL_MESSAGE);
	}
	SpinLockRelease(&pool->mutex);
	return done;
}

/*
 * The leader's executor finishing: each of its pool workers' jobs waited
 * for -- the Gather has read their queues or let go of them -- and the first
 * error raised
 */
static void
pool_executor_finish(QueryDesc *queryDesc)
{
	if (prev_ExecutorFinish)
		prev_ExecutorFinish(queryDesc);
	else
		standard_ExecutorFinish(queryDesc);

	if (jobs != NIL)
	{
		List	   *left = NIL;
		ListCell   *lc;
		bool		any_failed = false;
		int			code = 0;
		char		message[POOL_MESSAGE] = "";

		foreach(lc, jobs)
		{
			PoolJob    *j = lfirst(lc);
			bool		failed;
			int			c;
			char		m[POOL_MESSAGE];

			if (j->estate != queryDesc->estate)
			{
				left = lappend(left, j);
				continue;
			}
			if (!j->started)
			{
				/* never started: the plan never ran a vexec node of it */
				SpinLockAcquire(&pool->mutex);
				if (pool->slots[j->slotno].state == POOL_RESERVED && pool->slots[j->slotno].job == j->job)
					pool->slots[j->slotno].state = POOL_IDLE;
				SpinLockRelease(&pool->mutex);
				continue;
			}
			while (!job_done(j, &failed, &c, m))
			{
				CHECK_FOR_INTERRUPTS();
				(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 10,
								 WAIT_EVENT_PARALLEL_FINISH);
				ResetLatch(MyLatch);
			}
			if (failed && !any_failed)
			{
				any_failed = true;
				code = c;
				strlcpy(message, m, POOL_MESSAGE);
			}
		}
		jobs = left;
		if (jobs == NIL)
			reserved_contexts = NIL;
		if (any_failed)
			ereport(ERROR,
					(errcode(code),
					 errmsg_internal("%s", message),
					 errcontext("vexec pool worker")));
	}
}

/* The leader's transaction ended: its reserved workers let go, its running ones asked to stop */
static void
pool_xact_callback(XactEvent event, void *arg)
{
	ListCell   *lc;

	if (event != XACT_EVENT_ABORT && event != XACT_EVENT_COMMIT &&
		event != XACT_EVENT_PARALLEL_ABORT && event != XACT_EVENT_PARALLEL_COMMIT)
		return;
	if (pool == NULL || jobs == NIL)
	{
		jobs = NIL;
		reserved_contexts = NIL;
		vexec_pool_pending = 0;
		return;
	}
	foreach(lc, jobs)
	{
		PoolJob    *j = lfirst(lc);
		PoolSlot   *s = &pool->slots[j->slotno];
		pid_t		pid = 0;

		SpinLockAcquire(&pool->mutex);
		if (s->job == j->job && s->state == POOL_RESERVED)
			s->state = POOL_IDLE;
		else if (s->job == j->job && s->state == POOL_RUNNING && s->done_job != j->job)
		{
			s->cancel_job = j->job;
			pid = s->pid;
		}
		SpinLockRelease(&pool->mutex);
		if (pid != 0)
			(void) kill(pid, SIGINT);
	}
	/* the lists were in the transaction's memory */
	jobs = NIL;
	reserved_contexts = NIL;
	vexec_pool_pending = 0;
}

/* ---------------------------------------------------------------------
 * The worker
 * ---------------------------------------------------------------------
 */

/* SIGINT: the job stopped, where the leader asked it of the job running */
static void
pool_sigint(SIGNAL_ARGS)
{
	if (pool != NULL && my_slot >= 0 && my_job != 0 && pool->slots[my_slot].cancel_job == my_job)
	{
		InterruptPending = true;
		QueryCancelPending = true;
	}
	SetLatch(MyLatch);
}

/* The slot freed as the worker exits */
static void
pool_exit(int code, Datum arg)
{
	PoolSlot   *s = &pool->slots[DatumGetInt32(arg)];

	SpinLockAcquire(&pool->mutex);
	s->state = POOL_FREE;
	s->pid = 0;
	s->procno = INVALID_PROC_NUMBER;
	SpinLockRelease(&pool->mutex);
}

/* The job's end told the leader */
static void
job_answer(PoolSlot *s, uint64 job, bool failed, bool joined, int code, const char *message)
{
	ProcNumber	leader;

	SpinLockAcquire(&pool->mutex);
	s->done_job = job;
	s->failed = failed;
	s->joined = joined;
	s->sqlerrcode = code;
	if (message)
		strlcpy(s->message, message, POOL_MESSAGE);
	else
		s->message[0] = '\0';
	leader = s->leader_procno;
	if (s->state == POOL_RUNNING && s->job == job)
		s->state = POOL_IDLE;
	SpinLockRelease(&pool->mutex);
	if (leader != INVALID_PROC_NUMBER)
		SetLatch(&GetPGProcByNumber(leader)->procLatch);
}

/*
 * The fragment's relations' locks, each had at once or none: false where one
 * is not, a lock someone else holds or awaits
 */
static bool
lock_relations(shm_toc *toc)
{
	char	   *pstmtspace = shm_toc_lookup(toc, POOL_KEY_PLANNEDSTMT, false);
	PlannedStmt *stmt = (PlannedStmt *) stringToNode(pstmtspace);
	ListCell   *lc;

	foreach(lc, stmt->rtable)
	{
		RangeTblEntry *rte = lfirst(lc);

		if (rte->rtekind == RTE_RELATION && rte->rellockmode != NoLock &&
			!ConditionalLockRelationOid(rte->relid, rte->rellockmode))
			return false;
	}
	return true;
}

/* A worker's tuple queue attached and let go: the leader reads none of it */
static void
queue_none(dsm_segment *seg, shm_toc *toc, int worker)
{
	char	   *mqspace = shm_toc_lookup(toc, POOL_KEY_TUPLE_QUEUE, false);
	shm_mq	   *mq = (shm_mq *) (mqspace + (Size) worker * POOL_TUPLE_QUEUE_SIZE);
	shm_mq_handle *mqh;

	shm_mq_set_sender(mq, MyProc);
	mqh = shm_mq_attach(mq, seg, NULL);
	shm_mq_detach(mqh);
}

/* One job of the leader's, its DSM attached as ParallelWorkerMain() attaches a parallel worker's */
static bool
run_job(PoolSlot *s, PoolSlot *job, dsm_segment **segp, bool *session)
{
	dsm_segment *seg;
	shm_toc    *toc;
	char	   *tsnapspace;
	Snapshot	tsnap;
	Snapshot	asnap;

	ParallelWorkerNumber = job->worker_number;
	seg = dsm_attach(job->seg);
	if (seg == NULL)
		return false;			/* the leader is gone */
	*segp = seg;
	toc = shm_toc_attach(POOL_PARALLEL_MAGIC, dsm_segment_address(seg));
	if (toc == NULL)
		elog(ERROR, "vexec: a pool job's shared memory is no parallel context's");

	/* as ParallelWorkerMain() restores the leader's state, but for what a transaction without an XID lacks */
	SetParallelStartTimestamps(job->xact_ts, job->stmt_ts);
	SetSessionAuthorization(job->session_user_id, job->session_is_superuser);
	SetCurrentRoleId(job->outer_user_id, job->role_is_superuser);
	StartTransactionCommand();
	RestoreLibraryState(shm_toc_lookup(toc, POOL_KEY_LIBRARY, false));
	AttachSession(*(dsm_handle *) shm_toc_lookup(toc, POOL_KEY_SESSION_DSM, false));
	*session = true;
	asnap = RestoreSnapshot(shm_toc_lookup(toc, POOL_KEY_ACTIVE_SNAPSHOT, false));
	tsnapspace = shm_toc_lookup(toc, POOL_KEY_TRANSACTION_SNAPSHOT, true);
	tsnap = tsnapspace ? RestoreSnapshot(tsnapspace) : asnap;
	RestoreTransactionSnapshot(tsnap, GetPGProcByNumber(job->leader_procno));
	PushActiveSnapshot(asnap);
	InitializingParallelWorker = true;
	RestoreGUCState(shm_toc_lookup(toc, POOL_KEY_GUC, false));
	InitializingParallelWorker = false;
	SetUserIdAndSecContext(job->current_user_id, job->sec_context);

	if (!lock_relations(toc))
	{
		/* no part of the plan: the others do its share */
		queue_none(seg, toc, job->worker_number);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}
	EnterParallelMode();
	ParallelQueryMain(seg, toc);
	ExitParallelMode();
	PopActiveSnapshot();
	CommitTransactionCommand();
	return true;
}

static void
job_cleanup(dsm_segment **segp, bool *session)
{
	if (*session)
		DetachSession();
	*session = false;
	if (*segp != NULL)
		dsm_detach(*segp);
	*segp = NULL;
	ParallelWorkerNumber = -1;
	InitializingParallelWorker = false;
	debug_query_string = NULL;
	pgstat_report_activity(STATE_IDLE, NULL);
}

PGDLLEXPORT void vexec_pool_main(Datum main_arg);

void
vexec_pool_main(Datum main_arg)
{
	int			slotno = DatumGetInt32(main_arg);
	PoolSlot   *s;
	Oid			dbid;
	Oid			roleid;
	sigjmp_buf	local_sigjmp_buf;
	MemoryContext jobcxt;
	TimestampTz idle_since;
	dsm_segment *volatile seg = NULL;
	volatile bool session = false;
	volatile uint64 job = 0;

	memcpy(&dbid, MyBgworkerEntry->bgw_extra, sizeof(Oid));
	memcpy(&roleid, MyBgworkerEntry->bgw_extra + sizeof(Oid), sizeof(Oid));
	pqsignal(SIGINT, pool_sigint);
	BackgroundWorkerUnblockSignals();
	if (pool == NULL || slotno < 0 || slotno >= pool->nslots)
		proc_exit(0);
	s = &pool->slots[slotno];
	my_slot = slotno;
	before_shmem_exit(pool_exit, Int32GetDatum(slotno));
	BackgroundWorkerInitializeConnectionByOid(dbid, roleid,
											  BGWORKER_BYPASS_ALLOWCONN | BGWORKER_BYPASS_ROLELOGINCHECK);
	if (SetClientEncoding(GetDatabaseEncoding()) < 0)
		elog(ERROR, "SetClientEncoding(%d) failed", GetDatabaseEncoding());
	jobcxt = AllocSetContextCreate(TopMemoryContext, "vexec pool job", ALLOCSET_DEFAULT_SIZES);
	SpinLockAcquire(&pool->mutex);
	s->pid = MyProcPid;
	s->procno = MyProcNumber;
	if (s->state == POOL_STARTING)
		s->state = POOL_IDLE;
	SpinLockRelease(&pool->mutex);
	idle_since = GetCurrentTimestamp();

	/* an error of a job: told the leader, the transaction aborted, the worker idle again */
	if (sigsetjmp(local_sigjmp_buf, 1) != 0)
	{
		ErrorData  *edata;

		error_context_stack = NULL;
		HOLD_INTERRUPTS();
		MemoryContextSwitchTo(TopMemoryContext);
		edata = CopyErrorData();
		EmitErrorReport();
		FlushErrorState();
		AbortOutOfAnyTransaction();
		job_cleanup((dsm_segment **) &seg, (bool *) &session);
		if (job != 0)
			job_answer(s, job, true, true, edata->sqlerrcode, edata->message);
		FreeErrorData(edata);
		job = 0;
		my_job = 0;
		QueryCancelPending = false;
		MemoryContextReset(jobcxt);
		RESUME_INTERRUPTS();
		idle_since = GetCurrentTimestamp();
	}
	PG_exception_stack = &local_sigjmp_buf;

	for (;;)
	{
		PoolSlot	mine;
		bool		go = false;
		bool		joined;

		CHECK_FOR_INTERRUPTS();
		SpinLockAcquire(&pool->mutex);
		if (s->state == POOL_RUNNING && s->done_job != s->job)
		{
			mine = *s;
			go = true;
		}
		SpinLockRelease(&pool->mutex);
		if (!go)
		{
			int			rc;
			long		timeout = vexec_pool_idle_timeout > 0 ? (long) vexec_pool_idle_timeout * 1000L : -1;

			if (timeout > 0 &&
				TimestampDifferenceExceeds(idle_since, GetCurrentTimestamp(), (int) Min(timeout, INT_MAX)))
			{
				/* idle for long: the slot freed for another pool */
				bool		idle;

				SpinLockAcquire(&pool->mutex);
				idle = s->state == POOL_IDLE;
				if (idle)
					s->state = POOL_FREE;
				SpinLockRelease(&pool->mutex);
				if (idle)
					proc_exit(0);
			}
			rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_EXIT_ON_PM_DEATH | (timeout > 0 ? WL_TIMEOUT : 0),
						   timeout > 0 ? Min(timeout, 60000L) : -1, PG_WAIT_EXTENSION);
			(void) rc;
			ResetLatch(MyLatch);
			continue;
		}
		job = mine.job;
		my_job = job;
		MemoryContextSwitchTo(jobcxt);
		joined = run_job(s, &mine, (dsm_segment **) &seg, (bool *) &session);
		job_cleanup((dsm_segment **) &seg, (bool *) &session);
		MemoryContextSwitchTo(TopMemoryContext);
		MemoryContextReset(jobcxt);
		job_answer(s, job, false, joined, 0, NULL);
		job = 0;
		my_job = 0;
		idle_since = GetCurrentTimestamp();
	}
}

/*
 * vexec.pool(): the pool's workers in all, those idle and those running a
 * job now, and the jobs this backend gave it since it started -- NULLs but
 * the last where there is no pool
 */
PG_FUNCTION_INFO_V1(vexec_pool_info);
Datum
vexec_pool_info(PG_FUNCTION_ARGS)
{
	TupleDesc	desc;
	Datum		values[4];
	bool		nulls[4] = {false, false, false, false};

	if (get_call_result_type(fcinfo, NULL, &desc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	if (pool == NULL)
		nulls[0] = nulls[1] = nulls[2] = true;
	else
	{
		int			workers = 0;
		int			idle = 0;
		int			running = 0;

		SpinLockAcquire(&pool->mutex);
		for (int i = 0; i < pool->nslots; i++)
		{
			PoolState	st = pool->slots[i].state;

			workers += st != POOL_FREE && st != POOL_STARTING;
			idle += st == POOL_IDLE;
			running += st == POOL_RESERVED || st == POOL_RUNNING;
		}
		SpinLockRelease(&pool->mutex);
		values[0] = Int32GetDatum(workers);
		values[1] = Int32GetDatum(idle);
		values[2] = Int32GetDatum(running);
	}
	values[3] = Int64GetDatum(jobs_given);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(BlessTupleDesc(desc), values, nulls)));
}

/* ---------------------------------------------------------------------
 * Setup
 * ---------------------------------------------------------------------
 */

/* From _PG_init: the settings, and the pool's shared memory where the postmaster loads vexec */
void
vexec_pool_install(void)
{
	DefineCustomIntVariable("vexec.pool_workers",
							"The long-lived parallel workers of vexec's pool, a database and role's.",
							"Background workers started once and kept, which run a parallel plan's partial "
							"fragment in place of the workers the postmaster forks for each query, their "
							"caches and memory warm (pg_vector_executor.md §3.28, F3).  0, the default: no "
							"pool.",
							&vexec_pool_workers,
							0, 0, POOL_MAX_SLOTS,
							PGC_POSTMASTER, 0,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_pool_workers",
							 "Lets a parallel plan's Gather take vexec's pool workers, where there is a pool.",
							 "A SELECT whose transaction has written nothing, its Gather on the plan's top chain "
							 "and the leader taking part, takes the idle workers of the pool of its database and "
							 "role for its parallel workers; off, the postmaster's.",
							 &vexec_enable_pool_workers, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.pool_idle_timeout",
							"The seconds a pool worker waits idle before it exits.",
							"0: it waits as long as the server runs.",
							&vexec_pool_idle_timeout,
							300, 0, 86400,
							PGC_SIGHUP, GUC_UNIT_S,
							NULL, NULL, NULL);
	if (!process_shared_preload_libraries_in_progress)
		return;
	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = pool_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = pool_shmem_startup;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = pool_executor_finish;
	RegisterXactCallback(pool_xact_callback, NULL);
}
