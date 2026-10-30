#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# chte.sh's chars (pg_vector_executor.md §3.38, "After VP6, read again", F35 and
# F37; ch_table_engines.md VC47 and VC48): a mergetree storage made now keeps,
# beside each NOT NULL text column, a hidden column of its values' characters,
# which the engine library fills as a part is written and a merge copies.  A scan
# whose column only length() and comparison with the empty string read -- in its
# quals, in its target list, by a VecAgg above that can send no row to PostgreSQL's
# evaluator or a file -- reads that in place of the values: EXPLAIN ANALYZE
# VERBOSE's "Text Columns Read for Their Lengths".
#
# Text of one to four bytes a character, and empty, in both stores: length(), = ''
# and <> '' in quals and aggregates, serial and parallel, with rows deleted, parts
# merged and folded, a value past the batches' usual size.  Not for a column read
# for its values too, octet_length(), a comparison with anything else, a Nullable
# or LowCardinality column, a column added since the storage was made, a table
# with char_counts off, nor the settings off.  Every answer vexec off's.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"
D="$(mktemp -d "${TMPDIR:-/tmp}/chte-chars-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec,ch_table_engines'" "vexec.min_rows = 0" \
	"max_parallel_workers_per_gather = 0" "max_worker_processes = 16" "max_parallel_workers = 8" \
	"ch.max_merge_workers = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/chars-server.log"' EXIT

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
answer() {						# answer <settings> <query>: its rows' md5, in their text's order
	sql "$1 SELECT md5(coalesce(string_agg(x::text, '|' ORDER BY x::text), '')) FROM ($2) x"
}
same() {						# same <settings> <query>: vexec's answer against vexec off's
	local ref on
	ref=$(answer "SET vexec.mode = off;" "$2")
	on=$(answer "SET vexec.mode = force; $1" "$2")
	if [ "$on" = "$ref" ]; then echo "same"; else echo "DIFFERENT (${on:0:12}, vexec off ${ref:0:12}: $2)"; fi
}
cols() {						# cols <settings> <query>: the scan's columns read for their lengths
	local u
	u=$(sql "SET vexec.mode = force; $1 EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $2" |
		sed -n 's/^ *Text Columns Read for Their Lengths: //p' | head -1)
	echo "${u:-0}"
}
PAR="SET max_parallel_workers_per_gather = 4; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;
	SET min_parallel_table_scan_size = 0; SET vexec.scan_rows_per_worker = 8192; SET vexec.repartition_min_rows = 0;"

out=$(sql "SET client_min_messages = warning; CREATE EXTENSION vexec; CREATE EXTENSION ch_table_engines;")
case "$out" in *ERROR*) echo "setup: $out"; exit 1 ;; esac

