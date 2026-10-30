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
#     pages store's parts go into memory;
#   - F5 (pg_vector_executor.md §3.28): a part's text vouched for to vexec
#     (ch.parts.text_checked) where the extension found each value the
#     encoding's whole as it wrote the part -- rows, a sink's batches, rows
#     of a sink's into a table of its own files -- and a merge of such parts
#     alone; one holding values the encoding does not take whole
#     (vexec_test.text_of_bytes()), and rows of PostgreSQL's copied from it,
#     not, and their length(), regexp_replace(), = and min() PostgreSQL's,
#     in each store and in a table of its own files.
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
		# a part where the buffer passed 7,000 rows, at a batch's end: 6 of 1,024-row batches' 7,168,
		# 5 of 4,096-row batches' 8,192 (vexec.batch_rows, pg_vector_executor.md §3.38's F24)
		check "$t: its parts" "$(sql "SELECT count(*) >= 5 FROM ch.parts(('$t')::regclass)" 2>&1)" "t"

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

# F5: the parts' text vouched for or not, as found as it was written; the unvouched checked
out=$(sql "SET client_min_messages = warning; CREATE EXTENSION IF NOT EXISTS vexec_test;
	CREATE TABLE bad_h (id int, v text);
	INSERT INTO bad_h VALUES (1, 'café'), (2, 'caf' || vexec_test.text_of_bytes('\\xc3')),
		(3, 'café' || vexec_test.text_of_bytes('\\x00') || 'dcba'), (4, vexec_test.text_of_bytes('\\xff') || 'caf'),
		(5, 'caf' || vexec_test.text_of_bytes('\\xe282')), (6, 'http://ünï.cödé/päth'), (7, '');")
case "$out" in *ERROR*) echo "F5 setup: $out"; fail=1 ;; esac
answers() {						# answers <table>: each bad row's length(), regexp_replace() and =, alone, and min()
	local out="" id
	for id in 1 2 3 4 5 6 7; do
		out="$out$id: $(sql "SET vexec.batch_varlena_layout = offsets;
			SELECT length(v), vexec_test.raw(regexp_replace(v, '^caf(.)\$', '\\1')), v = 'café' FROM $1 WHERE id = $id" 2>&1 |
			sed 's/^ERROR: *//'); "
	done
	echo "$out$(sql "SELECT vexec_test.raw(min(v)), count(*) FROM $1" 2>&1)"
}
checked() {						# checked <table>: its parts' text_checked, in their names' order
	sql "SELECT string_agg(p.text_checked::text, ',' ORDER BY p.name) FROM ch.parts p
		JOIN pg_class c ON c.relfilenode = p.relfilenumber WHERE c.relname = '$1'"
}
for t in t_pages t_files t_own; do
	case $t in
		t_pages) using="USING mergetree WITH (order_by = 'id', storage = 'pages')" ;;
		t_files) using="USING mergetree WITH (order_by = 'id', storage = 'files')" ;;
		t_own) using="USING parquet" ;;
	esac
	echo "== F5, $t"
	sql "SET client_min_messages = warning; CREATE TABLE $t (id int, v text) $using;
		SET vexec.mode = off; INSERT INTO $t SELECT g + 100, 'café ' || g FROM generate_series(1, 50) g;" > /dev/null
	check "$t: rows, each value whole, vouched for" "$(checked $t)" "true"
	sql "SET vexec.mode = force; INSERT INTO $t SELECT g + 200, 'naïve ' || g FROM generate_series(1, 50) g;" > /dev/null
	check "$t: the sink's batches, each value whole, vouched for" "$(checked $t)" "true,true"
	sql "SET vexec.mode = force; INSERT INTO $t SELECT * FROM bad_h;" > /dev/null
	check "$t: the sink's batch of values not whole, not" "$(checked $t)" "true,true,false"
	want=$(PGOPTIONS="-c vexec.mode=off" answers $t)
	check "$t: answers over it, as PostgreSQL's" "$(PGOPTIONS="-c vexec.mode=force" answers $t)" "$want"
	sql "SELECT ch.optimize('$t', true)" > /dev/null
	check "$t: merged with it, not" "$(checked $t)" "false"
	check "$t: answers after the merge, as PostgreSQL's" "$(PGOPTIONS="-c vexec.mode=force" answers $t)" "$want"
	sql "SET client_min_messages = warning; CREATE TABLE ${t}_c (id int, v text) $using;
		SET vexec.mode = off; INSERT INTO ${t}_c SELECT * FROM $t;" > /dev/null
	check "${t}_c: its rows copied as PostgreSQL's, not" "$(checked ${t}_c)" "false"
	check "${t}_c: answers over them, as PostgreSQL's" "$(PGOPTIONS="-c vexec.mode=force" answers ${t}_c)" "$want"
done
sql "SET client_min_messages = warning; CREATE TABLE t_ok (id int, v text) USING mergetree WITH (order_by = 'id');
	INSERT INTO t_ok SELECT g, 'é' || g FROM generate_series(1, 50) g;
	INSERT INTO t_ok SELECT g + 50, 'ü' || g FROM generate_series(1, 50) g;" > /dev/null
sql "SELECT ch.optimize('t_ok', true)" > /dev/null
check "t_ok: a merge of vouched parts, vouched for" "$(checked t_ok)" "true"

echo
if grep -qE "TRAP:|PANIC|terminated by signal" "$D/log"; then
	echo "sink: the server log has failures"
	fail=1
fi
[ $fail -eq 0 ] && echo "sink: every check passed" || echo "sink: a check FAILED"
exit $fail
