/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * module.c
 *	  vexec_parquet's library: its settings, its hooks, the engine's errors,
 *	  and vexec's source and sink registered while the postmaster preloads
 *	  it.
 *
 * The library must be in shared_preload_libraries: the ProcessUtility hook
 * that takes a table's options out of CREATE TABLE before heap's parser
 * refuses them (options.c), the end of a statement that writes the buffered
 * rows (write.c), and vexec's registries, which take a source only while
 * the postmaster loads libraries, all need it there.  Arrow itself is not
 * started in the postmaster: the engine starts in each process as it is
 * first called, so that no Arrow or AWS thread is ever forked.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>
#include <stdlib.h>

#include "access/xact.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

#include "vexec_parquet.h"

PG_MODULE_MAGIC_EXT(.name = "vexec_parquet",.version = "1.0");

char	   *vpq_storage_root = NULL;
char	   *vpq_default_codec = NULL;
int			vpq_default_row_group_rows = 131072;
int			vpq_buffer_rows = 1048576;
int			vpq_buffer_mb = 256;
bool		vpq_sync = true;
bool		vpq_enable_pruning = true;
bool		vpq_enable_device_units = true;
bool		vpq_enable_point_reads = true;
bool		vpq_enable_fetch_columns = true;
int			vpq_fetch_threads = 16;
int			vpq_read_memory_mb = 256;
bool		vpq_enable_row_ranges = true;
bool		vpq_enable_step_pages = true;
bool		vpq_enable_text_marks = true;
bool		vpq_enable_projected_footers = true;
char	   *vpq_s3_endpoint = NULL;
char	   *vpq_s3_region = NULL;
char	   *vpq_s3_access_key_id = NULL;
char	   *vpq_s3_secret_access_key = NULL;
char	   *vpq_s3_session_token = NULL;
char	   *vpq_s3_scheme = NULL;
bool		vpq_s3_virtual_addressing = false;
bool		vpq_s3_anonymous = false;
bool		vpq_s3_prefetch = true;
int			vpq_s3_io_threads = 8;
static double vpq_s3_connect_timeout = 0;
static double vpq_s3_request_timeout = 0;
bool		vpq_preloaded = false;

static bool engine_started = false;

static bool
check_codec(char **newval, void **extra, GucSource source)
{
	static const char *const codecs[] = {"zstd", "lz4", "snappy", "gzip", "brotli", "none", NULL};

	for (int i = 0; codecs[i]; i++)
		if (strcmp(*newval, codecs[i]) == 0)
			return true;
	GUC_check_errdetail("The codecs are zstd, lz4, snappy, gzip, brotli and none.");
	return false;
}

static void
engine_on_exit(int code, Datum arg)
{
	if (engine_started)
		vpq_shutdown();
	engine_started = false;
}

/*
 * OT4 (pg_vector_executor.md §3.41): the backend's waits on storage, as the
 * engine marks them, under events of their own -- which pg_stat_activity and
 * pg_otel_probe's wait profile name -- registered as the engine first starts
 * in the process, before it reads anything.  The engine calls this on the
 * backend's thread alone; it neither allocates nor raises.
 */
static uint32 wait_read = 0;
static uint32 wait_fetch = 0;

static void
engine_wait(int what)
{
	if (what == VPQ_WAIT_READ && wait_read != 0)
		pgstat_report_wait_start(wait_read);
	else if (what == VPQ_WAIT_FETCH && wait_fetch != 0)
		pgstat_report_wait_start(wait_fetch);
	else if (what == VPQ_WAIT_NONE)
		pgstat_report_wait_end();
}

void
vpq_engine_ready(void)
{
	vpq_s3_config c;

	c.endpoint = vpq_s3_endpoint;
	c.region = vpq_s3_region;
	c.access_key = vpq_s3_access_key_id;
	c.secret_key = vpq_s3_secret_access_key;
	c.session_token = vpq_s3_session_token;
	c.scheme = vpq_s3_scheme;
	c.virtual_addressing = vpq_s3_virtual_addressing;
	c.anonymous = vpq_s3_anonymous;
	c.connect_timeout = vpq_s3_connect_timeout;
	c.request_timeout = vpq_s3_request_timeout;
	c.prefetch = vpq_s3_prefetch;
	c.io_threads = vpq_s3_io_threads;
	if (!engine_started)
	{
		/* the S3 client and Arrow's pools go before the process does */
		on_proc_exit(engine_on_exit, (Datum) 0);
		wait_read = WaitEventExtensionNew("VexecParquetRead");
		wait_fetch = WaitEventExtensionNew("VexecParquetFetch");
		vpq_set_wait_hook(engine_wait);
		engine_started = true;
	}
	VPQ_CHECK(vpq_configure(&c), "configuring Arrow");
}

