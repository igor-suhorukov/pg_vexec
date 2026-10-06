/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * window.c
 *	  ORCA's hashed window, as VecWindowHashAgg under its WindowAgg
 *	  (pg_vector_executor.md §3.3.4, §3.8, V5).
 *
 * ORCA offers its hashed window under create_vectorization_plan, which
 * vexec asks gp_orca for (orca.c, set_options) where it builds windows.
 * gp_orca's translator lowers each one it plans to a WindowAgg over a Sort
 * of the window's input by its partition columns and then its order
 * (TranslateDXLWindowHashAgg(), MakeWindowInputSort()), only where ORCA
 * takes the input to have no order, and offers the pair whole once the
 * input's nodes have been offered (gp_orca_vec.h, build_window).  Here
 * VecWindowHashAgg takes the Sort's place (exec/vecwindow.c): it hashes the
 * input's rows into the window's partitions and sorts each by the window's
 * order alone, and the WindowAgg above reads them as it read the Sort's --
 * each partition's rows together, in the window's order -- and computes
 * the window functions, PostgreSQL's own, as it computes them over any
 * input.  The partitions come in the table's order, not the partition
 * columns', which ORCA, taking the hashed window's input to have no order,
 * asks no node above it for.
 *
 * The Sort's keys are the partition's columns, each by the ordering
 * operator of the WindowAgg's equality operator for it, then the window's
 * order, each kept once (the translator's AddSortKey()): a key of the
 * window's order that is a partition key with the same operator is dropped,
 * and within a partition its rows tie on it anyway.  So the window's order
 * within a partition is the Sort's keys past the partition's.
 *
 * Declined, keeping the lowering: a window with no partition, whose Sort is
 * a sort of its whole input by the window's order; a partition key whose
 * equality operator has no hash function, which ORCA's metadata would have
 * kept from hashing anyway; a Sort that is not the lowering's shape; and in
 * auto mode an input of fewer rows than vexec.min_rows.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#include "vexec.h"
#include "exec/exec.h"
#include "plan/plan.h"

/* An expression over the Sort's child, as OUTER_VAR, over the scan tuple. */
static Node *
outer_to_index_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == OUTER_VAR)
	{
		Var		   *v = copyObject((Var *) node);

		v->varno = INDEX_VAR;
		return (Node *) v;
	}
	return expression_tree_mutator(node, outer_to_index_mutator, context);
}

/*
 * Why the lowered window cannot be VecWindowHashAgg, or NULL; the window's
 * order within a partition, the Sort's keys past the partition's, in *plan.
 */
static const char *
window_refusal(WindowAgg *window, Sort *sort, VexecWindowPlan *plan)
{
	Plan	   *input = sort->plan.lefttree;
	int			npart = 0;
	ListCell   *lc;
	int			i;

	if (window->partNumCols <= 0)
		return "no partition: its lowering is a sort of its whole input";
	if (input == NULL || sort->plan.qual != NIL || sort->numCols <= 0 ||
		list_length(sort->plan.targetlist) != list_length(input->targetlist))
		return "a Sort not of the lowering's shape";
	foreach(lc, sort->plan.targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (!IsA(tle->expr, Var) || ((Var *) tle->expr)->varno != OUTER_VAR ||
			((Var *) tle->expr)->varattno != tle->resno)
			return "a Sort that does not pass its input's columns through";
	}

	/* the partition's keys, and the Sort's first entries, made of them */
	for (i = 0; i < window->partNumCols; i++)
	{
		Oid			eqop = window->partOperators[i];
		Oid			sortop = get_ordering_op_for_equality_op(eqop, false);
		Oid			lhash;
		Oid			rhash;
		bool		seen = false;
		int			j;

		if (!get_op_hash_functions(eqop, &lhash, &rhash) || !OidIsValid(lhash))
			return psprintf("a partition key of %s, whose equality has no hash function",
							format_type_be(exprType((Node *) list_nth_node(TargetEntry,
																		   input->targetlist,
																		   window->partColIdx[i] - 1)->expr)));
		plan->partcols = lappend_int(plan->partcols, window->partColIdx[i]);
		plan->parteqops = lappend_oid(plan->parteqops, eqop);
		plan->partcollations = lappend_oid(plan->partcollations, window->partCollations[i]);

		for (j = 0; j < npart; j++)
			if (sort->sortColIdx[j] == window->partColIdx[i] &&
				sort->sortOperators[j] == sortop &&
				sort->collations[j] == window->partCollations[i] && !sort->nullsFirst[j])
				seen = true;
		if (seen)
			continue;
		if (npart >= sort->numCols || sort->sortColIdx[npart] != window->partColIdx[i] ||
			sort->sortOperators[npart] != sortop ||
			sort->collations[npart] != window->partCollations[i] || sort->nullsFirst[npart])
			return "a Sort whose first keys are not the partition's";
		npart++;
	}

	/* the window's order within a partition: the Sort's other keys */
	for (i = 0; i < sort->numCols; i++)
	{
		plan->sortcols = lappend_int(plan->sortcols, sort->sortColIdx[i]);
		plan->sortops = lappend_oid(plan->sortops, sort->sortOperators[i]);
		plan->sortcollations = lappend_oid(plan->sortcollations, sort->collations[i]);
		plan->sortnullsfirst = lappend_int(plan->sortnullsfirst, sort->nullsFirst[i] ? 1 : 0);
		if (i < npart)
			continue;
		plan->ordcols = lappend_int(plan->ordcols, sort->sortColIdx[i]);
		plan->ordops = lappend_oid(plan->ordops, sort->sortOperators[i]);
		plan->ordcollations = lappend_oid(plan->ordcollations, sort->collations[i]);
		plan->ordnullsfirst = lappend_int(plan->ordnullsfirst, sort->nullsFirst[i] ? 1 : 0);
	}
	return NULL;
}

