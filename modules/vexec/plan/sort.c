/* SPDX-License-Identifier: Apache-2.0 */
/*-------------------------------------------------------------------------
 *
 * sort.c
 *	  VecSort in the planners (pg_vector_executor.md §3.3.3, §3.3.4, §3.8,
 *	  V4): its path at UPPERREL_ORDERED in PostgreSQL's planner, its plan,
 *	  and ORCA's Sort, with the bound of a Limit above it.
 *
 * PostgreSQL's planner.  create_upper_paths_hook at UPPERREL_ORDERED is
 * called once core has made the ordered relation's paths
 * (create_ordered_paths()).  The hook adds a VecSort over the input
 * relation's cheapest path, with the query's sort keys, where that path is
 * not in their order already, to the query's final target, which core's
 * ordered paths carry.  Its one custom path is the row Sort
 * create_sort_path() makes of the same input, so that core plans the sort's
 * keys as it plans a Sort's -- prepare_sort_from_pathkeys() adds a key its
 * input lacks to the input's target list, which is static in createplan.c
 * -- and the builder takes the Sort's keys and its input from the plan core
 * made, and sets the Sort aside.  The VecSort projects: its target is the
 * ordered relation's, the query's final one, which it evaluates over the
 * sorted rows as a projection over a Sort would, through its target list.
 * Set-returning functions are added above it after the hook
 * (adjust_paths_for_srfs()), and a Limit above them.
 *
 * Its bound (§3.6, "Bounds").  create_ordered_paths() is given the LIMIT's
 * count and offset as the sort's limit (grouping_planner()), and passes
 * none where set-returning functions follow the sort; the hook is not
 * given it.  So the hook takes it from the query as preprocess_limit()
 * does (planner.c): a LIMIT whose count -- and offset, if any -- are
 * constants once the planner has folded them, the count and offset added;
 * none for FETCH ... WITH TIES, which reads past the bound's rows, and none
 * where the query has set-returning functions in its target list, which
 * may make any number of rows of each sorted one.  The Limit above then
 * reads at most the bound's rows of the sorted ones, which a bounded sort
 * keeps, as PostgreSQL's Sort keeps them when ExecSetTupleBound() passes it
 * the same bound at run time.
 *
 * In auto mode the VecSort competes with core's sorted paths by cost
 * (cost.c); in force mode it replaces them, where the oracle accepts it.
 *
 * Late columns (H6, §3.14).  A bounded VecSort -- of at most
 * VEXEC_LATE_ROWS rows -- straight over a VecScan sorts the keys and each
 * row's TID alone, where the child's other columns are the relation's
 * own and the narrower scan saves more than the fetches cost
 * (late_saving()): the scan then reads only the sort keys and its quals'
 * columns, and the other columns of the rows the sort keeps are fetched at
 * the end, by their TIDs, under the scan's snapshot
 * (table_tuple_fetch_row_version()).  A source that prices its fetches is
 * taken at its word (VC16): a column store's decodes a granule of each
 * column it reads, which a narrow row the scan reads anyway does not repay
 * -- three columns of ClickBench's hits sorted by EventTime, LIMIT 100,
 * took 1,344 ms serially with late columns and 671 without
 * (ch_table_engines.md §3, "VC4, VC6 and VC16, researched").  A Var raises
 * no error, so reading one only for the kept rows changes no answer; a
 * column the child computes keeps it as it is.  The rows that tie at the
 * bound's key are those tuplesort keeps, as without it.
 *
 * In parallel, the input's cheapest partial path is sorted so too, in each
 * participant, under a Gather Merge, the bound each participant's; over a
 * parallel VecScan, where the narrower scan saves more than the fetches
 * cost, each participant fetches the rows its sort keeps, by their TIDs,
 * under the query's snapshot, which every participant shares, and the path
 * is priced so -- without which the planner took a row Sort over the
 * Gather, the scan reading every column.  Where the
 * relation's cheapest paths are no VecScan -- ChScan's, cheaper than
 * VecScan's for every column of a wide table -- its VecScan paths, kept
 * aside as paths.c makes them (vexec_sort_keep_scan()), are sorted so too,
 * beside core's sorts for cost to choose: `SELECT * ... ORDER BY k LIMIT 10`
 * of a MergeTree table of 105 columns otherwise took a row Sort of every
 * column (ch_table_engines.md §3, "VC4, VC6 and VC16, researched").
 *
 * ORCA.  A Sort ORCA's translator built becomes a VecSort of the same keys
 * over the same child, its target list read over the scan tuple, the
 * child's row.  A Limit of a constant count over it -- or over a projection
 * of it, which passes rows as they come -- gives it its bound, as
 * ExecSetTupleBound() would give a Sort, when the Limit is offered to the
 * front end after its children.
 *
 * The device's order (V8d, §3.11).  The plan says whether an unbounded
 * VecSort takes the device's order (exec/vecsort.c), as VecHashJoin's says
 * whether it takes the device's probes: the strategy changes no plan's
 * shape and is not priced into the path; the node decides again as it
 * begins.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/spccache.h"

#include "vexec.h"
#include "exec/device.h"
#include "exec/exec.h"
#include "plan/plan.h"

static Plan *plan_vecsort(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
						  List *tlist, List *clauses, List *custom_plans);

/* add_path()'s fuzz, STD_FUZZ_FACTOR (PG19:src/backend/optimizer/util/pathnode.c:47) */
#define VEXEC_PATH_FUZZ		1.01

/*
 * A bounded sort keeps its columns late up to this many rows (H6): a
 * batch's rows before F24 made them a setting, kept
 */
#define VEXEC_LATE_ROWS		1024

/* W13: a bounded sort lends the device's read of its scan's units its rank up to this bound */
#define VEXEC_SCAN_RANK_MAX	(1 << 20)

/*
 * And over a relation's VecScan kept aside (vexec_sort_keep_scan()), up to
 * this many where its source does not price its fetches: there the
 * cheapest scan is a wide table's, and a fetch by TID of a column store
 * reads a block of every column of the row -- a MergeTree granule, 8.8 ms
 * a row of ClickBench's 105 columns, where late_saving() would price a
 * random page.  Measured on those 10M rows with 16 workers, a sort of
 * SELECT * bounded at 100 took 597 ms over the kept scan against 2,362 over
 * ChScan's, at 1,000 5,687 against 2,374 (ch_table_engines.md §3, "VC4,
 * VC6 and VC16, researched").  A source that prices them (minor 4's
 * VexecSourceEstimate.fetch, VC16) has its fetches priced so, up to
 * VEXEC_LATE_ROWS.
 */
#define VEXEC_KEPT_LATE_ROWS	100

/* A running bound keeps the first key of up to this many rows (H6). */
#define VEXEC_BOUND_ROWS	1000000

static const CustomPathMethods vecsort_path_methods = {
	.CustomName = VEXEC_SORT_NAME,
	.PlanCustomPath = plan_vecsort,
};

/* "1 sort key", "2 sort keys" */
static const char *
count_of(int n, const char *one, const char *many)
{
	return psprintf("%d %s", n, n == 1 ? one : many);
}

/*
 * The LIMIT's rows the sort must keep: its count and offset, where both are
 * constants once folded, and nothing after the sort makes or drops rows.
 * 0: no bound.
 */
int64
vexec_query_bound(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	Const	   *count = (Const *) parse->limitCount;
	int64		offset = 0;
	int64		n;

	if (count == NULL || !IsA(count, Const) || count->constisnull ||
		parse->limitOption != LIMIT_OPTION_COUNT || parse->hasTargetSRFs)
		return 0;
	n = DatumGetInt64(count->constvalue);
	if (n <= 0)
		return 0;
	if (parse->limitOffset != NULL)
	{
		Const	   *off = (Const *) parse->limitOffset;

		if (!IsA(off, Const))
			return 0;
		if (!off->constisnull)
			offset = Max(DatumGetInt64(off->constvalue), 0);
	}
	if (n > PG_INT32_MAX - offset)
		return 0;
	return n + offset;
}

/*
 * The final target as the VecSort evaluates it: what the input's target
 * holds whole -- an aggregate, a window function's result, a grouping
 * expression -- is a column of the scan tuple, as set_plan_references()
 * matches it (setrefs.c, fix_upper_expr()), and only what is computed over
 * those columns is the node's to evaluate.  For the oracle.
 */
static Node *
over_columns_mutator(Node *node, List *input_exprs)
{
	if (node == NULL)
		return NULL;
	if (list_member(input_exprs, node))
		return (Node *) makeVar(INDEX_VAR, 1, exprType(node), exprTypmod(node),
								exprCollation(node), 0);
	return expression_tree_mutator(node, over_columns_mutator, input_exprs);
}

/*
 * Whether a path of the ordered relation is an index scan's order: one whose
 * index orders it by a distance (indexorderbys) -- a nearest-neighbour
 * search's HNSW, IVFFlat or GiST index -- under projections or not.
 */
static bool
index_ordered_path(Path *path)
{
	while (IsA(path, ProjectionPath))
		path = ((ProjectionPath *) path)->subpath;
	return IsA(path, IndexPath) && ((IndexPath *) path)->indexorderbys != NIL;
}

/*
 * A VecSort path: the row Sort of the same input its one custom path.
 * late_ok: late columns, where the planner priced them a saving
 * (late_saving()).
 */
