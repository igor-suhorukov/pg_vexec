#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# chte.sh's sparse (pg_vector_executor.md §3.38, "After VP5, read again";
# ch_table_engines.md VC45): a part's column of ClickHouse's sparse
# serialization -- its rows of other than the type's default listed apart --
# has its rows of the default left out of the source's batches' visible rows
# where a qual is false of the default and of NULL: the qual's column read in
# the first step, an integer, a bool or a text.  ch.sparse_passed() counts
# them in the session's process: the plans here are serial.
#
# The table: a smallint, an int8 and a text mostly of their defaults, an int
# beside them.  Quals false of the default -- <> 0, > 2, <> '' -- pass rows
# over; quals true of it -- = 0, <> 5 OR a = 0 -- or of a column not sparse,
# none; ch.enable_sparse_quals off, none; rows deleted among them; a part
# merged; a qual that would raise at the default, a cast's, judged by none.  Every answer the
# same as with vexec off.
#
# VC46 and F36 (§3.38, "After VP6, read again"): count(*) under a qual exactly a
# column's not being its default -- <> 0, <> '', a boolean -- from each part's
# rows less its exact rows of the default, kept with its shared metadata, the
# scan reading none: EXPLAIN ANALYZE's "Units From Statistics".  Not under another
# qual, nor of a Nullable or LowCardinality column, nor with any other aggregate,
# a row deleted, the settings off or before a process has loaded the part; every
# count vexec off's.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"
D="$(mktemp -d "${TMPDIR:-/tmp}/chte-sparse-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec,ch_table_engines'" "vexec.min_rows = 0" \
	"max_parallel_workers_per_gather = 0" "ch.max_merge_workers = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/sparse-server.log"' EXIT

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
same() {						# same <query>: vexec's answers with the setting and without, against vexec off's
	local ref on off
	ref=$(answer "SET vexec.mode = off;" "$1")
	on=$(answer "SET vexec.mode = force;" "$1")
	off=$(answer "SET vexec.mode = force; SET ch.enable_sparse_quals = off;" "$1")
	if [ "$on" = "$ref" ] && [ "$off" = "$ref" ]; then
		echo "same"
	else
		echo "DIFFERENT (on ${on:0:12}, off ${off:0:12}, vexec off ${ref:0:12})"
	fi
}
passed() {						# passed <settings> <query>: the rows of a sparse default the query's scan left out
	sql "$1 SET vexec.mode = force; SET vexec.enable_count_under_quals = off; SELECT sparse_passed('${2//\'/\'\'}')"
}
some() { [ "$1" -gt 0 ] 2> /dev/null && echo "some" || echo "none ($1)"; }

out=$(sql "SET client_min_messages = warning; CREATE EXTENSION vexec; CREATE EXTENSION ch_table_engines;
	CREATE FUNCTION sparse_passed(q text) RETURNS bigint LANGUAGE plpgsql AS \$\$
	DECLARE
		p0 bigint := ch.sparse_passed();
	BEGIN
		EXECUTE 'SELECT count(*) FROM (' || q || ') x';
		RETURN ch.sparse_passed() - p0;
	END \$\$;
	CREATE TABLE sp (k int NOT NULL, a smallint NOT NULL, b int8 NOT NULL, t text NOT NULL, v int NOT NULL,
			f bool NOT NULL, ns text NOT NULL)
		USING mergetree WITH (order_by = 'k', settings = 'index_granularity = 1024');
	INSERT INTO sp SELECT g, CASE WHEN g % 61 = 0 THEN (g % 5) + 1 ELSE 0 END, CASE WHEN g % 47 = 0 THEN g ELSE 0 END,
			CASE WHEN g % 53 = 0 THEN 'x' || g ELSE '' END, g, g % 59 = 0, (g % 100)::text
		FROM generate_series(1, 300000) g;")
case "$out" in *ERROR*) echo "setup: $out"; exit 1 ;; esac

