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
#   - the batch layer in a coordinator's backend over rows gathered from the
#     segments: vexec_test's round trips and export check.
#
#   VEXEC_SEGMENTS   4
#   VEXEC_ROWS       rows of each table: 50000
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
. "$here/lib.sh"

SEGMENTS="${VEXEC_SEGMENTS:-4}"
ROWS="${VEXEC_ROWS:-50000}"
PRELOAD="gp_core,gp_orca,gp_sql,gp_ao,pax,vexec"
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

echo "the cluster leg: a coordinator and $SEGMENTS segments, $PRELOAD on every node"
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
	"SELECT id, v FROM %t WHERE v > 99990 ORDER BY v, id LIMIT 20"
)
# force-random: the per-structure layouts drawn at random each time a node
# reads them (§6.1), from a seed the coordinator's session sends the
# segments with its other settings
SEED="${VEXEC_SEED:-$(( (RANDOM << 15 | RANDOM) % 2147483646 + 1 ))}"
echo "  random session's seed: $SEED (VEXEC_SEED=$SEED reruns it)"
SESSIONS=("off" "explain" "force-postgres" "force-arrow" "force-random")
session_sets() {
	case "$1" in
		off) echo "SET vexec.mode = off;" ;;
		explain) echo "SET vexec.mode = explain;" ;;
		force-postgres) echo "SET vexec.mode = force; SET vexec.batch_format = postgres;" ;;
		force-arrow) echo "SET vexec.mode = force; SET vexec.batch_format = arrow;" ;;
		force-random) echo "SET vexec.mode = force; SET vexec.debug_layout_seed = $SEED;" ;;
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
		*) want="the slot path" ;;
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
for t in t_heap t_aoco t_porc t_porc_vec; do
	out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET gp.enable_explain_allstat = on;
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT k, v FROM $t WHERE v > 1000" |
		awk -v t="$t" 'index($0, "Vec Seq Scan on " t) { found = 1 }
			found && /allstat:/ { sub(/.*allstat: /, ""); n = split($0, e, "/"); c = 0
				for (i = 2; i <= n; i++) { k = split(e[i], f, "_"); if (f[k] + 0 > 0) c++ }
				print c; exit }')
	check "each of the $SEGMENTS segments ran the vector scan of $t, with rows of its own" "$out" "$SEGMENTS"
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
	out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET gp.optimizer_force_multistage_agg = on; SET gp.enable_explain_allstat = on;
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
	out=$(cq 0 $DB "SET gp.optimizer = on; SET vexec.mode = force; SET gp.enable_explain_allstat = on;
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT a.k, b.v FROM $t a JOIN $t b ON a.v = b.id" |
		awk '/Vec Hash Join/ { found = 1 }
			found && /allstat:/ { sub(/.*allstat: /, ""); n = split($0, e, "/"); c = 0
				for (i = 2; i <= n; i++) { k = split(e[i], f, "_"); if (f[k] + 0 > 0) c++ }
				print c; exit }')
	check "each of the $SEGMENTS segments ran the VecHashJoin of $t, with rows of its own" "$out" "$SEGMENTS"
done
# an empty build side: the probe side's Motions read to their end, and the
# statement ends, under an Append whose next branch needs the same senders.
# Broadcasts off, both sides are redistributed: the probe side receives a
# Motion that its VecHashJoin, its build side empty, never reads for rows.
# The coordinator sees what the segments' Instrumentation says -- the
# probe side's Motion read past the batch read before the build, to its
# end -- and none of a VecHashJoin's own figures, its drained Motions among
# them, which gp_core does not bring back: the coordinator, where the node
# never ran, prints none of them rather than its own zeros.
for t in t_heap t_aoco t_porc_vec; do
	q="SELECT count(*) FROM (SELECT a.k FROM $t a JOIN (SELECT * FROM t_heap WHERE v < 0) e ON a.v = e.k
		UNION ALL SELECT k FROM $t WHERE v > 99000) u"
	sets="SET gp.optimizer = on; SET gp.optimizer_enable_motion_broadcast = off; SET statement_timeout = '120s';"
	want=$(cq 0 $DB "$sets SET vexec.mode = off; $q")
	got=$(cq 0 $DB "$sets SET vexec.mode = force; $q")
	check "an empty build side over a Motion of $t, under an Append: the statement ends, answering as off" "$got" "$want"
	plan=$(cq 0 $DB "$sets SET vexec.mode = force;
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*) FROM $t a JOIN (SELECT * FROM t_heap WHERE v < 0) e ON a.v = e.k")
	probe=$(echo "$plan" | awk '/Vec Hash Join/ { found = 1 }
		found && /Motion/ { sub(/.*actual rows=/, ""); sub(/[.].*/, ""); print; exit }')
	if echo "$plan" | grep -q "Vec Hash Join (actual rows=0" && [ "${probe:-0}" -gt 1024 ] &&
		! echo "$plan" | grep -q "Inner Rows:"; then
		echo "  ok an empty build side's VecHashJoin reads its probe side's Motion of $t to its end;"
		echo "     the coordinator, which never ran it, prints none of its own figures"
	else
		echo "  FAILED the empty build side's plan of $t drains no Motion:"
		echo "$plan" | sed 's/^/    /'
		fail=1
	fi
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

# the batch layer over rows gathered from the segments
for t in t_heap t_porc_vec; do
	out=$(cq 0 $DB "SELECT count(*), min(rows), max(batches) FROM vexec_test.roundtrip('SELECT * FROM $t')")
	check "round trips over $t's rows" "$out" "7|$ROWS|$(( (ROWS + 1023) / 1024 ))"
	for f in postgres arrow; do
		out=$(cq 0 $DB "SELECT count(*), min(rows) FROM vexec_test.export('SELECT * FROM $t', '$f')")
		check "the export check over $t's rows, $f format" "$out" "7|$ROWS"
	done
done

echo "cluster: $([ $fail -eq 0 ] && echo passed || echo FAILED)"
exit $fail