static Path *
vecsort_path(RelOptInfo *rel, PathTarget *target, SortPath *rowpath, const VexecCost *cost,
			 int64 bound, bool late_ok)
{
	CustomPath *cp = makeNode(CustomPath);
	Path	   *input = rowpath->subpath;

	cp->path.pathtype = T_CustomScan;
	cp->path.parent = rel;
	cp->path.pathtarget = target;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = rel->consider_parallel && input->parallel_safe;
	cp->path.parallel_workers = input->parallel_workers;
	cp->path.rows = rowpath->path.rows;
	cp->path.disabled_nodes = rowpath->path.disabled_nodes;
	cp->path.startup_cost = cost->startup;
	cp->path.total_cost = cost->total;
	cp->path.pathkeys = rowpath->path.pathkeys;
	cp->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cp->custom_paths = list_make1(rowpath);
	cp->custom_private = list_make2(makeInteger((int) bound), makeInteger(late_ok ? 1 : 0));
	cp->methods = &vecsort_path_methods;
	return &cp->path;
}

/*
 * The sort keys as the input computes them: each pathkey's member of its
 * class that the input's target holds, else its first member that is no
 * constant, which prepare_sort_from_pathkeys() then adds to the input's
 * target list.
 */
static List *
sort_key_exprs(List *pathkeys, PathTarget *target)
{
	List	   *keys = NIL;
	ListCell   *lc;

	foreach(lc, pathkeys)
	{
		EquivalenceClass *ec = ((PathKey *) lfirst(lc))->pk_eclass;
		Expr	   *key = NULL;
		ListCell   *lm;

		foreach(lm, ec->ec_members)
		{
			EquivalenceMember *em = lfirst(lm);

			if (em->em_is_const)
				continue;
			if (list_member(target->exprs, em->em_expr))
			{
				key = em->em_expr;
				break;
			}
			if (key == NULL)
				key = em->em_expr;
		}
		if (key == NULL)
			return NIL;
		keys = lappend(keys, key);
	}
	return keys;
}

/*
 * M4, M5 (§3.21): whether a sort key is of a type the participants' shared
 * bound encodes, compared as an integer -- an integer, a date, a timestamp
 * (vecsort.c's bound_begin()).
 */
static bool
key_compared_as_integer(Expr *key)
{
	switch (exprType((Node *) key))
	{
		case INT2OID:
		case INT4OID:
		case DATEOID:
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			return true;
		default:
			return false;
	}
}

/* M5: whether the first sort key is compared as an integer (above). */
static bool
first_key_integer(List *pathkeys, PathTarget *target)
{
	List	   *keys = sort_key_exprs(pathkeys, target);

	return keys != NIL && key_compared_as_integer(linitial(keys));
}

/*
 * M5 (§3.21): core's Gather Merge over a row Sort of the partial path pinput
 * -- create_ordered_paths()'s, or a scan relation's sorted one -- taken out
 * of the ordered relation where VecSort's of the same input, gm, costs no
 * more to start and no more in all, by add_path()'s fuzz.  Within the fuzz
 * add_path() keeps the path it had, core's, though the vector sort saves
 * handing every row of the scan to a row Sort as a tuple, which its price
 * holds at a column's conversion: ClickBench's Q24 and Q26 on MergeTree took
 * 34 ms so and 25 with VecSort.  Only where the first key is compared as an
 * integer: over text keys VecSort's running bound, checked before the
 * scan's quals, made Q25 slower (29 -> 34 ms).
 */
static void
drop_row_sort_gather(RelOptInfo *output_rel, Path *pinput, Path *gm)
{
	ListCell   *lc;

	foreach(lc, output_rel->pathlist)
	{
		Path	   *p = lfirst(lc);
		Path	   *sub;

		if (!IsA(p, GatherMergePath))
			continue;
		sub = ((GatherMergePath *) p)->subpath;
		if (!IsA(sub, SortPath) || ((SortPath *) sub)->subpath != pinput)
			continue;
		if (!pathkeys_contained_in(gm->pathkeys, p->pathkeys) ||
			gm->startup_cost > p->startup_cost * VEXEC_PATH_FUZZ ||
			gm->total_cost > p->total_cost * VEXEC_PATH_FUZZ)
			continue;
		output_rel->pathlist = foreach_delete_current(output_rel->pathlist, lc);
	}
}

/*
 * F17 (§3.38): whether pinput is a grouping a bounded VecSort ranks in each
 * participant -- a hashed VecAgg's final stage below the Gather, its LIMIT a
 * batch's rows at most -- which the planner's estimate of its groups neither
 * refuses (vexec.min_rows) nor prices out: ANALYZE's sample counts a text
 * column's values hundreds of times short at 100M rows, ClickBench's Q12
 * 4,285 groups for 6.0M, and core's Sort over a Gather then took every group
 * through the Gather to the leader (37.5% of Q12's time).  Where the groups
 * are few, each participant sorts a few.  Not where the device groups --
 * either input -- whose own rank (W2) hands up the bound's groups alone.
 */
static bool
ranked_in_participants(Path *input, Path *pinput, int64 bound)
{
	return vexec_enable_participant_rank && pinput != NULL && bound > 0 && bound <= vexec_batch_rows &&
		vexec_is_vector_path(pinput) && vexec_agg_path_ranks(pinput) && !vexec_agg_path_device(pinput) &&
		!(input != NULL && vexec_agg_path_device(input));
}

/*
 * F17: core's Sort over a Gather of the same grouping, pinput, taken out of
 * the ordered relation where the Gather Merge over each participant's
 * bounded VecSort, gm, costs no more by add_path()'s fuzz, as M5's
 * drop_row_sort_gather() does for a scan
 */
static void
drop_sort_over_gather(RelOptInfo *output_rel, Path *pinput, Path *gm)
{
	ListCell   *lc;

	foreach(lc, output_rel->pathlist)
	{
		Path	   *p = lfirst(lc);
		Path	   *sub;

		if (!IsA(p, SortPath))
			continue;
		sub = ((SortPath *) p)->subpath;
		if (!IsA(sub, GatherPath) || ((GatherPath *) sub)->subpath != pinput)
			continue;
		if (!pathkeys_contained_in(gm->pathkeys, p->pathkeys) ||
			gm->total_cost > p->total_cost * VEXEC_PATH_FUZZ)
			continue;
		output_rel->pathlist = foreach_delete_current(output_rel->pathlist, lc);
	}
}

/*
 * F27 (§3.38): core's sorted grouping -- a GroupAggregate whose order is the
 * query's -- taken out of the ordered relation where the grouping's estimate
 * knows nothing of its groups -- an expression's, which has no statistics,
 * estimated at nine in ten of the input's rows or more -- and VecSort over
 * the hashed VecAgg of the same grouping, vsort, costs no more in whole.  A
 * LIMIT prices the sorted grouping by the share of its groups it takes, as
 * if those groups were all there are; ClickBench's Q42, date_trunc('minute',
 * EventTime) under a filter on EventDate, had 81,330 groups estimated of
 * 85,562 rows for about 1,440: its LIMIT 1010's share, 1.2%, made core's
 * plan 4,825 against VecAgg's and VecSort's 11,374, which ran 16 ms where
 * core's took 40.
 */
static void
drop_uninformed_sorted_agg(PlannerInfo *root, RelOptInfo *output_rel, Path *input, Path *vsort,
						   PathTarget *target)
{
	List	   *keys;
	Path	   *child;
	ListCell   *lc;

	if (!vexec_enable_uninformed_groups || !vexec_agg_path_ranks(input) ||
		((CustomPath *) input)->custom_paths == NIL)
		return;
	keys = sort_key_exprs(root->sort_pathkeys, target);
	if (keys == NIL || IsA(linitial(keys), Var))
		return;
	child = linitial(((CustomPath *) input)->custom_paths);
	if (child->rows <= 0 || input->rows < 0.9 * child->rows)
		return;
	foreach(lc, output_rel->pathlist)
	{
		Path	   *p = lfirst(lc);

		if (!IsA(p, AggPath) || ((AggPath *) p)->aggstrategy != AGG_SORTED ||
			!pathkeys_contained_in(root->sort_pathkeys, p->pathkeys) ||
			vsort->total_cost > p->total_cost)
			continue;
		output_rel->pathlist = foreach_delete_current(output_rel->pathlist, lc);
	}
}

/*
 * M4 (§3.21): whether a parallel sort's participants fetch its late columns
 * for the whole sort's rows alone, whichever of them is free fetching each
 * (vecsort.c): where its first key is a column the shared bound encodes.
 */
static bool
late_exchanged(List *keys)
{
	return vexec_enable_late_exchange && IsA(linitial(keys), Var) &&
		key_compared_as_integer(linitial(keys));
}

/*
 * M4: the rows each participant fetches one after another where they
 * exchange a parallel sort's n rows, each fetched once by the next one free:
 * n's share of them.  Priced as n fetches one after another, ClickBench's
 * Q23 over a table of its own Parquet files sorted every column of its rows
 * on the CPU, 813 to 841 ms, where late it takes 216 to 241.  W55 (§3.36):
 * shared among no more participants than the processors parallel query may
 * use -- max_parallel_workers' and the leader's -- hold their fetches at
 * once, each fetch on `threads` threads of the source's own
 * (VexecSourceEstimate.fetch_threads, minor 14): all of them for a fetch on
 * its caller's thread alone, one where a fetch takes them all
 */
