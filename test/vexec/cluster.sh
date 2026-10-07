#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The cluster leg (pg_vector_executor.md §5 V0 and V1, §6.1, §6.8): a
# coordinator and VEXEC_SEGMENTS segments of the port in one container, as
# the port's suites make one, with vexec in every node's
# shared_preload_libraries beside the port's modules.  On it:
#
#   - vexec loads on every node, and its settings are each node's;
#   - a session's vexec settings reach the segments: set only in the
#     session on the coordinator, never in a segment's configuration, they
#     are what a segment's session sees (gp_core's list, §3.10);
#   - a workload over distributed tables of each storage -- heap, ao_column,
#     PAX porc and porc_vec -- answers alike with vexec.mode off, explain,
#     force in the PostgreSQL format, force in the Arrow format and force
#     with the per-structure layouts drawn at random, under
#     ORCA and under the planner's route;
#   - under ORCA, the fragments carry vector scans to every segment:
#     EXPLAIN ANALYZE shows them executed below the Gather Motion, each with
#     the rows the segments read, and EXPLAIN (VEXEC) the alternatives
#     gp_orca's API offered (§3.3.4);
#   - on the gather route, each segment plans its own SQL with vexec's path
#     hooks: a statement a segment runs builds vector scans there, with the
#     session's settings, as vexec.debug_require_vector proves;
#   - under ORCA, hash joins run on the segments as VecHashJoins, over the
#     Motions that bring each segment its rows, each segment with rows of
#     its own; a VecHashJoin whose build side is empty reads its probe
#     side's Motions to their end, so that their senders stop waiting, and
#     the statement ends -- under an Append too, whose next branch needs
#     the same senders (§3.8, §6.5);
#   - V4: under ORCA with gp.enable_parallel, M8's Gathers in each
#     segment's fragments over vector nodes -- a partial VecAgg and a
#     VecHashJoin over parallel-aware VecScans, which share each segment's
#     share of the table among its workers -- and ORCA's sorts as VecSorts,
#     bounded by the Limit above them, their columns fetched late by TID;
#     and every query of the workload answered alike with workers;
#   - V5: under ORCA, its hashed window as a VecWindowHashAgg on every
#     segment, over the Redistribute Motion that brings each segment the
#     partitions it computes, under PostgreSQL's WindowAgg, answering as
#     ORCA's sorted window; and the workload in auto mode, ORCA's search
#     priced with vexec's nodes (CCostModelVec), its plans' vector nodes
#     built where it priced them, answering as off; and ORCA's two-stage
#     aggregation over a filtered VecScan, which a lower bound of its
#     partial plans too high had it prune;
#   - the batch layer in a coordinator's backend over rows gathered from the
#     segments: vexec_test's round trips and export check.
#
#   VEXEC_SEGMENTS      4
#   VEXEC_ROWS          rows of each table: 50000
#   VEXEC_INTERCONNECT  every node's gp.interconnect_type: gp_core's default,
#                       tcp; with shm, the shm module preloaded on every node
#                       after gp_core, its begins logged
#                       (gp.log_interconnect = verbose), and the suite's
#                       Motions found to have gone through its rings (V7)
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
. "$here/lib.sh"

SEGMENTS="${VEXEC_SEGMENTS:-4}"
ROWS="${VEXEC_ROWS:-50000}"
INTERCONNECT="${VEXEC_INTERCONNECT:-}"
PRELOAD="gp_core,gp_orca,gp_sql,gp_ao,pax,vexec"
[ "$INTERCONNECT" = shm ] && PRELOAD="gp_core,shm,gp_orca,gp_sql,gp_ao,pax,vexec"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/vexec-cluster-XXXXXX")"
SOCK="$(mktemp -d /tmp/vxc-XXXXXX)"
BASEPORT=$((7300 + RANDOM % 200))
SECRET="vexec-$(od -An -tx8 -N16 /dev/urandom | tr -d " \n")"	# gp_core takes 16 characters at least
NODES="$(seq 0 "$SEGMENTS")"
datadir() { echo "$ROOT/node$1"; }
sockdir() { echo "$SOCK/n$1"; }
port()    { echo $((BASEPORT + $1)); }
fail=0

cleanup() {
	for n in $NODES; do
		[ -n "${RESULTS_DIR:-}" ] && cp "$ROOT/node$n.log" "$RESULTS_DIR/cluster-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $ROOT" || rm -rf "$ROOT"
	rm -rf "$SOCK"
}
trap cleanup EXIT

cq() {						# cq <node> <db> <sql> [psql options]: -At
	local n="$1" db="$2" sql="$3"
	shift 3
	"$BINDIR/psql" -X -q -At -h "$(sockdir "$n")" -p "$(port "$n")" -U postgres -d "$db" "$@" -c "$sql" 2>&1
}
check() {					# check <what> <got> <want>
	if [ "$2" = "$3" ]; then
		echo "  ok $1"
	else
		echo "  FAILED $1: got [$2], want [$3]"
		fail=1
	fi
}

echo "the cluster leg: a coordinator and $SEGMENTS segments, $PRELOAD on every node${INTERCONNECT:+, gp.interconnect_type = $INTERCONNECT}"
CONF="$ROOT/gp_cluster.conf"
for n in $NODES; do
	echo "$((n + 1)) $((n - 1)) p $(sockdir "$n") $(port "$n") $(datadir "$n")"
done > "$CONF"
for n in $NODES; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N -U postgres --locale=C --encoding=UTF8 > "$ROOT/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n"; tail -20 "$ROOT/initdb$n.log"; exit 1; }
	{
		echo "shared_preload_libraries = '$PRELOAD'"
		echo "unix_socket_directories = '$(sockdir "$n")'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		echo "gp.cluster_config = '$CONF'"
		echo "gp.dbid = $((n + 1))"
		echo "gp.cluster_secret = '$SECRET'"
		echo "max_prepared_transactions = 100"
		echo "max_connections = 200"
		echo "shared_buffers = 128MB"
		echo "fsync = off"
		echo "max_parallel_workers_per_gather = 0"
		echo "max_worker_processes = 24"
		echo "max_parallel_workers = 16"
		[ -n "$INTERCONNECT" ] && echo "gp.interconnect_type = '$INTERCONNECT'"
		[ "$INTERCONNECT" = shm ] && echo "gp.log_interconnect = verbose"
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
	} >> "$(datadir "$n")/postgresql.auto.conf"
done
for n in $(echo $NODES | tr ' ' '\n' | sort -rn); do
	"$BINDIR/pg_ctl" -D "$(datadir "$n")" -l "$ROOT/node$n.log" -w -t 120 start > /dev/null 2>&1 \
		|| { echo "node $n did not start:"; tail -20 "$ROOT/node$n.log"; exit 1; }
done
for db in template1 postgres; do
	out=$(cq 0 "$db" "SET client_min_messages = warning; CREATE EXTENSION gp_core")
	[ -n "$out" ] && { echo "gp_core in $db: $out"; exit 1; }
done
out=$(cq 0 postgres "CREATE DATABASE vexec_cluster")
[ -n "$out" ] && { echo "CREATE DATABASE: $out"; exit 1; }
DB=vexec_cluster
out=$(cq 0 $DB "SET client_min_messages = warning; CREATE EXTENSION gp_sql; CREATE EXTENSION gp_ao; CREATE EXTENSION pax; CREATE EXTENSION vexec; CREATE EXTENSION vexec_test")
[ -n "$out" ] && { echo "the extensions: $out"; exit 1; }

# the interconnect the leg was asked for, on every node
if [ -n "$INTERCONNECT" ]; then
	for n in $NODES; do
		check "node $n has gp.interconnect_type = $INTERCONNECT" \
			"$(cq "$n" postgres "SHOW gp.interconnect_type")" "$INTERCONNECT"
	done
fi
# vexec on every node, in off mode
for n in $NODES; do
	check "node $n has vexec loaded, in off mode" \
		"$(cq "$n" postgres "SELECT current_setting('shared_preload_libraries') LIKE '%vexec%', current_setting('vexec.mode')")" "t|off"
done
check "the segments a query reaches have vexec" \
	"$(cq 0 $DB "SELECT count(*) FROM gp.exec_on_segments('SELECT current_setting(''vexec.mode'')') WHERE result = 'off'")" "$SEGMENTS"
