#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# vexec_postgis on the segments (pg_vector_executor.md §3.17, VK's "done
# when"), inside a container of pg_accel's port image, vexec and the pack
# installed (pack.sh): a coordinator and PACK_SEGMENTS segments of the port,
# as pg_accel's cluster leg makes them, with vexec and the pack in every
# node's shared_preload_libraries beside the port's modules.  On it:
#
#   - the pack registers on every node, and its declarations bind in each
#     segment's sessions, as vexec.declared_calls() run there shows;
#   - a segment planning its own SQL compiles PostGIS's functions as
#     declared calls (its EXPLAIN, run on the segment);
#   - queries over distributed tables -- heap, ao_column and PAX -- give the
#     answers of vexec.mode = off, errors included, in force mode under
#     ORCA and under the planner's route, in both batch formats, each
#     against off under the same planner: ORCA keeps a query's quals in
#     their order where PostgreSQL's planner orders them by cost, so the
#     two raise on different rows;
#   - and again with the segments restarted without the pack, where the
#     calls run row by row: the same answers, a segment without a pack
#     being slower, not wrong.
#
# (vexec_pgvector's test/cluster.sh, for PostGIS: its declarations, its
# tables and its queries.)
#
#   PACK_SEGMENTS   2
set -u
. /src/test/vexec/lib.sh

SEGMENTS="${PACK_SEGMENTS:-2}"
BASE="gp_core,gp_orca,gp_sql,gp_ao,pax"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/pack-cluster-XXXXXX")"
SOCK="$(mktemp -d /tmp/vpc-XXXXXX)"
BASEPORT=$((7500 + RANDOM % 200))
SECRET="vexec-$(od -An -tx8 -N16 /dev/urandom | tr -d ' \n')"	# gp_core takes 16 characters at least
NODES="$(seq 0 "$SEGMENTS")"
DB=pack
datadir() { echo "$ROOT/node$1"; }
sockdir() { echo "$SOCK/n$1"; }
port()    { echo $((BASEPORT + $1)); }
fail=0

cleanup() {
	for n in $NODES; do
		[ -n "${RESULTS_DIR:-}" ] && cp "$ROOT/node$n.log" "$RESULTS_DIR/cluster-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m immediate stop > /dev/null 2>&1
	done
	rm -rf "$ROOT" "$SOCK"
}
trap cleanup EXIT

cq() {						# cq <node> <db> <sql>: -At
	"$BINDIR/psql" -X -q -At -h "$(sockdir "$1")" -p "$(port "$1")" -U postgres -d "$2" -c "$3" 2>&1
}
check() {					# check <what> <got> <want>
	if [ "$2" = "$3" ]; then
		echo "  ok $1"
	else
		echo "  FAILED $1: got [$2], want [$3]"
		fail=1
	fi
}
preload() {					# preload <node> <libraries>
	sed -i "/^shared_preload_libraries/d" "$(datadir "$1")/postgresql.auto.conf"
	echo "shared_preload_libraries = '$2'" >> "$(datadir "$1")/postgresql.auto.conf"
}

echo "the cluster: a coordinator and $SEGMENTS segments, $BASE,vexec_postgis,vexec on every node"
CONF="$ROOT/gp_cluster.conf"
for n in $NODES; do
	echo "$((n + 1)) $((n - 1)) p $(sockdir "$n") $(port "$n") $(datadir "$n")"
done > "$CONF"
for n in $NODES; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N -U postgres --locale=C --encoding=UTF8 > "$ROOT/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n"; tail -20 "$ROOT/initdb$n.log"; exit 1; }
	{
		echo "unix_socket_directories = '$(sockdir "$n")'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		echo "gp.cluster_config = '$CONF'"
		echo "gp.dbid = $((n + 1))"
		echo "gp.cluster_secret = '$SECRET'"
		echo "max_prepared_transactions = 100"
		echo "max_connections = 100"
		echo "shared_buffers = 128MB"
		echo "fsync = off"
		echo "max_parallel_workers_per_gather = 0"
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
	} >> "$(datadir "$n")/postgresql.auto.conf"
	preload "$n" "$BASE,vexec_postgis,vexec"
done
start_segments() {
	for n in $(echo $NODES | tr ' ' '\n' | sort -rn); do
		[ "$n" -eq 0 ] && [ "${1:-all}" = segments ] && continue
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -l "$ROOT/node$n.log" -w -t 120 start > /dev/null 2>&1 \
			|| { echo "node $n did not start:"; tail -20 "$ROOT/node$n.log"; exit 1; }
	done
}
start_segments all
for db in template1 postgres; do
	out=$(cq 0 "$db" "SET client_min_messages = warning; CREATE EXTENSION gp_core")
	[ -n "$out" ] && { echo "gp_core in $db: $out"; exit 1; }
