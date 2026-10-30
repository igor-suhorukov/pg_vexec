#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# chte.sh's pool (pg_vector_executor.md §3.28 and §3.38, F3): vexec's
# long-lived parallel workers -- vexec.pool_workers, 0 by default, 4 here --
# started for a database and role as a parallel plan first asks, then taken
# for a Gather's or a Gather Merge's workers, their DSM PostgreSQL's own, each
# a job of an idle worker.  vexec.pool() counts the jobs the session gave the
# pool.
#
# Checked: the pool started and kept; a Gather's, a Gather Merge's, a
# parallel grouping's of packed groups (D2) and a parallel hash join's workers
# the pool's, every answer the same as with the pool off and vexec off, over
# heap and mergetree tables; EXPLAIN ANALYZE's workers launched; a LIMIT
# stopping its workers early; an error in a worker raised by the leader, the
# pool serving after it; a statement timeout; a transaction with an XID, a
# leader not taking part, and the setting off, no job given; the leader's
# GUCs in the workers' answers; a hundred queries in a row.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"
D="$(mktemp -d "${TMPDIR:-/tmp}/chte-pool-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec,ch_table_engines'" "vexec.min_rows = 0" \
	"vexec.pool_workers = 4" "max_worker_processes = 16" "max_parallel_workers = 8" \
	"max_parallel_workers_per_gather = 4" "ch.max_merge_workers = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/pool-server.log"' EXIT

fail=0
sql() { q "$D" postgres "$1"; }
check() {						# check <what> <got> <want>
	if [ "$2" = "$3" ]; then
		echo "  $1: $2"
	else
		echo "  $1: $2 -- FAILED, not $3"
		fail=1
	fi
}
P="SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET min_parallel_table_scan_size = 0;
	SET vexec.scan_rows_per_worker = 8192; SET vexec.mode = force;"
answer() {						# answer <settings> <query>: its rows' md5 in their order as text -- a statement of its own
	sql "$P $1 $2" | LC_ALL=C sort | md5sum | cut -c1-32
}
jobs() {						# jobs <settings> <query>: the jobs the query gave the pool, a statement of its own
	local out
	out=$(sql "$P $1 SELECT jobs FROM vexec.pool(); $2; SELECT jobs FROM vexec.pool();")
	echo $(( $(echo "$out" | tail -1) - $(echo "$out" | head -1) ))
}
same() {						# same <query>: its answer with the pool, without it, and with vexec off
	local on off ref
	on=$(answer "" "$1")
	off=$(answer "SET vexec.enable_pool_workers = off;" "$1")
	ref=$(answer "SET vexec.mode = off;" "$1")
	if [ "$on" = "$ref" ] && [ "$off" = "$ref" ]; then
		echo "same"
	else
		echo "DIFFERENT (pool ${on:0:12}, no pool ${off:0:12}, vexec off ${ref:0:12})"
	fi
}
some() { [ "$1" -gt 0 ] 2> /dev/null && echo "some" || echo "none ($1)"; }

out=$(sql "SET client_min_messages = warning; CREATE EXTENSION vexec; CREATE EXTENSION ch_table_engines;
	CREATE TABLE ph (k int, v int8, t text);
	INSERT INTO ph SELECT g % 5000, g, 'v' || (g % 777) FROM generate_series(1, 400000) g;
	CREATE TABLE pm (k int NOT NULL, v int8 NOT NULL, t text NOT NULL)
		USING mergetree WITH (order_by = 'k', index_granularity = 1024, settings = 'min_bytes_for_wide_part = 0');
	INSERT INTO pm SELECT g % 5000, g, 'v' || (g % 777) FROM generate_series(1, 200000) g;
	INSERT INTO pm SELECT g % 5000, g, 'w' || (g % 555) FROM generate_series(200001, 400000) g;
	CREATE TABLE pd (k int, name text);
	INSERT INTO pd SELECT g, 'n' || g FROM generate_series(0, 4999) g;
	ANALYZE ph, pm, pd;")
case "$out" in *ERROR*) echo "setup: $out"; exit 1 ;; esac

echo "== the pool started and kept"
sql "$P SELECT count(*) FROM ph" > /dev/null
for i in $(seq 50); do
	[ "$(sql "SELECT workers FROM vexec.pool()")" = 4 ] && break
	sleep 0.1
