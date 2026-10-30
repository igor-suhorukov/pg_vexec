/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec.h
 *	  vexec's settings, and what its parts share.
 *
 * vexec is the vectorized planner and executor of pg_vector_executor.md, as
 * one extension on the hooks PostgreSQL 19 already has.  Its parts are the
 * vectorized planner (plan/), the batch layer (batch/), the batch sources
 * (source/), the executor's nodes (exec/) and their expressions (expr/),
 * with the kernel packs' declarations of other extensions' functions
 * (expr/packs.c).  This header holds the settings of §3.12 and the few
 * things more than one part reads.
 *
 *-------------------------------------------------------------------------
 */
#ifndef VEXEC_H
#define VEXEC_H

#include "utils/guc.h"

#define VEXEC_VERSION	"0.4.0"

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

/* vexec.precision (§3.11, "Two modes") */
typedef enum VexecPrecision
{
	VEXEC_PRECISION_PRECISE,
	VEXEC_PRECISION_FAST
} VexecPrecision;

/*
 * vexec.batch_rows' least and most (F24, §3.38): the least vexec.gpu_chunk_rows'
 * too, a batch's rows; the most, its default, the rows a batch can hold
 * (batch.h's VEXEC_BATCH_ROWS)
 */
#define VEXEC_BATCH_ROWS_SETTING_MIN	1024
#define VEXEC_BATCH_ROWS_SETTING_MAX	4096

