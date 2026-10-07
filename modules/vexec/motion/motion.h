/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * motion.h
 *	  Batches across ORCA's Motions, as Arrow IPC frames: step A of
 *	  pg_vector_executor.md §3.10, V7.
 *
 * Below a Redistribute, a Broadcast, a random redistribution or a Gather
 * whose fragment's top is a vector node, VecMotionSend sends the node's
 * batches as frames (frame.h), one row of the Motion's for each batch and
 * target segment: the fragment's columns, all NULL, and two more, the
 * segment the frame goes to and the frame.  A Redistribute becomes an
 * Explicit Redistribute keyed on the segment column, which the vector
 * cdbhash fills, and a random redistribution one that deals each batch to
 * the next segment in turn; a Broadcast and a Gather keep their kind.  Above
 * the Motion, VecMotionRecv takes the frames and hands its parent batches,
 * or rows to a row parent: what the Motion's own target list and qual made
 * of the fragment's rows, which it takes over.  So the Motion's code is
 * unchanged: gp_core's Motion carries a row of two columns a batch, and a
 * transport frames of any length.
 *
 * The fragment's columns, NULL in every frame's row, keep the plan's
 * references what they were: a parent's Var resolves through
 * VecMotionRecv, the Motion and VecMotionSend to the fragment's own target
 * list, so EXPLAIN prints each column as it did, and the Motion's NULLs
 * cost a bitmap's bytes.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_MOTION_H
#define VEXEC_MOTION_H

#include "nodes/execnodes.h"
#include "nodes/plannodes.h"

#include "vexec.h"

struct GpCoreApi;
struct VexecNode;
struct VexecPlanState;

/* The gp_core API minor version the frames need (gpcore.c). */
#define VEXEC_GP_CORE_MINOR		15

/* gp_core's Motion: its CustomScan's name and kinds (the port's gp_motion.h). */
#define VEXEC_GP_MOTION_NAME		"GpMotion"
#define VEXEC_GP_MOTION_GATHER		0
#define VEXEC_GP_MOTION_HASH		1
#define VEXEC_GP_MOTION_BROADCAST	2
#define VEXEC_GP_MOTION_RANDOM		3
#define VEXEC_GP_MOTION_DML			4
#define VEXEC_GP_MOTION_EXPLICIT	5

/* What a frame's Motion node counts, for EXPLAIN ANALYZE. */
typedef struct VexecMotionStats
{
	int64		frames;			/* of batches */
	int64		schema_frames;
	int64		bytes;			/* the frames' */
	int64		rows;
	int64		rows_by_postgres;	/* keys PostgreSQL's evaluator computed */
} VexecMotionStats;

/* gpcore.c */
extern VEXEC_API const struct GpCoreApi *vexec_gp_core(void);
extern void vexec_gp_core_explain_register(void);
extern bool vexec_gp_core_squelch(PlanState *ps);

/* exec/vecmotion.c */
extern VexecMotionStats *vexec_motion_stats(struct VexecNode *node);

/* plan/motion.c: ORCA's Motions, their frames built around them */
extern bool vexec_is_gp_motion(Plan *plan);
extern Plan *vexec_orca_motion(struct VexecPlanState *ps, CustomScan *motion,
							   int cursorOptions);
extern void vexec_orca_motion_number(struct VexecPlanState *ps, PlannedStmt *stmt);

#endif							/* VEXEC_MOTION_H */