static double
late_shared_rows(Path *input, double n, double threads)
{
	double		at_once = floor(((double) max_parallel_workers + 1) / Max(threads, 1.0));

	return ceil(n / Max(Min(vexec_parallel_divisor(input), at_once), 1.0));
}

/*
 * H6, priced: a bounded VecSort straight over a VecScan sorts the keys and
 * TIDs alone and fetches the rest of each row it keeps
 * (vexec_sort_make_late()), so its input costs what the narrower scan
 * costs (paths.c, vexec_late_scan_cost()), and each participant fetches
 * its kept rows: at the price the source gives a fetch of the late columns
 * (VexecSourceEstimate.fetch, minor 4, VC16) -- a column store's decodes a
 * granule of each -- each participant fetching its own beside the others;
 * else at a random page and a tuple each; the whole sort's N rows once
 * where the participants exchange them (late_exchanged()), each
 * participant fetching its share (late_shared_rows()).  What a sort
 * over the input as it is saves so; 0 where it is not made late, or saves
 * nothing.  *priced says whether the source priced its fetches.
 *
 * Both scans are compared without what each one's cost carries for handing
 * its columns to a row parent (vexec_cost_rowout()): the VecSort reads
 * either's batches as they are, and vexec_cost_sort() already gives the
 * wider scan's back.  Compared with it, the late columns' share was given
 * back twice, and a sort of a hundred columns cost less than nothing.
 */
static Cost
late_saving(PlannerInfo *root, Path *input, int64 bound, bool *priced)
{
	List	   *keys;
	VexecCost	late;
	double		participants;
	double		fetched;
	double		spc_random_page_cost;
	Cost		fetch;
	double		threads;
	Cost		saved;

	*priced = false;
	if (bound <= 0 || bound > VEXEC_LATE_ROWS)
		return 0;
	keys = sort_key_exprs(root->sort_pathkeys, input->pathtarget);
	if (keys == NIL || !vexec_late_scan_cost(root, input, keys, &late, &fetch, &threads))
		return 0;
	participants = input->parallel_workers > 0 && !late_exchanged(keys) ?
		ceil(vexec_parallel_divisor(input)) : 1.0;
	fetched = input->parallel_workers > 0 && late_exchanged(keys) ?
		late_shared_rows(input, (double) bound, threads) : (double) bound;
	get_tablespace_page_costs(input->parent->reltablespace, &spc_random_page_cost, NULL);
	*priced = fetch > 0;
	saved = (input->total_cost - vexec_cost_rowout(input)) - (late.total - late.rowout) -
		(fetch > 0 ? fetched * (fetch + cpu_tuple_cost) :
		 fetched * participants * (spc_random_page_cost + cpu_tuple_cost));
	return Max(saved, 0);
}

/* A sort's cost less what its late columns save, never below nothing. */
static void
take_saving(VexecCost *cost, Cost saving)
{
	cost->startup = Max(cost->startup - saving, 0);
	cost->total = Max(cost->total - saving, cost->startup);
}

/*
 * A base relation's VecScan paths, kept aside for its ORDER BY (H6).  Where
 * another path of the relation is cheaper -- ChScan's or a row scan's of
 * every column of a wide table, which VecScan's prices transpose -- add_path()
 * drops the VecScan's and frees it, and the ordered relation's input then
 * holds no scan a bounded VecSort could read only its keys from, though
 * reading them is what makes such a sort cheap.  A copy of each, which no
 * path list holds and so nothing frees, stays for vexec_consider_sort():
 * the serial VecScan's and the parallel-aware one's.
 */
typedef struct VexecKeptScan
{
	RelOptInfo *rel;
	Path	   *path;			/* the serial VecScan's copy, or NULL */
	Path	   *partial;		/* the parallel-aware one's, or NULL */
} VexecKeptScan;

void
vexec_sort_keep_scan(VexecPlanState *ps, Path *path)
{
	MemoryContext old = MemoryContextSwitchTo(ps->mcxt);
	CustomPath *copy = palloc(sizeof(CustomPath));
	VexecKeptScan *kept = NULL;
	ListCell   *lc;

	memcpy(copy, path, sizeof(CustomPath));
	foreach(lc, ps->kept_scans)
		if (((VexecKeptScan *) lfirst(lc))->rel == path->parent)
			kept = lfirst(lc);
	if (kept == NULL)
	{
		kept = palloc0(sizeof(VexecKeptScan));
		kept->rel = path->parent;
		ps->kept_scans = lappend(ps->kept_scans, kept);
	}
	if (path->parallel_aware)
		kept->partial = &copy->path;
	else
		kept->path = &copy->path;
	MemoryContextSwitchTo(old);
}

/*
 * The kept VecScan of the ordered relation's input, with the input's
 * target: the scan/join relation's paths carry the final scan/join target
 * (apply_scanjoin_target_to_paths()), which a VecScan projects
 * (CUSTOMPATH_SUPPORT_PROJECTION), and a late sort over it only where that
 * target is the relation's columns (vexec_late_scan_cost()).  Its saving
 * over the input as it is, in *saving; NULL where it saves nothing, or the
 * bound is past VEXEC_KEPT_LATE_ROWS and the source does not price its
 * fetches.
 */
static Path *
kept_late_input(PlannerInfo *root, Path *kept, PathTarget *target, int64 bound, Cost *saving)
{
	CustomPath *copy;
	bool		priced;

	*saving = 0;
	if (kept == NULL)
		return NULL;
	copy = palloc(sizeof(CustomPath));
	memcpy(copy, kept, sizeof(CustomPath));
	copy->path.pathtarget = target;
	*saving = late_saving(root, &copy->path, bound, &priced);
	if (*saving <= 0 || (!priced && bound > VEXEC_KEPT_LATE_ROWS))
	{
		*saving = 0;
		pfree(copy);
		return NULL;
	}
	return &copy->path;
}

/*
 * The input relation's cheapest path, where the ordered relation's paths
 * still hold it -- as it is, or under the Sort, the incremental sort or the
 * projection create_ordered_paths() put over it -- and NULL elsewhere.  A
 * path already in the order is handed to the ordered relation itself
 * there, and when a later path of that relation beats it, add_path() frees
 * it, while the input relation's list and its cheapest_total_path still
 * name it (PG19:src/backend/optimizer/plan/planner.c,
 * create_ordered_paths(), "sorted_path = input_path"): the hook runs after
 * all of that, so the path is never read through the input relation alone.
 * Only pointers are compared here.  The input's partial paths are only ever
 * put under a Gather Merge there, and stay.
 */
static Path *
live_input(Path *input, RelOptInfo *output_rel)
{
	ListCell   *lc;

	if (input == NULL)
		return NULL;
	foreach(lc, output_rel->pathlist)
	{
		Path	   *path = lfirst(lc);

		while (path != NULL)
		{
			if (path == input)
				return input;
			switch (nodeTag(path))
			{
				case T_SortPath:
					path = ((SortPath *) path)->subpath;
					break;
				case T_IncrementalSortPath:
					path = ((IncrementalSortPath *) path)->spath.subpath;
					break;
				case T_ProjectionPath:
					path = ((ProjectionPath *) path)->subpath;
					break;
				default:
					path = NULL;
					break;
			}
		}
	}
	return NULL;
}

/* The ordered relation's input's kept VecScans, if any. */
static VexecKeptScan *
kept_scans_of(VexecPlanState *ps, RelOptInfo *rel)
{
	ListCell   *lc;

	foreach(lc, ps->kept_scans)
		if (((VexecKeptScan *) lfirst(lc))->rel == rel)
			return lfirst(lc);
	return NULL;
}

/* A base relation's serial VecScan kept aside, or NULL: for a grouping in its order (VC4, agg.c) */
Path *
vexec_sort_kept_scan(VexecPlanState *ps, RelOptInfo *rel)
{
	VexecKeptScan *kept = kept_scans_of(ps, rel);

	return kept != NULL ? kept->path : NULL;
}

/*
 * W13 (§3.27): whether the device ranks a sort key of the scanned relation
 * as the sort orders it -- a column of the relation, of a type the device
 * orders as the key's operator does (vexec_sort_device_key()), no float --
 * ordered by its type's < or >: *descending set.
 */
static bool
device_rank_key(Expr *key, Index relid, Oid sortop, Oid collation, bool *descending)
{
	Oid			typid = exprType((Node *) key);

	if (!IsA(key, Var) || ((Var *) key)->varno != relid || ((Var *) key)->varattno <= 0 ||
		((Var *) key)->varlevelsup != 0)
		return false;
	if (getBaseType(typid) == FLOAT4OID || getBaseType(typid) == FLOAT8OID)
		return false;
	return OidIsValid(sortop) && vexec_sort_device_key(typid, sortop, collation, descending) == NULL;
}

/*
 * W13 (§3.27): a bounded sort over the device's read of a serial VecScan's
 * units, its rank cut there (exec/vecscan.c): the scan's twin reading the
 * sort keys' columns and its quals' -- and each row's TID, where the sort's
 * target holds more of the relation's columns than its keys, those then
 * fetched late (H6; *late, and the source's price of a fetch in *fetch) --
 * every column of the target else, the bound's rows handed up.  NULL where
 * a key is no column of the relation the device ranks, or the device may
 * not read the units so (paths.c, vexec_scan_device_variant()).
 */
