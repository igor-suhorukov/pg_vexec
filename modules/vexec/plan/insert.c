/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * insert.c
 *	  VecInsert in PostgreSQL's planner (pg_vector_executor.md §3.16): an
 *	  INSERT whose rows are written a batch at a time, through the target's
 *	  sink (vexec_sink.h) or table_multi_insert(), in place of ModifyTable's
 *	  row loop.
 *
 * create_upper_paths_hook is called for the final relation after
 * ModifyTable's paths are made (PG19:src/backend/optimizer/plan/planner.c:
 * 2401, 2453-2456).  Beside each ModifyTablePath of an INSERT, a CustomPath
 * over the same subpath competes on cost; in force mode it takes their
 * place.  None is offered where ModifyTable must stay:
 *
 *	- RETURNING, ON CONFLICT, a WITH CHECK option (row-level security's or
 *	  a view's): ModifyTable's own work, row by row;
 *	- a trigger on INSERT, of any kind -- a foreign key's among them -- and
 *	  so transition tables too;
 *	- a deferrable unique or exclusion constraint, whose rechecks are AFTER
 *	  triggers' work;
 *	- anything but a table or a partitioned table: a foreign table, a view;
 *	  and a partitioned table with any of the above in a partition, or a
 *	  foreign table among them;
 *	- on a cluster's coordinator (gp.role = dispatch), where gp_core
 *	  dispatches an INSERT's ModifyTable to the segments: there VecInsert
 *	  comes from ORCA's translation, in the segments' fragments.
 *
 * Its plan is a CustomScan with no relation (scanrelid 0), the SELECT's
 * plan in lefttree, and in custom_private the target's range table index
 * and whether it sets the command tag.  It returns no row; its target list
 * repeats its child's, as a scan tuple of the child's row, only because the
 * top of a plan must label the statement's columns (createplan.c,
 * apply_tlist_labeling()).  The target goes into the statement's result
 * relations once the plan is finished (vexec_insert_finish_plan()).
 *
 * A partitioned target's partitions are read here, each locked as
 * find_all_inheritors() locks, and the plan depends on each of them
 * (PlannerGlobal.relationOids), as ModifyTable's does not: a trigger made
 * on a partition invalidates it, and the statement is planned again.
 *
 * In ORCA's plans VecInsert takes the place of the ModifyTable its
 * translator built (vexec_build_insert_from_modifytable(), from orca.c):
 * the same refusals, its input the ModifyTable's, whose target list is the
 * target's row.  On a cluster's coordinator the ModifyTable stays, with
 * VecInsert as its input: gp_core dispatches a write as its ModifyTable --
 * the fragment's command and result relation, the rows it counts
 * (pg19/modules/gp_core/gp_motion.c, motion_dml_run(), fragment_sql_ex())
 * -- and on each segment VecInsert writes every row, handing none up, so
 * that the ModifyTable writes none (exec/vecinsert.c).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "commands/trigger.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/relcache.h"

#include "vexec_sink.h"

#include "vexec.h"
#include "exec/exec.h"
#include "plan/plan.h"

static Plan *plan_vecinsert(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
							List *tlist, List *clauses, List *custom_plans);

static const CustomPathMethods vecinsert_path_methods = {
	.CustomName = VEXEC_INSERT_NAME,
	.PlanCustomPath = plan_vecinsert,
};

/* Whether the statement is planned on a cluster's coordinator. */
bool
vexec_on_coordinator(void)
{
	const char *role = GetConfigOption("gp.role", true, false);

	return role != NULL && strcmp(role, "dispatch") == 0;
}

/* Why a relation written needs ModifyTable, or NULL. */
static const char *
relation_refusal(Relation rel)
{
	TriggerDesc *trig = rel->trigdesc;
	const char *refusal = NULL;

	if (trig != NULL &&
		(trig->trig_insert_before_row || trig->trig_insert_after_row ||
		 trig->trig_insert_instead_row || trig->trig_insert_before_statement ||
		 trig->trig_insert_after_statement || trig->trig_insert_new_table))
		return "a trigger on INSERT, a foreign key's among them";
	if (rel->rd_rel->relhasindex && rel->rd_rel->relkind == RELKIND_RELATION)
	{
		List	   *indexes = RelationGetIndexList(rel);
		ListCell   *lc;

		foreach(lc, indexes)
		{
			Relation	idx = index_open(lfirst_oid(lc), AccessShareLock);
			bool		deferrable = !idx->rd_index->indimmediate;

			index_close(idx, AccessShareLock);
			if (deferrable)
			{
				refusal = "a deferrable unique or exclusion constraint";
				break;
			}
		}
		list_free(indexes);
	}
	return refusal;
}