# a session's settings on the coordinator reach the segments (§3.10)
check "a session's SET vexec.mode = force reaches every segment" \
	"$(cq 0 $DB "SET vexec.mode = force; SELECT count(*) FROM gp.exec_on_segments('SELECT current_setting(''vexec.mode'')') WHERE result = 'force'")" "$SEGMENTS"
check "and SET vexec.batch_format = arrow" \
	"$(cq 0 $DB "SET vexec.batch_format = arrow; SELECT count(*) FROM gp.exec_on_segments('SELECT current_setting(''vexec.batch_format'')') WHERE result = 'arrow'")" "$SEGMENTS"
check "and a superuser's SET vexec.debug_require_vector = on" \
	"$(cq 0 $DB "SET vexec.debug_require_vector = on; SELECT count(*) FROM gp.exec_on_segments('SELECT current_setting(''vexec.debug_require_vector'')') WHERE result = 'on'")" "$SEGMENTS"

# the tables, in each storage, distributed
cat > "$ROOT/tables.sql" <<SQL
SET client_min_messages = warning;
CREATE TABLE src AS
SELECT g AS id, g % 97 AS k, (g * 7919) % 100000 AS v, ((g % 1000) / 7.0)::numeric(12,4) AS n,
       'name ' || (g % 503) AS s, DATE '2020-01-01' + g % 1000 AS d,
       g % 3 = 0 AS b
FROM generate_series(1, $ROWS) g DISTRIBUTED BY (id);
CREATE TABLE t_heap (LIKE src) DISTRIBUTED BY (id);
CREATE TABLE t_aoco (LIKE src) USING ao_column WITH (compresstype=zstd) DISTRIBUTED BY (id);
CREATE TABLE t_porc (LIKE src) USING pax WITH (storage_format=porc) DISTRIBUTED BY (id);
CREATE TABLE t_porc_vec (LIKE src) USING pax WITH (storage_format=porc_vec) DISTRIBUTED BY (id);
INSERT INTO t_heap SELECT * FROM src;
INSERT INTO t_aoco SELECT * FROM src;
INSERT INTO t_porc SELECT * FROM src;
INSERT INTO t_porc_vec SELECT * FROM src;
ANALYZE;
SQL
out=$("$BINDIR/psql" -X -q -At -v ON_ERROR_STOP=1 -h "$(sockdir 0)" -p "$(port 0)" -U postgres -d $DB -f "$ROOT/tables.sql" 2>&1)
[ -n "$out" ] && { echo "the tables: $out"; exit 1; }

# the workload, each query over each table
QUERIES=(
	"SELECT count(*), sum(v), min(s), max(d) FROM %t"
	# H2: PAX's counts, from its statistics, under each segment's partial VecAgg
	"SELECT count(*), count(k), count(d) FROM %t"
	"SELECT k, count(*), sum(n), avg(v) FROM %t WHERE b GROUP BY k"
	"SELECT s, count(DISTINCT k) FROM %t GROUP BY s HAVING count(*) > 90"
	"SELECT a.k, count(*) FROM %t a JOIN %t b ON a.id = b.v WHERE b.k < 10 GROUP BY a.k"
	# V3: hash joins of each type, on and off the distribution key
	"SELECT a.id, b.id, b.s FROM %t a JOIN %t b ON a.k = b.k AND a.v < b.v WHERE a.id < 200 AND b.id < 300"
	"SELECT a.k, count(b.id), sum(b.v) FROM %t a LEFT JOIN %t b ON a.v = b.id AND b.b WHERE a.id % 50 = 0 GROUP BY a.k"
	"SELECT count(*), sum(a.v) FROM %t a WHERE EXISTS (SELECT 1 FROM %t b WHERE b.v = a.id AND b.k > a.k)"
	"SELECT count(*), sum(a.v) FROM %t a WHERE NOT EXISTS (SELECT 1 FROM %t b WHERE b.id = a.v AND b.d > a.d)"
	"SELECT b.id, a.id FROM %t a RIGHT JOIN %t b ON a.s = b.s AND a.id = b.k WHERE b.id < 120"
	"SELECT d, sum(v) OVER (PARTITION BY k ORDER BY id) FROM %t WHERE id % 1000 = 7"
	# V5: ORCA's hashed window, partitions on and off the distribution key
	"SELECT id, k, rank() OVER (PARTITION BY k ORDER BY v DESC, id), count(*) OVER (PARTITION BY s) FROM %t WHERE id % 13 = 0"
	"SELECT id, row_number() OVER (PARTITION BY id % 5 ORDER BY id), lag(v) OVER (PARTITION BY d ORDER BY id) FROM %t WHERE id % 17 = 0"
	"SELECT id, v FROM %t WHERE v > 99990 ORDER BY v, id LIMIT 20"
)
# force-random: the per-structure layouts drawn at random each time a node
# reads them (§6.1), from a seed the coordinator's session sends the
# segments with its other settings
SEED="${VEXEC_SEED:-$(( (RANDOM << 15 | RANDOM) % 2147483646 + 1 ))}"
echo "  random session's seed: $SEED (VEXEC_SEED=$SEED reruns it)"
SESSIONS=("off" "explain" "auto" "force-postgres" "force-arrow" "force-random" "force-parallel" "force-rows")
# with workers (V4): under ORCA M8's Gathers in the segments' fragments,
# whose parallel.c weighs them in PostgreSQL's units; on the gather route
# the segments' own plans
PARALLEL_SETS="SET gp.enable_parallel = on; SET max_parallel_workers_per_gather = 2; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;"
session_sets() {
	case "$1" in
		off) echo "SET vexec.mode = off;" ;;
		explain) echo "SET vexec.mode = explain;" ;;
		auto) echo "SET vexec.mode = auto;" ;;
		force-postgres) echo "SET vexec.mode = force; SET vexec.batch_format = postgres;" ;;
		force-arrow) echo "SET vexec.mode = force; SET vexec.batch_format = arrow;" ;;
		force-random) echo "SET vexec.mode = force; SET vexec.debug_layout_seed = $SEED;" ;;
		force-parallel) echo "SET vexec.mode = force; $PARALLEL_SETS" ;;
		# V7: the Motions carry rows, as before frames (§3.10, step C)
		force-rows) echo "SET vexec.mode = force; SET vexec.enable_motion_frames = off;" ;;
	esac
}
nq=0
nsame=0
for optimizer in on off; do
	for t in t_heap t_aoco t_porc t_porc_vec; do
		for qt in "${QUERIES[@]}"; do
			sql="${qt//%t/$t}"
			ref=""
			for s in "${SESSIONS[@]}"; do
				got=$(cq 0 $DB "SET gp.optimizer = $optimizer; $(session_sets "$s") $sql" | sort)
				if [ "$s" = off ]; then
					ref="$got"
				elif [ "$got" != "$ref" ]; then
					echo "  FAILED: $s differs from off under gp.optimizer = $optimizer: $sql"
					diff <(echo "$ref") <(echo "$got") | head -10
					fail=1
					continue
				fi
				nq=$((nq + 1))
				[ "$s" != off ] && nsame=$((nsame + 1))
			done
		done
	done
done
echo "  $nq queries over the four storages, under ORCA and the planner: $nsame answered as in off mode"

# under ORCA: the fragments' vector scans, executed on every segment, each
# reading through its storage's batch source
for t in t_heap t_aoco t_porc t_porc_vec; do
	case "$t" in
		t_aoco) want="gp_ao" ;;
		t_porc|t_porc_vec) want="pax" ;;
		*) want="heap's pages" ;;
	esac
	plan=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT k, v FROM $t WHERE v > 1000")
	if echo "$plan" | grep -q "Gather Motion" && echo "$plan" | grep -q "Vec Seq Scan on public.$t (actual rows=[1-9]" \
		&& echo "$plan" | grep -q "GPORCA" && echo "$plan" | grep -q "Source: $want"; then
		echo "  ok ORCA's fragment runs a vector scan of $t on the segments, through $want"
	else
		echo "  FAILED ORCA's plan of $t has no vector scan executed below its Gather Motion:"
		echo "$plan" | sed 's/^/    /'
		fail=1
	fi
