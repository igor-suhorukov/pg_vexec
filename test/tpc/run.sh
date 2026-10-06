#!/bin/bash
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# tpc: TPC-H's 22 queries and TPC-DS's 99 on a cluster -- every one planned
# by ORCA, and each answering as DuckDB answers it over the same data; and,
# timed, each under ORCA and under the planner's route, the question Route B
# is decided on (cloudberry.md, "Route B" under Still open).
#
# pg_accel's copy of the port's suite (github/cloudberry, pg19/test/tpc at
# e96065d0100), with what pg_vector_executor.md V0 adds to it:
#
#   TPC_STORAGE   the tables' storage: heap (the port's suite's only one),
#                 ao_column, pax (PAX porc) or pax_porc_vec, with gp_ao and
#                 PAX preloaded and their extensions made where one is used
#   TPC_VEXEC     vexec in every node's shared_preload_libraries, after the
#                 port's modules: 1 (where it is installed), 0
#   TPC_VEXEC_MODE  each query's vexec.mode: off; TPC_VEXEC_FORMAT its
#                 vexec.batch_format: postgres.  In force mode a query
#                 planned without the vector nodes it could have fails
#
# and a cluster secret long enough for gp_core, which refuses one under 16
# characters (pg19/modules/gp_core/gp_cluster.c:135): "tpc-" and three
# $RANDOMs come out shorter in 1.4% of runs, when ORCA falls back on every
# query for want of the secret ("a Motion, without gp.cluster_secret");
# and the load's VACUUM ANALYZE checked, whose errors the port's suite
# drops: PAX porc_vec's sampling crashed every segment on the build with
# assertions, and every plan after was made without statistics.
#
# Run on request only: it takes gigabytes and minutes.
#
#   CB_TPC=check  ORCA plans every query and its rows are DuckDB's: on the
#                 tests image, whose server checks its assertions
#   CB_TPC=time   and each timed, under ORCA and under the planner's route,
#                 whose rows are checked too; on the image built without
#                 assertions, the compose file's tpc service
#
#   docker compose -f pg19/docker/compose.yml run --rm -e CB_TPC=check tests tpc
#   docker compose -f pg19/docker/compose.yml --profile tpc build pg19-patched-noassert cb-ext-noassert
#   docker compose -f pg19/docker/compose.yml --profile tpc run --rm tpc
#
# DuckDB (/opt/duckdb in the image, pinned) generates the data with its
# tpch and tpcds extensions, gives the queries as they have them, and
# answers each over the same data (tpc.py): nothing of TPC's is copied into
# the tree.  A coordinator and TPC_SEGMENTS segments, the data in /tmp --
# a tmpfs in the tests service -- JIT and parallel query off, as the
# measurement of 2026-09-26 had them.  Settings:
#
#   TPC_SF             scale factor, 1: DuckDB's kit's answers at 1 are the
#                      ones checked; another may show two rows a LIMIT keeps
#                      that tie in its ORDER BY taken otherwise
#   TPC_KINDS          "h ds"
#   TPC_QUERIES        the queries to run, "q01 q15 05 24" (all)
#   TPC_SEGMENTS       4; 0: one node, no cluster -- gp_core, gp_orca and
#                      gp_sql loaded, no gp.role -- where ORCA's plans have
#                      no Motions (pg_vector_executor.md V2's measurement)
#   TPC_TIMEOUT        each statement's, 120 s
#   TPC_ROUNDS         each query's runs, least time kept: 1
#   TPC_WORKERS        the parallel workers per segment each query is run
#                      with, "0 2 4": 0 (none, gp.enable_parallel off, as
#                      the measurement of 2026-09-26 had them); more, each
#                      node given room for them, gp.enable_parallel on and
#                      max_parallel_workers_per_gather that many
#   TPC_SHARED_BUFFERS each node's: 256MB checked, 1GB timed
#   TPC_PLANNING       1: after the rounds, planning times (§6.8 of
#                      pg_vector_executor.md): EXPLAIN (SUMMARY)'s, with
#                      vexec off and in auto mode, under both planners, the
#                      least of three -- each query, a thousand one-row
#                      lookups by key, a five-table join; planning.tsv
#   TPC_PLANS          1: after the rounds, each query's EXPLAIN under ORCA,
#                      and under the planner's route where the run times it:
#                      its hash joins, those of them VecHashJoins, its other
#                      joins (pg_vector_executor.md V3); plans.tsv
#   TPC_PYTHON         /opt/duckdb/bin/python, the venv's
#   TPC_DUCKDB_EXTENSIONS  /opt/duckdb/extensions, the extensions it loads
#
# RESULTS_DIR keeps results.tsv, every output and each node's log.

