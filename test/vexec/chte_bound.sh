#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# chte.sh's bound (pg_vector_executor.md §3.38, F25; ch_table_engines.md
# VC43): a bounded VecSort's running bound, given to ch_table_engines'
# source as a key as it moves (set_keys(); vexec_source.h's minor 17, the
# spec's granule_keys), passes over the granules of a unit the source reads
# by the parts' min/max skip indexes -- not only whole units, by their
# parts' statistics -- at each unit the scan claims, and of the unit being
# read, the granules its reader has not begun.  ch.granules_bound() counts
# them in the session's process: the plans here are serial.
#
# The tables hold their rows in the key's order, k, t falling as k rises and
# v and d rising with it: ORDER BY t DESC finds its bound in the first
# granule and passes over every other granule of the first part, the parts
# after it passed over whole by their statistics; ORDER BY t ASC moves its
# bound with every granule, and passes over none.  Keys of timestamp, int8
# and date, either order, with an OFFSET; a reader in steps -- a qual -- and
# one of runs (vexec.enable_source_steps off); both stores; a bound after
# quals that may raise, none given to the source; a table made without the
# indexes; the settings off; a Nullable key, which has none; parallel scans.
# Every answer the same as with vexec off.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"
D="$(mktemp -d "${TMPDIR:-/tmp}/chte-bound-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec,ch_table_engines'" "vexec.min_rows = 0" \
	"max_parallel_workers_per_gather = 0" "ch.max_merge_workers = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/bound-server.log"' EXIT

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
answer() {						# answer <settings> <query>: its rows' md5, in the query's order
	sql "$1 SELECT md5(coalesce(string_agg(x::text, '|'), '')) FROM ($2) x"
}
same() {						# same <settings> <query>: vexec's answers, the granules passed over and not, against off's
	local ref on off
	ref=$(answer "$1 SET vexec.mode = off;" "$2")
	on=$(answer "$1 SET vexec.mode = force;" "$2")
	off=$(answer "$1 SET vexec.mode = force; SET vexec.enable_granule_bound = off;" "$2")
	if [ "$on" = "$ref" ] && [ "$off" = "$ref" ]; then
		echo "same"
	else
		echo "DIFFERENT (on ${on:0:12}, off ${off:0:12}, vexec off ${ref:0:12})"
	fi
}
passed() {						# passed <settings> <query>: the granules the bound passed over in the query
	sql "$1 SET vexec.mode = force; SELECT bound_passed('${2//\'/\'\'}')"
}
some() { [ "$1" -gt 0 ] 2> /dev/null && echo "some" || echo "none ($1)"; }

out=$(sql "SET client_min_messages = warning; CREATE EXTENSION vexec; CREATE EXTENSION ch_table_engines;
	CREATE FUNCTION bound_passed(q text) RETURNS bigint LANGUAGE plpgsql AS \$\$
	DECLARE
		g0 bigint := ch.granules_bound();
	BEGIN
		EXECUTE 'SELECT count(*) FROM (' || q || ') x';
		RETURN ch.granules_bound() - g0;
	END \$\$;")
case "$out" in *ERROR*) echo "setup: $out"; exit 1 ;; esac