static Path *
ranked_device_input(PlannerInfo *root, Path *scan, PathTarget *target, int64 bound, bool *late, Cost *fetch,
					double *threads)
{
	RelOptInfo *rel = scan->parent;
	List	   *keys = sort_key_exprs(root->sort_pathkeys, target);
	Bitmapset  *attrs = NULL;
	Path	   *dscan;
	double		width = 0;
	ListCell   *lc;
	ListCell   *lk;

	*late = false;
	*fetch = 0;
	*threads = 0;
	if (keys == NIL || list_length(keys) != list_length(root->sort_pathkeys))
		return NULL;
	forboth(lc, keys, lk, root->sort_pathkeys)
	{
		Expr	   *key = lfirst(lc);
		PathKey    *pk = lfirst(lk);
		Oid			typid = exprType((Node *) key);
		Oid			sortop = get_opfamily_member_for_cmptype(pk->pk_opfamily, typid, typid, pk->pk_cmptype);
		bool		desc;

		if (!device_rank_key(key, rel->relid, sortop, exprCollation((Node *) key), &desc))
			return NULL;
		width += get_typavgwidth(typid, exprTypmod((Node *) key));
	}

	/*
	 * late columns where every other column of the target is the relation's,
	 * one of its own among them the device would not read anyway, for the
	 * quals: ClickBench's Q24 fetched its SearchPhrase by TID, 80 ms of a
	 * fresh backend's on Parquet, where its qual had the device read it
	 */
	{
		Bitmapset  *qual_attrs = NULL;

		foreach(lc, rel->baserestrictinfo)
			pull_varattnos((Node *) ((RestrictInfo *) lfirst(lc))->clause, rel->relid, &qual_attrs);
		foreach(lc, target->exprs)
		{
			Var		   *v = lfirst(lc);

			if (list_member(keys, v))
				continue;
			if (!IsA(v, Var) || v->varno != rel->relid || v->varlevelsup != 0 || v->varattno == 0)
			{
				*late = false;
				break;
			}
			if (v->varattno > 0 && !bms_is_member(v->varattno - FirstLowInvalidHeapAttributeNumber, qual_attrs))
				*late = true;
		}
	}
	if (*late)
	{
		VexecCost	lcost;

		if (bound > VEXEC_LATE_ROWS || !vexec_late_scan_cost(root, scan, keys, &lcost, fetch, threads))
			*late = false;
	}
	if (*late)
	{
		pull_varattnos((Node *) keys, rel->relid, &attrs);
		foreach(lc, rel->baserestrictinfo)
			pull_varattnos((Node *) ((RestrictInfo *) lfirst(lc))->clause, rel->relid, &attrs);
		attrs = bms_add_member(attrs, SelfItemPointerAttributeNumber - FirstLowInvalidHeapAttributeNumber);
		width += sizeof(ItemPointerData);
	}
	else
	{
		pull_varattnos((Node *) target->exprs, rel->relid, &attrs);
		foreach(lc, rel->baserestrictinfo)
			pull_varattnos((Node *) ((RestrictInfo *) lfirst(lc))->clause, rel->relid, &attrs);
		width = target->width;
	}
	dscan = vexec_scan_device_variant(root, scan, attrs, Min((double) bound, scan->rows),
									  *late ? list_length(keys) + 1 : list_length(target->exprs), width);
	if (dscan != NULL)
		dscan->pathtarget = target;
	return dscan;
}

/*
 * W13: the device's read of a VecSort's child's units, where the plan made
 * it so (paths.c), lent the sort's rank: its bound and its keys, each a
 * column of the child's relation the device ranks -- after the late columns
 * made the child's target list the keys and each row's TID.  Unranked where
 * a key is not, its rows every one the filter keeps.
 */
static void
rank_device_scan(CustomScan *cscan)
{
	VexecSortPlan plan;
	CustomScan *child = (CustomScan *) cscan->scan.plan.lefttree;
	int			n;
	AttrNumber *attnos;
	bool	   *desc;
	bool	   *nulls_first;
	int			i = 0;
	ListCell   *lk;
	ListCell   *lo;
	ListCell   *lcoll;
	ListCell   *ln;

	vexec_sort_plan_decode(cscan, &plan);
	if (plan.bound <= 0 || plan.bound > VEXEC_SCAN_RANK_MAX)
		return;
	n = list_length(plan.keycols);
	attnos = palloc(sizeof(AttrNumber) * n);
	desc = palloc(sizeof(bool) * n);
	nulls_first = palloc(sizeof(bool) * n);
	forfour(lk, plan.keycols, lo, plan.operators, lcoll, plan.collations, ln, plan.nullsfirst)
	{
		TargetEntry *tle = get_tle_by_resno(child->scan.plan.targetlist, lfirst_int(lk));

		if (tle == NULL ||
			!device_rank_key(tle->expr, child->scan.scanrelid, lfirst_oid(lo), lfirst_oid(lcoll), &desc[i]))
			return;
		attnos[i] = ((Var *) tle->expr)->varattno;
		nulls_first[i] = lfirst_int(ln) != 0;
		i++;
	}
	vexec_scan_plan_rank(child, plan.bound, n, attnos, desc, nulls_first);
}

/*
 * VecSort's alternative for the query's ORDER BY (§3.3.3, UPPERREL_ORDERED):
 * recorded for EXPLAIN, and in auto and force mode added.  In parallel, the
 * input's cheapest partial path is sorted in each participant too, under a
 * Gather Merge, as create_ordered_paths() sorts it: each participant's
 * sort bounded as the whole one is, the LIMIT's rows being among each
 * one's first.  The input's cheapest path is taken where the planner's
 * paths still hold it (live_input()); where a sort of it lost to another
 * path, or it was in the order already and lost, only the partial path is
 * sorted.  Where neither is a VecScan, a bounded sort over the relation's
 * own, kept aside (vexec_sort_keep_scan()), is offered too, where its late
 * columns save: a scan of every column of a wide table is cheaper as
 * ChScan's than as VecScan's, and a VecSort over it reads them all, where
 * one over the VecScan reads the keys alone.
 */
