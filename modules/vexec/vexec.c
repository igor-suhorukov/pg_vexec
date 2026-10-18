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
int			vexec_scan_rows_per_worker = 163840;

/* per node kind */
bool		vexec_enable_scan = true;
bool		vexec_heap_page_reader = true;
bool		vexec_enable_prefilter = true;
bool		vexec_enable_source_layouts = true;
bool		vexec_enable_bitmapscan = true;
bool		vexec_enable_agg = true;
bool		vexec_enable_repartition = true;
int			vexec_partial_agg_limit = 16384;
bool		vexec_remember_agg_sizes = true;
bool		vexec_enable_group_topk = true;
bool		vexec_enable_shared_exprs = true;
bool		vexec_enable_row_memo = true;
bool		vexec_aggregate_statistics = true;
bool		vexec_enable_hashjoin = true;
bool		vexec_enable_join_filter = true;
bool		vexec_enable_sort = true;
bool		vexec_enable_running_bound = true;
bool		vexec_enable_window = true;
bool		vexec_enable_insert = true;
bool		vexec_enable_motion_frames = true;
bool		vexec_orca = true;
bool		vexec_orca_cost_model = true;
char	   *vexec_orca_settings = NULL;
double		vexec_compact_threshold = 0.25;

/* the batch formats (§3.4.4) */
int			vexec_batch_format = VEXEC_FORMAT_POSTGRES;
int			vexec_batch_varlena_layout = VEXEC_VARLENA_FORMAT;
int			vexec_batch_bool_layout = VEXEC_BOOL_FORMAT;
int			vexec_batch_temporal_layout = VEXEC_TEMPORAL_FORMAT;
int			vexec_batch_numeric_layout = VEXEC_NUMERIC_FORMAT;
bool		vexec_batch_dictionary = true;

/* the GPU tier (§3.11): off at every level by default */
int			vexec_gpu = VEXEC_GPU_OFF;
double		vexec_gpu_setup_cost = 100.0;
double		vexec_gpu_transfer_cost = 0.00001;
double		vexec_gpu_operator_factor = 0.0625;
bool		vexec_gpu_segments = false;
char	   *vexec_gpu_devices = NULL;
int			vexec_gpu_arena_mb = 256;
int			vexec_gpu_memory_limit_mb = 0;

/* tests */
bool		vexec_debug_require_vector = false;
bool		vexec_debug_check_plans = false;
int			vexec_debug_layout_seed = 0;

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
	MarkGUCPrefixReserved("vexec");

	vexec_source_registry_install();
	vexec_kernel_packs_install();
	vexec_exec_install();
	/* the groups VecAgg's tables reached, in shared memory (C6) */
	vexec_aggsize_install();
	/* pg_otel_probe's spans of vector nodes and statements, where it is loaded */
	vexec_report_install();
	vexec_planner_install();
	vexec_orca_install();
	vexec_explain_install();
	vexec_egress_install();
	vexec_ingest_install();

	/*
	 * This build has no GPU tier (V8): with vexec.gpu_devices set anyway it
	 * says so, and starts nothing (§3.11).
	 */
	if (vexec_gpu_devices != NULL && vexec_gpu_devices[0] != '\0')
		ereport(WARNING,
				(errmsg("vexec was built without its GPU tier; \"vexec.gpu_devices\" is ignored"),
				 errdetail("No GPU service is started, and no GPU is used.")));
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
	DefineCustomBoolVariable("vexec.enable_bitmapscan",
							 "Enables vector bitmap heap scans.",
							 "The pages a bitmap index scan selects, read a batch at a time.",
							 &vexec_enable_bitmapscan, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_agg",
							 "Enables vector aggregation.",
							 NULL, &vexec_enable_agg, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_repartition",
							 "Enables parallel vector aggregation that finalizes in every participant.",
							 "Each participant's partial groups are dealt out among them by their keys' hash, "
							 "and each finalizes its own; the Gather receives final groups.  A count(DISTINCT) "
							 "runs in parallel the same way, its pairs of keys and argument dealt out first.",
							 &vexec_enable_repartition, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.partial_agg_limit",
							"The groups a parallel aggregation's first stage keeps before it hands new keys on.",
							"Past them, the first stage under a repartition updates only the groups it holds, "
							"and hands each batch's others on as partial states, until the keys it hands on "
							"repeat; 0 keeps every group.",
							&vexec_partial_agg_limit, 16384, 0, INT_MAX,
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
	DefineCustomBoolVariable("vexec.enable_running_bound",
							 "Lets a bounded vector sort bound the vector scan below it.",
							 "Once the sort holds its rows, the scan drops a row whose first sort key "
							 "is past the first key of the sort's last row, which the sort would drop.",
							 &vexec_enable_running_bound, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_window",
							 "Enables vector hashed window aggregation, which only ORCA plans.",
							 NULL, &vexec_enable_window, true,
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
							 "Whether GPU-eligible subtrees run on a GPU.",
							 "off plans no GPU node, and one in a cached plan runs on the CPU; auto "
							 "chooses by cost where a GPU will be; force uses one wherever a subtree is "
							 "eligible.  It acts only with vexec.mode at auto or force.",
							 &vexec_gpu,
							 VEXEC_GPU_OFF,
							 gpu_options,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("vexec.gpu_setup_cost",
							 "The cost of starting a subtree on a GPU.",
							 NULL,
							 &vexec_gpu_setup_cost,
							 100.0, 0.0, DBL_MAX,
							 PGC_USERSET, GUC_EXPLAIN,
							 NULL, NULL, NULL);
	DefineCustomRealVariable("vexec.gpu_transfer_cost",
							 "The cost of moving one byte between a backend and a GPU.",
							 NULL,
							 &vexec_gpu_transfer_cost,
							 0.00001, 0.0, DBL_MAX,
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
	DefineCustomStringVariable("vexec.gpu_devices",
							   "The GPUs this server's GPU service uses.",
							   "Empty: no service, no arena, and CUDA is never loaded.",
							   &vexec_gpu_devices,
							   "",
							   PGC_POSTMASTER, 0,
							   NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.gpu_arena_mb",
							"The size of this server's GPU arena.",
							NULL,
							&vexec_gpu_arena_mb,
							256, 1, INT_MAX / 2,
							PGC_POSTMASTER, GUC_UNIT_MB,
							NULL, NULL, NULL);
	DefineCustomIntVariable("vexec.gpu_memory_limit_mb",
							"This server's share of each GPU's memory; 0 is all of it.",
							NULL,
							&vexec_gpu_memory_limit_mb,
							0, 0, INT_MAX / 2,
							PGC_POSTMASTER, GUC_UNIT_MB,
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
