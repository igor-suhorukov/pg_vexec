#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The cluster leg (pg_vector_executor.md §5 V0, §6.1, §6.8): a coordinator
# and VEXEC_SEGMENTS segments of the port in one container, as the port's
# suites make one, with vexec in every node's shared_preload_libraries
# beside the port's modules.  On it:
#
#   - vexec loads on every node, and its settings are each node's;
#   - a session's vexec settings and whether they reach the segments: not
#     before V1, which adds vexec's names to gp_core's list of settings sent
#     to them (§3.10), so this leg reports what a segment sees;
#   - a workload over distributed tables of each storage -- heap, ao_column,
#     PAX porc and porc_vec -- answers alike with vexec.mode off, explain,
#     force in the PostgreSQL format and force in the Arrow format, under
#     ORCA and under the planner's route;
#   - EXPLAIN (VEXEC) on the coordinator: ORCA's plans reach no planner
#     hook of vexec's (§2.1), and the planner's route records alternatives;
#   - the batch layer in a coordinator's backend over rows gathered from the
#     segments: vexec_test's round trips and export check.
#
# In V0 no vector node exists, so every session must answer alike; from V1
# the same leg checks vector nodes running on every segment.
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
# a session's setting on the coordinator, and what the segments see of it
seen=$(cq 0 $DB "SET vexec.mode = explain; SELECT string_agg(DISTINCT result, ',') FROM gp.exec_on_segments('SELECT current_setting(''vexec.mode'')')")
echo "  a session's SET vexec.mode = explain on the coordinator; the segments see: $seen (V1 sends vexec's settings to them)"

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
	"SELECT k, count(*), sum(n), avg(v) FROM %t WHERE b GROUP BY k"
	"SELECT s, count(DISTINCT k) FROM %t GROUP BY s HAVING count(*) > 90"
	"SELECT a.k, count(*) FROM %t a JOIN %t b ON a.id = b.v WHERE b.k < 10 GROUP BY a.k"
	"SELECT d, sum(v) OVER (PARTITION BY k ORDER BY id) FROM %t WHERE id % 1000 = 7"
	"SELECT id, v FROM %t WHERE v > 99990 ORDER BY v, id LIMIT 20"
)
SESSIONS=("off" "explain" "force-postgres" "force-arrow")
session_sets() {
	case "$1" in
		off) echo "SET vexec.mode = off;" ;;
		explain) echo "SET vexec.mode = explain;" ;;
		force-postgres) echo "SET vexec.mode = force; SET vexec.batch_format = postgres;" ;;
		force-arrow) echo "SET vexec.mode = force; SET vexec.batch_format = arrow;" ;;
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

# EXPLAIN (VEXEC) on the coordinator
explain=$(cq 0 $DB "SET gp.optimizer = on; SET gp.optimizer_trace_fallback = on; SET vexec.mode = explain; EXPLAIN (VEXEC, COSTS OFF) SELECT k, count(*) FROM t_aoco GROUP BY k")
orca=$(echo "$explain" | grep '^Vexec')
check "ORCA's plans reach no planner hook of vexec's" "$orca" "Vexec: no vector alternatives were considered"
[ "$orca" = "Vexec: no vector alternatives were considered" ] || echo "$explain" | sed 's/^/    /'

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