done
# each segment ran its part of the vector scan, with rows of its own
# (§6.8): EXPLAIN ANALYZE's per-segment figures, gp.enable_explain_allstat's
# "allstat: seg_firststart_total_ntuples/seg0_<ms>_<ms>_<rows>/..."
# -- with the Motions carrying rows: with frames (V7), the scan's batches go
# to VecMotionSend, a vector parent, whose rows gp_core counts instead
for t in t_heap t_aoco t_porc t_porc_vec; do
	out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET gp.enable_explain_allstat = on; SET vexec.enable_motion_frames = off;
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT k, v FROM $t WHERE v > 1000" |
		awk -v t="$t" 'index($0, "Vec Seq Scan on " t) { found = 1 }
			found && /allstat:/ { sub(/.*allstat: /, ""); n = split($0, e, "/"); c = 0
				for (i = 2; i <= n; i++) { k = split(e[i], f, "_"); if (f[k] + 0 > 0) c++ }
				print c; exit }')
	check "each of the $SEGMENTS segments ran the vector scan of $t, with rows of its own" "$out" "$SEGMENTS"
	out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET gp.enable_explain_allstat = on;
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT k, v FROM $t WHERE v > 1000" |
		awk '/Vec Motion Send/ { found = 1 }
			found && /allstat:/ { sub(/.*allstat: /, ""); n = split($0, e, "/"); c = 0
				for (i = 2; i <= n; i++) { k = split(e[i], f, "_"); if (f[k] + 0 > 0) c++ }
				print c; exit }')
	check "V7: each of the $SEGMENTS segments sent its vector scan of $t's batches as frames" "$out" "$SEGMENTS"
done
# under ORCA: aggregation in two stages (§3.10), a partial VecAgg on every
# segment below the Motion, the final one after it -- on the segments past a
# Redistribute for a grouping, on the coordinator past the Gather for none
for t in t_heap t_aoco t_porc t_porc_vec; do
	for q in "SELECT k, count(*), sum(n), avg(v) FROM $t GROUP BY k" "SELECT count(*), sum(n), max(v) FROM $t"; do
		plan=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET gp.optimizer_force_multistage_agg = on;
			EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $q")
		if echo "$plan" | grep -q "GPORCA" && echo "$plan" | grep -q -E "Vec Partial (Hash)?Aggregate \(actual rows=[1-9]" \
			&& echo "$plan" | grep -q -E "Vec Finalize (Hash)?Aggregate"; then
			echo "  ok ORCA aggregates $t in two VecAgg stages: $(echo "$q" | cut -c1-40)..."
		else
			echo "  FAILED ORCA's plan of $q has no partial and final VecAgg:"
			echo "$plan" | sed 's/^/    /'
			fail=1
		fi
	done
	out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET gp.optimizer_force_multistage_agg = on; SET gp.enable_explain_allstat = on; SET vexec.enable_motion_frames = off;
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*), sum(n) FROM $t" |
		awk '/Vec Partial Aggregate/ { found = 1 }
			found && /allstat:/ { sub(/.*allstat: /, ""); n = split($0, e, "/"); c = 0
				for (i = 2; i <= n; i++) { k = split(e[i], f, "_"); if (f[k] + 0 > 0) c++ }
				print c; exit }')
	check "each of the $SEGMENTS segments ran the partial VecAgg of $t" "$out" "$SEGMENTS"
done
# under ORCA: hash joins as VecHashJoins on every segment, over the Motions
# that bring each segment the rows of its keys (§3.10)
for t in t_heap t_aoco t_porc t_porc_vec; do
	q="SELECT a.k, count(*), sum(b.v) FROM $t a JOIN $t b ON a.v = b.id GROUP BY a.k"
	plan=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET statement_timeout = '120s';
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $q")
	if echo "$plan" | grep -q "GPORCA" && echo "$plan" | grep -q -E "Vec Hash Join \(actual rows=[1-9]" \
		&& echo "$plan" | grep -q "Motion"; then
		echo "  ok ORCA joins $t with itself in a VecHashJoin on the segments, over a Motion"
	else
		echo "  FAILED ORCA's plan of $q has no VecHashJoin executed over a Motion:"
		echo "$plan" | sed 's/^/    /'
		fail=1
	fi
	# allstat lists the segments where a node first started, which gp_core
	# learns from the node's ExecProcNode: a node whose rows a parent reads,
	# not one a vector parent reads by batches
	out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET gp.enable_explain_allstat = on; SET vexec.enable_motion_frames = off;
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT a.k, b.v FROM $t a JOIN $t b ON a.v = b.id" |
		awk '/Vec Hash Join/ { found = 1 }
			found && /allstat:/ { sub(/.*allstat: /, ""); n = split($0, e, "/"); c = 0
				for (i = 2; i <= n; i++) { k = split(e[i], f, "_"); if (f[k] + 0 > 0) c++ }
				print c; exit }')
	check "each of the $SEGMENTS segments ran the VecHashJoin of $t, with rows of its own" "$out" "$SEGMENTS"
done
# an empty build side: the probe side's Motions stopped, and the statement
# ends, under an Append whose next branch needs the same senders.
# Broadcasts off, both sides are redistributed: the probe side receives a
# Motion that its VecHashJoin, its build side empty, reads no more.  From V7
# gp_core squelches the join's subtree, as it would a HashJoin's, in a
# fragment whose slices stream (GpCoreApi.squelch_subtree) -- with the
# Motions carrying frames or rows -- and the coordinator, where the node
# never ran, prints what each segment kept of its own figures, as gp_core
# brings them back (GpCoreApi.explain_register): its subtree squelched on
# every segment, no Motion read to its end, and none of the figures only
# the process that ran it has, such as its inner rows.
for t in t_heap t_aoco t_porc_vec; do
	q="SELECT count(*) FROM (SELECT a.k FROM $t a JOIN (SELECT * FROM t_heap WHERE v < 0) e ON a.v = e.k
		UNION ALL SELECT k FROM $t WHERE v > 99000) u"
	sets="SET gp.optimizer = on; SET gp.optimizer_enable_motion_broadcast = off; SET statement_timeout = '120s';"
	want=$(cq 0 $DB "$sets SET vexec.mode = off; $q")
	for frames in on off; do
		got=$(cq 0 $DB "$sets SET vexec.mode = force; SET vexec.enable_motion_frames = $frames; $q")
		check "an empty build side over a Motion of $t, frames $frames, under an Append: the statement ends, answering as off" "$got" "$want"
		plan=$(cq 0 $DB "$sets SET vexec.mode = force; SET vexec.enable_motion_frames = $frames;
			EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*) FROM $t a JOIN (SELECT * FROM t_heap WHERE v < 0) e ON a.v = e.k")
		if echo "$plan" | grep -q "Vec Hash Join (actual rows=0" && echo "$plan" | grep -q "Squelched: $SEGMENTS" &&
			! echo "$plan" | grep -q "Drained Motions" && ! echo "$plan" | grep -q "Inner Rows:"; then
			echo "  ok an empty build side's VecHashJoin of $t, frames $frames: gp_core squelched its probe side on every segment"
		else
			echo "  FAILED the empty build side's VecHashJoin of $t, frames $frames, is not squelched on every segment:"
			echo "$plan" | sed 's/^/    /'
			fail=1
		fi
	done
done