void
vexec_consider_sort(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel,
					VexecPlanState *ps, const char *target_name)
{
	VexecAlt   *alt = NULL;
	Path	   *input;
	Path	   *pinput = NULL;
	Path	   *base;
	double		rows;
	bool		add = ps->mode == VEXEC_MODE_AUTO || ps->mode == VEXEC_MODE_FORCE;
	SortPath   *rowpath = NULL;
	SortPath   *prow = NULL;
	VexecCost	pcost;
	VexecKeptScan *kept;
	Path	   *linput = NULL;
	Path	   *lpinput = NULL;
	SortPath   *lrow = NULL;
	SortPath   *lprow = NULL;
	VexecCost	lcost;
	VexecCost	lpcost;
	Cost		lsaving = 0;
	Cost		lpsaving = 0;
	Path	   *dinput = NULL;
	SortPath   *drow = NULL;
	VexecCost	dcost;
	Path	   *rinput = NULL;
	SortPath   *rrow = NULL;
	VexecCost	rcost;
	bool		rlate = false;
	Cost		rfetch = 0;
	double		rthreads = 0;
	Path	   *rpinput = NULL;
	SortPath   *rprow = NULL;
	VexecCost	rpcost;
	bool		rplate = false;
	Cost		rpfetch = 0;
	double		rpthreads = 0;
	List	   *newpaths;
	VexecSteps	steps;
	VexecCost	cost;
	PathTarget *target;
	int64		bound;
	Cost		saving;
	Cost		psaving;
	bool		priced;
	bool		index_kept = false;
	ListCell   *lc;

	if (root->sort_pathkeys == NIL || output_rel->pathlist == NIL)
		return;

	/*
	 * The query's final target, which the ordered relation's paths make:
	 * create_ordered_paths() gives it to them and leaves the relation's own
	 * reltarget empty (planner.c).
	 */
	target = ((Path *) linitial(output_rel->pathlist))->pathtarget;
	if (ps->record)
		alt = vexec_alt_record(ps, "VecSort", target_name, root, NULL);
	if (alt == NULL && !add)
		return;

	if (!vexec_enable_sort)
	{
		vexec_alt_refuse(ps, alt, "vexec.enable_sort is off");
		return;
	}
	/*
	 * The input's cheapest path, where the planner's ordered paths still hold
	 * it (live_input()).  Where they do not -- its Sort lost to the Gather
	 * Merge of a parallel plan, or it was in the order already and lost --
	 * only the input's partial path is sorted, under a Gather Merge.
	 */
	input = live_input(input_rel->cheapest_total_path, output_rel);
	if (input != NULL && PATH_REQ_OUTER(input) != NULL)
	{
		vexec_alt_refuse(ps, alt, "a parameterized input");
		return;
	}
	if (input != NULL && pathkeys_contained_in(root->sort_pathkeys, input->pathkeys))
	{
		vexec_alt_refuse(ps, alt, "its input is in the order already");
		return;
	}

	/*
	 * The input's cheapest partial path, sorted in each participant under a
	 * Gather Merge where the query may run in parallel; and what late
	 * columns save, which a sort of few rows over a large scan is for (H6):
	 * such a sort is not refused for its few rows.
	 */
	bound = vexec_query_bound(root);
	if (output_rel->consider_parallel && input_rel->partial_pathlist != NIL)
	{
		pinput = linitial(input_rel->partial_pathlist);
		if (PATH_REQ_OUTER(pinput) != NULL ||
			(input != NULL && !equal(pinput->pathtarget->exprs, input->pathtarget->exprs)) ||
			pathkeys_contained_in(root->sort_pathkeys, pinput->pathkeys))
			pinput = NULL;
	}
	if (input == NULL && pinput == NULL)
	{
		vexec_alt_refuse(ps, alt, "no path of its input that the planner's ordered paths still hold, and no partial one");
		return;
	}
	base = input != NULL ? input : pinput;
	rows = input != NULL ? input->rows : pinput->rows * vexec_parallel_divisor(pinput);
	saving = input != NULL ? late_saving(root, input, bound, &priced) : 0;
	psaving = pinput != NULL ? late_saving(root, pinput, bound, &priced) : 0;

	/*
	 * Where the input's cheapest paths are no VecScan, the relation's own,
	 * kept aside, read for the keys alone by a bounded VecSort that fetches
	 * the rest of the rows it keeps -- serially, and in each participant
	 * under a Gather Merge -- where that saves.
	 */
	kept = kept_scans_of(ps, input_rel);
	if (kept != NULL && bound > 0 && bound <= VEXEC_LATE_ROWS)
	{
		if (input == NULL || !vexec_is_scan_path(input))
			linput = kept_late_input(root, kept->path, base->pathtarget, bound, &lsaving);
		if (output_rel->consider_parallel && (pinput == NULL || !vexec_is_scan_path(pinput)))
			lpinput = kept_late_input(root, kept->partial, base->pathtarget, bound, &lpsaving);
	}

	/*
	 * W7 (§3.27): the grouped relation's device path, kept aside where a
	 * cheaper one dropped it (agg.c, vexec_agg_kept_device()): a bounded sort
	 * over it gives back what the device's rank saves, which the grouped
	 * relation's choice did not count -- ClickBench's Q36 on mergetree,
	 * 741.56 so where the cheapest input's plan was 1,807.21
	 */
	if (bound > 0 && (input == NULL || !vexec_agg_path_device(input)))
	{
		dinput = vexec_agg_kept_device(ps, input_rel);
		if (dinput != NULL && !equal(dinput->pathtarget->exprs, base->pathtarget->exprs))
			dinput = NULL;
	}

	/*
	 * W13 (§3.27): over the device's read of the relation's units, ranked
	 * there by the sort's keys -- the bound's rows and those tying with the
	 * last of them handed up, the rest of each row fetched late by its TID:
	 * ClickBench's Q23, the ten earliest of 18,000 rows with "google" in
	 * their URLs, took 78 ms on 16 workers -- over the relation's serial
	 * VecScan, the input or the one kept aside
	 */
	if (bound > 0 && bound <= VEXEC_SCAN_RANK_MAX)
	{
		Path	   *sscan = input != NULL && vexec_is_scan_path(input) && !input->parallel_aware ? input :
			kept != NULL ? kept->path : NULL;

		if (sscan != NULL)
			rinput = ranked_device_input(root, sscan, base->pathtarget, bound, &rlate, &rfetch, &rthreads);
	}

	/*
	 * The same over the relation's parallel VecScan kept aside, the device's
	 * read in one participant of a parallel plan (exec/vecscan.c,
	 * dev_scan_owner()), where the rows have late columns the participants
	 * fetch together (M4) -- not where none is late: the serial read hands
	 * up the same rows without the workers.  ClickBench's Q23 on mergetree
	 * took 114 ms over the serial read, its ten rows' 103 late columns
	 * fetched in its one process, where 16 workers took 68; over this one,
	 * 58
	 */
	if (bound > 0 && bound <= VEXEC_LATE_ROWS && output_rel->consider_parallel && kept != NULL &&
		kept->partial != NULL)
	{
		List	   *keys = sort_key_exprs(root->sort_pathkeys, base->pathtarget);

		if (keys != NIL && late_exchanged(keys))
			rpinput = ranked_device_input(root, kept->partial, base->pathtarget, bound, &rplate, &rpfetch,
										  &rpthreads);
		if (rpinput != NULL && !rplate)
			rpinput = NULL;
	}

	/*
	 * Nor a bounded sort over a VecAgg the device takes, whose groups it
	 * ranks there and hands back the bound's alone (W2, §3.26): that rank is
	 * this node's to lend, and the planner's estimate of the groups may be
	 * far short -- ClickBench's Q12, 4,241 groups of SearchPhrase estimated,
	 * whose 892,231 PostgreSQL's Sort then took from the device: 106 ms
	 * where the rank took 6
	 */
	if (ps->mode != VEXEC_MODE_FORCE && rows < vexec_min_rows && saving <= 0 && psaving <= 0 &&
		linput == NULL && lpinput == NULL && dinput == NULL && rinput == NULL && rpinput == NULL &&
		!(bound > 0 && input != NULL && vexec_agg_path_device(input) && vexec_agg_path_ranks(input)) &&
		!ranked_in_participants(input, pinput, bound))
	{
		vexec_alt_refuse(ps, alt, psprintf("%.0f input rows, fewer than vexec.min_rows", rows));
		return;
	}
	memset(&steps, 0, sizeof(steps));
	vexec_oracle_exprs(root, (List *) over_columns_mutator((Node *) target->exprs,
														   base->pathtarget->exprs),
					   &steps);
	if (steps.refusal)
	{
		vexec_alt_refuse(ps, alt, steps.refusal);
		return;
	}

	if (input != NULL)
	{
		rowpath = create_sort_path(root, output_rel, input, root->sort_pathkeys,
								   bound > 0 ? (double) bound : -1.0);
		vexec_cost_sort(root, &rowpath->path, input, vexec_is_vector_path(input), &cost);
		if (bound > 0 && vexec_agg_path_ranks(input))
			vexec_cost_topn(&cost, input, bound);	/* C3 */
		take_saving(&cost, saving);
	}
	if (pinput != NULL)
	{
		prow = create_sort_path(root, output_rel, pinput, root->sort_pathkeys,
								bound > 0 ? (double) bound : -1.0);
		vexec_cost_sort(root, &prow->path, pinput, vexec_is_vector_path(pinput), &pcost);
		if (bound > 0 && vexec_agg_path_ranks(pinput))
			vexec_cost_topn(&pcost, pinput, bound);	/* C3 */
		take_saving(&pcost, psaving);
	}
	if (linput != NULL)
	{
		lrow = create_sort_path(root, output_rel, linput, root->sort_pathkeys, (double) bound);
		vexec_cost_sort(root, &lrow->path, linput, true, &lcost);
		take_saving(&lcost, lsaving);
	}
	if (lpinput != NULL)
	{
		lprow = create_sort_path(root, output_rel, lpinput, root->sort_pathkeys, (double) bound);
		vexec_cost_sort(root, &lprow->path, lpinput, true, &lpcost);
		take_saving(&lpcost, lpsaving);
	}
	if (dinput != NULL)
	{
		/* W7: over the device's grouping, its rank given back (C3, W2) */
		drow = create_sort_path(root, output_rel, dinput, root->sort_pathkeys, (double) bound);
		vexec_cost_sort(root, &drow->path, dinput, true, &dcost);
		vexec_cost_topn(&dcost, dinput, bound);
	}
	if (rinput != NULL)
	{
		/* W13: the bound's rows the device hands up sorted, each fetched late by its TID as H6 prices it */
		rrow = create_sort_path(root, output_rel, rinput, root->sort_pathkeys, (double) bound);
		vexec_cost_sort(root, &rrow->path, rinput, true, &rcost);
		if (rlate)
		{
			Cost		fetches = Min((double) bound, rinput->rows) * (rfetch + cpu_tuple_cost);

			rcost.startup += fetches;
			rcost.total += fetches;
		}
	}
	if (rpinput != NULL)
	{
		/*
		 * the one participant's rows handed up sorted, fetched late by every
		 * participant (M4), as many at once as late_shared_rows() says; and
		 * W55 (§3.36), the other participants' starts, which the exchange
		 * waits for though they read nothing: each worker's launch at
		 * parallel_setup_cost.  ClickBench's Q23 on its own Parquet file of
		 * 100M rows, a fetch on 16 threads: 16 workers 245 to 272 ms, the
		 * serial read 137 to 142; on mergetree, a fetch a granule of each
		 * column on its process's thread: 58 against 114 (§3.21)
		 */
		double		nfetched = Min((double) bound, rpinput->rows);
		Cost		fetches = late_shared_rows(kept->partial, nfetched, rpthreads) * (rpfetch + cpu_tuple_cost) +
			kept->partial->parallel_workers * parallel_setup_cost;

		rprow = create_sort_path(root, output_rel, rpinput, root->sort_pathkeys, (double) bound);
		vexec_cost_sort(root, &rprow->path, rpinput, true, &rpcost);
		rpcost.startup += fetches;
		rpcost.total += fetches;
	}
	vexec_alt_costed(ps, alt, input != NULL ? &cost : &pcost,
					 psprintf("%s%s%s%s%s", count_of(list_length(root->sort_pathkeys), "sort key", "sort keys"),
							  bound > 0 ? psprintf(", bounded at " INT64_FORMAT, bound) : "",
							  input != NULL ? "" : "; in each participant alone",
							  linput != NULL || lpinput != NULL ?
							  "; also over the relation's VecScan, its keys alone read" : "",
							  dinput != NULL ? "; also over the device's grouping, ranked there" : ""));
	if (dinput != NULL)
		vexec_alt_costed(ps, alt, &dcost, "over the device's grouping, its groups ranked there");
	if (rpinput != NULL)
		vexec_alt_costed(ps, alt, &rpcost,
						 "over the device's read of its units in one participant, ranked there, its other columns late, fetched by every participant");
	if (rinput != NULL)
		vexec_alt_costed(ps, alt, &rcost, rlate ? "over the device's read of its units, ranked there, its other columns late" :
						 "over the device's read of its units, ranked there");
	if (!add)
		return;
	newpaths = NIL;
	if (input != NULL)
		newpaths = lappend(newpaths, vecsort_path(output_rel, target, rowpath, &cost, bound, saving > 0));
	if (pinput != NULL)
	{
		Path	   *psort = vecsort_path(output_rel, target, prow, &pcost, bound, psaving > 0);
		double		total = compute_gather_rows(psort);
		Path	   *gm = (Path *) create_gather_merge_path(root, output_rel, psort, target,
														   root->sort_pathkeys, NULL, &total);

		/* M5: a vector partial input's VecSort over core's row Sort, within the fuzz */
		if (vexec_prefer_vector_sort && vexec_is_vector_path(pinput) &&
			first_key_integer(root->sort_pathkeys, pinput->pathtarget))
			drop_row_sort_gather(output_rel, pinput, gm);
		/*
		 * F17: a grouping ranked in each participant over core's Sort of a
		 * Gather of every group, and over VecSort's own, within the fuzz --
		 * left to the costs where the device may rank the grouping itself
		 */
		if (ranked_in_participants(input, pinput, bound) && dinput == NULL && rpinput == NULL)
		{
			drop_sort_over_gather(output_rel, pinput, gm);
			if (input != NULL && newpaths != NIL)
			{
				Path	   *over_gather = linitial(newpaths);

				elog(DEBUG2, "vexec F17: ranked in each participant %.2f..%.2f, over the Gather %.2f..%.2f",
					 gm->startup_cost, gm->total_cost, over_gather->startup_cost, over_gather->total_cost);
				if (gm->total_cost <= over_gather->total_cost * VEXEC_PATH_FUZZ)
					newpaths = list_delete_first(newpaths);
			}
		}
		newpaths = lappend(newpaths, gm);
	}
	if (linput != NULL)
		newpaths = lappend(newpaths, vecsort_path(output_rel, target, lrow, &lcost, bound, true));
	if (dinput != NULL)
		newpaths = lappend(newpaths, vecsort_path(output_rel, target, drow, &dcost, bound, false));
	if (rinput != NULL)
		newpaths = lappend(newpaths, vecsort_path(output_rel, target, rrow, &rcost, bound, rlate));
	if (rpinput != NULL)
	{
		Path	   *psort = vecsort_path(output_rel, target, rprow, &rpcost, bound, true);
		double		total = compute_gather_rows(psort);

		newpaths = lappend(newpaths,
						   create_gather_merge_path(root, output_rel, psort, target,
													root->sort_pathkeys, NULL, &total));
	}
	if (lpinput != NULL)
	{
		Path	   *psort = vecsort_path(output_rel, target, lprow, &lpcost, bound, true);
		double		total = compute_gather_rows(psort);

		newpaths = lappend(newpaths,
						   create_gather_merge_path(root, output_rel, psort, target,
													root->sort_pathkeys, NULL, &total));
	}

	/*
	 * Force mode replaces core's sorts, and keeps the paths an index orders,
	 * for cost to choose between them and VecSort: a nearest-neighbour
	 * search keeps its index where it has one -- on a cluster's segment too,
	 * which plans the search gp_core sends it with its LIMIT
	 * (pg19/modules/gp_core/gp_scan.c, bound_nearest()) -- and gets VecSort
	 * where it has none.  Where such a path is kept, the plan may hold no
	 * vector node, and VecSort is not counted possible
	 * (vexec.debug_require_vector).
	 */
	if (ps->mode == VEXEC_MODE_FORCE)
	{
		List	   *kept = NIL;

		foreach(lc, output_rel->pathlist)
			if (index_ordered_path((Path *) lfirst(lc)))
				kept = lappend(kept, lfirst(lc));
		output_rel->pathlist = kept;
		index_kept = kept != NIL;
	}
	/* F27: core's sorted grouping by an estimate that knows nothing, against the VecSort's whole cost */
	if (input != NULL && newpaths != NIL && linitial(newpaths) != NULL && bound > 0)
		drop_uninformed_sorted_agg(root, output_rel, input, (Path *) linitial(newpaths), target);
	foreach(lc, newpaths)
		add_path(output_rel, lfirst(lc));
	if (!index_kept)
		ps->npossible++;
	ps->sorts_built = true;
}

