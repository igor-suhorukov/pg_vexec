/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * run.c
 *	  ExecutorRun for a result that goes to the egress's receiver from a
 *	  vector node at the top of its plan: the node's batches go out as they
 *	  are made, with no row formed (pg_vector_executor.md §3.15, V10).
 *
 * PostgreSQL's executor hands a receiver a row a call
 * (PG19:src/backend/executor/execMain.c:1687-1797, ExecutePlan).  A vector
 * node hands a vector parent batches instead (exec/node.c,
 * vexec_next_batch), and the egress's receiver is such a parent: where the
 * statement's receiver is the egress's and its top node can hand up
 * batches, this hook runs the loop ExecutePlan would, a batch at a time.
 * It keeps what standard_ExecutorRun keeps around that loop -- the query's
 * memory context, its instrumentation, the receiver's startup and shutdown,
 * es_processed and es_total_processed, parallel mode, the nodes' shutdown
 * (execMain.c:318-398, 1687-1797) -- so that a portal, pg_stat_statements
 * and EXPLAIN ANALYZE see the statement as they see any other.
 *
 * Everything else goes to the hook it took the place of, or to
 * standard_ExecutorRun: another receiver; a top node that hands up rows
 * only (VecAgg, a node with a lazy qual or target); a portal fetched a
 * count of rows at a time, or backward.  Hooks loaded after vexec's wrap
 * this one and run as usual; for the batches, those loaded before it are
 * passed over.  With vexec.mode off no receiver is the egress's, so the
 * hook adds one comparison to a statement.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "executor/executor.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "nodes/plannodes.h"

#include "vexec.h"
#include "egress/egress.h"
#include "exec/exec.h"

static ExecutorRun_hook_type prev_executor_run = NULL;

/*
 * The result's columns in the top node's output, junk left out: the
 * receiver's tuple descriptor is the target list's without its junk
 * columns (execMain.c, InitPlan's junk filter).
 */
static int *
result_columns(Plan *plan, int ncols)
{
	int		   *colmap = palloc(sizeof(int) * Max(ncols, 1));
	ListCell   *lc;
	int			i = 0;
	int			k = 0;

	foreach(lc, plan->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (!tle->resjunk)
		{
			if (k >= ncols)
				elog(ERROR, "vexec egress: the top node has more result columns than its result");
			colmap[k++] = i;
		}
		i++;
	}
	if (k != ncols)
		elog(ERROR, "vexec egress: the top node has %d result columns, its result %d", k, ncols);
	return colmap;
}

/* Whether this run goes a batch at a time. */
static bool
runs_batches(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	PlanState  *ps = queryDesc->planstate;

	return vexec_egress_is_receiver(queryDesc->dest) &&
		queryDesc->operation == CMD_SELECT &&
		ScanDirectionIsForward(direction) && count == 0 &&
		!queryDesc->already_executed &&
		vexec_is_vector_state(ps) &&
		vexec_node_batchable((VexecNode *) ps);
}

static void
vexec_executor_run(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	EState	   *estate;
	PlanState  *ps;
	DestReceiver *dest;
	MemoryContext old;
	bool		parallel;
	int		   *colmap;
	VexecBatch *batch;

	if (!runs_batches(queryDesc, direction, count))
	{
		if (prev_executor_run)
			prev_executor_run(queryDesc, direction, count);
		else
			standard_ExecutorRun(queryDesc, direction, count);
		return;
	}

	estate = queryDesc->estate;
	ps = queryDesc->planstate;
	dest = queryDesc->dest;
	Assert(!(estate->es_top_eflags & EXEC_FLAG_EXPLAIN_ONLY));

	old = MemoryContextSwitchTo(estate->es_query_cxt);
	if (queryDesc->query_instr)
		InstrStart(queryDesc->query_instr);
	estate->es_processed = 0;
	dest->rStartup(dest, CMD_SELECT, queryDesc->tupDesc);
	colmap = result_columns(ps->plan, queryDesc->tupDesc->natts);

	estate->es_direction = direction;
	parallel = queryDesc->plannedstmt->parallelModeNeeded;
	queryDesc->already_executed = true;
	estate->es_use_parallel_mode = parallel;
	if (parallel)
		EnterParallelMode();

	for (;;)
	{
		ResetPerTupleExprContext(estate);
		batch = vexec_next_batch(ps);
		if (batch == NULL)
			break;
		estate->es_processed += vexec_egress_send_batch(dest, (VexecNode *) ps, batch, colmap);
	}

	if (!(estate->es_top_eflags & EXEC_FLAG_BACKWARD))
		ExecShutdownNode(ps);
	if (parallel)
		ExitParallelMode();

	estate->es_total_processed += estate->es_processed;
	dest->rShutdown(dest);
	if (queryDesc->query_instr)
		InstrStop(queryDesc->query_instr);
	MemoryContextSwitchTo(old);
}

void
vexec_egress_run_install(void)
{
	prev_executor_run = ExecutorRun_hook;
	ExecutorRun_hook = vexec_executor_run;
}