echo "== VC45: a sparse column's rows of the default left out"
check "a <> 0, rows passed over" "$(passed "" "SELECT v FROM sp WHERE a <> 0")" "295082"
check "a > 2" "$(some "$(passed "" "SELECT v FROM sp WHERE a > 2")")" "some"
check "b <> 0 AND a = 0" "$(some "$(passed "" "SELECT v FROM sp WHERE b <> 0 AND a = 0")")" "some"
check "t <> ''" "$(some "$(passed "" "SELECT v FROM sp WHERE t <> ''")")" "some"
check "f" "$(some "$(passed "" "SELECT v FROM sp WHERE f")")" "some"
check "a = 0, true of the default" "$(passed "" "SELECT v FROM sp WHERE a = 0")" "0"
check "a <> 5 OR a = 0" "$(passed "" "SELECT v FROM sp WHERE a <> 5 OR a = 0")" "0"
check "v > 7, not sparse" "$(passed "" "SELECT v FROM sp WHERE v > 7")" "0"
# a qual that would raise at the default -- ''::int -- judged by no folding: none of its rows passed over
check "mod(ns::int, 13) = 1, a cast" "$(passed "" "SELECT v FROM sp WHERE mod(ns::int, 13) = 1")" "0"
check "ch.enable_sparse_quals off" "$(passed "SET ch.enable_sparse_quals = off;" "SELECT v FROM sp WHERE a <> 0")" "0"
check "vexec.enable_source_steps off" \
	"$(some "$(passed "SET vexec.enable_source_steps = off;" "SELECT v FROM sp WHERE a <> 0")")" "some"
for qq in "SELECT count(*), sum(v) FROM sp WHERE a <> 0" \
	"SELECT a, count(*), min(v) FROM sp WHERE a <> 0 GROUP BY a" \
	"SELECT count(*), sum(b) FROM sp WHERE b <> 0 AND a = 0" \
	"SELECT count(*), max(t) FROM sp WHERE t <> ''" \
	"SELECT count(*) FROM sp WHERE a > 2 AND t <> ''" \
	"SELECT count(*), sum(v) FROM sp WHERE f" \
	"SELECT count(*) FROM sp WHERE a = 0" \
	"SELECT k, a FROM sp WHERE a <> 0 ORDER BY v DESC LIMIT 5" \
	"SELECT count(*), sum(v) FROM sp WHERE mod(ns::int, 13) = 1" \
	"SELECT count(*) FROM sp WHERE mod(ns::int, 13) = 1 AND a <> 0"; do
	check "${qq#SELECT }" "$(same "$qq")" "same"
done
echo "== VC46, F36: count(*) under a column's <> its default, from each part's exact figures"
units() {						# units <settings> <query>: the units of the query's scan its source answered
	local u
	u=$(sql "$1 SET vexec.mode = force; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $2" |
		sed -n 's/^ *Units From Statistics: //p' | head -1)
	echo "${u:-0}"
}
for qq in "a <> 0" "b <> 0" "t <> ''" "f" "0 <> a"; do
	check "count(*) WHERE $qq, a part read" "$(units "" "SELECT count(*) FROM sp WHERE $qq")" "1"
	check "count(*) WHERE $qq" "$(same "SELECT count(*) FROM sp WHERE $qq")" "same"
done
for qq in "a = 0" "a <> 1" "a > 0" "a <> 0 AND b <> 0" "t <> 'x'" "NOT f"; do
	check "count(*) WHERE $qq, read" "$(units "" "SELECT count(*) FROM sp WHERE $qq")" "0"
	check "count(*) WHERE $qq" "$(same "SELECT count(*) FROM sp WHERE $qq")" "same"
done
# a NOT NULL column's count() is PostgreSQL's count(*)
check "count(v) WHERE a <> 0" "$(units "" "SELECT count(v) FROM sp WHERE a <> 0")" "1"
check "sum(v) WHERE a <> 0, read" "$(units "" "SELECT count(*), sum(v) FROM sp WHERE a <> 0")" "0"
check "ch.enable_count_from_defaults off" \
	"$(units "SET ch.enable_count_from_defaults = off;" "SELECT count(*) FROM sp WHERE a <> 0")" "0"
check "vexec.enable_count_under_quals off" \
	"$(units "SET vexec.enable_count_under_quals = off;" "SELECT count(*) FROM sp WHERE a <> 0")" "0"