# V4, M8: under ORCA with workers, each segment's writer runs its fragment's
# split subtree in a Gather: the partial aggregation in three stages, the
# partial VecAgg over a parallel VecScan in each participant, which reads its
# share of the segment's table through the storage's source; a co-located
# join, its inner side read whole by each participant, as a VecHashJoin --
# in the fragment the Gather Motion to the coordinator receives, the
# writer's: a fragment below a Redistribute is a reader's, which leads no
# workers (pg19/orca/parallel.c)
for t in t_heap t_aoco t_porc t_porc_vec; do
	plan=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; $PARALLEL_SETS
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*), sum(v), sum(n) FROM $t")
	if echo "$plan" | grep -q "GPORCA" && echo "$plan" | grep -q -E "Gather \(actual" &&
		echo "$plan" | grep -q -E "Workers Launched: [1-9]" &&
		echo "$plan" | grep -q -E "Vec Partial Aggregate \(actual rows=[1-9]" &&
		echo "$plan" | grep -q -E "Parallel Vec Seq Scan on $t \(actual rows=[1-9]"; then
		echo "  ok with workers, each segment aggregates $t below a Gather over a parallel VecScan"
	else
		echo "  FAILED with workers, ORCA's plan of $t has no Gather over a parallel VecScan:"
		echo "$plan" | sed 's/^/    /'
		fail=1
	fi
	plan=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; $PARALLEL_SETS
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*), sum(b.v) FROM $t a JOIN $t b ON a.id = b.id")
	if echo "$plan" | grep -q "GPORCA" && echo "$plan" | grep -q -E "Workers Launched: [1-9]" &&
		echo "$plan" | grep -q -E "Vec Hash Join \(actual rows=[1-9]" &&
		echo "$plan" | grep -q -E "Parallel Vec Seq Scan on $t"; then
		echo "  ok with workers, each segment joins $t in a VecHashJoin over a parallel VecScan, below a Gather"
	else
		echo "  FAILED with workers, ORCA's join of $t has no VecHashJoin over a parallel VecScan below a Gather:"
		echo "$plan" | sed 's/^/    /'
		fail=1
	fi
	for q in "SELECT count(*), sum(v), sum(n) FROM $t" "SELECT a.k, count(*), sum(b.v) FROM $t a JOIN $t b ON a.id = b.id GROUP BY a.k" \
		"SELECT count(*), sum(b.v) FROM $t a JOIN $t b ON a.id = b.id"; do
		want=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = off; $q" | sort)
		got=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; $PARALLEL_SETS $q" | sort)
		check "with workers, $(echo "$q" | cut -c1-48)... answers as off" "$got" "$want"
	done
done
# V4: ORCA's sorts as VecSorts on the segments, bounded by the Limit above
# them, the other columns of the rows each keeps fetched by TID
for t in t_heap t_aoco t_porc t_porc_vec; do
	q="SELECT id, v, s, d FROM $t WHERE k < 50 ORDER BY v DESC, id LIMIT 15"
	plan=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF) $q")
	if echo "$plan" | grep -q "GPORCA" && echo "$plan" | grep -q -E "Vec Sort \(actual rows=[1-9]" &&
		echo "$plan" | grep -q "Bound: 15" && echo "$plan" | grep -q "fetched by TID"; then
		echo "  ok ORCA sorts $t in a bounded VecSort on the segments, its columns fetched late"
	else
		echo "  FAILED ORCA's plan of $q has no bounded VecSort of late columns:"
		echo "$plan" | sed 's/^/    /'
		fail=1
	fi
	check "ORCA's bounded VecSort of $t answers as off" \
		"$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; $q")" \
		"$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = off; $q")"
done

# V5: ORCA's hashed window as a VecWindowHashAgg on every segment, over the
# Redistribute Motion that brings each its partitions, under PostgreSQL's
# WindowAgg; in auto mode too, which ORCA's search chose with its price --
# its input of 7,142 rows under vexec.min_rows otherwise, below which no
# node of vexec's is priced or built
for t in t_heap t_aoco t_porc t_porc_vec; do
	q="SELECT id, k, rank() OVER (PARTITION BY k ORDER BY v DESC, id), sum(n) OVER (PARTITION BY k) FROM $t WHERE id % 7 = 0"
	for mode in force auto; do
		plan=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = $mode; SET vexec.min_rows = 1000; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) $q")
		if echo "$plan" | grep -q "GPORCA" && echo "$plan" | grep -q -E "Vec Window Hash Agg \(actual rows=[1-9]" &&
			echo "$plan" | grep -q "Redistribute Motion" && echo "$plan" | grep -q "Gather Motion"; then
			echo "  ok ORCA's hashed window over $t, $mode mode: a VecWindowHashAgg on the segments, over a Redistribute Motion"
		else
			echo "  FAILED ORCA's plan of $q, $mode mode, has no VecWindowHashAgg on the segments:"
			echo "$plan" | sed 's/^/    /'
			fail=1
		fi
	done
	check "ORCA's hashed window over $t answers as its sorted window" \
		"$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; $q" | sort)" \
		"$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = off; $q" | sort)"
done
# V5: auto mode's vector nodes, built where ORCA's search priced them
explain=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = auto; EXPLAIN (VEXEC, COSTS OFF) SELECT k, count(*) FROM t_aoco GROUP BY k")
if echo "$explain" | grep -q "built (priced in ORCA's search)" && echo "$explain" | grep -q "GPORCA"; then
	echo "  ok in auto mode ORCA's search prices vexec's nodes, which its plan has where it priced them"
else
	echo "  FAILED ORCA's EXPLAIN (VEXEC) in auto mode:"; echo "$explain" | sed 's/^/    /'; fail=1
fi
# V5: ORCA's partial plans -- an operator priced before its children have
# plans, a lower bound by which ORCA prunes the alternatives it has not
# priced -- price a filter over a VecScan at the lesser of its two prices.
# Priced as a row node there, its bound passed its plan's price, and ORCA
# pruned the two-stage aggregation over the scan: it gathered every row the
# filter passed to the coordinator, or redistributed them at random below
# the partial aggregation
out=$(cq 0 $DB "CREATE TABLE t_lb (id int, q int, v numeric(7,2)) DISTRIBUTED BY (id);
INSERT INTO t_lb SELECT g, 1 + (g * 7) % 100, (g % 1000) / 10.0 FROM generate_series(1, 2000000) g;
ANALYZE t_lb")
case "$out" in *ERROR*) echo "  FAILED t_lb: $out"; fail=1 ;; esac
for q in "SELECT count(*) FROM t_lb WHERE q BETWEEN 1 AND 20" "SELECT count(*), avg(v) FROM t_lb WHERE q BETWEEN 1 AND 20"; do
	for mode in auto force; do
		plan=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = $mode; EXPLAIN (COSTS OFF) $q")
		if echo "$plan" | grep -A1 "Vec Partial Aggregate" | grep -q "Vec Seq Scan on t_lb" &&
			! echo "$plan" | grep -q "Redistribute Motion"; then
			echo "  ok $mode mode: ORCA aggregates $q in two stages, the partial one over the VecScan"
		else
			echo "  FAILED ORCA's plan of $q in $mode mode:"; echo "$plan" | sed 's/^/    /'; fail=1
		fi
	done
done

# the gather route: the coordinator's VecHashJoin over the rows the segments send
plan=$(cq 0 $DB "SET gp.optimizer = off; SET vexec.mode = force; EXPLAIN (COSTS OFF) SELECT a.k, count(*) FROM t_aoco a JOIN t_porc b ON a.id = b.v GROUP BY a.k")
echo "$plan" | grep -q "Vec Hash Join" && echo "  ok on the gather route, the coordinator joins in a VecHashJoin" \
	|| { echo "  FAILED the gather route's plan has no VecHashJoin:"; echo "$plan" | sed 's/^/    /'; fail=1; }
# gp_core's runtime filter on: an inner join it may take stays a Hash Join (§3.8)
plan=$(cq 0 $DB "SET gp.optimizer = off; SET vexec.mode = force; SET gp.enable_runtime_filter = on; EXPLAIN (COSTS OFF) SELECT a.k, count(*) FROM t_aoco a JOIN t_porc b ON a.id = b.v GROUP BY a.k")
if echo "$plan" | grep -q " Hash Join" && ! echo "$plan" | grep -q -E "Vec Hash [A-Za-z ]*Join"; then
	echo "  ok with gp.enable_runtime_filter on, the gather route's join stays a Hash Join$(echo "$plan" | grep -q "RuntimeFilter" && echo ", under its RuntimeFilter")"
else
	echo "  FAILED with gp.enable_runtime_filter on, the gather route's plan:"; echo "$plan" | sed 's/^/    /'; fail=1
