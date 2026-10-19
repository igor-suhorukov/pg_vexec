#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# chte.sh's orders and fetches (ch_table_engines.md §3, VC4 and VC16):
# vexec over ch_table_engines' source on mergetree tables of both stores.
#
# VC4: a serial grouping by a prefix of a table's key is planned in the
# keys' order (vexec_source.h's ordering(), minor 4) and made so where the
# scan's parts, read one after another in the key's order, keep it: one
# part; parts whose ranges of the key's first column are apart; parts whose
# ranges meet, for the first column alone; NULLs of that column in the last
# part read.  Parts that overlap, a part's NULLs read before another's rows,
# and a key past the order hash.  A statement planned while the table's
# parts kept the order, run once an insert made one that breaks it, hashes.
# Every answer the same as with vexec off.
#
# VC16: a bounded VecSort's late columns over such a table name their
# columns to the source (fetch_columns()), whose fetches by TID then decode
# those alone, as ch.fetch_stats() counts; the sort's bound past 100 rows
# over the relation's VecScan kept aside, where ChScan is the cheapest scan,
# priced by the source's price of a fetch.  Every answer vexec off's.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"
D="$(mktemp -d "${TMPDIR:-/tmp}/chte-orders-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec,ch_table_engines'" "vexec.min_rows = 0" \
	"max_parallel_workers_per_gather = 0" "ch.max_merge_workers = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/orders-server.log"' EXIT

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
answer() {						# answer <settings> <query>: its rows' md5
	sql "$1 SELECT md5(coalesce(string_agg(x::text, '|' ORDER BY x::text), '')) FROM ($2) x"
}
in_order() {					# in_order <query>: the grouping's EXPLAIN ANALYZE line, or its plan's lack of one
	local plan
	plan=$(sql "SET vexec.mode = auto; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $1")
	case "$plan" in
		*"Groups In Order: yes"*) echo "in order" ;;
		*"Groups In Order: no: hashed"*) echo "hashed at run time" ;;
		*"Vec HashAggregate"*) echo "planned hashed" ;;
		*ERROR*) echo "$plan" ;;
		*) echo "no VecAgg" ;;
	esac
}
same() {						# same <query>: vexec's answer in auto mode against off's
	local ref got
	ref=$(answer "SET vexec.mode = off;" "$1")
	got=$(answer "SET vexec.mode = auto;" "$1")
	[ "$got" = "$ref" ] && echo "same" || echo "DIFFERENT"
}

out=$(sql "SET client_min_messages = warning; CREATE EXTENSION vexec; CREATE EXTENSION ch_table_engines;
CREATE FUNCTION rows_of(lo int, hi int, nulls int) RETURNS TABLE (k int, t text, v int8) LANGUAGE sql AS
\$\$ SELECT * FROM (SELECT g / 50, 't' || (g % 7), g::int8 FROM generate_series(lo * 50, hi * 50 - 1) g
     UNION ALL SELECT NULL, 'tn', g FROM generate_series(1, nulls) g) s ORDER BY random() \$\$;")
case "$out" in *ERROR*) echo "setup: $out"; exit 1 ;; esac

