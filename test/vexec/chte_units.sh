#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# chte.sh's units (pg_vector_executor.md §3.38, F28; ch_table_engines.md
# VC44): a parallel scan's units cut over the granules its quals keep, by
# the sizes a whole scan's are cut by and of at least
# ch.parallel_unit_min_bytes of its columns decoded, and a vector scan's
# workers counted by its rows weighed by the bytes its columns decode to
# (vexec.scan_row_bytes), no more participants than the source's units
# (vexec_source.h's minor 18, the estimate's raw and parallel_units).
#
# The table: four parts of 128 granules of 1,024 rows, k its key, a text of
# 200 bytes a row.  A range of the key keeping 103 granules a part makes 8
# units (2 a part) -- 7 workers and the leader -- where its rows alone would
# give 2 workers; the settings off, as before.  Every answer the same as with
# vexec off, and as with the settings off.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"
D="$(mktemp -d "${TMPDIR:-/tmp}/chte-units-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec,ch_table_engines'" "vexec.min_rows = 0" \
	"max_parallel_workers_per_gather = 8" "max_parallel_workers = 8" "max_worker_processes = 16" \
	"ch.max_merge_workers = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/units-server.log"' EXIT

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
P="SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;"
workers() {						# workers <settings> <query>: the Gather's planned workers, 0 where none
	local w
	w=$(sql "$P $1 SET vexec.mode = force; EXPLAIN (COSTS OFF) $2" | sed -n 's/^ *Workers Planned: \([0-9]*\)$/\1/p' | head -1)
	echo "${w:-0}"
}
answer() {						# answer <settings> <query>: its rows' md5, in their text's order
	sql "$P $1 SELECT md5(coalesce(string_agg(x::text, '|' ORDER BY x::text), '')) FROM ($2) x"
}
same() {						# same <query>: vexec's answers with the settings and without, against vexec off's
	local ref on off
	ref=$(answer "SET vexec.mode = off;" "$1")
	on=$(answer "SET vexec.mode = force;" "$1")
	off=$(answer "SET vexec.mode = force; SET ch.parallel_key_units = off; SET vexec.scan_row_bytes = 0;" "$1")
	if [ "$on" = "$ref" ] && [ "$off" = "$ref" ]; then
		echo "same"
	else
		echo "DIFFERENT (on ${on:0:12}, off ${off:0:12}, vexec off ${ref:0:12})"
	fi
}
units() {						# units <settings> <query>: ChScan's units of the granules kept, from EXPLAIN ANALYZE
	sql "$P $1 SET vexec.mode = off; SELECT key_units('${2//\'/\'\'}')"
}

out=$(sql "SET client_min_messages = warning; CREATE EXTENSION vexec; CREATE EXTENSION ch_table_engines;
	CREATE FUNCTION key_units(q text) RETURNS text LANGUAGE plpgsql AS \$\$
	DECLARE
		plan jsonb;
		node jsonb;
	BEGIN
		EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON) ' || q INTO plan;
		node := jsonb_path_query_first(plan, 'strict \$.**? (@.\"Custom Plan Provider\" == \"ChScan\")');
		RETURN coalesce(node->>'Units of the Granules Kept', 'none');
	END \$\$;
	CREATE TABLE un (k int NOT NULL, v int NOT NULL, t text NOT NULL)
		USING mergetree WITH (order_by = 'k', index_granularity = 1024, settings = 'min_bytes_for_wide_part = 0');
	INSERT INTO un SELECT g % 1000, g, repeat(chr(97 + g % 26), 200) FROM generate_series(1, 131072) g;
	INSERT INTO un SELECT g % 1000, g, repeat(chr(98 + g % 24), 200) FROM generate_series(1, 131072) g;
	INSERT INTO un SELECT g % 1000, g, repeat(chr(99 + g % 22), 200) FROM generate_series(1, 131072) g;
	INSERT INTO un SELECT g % 1000, g, repeat(chr(100 + g % 20), 200) FROM generate_series(1, 131072) g;
	ANALYZE un;")
case "$out" in *ERROR*) echo "setup: $out"; exit 1 ;; esac
check "parts" "$(sql "SELECT count(*) FROM ch.parts('un')")" "4"

echo "== VC44: units over the granules kept"
Q="SELECT t, count(*) FROM un WHERE k < 800 GROUP BY t"
# k < 800 keeps 103 granules a part: of k and t, 64 and 39 a part; of k alone, 4 bytes a row, one each
check "k < 800, units" "$(units "" "SELECT count(*), sum(length(t)) FROM un WHERE k < 800")" "8"
check "k < 800, k alone, units" "$(units "" "SELECT count(*) FROM un WHERE k < 800")" "4"
check "k < 800, k alone, no least of bytes, units" \
	"$(units "SET ch.parallel_unit_min_bytes = 0;" "SELECT count(*) FROM un WHERE k < 800")" "8"
check "k < 800, units off" "$(units "SET ch.parallel_key_units = off;" "SELECT count(*), sum(length(t)) FROM un WHERE k < 800")" "none"
check "k < 100, units" "$(units "" "SELECT count(*), sum(length(t)) FROM un WHERE k < 100")" "4"
check "every granule, units" "$(units "" "SELECT count(*), sum(length(t)) FROM un WHERE k >= 0")" "none"
echo "== F28: workers by the bytes a row decodes to, no more participants than units"
# 422,000 rows of 208 bytes: 16 workers by rows of 32 bytes, 2 by rows alone, 7 by the 8 units
check "k < 800, workers" "$(workers "" "$Q")" "7"
check "k < 800, rows alone" "$(workers "SET vexec.scan_row_bytes = 0;" "$Q")" "2"
check "no qual, workers" "$(workers "" "SELECT t, count(*) FROM un GROUP BY t")" "7"
# 8 bytes a row count one: 2 workers by rows; 4 units
check "no text, k < 800, workers" "$(workers "" "SELECT v % 10, count(*) FROM un WHERE k < 800 GROUP BY 1")" "2"
# a worker a 16,384 rows: k < 100's 4 units (one a part) leave 3 workers, every granule's 8 units 7
R="SET vexec.scan_rows_per_worker = 16384;"
check "k < 100, workers" "$(workers "$R" "SELECT t, count(*) FROM un WHERE k < 100 GROUP BY t")" "3"
check "k < 100, units of every granule, workers" \
	"$(workers "$R SET ch.parallel_key_units = off;" "SELECT t, count(*) FROM un WHERE k < 100 GROUP BY t")" "7"
echo "== the answers"
for qq in "$Q" "SELECT t, count(*), sum(v) FROM un WHERE k < 100 GROUP BY t" \
	"SELECT count(*), sum(v), sum(length(t)) FROM un WHERE k BETWEEN 100 AND 224" \
	"SELECT v % 10, count(*) FROM un WHERE k < 800 GROUP BY 1" \
	"SELECT k, count(*) FROM un WHERE k BETWEEN 700 AND 900 GROUP BY k ORDER BY 2 DESC, 1 LIMIT 5"; do
	check "${qq#SELECT }" "$(same "$qq")" "same"
done

echo
if grep -qE "TRAP:|PANIC|terminated by signal" "$D/log"; then
	echo "units: the server log has failures"
	fail=1
fi
[ $fail -eq 0 ] && echo "units: every check passed" || echo "units: a check FAILED"
exit $fail