set -u

MODE="${CB_TPC:-}"
case "$MODE" in
	check|time) ;;
	*)
		echo "  skipped: TPC-H and TPC-DS run on request, CB_TPC=check or CB_TPC=time"
		exit 77 ;;
esac

TPC_PYTHON="${TPC_PYTHON:-/opt/duckdb/bin/python}"
export TPC_DUCKDB_EXTENSIONS="${TPC_DUCKDB_EXTENSIONS-/opt/duckdb/extensions}"
if ! "$TPC_PYTHON" -c 'import duckdb' 2> /dev/null; then
	echo "  skipped: no DuckDB for $TPC_PYTHON (set TPC_PYTHON, or use the image's /opt/duckdb)"
	exit 77
fi

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

STORAGE="${TPC_STORAGE:-heap}"
case "$STORAGE" in
	heap) STORAGE_CLAUSE=""; STORAGE_PRELOAD=""; STORAGE_EXTENSIONS="" ;;
	ao_column) STORAGE_CLAUSE="USING ao_column WITH (compresstype=zstd)"; STORAGE_PRELOAD=",gp_ao"; STORAGE_EXTENSIONS="CREATE EXTENSION gp_ao;" ;;
	pax) STORAGE_CLAUSE="USING pax WITH (storage_format=porc)"; STORAGE_PRELOAD=",gp_ao,pax"; STORAGE_EXTENSIONS="CREATE EXTENSION pax;" ;;
	pax_porc_vec) STORAGE_CLAUSE="USING pax WITH (storage_format=porc_vec)"; STORAGE_PRELOAD=",gp_ao,pax"; STORAGE_EXTENSIONS="CREATE EXTENSION pax;" ;;
	*) echo "  TPC_STORAGE is heap, ao_column, pax or pax_porc_vec, not $STORAGE"; exit 1 ;;
esac
VEXEC_PRELOAD=""
if [ "${TPC_VEXEC:-1}" = 1 ] && [ -f "$("$BINDIR/pg_config" --pkglibdir)/vexec.so" ]; then
	VEXEC_PRELOAD=",vexec"
fi
VEXEC_OPTIONS=""
[ -n "$VEXEC_PRELOAD" ] && VEXEC_OPTIONS="-c vexec.mode=${TPC_VEXEC_MODE:-off} -c vexec.batch_format=${TPC_VEXEC_FORMAT:-postgres}"
# TPC_VEXEC_NUMERIC: vexec.batch_numeric_layout, scaled or varlena, which
# separates the scaled numeric's gain from batching's (§3.4.4, V2)
[ -n "$VEXEC_PRELOAD" ] && [ -n "${TPC_VEXEC_NUMERIC:-}" ] && VEXEC_OPTIONS="$VEXEC_OPTIONS -c vexec.batch_numeric_layout=$TPC_VEXEC_NUMERIC"
# in force mode, checked, a statement planned without a vector node where
# one could be built fails (vexec.debug_require_vector): every query's plan
# is then known to carry its vector scans.  Timed, the check is left out of
# the times, and the planner's route, which the check does not run, is not
# failed by it.
[ -n "$VEXEC_PRELOAD" ] && [ "${TPC_VEXEC_MODE:-off}" = force ] && [ "$MODE" = check ] &&
	VEXEC_OPTIONS="$VEXEC_OPTIONS -c vexec.debug_require_vector=on"
SF="${TPC_SF:-1}"
KINDS="${TPC_KINDS:-h ds}"
SEGMENTS="${TPC_SEGMENTS:-4}"
TIMEOUT="${TPC_TIMEOUT:-120}"
ROUNDS="${TPC_ROUNDS:-1}"
WORKERS="${TPC_WORKERS:-0}"
MAXW=0
for w in $WORKERS; do [ "$w" -gt "$MAXW" ] && MAXW=$w; done
if [ "$MODE" = time ]; then
	SHARED_BUFFERS="${TPC_SHARED_BUFFERS:-1GB}"
else
	SHARED_BUFFERS="${TPC_SHARED_BUFFERS:-256MB}"
fi

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/cb-tpc-XXXXXX")"
SOCK="$(mktemp -d /tmp/cbtpc-XXXXXX)"
BASEPORT="${PGPORT:-$((8100 + RANDOM % 200))}"
SECRET="tpc-$(od -An -tx8 -N16 /dev/urandom | tr -d ' \n')"
NODES="$(seq 0 "$SEGMENTS")"

