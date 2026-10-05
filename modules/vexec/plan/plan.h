/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * plan.h
 *	  The vectorized planner's shared core (pg_vector_executor.md §3.3.2):
 *	  the capability oracle, the vector cost model, the reasons, and the
 *	  plan check, with the state they keep while a statement is planned.
 *
 * For every scan, hash join, aggregation and sort a planner considers, the
 * vectorized planner settles whether a vector alternative exists (the
 * oracle), what it costs (the cost model), and whether it wins (the
 * planner's own search).  Every alternative not taken keeps its reason, for
 * EXPLAIN's vexec option.
 *
 * In V0 nothing is vectorized: no vector node exists yet, so in every mode
 * but off the hooks only cost and record the alternatives (§5, V0).
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_PLAN_H
#define VEXEC_PLAN_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"

#include "vexec.h"
#include "batch/batch.h"

/*
 * What the oracle makes of an expression (§3.3.2): its kernel steps, its
 * fallback steps -- PostgreSQL's evaluator, row by row, inside the vector
 * node -- or that it cannot be in a vector node at all.  The costs are per
 * row, at PostgreSQL's own prices (cost_qual_eval_node); the cost model
 * applies the vector factor to the kernels' share only.
 */
typedef struct VexecSteps
{
	int			kernel;
	int			fallback;
	Cost		kernel_cost;
	Cost		fallback_cost;
	Cost		startup;
	const char *refusal;		/* not in a vector node: why; NULL if it can
								 * be */
} VexecSteps;

/* The cost of a vector alternative, beside its row counterpart's (§3.3.2). */
typedef struct VexecCost
{
	Cost		row_startup;	/* PostgreSQL's own, from its cost functions */
	Cost		row_total;
	Cost		startup;		/* the vector alternative's */
	Cost		total;			/* rowout included */
	Cost		convert_in;		/* transposing row inputs, included */
	Cost		rowout;			/* handing rows to a row parent, included */
	double		rows;
} VexecCost;

/* One vector alternative a planner considered, and what became of it. */
typedef struct VexecAlt
{
	const char *node;			/* VecScan, VecHashJoin, VecAgg, VecSort */
	char	   *target;			/* the relations, or the stage */
	const void *root;			/* joins: the query level, whose range table
								 * relids index */
	Relids		relids;			/* joins: one record per join relation */
	bool		possible;		/* the oracle accepted it */
	char	   *reason;			/* why not possible, or why not chosen */
	char	   *detail;			/* source, steps, layouts */
	VexecCost	cost;
} VexecAlt;

/* What a statement's planning keeps (PlannerGlobal's extension state). */
typedef struct VexecPlanState
{
	MemoryContext mcxt;			/* planning's: GEQO's join contexts are
								 * shorter */
	int			mode;			/* vexec.mode when planning began */
	bool		gate_open;		/* the statement's gates (§3.3.3) */
	const char *gate_reason;
	bool		record;			/* keep the alternatives for EXPLAIN */
	VexecLayoutConfig layout;	/* the format the conversions are priced in */
	List	   *alts;			/* VexecAlt */
	int			nalts;
	int			dropped;		/* past the record's cap */
} VexecPlanState;

/* The most alternatives one statement records. */
#define VEXEC_MAX_ALTS		256

/* oracle.c */
extern void vexec_oracle_expr(PlannerInfo *root, Node *expr, VexecSteps *steps);
extern void vexec_oracle_exprs(PlannerInfo *root, List *exprs, VexecSteps *steps);
extern bool vexec_kernel_bound(Oid funcid, Oid inputtype, Oid collation);

/* cost.c */
extern void vexec_cost_scan(PlannerInfo *root, RelOptInfo *rel, Path *rowpath,
							const VexecSteps *quals, const VexecSteps *target,
							int ncols_in, int ncols_out, double source_bytes,
							VexecCost *cost);
extern void vexec_cost_hashjoin(PlannerInfo *root, Path *rowpath,
								Path *outer, Path *inner, int nclauses,
								int nclauses_kernel, VexecCost *cost);
extern void vexec_cost_agg(PlannerInfo *root, Path *rowpath, Path *input,
						   int ngroupcols, int ngroupcols_kernel,
						   int naggs_kernel, double numgroups, VexecCost *cost);
extern void vexec_cost_sort(PlannerInfo *root, Path *rowpath, Path *input, VexecCost *cost);

/* reasons.c */
extern VexecAlt *vexec_alt_record(VexecPlanState *ps, const char *node, const char *target,
								  const void *root, Relids relids);
extern void vexec_alt_refuse(VexecPlanState *ps, VexecAlt *alt, const char *reason);
extern void vexec_alt_costed(VexecPlanState *ps, VexecAlt *alt, const VexecCost *cost,
							 const char *detail);
extern char *vexec_relids_names(PlannerInfo *root, Relids relids);
extern Node *vexec_reasons_node(VexecPlanState *ps);

/* paths.c */
extern VexecPlanState *vexec_plan_state(PlannerInfo *root);

/* explain.c */
extern bool vexec_explain_requested(void *es);

/* check.c */
extern int	vexec_check_plan(PlannedStmt *pstmt, int mode);

#endif							/* VEXEC_PLAN_H */