/* Why the target needs ModifyTable, or NULL. */
static const char *
target_refusal(Relation rel)
{
	const char *refusal;
	List	   *parts;
	ListCell   *lc;

	switch (rel->rd_rel->relkind)
	{
		case RELKIND_RELATION:
			return relation_refusal(rel);
		case RELKIND_PARTITIONED_TABLE:
			break;
		case RELKIND_FOREIGN_TABLE:
			return "a foreign table";
		default:
			return "not a table";
	}
	if ((refusal = relation_refusal(rel)) != NULL)
		return refusal;

	/* each partition, at every level, as the rows may go to any */
	parts = find_all_inheritors(RelationGetRelid(rel), AccessShareLock, NULL);
	foreach(lc, parts)
	{
		Relation	part;

		if (lfirst_oid(lc) == RelationGetRelid(rel))
			continue;
		part = table_open(lfirst_oid(lc), NoLock);
		if (part->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
			refusal = "a foreign table among its partitions";
		else if ((refusal = relation_refusal(part)) != NULL)
			refusal = psprintf("%s, on its partition %s", refusal,
							   quote_identifier(RelationGetRelationName(part)));
		table_close(part, NoLock);
		if (refusal != NULL)
			break;
	}
	list_free(parts);
	return refusal;
}

/*
 * VecInsert's cost beside ModifyTable's (§3.3.2).  PostgreSQL prices an
 * INSERT as its subpath alone (create_modifytable_path()), so the row each
 * ModifyTable writes -- a tuple's work, cpu_tuple_cost -- is in no price;
 * VecInsert does it a batch at a time, at vexec.cpu_tuple_factor, and is
 * priced below ModifyTable by what that saves.  Row inputs pay to be
 * transposed into batches, which may cost more than it saves.
 */
/*
 * Whether the INSERT's rows come as batches: a vector path, or a projection
 * of one that projects itself -- the target list's coercions to the
 * target's types, which createplan.c puts into the vector node's own target
 * list (create_projection_plan(), is_projection_capable_path()).
 */
static bool
vector_input(Path *sub)
{
	if (IsA(sub, ProjectionPath))
	{
		Path	   *below = ((ProjectionPath *) sub)->subpath;

		return vexec_is_vector_path(below) &&
			(((CustomPath *) below)->flags & CUSTOMPATH_SUPPORT_PROJECTION) != 0;
	}
	return vexec_is_vector_path(sub);
}

static void
cost_insert(ModifyTablePath *mtp, int ncols, VexecCost *cost)
{
	Path	   *sub = mtp->subpath;
	double		rows = sub->rows;
	bool		vector_in = vector_input(sub);

	memset(cost, 0, sizeof(VexecCost));
	cost->rows = rows;
	cost->row_startup = mtp->path.startup_cost;
	cost->row_total = mtp->path.total_cost;
	cost->convert_in = vector_in ? 0 : vexec_convert_cost * ncols * rows;
	cost->startup = mtp->path.startup_cost + vexec_batch_setup_cost;
	cost->total = mtp->path.total_cost - cpu_tuple_cost * (1.0 - vexec_cpu_tuple_factor) * rows +
		cost->convert_in + vexec_batch_setup_cost;
	if (cost->total < cost->startup)
		cost->total = cost->startup;
}

static Path *
insert_path(RelOptInfo *rel, ModifyTablePath *mtp, const VexecCost *cost)
{
	CustomPath *cp = makeNode(CustomPath);

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = mtp->path.pathtarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	cp->path.rows = mtp->path.rows;
	cp->path.disabled_nodes = mtp->path.disabled_nodes;
	cp->path.startup_cost = cost->startup;
	cp->path.total_cost = cost->total;
	cp->path.pathkeys = NIL;
	cp->flags = 0;
	cp->custom_paths = list_make1(mtp->subpath);
	cp->custom_private = list_make2_int(linitial_int(mtp->resultRelations),
										mtp->canSetTag ? 1 : 0);
	cp->methods = &vecinsert_path_methods;
	return &cp->path;
}

/*
 * VecInsert beside the final relation's ModifyTablePaths of an INSERT, at
 * the top query level.  In explain mode, and for EXPLAIN (VEXEC), it is
 * recorded with its cost or its refusal.
 */
void
vexec_consider_insert(PlannerInfo *root, RelOptInfo *final_rel, VexecPlanState *ps)
{
	Query	   *parse = root->parse;
	bool		add = ps->mode == VEXEC_MODE_AUTO || ps->mode == VEXEC_MODE_FORCE;
	VexecAlt   *alt = NULL;
	RangeTblEntry *rte;
	const char *refusal = NULL;
	const char *how = NULL;
	List	   *mtps = NIL;
	List	   *paths = NIL;
	ListCell   *lc;
	char	   *relname;

	if (parse->commandType != CMD_INSERT || parse->resultRelation <= 0 ||
		root->parent_root != NULL)
		return;
	foreach(lc, final_rel->pathlist)
		if (IsA(lfirst(lc), ModifyTablePath) &&
			((ModifyTablePath *) lfirst(lc))->operation == CMD_INSERT)
			mtps = lappend(mtps, lfirst(lc));
	if (mtps == NIL)
		return;

	rte = rt_fetch(parse->resultRelation, parse->rtable);
	relname = get_rel_name(rte->relid);
	if (ps->record)
		alt = vexec_alt_record(ps, "VecInsert",
							   psprintf("INSERT INTO %s", quote_identifier(relname ? relname : "?")),
							   root, NULL);
	if (alt == NULL && !add)
		return;

	if (!vexec_enable_insert)
		refusal = "vexec.enable_insert is off";
	else if (parse->returningList != NIL)
		refusal = "RETURNING";
	else if (parse->onConflict != NULL)
		refusal = "ON CONFLICT";
	else if (parse->withCheckOptions != NIL)
		refusal = "a WITH CHECK option: row-level security's, or a view's";
	else if (vexec_on_coordinator())
		refusal = "a cluster's coordinator, where ORCA's translation builds it on the segments";
	else
	{
		Relation	rel = table_open(rte->relid, NoLock);

		refusal = target_refusal(rel);
		if (refusal == NULL && rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
			how = "each partition's sink or table_multi_insert, the rows routed";
		else if (refusal == NULL)
		{
			const VexecSinkRoutine *sink = vexec_find_sink(rel->rd_tableam);

			how = sink != NULL ? psprintf("sink: %s", sink->name) : "table_multi_insert";
		}
		table_close(rel, NoLock);
	}
	if (refusal == NULL && ps->mode != VEXEC_MODE_FORCE &&
		((ModifyTablePath *) linitial(mtps))->subpath->rows < vexec_min_rows)
		refusal = psprintf("%.0f rows, fewer than vexec.min_rows",
						   ((ModifyTablePath *) linitial(mtps))->subpath->rows);
	if (refusal != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return;
	}

	foreach(lc, mtps)
	{
		ModifyTablePath *mtp = lfirst(lc);
		VexecCost	cost;

		cost_insert(mtp, list_length(root->processed_tlist), &cost);
		if (foreach_current_index(lc) == 0)
			vexec_alt_costed(ps, alt, &cost,
							 psprintf("%s; input: %s", how,
									  vector_input(mtp->subpath) ? "batches" : "rows"));
		if (add)
			paths = lappend(paths, insert_path(final_rel, mtp, &cost));
	}
	if (!add)
		return;
	if (ps->mode == VEXEC_MODE_FORCE)
		final_rel->pathlist = NIL;
	foreach(lc, paths)
		add_path(final_rel, lfirst(lc));
	ps->npossible++;
}

/*
 * VecInsert's plan: the subpath's plan in lefttree, its row as the scan
 * tuple and the target list, the target's range table index in
 * custom_private.
 */
static Plan *
plan_vecinsert(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			   List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Plan	   *child = linitial(custom_plans);
	List	   *scan_tlist = NIL;
	List	   *out_tlist = NIL;
	ListCell   *lc;

	(void) rel;
	(void) tlist;
	(void) clauses;
	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = lfirst(lc);

		scan_tlist = lappend(scan_tlist, makeTargetEntry(copyObject(tle->expr), tle->resno,
														 tle->resname, tle->resjunk));
		out_tlist = lappend(out_tlist, makeTargetEntry(copyObject(tle->expr), tle->resno,
													   tle->resname, tle->resjunk));
	}
	cscan->scan.plan.targetlist = out_tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.plan.lefttree = child;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_private = best_path->custom_private;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->methods = vexec_insert_methods();

	/* the plan depends on every partition its rows may go to */
	{
		RangeTblEntry *rte = planner_rt_fetch(linitial_int(best_path->custom_private), root);

		if (rte->relkind == RELKIND_PARTITIONED_TABLE)
		{
			List	   *parts = find_all_inheritors(rte->relid, NoLock, NULL);

			root->glob->relationOids = list_concat(root->glob->relationOids, parts);
		}
	}
	return &cscan->scan.plan;
}

/*
 * The statement's result relation, as standard_planner() records
 * ModifyTable's, from what set_plan_references() collects: VecInsert's target
 * goes into PlannedStmt.resultRelationRelids once the plan is finished.
 * VecInsert is the top of a top-level statement's plan, whose range table
 * indexes are final.
 */
void
vexec_insert_finish_plan(PlannedStmt *pstmt)
{
	CustomScan *cscan = (CustomScan *) pstmt->planTree;
	int			rti;

	if (cscan == NULL || !IsA(cscan, CustomScan) || cscan->methods != vexec_insert_methods())
		return;
	rti = linitial_int(cscan->custom_private);
	pstmt->resultRelationRelids = bms_add_member(pstmt->resultRelationRelids, rti);
}

/*
 * VecInsert in place of an INSERT's ModifyTable that ORCA's translator built
 * (orca.c), in executor form: its subplan the node's child, the child's row
 * its scan tuple, no target list, as ModifyTable has none without
 * RETURNING.  NULL, with *refusal, where ModifyTable must stay; a
 * partitioned target's partitions are added to *relation_oids.
 */
Plan *
vexec_build_insert_from_modifytable(ModifyTable *mt, List *rtable, const char **refusal,
									List **relation_oids)
{
	CustomScan *cscan;
	Plan	   *child = mt->plan.lefttree;
	RangeTblEntry *rte;
	Relation	rel;
	List	   *scan_tlist = NIL;
	ListCell   *lc;
	Index		rti;

	*refusal = NULL;
	if (mt->operation != CMD_INSERT)
		return NULL;
	if (list_length(mt->resultRelations) != 1 || child == NULL)
	{
		*refusal = "not one target";
		return NULL;
	}
	rti = linitial_int(mt->resultRelations);
	if (rti < 1 || rti > list_length(rtable))
	{
		*refusal = "not one target";
		return NULL;
	}
	if (!vexec_enable_insert)
		*refusal = "vexec.enable_insert is off";
	else if (mt->returningLists != NIL)
		*refusal = "RETURNING";
	else if (mt->onConflictAction != ONCONFLICT_NONE)
		*refusal = "ON CONFLICT";
	else if (mt->withCheckOptionLists != NIL)
		*refusal = "a WITH CHECK option: row-level security's, or a view's";
	if (*refusal != NULL)
		return NULL;
	rte = rt_fetch(rti, rtable);
	if (rte->rtekind != RTE_RELATION)
	{
		*refusal = "not a table";
		return NULL;
	}
	rel = table_open(rte->relid, NoLock);
	*refusal = target_refusal(rel);
	if (*refusal == NULL && list_length(child->targetlist) != RelationGetDescr(rel)->natts)
		*refusal = "a junk column in the row to insert";
	if (*refusal == NULL && rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		*relation_oids = list_concat(*relation_oids,
									 find_all_inheritors(rte->relid, NoLock, NULL));
	table_close(rel, NoLock);
	if (*refusal != NULL)
		return NULL;
	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = lfirst(lc);

		if (tle->resjunk)
		{
			*refusal = "a junk column in the row to insert";
			return NULL;
		}
		scan_tlist = lappend(scan_tlist, makeTargetEntry(copyObject(tle->expr), tle->resno,
														 tle->resname, false));
	}

	cscan = makeNode(CustomScan);
	cscan->scan.plan = mt->plan;
	cscan->scan.plan.type = T_CustomScan;
	cscan->scan.plan.targetlist = NIL;
	cscan->scan.plan.qual = NIL;
	cscan->scan.plan.lefttree = child;
	cscan->scan.plan.righttree = NULL;
	cscan->scan.scanrelid = 0;
	cscan->flags = 0;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_private = list_make2_int(rti, mt->canSetTag ? 1 : 0);
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_relids = NULL;
	cscan->methods = vexec_insert_methods();
	return &cscan->scan.plan;
}
