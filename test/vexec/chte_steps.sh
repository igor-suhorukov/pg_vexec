#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# chte.sh's steps (ch_table_engines.md §3, VC2): vexec's columns in steps
# over ch_table_engines' source on mergetree tables of both stores
# (vexec_source.h's VEXEC_SRC_STEPS and next_rest(), minor 5).  The first
# vector qual's columns come with each granule; each later qual's, and the
# rest, only for the batches where the quals before them left a row, so
# that the engine library decodes a later step's columns only in the
# granules where a row is left.
#
# The tables hold their rows in the key's order, a = k / 1000, so that a
# qual on a keeps the rows of a few granules: a later step is read for few
# batches -- EXPLAIN's "Batches by Step" -- and for none where no row is
# left.  Through a qual of no row, of every row, of a few; TIDs; deleted
# rows; a low_cardinality column, handed as its dictionary, in a later step;
# a column parts written before it was added lack; parts written by two
# inserts; parallel scans; a bounded sort's late columns; rescans.  Every
# answer the same as with vexec off, and as with vexec.enable_source_steps
# off.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"
D="$(mktemp -d "${TMPDIR:-/tmp}/chte-steps-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec,ch_table_engines'" "vexec.min_rows = 0" \
	"max_parallel_workers_per_gather = 0" "ch.max_merge_workers = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/steps-server.log"' EXIT

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
same() {						# same <settings> <query>: vexec's answers, steps on and off, against off's
	local ref on off
	ref=$(answer "$1 SET vexec.mode = off;" "$2")
	on=$(answer "$1 SET vexec.mode = force;" "$2")
	off=$(answer "$1 SET vexec.mode = force; SET vexec.enable_source_steps = off;" "$2")
	if [ "$on" = "$ref" ] && [ "$off" = "$ref" ]; then
		echo "same"
	else
		echo "DIFFERENT (steps on ${on:0:12}, off ${off:0:12}, vexec off ${ref:0:12})"
	fi
}
steps() {						# steps <settings> <query>: the scan's columns and batches by step
	local plan c b
	plan=$(sql "$1 SET vexec.mode = force; EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $2")
	c=$(echo "$plan" | sed -n 's/^ *Columns by Step: //p' | head -1)
	b=$(echo "$plan" | sed -n 's/^ *Batches by Step: //p' | head -1)
	case "$plan" in *ERROR*) echo "$plan" | grep ERROR ;; *) echo "${c:-none} | ${b:-none}" ;; esac
}
fewer() {						# fewer <batches by step>: whether each later step was read for fewer batches than the first
	local first rest
	IFS=', ' read -r first rest <<< "$1"
	for r in $rest; do
		[ "$r" -lt "$first" ] || { echo "no"; return; }
	done
	echo "yes"
}

out=$(sql "SET client_min_messages = warning; CREATE EXTENSION vexec; CREATE EXTENSION ch_table_engines;")
case "$out" in *ERROR*) echo "setup: $out"; exit 1 ;; esac