fi
# the gather route: a nearest-neighbour search, a LIMIT over a VecSort by a
# distance GiST orders by, still sends the segments its ORDER BY and LIMIT
# (gp_core's bound_gathers(), which sees the VecSort as the Sort it stands
# for through gp_orca's API, describe_node), each segment's GiST index
# answering it, and answers as off does
out=$(cq 0 $DB "SET client_min_messages = warning;
	CREATE TABLE t_pts AS SELECT g AS id, point(g % 997, g / 997) AS p FROM generate_series(1, 30000) g DISTRIBUTED BY (id);
	CREATE INDEX t_pts_p ON t_pts USING gist (p); ANALYZE t_pts")
case "$out" in *ERROR*) echo "  FAILED the points' table: $out"; fail=1 ;; esac
q="SELECT id, p <-> point(500.3, 10.7) FROM t_pts ORDER BY p <-> point(500.3, 10.7) LIMIT 7"
plan=$(cq 0 $DB "SET gp.optimizer = off; SET vexec.mode = force; EXPLAIN (VERBOSE, COSTS OFF) $q")
if echo "$plan" | grep -q "Vec Sort" && echo "$plan" | grep -q "Remote SQL: .*ORDER BY .*LIMIT 7"; then
	echo "  ok on the gather route, a nearest-neighbour search's VecSort sends the segments its ORDER BY and LIMIT"
else
	echo "  FAILED on the gather route, the nearest-neighbour search's plan:"; echo "$plan" | sed 's/^/    /'; fail=1
fi
check "on the gather route, the nearest-neighbour search answers as off" \
	"$(cq 0 $DB "SET gp.optimizer = off; SET vexec.mode = force; $q")" \
	"$(cq 0 $DB "SET gp.optimizer = off; SET vexec.mode = off; $q")"
# and each segment plans what it is sent with its GiST index, force mode
# keeping the index's order (plan/sort.c)
out=$(cq 0 $DB "SET gp.optimizer = off; SET vexec.mode = force;
	SELECT count(*) FROM gp.exec_on_segments('EXPLAIN (COSTS OFF, FORMAT JSON) SELECT id FROM t_pts ORDER BY p <-> point(500.3, 10.7) LIMIT 7')
		WHERE result LIKE '%\"Index Scan\"%' AND result LIKE '%\"t_pts_p\"%'")
check "on the gather route, every segment answers the search with its GiST index" "$out" "$SEGMENTS"
# the gather route: the coordinator's VecAgg over the rows the segments send
plan=$(cq 0 $DB "SET gp.optimizer = off; SET vexec.mode = force; EXPLAIN (COSTS OFF) SELECT k, count(*), sum(n) FROM t_aoco GROUP BY k")
echo "$plan" | grep -q "Vec HashAggregate" && echo "  ok on the gather route, the coordinator aggregates in a VecAgg" \
	|| { echo "  FAILED the gather route's plan has no VecAgg:"; echo "$plan" | sed 's/^/    /'; fail=1; }
seg_rows=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) SELECT k FROM t_heap WHERE v > 1000" | grep -c '"Custom Plan Provider": "VecScan"')
[ "$seg_rows" -gt 0 ] && echo "  ok EXPLAIN ANALYZE's JSON names the VecScan the segments ran" \
	|| { echo "  FAILED EXPLAIN ANALYZE's JSON names no VecScan"; fail=1; }
explain=$(cq 0 $DB "SET gp.optimizer = on; SET gp.optimizer_trace_fallback = on; SET vexec.mode = explain; EXPLAIN (VEXEC, COSTS OFF) SELECT k, count(*) FROM t_aoco GROUP BY k")
if echo "$explain" | grep -q "VecScan on t_aoco: not chosen (explain mode)" && ! echo "$explain" | grep -q "Vec Seq Scan"; then
	echo "  ok ORCA's plans offer their scans to vexec through gp_orca's API, and explain mode builds none"
else
	echo "  FAILED ORCA's EXPLAIN (VEXEC) in explain mode:"
	echo "$explain" | sed 's/^/    /'
	fail=1
fi