for store in pages files; do
	T="cc_$store"
	echo "== the $store store"
	out=$(sql "CREATE TABLE $T (k int NOT NULL, t text NOT NULL, u varchar NOT NULL, n text, v int NOT NULL, lc text NOT NULL, d text NOT NULL, sp text NOT NULL)
			USING mergetree WITH (order_by = 'k', storage = '$store', low_cardinality = 'lc', settings = 'index_granularity = 1024');
		INSERT INTO $T SELECT g,
			CASE g % 5 WHEN 0 THEN '' WHEN 1 THEN 'ascii' || g WHEN 2 THEN 'ключ' || g WHEN 3 THEN '日本語' || g ELSE '😀é' || g END,
			CASE WHEN g % 3 = 0 THEN '' ELSE 'é' || g END,
			CASE g % 4 WHEN 0 THEN NULL WHEN 1 THEN '' ELSE 'ж' || g END,
			g % 100,
			CASE WHEN g % 7 = 0 THEN '' ELSE 'ля' || (g % 7) END,
			'd' || g,
			CASE WHEN g % 100 = 0 THEN 'щ' || g ELSE '' END
		FROM generate_series(1, 100000) g;")
	case "$out" in *ERROR*) echo "  $T: $out"; fail=1; continue ;; esac

	# read for their lengths: the quals' and a plain or first-stage aggregate's
	for qq in "SELECT count(*), sum(v) FROM $T WHERE t <> ''" \
		"SELECT count(*), sum(v) FROM $T WHERE t = ''" \
		"SELECT count(*), sum(v) FROM $T WHERE '' <> t" \
		"SELECT v, count(*) FROM $T WHERE t <> '' GROUP BY v" \
		"SELECT sum(length(t)), avg(length(t)), count(*) FROM $T" \
		"SELECT sum(length(t)), count(*) FROM $T WHERE t <> ''" \
		"SELECT sum(length(t)) + 1, max(length(t)), min(length(t)) FROM $T WHERE t <> ''"; do
		check "${qq#SELECT }" "$(cols "" "$qq")" "1"
		check "${qq#SELECT }" "$(same "" "$qq")" "same"
	done
	check "two columns" "$(cols "" "SELECT sum(length(t)), sum(length(d)) FROM $T WHERE u <> ''")" "3"
	check "two columns" "$(same "" "SELECT sum(length(t)), sum(length(d)) FROM $T WHERE u <> ''")" "same"
	# a first stage under a VecRepartition, which freezes, reads them; a stage or a serial grouping that may spill, not for its aggregate
	sql "ALTER TABLE $T SET (parallel_workers = 4); ANALYZE $T" > /dev/null
	qq="SELECT k, avg(length(t)), count(*) FROM $T WHERE t <> '' GROUP BY k"
	check "parallel: ${qq#SELECT }" "$(cols "$PAR" "$qq")" "1"
	check "parallel: ${qq#SELECT }" "$(same "$PAR" "$qq")" "same"
	check "parallel: a key that may raise" "$(cols "$PAR" "SELECT k % 9000, sum(length(t)) FROM $T GROUP BY 1")" "0"
	check "parallel: a key that may raise" "$(same "$PAR" "SELECT k % 9000, sum(length(t)) FROM $T GROUP BY 1")" "same"
	check "parallel: a plain aggregate" "$(same "$PAR" "SELECT sum(length(t)), count(*) FROM $T WHERE u <> ''")" "same"
	qq="SELECT v, avg(length(t)), count(*) FROM $T WHERE t <> '' GROUP BY v"
	check "parallel, no repartition: its aggregate" "$(cols "$PAR" "$qq")" "0"
	check "parallel, no repartition: its aggregate" "$(same "$PAR" "$qq")" "same"
	check "serial: a grouping's aggregate reads the values" "$(cols "" "SELECT v, sum(length(t)) FROM $T GROUP BY v")" "0"
	check "serial: a grouping's aggregate" "$(same "" "SELECT v, sum(length(t)) FROM $T GROUP BY v")" "same"
	# an aggregate's other conditions over them, a scan's own target
	qq="SELECT count(*) FILTER (WHERE t <> ''), sum(length(t)) FILTER (WHERE t = ''), sum(v) FROM $T"
	check "FILTER" "$(cols "" "$qq")" "1"
	check "FILTER" "$(same "" "$qq")" "same"
	qq="SELECT sum(CASE WHEN t <> '' THEN v ELSE 0 END), count(*) FROM $T"
	check "CASE" "$(same "" "$qq")" "same"
	qq="SELECT k, length(t) FROM $T WHERE v = 1"
	check "a target" "$(cols "" "$qq")" "1"
	check "a target" "$(same "" "$qq")" "same"
	qq="SELECT k, length(t) FROM $T WHERE v = 1 ORDER BY k DESC LIMIT 20"
	check "a target under a sort" "$(same "" "$qq")" "same"
	qq="SELECT length(u), t <> '', u = '' FROM $T WHERE k < 50"
	check "the varchar's and others' targets" "$(same "" "$qq")" "same"
	qq="SELECT sum(length(t) + 1) FROM $T"
	check "an input that may raise" "$(cols "" "$qq")" "0"
	check "an input that may raise" "$(same "" "$qq")" "same"
	# a column nearly all empty: its counts, like its values, a sparse column of the part's (VC45)
	check "the part's sparse counts passed over" "$(sql "SET vexec.mode = force; SELECT count(*), sum(v) FROM $T WHERE sp <> ''; SELECT ch.sparse_passed() > 0" | tail -1)" "t"
	for qq in "SELECT count(*), sum(v), sum(length(sp)), max(length(sp)) FROM $T WHERE sp <> ''" \
		"SELECT count(*), sum(v), sum(length(sp)) FROM $T WHERE sp = ''" \
		"SELECT sum(length(sp)), count(*) FROM $T" \
		"SELECT v, count(*) FROM $T WHERE sp <> '' GROUP BY v"; do
		check "sparse: ${qq#SELECT }" "$(cols "" "$qq")" "1"
		check "sparse: ${qq#SELECT }" "$(same "" "$qq")" "same"
		check "sparse, its sparse quals off: ${qq#SELECT }" "$(same "SET ch.enable_sparse_quals = off;" "$qq")" "same"
		check "sparse, in parallel: ${qq#SELECT }" "$(same "$PAR" "$qq")" "same"
	done
	qq="SELECT sum(length(t)), sum(v / NULLIF(k % 2, 0)) FROM $T"
	check "another input that may raise" "$(cols "" "$qq")" "0"
	check "another input that may raise" "$(same "" "$qq")" "same"
	qq="SELECT sum(length(t)) FROM $T WHERE v / (k % 3 + 1) >= 0"
	check "a qual that may raise" "$(cols "" "$qq")" "0"
	check "a qual that may raise" "$(same "" "$qq")" "same"

	# read for their values
	for qq in "SELECT count(*), max(t) FROM $T WHERE t <> ''" \
		"SELECT t, count(*) FROM $T WHERE t <> '' GROUP BY t" \
		"SELECT sum(length(t)), count(t), max(t) FROM $T" \
		"SELECT sum(octet_length(t)) FROM $T" \
		"SELECT count(*) FROM $T WHERE t = 'ascii1'" \
		"SELECT count(*) FROM $T WHERE t > ''" \
		"SELECT sum(length(n)), count(*) FROM $T WHERE n <> ''" \
		"SELECT sum(length(lc)), count(*) FROM $T WHERE lc <> ''" \
		"SELECT k, t FROM $T WHERE t <> '' ORDER BY k LIMIT 5"; do
		check "values: ${qq#SELECT }" "$(cols "" "$qq")" "0"
		check "values: ${qq#SELECT }" "$(same "" "$qq")" "same"
	done
	qq="SELECT sum(length(t)), count(*) FROM $T WHERE t <> ''"
	check "ch.enable_char_counts off" "$(cols "SET ch.enable_char_counts = off;" "$qq")" "0"
	check "ch.enable_char_counts off" "$(same "SET ch.enable_char_counts = off;" "$qq")" "same"
	check "vexec.enable_length_columns off" "$(cols "SET vexec.enable_length_columns = off;" "$qq")" "0"
	check "vexec.enable_length_columns off" "$(same "SET vexec.enable_length_columns = off;" "$qq")" "same"

	# another part, a value past the batches' usual size, rows deleted, merged, folded
	out=$(sql "INSERT INTO $T VALUES (200001, repeat('ё', 100000), 'x', NULL, 1, 'y', 'z', ''), (200002, '', '', '', 2, '', '', '');
		INSERT INTO $T SELECT g, 'ß' || g, 'é', 'x', g % 100, 'y', 'z', '' FROM generate_series(200003, 230000) g;")
	case "$out" in *ERROR*) echo "  insert: $out"; fail=1 ;; esac
	check "three parts" "$(cols "" "$qq")" "1"
	check "three parts" "$(same "" "$qq")" "same"
	check "three parts, parallel" "$(same "$PAR" "SELECT v, avg(length(t)), count(*) FROM $T WHERE t <> '' GROUP BY v")" "same"
	out=$(sql "DELETE FROM $T WHERE k % 11 = 0; UPDATE $T SET t = 'обновлено' WHERE k % 13 = 0")
	case "$out" in *ERROR*) echo "  delete: $out"; fail=1 ;; esac
	check "rows deleted and updated" "$(cols "" "$qq")" "1"
	check "rows deleted and updated" "$(same "" "$qq")" "same"
	sql "VACUUM $T" > /dev/null
	check "folded" "$(same "" "$qq")" "same"
	out=$(sql "SELECT ch.optimize('$T', true)")
	case "$out" in *ERROR*) echo "  optimize: $out"; fail=1 ;; esac
	check "merged" "$(cols "" "$qq")" "1"
	check "merged" "$(same "" "$qq")" "same"
	check "merged: every value's length" \
		"$(same "" "SELECT count(*) FROM (SELECT k, length(t) l FROM $T) a JOIN (SELECT k, char_length(t) c FROM $T) b USING (k) WHERE l = c")" "same"
	# a column added since: none kept; its old rows' value its default's
	out=$(sql "ALTER TABLE $T ADD COLUMN w text NOT NULL DEFAULT 'ёж'")
	case "$out" in *ERROR*) echo "  add column: $out"; fail=1 ;; esac
	check "a column added" "$(cols "" "SELECT sum(length(w)), count(*) FROM $T WHERE w <> ''")" "0"
	check "a column added" "$(same "" "SELECT sum(length(w)), count(*) FROM $T WHERE w <> ''")" "same"
	# a column dropped: the others' read as before
	sql "ALTER TABLE $T DROP COLUMN d" > /dev/null
	check "a column dropped" "$(cols "" "$qq")" "1"
	check "a column dropped" "$(same "" "$qq")" "same"
	# a NOT NULL dropped: the column Nullable from then on, its values read
	out=$(sql "ALTER TABLE $T ALTER COLUMN u DROP NOT NULL")
	case "$out" in *ERROR*) echo "  drop not null: $out"; fail=1 ;; esac
	check "NOT NULL dropped" "$(cols "" "SELECT sum(length(u)), count(*) FROM $T")" "0"
	check "NOT NULL dropped" "$(same "" "SELECT sum(length(u)), count(*) FROM $T")" "same"
	# a storage made anew by TRUNCATE
	out=$(sql "TRUNCATE $T; INSERT INTO $T (k, t, u, v, lc, w, sp) SELECT g, 'ж' || g, 'й', 1, '', 'э', '' FROM generate_series(1, 5000) g")
	case "$out" in *ERROR*) echo "  truncate: $out"; fail=1 ;; esac
	check "after a TRUNCATE" "$(cols "" "$qq")" "1"
	check "after a TRUNCATE" "$(same "" "$qq")" "same"
	# a rewrite: a new storage, of the table's options, its values' characters counted anew
	out=$(sql "VACUUM FULL $T")
	case "$out" in *ERROR*) echo "  vacuum full: $out"; fail=1 ;; esac
	check "after a VACUUM FULL" "$(cols "" "$qq")" "1"
	check "after a VACUUM FULL" "$(same "" "$qq")" "same"
	check "after a VACUUM FULL: low_cardinality stays" "$(sql "SELECT ch.definition('$T') LIKE '%lc_chars%' OR ch.definition('$T') NOT LIKE '%LowCardinality%'")" "f"
	out=$(sql "ALTER TABLE $T ALTER COLUMN t TYPE text USING t || 'ё'")
	case "$out" in *ERROR*) echo "  alter type: $out"; fail=1 ;; esac
	check "after a rewrite of the column" "$(cols "" "$qq")" "1"
	check "after a rewrite of the column" "$(same "" "$qq")" "same"
	check "after a rewrite of the column: its values" "$(sql "SET vexec.mode = force; SELECT sum(length(t)) FROM $T")" "$(sql "SET vexec.mode = off; SELECT sum(char_length(t)) FROM $T")"