datadir() { echo "$ROOT/node$1"; }
sockdir() { echo "$SOCK/n$1"; }
port()    { echo $((BASEPORT + $1)); }

cleanup() {
	for n in $NODES; do
		[ -n "${RESULTS_DIR:-}" ] && cp "$ROOT/node$n.log" "$RESULTS_DIR/tpc-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m immediate stop > /dev/null 2>&1
	done
	if [ -n "${RESULTS_DIR:-}" ]; then
		cp "$ROOT/results.tsv" "$RESULTS_DIR/tpc-results.tsv" 2> /dev/null
		cp "$ROOT/planning.tsv" "$RESULTS_DIR/tpc-planning.tsv" 2> /dev/null
		cp "$ROOT/plans.tsv" "$RESULTS_DIR/tpc-plans.tsv" 2> /dev/null
		[ -d "$ROOT/out" ] && cp -r "$ROOT/out" "$RESULTS_DIR/tpc-out"
	fi
	[ -n "${KEEP:-}" ] && echo "kept: $ROOT" || rm -rf "$ROOT"
	rm -rf "$SOCK"
}
trap cleanup EXIT

q() {						# q <db> <sql>
	"$PSQL" -X -q -t -A -h "$(sockdir 0)" -p "$(port 0)" -d "$1" -c "$2" 2>&1
}

if [ "$SEGMENTS" -gt 0 ]; then
	echo "TPC-H and TPC-DS, ${MODE%e}ed: scale factor $SF, a coordinator and $SEGMENTS segments, $STORAGE tables"
else
	echo "TPC-H and TPC-DS, ${MODE%e}ed: scale factor $SF, one node, $STORAGE tables"
fi
[ -n "$VEXEC_PRELOAD" ] && echo "  vexec    preloaded on every node: $VEXEC_OPTIONS"
echo "  bindir   $BINDIR"
echo "  root     $ROOT"
echo "  duckdb   $("$TPC_PYTHON" -c 'import duckdb; print(duckdb.__version__)')"
echo

# The data, the queries and DuckDB's answers, a kind at a time.
for kind in $KINDS; do
	start=$(date +%s)
	"$TPC_PYTHON" "$here/tpc.py" prepare "$kind" "$SF" "$ROOT/$kind" \
		|| { echo "DuckDB did not prepare $kind"; exit 1; }
	echo "  $kind: data, queries and DuckDB's answers in $(( $(date +%s) - start )) s"
done

# The cluster, as the port's suites make one (postgis_cluster/run.sh).
CONF="$ROOT/gp_cluster.conf"
for n in $NODES; do
	echo "$((n + 1)) $((n - 1)) p $(sockdir "$n") $(port "$n") $(datadir "$n")"
done > "$CONF"
for n in $NODES; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N --locale=C --encoding=UTF8 > "$ROOT/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n"; tail -20 "$ROOT/initdb$n.log"; exit 1; }
	{
		echo "shared_preload_libraries = 'gp_core,gp_orca,gp_sql$STORAGE_PRELOAD$VEXEC_PRELOAD'"
		echo "unix_socket_directories = '$(sockdir "$n")'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		if [ "$SEGMENTS" -gt 0 ]; then
			echo "gp.cluster_config = '$CONF'"
			echo "gp.dbid = $((n + 1))"
			echo "gp.cluster_secret = '$SECRET'"
		fi
		echo "max_prepared_transactions = 100"
		echo "max_connections = 300"
		echo "shared_buffers = $SHARED_BUFFERS"
		echo "work_mem = 64MB"
		echo "maintenance_work_mem = 256MB"
		echo "max_parallel_workers_per_gather = 0"
		if [ "$MAXW" -gt 0 ]; then
			echo "max_parallel_workers = $MAXW"
			echo "max_worker_processes = $((MAXW + 8))"
		fi
		echo "jit = off"
		echo "fsync = off"
		echo "synchronous_commit = off"
		[ "$n" -eq 0 ] && [ "$SEGMENTS" -gt 0 ] && echo "gp.role = 'dispatch'"
	} >> "$(datadir "$n")/postgresql.auto.conf"
done
for n in $(seq 1 "$SEGMENTS") 0; do
	"$BINDIR/pg_ctl" -D "$(datadir "$n")" -l "$ROOT/node$n.log" -w -t 60 start > /dev/null 2>&1 \
		|| { echo "node $n did not start"; tail -20 "$ROOT/node$n.log"; exit 1; }