void
vpq_engine_error(int rc, const char *what)
{
	ereport(ERROR,
			(errcode(rc == VPQ_NOT_FOUND ? ERRCODE_UNDEFINED_FILE : ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
			 errmsg("vexec_parquet: %s: %s", what,
					rc == VPQ_NOT_FOUND ? "not found" : vpq_error())));
}

/* PQ8: the bytes of Arrow's freed large blocks the process keeps for its next reads */
static void
assign_read_memory(int newval, void *extra)
{
	vpq_pool_set_keep((int64_t) newval * 1024 * 1024);
}

static void
xact_callback(XactEvent event, void *arg)
{
	/*
	 * Before commit: the buffered rows become files, the lock rows go, the
	 * counters of storages made by the transaction are written -- catalog
	 * writes, under a snapshot, which commit has none of
	 */
	if (event == XACT_EVENT_PARALLEL_PRE_COMMIT)
		return;					/* a parallel worker writes no row of ours */
	if (event == XACT_EVENT_PRE_COMMIT || event == XACT_EVENT_PRE_PREPARE)
	{
		bool		pushed;

		if (!vpq_write_pending() && !vpq_tombstones_pending() && !vpq_counters_pending())
			return;
		pushed = !ActiveSnapshotSet();

		if (pushed)
			PushActiveSnapshot(GetLatestSnapshot());
		vpq_buffer_flush_all();
		vpq_tombstones_xact_callback(event);
		vpq_counters_xact_callback(event);
		if (pushed)
			PopActiveSnapshot();
		return;
	}
	vpq_tombstones_xact_callback(event);
	vpq_counters_xact_callback(event);
	if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT ||
		event == XACT_EVENT_PARALLEL_COMMIT || event == XACT_EVENT_PARALLEL_ABORT ||
		event == XACT_EVENT_PREPARE)
	{
		vpq_fetch_reset();
		vpq_listings_xact_end();
		vpq_tables_xact_end();
		/* PQ8: the blocks kept for the transaction's reads given back */
		vpq_pool_release();
	}
}