for store in pages files; do
	t="bd_$store"
	echo "== F25 and VC43, the $store store"
	out=$(sql "SET client_min_messages = warning; SET ch.insert_buffer_rows = 50000;
		CREATE TABLE $t (k int NOT NULL, t timestamp NOT NULL, v int8 NOT NULL, d date NOT NULL, q int NOT NULL,
				s text NOT NULL, n int8)
			USING mergetree WITH (order_by = 'k', storage = '$store', settings = 'index_granularity = 1024');
		INSERT INTO $t SELECT g, TIMESTAMP '2021-01-01' - g * interval '1 second', g * 3, DATE '2020-01-01' + g / 1000,
				g % 7, 's' || (g % 97), CASE WHEN g % 5 = 0 THEN NULL ELSE g END
			FROM generate_series(1, 200000) g;
		ANALYZE $t;")
	case "$out" in *ERROR*) echo "  $t: $out"; fail=1; continue ;; esac

	# the first granule's bound: the first part's other 48 granules passed over, by a reader of runs and in steps
	check "$t, t DESC" "$(passed "" "SELECT k, t FROM $t ORDER BY t DESC LIMIT 10")" "48"
	check "$t, t DESC, a qual: steps" "$(some "$(passed "" "SELECT k, t FROM $t WHERE q <> 3 ORDER BY t DESC LIMIT 10")")" "some"
	check "$t, t DESC, a qual, no steps" \
		"$(some "$(passed "SET vexec.enable_source_steps = off;" "SELECT k, t FROM $t WHERE q <> 3 ORDER BY t DESC LIMIT 10")")" "some"
	# a bound that moves with every granule passes over none
	check "$t, t ASC" "$(passed "" "SELECT k, t FROM $t ORDER BY t LIMIT 10")" "0"
	# int8 and date keys, either way
	check "$t, v ASC" "$(some "$(passed "" "SELECT k, v FROM $t ORDER BY v LIMIT 5")")" "some"
	check "$t, v DESC" "$(passed "" "SELECT k, v FROM $t ORDER BY v DESC LIMIT 5")" "0"
	check "$t, d ASC" "$(some "$(passed "" "SELECT k, d FROM $t ORDER BY d, k LIMIT 5")")" "some"
	# none where the setting is off, or the indexes are not read
	check "$t, vexec.enable_granule_bound off" \
		"$(passed "SET vexec.enable_granule_bound = off;" "SELECT k, t FROM $t ORDER BY t DESC LIMIT 10")" "0"
	check "$t, ch.enable_skip_indexes off" \
		"$(passed "SET ch.enable_skip_indexes = off;" "SELECT k, t FROM $t ORDER BY t DESC LIMIT 10")" "0"
	# a qual that may raise: the bound after it, none given to the source
	check "$t, a qual that may raise" \
		"$(passed "" "SELECT k, t FROM $t WHERE 100 / (q + 1) > 0 ORDER BY t DESC LIMIT 10")" "0"
	# a Nullable key has no index
	check "$t, a Nullable key" "$(passed "" "SELECT k, n FROM $t ORDER BY n DESC NULLS LAST LIMIT 10")" "0"

	for qq in "SELECT k, t FROM $t ORDER BY t DESC LIMIT 10" \
		"SELECT k, t FROM $t ORDER BY t LIMIT 10" \
		"SELECT k, t, s FROM $t WHERE q <> 3 ORDER BY t DESC LIMIT 10" \
		"SELECT k, t FROM $t ORDER BY t DESC LIMIT 10 OFFSET 2500" \
		"SELECT k, v FROM $t ORDER BY v LIMIT 5" \
		"SELECT k, v FROM $t ORDER BY v DESC LIMIT 5" \
		"SELECT k, d FROM $t ORDER BY d, k LIMIT 5" \
		"SELECT k, d FROM $t ORDER BY d DESC, k LIMIT 5" \
		"SELECT k, t FROM $t WHERE 100 / (q + 1) > 0 ORDER BY t DESC LIMIT 10" \
		"SELECT k, n FROM $t ORDER BY n DESC NULLS LAST, k LIMIT 10" \
		"SELECT * FROM $t WHERE s LIKE 's1%' ORDER BY t DESC LIMIT 7"; do
		check "$t: ${qq#SELECT }" "$(same "" "$qq")" "same"
	done
	check "$t: no steps, a qual" "$(same "SET vexec.enable_source_steps = off;" \
		"SELECT k, t, s FROM $t WHERE q <> 3 ORDER BY t DESC LIMIT 10")" "same"
	# in parallel, the bound shared by the participants
	check "$t, in parallel" "$(same "SET max_parallel_workers_per_gather = 4; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET vexec.scan_rows_per_worker = 10000;" \
		"SELECT k, t, v FROM $t WHERE q <> 5 ORDER BY t DESC LIMIT 25")" "same"
	# rows deleted, among the granules kept
	out=$(sql "DELETE FROM $t WHERE k BETWEEN 3 AND 5")
	case "$out" in *ERROR*) echo "  $t delete: $out"; fail=1 ;; esac
	check "$t, rows deleted" "$(same "" "SELECT k, t FROM $t ORDER BY t DESC LIMIT 10")" "same"
done

# a table made without the indexes: none passed over, the same answers
echo "== a table without the indexes"
out=$(sql "SET client_min_messages = warning; SET ch.insert_buffer_rows = 50000;
	CREATE TABLE bd_off (k int NOT NULL, t timestamp NOT NULL)
		USING mergetree WITH (order_by = 'k', settings = 'index_granularity = 1024, add_minmax_index_for_temporal_columns = 0, add_minmax_index_for_numeric_columns = 0');
	INSERT INTO bd_off SELECT g, TIMESTAMP '2021-01-01' - g * interval '1 second' FROM generate_series(1, 200000) g;")
case "$out" in *ERROR*) echo "  bd_off: $out"; fail=1 ;; esac
check "bd_off, t DESC" "$(passed "" "SELECT k, t FROM bd_off ORDER BY t DESC LIMIT 10")" "0"
check "bd_off: t DESC" "$(same "" "SELECT k, t FROM bd_off ORDER BY t DESC LIMIT 10")" "same"

echo
if grep -qE "TRAP:|PANIC|terminated by signal" "$D/log"; then
	echo "bound: the server log has failures"
	fail=1
fi
[ $fail -eq 0 ] && echo "bound: every check passed" || echo "bound: a check FAILED"
exit $fail