# the gather route: each segment plans its own SQL with vexec's path hooks,
# with the session's settings; a segment that built no vector node fails
for t in t_heap t_aoco t_porc t_porc_vec; do
	out=$(cq 0 $DB "SET gp.optimizer = off; SET vexec.mode = force; SET vexec.debug_require_vector = on;
		SELECT count(*) FROM gp.exec_on_segments('SELECT count(*) FROM $t WHERE v > 1000') WHERE result::int8 > 0")
	check "on the gather route, every segment scans $t with a vector scan" "$out" "$SEGMENTS"
done
planner=$(cq 0 $DB "SET gp.optimizer = off; SET vexec.mode = explain; EXPLAIN (VEXEC, COSTS OFF) SELECT k, count(*) FROM t_aoco GROUP BY k" | grep -c 'Vec')
[ "$planner" -gt 0 ] && echo "  ok the planner's route records vector alternatives on the coordinator ($planner lines)" \
	|| { echo "  FAILED the planner's route records nothing"; fail=1; }

# V7: batches across ORCA's Motions as Arrow IPC frames (§3.10, step A)
# -- over a Redistribute (made an Explicit Redistribute), a Broadcast and a
# Gather, a vector node on each side; not over a sorted Gather
frames_plan() {				# frames_plan <sets> <query>: EXPLAIN's text
	cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; $1 EXPLAIN (COSTS OFF) $2"
}
plan=$(frames_plan "" "SELECT s, count(*) FROM t_heap GROUP BY s")
if echo "$plan" | grep -q "Explicit Redistribute Motion" && echo "$plan" | grep -q "Frames To: the segments their keys hash to" &&
	echo "$plan" | grep -q "Frames To: the one gathering" && echo "$plan" | grep -q "Vec Motion Receive"; then
	echo "  ok V7: a grouping's Redistribute and its Gather carry frames, VecMotionSend below, VecMotionRecv above"
else
	echo "  FAILED V7: no frames over the grouping's Motions:"; echo "$plan" | sed 's/^/    /'; fail=1
fi
plan=$(frames_plan "SET gp.optimizer_enable_motion_broadcast = on;" "SELECT a.id FROM t_heap a JOIN (SELECT * FROM t_aoco WHERE id < 100) b ON a.k = b.k")
echo "$plan" | grep -q "Frames To: every segment" && echo "  ok V7: a Broadcast carries frames" \
	|| { echo "  FAILED V7: no frames over the Broadcast:"; echo "$plan" | sed 's/^/    /'; fail=1; }
plan=$(frames_plan "" "SELECT id, v FROM t_heap WHERE v > 99000 ORDER BY v, id")
if echo "$plan" | grep -q "Merge Key" && ! echo "$plan" | grep -q "Frames To: the one gathering"; then
	echo "  ok V7: a sorted Gather, which merges its senders' rows, carries rows"
else
	echo "  FAILED V7: the sorted Gather:"; echo "$plan" | sed 's/^/    /'; fail=1
fi
plan=$(frames_plan "SET vexec.enable_motion_frames = off;" "SELECT s, count(*) FROM t_heap GROUP BY s")
if ! echo "$plan" | grep -q "Vec Motion" && echo "$plan" | grep -q "Redistribute Motion"; then
	echo "  ok V7: with vexec.enable_motion_frames off, the Motions carry rows"
else
	echo "  FAILED V7: frames with vexec.enable_motion_frames off:"; echo "$plan" | sed 's/^/    /'; fail=1
fi
# EXPLAIN ANALYZE: the frames each node sent and received, which the
# segments kept and gp_core brought back (GpCoreApi.explain_register)
plan=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force;
	EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT s, count(*) FROM t_porc_vec GROUP BY s")
sent=$(echo "$plan" | awk '/Vec Motion Send/ { s = 1 } s && /Frames: / { sub(/.*Frames: /, ""); print; exit }')
batches=$(echo "$plan" | awk '/Vec Partial HashAggregate/ { s = 1 } s && /Batches: / { sub(/.*Batches: /, ""); print; exit }')
if [ "${sent:-0}" -ge "$SEGMENTS" ] && [ "${batches:-0}" -gt 0 ] && echo "$plan" | grep -q "Segments Reporting: $SEGMENTS"; then
	echo "  ok V7: EXPLAIN ANALYZE prints the frames and batches the segments' vector nodes counted ($sent frames)"
else
	echo "  FAILED V7: EXPLAIN ANALYZE prints none of the segments' figures:"; echo "$plan" | sed 's/^/    /'; fail=1
fi

# V7: the switch acts from one statement to the next (§3.10): a prepared
# statement's generic plan and a PL/pgSQL function's cached plan included,
# with the same answers -- the setting's assign hook makes the session's
# cached plans be planned again
out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET plan_cache_mode = force_generic_plan;
	PREPARE p(int) AS SELECT s, count(*) FROM t_heap WHERE k < \$1 GROUP BY s;
	EXPLAIN (COSTS OFF) EXECUTE p(50);
	SET vexec.enable_motion_frames = off;
	EXPLAIN (COSTS OFF) EXECUTE p(50);
	SET vexec.enable_motion_frames = on;
	EXPLAIN (COSTS OFF) EXECUTE p(50);" | grep -c "Vec Motion Send")
on1=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET plan_cache_mode = force_generic_plan;
	PREPARE p(int) AS SELECT s, count(*) FROM t_heap WHERE k < \$1 GROUP BY s;
	EXPLAIN (COSTS OFF) EXECUTE p(50);" | grep -c "Vec Motion Send")
off1=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET plan_cache_mode = force_generic_plan;
	PREPARE p(int) AS SELECT s, count(*) FROM t_heap WHERE k < \$1 GROUP BY s;
	EXECUTE p(50); SET vexec.enable_motion_frames = off;
	EXPLAIN (COSTS OFF) EXECUTE p(50);" | grep -c "Vec Motion Send")
check "V7: a prepared statement's generic plan follows SET vexec.enable_motion_frames, off then on" "$on1|$off1|$out" "2|0|4"
answers=$(for f in on off on; do cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET vexec.enable_motion_frames = $f;
	PREPARE p(int) AS SELECT s, count(*) FROM t_heap WHERE k < \$1 GROUP BY s; EXECUTE p(50);" | sort | md5sum; done | sort -u | wc -l)
check "V7: and answers alike with frames and with rows" "$answers" "1"
cq 0 $DB "CREATE OR REPLACE FUNCTION v7_count(lim int) RETURNS bigint LANGUAGE plpgsql AS \$f\$
	DECLARE n bigint; BEGIN SELECT sum(c) INTO n FROM (SELECT s, count(*) c FROM t_heap WHERE k < lim GROUP BY s) g; RETURN n; END \$f\$" > /dev/null
out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET plan_cache_mode = force_generic_plan;
	LOAD 'auto_explain'; SET auto_explain.log_min_duration = 0; SET auto_explain.log_nested_statements = on;
	SET client_min_messages = log;
	SELECT v7_count(50); SET vexec.enable_motion_frames = off; SELECT v7_count(50);
	SET vexec.enable_motion_frames = on; SELECT v7_count(50);" 2>&1)
# auto_explain prints the coordinator's part of the plan: the receiver
fp=$(echo "$out" | awk '/Query Text: SELECT sum\(c\)/ { n++ } /Vec Motion Receive/ && n > 0 { f[n]++ } END { for (i = 1; i <= n; i++) printf "%s%d", (i > 1 ? "|" : ""), (f[i] > 0) }')
check "V7: a PL/pgSQL function's cached plan follows the setting: frames, rows, frames" "$fp" "1|0|1"

# V7: the vector cdbhash puts each row where gp_core's cdbhash puts it, for
# every key type, in both formats (§6.2): tables distributed by a key of
# each type, loaded through ORCA's Redistribute with frames, and with
# vexec off, every key on the same segment.  Not name, whose INSERT ORCA
# leaves to the planner: vexec_test's cdbhash() checks its kernel, with
# every other one, against gp_core's cdbhash row by row (below)
KEYS=("int2" "int4" "int8" "numeric(12,4)" "numeric(30,6)" "numeric" "float4" "float8" "text" "varchar(20)" "bpchar(12)" "bytea"
	"date" "timestamp" "timestamptz" "time" "interval" "bool" "uuid" "oid" "int4,text")
keyexpr() {					# keyexpr <type>: a key of the type over g, NULLs among them
	case "$1" in
		int2) echo "(g % 30000 - 15000)::int2" ;;
		int4) echo "(g * 7919 - 4000000)::int4" ;;
		int8) echo "(g::int8 * 1000000007 - 5000000000000)" ;;
		"numeric(12,4)") echo "((g % 100000) / 7.0 - 5000)::numeric(12,4)" ;;
		"numeric(30,6)") echo "(g::numeric * 1234567.891234 - 999999999.5)::numeric(30,6)" ;;
		numeric) echo "CASE g % 211 WHEN 0 THEN 'NaN'::numeric WHEN 1 THEN 'Infinity' ELSE g::numeric / 3 END" ;;
		float4) echo "CASE g % 101 WHEN 0 THEN 'NaN'::float4 WHEN 1 THEN '-0' WHEN 2 THEN 'Infinity' ELSE g * 0.37 END::float4" ;;
		float8) echo "CASE g % 101 WHEN 0 THEN 'NaN'::float8 WHEN 1 THEN '-0' WHEN 2 THEN '-Infinity' ELSE g * 0.37 END" ;;
		text) echo "'k' || g % 3000 || repeat('x', g % 20)" ;;
		"varchar(20)") echo "('v' || g % 3000)::varchar(20)" ;;
		"bpchar(12)") echo "('b' || g % 500)::bpchar(12)" ;;
		bytea) echo "decode(md5((g % 4000)::text), 'hex')" ;;
		date) echo "CASE g % 997 WHEN 0 THEN 'infinity'::date ELSE DATE '2000-01-01' + (g % 20000 - 10000) END" ;;
		timestamp) echo "CASE g % 997 WHEN 0 THEN '-infinity'::timestamp ELSE TIMESTAMP '2000-01-01' + g * interval '17 minutes' END" ;;
		timestamptz) echo "TIMESTAMPTZ '1999-12-31 23:00+00' + g * interval '1 hour 1 second'" ;;
		time) echo "TIME '00:00' + (g % 1440) * interval '1 minute'" ;;
		interval) echo "g * interval '1 hour 3 minutes'" ;;
		bool) echo "g % 3 = 0" ;;
		uuid) echo "md5((g % 5000)::text)::uuid" ;;
		oid) echo "(g % 100000)::oid" ;;
		"int4,text") echo "g % 1000, 't' || g % 77" ;;
	esac
}
for f in postgres arrow; do
	bad=""
	for kt in "${KEYS[@]}"; do
		cols="k"
		defs="k $kt"
		if [ "$kt" = "int4,text" ]; then cols="k, k2"; defs="k int4, k2 text"; fi
		tn="dk_$(echo "$kt" | tr -c 'a-z0-9' '_' | sed 's/_*$//')"
		out=$(cq 0 $DB "SET client_min_messages = warning; DROP TABLE IF EXISTS ${tn}_r, ${tn}_f;
			CREATE TABLE ${tn}_r ($defs) DISTRIBUTED BY ($cols); CREATE TABLE ${tn}_f ($defs) DISTRIBUTED BY ($cols);
			SET gp.optimizer = on;
			SET vexec.mode = off; INSERT INTO ${tn}_r SELECT $(keyexpr "$kt") FROM (SELECT CASE WHEN id % 53 = 0 THEN NULL ELSE id END AS g FROM src) x;
			SET vexec.mode = force; SET vexec.batch_format = $f;
			INSERT INTO ${tn}_f SELECT $(keyexpr "$kt") FROM (SELECT CASE WHEN id % 53 = 0 THEN NULL ELSE id END AS g FROM src) x;
			SELECT (SELECT count(*) FROM ${tn}_f) || '|' ||
				(SELECT count(*) FROM ((SELECT $cols, gp_segment_id FROM ${tn}_r EXCEPT ALL SELECT $cols, gp_segment_id FROM ${tn}_f)
				 UNION ALL (SELECT $cols, gp_segment_id FROM ${tn}_f EXCEPT ALL SELECT $cols, gp_segment_id FROM ${tn}_r)) d)" 2>&1 | tail -1)
		plan=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET vexec.batch_format = $f;
			EXPLAIN (COSTS OFF) INSERT INTO ${tn}_f SELECT $(keyexpr "$kt") FROM (SELECT id AS g FROM src) x")
		if [ "$out" != "$ROWS|0" ] || ! echo "$plan" | grep -q "Frames To: the segments their keys hash to"; then
			bad="$bad $kt[$out$(echo "$plan" | grep -q 'Frames To' || echo ', no frames')]"
		fi
	done
	check "V7: the vector cdbhash puts every key, ${#KEYS[@]} types with NULLs, -0, NaN and the infinities, where gp_core's does, $f format" "${bad:- none differ}" " none differ"
done

# V7: the vector cdbhash's kernels against gp_core's own cdbhash, row by
# row (vexec_test.cdbhash()), for every key type -- name too -- and keys of
# several columns, over segment counts that are and are not powers of two,
# in both formats and with the per-structure layouts' other choices: the
# kernels read every layout a key's column can have
CONFIGS=("postgres format format format format" "arrow format format format format"
	"postgres view format format format" "arrow offsets byte postgres format"
	"postgres format bit arrow varlena" "arrow datum format format varlena")
bad=""
for cfg in "${CONFIGS[@]}"; do
	read -r f vl bo te nu <<< "$cfg"
	for kt in "${KEYS[@]}" name "int8,date,text"; do
		case "$kt" in
			name) ke="('n' || g % 700)::name" ;;
			"int8,date,text") ke="g::int8 * 31, DATE '2001-01-01' + g % 4000, 'z' || g % 101" ;;
			*) ke="$(keyexpr "$kt")" ;;
		esac
		out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force;
			SELECT coalesce(string_agg(n || ':' || h.rows || '/' || h.mismatches, ' '), 'same')
			FROM unnest(ARRAY[1, 2, 3, 4, 5, 7, 16]) n,
			     LATERAL vexec_test.cdbhash(\$q\$SELECT $ke FROM (SELECT CASE WHEN id % 53 = 0 THEN NULL ELSE id END AS g FROM src WHERE id <= 20000) x\$q\$,
			                                n, '$f', '$vl', '$bo', '$te', '$nu') h
			WHERE h.mismatches <> 0 OR h.rows <> 20000")
		[ "$out" = same ] || bad="$bad [$cfg] $kt: $out;"
	done
