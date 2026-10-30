#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# chte.sh's condition cache (pg_vector_executor.md §3.28, F7; ch_table_engines.md
# VC30): vexec keeps, for a scan's immutable quals, the granules of a part no row
# of which is true of them, and ch_table_engines' source passes them over at the
# next scan of the same quals (vexec_source.h's granules_passed and a batch's
# granule, minor 10).  On mergetree tables of both stores, a granule 1,024 rows,
# the rows in the key's order, a needle in a few granules:
#
#   - the first scan of a qual reads every granule and finds the needle's absent
#     from the others; the second passes those over and reads the rest -- the
#     answers PostgreSQL's, vexec off;
#   - other constants, another key: kept apart;
#   - a stable qual, a parameter's: never kept; vexec.enable_condition_cache off:
#     neither read nor filled;
#   - a granule with a deleted row: not kept, the answers still PostgreSQL's;
#   - a scan under a hash join's keys (VC3): not kept;
#   - a merged part: a unit of its own, its granules found anew;
#   - F21: a scan the cache leaves few granules planned without workers;
#   - F19: a scan under a running bound learns the granules it has not seen.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"
D="$(mktemp -d "${TMPDIR:-/tmp}/chte-cc-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec,ch_table_engines'" "vexec.min_rows = 0" \
	"max_parallel_workers_per_gather = 0" "ch.max_merge_workers = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/condcache-server.log"' EXIT

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
answer() {						# answer <settings> <query>: its rows
	sql "$1 $2" | tr '\n' ' '
}
granules() {					# granules <settings> <query>: the scan's granules passed over, and found none true
	sql "$1 EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $2" |
		awk -F': ' '/Granules Passed Over/ { p = $2 } /Granules Found None True/ { k = $2 }
			END { printf "passed %s, none true %s", (p == "" ? "-" : p), (k == "" ? "-" : k) }'
}

out=$(sql "SET client_min_messages = warning; CREATE EXTENSION vexec; CREATE EXTENSION ch_table_engines;
	CREATE TABLE cc_dim AS SELECT g AS k FROM generate_series(2000, 2047) g; ANALYZE cc_dim;")
case "$out" in *ERROR*) echo "setup: $out"; exit 1 ;; esac
VX="SET vexec.mode = force;"
OFF="SET vexec.mode = off;"