done

echo "== a table without them"
out=$(sql "CREATE TABLE cc_off (k int NOT NULL, t text NOT NULL) USING mergetree WITH (order_by = 'k', char_counts = false);
	INSERT INTO cc_off SELECT g, 'ж' || g FROM generate_series(1, 10000) g;
	CREATE TABLE cc_rows (k int NOT NULL, t text NOT NULL) USING replacingmergetree WITH (order_by = 'k');
	INSERT INTO cc_rows SELECT g, 'ж' || g FROM generate_series(1, 10000) g;")
case "$out" in *ERROR*) echo "  setup: $out"; fail=1 ;; esac
for T in cc_off cc_rows; do
	check "$T" "$(cols "" "SELECT sum(length(t)), count(*) FROM $T WHERE t <> ''")" "0"
	check "$T" "$(same "" "SELECT sum(length(t)), count(*) FROM $T WHERE t <> ''")" "same"
done
sql "VACUUM FULL cc_off" > /dev/null
check "cc_off after a VACUUM FULL" "$(cols "" "SELECT sum(length(t)), count(*) FROM cc_off WHERE t <> ''")" "0"
check "cc_off after a VACUUM FULL" "$(same "" "SELECT sum(length(t)), count(*) FROM cc_off WHERE t <> ''")" "same"

echo
if grep -qE "TRAP:|PANIC|terminated by signal" "$D/log"; then
	echo "chars: the server log has failures"
	fail=1
fi
[ $fail -eq 0 ] && echo "chars: every check passed" || echo "chars: a check FAILED"
exit $fail