for store in pages files; do
	t="st_$store"
	echo "== VC2, the $store store"
	out=$(sql "SET client_min_messages = warning;
		CREATE TABLE $t (k int, a int, b int8, u text, w text, m text, n numeric(12,2), d date)
			USING mergetree WITH (order_by = 'k', storage = '$store', low_cardinality = 'm');
		INSERT INTO $t SELECT g, g / 1000, g * 7, 'u' || (g % 997), repeat(md5(g::text), 3),
			CASE WHEN g % 11 = 0 THEN NULL ELSE 'm' || (g % 5) END, g / 3.0, DATE '2024-01-01' + g % 365
			FROM generate_series(1, 200000) g;
		INSERT INTO $t SELECT g, g / 1000, g * 7, 'u' || (g % 997), md5(g::text), 'm' || (g % 3), g / 7.0,
			DATE '2025-01-01' + g % 100
			FROM generate_series(200001, 260000) g;
		ANALYZE $t;")
	case "$out" in *ERROR*) echo "  $t: $out"; fail=1; continue ;; esac

	# a qual of a few granules: the later steps read for few batches
	got=$(steps "" "SELECT k, b, u, w, n FROM $t WHERE a = 37")
	echo "  a = 37: columns | batches by step: $got"
	check "$t, a = 37: later steps read for fewer batches" "$(fewer "${got#*| }")" "yes"
	got=$(steps "" "SELECT k, w FROM $t WHERE a = -1")
	check "$t, a qual of no row: no later step read" "${got#*| }" "$(echo "${got#*| }" | sed 's/, [0-9]*/, 0/g')"
	got=$(steps "" "SELECT k, b, w FROM $t WHERE a BETWEEN 30 AND 32 AND u LIKE 'u1%' AND n > 0")
	echo "  three quals: columns | batches by step: $got"

	for qq in "SELECT k, b, u, w, n FROM $t WHERE a = 37" \
		"SELECT k, w FROM $t WHERE a = -1" \
		"SELECT k, b, d FROM $t WHERE b > 0" \
		"SELECT k, b, w FROM $t WHERE a BETWEEN 30 AND 32 AND u LIKE 'u1%' AND n > 0" \
		"SELECT ctid, k, n FROM $t WHERE a = 211" \
		"SELECT k, m, w FROM $t WHERE a IN (5, 120, 230) AND k % 3 = 0" \
		"SELECT m, count(*), min(w), sum(n) FROM $t WHERE a < 40 AND u LIKE '%9' GROUP BY m" \
		"SELECT k, upper(m), length(w) FROM $t WHERE a = 77 AND m IS NOT NULL" \
		"SELECT k, regexp_replace(u, 'u', 'v'), d FROM $t WHERE a = 150 AND regexp_replace(w, '[0-9]', '', 'g') <> ''" \
		"SELECT k, b FROM $t WHERE a > 100 AND (CASE WHEN k % 97 = 0 THEN 0 ELSE 1000 / (k % 97) END) > 900" \
		"SELECT * FROM $t WHERE u LIKE 'u42%' ORDER BY b DESC LIMIT 7" \
		"SELECT k, w FROM $t WHERE a = 255"; do
		check "$t: ${qq#SELECT }" "$(same "" "$qq")" "same"
	done
	# in parallel, a unit a participant's
	check "$t, in parallel" "$(same "SET max_parallel_workers_per_gather = 4; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET vexec.scan_rows_per_worker = 10000;" \
		"SELECT k, b, u, w FROM $t WHERE a IN (3, 99, 240) OR k = 7")" "same"
	# a subquery's scan, for each outer row
	check "$t, rescanned" "$(same "" "SELECT o.x, (SELECT max(w) FROM $t WHERE a = o.x AND u LIKE '%1') FROM (VALUES (4), (44), (244)) o(x)")" "same"

	# deleted rows: the first step's row ids know them
	out=$(sql "DELETE FROM $t WHERE a = 37 AND k % 2 = 0; DELETE FROM $t WHERE k = 211500")
	case "$out" in *ERROR*) echo "  $t delete: $out"; fail=1 ;; esac
	check "$t, rows deleted" "$(same "" "SELECT k, b, u, w FROM $t WHERE a IN (37, 211)")" "same"

	# a column the parts were written without: its missing value in a later step
	out=$(sql "ALTER TABLE $t ADD COLUMN z text DEFAULT 'zz';
		INSERT INTO $t (k, a, b, u, w, m, n, d, z) VALUES (300000, 300, 1, 'u1', 'w', 'm1', 1, '2026-01-01', 'new');")
	case "$out" in *ERROR*) echo "  $t add column: $out"; fail=1 ;; esac
	check "$t, a column added" "$(same "" "SELECT k, z, w FROM $t WHERE a IN (38, 300)")" "same"
done

echo
if grep -qE "TRAP:|PANIC|terminated by signal" "$D/log"; then
	echo "steps: the server log has failures"
	fail=1
fi
[ $fail -eq 0 ] && echo "steps: every check passed" || echo "steps: a check FAILED"
exit $fail