for store in pages files; do
	echo "== VC4, the $store store"
	# name: its loads, a statement each, a part each: lo hi nulls
	for spec in "one:0 40 0" "apart:0 20 0,20 40 0" "meeting:0 20 0,19 40 0" "overlapping:0 30 0,10 40 0" \
		"nulls_last:0 20 0,20 40 300" "nulls_first:0 20 300,20 40 0"; do
		name="${spec%%:*}"
		t="o_${name}_$store"
		out=$(sql "SET client_min_messages = warning; CREATE TABLE $t (k int, t text, v int8)
			USING mergetree WITH (order_by = 'k, t', storage = '$store');")
		IFS=, read -ra loads <<< "${spec#*:}"
		for l in "${loads[@]}"; do
			read -r lo hi nulls <<< "$l"
			out="$out$(sql "INSERT INTO $t SELECT * FROM rows_of($lo, $hi, $nulls)")"
		done
		out="$out$(sql "ANALYZE $t")"
		case "$out" in *ERROR*) echo "  $t: $out"; fail=1; continue ;; esac
		case "$name" in
			one|apart|nulls_last) want_k="in order"; want_kt="in order" ;;
			meeting) want_k="in order"; want_kt="planned hashed" ;;
			*) want_k="planned hashed"; want_kt="planned hashed" ;;
		esac
		check "$t, GROUP BY k" "$(in_order "SELECT k, count(*), sum(v) FROM $t GROUP BY k")" "$want_k"
		check "$t, GROUP BY t, k" "$(in_order "SELECT t, k, count(*) FROM $t GROUP BY t, k")" "$want_kt"
		check "$t, GROUP BY t" "$(in_order "SELECT t, count(*) FROM $t GROUP BY t")" "planned hashed"
		for qq in "SELECT k, count(*), sum(v), min(t), max(v) FROM $t GROUP BY k" \
			"SELECT k, t, count(*), sum(v) FROM $t GROUP BY k, t" \
			"SELECT k, count(*) FROM $t WHERE v % 3 = 1 GROUP BY k HAVING count(*) > 10" \
			"SELECT t, count(*) FROM $t GROUP BY t"; do
			check "$t: ${qq#SELECT }" "$(same "$qq")" "same"
		done
	done

	# planned while the parts are apart, run after an insert that overlaps them
	t="o_apart_$store"
	got=$(sql "SET vexec.mode = auto; SET plan_cache_mode = force_generic_plan;
		PREPARE g AS SELECT k, count(*), sum(v) FROM $t GROUP BY k;
		EXPLAIN (COSTS OFF) EXECUTE g;
		INSERT INTO $t SELECT * FROM rows_of(5, 15, 0);
		EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) EXECUTE g;")
	case "$got" in
		*"Group Order: the scan's"*"Groups In Order: no: hashed"*) check "$t, a plan made before an overlapping part" "hashed at run time" "hashed at run time" ;;
		*) check "$t, a plan made before an overlapping part" "$got" "hashed at run time" ;;
	esac
	check "$t, after the overlapping part" "$(same "SELECT k, count(*), sum(v) FROM $t GROUP BY k")" "same"
done

echo "== VC16: a VecSort's late columns over mergetree"
out=$(sql "SET client_min_messages = warning;
CREATE TABLE wide (k int, a int, b text, c numeric(12,2), $(for i in $(seq 1 40); do printf 'p%d text, ' $i; done) z int)
	USING mergetree WITH (order_by = 'k');
INSERT INTO wide SELECT g, g % 1000, 'b' || g, g / 7.0, $(for i in $(seq 1 40); do printf "md5((g + %d)::text), " $i; done) g
	FROM generate_series(1, 200000) g;
ANALYZE wide;")
case "$out" in *ERROR*) echo "  wide: $out"; fail=1 ;; esac
late() {						# late <query>: whether its VecSort fetches late, serially, the columns its fetches decode a row
	local plan f c
	plan=$(sql "SET vexec.mode = auto; EXPLAIN (COSTS OFF) $1")
	read -r f c <<< "$(sql "SET vexec.mode = auto; $1;
		SELECT fetches || ' ' || columns FROM ch.fetch_stats()" | tail -1)"
	case "$plan" in
		*"fetched by TID"*) echo "late, $(( c / (f > 0 ? f : 1) )) columns a fetch" ;;
		*) echo "not late" ;;
	esac
}
for qq in "SELECT p1, p2, p3 FROM wide ORDER BY a, k LIMIT 10" "SELECT * FROM wide ORDER BY a, k LIMIT 10" \
	"SELECT * FROM wide ORDER BY a DESC, k LIMIT 300" "SELECT * FROM wide ORDER BY k DESC LIMIT 300"; do
	echo "  ${qq}: $(late "$qq")"
	check "${qq}: the answer" "$(same "$qq")" "same"
done
# the scan tuple's columns, the sort's keys among them, are made from the fetched row
check "wide, three columns and the keys late: decoded alone" \
	"$(late "SELECT p1, p2, p3 FROM wide ORDER BY a, k LIMIT 10")" "late, 5 columns a fetch"
check "wide, every column late" "$(late "SELECT * FROM wide ORDER BY a, k LIMIT 10")" "late, 45 columns a fetch"

echo
if grep -qE "TRAP:|PANIC|terminated by signal" "$D/log"; then
	echo "orders: the server log has failures"
	fail=1
fi
[ $fail -eq 0 ] && echo "orders: every check passed" || echo "orders: a check FAILED"
exit $fail