done
check "V7: the vector cdbhash gives gp_core's segment to every row, $((${#KEYS[@]} + 2)) key types in ${#CONFIGS[@]} layouts, 1 to 16 segments" "${bad:- none differ}" " none differ"

# V7: and every key type of the layouts' semantics corpus (§6.2), each of
# its columns a key on its own where its type has a hash operator class,
# with its NULLs, -0, NaN, infinities, numerics of several scales, bpchar's
# padding and TOAST's values, in both formats
"$BINDIR/psql" -X -q -At -v ON_ERROR_STOP=1 -h "$(sockdir 0)" -p "$(port 0)" -U postgres -d $DB \
	-f "$here/../../modules/vexec/sql/vexec_corpus_setup.sql" > /dev/null 2>&1 || { echo "  FAILED the corpus"; fail=1; }
cq 0 $DB "CREATE FUNCTION v7_corpus_cdbhash(f text) RETURNS text LANGUAGE plpgsql AS \$f\$
DECLARE c record; r record; bad text := ''; nk int := 0; nohash text := '';
BEGIN
	FOR c IN SELECT attname, format_type(atttypid, atttypmod) AS t FROM pg_attribute
		WHERE attrelid = 'corpus'::regclass AND attnum > 0 AND NOT attisdropped ORDER BY attnum LOOP
		BEGIN
			FOR r IN SELECT n, h.mismatches, h.rows FROM unnest(ARRAY[1, 2, 3, 4, 5, 7, 16]) n,
				LATERAL vexec_test.cdbhash(format('SELECT %I FROM corpus', c.attname), n, f) h LOOP
				IF r.mismatches <> 0 OR r.rows <> 2600 THEN
					bad := bad || format(' %s/%s:%s', c.attname, r.n, r.mismatches);
				END IF;
			END LOOP;
			nk := nk + 1;
		EXCEPTION WHEN undefined_object OR internal_error THEN
			nohash := nohash || ' ' || c.t;
		END;
	END LOOP;
	RETURN nk || ' keys;' || coalesce(nullif(bad, ''), ' none differ') || '; no hash operator class:' || nohash;
END \$f\$" > /dev/null
for f in postgres arrow; do
	out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET client_min_messages = warning; SELECT v7_corpus_cdbhash('$f')")
	check "V7: the vector cdbhash gives gp_core's segment to every value of the corpus's key types, 1 to 16 segments, $f format" \
		"$out" "36 keys; none differ; no hash operator class: money json point"
done

