/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec.h
 *	  vexec's settings, and what its parts share.
 *
 * vexec is the vectorized planner and executor of pg_vector_executor.md, as
 * one extension on the hooks PostgreSQL 19 already has.  Its parts are the
 * vectorized planner (plan/), the batch layer (batch/), the batch sources
 * (source/), the executor's nodes (exec/) and their expressions (expr/).  This header holds the settings of §3.12 and the few things more
 * than one part reads.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_H
#define VEXEC_H

#include "utils/guc.h"

#define VEXEC_VERSION	"0.3.0"

/*
 * What vexec exports to other modules of its own repository's tests
 * (modules/vexec_test).  PGXS builds a module with hidden visibility, so
 * only what is marked so can be reached from another library; storage
 * modules and the planned extensions reach vexec through rendezvous
 * variables instead (vexec_source.h), and never by symbol.
 */
#define VEXEC_API	PGDLLEXPORT

/* vexec.mode (§3.12) */
typedef enum VexecMode
{
	VEXEC_MODE_OFF,				/* no vector alternative is built */
	VEXEC_MODE_EXPLAIN,			/* alternatives costed and recorded, never
								 * chosen */
	VEXEC_MODE_AUTO,			/* chosen by cost */
	VEXEC_MODE_FORCE			/* chosen wherever the oracle accepts them */
} VexecMode;

/* vexec.batch_format (§3.4.4) */
typedef enum VexecBatchFormat
{
	VEXEC_FORMAT_POSTGRES,		/* PostgreSQL's own representations */
	VEXEC_FORMAT_ARROW			/* Arrow's columnar format, standard types */
} VexecBatchFormat;

/* The four per-structure settings; "format" takes the format's own. */
typedef enum VexecVarlenaSetting
{
	VEXEC_VARLENA_FORMAT,
	VEXEC_VARLENA_DATUM,
	VEXEC_VARLENA_VIEW,
	VEXEC_VARLENA_OFFSETS
} VexecVarlenaSetting;

typedef enum VexecBoolSetting
{
	VEXEC_BOOL_FORMAT,
	VEXEC_BOOL_BYTE,
	VEXEC_BOOL_BIT
} VexecBoolSetting;

typedef enum VexecTemporalSetting
{
	VEXEC_TEMPORAL_FORMAT,
	VEXEC_TEMPORAL_POSTGRES,
	VEXEC_TEMPORAL_ARROW
} VexecTemporalSetting;

typedef enum VexecNumericSetting
{
	VEXEC_NUMERIC_FORMAT,
	VEXEC_NUMERIC_SCALED,
	VEXEC_NUMERIC_VARLENA
} VexecNumericSetting;

/* vexec.gpu (§3.11) */
typedef enum VexecGpuMode
{
	VEXEC_GPU_OFF,
	VEXEC_GPU_AUTO,
	VEXEC_GPU_FORCE
} VexecGpuMode;

/* The settings of §3.12 (vexec.c defines them). */
extern PGDLLIMPORT int vexec_mode;
extern PGDLLIMPORT double vexec_cpu_tuple_factor;
extern PGDLLIMPORT double vexec_cpu_operator_factor;
extern PGDLLIMPORT double vexec_convert_cost;
extern PGDLLIMPORT double vexec_batch_setup_cost;
extern PGDLLIMPORT int vexec_min_rows;
extern PGDLLIMPORT bool vexec_enable_scan;
extern PGDLLIMPORT bool vexec_enable_agg;
extern PGDLLIMPORT bool vexec_aggregate_statistics;
extern PGDLLIMPORT bool vexec_enable_hashjoin;
extern PGDLLIMPORT bool vexec_enable_sort;
extern PGDLLIMPORT bool vexec_enable_window;
extern PGDLLIMPORT bool vexec_enable_insert;
extern PGDLLIMPORT bool vexec_orca;
extern PGDLLIMPORT double vexec_compact_threshold;
extern PGDLLIMPORT int vexec_batch_format;
extern PGDLLIMPORT int vexec_batch_varlena_layout;
extern PGDLLIMPORT int vexec_batch_bool_layout;
extern PGDLLIMPORT int vexec_batch_temporal_layout;
extern PGDLLIMPORT int vexec_batch_numeric_layout;
extern PGDLLIMPORT bool vexec_batch_dictionary;
extern PGDLLIMPORT int vexec_gpu;
extern PGDLLIMPORT double vexec_gpu_setup_cost;
extern PGDLLIMPORT double vexec_gpu_transfer_cost;
extern PGDLLIMPORT double vexec_gpu_operator_factor;
extern PGDLLIMPORT bool vexec_gpu_segments;
extern PGDLLIMPORT char *vexec_gpu_devices;
extern PGDLLIMPORT int vexec_gpu_arena_mb;
extern PGDLLIMPORT int vexec_gpu_memory_limit_mb;
extern PGDLLIMPORT bool vexec_debug_require_vector;
extern PGDLLIMPORT bool vexec_debug_check_plans;
extern PGDLLIMPORT int vexec_debug_layout_seed;

extern const struct config_enum_entry vexec_mode_options[];

/* vexec.c */
extern const char *vexec_mode_name(int mode);

/* plan/paths.c: the planner's hooks */
extern void vexec_planner_install(void);

/* plan/explain.c: EXPLAIN's vexec option */
extern void vexec_explain_install(void);

/* source/registry.c */
extern void vexec_source_registry_install(void);

/* exec/methods.c: the vector nodes' methods, registered by name */
extern void vexec_exec_install(void);

#endif							/* VEXEC_H */
