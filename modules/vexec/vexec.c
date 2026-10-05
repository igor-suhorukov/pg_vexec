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

#include "vexec.h"
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

/* per node kind */
bool		vexec_enable_scan = true;
bool		vexec_enable_agg = true;
bool		vexec_enable_hashjoin = true;
bool		vexec_enable_sort = true;
bool		vexec_enable_window = true;
bool		vexec_enable_insert = true;
bool		vexec_orca = true;
double		vexec_compact_threshold = 0.25;

/* the batch formats (§3.4.4) */
int			vexec_batch_format = VEXEC_FORMAT_POSTGRES;
int			vexec_batch_varlena_layout = VEXEC_VARLENA_FORMAT;
int			vexec_batch_bool_layout = VEXEC_BOOL_FORMAT;
int			vexec_batch_temporal_layout = VEXEC_TEMPORAL_FORMAT;
int			vexec_batch_numeric_layout = VEXEC_NUMERIC_FORMAT;
bool		vexec_batch_dictionary = false;

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
	 * through, and registers the batch sources' registry for storage modules
	 * to find while the postmaster loads them (§3.5.1): neither can be done
	 * once the postmaster is running.  It does not need gp_core, so it never
	 * asks for it (§3.2).
	 */
	CB_REQUIRE_PRELOAD("vexec");

	define_settings();
	MarkGUCPrefixReserved("vexec");

	vexec_source_registry_install();
	vexec_exec_install();
	vexec_planner_install();
	vexec_orca_install();
	vexec_explain_install();

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

	DefineCustomBoolVariable("vexec.enable_scan",
							 "Enables vector scans.",
							 NULL, &vexec_enable_scan, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_agg",
							 "Enables vector aggregation.",
							 NULL, &vexec_enable_agg, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_hashjoin",
							 "Enables vector hash joins.",
							 NULL, &vexec_enable_hashjoin, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_sort",
							 "Enables vector sorts.",
							 NULL, &vexec_enable_sort, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_window",
							 "Enables vector hashed window aggregation, which only ORCA plans.",
							 NULL, &vexec_enable_window, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.enable_insert",
							 "Enables vector inserts.",
							 NULL, &vexec_enable_insert, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec.orca",
							 "Uses ORCA's front end where gp_orca's API is present.",
							 NULL, &vexec_orca, true,
							 PGC_USERSET, GUC_EXPLAIN, NULL, NULL, NULL);
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
							 NULL, &vexec_batch_dictionary, false,
							 PGC_USERSET, 0, NULL, NULL, NULL);

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
