/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * registry.c
 *	  vexec's side of the batch-source registry (pg_vector_executor.md
 *	  §3.5.1; the contract is vexec_source.h).
 *
 * The registry lives behind a rendezvous variable, and storage modules
 * register into it from their own _PG_init in any order with vexec.  vexec
 * makes it when it loads, reads it when it plans a scan, and shows it in
 * vexec.sources().  Heap and every access method without a source are read
 * through the slot path (§3.5.2, §3.5.5), from V1.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_am.h"
#include "funcapi.h"
#include "utils/builtins.h"
#include "utils/tuplestore.h"

#include "vexec_source.h"

#include "vexec.h"
#include "source/source.h"

PG_FUNCTION_INFO_V1(vexec_sources);

void
vexec_source_registry_install(void)
{
	(void) vexec_source_registry();
}

/*
 * How a relation's scan would get its batches: through the source its
 * access method registered, or through the slot path, which every access
 * method that can be scanned at all has.  *how names which.
 */
const VexecSourceRoutine *
vexec_source_for(Relation rel, const char **how)
{
	const VexecSourceRoutine *src;

	if (rel->rd_tableam == NULL)
	{
		*how = "no table access method";
		return NULL;
	}
	src = vexec_find_source(rel->rd_tableam);
	if (src != NULL)
	{
		*how = src->name ? src->name : "a registered source";
		return src;
	}
	*how = "the slot path";
	return NULL;
}

/* The name of the table access method of this database a source serves. */
static char *
source_am_name(const VexecSourceRoutine *src)
{
	Relation	pg_am;
	SysScanDesc scan;
	HeapTuple	tup;
	char	   *amname = NULL;

	pg_am = table_open(AccessMethodRelationId, AccessShareLock);
	scan = systable_beginscan(pg_am, InvalidOid, false, NULL, 0, NULL);
	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_am	am = (Form_pg_am) GETSTRUCT(tup);

		if (am->amtype == AMTYPE_TABLE &&
			GetTableAmRoutine(am->amhandler) == src->am)
		{
			amname = pstrdup(NameStr(am->amname));
			break;
		}
	}
	systable_endscan(scan);
	table_close(pg_am, AccessShareLock);
	return amname;
}

/*
 * vexec.sources(): the sources registered in this server, each with the
 * table access method of this database it serves, if any.
 */
Datum
vexec_sources(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	VexecSourceRegistry **rv;
	VexecSourceRegistry *reg;
	int			i;

	InitMaterializedSRF(fcinfo, 0);

	rv = (VexecSourceRegistry **) find_rendezvous_variable(VEXEC_SOURCE_RENDEZVOUS);
	reg = *rv;
	if (reg == NULL || reg->magic != VEXEC_SOURCE_MAGIC)
		PG_RETURN_VOID();

	for (i = 0; i < reg->nsources; i++)
	{
		const VexecSourceRoutine *src = reg->sources[i];
		Datum		values[5];
		bool		nulls[5] = {false, false, false, false, false};
		char	   *amname = source_am_name(src);

		values[0] = CStringGetTextDatum(src->name ? src->name : "");
		if (amname)
			values[1] = CStringGetTextDatum(amname);
		else
			nulls[1] = true;
		values[2] = Int32GetDatum(src->minor);
		values[3] = BoolGetDatum(VEXEC_SOURCE_HAS(src, estimate));
		values[4] = BoolGetDatum(VEXEC_SOURCE_HAS(src, aggregate));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}

	PG_RETURN_VOID();
}