# V7: a Redistribute ORCA hashes with the legacy cdbhash -- every table of
# the query keyed with a cdbhash_*_ops class, or placed at random -- sends
# its frames where gp_core's legacy cdbhash puts each key, the keys hashed
# row by row by gp_core (GpCoreApi.hash_segment): a join into a legacy-keyed
# table's placement finds each of its rows, in both formats.  ORCA has found
# int4's and int8's legacy families since the port's 11af2ab11f4.
out=$(cq 0 $DB "SET client_min_messages = warning;
	CREATE TABLE lg_t (key text, n int) DISTRIBUTED BY (key cdbhash_text_ops);
	CREATE TABLE lg_n (key numeric(12,4), n int) DISTRIBUTED BY (key cdbhash_numeric_ops);
	CREATE TABLE lg_i (key int4, n int) DISTRIBUTED BY (key cdbhash_int4_ops);
	CREATE TABLE lg_l (key int8, n int) DISTRIBUTED BY (key cdbhash_int8_ops);
	CREATE TABLE lg_r (id int, t text, nu numeric(12,4), i int4, l int8) DISTRIBUTED RANDOMLY;
	INSERT INTO lg_t SELECT 'k' || g, g FROM generate_series(1, 20000) g;
	INSERT INTO lg_n SELECT g / 4.0, g FROM generate_series(1, 20000) g;
	INSERT INTO lg_i SELECT g * 7, g FROM generate_series(1, 20000) g;
	INSERT INTO lg_l SELECT g * 1000003::int8, g FROM generate_series(1, 20000) g;
	INSERT INTO lg_r SELECT g, 'k' || g, g / 4.0, g * 7, g * 1000003::int8 FROM generate_series(1, 20000) g;
	ANALYZE lg_t; ANALYZE lg_n; ANALYZE lg_i; ANALYZE lg_l; ANALYZE lg_r" 2>&1)
[ -n "$out" ] && { echo "  FAILED the legacy-keyed tables: $out"; fail=1; }
bad=""
for lk in lg_t:t lg_n:nu lg_i:i lg_l:l; do
	q="SELECT count(*) FROM lg_r r JOIN ${lk%:*} x ON x.key = r.${lk#*:}"
	for f in postgres arrow; do
		sets="SET gp.optimizer = on; SET vexec.mode = force; SET vexec.batch_format = $f; SET gp.optimizer_enable_motion_broadcast = off;"
		plan=$(cq 0 $DB "$sets EXPLAIN (VERBOSE, COSTS OFF) $q")
		echo "$plan" | grep -q "Hash: legacy, row by row" && echo "$plan" | grep -q "Optimizer: GPORCA" \
			|| bad="$bad ${lk%:*}/$f[no legacy frames]"
		got=$(cq 0 $DB "$sets $q")
		[ "$got" = 20000 ] || bad="$bad ${lk%:*}/$f[$got]"
	done
done
check "V7: legacy-hashed Redistributes' frames -- text, numeric, int4, int8 keys -- go where gp_core's legacy cdbhash puts their rows, both formats" \
	"${bad:- every row found}" " every row found"

# V7: the rows a vector node leaves undecided -- a qual or a join filter
# PostgreSQL's evaluator is to compute, which may raise, outside its
# batch's selection (vexec_next_batch()) -- resolved by VecMotionSend below
# a Gather, a Broadcast and a Redistribute: a division by zero raises, and
# a timestamp compared with a timestamptz passes its rows, as across Motions
# that carry rows.  The port's suites found them dropped (2026-10-07), and
# each of these lost them then.
out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SELECT id FROM t_heap WHERE k / (v - v) = 9" 2>&1)
check "V7: a qual dividing by zero, its rows left to PostgreSQL's evaluator under a Gather's frames, raises" \
	"$(echo "$out" | grep -c 'division by zero')" "1"
out=$(cq 0 $DB "SET client_min_messages = warning;
	CREATE TABLE v7_ts1 (a int, b timestamp, bb timestamptz) DISTRIBUTED BY (a, b);
	CREATE TABLE v7_ts2 (c int, d timestamp, dd timestamptz) DISTRIBUTED BY (c, d);
	INSERT INTO v7_ts1 SELECT g, timestamp '2016-11-01' + g * interval '1 day', timestamptz '2016-11-01 00:00+00' + g * interval '1 day' FROM generate_series(9, 13) g;
	INSERT INTO v7_ts2 SELECT g, timestamp '2016-11-01' + g * interval '1 day', timestamptz '2016-11-01 00:00+00' + g * interval '1 day' FROM generate_series(9, 13) g;
	CREATE TABLE v7_tsb1 (a int, b timestamp, bb timestamptz) DISTRIBUTED BY (a, b);
	CREATE TABLE v7_tsb2 (c int, d timestamp, dd timestamptz) DISTRIBUTED BY (c, d);
	INSERT INTO v7_tsb1 SELECT g, timestamp '2016-11-01' + g * interval '1 hour', timestamptz '2016-11-01 00:00+00' + g * interval '1 hour' FROM generate_series(1, 3000) g;
	INSERT INTO v7_tsb2 SELECT g, timestamp '2016-11-01' + g * interval '1 hour', timestamptz '2016-11-01 00:00+00' + g * interval '1 hour' FROM generate_series(1, 3000) g;
	ANALYZE v7_ts1; ANALYZE v7_ts2; ANALYZE v7_tsb1; ANALYZE v7_tsb2" 2>&1)
[ -n "$out" ] && { echo "  FAILED the timestamp tables: $out"; fail=1; }
bad=""
for q in "SELECT a, b FROM v7_ts1 JOIN v7_ts2 ON a = c AND b = dd AND b = bb AND b = timestamp '2016-11-11'" \
	"SELECT count(*), sum(t.id) FROM (SELECT a, c FROM v7_tsb1 JOIN v7_tsb2 ON a = c AND b = dd) j JOIN t_heap t ON t.v = j.a" \
	"SELECT j.a % 10, count(*) FROM (SELECT a, c FROM v7_tsb1 JOIN v7_tsb2 ON a = c AND b = dd) j GROUP BY 1 ORDER BY 1"; do
	for f in postgres arrow; do
		sets="SET gp.optimizer = on; SET vexec.mode = force; SET vexec.batch_format = $f;"
		want=$(cq 0 $DB "$sets SET vexec.enable_motion_frames = off; $q")
		got=$(cq 0 $DB "$sets $q")
		plan=$(cq 0 $DB "$sets EXPLAIN (COSTS OFF) $q")
		[ "$got" = "$want" ] && [ -n "$(echo "$want" | grep -v '^0|$')" ] && echo "$plan" | grep -q "Vec Motion Send" \
			|| bad="$bad [$f: $q: $(echo "$got" | head -2 | tr '\n' ' ')]"
	done
done
check "V7: a join filter's rows left to PostgreSQL's evaluator, through a Gather's, a Broadcast's and a Redistribute's frames, as across rows, both formats" \
	"${bad:- all alike}" " all alike"

# V7: a numeric a kernel computes is scaled at its arithmetic's scale,
# whatever the column's typmod -- extract()'s here -- and crosses a
# Redistribute in that shape, which the receiver takes as one of numeric's
# (frame.c).  The port's suites found it refused as malformed (2026-10-07),
# and each of these was then.
bad=""
for q in "SELECT y, rank() OVER (PARTITION BY y ORDER BY id) FROM (SELECT id, extract(year FROM d) AS y FROM t_heap) s ORDER BY 2, 1 LIMIT 5" \
	"SELECT count(*), sum(a.m) FROM (SELECT extract(year FROM d) AS y, n * 3 + 1 AS m FROM t_heap) a JOIN t_heap b ON a.y = b.k"; do
	for f in postgres arrow; do
		sets="SET gp.optimizer = on; SET vexec.mode = force; SET vexec.batch_format = $f;"
		want=$(cq 0 $DB "$sets SET vexec.enable_motion_frames = off; $q")
		got=$(cq 0 $DB "$sets $q")
		plan=$(cq 0 $DB "$sets EXPLAIN (COSTS OFF) $q")
		[ "$got" = "$want" ] && echo "$plan" | grep -q "Vec Motion Send" || bad="$bad [$f: $q: $(echo "$got" | head -2 | tr '\n' ' ')]"
	done
done
check "V7: numerics scaled by kernels -- extract()'s -- cross Redistributes as frames, both formats" \
	"${bad:- all alike}" " all alike"

# V7: the hang tests (§6.5) with frames: a LIMIT over Motions, a CTE read in
# two slices, a cursor read in part and closed, and a statement cancelled in
# the middle of its frames -- each ends, and the cluster answers after
out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET statement_timeout = '120s';
	SELECT count(*) FROM (SELECT a.id FROM t_heap a JOIN t_heap b ON a.k = b.v LIMIT 7) l")
check "V7: a LIMIT over an island with frames below it ends" "$out" "7"
out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET statement_timeout = '120s';
	WITH c AS (SELECT k, count(*) AS n FROM t_porc GROUP BY k) SELECT count(*) FROM c a JOIN c b ON a.k = b.k + 1")
check "V7: a CTE read twice, its slices' frames shared, ends" "$out" "96"
out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET statement_timeout = '120s';
	BEGIN; DECLARE cur NO SCROLL CURSOR FOR SELECT s, count(*) FROM t_heap GROUP BY s; FETCH 5 FROM cur; CLOSE cur; COMMIT;
	SELECT 'after'" | tail -1)
check "V7: a cursor over frames read five rows and closed, the session goes on" "$out" "after"
out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET statement_timeout = '300ms';
	SELECT count(*) FROM t_heap a JOIN t_heap b ON a.k = b.k JOIN t_aoco c ON b.k = c.k" 2>&1 | grep -c "canceling statement due to statement timeout")
check "V7: statement_timeout in the middle of the frames cancels the statement" "$out" "1"
out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SELECT count(*) FROM t_heap a JOIN t_heap b ON a.id = b.v")
check "V7: and the cluster answers after it" "$out" "$(cq 0 $DB "SET vexec.mode = off; SELECT count(*) FROM t_heap a JOIN t_heap b ON a.id = b.v")"

# the batch layer over rows gathered from the segments
for t in t_heap t_porc_vec; do
	out=$(cq 0 $DB "SELECT count(*), min(rows), max(batches) FROM vexec_test.roundtrip('SELECT * FROM $t')")
	check "round trips over $t's rows" "$out" "7|$ROWS|$(( (ROWS + 1023) / 1024 ))"
	for f in postgres arrow; do
		out=$(cq 0 $DB "SELECT count(*), min(rows) FROM vexec_test.export('SELECT * FROM $t', '$f')")
		check "the export check over $t's rows, $f format" "$out" "7|$ROWS"
	done
done

# over shm: the segments' logs have its senders and receivers through rings
if [ "$INTERCONNECT" = shm ]; then
	sends=$(cat "$ROOT"/node[1-9]*.log | grep -c "interconnect shm: .* sends to .* through shared memory")
	recvs=$(cat "$ROOT"/node[1-9]*.log | grep -c "interconnect shm: .* receives from .* through shared memory")
	tcps=$(cat "$ROOT"/node[1-9]*.log | grep -c "interconnect shm: .* over tcp")
	if [ "$sends" -gt 0 ] && [ "$recvs" -gt 0 ] && [ "$tcps" = 0 ]; then
		echo "  ok the suite's Motions went through shm's rings: $sends senders and $recvs receivers began so, none over tcp"
	else
		echo "  FAILED the suite's Motions over shm: $sends senders and $recvs receivers through rings, $tcps over tcp"
		fail=1
	fi
fi

echo "cluster: $([ $fail -eq 0 ] && echo passed || echo FAILED)"
exit $fail