done
for db in template1 postgres; do
	q "$db" "SET client_min_messages = warning; CREATE EXTENSION gp_core" > /dev/null
done

# Each kind's database: its tables, loaded in parallel, the files dropped
# as each is loaded, and analyzed.  A failed ANALYZE fails the run, after
# the queries, which then run without statistics.
analyze_failed=0
for kind in $KINDS; do
	start=$(date +%s)
	db="tpc$kind"
	q postgres "CREATE DATABASE $db" > /dev/null
	if [ -n "$STORAGE_EXTENSIONS" ]; then
		out=$(q "$db" "SET client_min_messages = warning; CREATE EXTENSION gp_sql; $STORAGE_EXTENSIONS")
		[ -n "$out" ] && { echo "the extensions of $STORAGE in $kind: $out"; exit 1; }
	fi
	# each table in the storage asked for, its clause before the distribution
	[ -n "$STORAGE_CLAUSE" ] && sed -i "s/ DISTRIBUTED / $STORAGE_CLAUSE DISTRIBUTED /" "$ROOT/$kind/schema.sql"
	out=$(q "$db" "SET client_min_messages = warning; $(cat "$ROOT/$kind/schema.sql")")
	[ -n "$out" ] && { echo "the tables of $kind: $out"; exit 1; }
	ls -S "$ROOT/$kind/data/"*.csv | xargs -P 6 -I{} bash -c '
		t=$(basename {} .csv)
		out=$("$0" -X -q -h "$1" -p "$2" -d "$3" -c "\\copy $t FROM '"'"'{}'"'"' WITH (FORMAT csv, DELIMITER '"'"'|'"'"', NULL '"''"')" 2>&1)
		[ -n "$out" ] && echo "loading $t: $out"
		rm -f {}' "$PSQL" "$(sockdir 0)" "$(port 0)" "$db"
	out=$(q "$db" "VACUUM ANALYZE")
	if echo "$out" | grep -q -E 'ERROR|FATAL|PANIC|server closed the connection'; then
		echo "  NOT OK $kind: VACUUM ANALYZE failed, so its tables have no statistics: $(echo "$out" | grep -m1 -E 'ERROR|FATAL|PANIC|server closed')"
		analyze_failed=1
	fi
	echo "  $kind: loaded and analyzed in $(( $(date +%s) - start )) s"
done
echo

# The settings of a run with <w> parallel workers per segment.
parallel() {
	[ "$1" -gt 0 ] && echo "-c gp.enable_parallel=on -c max_parallel_workers_per_gather=$1"
}

# one <db> <sql file> <optimizer on|off> <out> <workers>: "ms status" --
# ok, timeout, or error -- and the rows in <out>, what psql said besides in
# <out>.err.
one() {
	local s e rc st
	s=$(date +%s%N)
	PGOPTIONS="-c gp.optimizer=$3 -c gp.optimizer_trace_fallback=on -c gp.optimizer_print_missing_stats=off -c statement_timeout=${TIMEOUT}s $(parallel "$5") $VEXEC_OPTIONS" \
		"$PSQL" -X -h "$(sockdir 0)" -p "$(port 0)" -d "$1" -At -v ON_ERROR_STOP=1 -f "$2" > "$4" 2> "$4.err"
	rc=$?
	e=$(date +%s%N)
	if [ $rc -eq 0 ]; then st=ok
	elif grep -q 'statement timeout' "$4.err"; then st=timeout
	else st=error; fi
	echo "$(( (e - s) / 1000000 )) $st"
}