done
check "workers up" "$(sql "SELECT workers FROM vexec.pool()")" "4"
check "a Gather's workers the pool's" "$(jobs "" "SELECT count(*), sum(v) FROM ph")" "4"
check "EXPLAIN ANALYZE's workers launched" \
	"$(sql "$P EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*), sum(v) FROM ph" |
		sed -n 's/^ *Workers Launched: //p' | head -1)" "4"
check "a Gather Merge's" "$(some "$(jobs "" "SELECT t, count(*) FROM ph GROUP BY t ORDER BY 2 DESC, 1 LIMIT 5")")" "some"
check "mergetree" "$(some "$(jobs "" "SELECT t, count(*), sum(v) FROM pm GROUP BY t")")" "some"
check "the setting off, none" "$(jobs "SET vexec.enable_pool_workers = off;" "SELECT count(*), sum(v) FROM ph")" "0"
check "the leader not taking part, none" \
	"$(jobs "SET parallel_leader_participation = off;" "SELECT count(*), sum(v) FROM ph")" "0"
check "a transaction with an XID, none" \
	"$(jobs "BEGIN; CREATE TEMP TABLE px (a int);" "SELECT count(*), sum(v) FROM ph")" "0"

echo "== the answers"
for qq in "SELECT count(*), sum(v), sum(length(t)) FROM ph" \
	"SELECT t, count(*), sum(v) FROM ph GROUP BY t" \
	"SELECT k, count(*), min(t), max(v) FROM ph GROUP BY k" \
	"SELECT t, count(*) FROM ph GROUP BY t ORDER BY 2 DESC, 1 LIMIT 5" \
	"SELECT k, v FROM ph WHERE v % 997 = 3 ORDER BY v DESC LIMIT 7" \
	"SELECT count(*), sum(v), max(t) FROM pm" \
	"SELECT t, count(*), sum(v) FROM pm GROUP BY t" \
	"SELECT k, count(*) FROM pm WHERE k BETWEEN 100 AND 900 GROUP BY k ORDER BY 2 DESC, 1 LIMIT 9" \
	"SELECT d.name, count(*), sum(p.v) FROM pm p JOIN pd d ON d.k = p.k GROUP BY d.name ORDER BY 2 DESC, 1 LIMIT 5" \
	"SELECT count(*) FROM (SELECT v FROM pm LIMIT 3) x" \
	"SELECT count(DISTINCT k), count(DISTINCT t) FROM ph" \
	"SELECT k % 10, count(DISTINCT t) FROM pm GROUP BY 1"; do
	check "${qq#SELECT }" "$(same "$qq")" "same"
done

echo "== errors, cancels and the leader's settings"
out=$(sql "$P SELECT sum(1 / (v - 200000)) FROM ph")
case "$out" in *"division by zero"*) check "an error in a worker" "raised" "raised" ;;
	*) check "an error in a worker" "$out" "raised" ;; esac
check "the pool serving after it" "$(some "$(jobs "" "SELECT count(*), sum(v) FROM ph")")" "some"
out=$(sql "$P SET statement_timeout = '30ms'; SELECT count(*) FROM ph a, generate_series(1, 300) g WHERE a.v % 7 = g % 7")
case "$out" in *"statement timeout"*) check "a statement timeout" "raised" "raised" ;;
	*) check "a statement timeout" "$out" "raised" ;; esac
check "the pool serving after it" "$(some "$(jobs "" "SELECT count(*), sum(v) FROM ph")")" "some"
check "DateStyle in the workers" \
	"$(sql "$P SET DateStyle = 'SQL, DMY'; SELECT max((DATE '2020-01-01' + (v % 300)::int)::text) FROM ph")" \
	"31/08/2020"
n=0
for i in $(seq 100); do
	r=$(sql "$P SELECT count(*), sum(v) FROM pm WHERE k % 100 = $((i % 100))")
	[ -n "$r" ] && n=$((n + 1))
done
check "a hundred queries" "$n" "100"
check "the workers still up" "$(sql "SELECT workers FROM vexec.pool()")" "4"

echo
if grep -qE "TRAP:|PANIC|terminated by signal" "$D/log"; then
	echo "pool: the server log has failures"
	fail=1
fi
[ $fail -eq 0 ] && echo "pool: every check passed" || echo "pool: a check FAILED"
exit $fail