/*
 * The lowered hashed window, VecWindowHashAgg in its Sort's place under its
 * WindowAgg; NULL to keep the lowering.  The Sort's plan node id, costs and
 * parameters are kept, as the translator gave the Sort its input's.
 */
Plan *
vexec_build_window(VexecPlanState *ps, WindowAgg *window, List *rtable)
{
	Sort	   *sort = (Sort *) window->plan.lefttree;
	Plan	   *input;
	VexecAlt   *alt = NULL;
	VexecWindowPlan plan;
	VexecCost	cost;
	CustomScan *cscan;
	List	   *scan_tlist = NIL;
	const char *refusal;
	ListCell   *lc;

	(void) rtable;
	if (sort == NULL || !IsA(sort, Sort) || sort->plan.lefttree == NULL)
		return NULL;
	input = sort->plan.lefttree;
	if (ps->record)
		alt = vexec_alt_record(ps, "VecWindowHashAgg", "ORCA's hashed window", NULL, NULL);
	if (!vexec_enable_window)
	{
		vexec_alt_refuse(ps, alt, "vexec.enable_window is off");
		return NULL;
	}
	memset(&plan, 0, sizeof(plan));
	if ((refusal = window_refusal(window, sort, &plan)) != NULL)
	{
		vexec_alt_refuse(ps, alt, refusal);
		return NULL;
	}
	if (ps->mode != VEXEC_MODE_FORCE && input->plan_rows < vexec_min_rows)
	{
		vexec_alt_refuse(ps, alt, psprintf("%.0f input rows, fewer than vexec.min_rows",
										   input->plan_rows));
		return NULL;
	}

	memset(&cost, 0, sizeof(cost));
	cost.rows = sort->plan.plan_rows;
	vexec_alt_costed(ps, alt, &cost,
					 psprintf("ORCA's hashed window; %d partition keys, %d keys of its order within a partition, over %s",
							  list_length(plan.partcols), list_length(plan.ordcols),
							  vexec_is_vector_node(input) ? "a vector node" : "rows"));
	ps->npossible++;
	if (ps->mode != VEXEC_MODE_AUTO && ps->mode != VEXEC_MODE_FORCE)
		return NULL;

	/* the scan tuple: the input's row */
	foreach(lc, input->targetlist)
	{
		Var		   *v = makeVarFromTargetEntry(OUTER_VAR, lfirst_node(TargetEntry, lc));

		v->varnosyn = 0;
		v->varattnosyn = 0;
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) v, list_length(scan_tlist) + 1,
														 NULL, false));
	}

	cscan = makeNode(CustomScan);
	cscan->scan.plan = sort->plan;
	cscan->scan.plan.type = T_CustomScan;
	cscan->scan.plan.targetlist = (List *)
		outer_to_index_mutator((Node *) sort->plan.targetlist, NULL);
	cscan->scan.plan.qual = NIL;
	cscan->scan.plan.lefttree = input;
	cscan->scan.plan.righttree = NULL;
	cscan->scan.scanrelid = 0;
	cscan->flags = 0;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = vexec_window_plan_encode(&plan);
	cscan->methods = vexec_window_methods();

	window->plan.lefttree = &cscan->scan.plan;
	return &window->plan;
}
