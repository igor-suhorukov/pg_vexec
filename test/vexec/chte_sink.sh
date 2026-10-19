#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# chte.sh's sink (ch_table_engines.md §3, VC18): VecInsert's batches into
# mergetree tables of both stores through ch_table_engines' sink, which fills
# ClickHouse's columns from them directly -- the write buffer holding the
# engine library's writer, not copies of the columns -- and writes a pages
# store's part into memory, then into its pages, with no temporary files.
# loads (chte_loads.sh) covers every type class; here what the writer kept
# by the buffer changes:
#
#   - an index's entries made from the TIDs the sink gives, found by an
#     index scan;
#   - a collapsing engine's sign, narrowed to Int8, and a sign out of range;
#   - parts made mid-statement at ch.insert_buffer_rows, and as a scan of
#     the table begins; rows of tuple_insert and of the sink in one
#     transaction, each kind making the other's rows a part first;
#   - a savepoint rolled back, and a transaction aborted, with the writer's
#     rows unwritten;
#   - every answer that of a heap table loaded alike, with
#     ch.enable_sink_columns and ch.enable_memory_parts on and off; no
#     tmp_insert_ directory left in the data directory, nor made where the
#     pages store's parts go into memory.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"
D="$(mktemp -d "${TMPDIR:-/tmp}/chte-sink-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec,ch_table_engines'" "vexec.min_rows = 0" \
	"max_parallel_workers_per_gather = 0" "ch.max_merge_workers = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/sink-server.log"' EXIT

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
rows() {						# rows <table>: its rows' md5, read with vexec off
	sql "SET vexec.mode = off; SELECT md5(coalesce(string_agg(x::text, '|' ORDER BY x::text), '')) FROM $1 x"
}
err() {							# err <sql>: the error it raises, or "none"
	local out
	out=$(sql "$1" 2>&1)
	case "$out" in *ERROR*) echo "$out" | grep -o 'ERROR:.*' | head -1 | sed 's/ (.*//' ;; *) echo "none" ;; esac
}
tmp_dirs() {					# tmp_dirs: the tmp_insert_ directories in the data directory
	find "$D" -type d -name 'tmp_insert_*' 2> /dev/null | wc -l
}

out=$(sql "SET client_min_messages = warning; CREATE EXTENSION vexec; CREATE EXTENSION ch_table_engines;
CREATE TABLE src AS SELECT g AS k, g % 1000 AS a, 'v' || g AS t, (g % 7)::int2 AS s, g / 3.0 AS n,
	CASE WHEN g % 9 = 0 THEN NULL ELSE ('{' || g % 5 || '}')::int[] END AS arr
	FROM generate_series(1, 40000) g;")
case "$out" in *ERROR*) echo "setup: $out"; exit 1 ;; esac

