/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * kernels.c
 *	  The binding table: a built-in function's OID to its kernel
 *	  (pg_vector_executor.md §3.7, kernel.h).
 *
 * Each family of kernels adds the functions it serves, by the constants of
 * utils/fmgroids.h, the server's own names for pg_catalog's OIDs, which are
 * fixed below FirstGenbkiObjectId: a kernel is never found by name or
 * through search_path.  The table is made once a backend, at first use.
 *
 * A function is left to fmgr, whatever its kernel, where PostgreSQL's
 * evaluator would do something per call that a kernel does not: a
 * security-definer function, one with proconfig, and one a plugin claims
 * through needs_fmgr_hook (PG19:src/backend/utils/fmgr/fmgr.c:193-209).  An
 * administrator can ALTER a built-in function so.  The EXECUTE permission
 * check and InvokeFunctionExecuteHook are made for every function of a
 * vector node's expressions when the node begins, by ExecInitCustomScan's
 * own compilation of its qual and target list (ExecInitFunc,
 * PG19:src/backend/executor/execExpr.c:2708-2711), before any kernel runs.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/regproc.h"
#include "utils/tuplestore.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "vexec.h"
#include "expr/kernel.h"

PG_FUNCTION_INFO_V1(vexec_kernel_list);

static HTAB *kernel_table = NULL;
static int	nkernels = 0;

static void
add_kernel(Oid funcid, const VexecKernelDef *def, const void *info)
{
	VexecKernelEntry *e;
	bool		found;

	e = hash_search(kernel_table, &funcid, HASH_ENTER, &found);
	if (found)
		elog(ERROR, "vexec: two kernels for function %u", funcid);
	e->def = def;
	e->info = info;
	nkernels++;
}

static void
build_table(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(VexecKernelEntry);
	ctl.hcxt = TopMemoryContext;
	kernel_table = hash_create("vexec kernels", 512, &ctl,
							   HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	vexec_kernels_compare(add_kernel);
	vexec_kernels_arith(add_kernel);
	vexec_kernels_text(add_kernel);
	vexec_kernels_numeric(add_kernel);
	vexec_kernels_datetime(add_kernel);
}

/*
 * Whether a function may run in a kernel rather than through fmgr: it is
 * not security definer, has no proconfig, and no plugin claims it.
 */
bool
vexec_kernel_permitted(Oid funcid)
{
	HeapTuple	tup;
	Form_pg_proc proc;
	bool		ok;

	if (FmgrHookIsNeeded(funcid))
		return false;
	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tup))
		return false;
	proc = (Form_pg_proc) GETSTRUCT(tup);
	ok = !proc->prosecdef && heap_attisnull(tup, Anum_pg_proc_proconfig, NULL);
	ReleaseSysCache(tup);
	return ok;
}

/*
 * The kernel of a function for these argument types and this collation,
 * or NULL.  The family's bind() then checks the call itself.
 */
const VexecKernelEntry *
vexec_kernel_find(Oid funcid, int nargs, const Oid *argtypes, Oid collation)
{
	VexecKernelEntry *e;

	(void) nargs;
	(void) argtypes;
	(void) collation;
	if (kernel_table == NULL)
		build_table();
	e = hash_search(kernel_table, &funcid, HASH_FIND, NULL);
	if (e == NULL)
		return NULL;
	if (!vexec_kernel_permitted(funcid))
		return NULL;
	return e;
}

int
vexec_kernel_count(void)
{
	if (kernel_table == NULL)
		build_table();
	return nkernels;
}

/*
 * vexec.kernels(): every function with a kernel, by its OID and its name as
 * pg_catalog knows it, with the kernel's family and whether it can fail.
 */
Datum
vexec_kernel_list(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	HASH_SEQ_STATUS seq;
	VexecKernelEntry *e;

	InitMaterializedSRF(fcinfo, 0);
	if (kernel_table == NULL)
		build_table();
	hash_seq_init(&seq, kernel_table);
	while ((e = hash_seq_search(&seq)) != NULL)
	{
		Datum		values[4];
		bool		nulls[4] = {false, false, false, false};

		values[0] = ObjectIdGetDatum(e->funcid);
		values[1] = CStringGetTextDatum(format_procedure(e->funcid));
		values[2] = CStringGetTextDatum(e->def->name);
		values[3] = BoolGetDatum(e->def->can_fail);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	PG_RETURN_VOID();
}
