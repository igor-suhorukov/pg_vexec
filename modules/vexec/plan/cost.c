/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * cost.c
 *	  The vector cost model (pg_vector_executor.md §3.3.2).
 *
 * It works in PostgreSQL's cost units, so that a vector path and a row path
 * compare directly in add_path, and ORCA's vector prices sit on the same
 * scale as its others.
 *
 *	- Derived from the row cost.  A vector alternative costs what its row
 *	  counterpart costs, computed by PostgreSQL's own cost functions, with
 *	  vexec.cpu_tuple_factor and vexec.cpu_operator_factor on the work the
 *	  kernels do.  Fallback steps keep the full cpu_operator_cost.  Because
 *	  the row terms are PostgreSQL's own, they align with its costs by
 *	  construction (§6.8's check).
 *	- Columnar sources read only their columns: a registered source's
 *	  estimate gives the bytes of the needed columns after pruning, in place
 *	  of every page.  Heap keeps its page term, plus the transposition of each
 *	  needed column of each row (vexec.convert_cost).
 *	- Crossing between rows and batches is charged where it happens: a
 *	  vector alternative over row inputs pays to transpose their rows, and
 *	  every vector alternative carries the cost of handing its rows to a row
 *	  parent, which a vector parent subtracts.  This is the converter
 *	  discount of Calcite's adapters
 *	  (calcite/core/src/main/java/org/apache/calcite/adapter/jdbc/JdbcToEnumerableConverter.java:88-95).
 *	- Startup: vexec.batch_setup_cost a node.
 *
 * The batch format changes the crossing terms: the PostgreSQL format costs
 * nothing at PostgreSQL's own boundaries, the Arrow format a conversion
 * there.  Until the factors are measured (before V2), both formats are
 * priced alike; the planner records the format it priced (§3.4.4).
 *
 * Bad factors move work between the row and the vector executor; they
 * never change an answer.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "utils/rel.h"
#include "utils/spccache.h"

#include "vexec.h"
#include "plan/plan.h"

/*
 * A sequential scan's vector alternative.  rowpath is PostgreSQL's
 * sequential scan of the relation (create_seqscan_path), whose terms are
 * those of cost_seqscan (PG19:src/backend/optimizer/path/costsize.c): a
 * page term, cpu_tuple_cost and the quals a tuple, the target an output
 * row.  source_bytes, when a registered source gave it, replaces the page
 * term with the needed columns' bytes.
 */
void
vexec_cost_scan(PlannerInfo *root, RelOptInfo *rel, Path *rowpath,
				const VexecSteps *quals, const VexecSteps *target,
				int ncols_in, int ncols_out, double source_bytes,
				VexecCost *cost)
{
	double		tuples = rel->tuples;
	double		rows = rowpath->rows;
	Cost		qual_total = rel->baserestrictcost.per_tuple;
	Cost		qual_kernel = Min(quals->kernel_cost, qual_total);
	Cost		target_total = rowpath->pathtarget->cost.per_tuple;
	Cost		target_kernel = Min(target->kernel_cost, target_total);
	Cost		row_cpu;
	Cost		disk;
	Cost		cpu;

	memset(cost, 0, sizeof(VexecCost));
	cost->row_startup = rowpath->startup_cost;
	cost->row_total = rowpath->total_cost;
	cost->rows = rows;

	row_cpu = (cpu_tuple_cost + qual_total) * tuples + target_total * rows;
	disk = Max(rowpath->total_cost - rowpath->startup_cost - row_cpu, 0);
	if (source_bytes >= 0)
	{
		double		spc_seq_page_cost;

		get_tablespace_page_costs(rel->reltablespace, NULL, &spc_seq_page_cost);
		disk = spc_seq_page_cost * ceil(source_bytes / BLCKSZ);
	}

	cpu = cpu_tuple_cost * vexec_cpu_tuple_factor * tuples +
		(qual_kernel * vexec_cpu_operator_factor + (qual_total - qual_kernel)) * tuples +
		(target_kernel * vexec_cpu_operator_factor + (target_total - target_kernel)) * rows;

	/* the slot path and heap: each needed column of each row, transposed */
	cost->convert_in = source_bytes >= 0 ? 0 : vexec_convert_cost * ncols_in * tuples;
	cost->rowout = vexec_convert_cost * ncols_out * rows;
	cost->startup = rowpath->startup_cost + vexec_batch_setup_cost;
	cost->total = cost->startup + disk + cpu + cost->convert_in + cost->rowout;
}

/* Columns a path hands its parent. */
static int
path_width_cols(Path *path)
{
	return Max(list_length(path->pathtarget->exprs), 1);
}

/*
 * A hash join's vector alternative.  rowpath is PostgreSQL's HashPath for
 * the same inputs (initial_cost_hashjoin, final_cost_hashjoin): a
 * cpu_operator_cost a hash clause and a cpu_tuple_cost an inner row to
 * build, a cpu_operator_cost a hash clause an outer row to probe, and a
 * cpu_tuple_cost an output row.  The kernels' share of the hashing, and the
 * tuple costs, take the vector factors; the row inputs are transposed.
 */
void
vexec_cost_hashjoin(PlannerInfo *root, Path *rowpath, Path *outer, Path *inner,
					int nclauses, int nclauses_kernel, VexecCost *cost)
{
	double		save_op = (1.0 - vexec_cpu_operator_factor) * cpu_operator_cost * nclauses_kernel;
	double		save_tuple = (1.0 - vexec_cpu_tuple_factor) * cpu_tuple_cost;
	Cost		save_startup;
	Cost		save_run;

	(void) root;
	(void) nclauses;
	memset(cost, 0, sizeof(VexecCost));
	cost->row_startup = rowpath->startup_cost;
	cost->row_total = rowpath->total_cost;
	cost->rows = rowpath->rows;

	save_startup = (save_op + save_tuple) * inner->rows;
	save_run = save_op * outer->rows + save_tuple * rowpath->rows;

	cost->convert_in = vexec_convert_cost *
		(path_width_cols(outer) * outer->rows + path_width_cols(inner) * inner->rows);
	cost->rowout = vexec_convert_cost * path_width_cols(rowpath) * rowpath->rows;
	cost->startup = rowpath->startup_cost - save_startup + vexec_batch_setup_cost +
		vexec_convert_cost * path_width_cols(inner) * inner->rows;
	cost->total = rowpath->total_cost - save_startup - save_run + vexec_batch_setup_cost +
		cost->convert_in + cost->rowout;
}

/*
 * An aggregation's vector alternative, plain or hashed.  rowpath is
 * PostgreSQL's AggPath over the same input (cost_agg): a cpu_operator_cost
 * a grouping column an input row to hash, the transition functions an
 * input row, a cpu_tuple_cost a group.  The kernels' share of the hashing
 * and of the transitions take the operator factor -- aggregates without a
 * vector transition call theirs through fmgr, row by row, at full cost --
 * and the groups the tuple factor.
 */
void
vexec_cost_agg(PlannerInfo *root, Path *rowpath, Path *input, bool input_vector,
			   int ngroupcols, int ngroupcols_kernel, int naggs_kernel,
			   double numgroups, VexecCost *cost)
{
	double		save_hash = (1.0 - vexec_cpu_operator_factor) * cpu_operator_cost *
		ngroupcols_kernel * input->rows;
	double		save_trans = (1.0 - vexec_cpu_operator_factor) * cpu_operator_cost *
		naggs_kernel * input->rows;
	double		save_groups = (1.0 - vexec_cpu_tuple_factor) * cpu_tuple_cost * numgroups;

	(void) root;
	(void) ngroupcols;
	memset(cost, 0, sizeof(VexecCost));
	cost->row_startup = rowpath->startup_cost;
	cost->row_total = rowpath->total_cost;
	cost->rows = rowpath->rows;

	/*
	 * Rows in are transposed; a vector child's batches are read as they are,
	 * and the handing out of its rows that its cost carries is saved.
	 */
	cost->convert_in = vexec_convert_cost * path_width_cols(input) * input->rows;
	if (input_vector)
		cost->convert_in = -cost->convert_in;
	cost->rowout = vexec_convert_cost * path_width_cols(rowpath) * rowpath->rows;
	cost->startup = rowpath->startup_cost - save_hash - save_trans +
		vexec_batch_setup_cost + cost->convert_in;
	cost->total = rowpath->total_cost - save_hash - save_trans - save_groups +
		vexec_batch_setup_cost + cost->convert_in + cost->rowout;
}

/*
 * A sort's vector alternative.  It sorts through PostgreSQL's tuplesort,
 * as the row Sort does; its gain is the island it keeps whole (§3.8), which
 * shows as the crossings its parent and its child no longer pay.
 */
void
vexec_cost_sort(PlannerInfo *root, Path *rowpath, Path *input, VexecCost *cost)
{
	(void) root;
	memset(cost, 0, sizeof(VexecCost));
	cost->row_startup = rowpath->startup_cost;
	cost->row_total = rowpath->total_cost;
	cost->rows = rowpath->rows;

	cost->convert_in = vexec_convert_cost * path_width_cols(input) * input->rows;
	cost->rowout = vexec_convert_cost * path_width_cols(rowpath) * rowpath->rows;
	cost->startup = rowpath->startup_cost + vexec_batch_setup_cost + cost->convert_in;
	cost->total = rowpath->total_cost + vexec_batch_setup_cost + cost->convert_in + cost->rowout;
}

/*
 * A scan ORCA's translator built, priced as a row scan and as a vector scan
 * in PostgreSQL's units: the table's pages, its tuples, and the oracle's
 * counts of the quals' and the target's steps, at cpu_operator_cost a step.
 * ORCA's own costs are in its units, and its choices in V1 are its own; in
 * auto mode its scan becomes a vector scan where this prices it lower (V5's
 * CCostModelVec prices vector operators inside ORCA's search).
 */
void
vexec_cost_plan_scan(Relation rel, const VexecSteps *quals, const VexecSteps *target,
					 int ncols_in, int ncols_out, double rows, VexecCost *cost)
{
	double		tuples = Max(rel->rd_rel->reltuples, rows);
	double		pages = Max(rel->rd_rel->relpages, 0);
	double		disk = seq_page_cost * pages;
	double		qual_ops = quals->kernel + quals->fallback;
	double		target_ops = target->kernel + target->fallback;
	Cost		row_cpu;
	Cost		cpu;

	memset(cost, 0, sizeof(VexecCost));
	row_cpu = (cpu_tuple_cost + cpu_operator_cost * qual_ops) * tuples +
		cpu_operator_cost * target_ops * rows;
	cpu = cpu_tuple_cost * vexec_cpu_tuple_factor * tuples +
		cpu_operator_cost * (quals->kernel * vexec_cpu_operator_factor + quals->fallback) * tuples +
		cpu_operator_cost * (target->kernel * vexec_cpu_operator_factor + target->fallback) * rows;
	cost->rows = rows;
	cost->row_startup = 0;
	cost->row_total = disk + row_cpu;
	cost->convert_in = vexec_convert_cost * ncols_in * tuples;
	cost->rowout = vexec_convert_cost * ncols_out * rows;
	cost->startup = vexec_batch_setup_cost;
	cost->total = cost->startup + disk + cpu + cost->convert_in + cost->rowout;
}

/*
 * An Agg ORCA's translator built, priced as a row Agg and as VecAgg in
 * PostgreSQL's units, as cost_agg() would price it (costsize.c): a
 * cpu_operator_cost a grouping column and an aggregate an input row, a
 * cpu_tuple_cost a group; the vector transitions and the keys' hashing at
 * the operator factor.  ORCA's choices are its own until V5; in auto mode
 * its Agg becomes VecAgg where this prices it lower.
 */
void
vexec_cost_plan_agg(Plan *agg, int ngroupcols, int naggs, int naggs_kernel,
					bool input_vector, VexecCost *cost)
{
	Plan	   *input = agg->lefttree;
	double		in_rows = input ? Max(input->plan_rows, 1) : 1;
	double		groups = Max(agg->plan_rows, 1);
	int			in_cols = input ? Max(list_length(input->targetlist), 1) : 1;
	Cost		row_cpu;
	Cost		cpu;

	memset(cost, 0, sizeof(VexecCost));
	row_cpu = cpu_operator_cost * (ngroupcols + naggs) * in_rows + cpu_tuple_cost * groups;
	cpu = cpu_operator_cost * (ngroupcols * vexec_cpu_operator_factor +
							   naggs_kernel * vexec_cpu_operator_factor +
							   (naggs - naggs_kernel)) * in_rows +
		cpu_tuple_cost * vexec_cpu_tuple_factor * groups;
	cost->rows = groups;
	cost->row_startup = row_cpu;
	cost->row_total = row_cpu;
	cost->convert_in = vexec_convert_cost * in_cols * in_rows * (input_vector ? -1 : 1);
	cost->rowout = vexec_convert_cost * Max(list_length(agg->targetlist), 1) * groups;
	cost->startup = vexec_batch_setup_cost + cpu + cost->convert_in;
	cost->total = cost->startup + cost->rowout;
}