for store in pages files; do
	for mode in "on on" "off off" "on off"; do
		read -r cols mem <<< "$mode"
		set="SET vexec.mode = force; SET ch.enable_sink_columns = $cols; SET ch.enable_memory_parts = $mem;"
		t="s_${store}_${cols}_$mem"
		echo "== VC18, the $store store, ch.enable_sink_columns $cols, ch.enable_memory_parts $mem"
		out=$(sql "SET client_min_messages = warning;
			CREATE TABLE $t (k int, a int, t text, s int2, n numeric(12,2), arr int[])
				USING mergetree WITH (order_by = 'a, k', storage = '$store');
			CREATE TABLE ${t}_h (LIKE $t);")
		case "$out" in *ERROR*) echo "  $t: $out"; fail=1; continue ;; esac

		# parts made mid-statement, the rows heap's
		sql "$set SET ch.insert_buffer_rows = 7000; INSERT INTO $t SELECT * FROM src;" > /dev/null
		sql "INSERT INTO ${t}_h SELECT * FROM src;" > /dev/null
		check "$t: 40000 rows, parts every 7000" "$(rows $t)" "$(rows ${t}_h)"
		check "$t: its parts" "$(sql "SELECT count(*) > 5 FROM ch.parts(('$t')::regclass)" 2>&1)" "t"

		# rows of tuple_insert and of the sink in one transaction, a scan between
		sql "BEGIN; SET LOCAL vexec.mode = off; INSERT INTO $t VALUES (-1, -1, 'row', 1, 1, NULL);
			SET LOCAL vexec.mode = force; $set INSERT INTO $t SELECT k + 100000, a, t, s, n, arr FROM src WHERE k < 5000;
			SELECT count(*) FROM $t;
			SET LOCAL vexec.mode = off; INSERT INTO $t VALUES (-2, -2, 'row', 1, 1, NULL); COMMIT;" > /dev/null
		sql "INSERT INTO ${t}_h VALUES (-1, -1, 'row', 1, 1, NULL);
			INSERT INTO ${t}_h SELECT k + 100000, a, t, s, n, arr FROM src WHERE k < 5000;
			INSERT INTO ${t}_h VALUES (-2, -2, 'row', 1, 1, NULL);" > /dev/null
		check "$t: rows and batches in one transaction" "$(rows $t)" "$(rows ${t}_h)"

		# a savepoint rolled back, a transaction aborted, the writer's rows unwritten
		sql "BEGIN; $set SAVEPOINT p; INSERT INTO $t SELECT k + 200000, a, t, s, n, arr FROM src;
			ROLLBACK TO p; INSERT INTO $t SELECT k + 300000, a, t, s, n, arr FROM src WHERE k < 100; COMMIT;" > /dev/null
		sql "INSERT INTO ${t}_h SELECT k + 300000, a, t, s, n, arr FROM src WHERE k < 100;" > /dev/null
		sql "BEGIN; $set INSERT INTO $t SELECT k + 400000, a, t, s, n, arr FROM src; ROLLBACK;" > /dev/null
		check "$t: a savepoint rolled back, a transaction aborted" "$(rows $t)" "$(rows ${t}_h)"

		# an index's entries from the sink's TIDs, the rows found through it
		sql "CREATE INDEX ${t}_k ON $t (k); CREATE INDEX ${t}_hk ON ${t}_h (k);" > /dev/null
		sql "$set INSERT INTO $t SELECT 600000 + k, a, t, s, n, arr FROM src WHERE k <= 2000;" > /dev/null
		sql "INSERT INTO ${t}_h SELECT 600000 + k, a, t, s, n, arr FROM src WHERE k <= 2000;" > /dev/null
		check "$t: rows inserted under an index" "$(rows $t)" "$(rows ${t}_h)"
		check "$t: found through the index" \
			"$(sql "SET vexec.mode = off; SET enable_seqscan = off; SET enable_bitmapscan = off;
				SELECT count(*), sum(a) FROM $t WHERE k BETWEEN 600100 AND 601900")" \
			"$(sql "SELECT count(*), sum(a) FROM ${t}_h WHERE k BETWEEN 600100 AND 601900")"
		check "$t: no tmp_insert_ directory" "$(tmp_dirs)" "0"
	done

	# a collapsing engine's sign, narrowed to Int8; one out of range
	t="c_$store"
	out=$(sql "SET client_min_messages = warning;
		CREATE TABLE $t (k int, v int, sign int2 NOT NULL) USING collapsingmergetree
			WITH (order_by = 'k', sign = 'sign', storage = '$store');
		CREATE TABLE ${t}_h (k int, v int, sign int2 NOT NULL);")
	case "$out" in *ERROR*) echo "  $t: $out"; fail=1; continue ;; esac
	sql "SET vexec.mode = force; INSERT INTO $t SELECT k, a, CASE WHEN k % 2 = 0 THEN 1 ELSE -1 END FROM src;" > /dev/null
	sql "INSERT INTO ${t}_h SELECT k, a, CASE WHEN k % 2 = 0 THEN 1 ELSE -1 END FROM src;" > /dev/null
	check "$t: signs" "$(rows $t)" "$(rows ${t}_h)"
	check "$t: a sign out of range" "$(err "SET vexec.mode = force; INSERT INTO $t SELECT k, a, 300 FROM src WHERE k < 5")" \
		"ERROR:  sign value 300 is out of range"
done

echo
if grep -qE "TRAP:|PANIC|terminated by signal" "$D/log"; then
	echo "sink: the server log has failures"
	fail=1
fi
[ $fail -eq 0 ] && echo "sink: every check passed" || echo "sink: a check FAILED"
exit $fail