void
_PG_init(void)
{
	vpq_preloaded = process_shared_preload_libraries_in_progress;

	/* Arrow's allocations from the system's allocator, where the user names none */
	setenv("ARROW_DEFAULT_MEMORY_POOL", "system", 0);

	DefineCustomStringVariable("vexec_parquet.storage",
							   "The root under which tables of their own files without a storage option keep them.",
							   "A local directory or an s3:// URI; empty for each database's own directory.",
							   &vpq_storage_root, "", PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("vexec_parquet.codec",
							   "The codec of the files a table without a codec option writes.",
							   NULL, &vpq_default_codec, "zstd", PGC_USERSET, 0, check_codec, NULL, NULL);
	DefineCustomIntVariable("vexec_parquet.row_group_rows",
							"The rows of a row group of a file a table without a row_group_rows option writes.",
							NULL, &vpq_default_row_group_rows, 131072, 1024, 64 * 1024 * 1024,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec_parquet.buffer_rows",
							"The rows of a table buffered before they are written as a file.",
							NULL, &vpq_buffer_rows, 1048576, 1024, 64 * 1024 * 1024,
							PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec_parquet.buffer_mb",
							"The memory a table's buffered rows take before they are written as a file.",
							NULL, &vpq_buffer_mb, 256, 1, 16 * 1024, PGC_USERSET, GUC_UNIT_MB,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.sync",
							 "Whether a local file is synced to disk before its row commits.",
							 NULL, &vpq_sync, true, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.enable_pruning",
							 "Whether a scan passes over row groups whose statistics no row of which meets its quals.",
							 NULL, &vpq_enable_pruning, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.enable_device_units",
							 "Whether vexec's GPU tier may read a scan's local files itself.",
							 NULL, &vpq_enable_device_units, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.enable_point_reads",
							 "Whether a fetch by TID reads each column's data page holding its row alone.",
							 NULL, &vpq_enable_point_reads, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.enable_fetch_columns",
							 "Whether a fetch by TID reads only the columns vexec names for its slot.",
							 NULL, &vpq_enable_fetch_columns, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec_parquet.fetch_threads",
							"The threads a fetch by TID reads its columns' data pages on, at once.",
							"1: one page at a time, on the backend's own.",
							&vpq_fetch_threads, 16, 1, 256, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec_parquet.read_memory_mb",
							"The memory of Arrow's freed large buffers a process keeps for its next reads.",
							"Given back as the transaction ends; 0 keeps none. A block of 256 kB or more is grown and shrunk in place either way.",
							&vpq_read_memory_mb, 256, 0, INT_MAX / 2, PGC_USERSET, GUC_UNIT_MB,
							NULL, assign_read_memory, NULL);
	DefineCustomBoolVariable("vexec_parquet.enable_projected_footers",
							 "Whether a scan reads an external file's row groups through a footer of its columns alone.",
							 "Made from the file's footer's index in the shared footer cache; else the footer whole.",
							 &vpq_enable_projected_footers, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.enable_step_pages",
							 "Whether a scan's later steps decode only the data pages their batches need.",
							 NULL, &vpq_enable_step_pages, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.enable_text_marks",
							 "Whether another writer's text a scan found UTF-8 whole is marked so, and not checked again.",
							 "A column chunk's, every value checked, in the file's facts in the shared footer cache.",
							 &vpq_enable_text_marks, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.enable_row_ranges",
							 "Whether a parallel scan that keeps few row groups reads them as ranges of rows.",
							 "Only where no device may read the scan's units.",
							 &vpq_enable_row_ranges, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("vexec_parquet.s3_endpoint",
							   "S3's endpoint, host[:port], or with its http:// or https:// scheme.",
							   "Empty: AWS's, or AWS_ENDPOINT_URL's.",
							   &vpq_s3_endpoint, "", PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("vexec_parquet.s3_region", "S3's region.", NULL,
							   &vpq_s3_region, "", PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomStringVariable("vexec_parquet.s3_access_key_id",
							   "S3's access key; empty for the AWS SDK's chain of credentials.", NULL,
							   &vpq_s3_access_key_id, "", PGC_SUSET, GUC_SUPERUSER_ONLY, NULL, NULL, NULL);
	DefineCustomStringVariable("vexec_parquet.s3_secret_access_key", "S3's secret key.", NULL,
							   &vpq_s3_secret_access_key, "", PGC_SUSET,
							   GUC_SUPERUSER_ONLY | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomStringVariable("vexec_parquet.s3_session_token", "S3's session token.", NULL,
							   &vpq_s3_session_token, "", PGC_SUSET,
							   GUC_SUPERUSER_ONLY | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomStringVariable("vexec_parquet.s3_scheme", "http or https, where the endpoint names none.",
							   NULL, &vpq_s3_scheme, "https", PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.s3_virtual_addressing",
							 "Whether S3's buckets are named in the host, not in the path.",
							 NULL, &vpq_s3_virtual_addressing, false, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.s3_anonymous", "Whether S3 is read without credentials.",
							 NULL, &vpq_s3_anonymous, false, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.s3_prefetch",
							 "Whether a row group's column chunks on S3 are read at once, as coalesced concurrent ranges.",
							 NULL, &vpq_s3_prefetch, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec_parquet.s3_io_threads",
							"Arrow's I/O threads in a process: S3's concurrent requests.",
							NULL, &vpq_s3_io_threads, 8, 1, 256, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("vexec_parquet.s3_connect_timeout",
							 "Seconds S3's connections may take; 0 the SDK's.", NULL,
							 &vpq_s3_connect_timeout, 0, 0, 3600, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("vexec_parquet.s3_request_timeout",
							 "Seconds S3's requests may take; 0 the SDK's.", NULL,
							 &vpq_s3_request_timeout, 0, 0, 3600, PGC_SUSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("vexec_parquet.footer_cache_size",
							"Shared memory keeping what scans ask of external files' footers, so that a process reads no footer another has read.",
							"0 keeps none; under 1 MB, none either.",
							&vpq_footer_cache_size, 64 * 1024, 0, INT_MAX / 2, PGC_POSTMASTER, GUC_UNIT_KB,
							NULL, NULL, NULL);
	DefineCustomBoolVariable("vexec_parquet.enable_footer_cache",
							 "Whether a scan reads and fills the shared footer cache.",
							 NULL, &vpq_enable_footer_cache, true, PGC_USERSET, 0, NULL, NULL, NULL);
	MarkGUCPrefixReserved("vexec_parquet");

	vpq_table_init();
	vpq_catalog_init();
	vpq_options_init();
	vpq_write_init();
	RegisterXactCallback(xact_callback, NULL);

	if (vpq_preloaded)
	{
		vpq_footers_install();
		vpq_register_vexec();
		/* OT5: pg_otel_probe's spans of its tables' scans, where it is loaded */
		vpq_register_otel();
	}
}
