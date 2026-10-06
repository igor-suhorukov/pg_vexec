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
 * In V1 the scan is vectorized: VecScan, and ORCA's Results over it as
 * VecResult; in V2 aggregation, VecAgg; in V3 hash joins, VecHashJoin; in
 * V4 sorts, VecSort, and scans in parallel; in V5 ORCA's hashed window,
 * VecWindowHashAgg, and ORCA's search priced with vexec's nodes (§5).
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_PLAN_H
#define VEXEC_PLAN_H

#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"
#include "utils/relcache.h"

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
	const char *status;			/* possible: "added", "built", "not chosen",
								 * "not built" */
	char	   *reason;			/* why not possible, or what became of it */
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
	int			npossible;		/* vector nodes the oracle accepted, built or
								 * added as paths: what
								 * vexec.debug_require_vector asks of a plan */
	bool		joins_built;	/* a VecHashJoin's plan was made */
	bool		sorts_built;	/* a VecSort's path was added */
	Query	   *parse;			/* ORCA's: the statement, while it is planned */
	bool		orca_costed;	/* ORCA's search priced vexec's nodes
								 * (CCostModelVec): translation builds them
								 * where it priced them */
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
								int nclauses_kernel, bool outer_vector,
								bool inner_vector, VexecCost *cost);
extern void vexec_cost_plan_hashjoin(Plan *join, Plan *outer, Plan *inner, int nclauses,
									 int nclauses_kernel, VexecCost *cost);
extern void vexec_cost_agg(PlannerInfo *root, Path *rowpath, Path *input,
						   bool input_vector, int ngroupcols, int ngroupcols_kernel,
						   int naggs_kernel, double numgroups, VexecCost *cost);
extern void vexec_cost_plan_agg(Plan *agg, int ngroupcols, int naggs, int naggs_kernel,
								bool input_vector, VexecCost *cost);
extern void vexec_cost_sort(PlannerInfo *root, Path *rowpath, Path *input, bool input_vector,
							VexecCost *cost);
extern void vexec_cost_plan_sort(Plan *sort, bool input_vector, VexecCost *cost);
extern void vexec_cost_bitmapscan(PlannerInfo *root, RelOptInfo *rel, Path *rowpath,
								  const VexecSteps *quals, const VexecSteps *target,
								  VexecCost *cost);
extern void vexec_cost_plan_scan(Relation rel, const VexecSteps *quals,
								 const VexecSteps *target, int ncols_in, int ncols_out,
								 double rows, VexecCost *cost);

/* reasons.c */
extern VexecAlt *vexec_alt_record(VexecPlanState *ps, const char *node, const char *target,
								  const void *root, Relids relids);
extern void vexec_alt_refuse(VexecPlanState *ps, VexecAlt *alt, const char *reason);
extern void vexec_alt_costed(VexecPlanState *ps, VexecAlt *alt, const VexecCost *cost,
							 const char *detail);
extern char *vexec_relids_names(PlannerInfo *root, Relids relids);
extern Node *vexec_reasons_node(VexecPlanState *ps);

/* paths.c */
extern int	vexec_planner_extension_id(void);
extern VexecPlanState *vexec_plan_state(PlannerInfo *root);
extern const char *vexec_statement_gate(Query *parse, int cursorOptions);

/* orca.c: ORCA's front end, through gp_orca's API */
extern void vexec_orca_install(void);

/* agg.c */
extern void vexec_consider_agg(PlannerInfo *root, RelOptInfo *input_rel,
							   RelOptInfo *output_rel, GroupPathExtraData *extra,
							   VexecPlanState *ps, const char *target_name);
extern const char *vexec_orca_agg_refusal(Agg *agg);
extern Plan *vexec_build_agg_from_agg(Agg *agg);
extern Agg *vexec_agg_describe(CustomScan *cscan);

/* join.c */
extern bool vexec_is_vector_path(Path *path);
extern void vexec_consider_hashjoin(PlannerInfo *root, RelOptInfo *joinrel,
									RelOptInfo *outerrel, RelOptInfo *innerrel,
									JoinType jointype, JoinPathExtraData *extra,
									VexecPlanState *ps);
extern const char *vexec_orca_hashjoin_refusal(HashJoin *hj);
extern void vexec_join_finish_plan(PlannedStmt *pstmt);
extern Plan *vexec_build_hashjoin_from_hashjoin(HashJoin *hj);

/* sort.c */
extern void vexec_consider_sort(PlannerInfo *root, RelOptInfo *input_rel,
								RelOptInfo *output_rel, VexecPlanState *ps,
								const char *target_name);
extern const char *vexec_orca_sort_refusal(Sort *sort);
extern Plan *vexec_build_sort_from_sort(Sort *sort);
extern Plan *vexec_unbuild_sort(CustomScan *cscan);
extern Sort *vexec_sort_describe(CustomScan *cscan);
extern void vexec_orca_limit_bound(Limit *limit);

/* window.c */
extern Plan *vexec_build_window(VexecPlanState *ps, WindowAgg *window, List *rtable);

/* build.c */
extern Path *vexec_scan_path(PlannerInfo *root, RelOptInfo *rel, Path *rowpath,
							 const VexecCost *cost);
extern Path *vexec_bitmapscan_path(PlannerInfo *root, RelOptInfo *rel, BitmapHeapPath *bp,
								   const VexecSteps *quals, const VexecSteps *target);
extern Plan *vexec_build_bitmapscan_from_bitmapscan(BitmapHeapScan *bhs);
extern Plan *vexec_build_scan_from_seqscan(SeqScan *seqscan);
extern Plan *vexec_build_result_from_result(Result *result);

/* explain.c */
extern bool vexec_explain_requested(void *es);

/* check.c */
extern int	vexec_check_plan(PlannedStmt *pstmt, int mode);

#endif							/* VEXEC_PLAN_H */