/*
 * V8d: whether a VecSort takes the device's order where the node can take
 * it as it begins: an unbounded sort over keys the device orders as their
 * operators do (vexec_sort_device_key()), of SORT_DEVICE_MIN_ROWS rows or
 * more, the rows' width within work_mem, which the node's kept rows may not
 * pass; and, where PostgreSQL's planner gives root, keys of as many
 * distinct values as rows, by its estimate: rows that tie are tuplesort's
 * to order, which the node learns only once the device has their keys --
 * from its first chunk, of a few batches, where they tie early.  Measured
 * on this host (§5, V8d), the device's order was as fast as tuplesort's at
 * 5,000 rows, 1.1 to 1.4 times at 10,000 and 2.3 to 2.6 at 100,000.  Force
 * takes it wherever it is possible.
 */
#define SORT_DEVICE_MIN_ROWS	(16 * 1024)

static bool
sort_device(PlannerInfo *root, Sort *sort, int64 bound)
{
	Plan	   *child = sort->plan.lefttree;
	List	   *exprs = NIL;

	if (vexec_gpu == VEXEC_GPU_OFF || !vexec_enable_gpu_sort || bound > 0 || child == NULL)
		return false;
	if (vexec_device_ready() == NULL)
		return false;
	for (int i = 0; i < sort->numCols; i++)
	{
		TargetEntry *tle = get_tle_by_resno(child->targetlist, sort->sortColIdx[i]);
		bool		desc;

		if (tle == NULL ||
			vexec_sort_device_key(exprType((Node *) tle->expr), sort->sortOperators[i],
								  sort->collations[i], &desc) != NULL)
			return false;
		exprs = lappend(exprs, tle->expr);
	}
	if (vexec_gpu == VEXEC_GPU_FORCE)
		return true;
	if (child->plan_rows < SORT_DEVICE_MIN_ROWS ||
		child->plan_rows * (child->plan_width + 32) > (double) work_mem * 1024.0)
		return false;
	return root == NULL ||
		estimate_num_groups(root, exprs, child->plan_rows, NULL, NULL) >= 0.99 * child->plan_rows;
}

/*
 * VecSort's plan: the Sort core made of its custom path gives its keys and
 * its child, and is set aside.  Its scan tuple is the child's row, in
 * planner form for set_plan_references() to match its target list to,
 * which planner_shutdown_hook then makes references to the child's
 * columns (join.c, vexec_join_finish_plan()).
 */
static Plan *
plan_vecsort(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			 List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	Sort	   *sort = linitial(custom_plans);
	VexecSortPlan plan;
	List	   *scan_tlist = NIL;
	ListCell   *lc;
	int			i;

	(void) rel;
	(void) clauses;
	if (list_length(custom_plans) != 1 || !IsA(sort, Sort) || sort->plan.lefttree == NULL)
		elog(ERROR, "vexec: a VecSort's path planned without its Sort");

	memset(&plan, 0, sizeof(plan));
	for (i = 0; i < sort->numCols; i++)
	{
		plan.keycols = lappend_int(plan.keycols, sort->sortColIdx[i]);
		plan.operators = lappend_oid(plan.operators, sort->sortOperators[i]);
		plan.collations = lappend_oid(plan.collations, sort->collations[i]);
		plan.nullsfirst = lappend_int(plan.nullsfirst, sort->nullsFirst[i] ? 1 : 0);
	}
	plan.bound = intVal(linitial(best_path->custom_private));
	plan.device = sort_device(root, sort, plan.bound);

	/* the scan tuple: the child's row, column by column */
	foreach(lc, sort->plan.lefttree->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry(copyObject(tle->expr), list_length(scan_tlist) + 1,
											 NULL, false));
	}

	cscan->scan.plan.targetlist = tlist;
	cscan->scan.plan.qual = NIL;
	cscan->scan.plan.lefttree = sort->plan.lefttree;
	cscan->scan.scanrelid = 0;
	cscan->flags = best_path->flags;
	cscan->custom_plans = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_private = vexec_sort_plan_encode(&plan);
	cscan->custom_scan_tlist = scan_tlist;
	cscan->methods = vexec_sort_methods();
	{
		bool		late_ok = intVal(lsecond(best_path->custom_private)) != 0;

		(void) vexec_sort_make_late(cscan, late_ok, late_ok);
	}
	/* W13: the device's read of the child's units ranked by the sort's keys */
	if (IsA(cscan->scan.plan.lefttree, CustomScan) &&
		vexec_scan_plan_is_device((CustomScan *) cscan->scan.plan.lefttree))
		rank_device_scan(cscan);
	(void) vexec_sort_plan_running_bound(cscan);
	return &cscan->scan.plan;
}

