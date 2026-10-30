/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * vexec.c
 *	  _PG_init, and the settings of pg_vector_executor.md §3.12.
 *
 * vexec may only be preloaded (§1.2): without "shared_preload_libraries", a
 * LOAD, a CREATE EXTENSION or a call of one of its functions fails before
 * anything is registered.  Preloaded and with vexec.mode = off, every hook
 * calls the one it took the place of and adds nothing, so the server
 * behaves as PostgreSQL 19 does.
 *
 * Every setting is dotted, and the prefix is reserved, so that a name left
 * in a configuration file becomes a placeholder rather than an error, as
 * the port requires of its own names.  The planner's settings are marked
 * for EXPLAIN (SETTINGS), as PostgreSQL's enable_* are.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>
#include <limits.h>

#include "cb_module.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/guc.h"
#include "utils/plancache.h"

#include "vexec.h"
#include "egress/egress.h"
#include "exec/exec.h"
#include "plan/plan.h"

PG_MODULE_MAGIC_EXT(
					.name = "vexec",
					.version = VEXEC_VERSION
);

/* vexec.mode */
int			vexec_mode = VEXEC_MODE_OFF;

/* the cost model's factors (§3.3.2); Estimates until V2 measures them */
double		vexec_cpu_tuple_factor = 0.35;
double		vexec_cpu_operator_factor = 0.35;
double		vexec_convert_cost = 0.001;
double		vexec_batch_setup_cost = 1.0;
int			vexec_min_rows = 10000;
int			vexec_repartition_min_rows = 500000;
int			vexec_scan_rows_per_worker = 163840;
int			vexec_scan_row_bytes = 32;

/* per node kind */
bool		vexec_enable_scan = true;
bool		vexec_heap_page_reader = true;
bool		vexec_enable_prefilter = true;
bool		vexec_enable_source_layouts = true;
bool		vexec_trust_source_text = true;
bool		vexec_enable_source_steps = true;
bool		vexec_enable_source_join_keys = true;
bool		vexec_enable_bitmapscan = true;
bool		vexec_enable_agg = true;
bool		vexec_enable_agg_below_min_rows = true;
bool		vexec_enable_repartition = true;
bool		vexec_enable_packed_groups = true;
bool		vexec_enable_agg_split = true;
int			vexec_exchange_memory = 0;
bool		vexec_enable_exchange_area = true;
bool		vexec_drop_implied_keys = true;
bool		vexec_drop_implied_key_inputs = true;
int			vexec_repartition_shared_memory = -1;
bool		vexec_enable_sorted_agg = true;
int			vexec_partial_agg_limit = 16384;
bool		vexec_partial_agg_bypass = true;
bool		vexec_remember_agg_sizes = true;
bool		vexec_enable_group_topk = true;
bool		vexec_enable_shared_exprs = true;
bool		vexec_enable_row_memo = true;
bool		vexec_aggregate_statistics = true;
bool		vexec_enable_hashjoin = true;
bool		vexec_enable_join_filter = true;
bool		vexec_enable_sort = true;
bool		vexec_prefer_vector_sort = true;
bool		vexec_enable_late_exchange = true;
bool		vexec_enable_running_bound = true;
bool		vexec_enable_typed_bound = true;
bool		vexec_enable_granule_bound = true;
bool		vexec_enable_final_subpartitions = true;
bool		vexec_enable_final_references = true;
bool		vexec_enable_final_record_groups = true;
bool		vexec_enable_one_key_batches = true;
bool		vexec_enable_vector_keys = true;
bool		vexec_enable_key_runs = true;
bool		vexec_enable_length_columns = true;
bool		vexec_enable_topk_table_bound = true;
bool		vexec_bound_fills_cache = true;
bool		vexec_enable_count_under_quals = true;
bool		vexec_enable_bypass_over_runs = true;
bool		vexec_enable_pack_prefetch = true;
bool		vexec_enable_consecutive_keys = true;
bool		vexec_enable_participant_rank = true;
bool		vexec_enable_uninformed_groups = true;
bool		vexec_condcache_planning = true;
bool		vexec_enable_window = true;
bool		vexec_enable_append = true;
bool		vexec_enable_insert = true;
bool		vexec_enable_motion_frames = true;
bool		vexec_orca = true;
bool		vexec_orca_cost_model = true;
char	   *vexec_orca_settings = NULL;
double		vexec_compact_threshold = 0.25;

/* the rows a batch is filled with (F24, §3.38), and the batch formats (§3.4.4) */
int			vexec_batch_rows = VEXEC_BATCH_ROWS_SETTING_MAX;
int			vexec_batch_format = VEXEC_FORMAT_POSTGRES;
int			vexec_batch_varlena_layout = VEXEC_VARLENA_FORMAT;
int			vexec_batch_bool_layout = VEXEC_BOOL_FORMAT;
int			vexec_batch_temporal_layout = VEXEC_TEMPORAL_FORMAT;
int			vexec_batch_numeric_layout = VEXEC_NUMERIC_FORMAT;
bool		vexec_batch_dictionary = true;

/* the GPU tier (§3.11): off at every level by default */
int			vexec_gpu = VEXEC_GPU_OFF;
double		vexec_gpu_setup_cost = 1000.0;
double		vexec_gpu_transfer_cost = 0.0001;
double		vexec_gpu_operator_factor = 0.0625;
bool		vexec_gpu_segments = false;
int			vexec_gpu_chunk_rows = 1048576;
double		vexec_gpu_request_share = 0.125;
bool		vexec_enable_gpu_agg = true;
bool		vexec_enable_gpu_hashjoin = true;
bool		vexec_enable_gpu_sort = true;
bool		vexec_enable_gpu_scan = true;
bool		vexec_gpu_price_asked = true;
bool		vexec_gpu_sort_rank = true;