for store in pages files; do
	t="cc_$store"
	echo "== the $store store"
	out=$(sql "SET client_min_messages = warning;
		CREATE TABLE $t (k int, v text, n int) USING mergetree
			WITH (order_by = 'k', storage = '$store', index_granularity = 1024);
		INSERT INTO $t SELECT g, CASE WHEN g / 1024 IN (3, 17, 40) AND g % 100 = 7 THEN 'a needle ' || g ELSE 'hay ' || g END,
			g % 13 FROM generate_series(0, 50 * 1024 - 1) g;")
	case "$out" in *ERROR*) echo "  $t: $out"; fail=1; continue ;; esac
	q1="SELECT count(*), sum(n) FROM $t WHERE v LIKE '%needle%'"
	want=$(answer "$OFF" "$q1")
	check "$t: the first scan, every granule read, 47 found none true" "$(granules "$VX" "$q1")" "passed 0, none true 47"
	check "$t: the second, those 47 passed over" "$(granules "$VX" "$q1")" "passed 47, none true 0"
	check "$t: its answer PostgreSQL's" "$(answer "$VX" "$q1")" "$want"
	check "$t: a qual of other constants, kept apart" \
		"$(granules "$VX" "SELECT count(*) FROM $t WHERE v LIKE '%needle 31%'")" "passed 0, none true 49"
	check "$t: with another qual before it, another key" \
		"$(granules "$VX" "SELECT count(*) FROM $t WHERE n = 7 AND v LIKE '%needle%'")" "passed 0, none true 47"
	check "$t: off, neither read nor filled" \
		"$(granules "$VX SET vexec.enable_condition_cache = off;" "$q1")" "passed -, none true -"
	# a stable qual, and a parameter's: never kept
	before=$(sql "SELECT kept FROM vexec.condition_cache()")
	check "$t: a stable qual, not kept" \
		"$(granules "$VX" "SELECT count(*) FROM $t WHERE v LIKE '%' || current_setting('application_name') || 'needle%'")" \
		"passed -, none true -"
	check "$t: a parameter's, not kept" \
		"$(sql "$VX PREPARE p(text) AS SELECT count(*) FROM $t WHERE v LIKE \$1; SET plan_cache_mode = force_generic_plan;
			EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) EXECUTE p('%needle%')" |
			grep -c 'Condition Cache')" "0"
	check "$t: nothing kept of them" "$(sql "SELECT kept FROM vexec.condition_cache()")" "$before"
	# a deleted row in a granule: not kept, the answers still PostgreSQL's
	sql "DELETE FROM $t WHERE k = 20 * 1024 + 5" > /dev/null
	q2="SELECT count(*), sum(n) FROM $t WHERE v LIKE '%needle 17%'"
	check "$t: a granule with a deleted row, not kept" "$(granules "$VX" "$q2")" "passed 0, none true 48"
	check "$t: and passed over the next time the others alone" "$(granules "$VX" "$q2")" "passed 48, none true 0"
	check "$t: its answer PostgreSQL's" "$(answer "$VX" "$q2")" "$(answer "$OFF" "$q2")"
	check "$t: the first qual's answer PostgreSQL's" "$(answer "$VX" "$q1")" "$(answer "$OFF" "$q1")"
	# a join's keys before the quals: the keys 2000 to 2047 leave none of granule 1's
	# rows 1024 to 1999, which the quals take, so nothing kept
	q3="SELECT count(*) FROM $t f JOIN cc_dim d ON f.k = d.k WHERE f.v LIKE '%hay 1%'"
	q4="SELECT count(*), sum(n) FROM $t WHERE v LIKE '%hay 1%'"
	check "$t: a join's keys before the quals, nothing kept" "$(granules "$VX" "$q3")" "passed 0, none true 0"
	check "$t: the join's answer PostgreSQL's" "$(answer "$VX" "$q3")" "$(answer "$OFF" "$q3")"
	check "$t: so the quals alone found anew" "$(granules "$VX" "$q4" | sed 's/, none.*//')" "passed 0"
	check "$t: and their answer PostgreSQL's, the cache's in use" "$(answer "$VX" "$q4")" "$(answer "$OFF" "$q4")"
	# F19 (§3.38): under a running bound planned before the quals, the quals go first
	# in a granule the cache has not seen whole, which it learns; the setting off, the
	# bound first and nothing learnt.  The first key n spans every granule: no min/max
	# index passes one over (VC43)
	q7="SELECT k, v FROM $t WHERE v LIKE '%a needle%' ORDER BY n, k LIMIT 3"
	q8="SELECT k, v FROM $t WHERE v LIKE '%a needl%' ORDER BY n, k LIMIT 3"
	check "$t: a running bound's first scan, 46 found none true" "$(granules "$VX" "$q7")" "passed 0, none true 46"
	check "$t: the second, those 46 passed over" "$(granules "$VX" "$q7")" "passed 46, none true 0"
	check "$t: its answer PostgreSQL's" "$(answer "$VX" "$q7")" "$(answer "$OFF" "$q7")"
	check "$t: the bound first with the setting off, nothing learnt" \
		"$(granules "$VX SET vexec.bound_fills_condition_cache = off;" "$q8")" "passed 0, none true 0"
	check "$t: its answer PostgreSQL's" "$(answer "$VX SET vexec.bound_fills_condition_cache = off;" "$q8")" "$(answer "$OFF" "$q8")"
	# a merged part: a unit of its own, its granules found anew
	sql "INSERT INTO $t VALUES (60000, 'a needle more', 1)" > /dev/null
	sql "SELECT ch.optimize('$t', true)" > /dev/null
	check "$t: the merged part's granules found anew" "$(granules "$VX" "$q1" | sed 's/, none.*//')" "passed 0"
	check "$t: then passed over" "$(granules "$VX" "$q1" | sed 's/none true.*//')" "passed 46, "
	check "$t: its answer PostgreSQL's" "$(answer "$VX" "$q1")" "$(answer "$OFF" "$q1")"
done

# F21 (pg_vector_executor.md §3.38): a scan whose quals' granules the cache knows
# nearly all false is planned without workers, by the relation's figures under
# its quals, whatever their order; its first plan, and with the setting off,
# with them.  A table of 8,192-row granules, v spread over each so that no
# min/max index passes a granule over (VC43); min_parallel_table_scan_size
# PostgreSQL's, as the few pages left are priced against it
echo "== F21: planning by the granules the cache leaves"
PAR="SET max_parallel_workers_per_gather = 4; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;
	SET vexec.scan_rows_per_worker = 100000; SET vexec.mode = force;"
out=$(sql "SET client_min_messages = warning; SET ch.insert_buffer_rows = 1000000;
	CREATE TABLE cc_plan (k int NOT NULL, v int8 NOT NULL, w int8 NOT NULL) USING mergetree WITH (order_by = 'k');
	INSERT INTO cc_plan SELECT g, (g::int8 * 7919) % 1000003, g FROM generate_series(1, 2000000) g;")
case "$out" in *ERROR*) echo "  cc_plan: $out"; fail=1 ;; esac
gathers() { sql "$1 EXPLAIN (COSTS OFF) $2" | grep -c 'Gather'; }
q5="SELECT count(*) FROM cc_plan WHERE v = 500001 AND w > 0"
q6="SELECT count(*) FROM cc_plan WHERE w > 0 AND v = 500001"
check "cc_plan: the first plan, with workers" "$(gathers "$PAR" "$q5")" "1"
sql "$PAR $q5" > /dev/null
check "cc_plan: the next, its quals in another order, without" "$(gathers "$PAR" "$q6")" "0"
check "cc_plan: with the setting off, with workers" \
	"$(gathers "$PAR SET vexec.enable_condition_cache_planning = off;" "$q6")" "1"
check "cc_plan: its answer PostgreSQL's" "$(answer "$PAR" "$q6")" "$(answer "SET vexec.mode = off;" "$q6")"

echo
if grep -qE "TRAP:|PANIC|terminated by signal" "$D/log"; then
	echo "condition cache: the server log has failures"
	fail=1
fi
[ $fail -eq 0 ] && echo "condition cache: every check passed" || echo "condition cache: a check FAILED"
exit $fail