done
out=$(cq 0 postgres "CREATE DATABASE $DB")
[ -n "$out" ] && { echo "CREATE DATABASE: $out"; exit 1; }
out=$(cq 0 $DB "SET client_min_messages = warning; CREATE EXTENSION gp_sql; CREATE EXTENSION gp_ao; CREATE EXTENSION pax; CREATE EXTENSION postgis; CREATE EXTENSION vexec; CREATE EXTENSION vexec_postgis")
[ -n "$out" ] && { echo "the extensions: $out"; exit 1; }

# the pack on every node, and its declarations bound in each segment's sessions
check "the coordinator has the pack" \
	"$(cq 0 $DB "SELECT string_agg(pack || ' for ' || extension, ', ') FROM vexec.kernel_packs()")" "vexec_postgis for postgis"
check "every segment has the pack" \
	"$(cq 0 $DB "SELECT count(*) FROM gp.exec_on_segments('SELECT count(*) FROM vexec.kernel_packs()') WHERE result = '1'")" "$SEGMENTS"
check "PostGIS's functions bind on the coordinator" \
	"$(cq 0 $DB "SELECT count(*) FROM vexec.declared_calls() WHERE extension = 'postgis'")" "26"
check "and in every segment's sessions" \
	"$(cq 0 $DB "SELECT count(*) FROM gp.exec_on_segments('SELECT count(*) FROM vexec.declared_calls() WHERE extension = ''postgis''') WHERE result = '26'")" "$SEGMENTS"

# the tables, distributed, in each storage
cat > "$ROOT/tables.sql" <<'SQL'
SET client_min_messages = warning;
SELECT setseed(0.53);
CREATE TABLE src AS
SELECT i AS id, i % 10 AS category,
	   CASE WHEN i % 499 = 0 THEN 'SRID=4326;POINT EMPTY'::geometry
			WHEN i % 3 = 0 THEN ST_SetSRID(ST_MakePoint(x, y), 4326)
			WHEN i % 3 = 1 THEN ST_MakeEnvelope(x, y, x + 0.5, y + 0.5, 4326)
			ELSE ST_SetSRID(ST_MakeLine(ST_MakePoint(x, y), ST_MakePoint(x + 1, y - 1)), 4326) END AS g
FROM (SELECT i, random() * 100 AS x, random() * 100 AS y FROM generate_series(1, 20000) i) s DISTRIBUTED BY (id);
CREATE TABLE t_heap (LIKE src) DISTRIBUTED BY (id);
CREATE TABLE t_aoco (LIKE src) USING ao_column DISTRIBUTED BY (id);
CREATE TABLE t_pax (LIKE src) USING pax DISTRIBUTED BY (id);
INSERT INTO t_heap SELECT * FROM src;
INSERT INTO t_aoco SELECT * FROM src;
INSERT INTO t_pax SELECT * FROM src;
CREATE TABLE mixed (id int, g geometry) DISTRIBUTED BY (id);
INSERT INTO mixed
SELECT i, ST_SetSRID(ST_MakePoint(i % 100, i % 37), CASE WHEN i % 1000 = 0 THEN 3857 ELSE 4326 END)
FROM generate_series(1, 3000) i;
ANALYZE;
SQL
out=$("$BINDIR/psql" -X -q -At -v ON_ERROR_STOP=1 -h "$(sockdir 0)" -p "$(port 0)" -U postgres -d $DB -f "$ROOT/tables.sql" 2>&1)
[ -n "$out" ] && { echo "the tables: $out"; exit 1; }