check "ch.enable_shared_part_metadata off" \
	"$(units "SET ch.enable_shared_part_metadata = off;" "SELECT count(*) FROM sp WHERE a <> 0")" "0"
check "off: the same count" "$(same "SELECT count(*) FROM sp WHERE a <> 0")" "same"
out=$(sql "CREATE TABLE sq (k int NOT NULL, a smallint NOT NULL, n int, lc text NOT NULL)
		USING mergetree WITH (order_by = 'k', low_cardinality = 'lc', settings = 'index_granularity = 1024');
	INSERT INTO sq SELECT g, CASE WHEN g % 61 = 0 THEN 1 ELSE 0 END, CASE WHEN g % 61 = 0 THEN 1 ELSE 0 END,
		CASE WHEN g % 61 = 0 THEN 'x' ELSE '' END FROM generate_series(1, 100000) g;
	INSERT INTO sq SELECT g, CASE WHEN g % 67 = 0 THEN 1 ELSE 0 END, CASE WHEN g % 67 = 0 THEN 1 ELSE 0 END,
		CASE WHEN g % 67 = 0 THEN 'x' ELSE '' END FROM generate_series(100001, 200000) g;")
case "$out" in *ERROR*) echo "  sq: $out"; fail=1 ;; esac
# the writer's process attached its parts, and kept their figures
check "two parts" "$(units "" "SELECT count(*) FROM sq WHERE a <> 0")" "2"
check "two parts: the count" "$(same "SELECT count(*) FROM sq WHERE a <> 0")" "same"
check "a Nullable column, read" "$(units "" "SELECT count(*) FROM sq WHERE n <> 0")" "0"
check "a Nullable column: the count" "$(same "SELECT count(*) FROM sq WHERE n <> 0")" "same"
check "LowCardinality, read" "$(units "" "SELECT count(*) FROM sq WHERE lc <> ''")" "0"
check "LowCardinality: the count" "$(same "SELECT count(*) FROM sq WHERE lc <> ''")" "same"
out=$(sql "SELECT ch.optimize('sq', true)")
case "$out" in *ERROR*) echo "  optimize: $out"; fail=1 ;; esac
check "merged" "$(units "" "SELECT count(*) FROM sq WHERE a <> 0")" "1"
check "merged: the count" "$(same "SELECT count(*) FROM sq WHERE a <> 0")" "same"
server_stop "$D"; server_start "$D" || { echo "the server did not start again"; exit 1; }
# a serial scan claiming the part attaches it, its figures kept then
check "after a restart" "$(units "" "SELECT count(*) FROM sq WHERE a <> 0")" "1"
check "after a restart: the count" "$(same "SELECT count(*) FROM sq WHERE a <> 0")" "same"
out=$(sql "DELETE FROM sq WHERE k = 61")
case "$out" in *ERROR*) echo "  delete: $out"; fail=1 ;; esac
check "a row deleted: read" "$(units "" "SELECT count(*) FROM sq WHERE a <> 0")" "0"
check "a row deleted: the count" "$(same "SELECT count(*) FROM sq WHERE a <> 0")" "same"

echo "== rows deleted among them, a part merged"
out=$(sql "DELETE FROM sp WHERE k % 122 = 0")
case "$out" in *ERROR*) echo "  delete: $out"; fail=1 ;; esac
check "deleted: a <> 0" "$(same "SELECT count(*), sum(v) FROM sp WHERE a <> 0")" "same"
out=$(sql "INSERT INTO sp SELECT g, CASE WHEN g % 7 = 0 THEN 1 ELSE 0 END, 0, '', g, false, (g % 100)::text FROM generate_series(300001, 320000) g")
case "$out" in *ERROR*) echo "  insert: $out"; fail=1 ;; esac
check "two parts: a <> 0" "$(same "SELECT count(*), sum(v) FROM sp WHERE a <> 0")" "same"

echo
if grep -qE "TRAP:|PANIC|terminated by signal" "$D/log"; then
	echo "sparse: the server log has failures"
	fail=1
fi
[ $fail -eq 0 ] && echo "sparse: every check passed" || echo "sparse: a check FAILED"
exit $fail