/*
 * A bounded VecSort over a VecScan made to sort its keys and TIDs alone,
 * the rest of each kept row fetched by its TID (H6, above), where every
 * column of the child's row that is no key is a Var of the scanned
 * relation, and one at least a column of its own; whether it was.  Over a
 * serial scan only where serial_ok, a parallel one only where parallel_ok:
 * PostgreSQL's planner priced a saving there (late_saving()) -- a fetch in
 * a column store reads a granule of each column it decodes, which a narrow
 * row the scan reads anyway does not repay; ORCA's sorts are made late over
 * a serial scan, unpriced.  The
 * child's target list becomes the keys, then ctid; the scan tuple the
 * child's row as it was, over the relation's Vars.  Its Vars are those of
 * the relation the child scans, whichever form the plan is in --
 * PostgreSQL's planner's, before set_plan_references() moves them by the
 * same offset as the child's, or ORCA's -- and the executor finds the
 * relation from the child.
 */
bool
vexec_sort_make_late(CustomScan *cscan, bool serial_ok, bool parallel_ok)
{
	VexecSortPlan plan;
	Plan	   *child = cscan->scan.plan.lefttree;
	Index		relid;
	List	   *scan_tlist = NIL;
	List	   *child_tlist = NIL;
	List	   *keycols = NIL;
	bool		any_late = false;
	ListCell   *lc;
	int			nkeys;
	int			i;

	vexec_sort_plan_decode(cscan, &plan);
	if (plan.bound <= 0 || plan.bound > VEXEC_LATE_ROWS || plan.late_tidcol > 0)
		return false;
	if (child == NULL || !IsA(child, CustomScan) ||
		((CustomScan *) child)->methods != vexec_scan_methods() ||
		(child->parallel_aware ? !parallel_ok : !serial_ok) || ((Scan *) child)->scanrelid == 0 ||
		contain_subplans((Node *) child->targetlist))
		return false;
	relid = ((Scan *) child)->scanrelid;

	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		Var		   *v = (Var *) tle->expr;

		if (list_member_int(plan.keycols, foreach_current_index(lc) + 1))
			continue;
		if (!IsA(v, Var) || v->varno != relid || v->varlevelsup != 0 || v->varattno == 0)
			return false;
		if (v->varattno > 0)
			any_late = true;
	}
	if (!any_late)
		return false;

	/* the scan tuple: the child's row as it was */
	foreach(lc, child->targetlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		scan_tlist = lappend(scan_tlist,
							 makeTargetEntry(copyObject(tle->expr), list_length(scan_tlist) + 1,
											 NULL, false));
	}

	/* the child's: the keys, then each row's TID */
	nkeys = list_length(plan.keycols);
	for (i = 0; i < nkeys; i++)
	{
		TargetEntry *tle = list_nth_node(TargetEntry, child->targetlist,
										 list_nth_int(plan.keycols, i) - 1);

		child_tlist = lappend(child_tlist, makeTargetEntry(copyObject(tle->expr), i + 1,
														   NULL, false));
		keycols = lappend_int(keycols, i + 1);
	}
	child_tlist = lappend(child_tlist,
						  makeTargetEntry((Expr *) makeVar(relid, SelfItemPointerAttributeNumber,
														   TIDOID, -1, InvalidOid, 0),
										  nkeys + 1, NULL, false));

	child->targetlist = child_tlist;
	cscan->custom_scan_tlist = scan_tlist;
	plan.keycols = keycols;
	plan.late_tidcol = nkeys + 1;
	plan.late_relid = 0;		/* the child's, wherever setrefs puts it */
	cscan->custom_private = vexec_sort_plan_encode(&plan);
	return true;
}

/*
 * Whether an operator's function may raise an error for some row's values:
 * not a comparison by a btree operator family's operator of two built-in
 * scalar types, nor LIKE or ILIKE of a constant pattern -- whose errors,
 * a pattern's or a collation's, come from constants and are raised at the
 * first row.
 */
static bool
operator_may_raise(Oid opno, List *args)
{
	Oid			left;
	Oid			right;
	char	   *name;

	if (opno >= FirstNormalObjectId)
		return true;
	op_input_types(opno, &left, &right);
	if (!OidIsValid(left) || !OidIsValid(right) ||
		get_typtype(getBaseType(left)) != TYPTYPE_BASE || type_is_array(getBaseType(left)) ||
		get_typtype(getBaseType(right)) != TYPTYPE_BASE || type_is_array(getBaseType(right)))
		return true;
	if (get_op_index_interpretation(opno) != NIL)
		return false;
	name = get_opname(opno);
	if (name != NULL &&
		(strcmp(name, "~~") == 0 || strcmp(name, "!~~") == 0 ||
		 strcmp(name, "~~*") == 0 || strcmp(name, "!~~*") == 0))
		return list_length(args) != 2 || !IsA(lsecond(args), Const);
	return true;
}

/*
 * Whether an expression may raise an error for some row's values (H6): the
 * running bound is checked before a scan's quals only where none may, so
 * that a row PostgreSQL's quals would raise on is never dropped unread.
 * Only what is known not to: Vars, constants, parameters from outside, AND,
 * OR and NOT, NULL tests, relabelings, and the comparisons above.  A
 * PARAM_EXEC may run an InitPlan, which may raise.
 */
static bool
may_raise_walker(Node *node, void *context)
{
	if (node == NULL)
		return false;
	switch (nodeTag(node))
	{
		case T_Var:
		case T_Const:
			return false;
		case T_Param:
			return ((Param *) node)->paramkind != PARAM_EXTERN;
		case T_BoolExpr:
		case T_NullTest:
		case T_BooleanTest:
		case T_RelabelType:
		case T_List:
		case T_TargetEntry:
			break;
		case T_OpExpr:
		case T_DistinctExpr:
		case T_NullIfExpr:
			if (operator_may_raise(((OpExpr *) node)->opno, ((OpExpr *) node)->args))
				return true;
			break;
		case T_ScalarArrayOpExpr:
			if (operator_may_raise(((ScalarArrayOpExpr *) node)->opno, NIL))
				return true;
			break;
		default:
			return true;
	}
	return expression_tree_walker(node, may_raise_walker, context);
}

/*
 * Whether an expression may raise an error for some row's values (above):
 * for a filter lent to a scan, H6's and X6's, which drops rows before or
 * without the expressions PostgreSQL would compute for them.
 */
bool
vexec_expr_may_raise(Node *node)
{
	return may_raise_walker(node, NULL);
}

/*
 * Whether a running bound over a text key goes after the quals: where they only
 * ask of a text column's length or whether it is empty, which reads no value's
 * bytes, and the bound's comparison reads the key's -- ClickBench's Q25 at 100M
 * rows, 87% of its rows empty, each compared before its qual dropped it, took 128
 * ms and 89 by core's row Sort.
 */
/*
 * The running bound (H6): a bounded VecSort straight over a vector scan --
 * a VecScan or a VecBitmapHeapScan, parallel-aware or not -- whose first
 * sort key is a column of the scan's relation lends the scan the first key
 * of its N-th row once tuplesort keeps its rows in a bounded heap, and the
 * scan drops a row whose first key is strictly past it: before its quals,
 * in heap's page reader before the row's other columns are deformed, where
 * none of the quals may raise an error; after them otherwise, so that no
 * error PostgreSQL would raise is skipped.  None where the scan's target
 * list may raise: PostgreSQL computes it for every row the quals pass, a
 * row the sort then discards among them.  Whether it was planned.
 */
static bool
bound_after_empty_text(CustomScan *scan, const Var *key)
{
	VexecLengthUses w;
	bool		after = scan->scan.plan.qual != NIL && !get_typbyval(key->vartype);

	if (!after)
		return false;
	w.varno = scan->scan.scanrelid;
	w.n = MaxHeapAttributeNumber;
	w.uses = palloc0(w.n);
	(void) vexec_length_uses((Node *) scan->scan.plan.qual, &w);
	for (int i = 0; i < w.n && after; i++)
		after = w.uses[i] != 2;
	pfree(w.uses);
	return after;
}

bool
vexec_sort_plan_running_bound(CustomScan *cscan)
{
	VexecSortPlan plan;
	Plan	   *child = cscan->scan.plan.lefttree;
	CustomScan *scan;
	TargetEntry *tle;
	Var		   *v;

	vexec_sort_plan_decode(cscan, &plan);
	plan.bound_attno = 0;
	plan.bound_before_quals = false;
	if (vexec_enable_running_bound && plan.bound > 0 && plan.bound <= VEXEC_BOUND_ROWS &&
		child != NULL && IsA(child, CustomScan) &&
		(((CustomScan *) child)->methods == vexec_scan_methods() ||
		 ((CustomScan *) child)->methods == vexec_bitmapscan_methods()) &&
		((Scan *) child)->scanrelid != 0 &&
		linitial_int(plan.keycols) <= list_length(child->targetlist))
	{
		scan = (CustomScan *) child;
		tle = list_nth_node(TargetEntry, child->targetlist, linitial_int(plan.keycols) - 1);
		v = (Var *) tle->expr;
		if (IsA(v, Var) && v->varno == scan->scan.scanrelid && v->varattno > 0 &&
			v->varlevelsup == 0 && !may_raise_walker((Node *) child->targetlist, NULL))
		{
			plan.bound_attno = v->varattno;
			plan.bound_before_quals = !bound_after_empty_text(scan, v) &&
				!may_raise_walker((Node *) child->qual, NULL) &&
				!may_raise_walker((Node *) scan->custom_exprs, NULL);
		}
	}
	cscan->custom_private = vexec_sort_plan_encode(&plan);
	return plan.bound_attno > 0;
}