/* the two modes (§3.11): every answer PostgreSQL's, or fast */
int			vexec_precision = VEXEC_PRECISION_PRECISE;

/* tests */
bool		vexec_debug_require_vector = false;
bool		vexec_debug_check_plans = false;
int			vexec_debug_layout_seed = 0;
int			vexec_debug_gpu_rank_hash_bits = 0;

const struct config_enum_entry vexec_mode_options[] = {
	{"off", VEXEC_MODE_OFF, false},
	{"explain", VEXEC_MODE_EXPLAIN, false},
	{"auto", VEXEC_MODE_AUTO, false},
	{"force", VEXEC_MODE_FORCE, false},
	{NULL, 0, false}
};

static const struct config_enum_entry batch_format_options[] = {
	{"postgres", VEXEC_FORMAT_POSTGRES, false},
	{"arrow", VEXEC_FORMAT_ARROW, false},
	{NULL, 0, false}
};

static const struct config_enum_entry varlena_layout_options[] = {
	{"format", VEXEC_VARLENA_FORMAT, false},
	{"datum", VEXEC_VARLENA_DATUM, false},
	{"view", VEXEC_VARLENA_VIEW, false},
	{"offsets", VEXEC_VARLENA_OFFSETS, false},
	{NULL, 0, false}
};

static const struct config_enum_entry bool_layout_options[] = {
	{"format", VEXEC_BOOL_FORMAT, false},
	{"byte", VEXEC_BOOL_BYTE, false},
	{"bit", VEXEC_BOOL_BIT, false},
	{NULL, 0, false}
};

static const struct config_enum_entry temporal_layout_options[] = {
	{"format", VEXEC_TEMPORAL_FORMAT, false},
	{"postgres", VEXEC_TEMPORAL_POSTGRES, false},
	{"arrow", VEXEC_TEMPORAL_ARROW, false},
	{NULL, 0, false}
};

static const struct config_enum_entry numeric_layout_options[] = {
	{"format", VEXEC_NUMERIC_FORMAT, false},
	{"scaled", VEXEC_NUMERIC_SCALED, false},
	{"varlena", VEXEC_NUMERIC_VARLENA, false},
	{NULL, 0, false}
};

static const struct config_enum_entry gpu_options[] = {
	{"off", VEXEC_GPU_OFF, false},
	{"auto", VEXEC_GPU_AUTO, false},
	{"force", VEXEC_GPU_FORCE, false},
	{NULL, 0, false}
};

static const struct config_enum_entry precision_options[] = {
	{"precise", VEXEC_PRECISION_PRECISE, false},
	{"fast", VEXEC_PRECISION_FAST, false},
	{NULL, 0, false}
};

static void define_settings(void);

/*
 * vexec.enable_motion_frames, as it changes: frames change a plan's shape
 * (§3.10, "Switching frames and transports"), so the session's cached
 * plans are planned again at their next execution, with the new value --
 * as DISCARD PLANS has them (PG19:src/backend/commands/discard.c:41, 76).
 * ResetPlanCache() marks them invalid, and raises nothing.
 */
static void
assign_enable_motion_frames(bool newval, void *extra)
{
	(void) extra;
	if (newval != vexec_enable_motion_frames)
		ResetPlanCache();
}

/*
 * vexec.batch_rows (F24, §3.38): a power of two, as the rows a batch can
 * hold are; the setting's bounds keep it within them
 */
static bool
check_batch_rows(int *newval, void **extra, GucSource source)
{
	(void) extra;
	(void) source;
	if ((*newval & (*newval - 1)) != 0)
	{
		GUC_check_errdetail("vexec.batch_rows must be a power of two.");
		return false;
	}
	return true;
}

const char *
vexec_mode_name(int mode)
{
	const struct config_enum_entry *e;

	for (e = vexec_mode_options; e->name != NULL; e++)
		if (e->val == mode)
			return e->name;
	return "?";
}

void
_PG_init(void)
{
	/*
	 * vexec installs the planner's hooks, which every backend's plans go
	 * through, and registers the batch sources' registry and the kernel
	 * packs' for storage modules and packs to find while the postmaster
	 * loads them (§3.5.1, §3.17): neither can be done once the postmaster is
	 * running.  It does not need gp_core, so it never asks for it (§3.2).
	 */
	CB_REQUIRE_PRELOAD("vexec");

	define_settings();
	/* F3's pool of long-lived parallel workers: its settings among vexec's, its shared memory */
	vexec_pool_install();
	MarkGUCPrefixReserved("vexec");

	vexec_source_registry_install();
	vexec_kernel_packs_install();
	vexec_exec_install();
	/* the groups VecAgg's tables reached, in shared memory (C6) */
	vexec_aggsize_install();
	/* the exchange area, D2's partitions' blocks, in shared memory (M1) */
	vexec_exchange_install();
	/* the condition cache, in shared memory (F7) */
	vexec_condcache_install();
	/* the columns statements asked the device for, in shared memory (W20) */
	vexec_device_asked_install();
	/* pg_otel_probe's spans of vector nodes and statements, where it is loaded */
	vexec_report_install();
	vexec_planner_install();
	vexec_orca_install();
	vexec_explain_install();
	vexec_egress_install();
	vexec_ingest_install();

	/*
	 * The GPU tier is an extension of its own, vexec_gpu (§3.11): its
	 * arena, worker and executor are its, found through the device contract
	 * (vexec_device.h) as a node needs them.  Without it every node runs on
	 * the CPU, whatever vexec.gpu says.
	 */
}

