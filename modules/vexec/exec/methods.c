/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * methods.c
 *	  vexec's CustomScan methods, registered by name (pg_vector_executor.md
 *	  §3.6).
 *
 * One CustomScanMethods table per node kind, registered in _PG_init, so
 * that a plan read back from its text -- by a parallel worker, or by a
 * segment that receives a fragment -- finds its nodes' methods by name
 * (PG19:src/backend/nodes/extensible.c:108-116).  A server without vexec
 * preloaded raises "CustomScanMethods "VecScan" was not registered" there,
 * cleanly (§1.2).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/extensible.h"
#include "nodes/plannodes.h"

#include "vexec.h"
#include "exec/exec.h"

static const CustomScanMethods scan_methods = {
	.CustomName = VEXEC_SCAN_NAME,
	.CreateCustomScanState = vexec_create_scan_state,
};

static const CustomScanMethods result_methods = {
	.CustomName = VEXEC_RESULT_NAME,
	.CreateCustomScanState = vexec_create_result_state,
};

static const CustomScanMethods agg_methods = {
	.CustomName = VEXEC_AGG_NAME,
	.CreateCustomScanState = vexec_create_agg_state,
};

void
vexec_exec_install(void)
{
	RegisterCustomScanMethods(&scan_methods);
	RegisterCustomScanMethods(&result_methods);
	RegisterCustomScanMethods(&agg_methods);
}

const CustomScanMethods *
vexec_scan_methods(void)
{
	return &scan_methods;
}

const CustomScanMethods *
vexec_result_methods(void)
{
	return &result_methods;
}

const CustomScanMethods *
vexec_agg_methods(void)
{
	return &agg_methods;
}

/* Whether a plan node is one of vexec's vector nodes. */
bool
vexec_is_vector_node(Plan *plan)
{
	CustomScan *cscan;

	if (plan == NULL || !IsA(plan, CustomScan))
		return false;
	cscan = (CustomScan *) plan;
	return cscan->methods == &scan_methods || cscan->methods == &result_methods ||
		cscan->methods == &agg_methods;
}

/* Whether a plan state is one of vexec's vector nodes'. */
bool
vexec_is_vector_state(PlanState *ps)
{
	return ps != NULL && IsA(ps, CustomScanState) && vexec_is_vector_node(ps->plan);
}