/* The settings of §3.12 (vexec.c defines them). */
extern PGDLLIMPORT int vexec_mode;
extern PGDLLIMPORT double vexec_cpu_tuple_factor;
extern PGDLLIMPORT double vexec_cpu_operator_factor;
extern PGDLLIMPORT double vexec_convert_cost;
extern PGDLLIMPORT double vexec_batch_setup_cost;
extern PGDLLIMPORT int vexec_min_rows;
extern PGDLLIMPORT int vexec_scan_rows_per_worker;
extern PGDLLIMPORT int vexec_scan_row_bytes;
extern PGDLLIMPORT bool vexec_enable_scan;
extern PGDLLIMPORT bool vexec_heap_page_reader;
extern PGDLLIMPORT bool vexec_enable_prefilter;
extern PGDLLIMPORT bool vexec_enable_source_layouts;
extern PGDLLIMPORT bool vexec_trust_source_text;
extern PGDLLIMPORT bool vexec_enable_source_steps;
extern PGDLLIMPORT bool vexec_enable_source_join_keys;
extern PGDLLIMPORT bool vexec_enable_bitmapscan;
extern PGDLLIMPORT bool vexec_enable_agg;
extern PGDLLIMPORT bool vexec_enable_agg_below_min_rows;
extern PGDLLIMPORT bool vexec_enable_repartition;
extern PGDLLIMPORT bool vexec_enable_packed_groups;
extern PGDLLIMPORT bool vexec_enable_agg_split;
extern PGDLLIMPORT int vexec_exchange_memory;
extern PGDLLIMPORT bool vexec_enable_exchange_area;
extern PGDLLIMPORT bool vexec_drop_implied_keys;
extern PGDLLIMPORT bool vexec_drop_implied_key_inputs;
extern PGDLLIMPORT int vexec_repartition_shared_memory;
extern PGDLLIMPORT bool vexec_enable_sorted_agg;
extern PGDLLIMPORT int vexec_repartition_min_rows;
extern PGDLLIMPORT int vexec_partial_agg_limit;
extern PGDLLIMPORT bool vexec_partial_agg_bypass;
extern PGDLLIMPORT bool vexec_remember_agg_sizes;
extern PGDLLIMPORT bool vexec_enable_group_topk;
extern PGDLLIMPORT bool vexec_enable_shared_exprs;
extern PGDLLIMPORT bool vexec_enable_row_memo;
extern PGDLLIMPORT bool vexec_aggregate_statistics;
extern PGDLLIMPORT bool vexec_enable_hashjoin;
extern PGDLLIMPORT bool vexec_enable_join_filter;
extern PGDLLIMPORT bool vexec_enable_sort;
extern PGDLLIMPORT bool vexec_prefer_vector_sort;
extern PGDLLIMPORT bool vexec_enable_late_exchange;
extern PGDLLIMPORT bool vexec_enable_running_bound;
extern PGDLLIMPORT bool vexec_enable_typed_bound;
extern PGDLLIMPORT bool vexec_enable_granule_bound;
extern PGDLLIMPORT bool vexec_enable_final_subpartitions;
extern PGDLLIMPORT bool vexec_enable_final_references;
extern PGDLLIMPORT bool vexec_enable_final_record_groups;
extern PGDLLIMPORT bool vexec_enable_one_key_batches;
extern PGDLLIMPORT bool vexec_enable_vector_keys;
extern PGDLLIMPORT bool vexec_enable_key_runs;
extern PGDLLIMPORT bool vexec_enable_length_columns;
extern PGDLLIMPORT bool vexec_enable_topk_table_bound;
extern PGDLLIMPORT bool vexec_bound_fills_cache;
extern PGDLLIMPORT bool vexec_enable_count_under_quals;
extern PGDLLIMPORT bool vexec_enable_bypass_over_runs;
extern PGDLLIMPORT bool vexec_enable_pack_prefetch;
extern PGDLLIMPORT bool vexec_enable_consecutive_keys;
extern PGDLLIMPORT bool vexec_enable_participant_rank;
extern PGDLLIMPORT bool vexec_enable_uninformed_groups;
extern PGDLLIMPORT bool vexec_condcache_planning;
extern PGDLLIMPORT bool vexec_enable_window;
extern PGDLLIMPORT bool vexec_enable_append;
extern PGDLLIMPORT bool vexec_enable_insert;
extern PGDLLIMPORT bool vexec_enable_motion_frames;
extern PGDLLIMPORT bool vexec_orca;
extern PGDLLIMPORT bool vexec_orca_cost_model;
extern PGDLLIMPORT char *vexec_orca_settings;
extern PGDLLIMPORT double vexec_compact_threshold;
extern PGDLLIMPORT int vexec_batch_rows;
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
extern PGDLLIMPORT int vexec_gpu_chunk_rows;
extern PGDLLIMPORT double vexec_gpu_request_share;
extern PGDLLIMPORT bool vexec_enable_gpu_agg;
extern PGDLLIMPORT bool vexec_enable_gpu_hashjoin;
extern PGDLLIMPORT bool vexec_enable_gpu_sort;
extern PGDLLIMPORT bool vexec_enable_gpu_scan;
extern PGDLLIMPORT bool vexec_gpu_price_asked;
extern PGDLLIMPORT bool vexec_gpu_sort_rank;
extern PGDLLIMPORT int vexec_precision;
extern PGDLLIMPORT bool vexec_debug_require_vector;
extern PGDLLIMPORT bool vexec_debug_check_plans;
extern PGDLLIMPORT int vexec_debug_layout_seed;
extern PGDLLIMPORT int vexec_debug_gpu_rank_hash_bits;

extern const struct config_enum_entry vexec_mode_options[];

/* vexec.c */
extern const char *vexec_mode_name(int mode);

/*
 * plan/orca.c: vexec.orca_settings, ORCA's settings for the statements vexec
 * takes (gp_orca_vec.h, set_options), as measured one by one (V5).
 */
#define VEXEC_ORCA_SETTINGS_DEFAULT ""
extern bool vexec_orca_settings_check(char **newval, void **extra, GucSource source);

/* plan/paths.c: the planner's hooks */
extern void vexec_planner_install(void);

/* plan/explain.c: EXPLAIN's vexec option */
extern void vexec_explain_install(void);

/* source/registry.c */
extern void vexec_source_registry_install(void);

/* expr/packs.c: the kernel packs' registry (vexec_kernels.h) */
extern void vexec_kernel_packs_install(void);

/* exec/methods.c: the vector nodes' methods, registered by name */
extern void vexec_exec_install(void);

#endif							/* VEXEC_H */