static void
define_settings(void)
{
	DefineCustomEnumVariable("vexec.mode",
							 "Whether vector plans are considered, and how.",
							 "off builds no vector alternative; explain costs and records them but never "
							 "chooses one; auto chooses by cost; force chooses one wherever it is possible.",
							 &vexec_mode,
							 VEXEC_MODE_OFF,
							 vexec_mode_options,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);

	DefineCustomRealVariable("vexec.cpu_tuple_factor",
							 "The vector executor's share of cpu_tuple_cost.",
							 NULL,
							 &vexec_cpu_tuple_factor,
							 0.35, 0.0, 100.0,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("vexec.cpu_operator_factor",
							 "The vector executor's share of cpu_operator_cost, for an operation a kernel runs.",
							 "An operation that runs in PostgreSQL's own evaluator, row by row, keeps the whole cost.",
							 &vexec_cpu_operator_factor,
							 0.35, 0.0, 100.0,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("vexec.convert_cost",
							 "The cost of moving one value between a row and a batch.",
							 NULL,
							 &vexec_convert_cost,
							 0.001, 0.0, DBL_MAX,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("vexec.batch_setup_cost",
							 "The cost of starting one vector node.",
							 NULL,
							 &vexec_batch_setup_cost,
							 1.0, 0.0, DBL_MAX,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.min_rows",
							"Below this estimate of a relation's rows no vector alternative is built.",
							"It saves planning time; force mode ignores it.",
							&vexec_min_rows,
							10000, 0, INT_MAX,
							PGC_USERSET, GUC_EXPLAIN,
							NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.scan_rows_per_worker",
							"Gives a vector scan of a batch source at least a parallel worker this many rows.",
							"A source's pages are its compressed bytes, which PostgreSQL's rule sizes a "
							"scan's workers by.  Up to max_parallel_workers_per_gather; a parallel_workers "
							"reloption stays the relation's; 0: PostgreSQL's rule alone.",
							&vexec_scan_rows_per_worker,
							163840, 0, INT_MAX,
							PGC_USERSET, GUC_EXPLAIN,
							NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.scan_row_bytes",
							"Counts a row of a vector scan as a row of vexec.scan_rows_per_worker for each "
							"this many bytes its columns decode to.",
							"A row of long texts makes more work than a row of a few integers: where the source "
							"tells the bytes its scan decodes, a row counts as their bytes over this many, at "
							"least one; and a scan has no more participants than the units its source cuts it "
							"into.  0: every row counts one.",
							&vexec_scan_row_bytes,
							32, 0, INT_MAX,
							PGC_USERSET, GUC_EXPLAIN,
							NULL, NULL, NULL);

	DefineCustomBoolVariable("vexec.enable_scan",
							 "Enables vector scans.",
							 NULL, &vexec_enable_scan, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.heap_page_reader",
							 "Reads heap tables a page at a time into a vector scan's batches.",
							 "Off, a vector scan of a heap table reads it a row at a time, "
							 "through the table's slots, as it reads any other access method's.",
							 &vexec_heap_page_reader, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_prefilter",
							 "Checks a heap scan's leading quals on a tuple before deforming it.",
							 "Heap's page reader walks a tuple only as far as the attributes of the quals "
							 "that cannot raise -- comparisons with constants, IN lists, LIKE literals, "
							 "NULL tests -- and drops it at the first that fails.",
							 &vexec_enable_prefilter, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_source_layouts",
							 "Keeps a source's text in the layout it hands it, through a vector scan's quals.",
							 "A vector scan reads the text of a registered source's batches -- offsets, "
							 "views or Datums -- as the source gave it, through its kernels and its "
							 "parents, and makes it the format's only where PostgreSQL's evaluator reads "
							 "it, at the rows past the scan's vector quals.",
							 &vexec_enable_source_layouts, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.trust_source_text",
							 "Takes the text a source vouches for as the database encoding's whole, unchecked.",
							 "A registered source may say of a batch's text column that each of its values "
							 "was written through PostgreSQL's input functions, as PostgreSQL's own rows' "
							 "are: the text kernels then check none of them, as PostgreSQL checks no stored "
							 "value.  Off, every value is checked as any other source's.",
							 &vexec_trust_source_text, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_source_steps",
							 "Reads a source's columns in steps, the quals' first.",
							 "A vector scan of a registered source that takes steps asks for the columns of "
							 "its first vector qual first, then each later qual's, then the rest, each step "
							 "only for the batches where a row is left, so that the source decodes a later "
							 "step's columns only in its units where the quals before kept a row.",
							 &vexec_enable_source_steps, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_source_join_keys",
							 "Gives a source a hash join's inner keys, to pass its units over by.",
							 "A vector scan on the outer side of an inner, semi or right hash join gives "
							 "a registered source that takes them the inner keys' range, and their values "
							 "where they are few, so that the source passes over the units -- parts, "
							 "granules -- none of whose rows any inner row meets; the join builds its "
							 "table before it reads an outer row.",
							 &vexec_enable_source_join_keys, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_bitmapscan",
							 "Enables vector bitmap heap scans.",
							 "The pages a bitmap index scan selects, read a batch at a time.",
							 &vexec_enable_bitmapscan, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_agg",
							 "Enables vector aggregation.",
							 NULL, &vexec_enable_agg, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_agg_below_min_rows",
							 "Plans a vector aggregation over a vector scan whatever vexec.min_rows says.",
							 "vexec.min_rows keeps a grouping of a few rows on PostgreSQL's executor where "
							 "its input is rows, which a vector node would convert; a vector scan's batches "
							 "are vector already, and PostgreSQL's Sort or HashAggregate would read them "
							 "as rows (pg_vector_executor.md §3.38, F20).",
							 &vexec_enable_agg_below_min_rows, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_repartition",
							 "Enables parallel vector aggregation that finalizes in every participant.",
							 "Each participant's partial groups are dealt out among them by their keys' hash, "
							 "and each finalizes its own; the Gather receives final groups.  A count(DISTINCT) "
							 "runs in parallel the same way, its pairs of keys and argument dealt out first.",
							 &vexec_enable_repartition, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_agg_split",
							 "Plans a count(DISTINCT) grouping's other aggregates apart, by the keys alone.",
							 "The other aggregates group the input by the keys, its count(DISTINCT) runs "
							 "its stages with no other state, and the two aggregations' groups are joined "
							 "by their keys; it competes with the grouping planned whole by cost.",
							 &vexec_enable_agg_split, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.drop_implied_keys",
							 "Leaves out of a vector grouping's keys those the other keys imply.",
							 "A key that is an immutable expression of other keys -- columns compared by "
							 "identity, as integers, dates, timestamps and text are -- makes no group the "
							 "others do not; it is computed from them where the output reads it "
							 "(pg_vector_executor.md §3.21, M3).",
							 &vexec_drop_implied_keys, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.drop_implied_key_inputs",
							 "Leaves the keys vexec.drop_implied_keys drops out of the vector scan below too.",
							 "A vector aggregation reads none of them -- its output computes them from the "
							 "kept keys -- so the vector scan under it no longer computes them for every row "
							 "(pg_vector_executor.md §3.38, F22).",
							 &vexec_drop_implied_key_inputs, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_packed_groups",
							 "Hands a parallel aggregation's partial groups on to its final stage as they lie.",
							 "A partial stage under a VecRepartition makes its groups in the partitions they "
							 "go to, their keys and states side by side in vexec's own layout, which the final "
							 "stage merges as they lie; the partitions in the parallel query's shared memory, "
							 "in files only past hash_mem.  Off, the groups go on as batches of PostgreSQL's "
							 "partial states.",
							 &vexec_enable_packed_groups, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.exchange_memory",
							"The main shared memory a parallel aggregation's partitions are cut from first.",
							"Kept from the server's start and never given back, so that its pages are not "
							"reserved, zeroed and released again with each query's own segments; 0 makes none "
							"(pg_vector_executor.md §3.21, M1).",
							&vexec_exchange_memory, 0, 0, INT_MAX / 2,
							PGC_POSTMASTER, GUC_UNIT_MB, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.condition_cache_size",
							"The main shared memory of the condition cache: the granules no row of which is true of a scan's quals.",
							"Kept from the server's start; 0 makes no cache (pg_vector_executor.md §3.28, F7).",
							&vexec_condition_cache_size, 64 * 1024, 0, INT_MAX / 2,
							PGC_POSTMASTER, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_condition_cache",
							 "Passes over the granules no row of which is true of a vector scan's quals, as the condition cache knows them.",
							 "A source of units that never change -- a MergeTree part's marks -- leaves unread the granules "
							 "an earlier scan of the same immutable quals found none of whose rows true, and a scan "
							 "keeps those it finds so.  Off, a scan neither reads the cache nor fills it.",
							 &vexec_enable_condition_cache, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_exchange_area",
							 "Lets a parallel aggregation's partitions take blocks of the exchange area.",
							 "Off, they are cut from segments of each participant's own, as without an area.",
							 &vexec_enable_exchange_area, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.repartition_shared_memory",
							"The shared memory each participant's packed groups may take in a parallel aggregation's partitions.",
							"Past it, they go to files; -1 is hash_mem, 0 none.",
							&vexec_repartition_shared_memory, -1, -1, MAX_KILOBYTES,
							PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_sorted_agg",
							 "Enables vector grouping of input in its keys' order, which a source's serial scan hands.",
							 "A row whose keys are the open group's goes to it, any other closes it: no table, "
							 "no spill, each group handed up as it closes.  Where the scan's rows turn out not to "
							 "come in that order as it begins, the grouping hashes.",
							 &vexec_enable_sorted_agg, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.repartition_min_rows",
							"The input rows from which a grouping under ORCA's parallel plan is repartitioned.",
							"On one node gp_orca's parallel pass splits a grouping of many groups in three "
							"stages below a Gather, its partial groups dealt out among the participants, "
							"where its input by ORCA's estimate has at least these rows -- twice these at the "
							"Gather's top, where a Gather below the grouping is the other plan; fewer do not "
							"win back the repartition's files.",
							&vexec_repartition_min_rows, 500000, 0, INT_MAX,
							PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.partial_agg_limit",
							"The groups a parallel aggregation's first stage keeps before it hands new keys on.",
							"Past them, the first stage under a repartition updates only the groups it holds, "
							"and hands each batch's others on as partial states, until the keys it hands on "
							"repeat; 0 keeps every group.",
							&vexec_partial_agg_limit, 16384, 0, INT_MAX,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.partial_agg_bypass",
							 "Lets a frozen first stage whose rows make groups of their own hand them on with no lookup.",
							 "Where its rows seldom meet a key it holds or one of their batch's, each row "
							 "of the next batches is a group of its own in the partition it goes to, its "
							 "packed groups' only.",
							 &vexec_partial_agg_bypass, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_shared_exprs",
							 "Computes once a batch the subexpressions a vector node's targets share.",
							 "Equal subtrees of kernels, over columns and constants, are one, and its "
							 "column is kept for the batch for each target that asks for the same rows.",
							 &vexec_enable_shared_exprs, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_row_memo",
							 "Reuses a row-by-row target's result for an input value it has met.",
							 "A target PostgreSQL's evaluator computes a row at a time, a function of one "
							 "input column with no volatile function, keeps its results by that column's "
							 "value, up to 16384 of them, and computes each value once, at its first row.",
							 &vexec_enable_row_memo, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_group_topk",
							 "Lets a vector aggregation under a LIMIT without ORDER BY keep only the groups it returns.",
							 "Any n groups are the answer of GROUP BY ... LIMIT n: the aggregation keeps "
							 "those of the n smallest hashes of their keys, and passes over the rows of a "
							 "key past them.  Read when a statement is planned.",
							 &vexec_enable_group_topk, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.remember_agg_sizes",
							 "Sizes a vector aggregation's hash table by the groups the same node held before.",
							 "The groups each node's table reached are kept in shared memory by the statement's "
							 "query id, or its text, and the node; off, a table begins at the planner's estimate "
							 "and grows as it fills.",
							 &vexec_remember_agg_sizes, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.aggregate_statistics",
							 "Lets a batch source answer a vector aggregate from the statistics it keeps.",
							 "A plain aggregate over a scan with no qual takes a unit of rows -- "
							 "a file, a group -- from its statistics where they are exact.",
							 &vexec_aggregate_statistics, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_hashjoin",
							 "Enables vector hash joins.",
							 NULL, &vexec_enable_hashjoin, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_join_filter",
							 "Lets a vector hash join lend the scan on its outer side its inner keys.",
							 "Once its table is built, an inner, semi or right join's integer key "
							 "-- its range, and its values as a bitmap or a Bloom filter -- goes to the "
							 "vector scan below it, which drops the rows no inner row can match.",
							 &vexec_enable_join_filter, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_sort",
							 "Enables vector sorts.",
							 NULL, &vexec_enable_sort, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.prefer_vector_sort",
							 "Lets a vector sort under a Gather Merge replace core's row sort of the same input "
							 "where it costs no more.",
							 "Where the two cost the same within the planner's fuzz, the row sort's path is "
							 "kept; this takes it out where the first sort key is compared as an integer "
							 "(pg_vector_executor.md §3.21, M5).",
							 &vexec_prefer_vector_sort, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_late_exchange",
							 "Lets a parallel bounded vector sort fetch its late columns for the whole sort's rows alone.",
							 "Its participants publish their rows' first keys and TIDs, the bound of all of them "
							 "decides which rows are fetched, and whichever participant is free fetches each "
							 "(pg_vector_executor.md §3.21, M4).",
							 &vexec_enable_late_exchange, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_running_bound",
							 "Lets a bounded vector sort bound the vector scan below it.",
							 "Once the sort holds its rows, the scan drops a row whose first sort key "
							 "is past the first key of the sort's last row, which the sort would drop.",
							 &vexec_enable_running_bound, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_typed_bound",
							 "Compares a running bound's integer key with the bound a word of rows at a time.",
							 "A first key of bigint, timestamp, timestamptz, integer or date, held flat, is "
							 "compared as the integers it holds, as PostgreSQL's comparator compares them, "
							 "with no Datum made and no comparator called a row; any other key goes through "
							 "the comparator (pg_vector_executor.md §3.38, F18).",
							 &vexec_enable_typed_bound, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_granule_bound",
							 "Lets a running bound's source pass over the granules of a unit it reads.",
							 "A source that keeps each granule's least and greatest values -- a mergetree "
							 "table's min/max indexes -- passes over a granule no row of which can pass the "
							 "bound as it moves, not only a whole unit (the source contract's minor version 17; "
							 "pg_vector_executor.md §3.38, F25).",
							 &vexec_enable_granule_bound, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_final_subpartitions",
							 "Merges a final aggregation stage's partial groups a cache-sized run at a time.",
							 "A parallel grouping's final stage scatters the partial groups it is handed by "
							 "their hash into sub-buffers, then merges a run of them at a time into a table "
							 "sized for the run, handing its groups on before the next, where one table of "
							 "every group misses the caches a group (pg_vector_executor.md §3.38, F23).",
							 &vexec_enable_final_subpartitions, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_final_references",
							 "Merges a final aggregation stage's partial groups where the exchange holds them.",
							 "A final stage's sub-buffers hold references to its partial groups in the exchange's "
							 "chunks, which stay until its groups are out, in place of their copies; and a group "
							 "of keys and states by value is, where its run first meets it, the partial group "
							 "itself (pg_vector_executor.md §3.38, F29).",
							 &vexec_enable_final_references, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_final_record_groups",
							 "Makes a final aggregation stage's partial group, where it may, its group.",
							 "With vexec.enable_final_references, a group whose keys are their bits or bytes and "
							 "whose states are by value is, where its run first meets it, the partial group "
							 "itself where the exchange keeps it -- no group made, nothing copied or merged "
							 "(pg_vector_executor.md §3.38, F29).",
							 &vexec_enable_final_record_groups, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_vector_keys",
							 "Reads a grouping's integer keys from their columns, with no probe made a row.",
							 "A hashed grouping's batch whose keys are integers with no NULL has their hashes "
							 "made over the columns, a row's keys compared with the row before's by their "
							 "values, and a probe made only for a row looked up (pg_vector_executor.md §3.38, "
							 "F30).",
							 &vexec_enable_vector_keys, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_count_under_quals",
							 "Lets a source answer a plain count(*) under a scan's one qual, unit by unit.",
							 "Where the source's estimate says every unit's rows that satisfy the qual are known "
							 "without reading them -- a mergetree column's <> its default, by its parts' exact "
							 "figures -- count(*) over the serial scan reads none (pg_vector_executor.md §3.38, "
							 "F36).",
							 &vexec_enable_count_under_quals, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.bound_fills_condition_cache",
							 "Lets a scan under a running bound teach the condition cache.",
							 "A LIMIT's running bound checked before the quals leaves them unevaluated on the rows "
							 "it drops, so that their granules are never known to hold no row true: where the cache "
							 "has not seen a batch's granule whole the quals go first, and a later try passes the "
							 "granule over (pg_vector_executor.md §3.38, F19).",
							 &vexec_bound_fills_cache, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_topk_table_bound",
							 "Sizes a grouping's table under a LIMIT without ORDER BY by the groups the LIMIT keeps.",
							 "Under its hash bound (vexec.enable_group_topk) a table holds few groups whatever the "
							 "planner's estimate of all of them, and grows if it must (pg_vector_executor.md §3.38, "
							 "F38).",
							 &vexec_enable_topk_table_bound, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_length_columns",
							 "Asks a source for a text column's character counts where only they are read.",
							 "A column a scan and a VecAgg above it read only by length() and by comparison with "
							 "the empty string is asked for so; a source that keeps its values' characters -- a "
							 "mergetree storage's hidden column of them -- hands them, no value's bytes read "
							 "(pg_vector_executor.md §3.38, F35 and F37).",
							 &vexec_enable_length_columns, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_key_runs",
							 "Cuts a grouping's batch into runs of one key before any row is hashed.",
							 "A hashed grouping's batch whose keys are integers, bytes or a dictionary's codes has "
							 "its rows compared with the row before's over the columns; each run's first row alone "
							 "is hashed and looked up, and its rows take the group at once, a count(*) by the run's "
							 "length (pg_vector_executor.md §3.38, F32).  It needs vexec.enable_consecutive_keys.",
							 &vexec_enable_key_runs, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_bypass_over_runs",
							 "Measures a frozen first stage's bypass over the rows it looks up, not its rows.",
							 "Where a batch is cut into runs (vexec.enable_key_runs), a run's repeats do not count "
							 "against vexec.partial_agg_bypass's share, which is lower for it, and each bypassed "
							 "run is a packed group of its own (pg_vector_executor.md §3.38, F33).",
							 &vexec_enable_bypass_over_runs, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_pack_prefetch",
							 "Fetches a partition's next cache lines for writing as its packed groups are made.",
							 "A first stage writes its packed groups to many partitions at once, more streams than "
							 "the hardware's prefetchers follow, each new line a demand fetch: prefetchw ahead of "
							 "each (pg_vector_executor.md §3.38, F34).",
							 &vexec_enable_pack_prefetch, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_one_key_batches",
							 "Takes a grouping's batch whose rows are of one key as a plain aggregation's.",
							 "A hashed grouping's batch whose key columns hold one value -- compared by their "
							 "bits, bytes or dictionary codes before any row is hashed -- takes its group once "
							 "and its transitions over the batch at once into it (pg_vector_executor.md §3.38, "
							 "F31).",
							 &vexec_enable_one_key_batches, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_consecutive_keys",
							 "Lets a grouping's row whose keys are the row before's take its group without a lookup.",
							 "A hashed grouping's row whose key hash and keys -- compared by their bits or "
							 "bytes -- are the previous row's takes that row's group, as ClickHouse's hash "
							 "methods keep the last key's place: a table read in its keys' order repeats them "
							 "(pg_vector_executor.md §3.38, F26).",
							 &vexec_enable_consecutive_keys, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_participant_rank",
							 "Ranks a parallel grouping's LIMIT in each participant whatever its groups' estimate.",
							 "A bounded vector sort over a hashed grouping's final stage below the Gather, for a "
							 "LIMIT of a batch's rows at most, is neither refused for the few groups estimated nor "
							 "left to core's Sort over a Gather of every group where it costs no more, by "
							 "add_path()'s fuzz (pg_vector_executor.md §3.38, F17).",
							 &vexec_enable_participant_rank, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_uninformed_groups",
							 "Compares a LIMIT's sorted grouping by its whole cost where its group estimate knows nothing.",
							 "Where an expression's groups, which have no statistics, are estimated at nine in ten "
							 "of the input's rows or more, core's sorted grouping under a LIMIT is not kept for the "
							 "share of its groups the LIMIT takes: VecSort over a hashed VecAgg replaces it where "
							 "its whole cost is no more (pg_vector_executor.md §3.38, F27).",
							 &vexec_enable_uninformed_groups, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_condition_cache_planning",
							 "Prices a scan by the granules the condition cache leaves it, where scans of its quals saw them.",
							 "The condition cache keeps, for a relation and its quals, the granules scans of them saw "
							 "whole and those found none true; planning takes the share left off the source's "
							 "estimate of the rows and bytes the scan reads, so that a scan the cache leaves few "
							 "granules runs without workers (pg_vector_executor.md §3.38, F21).",
							 &vexec_condcache_planning, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_window",
							 "Enables vector hashed window aggregation, which only ORCA plans.",
							 NULL, &vexec_enable_window, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_append",
							 "Enables vector appends, which only ORCA plans.",
							 "An Append whose children are vector nodes reads their batches in turn, "
							 "with no row between them and a vector parent.",
							 &vexec_enable_append, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_insert",
							 "Enables vector inserts.",
							 NULL, &vexec_enable_insert, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_motion_frames",
							 "Sends batches across ORCA's Motions, and through a parallel aggregation's "
							 "partitions, as Arrow IPC frames.",
							 "Where a Motion's fragment's top is a vector node, it sends its batches "
							 "as frames, and a vector node above the Motion takes them; off, rows, one "
							 "a row.  Read when a statement is planned: a change of its value has the "
							 "session's cached plans planned again.  A VecRepartition's partitions hold "
							 "frames of the groups its partial aggregation hands it as batches; off, "
							 "tuples.",
							 &vexec_enable_motion_frames, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, assign_enable_motion_frames, NULL);
	DefineCustomBoolVariable("vexec.orca",
							 "Uses ORCA's front end where gp_orca's API is present.",
							 NULL, &vexec_orca, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.orca_cost_model",
							 "Prices vexec's nodes in ORCA's search, and offers ORCA's hashed window.",
							 "In auto and force mode, ORCA's cost model with vexec's nodes priced in "
							 "chooses ORCA's plans, which then get vector nodes where it priced them; "
							 "off, ORCA's own cost model, and vector nodes where vexec's own costs "
							 "choose them.",
							 &vexec_orca_cost_model, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomStringVariable("vexec.orca_settings",
							   "ORCA's settings for the statements vexec takes, set for their planning alone.",
							   "A comma-separated list of name=value, each a setting of gp_orca's "
							   "(gp.optimizer_...), in auto and force mode.",
							   &vexec_orca_settings,
							   VEXEC_ORCA_SETTINGS_DEFAULT,
							   PGC_USERSET, GUC_EXPLAIN | GUC_LIST_INPUT,
							   vexec_orca_settings_check, NULL, NULL);
	DefineCustomRealVariable("vexec.compact_threshold",
							 "Below this share of selected rows, a batch is compacted.",
							 NULL,
							 &vexec_compact_threshold,
							 0.25, 0.0, 1.0,
							 PGC_USERSET, 0,
							 NULL, NULL, NULL);

	DefineCustomIntVariable("vexec.batch_rows",
							"The rows a vector node puts into a batch.",
							"Each node fills a batch it makes with up to this many rows, a power of two "
							"from 1,024 to 4,096, the most a batch can hold; it reads the setting as it "
							"begins the batch, and takes a batch of any rows up to the most.  Fewer rows "
							"keep a batch's columns in the processor's caches, more do each batch's work "
							"fewer times (pg_vector_executor.md §3.38, F24).",
							&vexec_batch_rows,
							VEXEC_BATCH_ROWS_SETTING_MAX, VEXEC_BATCH_ROWS_SETTING_MIN,
							VEXEC_BATCH_ROWS_SETTING_MAX,
							PGC_USERSET, GUC_EXPLAIN,
							check_batch_rows, NULL, NULL);
	DefineCustomEnumVariable("vexec.batch_format",
							 "The in-memory format of batches.",
							 "postgres holds values as PostgreSQL holds them; arrow is Arrow's columnar "
							 "format with its standard types.  The planner reads it for its conversion "
							 "prices, and each vector node when it starts.",
							 &vexec_batch_format,
							 VEXEC_FORMAT_POSTGRES,
							 batch_format_options,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomEnumVariable("vexec.batch_varlena_layout",
							 "How a batch holds text, bytea and the other variable-length types.",
							 "datum: Datums pointing at headered values; view: Arrow's views; offsets: "
							 "Arrow's offsets; format: the batch format's own.",
							 &vexec_batch_varlena_layout,
							 VEXEC_VARLENA_FORMAT,
							 varlena_layout_options,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomEnumVariable("vexec.batch_bool_layout",
							 "How a batch holds booleans: a byte or a bit a value.",
							 "format: the batch format's own.",
							 &vexec_batch_bool_layout,
							 VEXEC_BOOL_FORMAT,
							 bool_layout_options,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomEnumVariable("vexec.batch_temporal_layout",
							 "How a batch holds dates, timestamps and intervals.",
							 "postgres: PostgreSQL's epoch and interval; arrow: the Unix epoch and "
							 "month_day_nano; format: the batch format's own.",
							 &vexec_batch_temporal_layout,
							 VEXEC_TEMPORAL_FORMAT,
							 temporal_layout_options,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomEnumVariable("vexec.batch_numeric_layout",
							 "How a batch holds a numeric whose typmod bounds its digits.",
							 "scaled: a 64- or 128-bit integer at the typmod's scale; varlena: the "
							 "batch format's layout for other numerics; format: the batch format's "
							 "own, which is scaled in both.",
							 &vexec_batch_numeric_layout,
							 VEXEC_NUMERIC_FORMAT,
							 numeric_layout_options,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.batch_dictionary",
							 "Lets sources hand dictionary-encoded columns.",
							 "A vector scan asks its source for the columns it keeps as dictionaries "
							 "-- ch_table_engines' LowCardinality columns -- as their codes and "
							 "dictionaries, and evaluates an expression of one such column over the "
							 "dictionary's values, once for every batch that comes with it.",
							 &vexec_batch_dictionary, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);

	DefineCustomEnumVariable("vexec.gpu",
							 "Whether VecAgg, VecHashJoin and VecSort run their operators on a GPU.",
							 "off gives no node the device's strategy, and one in a cached plan runs on "
							 "the CPU; auto chooses by cost where a device provider (vexec_gpu) serves; "
							 "force takes the device wherever a node's operator is eligible.  It acts "
							 "only with vexec.mode at auto or force.",
							 &vexec_gpu,
							 VEXEC_GPU_OFF,
							 gpu_options,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("vexec.gpu_setup_cost",
							 "The cost of starting a node's operator on a GPU: a task.",
							 NULL,
							 &vexec_gpu_setup_cost,
							 1000.0, 0.0, DBL_MAX,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("vexec.gpu_transfer_cost",
							 "The cost of moving one byte between a backend and a GPU.",
							 NULL,
							 &vexec_gpu_transfer_cost,
							 0.0001, 0.0, DBL_MAX,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("vexec.gpu_operator_factor",
							 "A GPU's share of cpu_operator_cost.",
							 NULL,
							 &vexec_gpu_operator_factor,
							 0.0625, 0.0, 100.0,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.gpu_segments",
							 "Whether the segments have GPU services, for ORCA's fragments.",
							 "The coordinator's statement of the cluster's hardware.",
							 &vexec_gpu_segments, false,
							 PGC_SIGHUP, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.gpu_chunk_rows",
							"The rows a node gathers from its batches before they cross to the device.",
							"A chunk crosses the bus at once: a batch of 1,024 int8 values crosses at "
							"1.6 GB/s, 8 MB at 27.6 (§3.11).",
							&vexec_gpu_chunk_rows,
							1048576, VEXEC_BATCH_ROWS_SETTING_MIN, 64 * 1048576,
							PGC_USERSET, GUC_EXPLAIN,
							NULL, NULL, NULL);
	DefineCustomRealVariable("vexec.gpu_request_share",
							 "The share of the device's free memory a units request's columns are gathered within.",
							 "A request of the units a device reads itself -- Parquet's row groups, Arrow's record "
							 "batches, MergeTree's granules -- holds units while their columns' bytes, decoded, stay "
							 "within this share of the memory the device's tasks have not reserved, as well as up "
							 "to 32 chunks' rows: their decoding takes about twice their bytes, and another "
							 "participant's requests take theirs (pg_vector_executor.md §3.35, W38). "
							 "0 sizes a request by its rows alone.",
							 &vexec_gpu_request_share,
							 0.125, 0.0, 1.0,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_gpu_agg",
							 "Enables VecAgg's device strategy.",
							 NULL,
							 &vexec_enable_gpu_agg, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_gpu_hashjoin",
							 "Enables VecHashJoin's device strategy.",
							 NULL,
							 &vexec_enable_gpu_hashjoin, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_gpu_sort",
							 "Enables VecSort's device strategy.",
							 NULL,
							 &vexec_enable_gpu_sort, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_gpu_scan",
							 "Enables a device strategy's reading of its scan's units itself.",
							 "VecAgg's device strategy over a scan whose source hands Parquet's or "
							 "Arrow's units has the device read and decode them from their files.",
							 &vexec_enable_gpu_scan, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.gpu_price_asked",
							 "Prices a column the device keeps not as kept once a statement has asked the device for it.",
							 "The device keeps the columns it reads.  A statement whose planning priced a "
							 "device read of a column the device keeps not asks for it; the next statement "
							 "prices it as kept, and takes the device and pays the read where that is "
							 "cheaper (pg_vector_executor.md §3.28, W20).  Off, a column is priced as the "
							 "device keeps it.",
							 &vexec_gpu_price_asked, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.gpu_sort_rank",
							 "Lets the device group a grouping ranked by its count by sorting its keys.",
							 "A device grouping whose answer is ranked by count(*), descending, its other "
							 "aggregates counts, sums, minimums and maximums of no float nor text, sorts its "
							 "rows' 64-bit keys, ranks their runs, and groups the ranked keys' rows alone "
							 "(pg_vector_executor.md §3.33, W37).  Off, it groups every row by its hash.",
							 &vexec_gpu_sort_rank, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomEnumVariable("vexec.precision",
							 "Whether every answer is PostgreSQL's, or kernels that declare a fast "
							 "variant may answer otherwise in the last digits or at a boundary.",
							 "precise, the default: every answer is PostgreSQL's or the extension's. "
							 "fast: a float's sum and average in the device's order, a float's min and "
							 "max on the device, and the kernel packs' fast variants; never another "
							 "error, NULL, integer, numeric or text.",
							 &vexec_precision,
							 VEXEC_PRECISION_PRECISE,
							 precision_options,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);

	DefineCustomBoolVariable("vexec.debug_require_vector",
							 "Fails a statement whose plan has no vector node where one was possible.",
							 "For tests that must see vectorization engage: in auto or force mode, a "
							 "plan that the oracle accepted a vector node for and that has none.",
							 &vexec_debug_require_vector, false,
							 PGC_SUSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.debug_layout_seed",
							"Draws the per-structure layout settings at random, from this seed.",
							"For the differential runner's fourth session (pg_vector_executor.md "
							"§6.1): every time a node reads the format in effect, each of the "
							"four per-structure settings is drawn anew.  0 reads the settings.",
							&vexec_debug_layout_seed,
							0, 0, INT_MAX,
							PGC_SUSET, GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.debug_gpu_rank_hash_bits",
							"Cuts the keys' hashes of a device grouping ranked by sort to so many bits.",
							"For tests of vexec.gpu_sort_rank: two keys of one hash among the ranked "
							"groups make the device group the rows by their hash instead.  0 keeps all 64.",
							&vexec_debug_gpu_rank_hash_bits,
							0, 0, 63,
							PGC_SUSET, GUC_NOT_IN_SAMPLE,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.debug_check_plans",
							 "Checks every plan the planner finishes against vexec's invariants.",
							 "On by default where the server checks its assertions.",
							 &vexec_debug_check_plans,
#ifdef USE_ASSERT_CHECKING
							 true,
#else
							 false,
#endif
							 PGC_SUSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
}