BOX="ST_MakeEnvelope(10, 10, 30, 30, 4326)"
QUERIES=(
	"SELECT id FROM %t WHERE g && $BOX"
	"SELECT category, count(*) FROM %t WHERE ST_Intersects(g, $BOX) GROUP BY category"
	"SELECT count(*) FROM %t WHERE ST_Within(g, ST_MakeEnvelope(20, 20, 60, 60, 4326))"
	"SELECT id, ST_X(g), ST_Y(g), ST_SRID(g) FROM %t WHERE id % 3 < 1"
	"SELECT count(*), sum(ST_X(g)::numeric) FROM %t WHERE ST_Contains(ST_MakeEnvelope(0, 0, 50, 50, 4326), g) AND id % 3 < 1"
	"SELECT id, ST_Disjoint(g, $BOX), ST_Equals(g, g) FROM %t WHERE id % 97 = 5"
	"SELECT count(*) FROM %t WHERE ST_X(g) > 50"
	"SELECT count(*) FROM mixed WHERE ST_Intersects(g, $BOX)"
	"SELECT count(*) FROM mixed WHERE id % 1000 > 0 AND ST_Intersects(g, $BOX)"
)
SESSIONS=("force-orca" "force-orca-arrow" "force-planner" "force-planner-arrow")
session_sets() {
	case "$1" in
		off-orca) echo "SET vexec.mode = off; SET gp.optimizer = on;" ;;
		off-planner) echo "SET vexec.mode = off; SET gp.optimizer = off;" ;;
		force-orca) echo "SET vexec.mode = force; SET gp.optimizer = on;" ;;
		force-orca-arrow) echo "SET vexec.mode = force; SET gp.optimizer = on; SET vexec.batch_format = arrow;" ;;
		force-planner) echo "SET vexec.mode = force; SET gp.optimizer = off;" ;;
		force-planner-arrow) echo "SET vexec.mode = force; SET gp.optimizer = off; SET vexec.batch_format = arrow;" ;;
	esac
}
answer() {					# answer <session> <query>: its rows' md5, or its error
	local out
	out=$("$BINDIR/psql" -X -q -At -h "$(sockdir 0)" -p "$(port 0)" -U postgres -d $DB \
		-c "$(session_sets "$1")" -c "$2" 2>&1)
	if echo "$out" | grep -q '^ERROR:'; then
		echo "$out" | grep -m1 '^ERROR:'
	else
		echo "$out" | sort | md5sum | cut -c1-12
	fi
}
workload() {				# workload <label>
	local nq=0 nsame=0 q t s ref got
	for q in "${QUERIES[@]}"; do
		for t in t_heap t_aoco t_pax; do
			[[ "$q" == *%t* ]] || [ "$t" = t_heap ] || continue
			for s in "${SESSIONS[@]}"; do
				ref="$(answer "off-$(echo "$s" | cut -d- -f2)" "${q//%t/$t}")"
				nq=$((nq + 1))
				got="$(answer "$s" "${q//%t/$t}")"
				if [ "$got" = "$ref" ]; then
					nsame=$((nsame + 1))
				else
					echo "  FAILED $1, $s: ${q//%t/$t}: got [$got], off gives [$ref]"
					fail=1
				fi
			done
		done
	done
	echo "  $1: $nsame of $nq answers equal to off's under the same planner"
}
workload "with the pack on every node"

# a segment planning its own SQL compiles the predicate as a declared call
check "a segment's own plan has the declared call" \
	"$(cq 0 $DB "SET vexec.mode = force; SELECT count(*) FROM gp.exec_on_segments(\$\$EXPLAIN (VERBOSE, COSTS OFF, FORMAT JSON) SELECT count(*) FROM t_heap WHERE ST_Intersects(g, $BOX)\$\$) WHERE result LIKE '%st_intersects(geometry,geometry) [vexec_postgis: prefilter]%'")" "$SEGMENTS"
# under ORCA, the coordinator's plan names it in the scan the segments run
check "ORCA's plan has the declared call" \
	"$(cq 0 $DB "SET vexec.mode = force; SET gp.optimizer = on; EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*) FROM t_pax WHERE g && $BOX" | grep -c 'Declared Calls: geometry_overlaps(geometry,geometry) \[vexec_postgis: never raises\]')" "1"

# the segments without the pack
for n in $(seq 1 "$SEGMENTS"); do
	"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m fast -w stop > /dev/null 2>&1
	preload "$n" "$BASE,vexec"
done
start_segments segments
check "no segment has the pack" \
	"$(cq 0 $DB "SELECT count(*) FROM gp.exec_on_segments('SELECT count(*) FROM vexec.kernel_packs()') WHERE result = '0'")" "$SEGMENTS"
check "a segment's own plan computes the predicate row by row" \
	"$(cq 0 $DB "SET vexec.mode = force; SELECT count(*) FROM gp.exec_on_segments(\$\$EXPLAIN (VERBOSE, COSTS OFF, FORMAT JSON) SELECT count(*) FROM t_heap WHERE ST_Intersects(g, $BOX)\$\$) WHERE result LIKE '%Row-by-Row Qual%' AND result NOT LIKE '%Declared Calls%'")" "$SEGMENTS"
workload "with the pack on the coordinator alone"

echo
echo "the cluster: $([ $fail -eq 0 ] && echo "every check passed" || echo "a check FAILED")"
exit $fail