/* ---------------------------------------------------------------------
 * ORCA's Sort, and the Limit above it
 * ---------------------------------------------------------------------
 */

/* Whether ORCA's Sort can be a VecSort, and why not. */
const char *
vexec_orca_sort_refusal(Sort *sort)
{
	if (sort->plan.lefttree == NULL)
		return "no input";
	if (sort->numCols <= 0)
		return "no sort key";
	if (sort->plan.qual != NIL)
		return "a Sort with conditions";
	return NULL;
}

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
 * VecSort in place of a Sort ORCA's translator built: the same keys over the
 * same child, its plan node id, costs and parameters kept; its target list,
 * which reads the child's columns as OUTER_VAR, read over the scan tuple,
 * which is the child's row and whose columns are references to the child's.
 */
Plan *
vexec_build_sort_from_sort(Sort *sort)
{
	CustomScan *cscan = makeNode(CustomScan);
	VexecSortPlan plan;
	List	   *scan_tlist = NIL;
	ListCell   *lc;
	int			i;

	memset(&plan, 0, sizeof(plan));
	for (i = 0; i < sort->numCols; i++)
	{
		plan.keycols = lappend_int(plan.keycols, sort->sortColIdx[i]);
		plan.operators = lappend_oid(plan.operators, sort->sortOperators[i]);
		plan.collations = lappend_oid(plan.collations, sort->collations[i]);
		plan.nullsfirst = lappend_int(plan.nullsfirst, sort->nullsFirst[i] ? 1 : 0);
	}
	/* a Limit above may bound it yet (vexec_sort_set_bound()): the node then keeps tuplesort's */
	plan.device = sort_device(NULL, sort, 0);
	foreach(lc, sort->plan.lefttree->targetlist)
	{
		Var		   *v = makeVarFromTargetEntry(OUTER_VAR, lfirst_node(TargetEntry, lc));

		v->varnosyn = 0;
		v->varattnosyn = 0;
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) v, list_length(scan_tlist) + 1,
														 NULL, false));
	}

	cscan->scan.plan = sort->plan;
	cscan->scan.plan.type = T_CustomScan;
	cscan->scan.plan.targetlist = (List *)
		outer_to_index_mutator((Node *) sort->plan.targetlist, NULL);
	cscan->scan.scanrelid = 0;
	cscan->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	cscan->custom_scan_tlist = scan_tlist;
	cscan->custom_private = vexec_sort_plan_encode(&plan);
	cscan->methods = vexec_sort_methods();
	return &cscan->scan.plan;
}

/* An expression over the scan tuple, as OUTER_VAR over the child, again. */
static Node *
index_to_outer_mutator(Node *node, void *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var) && ((Var *) node)->varno == INDEX_VAR)
	{
		Var		   *v = copyObject((Var *) node);

		v->varno = OUTER_VAR;
		return (Node *) v;
	}
	return expression_tree_mutator(node, index_to_outer_mutator, context);
}

/*
 * The Sort a VecSort was made of from ORCA's: for a parent that marks and
 * restores it, which a vector node does not do (orca.c).
 */
Plan *
vexec_unbuild_sort(CustomScan *cscan)
{
	Sort	   *sort = makeNode(Sort);
	VexecSortPlan plan;
	int			n;
	int			i;

	vexec_sort_plan_decode(cscan, &plan);
	n = list_length(plan.keycols);
	sort->plan = cscan->scan.plan;
	sort->plan.type = T_Sort;
	sort->plan.targetlist = (List *) index_to_outer_mutator((Node *) cscan->scan.plan.targetlist,
															NULL);
	sort->numCols = n;
	sort->sortColIdx = palloc(sizeof(AttrNumber) * n);
	sort->sortOperators = palloc(sizeof(Oid) * n);
	sort->collations = palloc(sizeof(Oid) * n);
	sort->nullsFirst = palloc(sizeof(bool) * n);
	for (i = 0; i < n; i++)
	{
		sort->sortColIdx[i] = (AttrNumber) list_nth_int(plan.keycols, i);
		sort->sortOperators[i] = list_nth_oid(plan.operators, i);
		sort->collations[i] = list_nth_oid(plan.collations, i);
		sort->nullsFirst[i] = list_nth_int(plan.nullsfirst, i) != 0;
	}
	return &sort->plan;
}

/*
 * What a VecSort stands for, to a pass over the finished plan that looks at
 * nodes by their kind (gp_orca_vec.h, describe_node; gp_core's
 * bound_gathers(), which sends a nearest-neighbour search's ORDER BY and
 * LIMIT to the segments through the sort below a Limit): a Sort of its keys
 * whose target list is its child's row, a column an OUTER_VAR Var, which the
 * keys index, and whose lefttree is the node's own child, through which the
 * pass may change it.  The node's own target list is its projection, the
 * query's final target on PostgreSQL's planner's route.  Read, never put in
 * the plan.
 */
Sort *
vexec_sort_describe(CustomScan *cscan)
{
	Sort	   *sort = makeNode(Sort);
	Plan	   *child = cscan->scan.plan.lefttree;
	VexecSortPlan plan;
	ListCell   *lc;
	int			n;
	int			i;

	vexec_sort_plan_decode(cscan, &plan);
	n = list_length(plan.keycols);
	sort->plan = cscan->scan.plan;
	sort->plan.type = T_Sort;
	sort->plan.qual = NIL;
	sort->plan.targetlist = NIL;
	sort->plan.lefttree = child;
	sort->plan.righttree = NULL;
	if (child != NULL)
	{
		foreach(lc, child->targetlist)
		{
			TargetEntry *tle = lfirst_node(TargetEntry, lc);

			sort->plan.targetlist =
				lappend(sort->plan.targetlist,
						makeTargetEntry((Expr *) makeVar(OUTER_VAR, tle->resno,
														 exprType((Node *) tle->expr),
														 exprTypmod((Node *) tle->expr),
														 exprCollation((Node *) tle->expr), 0),
										tle->resno, NULL, false));
		}
	}
	sort->numCols = n;
	sort->sortColIdx = palloc(sizeof(AttrNumber) * n);
	sort->sortOperators = palloc(sizeof(Oid) * n);
	sort->collations = palloc(sizeof(Oid) * n);
	sort->nullsFirst = palloc(sizeof(bool) * n);
	for (i = 0; i < n; i++)
	{
		sort->sortColIdx[i] = (AttrNumber) list_nth_int(plan.keycols, i);
		sort->sortOperators[i] = list_nth_oid(plan.operators, i);
		sort->collations[i] = list_nth_oid(plan.collations, i);
		sort->nullsFirst[i] = list_nth_int(plan.nullsfirst, i) != 0;
	}
	return sort;
}

/*
 * A Limit ORCA's translator built: where its count and offset are
 * constants, and a VecSort is below it, directly or under projections that
 * pass every row on (a Result of no qual and no set-returning function, or
 * a VecResult of the same), the VecSort is bounded at their sum, as
 * ExecSetTupleBound() bounds a Sort (execProcnode.c).  The Limit stays.
 */
void
vexec_orca_limit_bound(Limit *limit)
{
	Plan	   *below = limit->plan.lefttree;
	Const	   *count = (Const *) limit->limitCount;
	int64		offset = 0;
	int64		n;

	if (count == NULL || !IsA(count, Const) || count->constisnull ||
		limit->limitOption != LIMIT_OPTION_COUNT)
		return;
	n = DatumGetInt64(count->constvalue);
	if (n <= 0)
		return;
	if (limit->limitOffset != NULL)
	{
		Const	   *off = (Const *) limit->limitOffset;

		if (!IsA(off, Const))
			return;
		if (!off->constisnull)
			offset = Max(DatumGetInt64(off->constvalue), 0);
	}
	if (n > PG_INT32_MAX - offset)
		return;

	while (below != NULL)
	{
		if (IsA(below, Result) && below->qual == NIL &&
			((Result *) below)->resconstantqual == NULL &&
			!expression_returns_set((Node *) below->targetlist))
			below = below->lefttree;
		else if (IsA(below, CustomScan) &&
				 ((CustomScan *) below)->methods == vexec_result_methods() &&
				 ((CustomScan *) below)->custom_plans == NIL &&	/* no Vec Append */
				 below->qual == NIL &&
				 !expression_returns_set((Node *) below->targetlist))
			below = below->lefttree;
		else
			break;
	}
	if (below != NULL && IsA(below, CustomScan) &&
		((CustomScan *) below)->methods == vexec_sort_methods())
	{
		vexec_sort_set_bound((CustomScan *) below, n + offset);
		(void) vexec_sort_make_late((CustomScan *) below, true, false);
		(void) vexec_sort_plan_running_bound((CustomScan *) below);
	}
}