: > "$ROOT/results.tsv"
for round in $(seq 1 "$ROUNDS"); do
	start=$(date +%s)
	mkdir -p "$ROOT/out/r$round"
	for kind in $KINDS; do
		for f in "$ROOT/$kind/q/"*.sql; do
			name=$(basename "$f" .sql)
			[ -n "${TPC_QUERIES:-}" ] && [[ " $TPC_QUERIES " != *" $name "* ]] && continue
			for w in $WORKERS; do
				out="$ROOT/out/r$round/$kind-$name"
				[ "$w" -gt 0 ] && out="$out-w$w"
				read -r oms ost <<< "$(one "tpc$kind" "$f" on "$out-orca.out" "$w")"
				# The planner took the query where ORCA's trace says it fell back,
				# the DETAIL saying why: a feature, or an assertion of ORCA's own
				# where it checks them (GPOS_DEBUG).
				reason=$(sed -n '/GPORCA failed to produce a plan/,/^DETAIL:/{s/^DETAIL:  //p}' "$out-orca.out.err" | head -1 |
					sed 's/^Falling back to Postgres-based planner because GPORCA does not support the following feature: //; s/^Falling back to Postgres-based planner because //' | cut -c1-200)
				if grep -q 'GPORCA failed to produce a plan' "$out-orca.out.err"; then
					planned=no; reason="${reason:-no reason given}"
				else
					planned=yes; reason=-
				fi
				if [ "$MODE" = time ]; then
					read -r pms pst <<< "$(one "tpc$kind" "$f" off "$out-planner.out" "$w")"
				else
					pms=0; pst=-
				fi
				# the Gathers of ORCA's plan, the writers' fragments' (orca/parallel.c)
				gathers=-
				if [ "$w" -gt 0 ] && [ "$planned" = yes ]; then
					gathers=$(PGOPTIONS="-c gp.optimizer=on $(parallel "$w")" "$PSQL" -X -h "$(sockdir 0)" -p "$(port 0)" \
						-d "tpc$kind" -At -c "EXPLAIN (COSTS OFF) $(sed 's/;[[:space:]]*$//' "$f")" 2> /dev/null |
						grep -c -E '^[[:space:]]*(->  )?Gather$') || gathers=0
				fi
				printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$round" "$kind" "$name" "$planned" \
					"$(echo "$reason" | tr '\t' ' ')" "$oms" "$ost" "$pms" "$pst" "$w" "$gathers" >> "$ROOT/results.tsv"
			done
		done
	done
	echo "  round $round: $(awk -v r="$round" '$1 == r' "$ROOT/results.tsv" | wc -l) queries in $(( $(date +%s) - start )) s"
done
echo

# Planning times: a statement's EXPLAIN (SUMMARY) Planning Time, the least
# of three, summed over a file's statements, each given on a line of its own.
plan_ms() {					# plan_ms <db> <optimizer on|off> <vexec mode> <file>
	local best="" ms
	for i in 1 2 3; do
		ms=$("$TPC_PYTHON" -c '
import sys
text = "\n".join(l.split("--")[0] for l in open(sys.argv[1]).read().splitlines())
for s in (x.strip() for x in text.split(";")):
    if s:
        print("EXPLAIN (SUMMARY ON, COSTS OFF) " + " ".join(s.split()) + ";")
' "$4" | PGOPTIONS="-c gp.optimizer=$2 -c vexec.mode=$3 -c statement_timeout=${TIMEOUT}s" \
			"$PSQL" -X -h "$(sockdir 0)" -p "$(port 0)" -d "$1" -At 2>&1 |
			awk '/^Planning Time: / { s += $3; n++ } END { if (n) printf "%.3f", s }')
		[ -z "$ms" ] && { echo "-"; return; }
		if [ -z "$best" ] || awk -v a="$ms" -v b="$best" 'BEGIN { exit !(a < b) }'; then best=$ms; fi
	done
	echo "$best"
}
if [ "${TPC_PLANNING:-0}" = 1 ] && [ -n "$VEXEC_PRELOAD" ]; then
	start=$(date +%s)
	: > "$ROOT/planning.tsv"
	# a thousand one-row lookups by key, and a five-table join, in TPC-H's
	if [[ " $KINDS " == *" h "* ]]; then
		for k in $(seq 1 1000); do
			echo "SELECT o_orderstatus FROM orders WHERE o_orderkey = $(( (k * 5987) % 1500000 + 1 ));"
		done > "$ROOT/lookups.sql"
		echo "SELECT n_name, count(*) FROM customer, orders, lineitem, supplier, nation
WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey
  AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey GROUP BY n_name;" > "$ROOT/join5.sql"
	fi
	for opt in on off; do
		for vm in off auto; do
			for kind in $KINDS; do
				for f in "$ROOT/$kind/q/"*.sql; do
					name=$(basename "$f" .sql)
					[ -n "${TPC_QUERIES:-}" ] && [[ " $TPC_QUERIES " != *" $name "* ]] && continue
					printf '%s\t%s\t%s\t%s\t%s\n' "$kind" "$name" "$opt" "$vm" "$(plan_ms "tpc$kind" $opt $vm "$f")" >> "$ROOT/planning.tsv"
				done
			done
			if [ -f "$ROOT/lookups.sql" ]; then
				printf '%s\t%s\t%s\t%s\t%s\n' h lookups1000 $opt $vm "$(plan_ms tpch $opt $vm "$ROOT/lookups.sql")" >> "$ROOT/planning.tsv"
				printf '%s\t%s\t%s\t%s\t%s\n' h join5 $opt $vm "$(plan_ms tpch $opt $vm "$ROOT/join5.sql")" >> "$ROOT/planning.tsv"
			fi
		done
	done
	echo "  planning times: $(wc -l < "$ROOT/planning.tsv") measured in $(( $(date +%s) - start )) s"
	awk -F'\t' '$5 != "-" { k = $3 "/" $4; if ($2 == "lookups1000" || $2 == "join5") { x[$2 " " k] = $5; next }
			s[k] += $5; n[k]++; l[k] += log($5 > 0.001 ? $5 : 0.001) }
		END {
			for (o = 0; o < 2; o++) { opt = o ? "off" : "on"; name = o ? "the planner" : "ORCA"
				a = opt "/off"; b = opt "/auto"
				if (n[a] && n[b])
					printf "  planning, %s: the queries %.1f ms with vexec off, %.1f ms in auto mode; auto/off %.3f (geometric mean)\n",
						name, s[a], s[b], exp(l[b] / n[b] - l[a] / n[a])
				if (("lookups1000 " a) in x)
					printf "  planning, %s: a thousand lookups %.1f ms off, %.1f ms auto; the five-table join %.2f ms off, %.2f ms auto\n",
						name, x["lookups1000 " a], x["lookups1000 " b], x["join5 " a], x["join5 " b] }
		}' "$ROOT/planning.tsv"
fi

# The plans' joins (pg_vector_executor.md V3): each query's EXPLAIN under
# ORCA, with the session's vexec settings, and under the planner's route
# where the run times it -- its hash joins, those of them VecHashJoins, and
# its other joins; plans.tsv
if [ "${TPC_PLANS:-0}" = 1 ]; then
	start=$(date +%s)
	: > "$ROOT/plans.tsv"
	opts="on"
	[ "$MODE" = time ] && opts="on off"
	for opt in $opts; do
		for kind in $KINDS; do
			for f in "$ROOT/$kind/q/"*.sql; do
				name=$(basename "$f" .sql)
				[ -n "${TPC_QUERIES:-}" ] && [[ " $TPC_QUERIES " != *" $name "* ]] && continue
				counts=$("$TPC_PYTHON" -c '
import sys
text = "\n".join(l.split("--")[0] for l in open(sys.argv[1]).read().splitlines())
for s in (x.strip() for x in text.split(";")):
    if s:
        print("EXPLAIN (COSTS OFF) " + " ".join(s.split()) + ";")
' "$f" | PGOPTIONS="-c gp.optimizer=$opt -c statement_timeout=${TIMEOUT}s $VEXEC_OPTIONS -c vexec.debug_require_vector=off" \
					"$PSQL" -X -h "$(sockdir 0)" -p "$(port 0)" -d "tpc$kind" -At 2> /dev/null |
					awk '/Vec Hash [A-Za-z ]*Join/ { v++; next }
						/Hash [A-Za-z ]*Join/ { r++; next }
						/Nested Loop|Merge [A-Za-z ]*Join/ { o++ }
						END { printf "%d\t%d\t%d", v, r, o }')
				printf '%s\t%s\t%s\t%s\n' "$kind" "$name" "$opt" "$counts" >> "$ROOT/plans.tsv"
			done
		done
	done
	awk -F'\t' '{ k = ($3 == "on") ? "ORCA" : "the planner"; v[k] += $4; r[k] += $5; o[k] += $6; n[k]++
			if ($4 + $5 > 0) q[k]++; if ($4 > 0 && $5 == 0) a[k]++ }
		END { for (k in n)
			printf "  plans, %s: %d hash joins in %d of %d queries, %d of them VecHashJoins; %d queries with every hash join one; %d other joins\n",
				k, v[k] + r[k], q[k], n[k], v[k], a[k], o[k] }' "$ROOT/plans.tsv"
	echo "  plans: read in $(( $(date +%s) - start )) s"
fi

"$TPC_PYTHON" "$here/tpc.py" report "$ROOT" "$MODE"
rc=$?
if [ "$analyze_failed" = 1 ]; then
	echo "  FAILED: the load's VACUUM ANALYZE failed (above), and the queries ran without statistics"
	rc=1
fi
exit $rc
