#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# chte.sh's join keys (ch_table_engines.md §3, VC3): a hash join's inner
# keys lent to ch_table_engines' source on the outer side's mergetree table
# (vexec_source.h's set_join_keys(), minor 6), in both stores.  The join
# builds its table before it reads an outer row, and the scan gives the
# source the keys' range, and their values where there are at most 1,024;
# the extension ANDs them with the scan's quals into ClickHouse's filter for
# the units it claims from then on, so that the parts' summaries and
# statistics and the primary key pass over the parts and granules no inner
# row meets.  vexec still checks every row the source hands.
#
# The fact table, f_<store>, is ordered by k and written by five inserts --
# four parts of apart ranges of k, one that overlaps them -- with NULL keys
# and deleted rows; nf_<store> holds the same rows, its order tuple(), its
# parts' ranges apart.  The rows the source hands, as the relation's
# statistics count them in the transaction, are fewer with the keys than
# without for a selective join, and the same where none may be lent: a left
# or anti join, a qual that may raise, vexec.enable_source_join_keys off.
# The values only on a column of the table's key, as at most 32 ranges: keys
# of three clusters far apart read their clusters' granules, a tenth of the
# rows at most.  Through inner, semi and right joins; int2, int4 and int8 keys;
# more values than an IN list takes; a mergetree
# table on the inner side; a qual beside the keys; a rescanned join;
# parallel scans.  Every answer the same as with vexec off, and as with
# the keys off.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/lib.sh"
D="$(mktemp -d "${TMPDIR:-/tmp}/chte-joinkeys-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec,ch_table_engines'" "vexec.min_rows = 0" \
	"max_parallel_workers_per_gather = 0" "ch.max_merge_workers = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/joinkeys-server.log"' EXIT

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
same() {						# same <settings> <query>: vexec's answers, keys on and off, against off's
	local ref on off
	ref=$(answer "$1 SET vexec.mode = off;" "$2")
	on=$(answer "$1 SET vexec.mode = force;" "$2")
	off=$(answer "$1 SET vexec.mode = force; SET vexec.enable_source_join_keys = off;" "$2")
	if [ "$on" = "$ref" ] && [ "$off" = "$ref" ]; then
		echo "same"
	else
		echo "DIFFERENT (keys on ${on:0:12}, off ${off:0:12}, vexec off ${ref:0:12})"
	fi
}
handed() {						# handed <table> <settings> <query>: the rows the table's source handed, in force mode
	sql "$2 SET vexec.mode = force; SELECT handed('$1', \$q\$$3\$q\$)"
}
fewer() {						# fewer <table> <query>: the rows handed with the keys, against without
	local on off
	on=$(handed "$1" "" "$2")
	off=$(handed "$1" "SET vexec.enable_source_join_keys = off;" "$2")
	if [[ "$on" =~ ^[0-9]+$ ]] && [[ "$off" =~ ^[0-9]+$ ]] && [ "$on" -lt "$off" ]; then
		echo "fewer"
	else
		echo "not fewer ($on of $off)"
	fi
}
equal() {						# equal <table> <query>: the rows handed, the keys on as off
	local on off
	on=$(handed "$1" "" "$2")
	off=$(handed "$1" "SET vexec.enable_source_join_keys = off;" "$2")
	[ "$on" = "$off" ] && echo "equal" || echo "not equal ($on of $off)"
}
given() {						# given <settings> <query>: the scan's EXPLAIN lines of the keys
	local plan
	plan=$(sql "$1 SET vexec.mode = force; EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) $2")
	case "$plan" in
		*ERROR*) echo "$plan" | grep ERROR ;;
		*) echo "$plan" | sed -n 's/^ *Join Keys Given\( to Source\)\{0,1\}: //p' | paste -sd'/' ;;
	esac
}

out=$(sql "SET client_min_messages = warning; CREATE EXTENSION vexec; CREATE EXTENSION ch_table_engines;
	CREATE FUNCTION handed(rel regclass, q text) RETURNS int8 LANGUAGE plpgsql AS \$f\$
	DECLARE b int8;
	BEGIN
		b := pg_stat_get_xact_tuples_returned(rel);
		EXECUTE q;
		RETURN pg_stat_get_xact_tuples_returned(rel) - b;
	END \$f\$;
	CREATE TABLE dim (k int, k8 int8, k2 int2, tag text);
	INSERT INTO dim SELECT g, g, (g % 30000)::int2, CASE WHEN g BETWEEN 120000 AND 120050 THEN 'few'
		WHEN g % 97 = 0 THEN 'spread' ELSE 'other' END FROM generate_series(1, 400000, 3) g;
	INSERT INTO dim VALUES (NULL, NULL, NULL, 'few'), (999999, 999999, 32000, 'far');
	INSERT INTO dim SELECT g, g, (g % 30000)::int2, 'clusters'
		FROM generate_series(1, 400000) g WHERE g % 150000 BETWEEN 50000 AND 50030;
	ANALYZE dim;")
case "$out" in *ERROR*) echo "setup: $out"; exit 1 ;; esac

for store in pages files; do
	f="f_$store"; nf="nf_$store"; md="mdim_$store"
	echo "== VC3, the $store store"
	out=$(sql "SET client_min_messages = warning;
		CREATE TABLE $f (k int, k8 int8, k2 int2, a int, w text)
			USING mergetree WITH (order_by = 'k', storage = '$store', index_granularity = 1024);
		CREATE TABLE $nf (k int, k8 int8, k2 int2, a int, w text) USING mergetree WITH (storage = '$store');
		CREATE TABLE $md (k int, tag text) USING mergetree WITH (order_by = 'k', storage = '$store');
		INSERT INTO $md SELECT k, tag FROM dim;
		ANALYZE $md;")
	case "$out" in *ERROR*) echo "  $f: $out"; fail=1; continue ;; esac
	for t in "$f" "$nf"; do
		out=$(sql "INSERT INTO $t SELECT g, g, (g % 30000)::int2, g % 100, md5(g::text) FROM generate_series(1, 100000) g;
			INSERT INTO $t SELECT g, g, (g % 30000)::int2, g % 100, md5(g::text) FROM generate_series(100001, 200000) g;
			INSERT INTO $t SELECT g, g, (g % 30000)::int2, g % 100, md5(g::text) FROM generate_series(200001, 300000) g;
			INSERT INTO $t SELECT CASE WHEN g % 50 = 0 THEN NULL ELSE g END, g, (g % 30000)::int2, g % 100, md5(g::text)
				FROM generate_series(300001, 400000) g;
			INSERT INTO $t SELECT g, g, (g % 30000)::int2, g % 100, md5(g::text) FROM generate_series(7, 400000, 5000) g;
			DELETE FROM $t WHERE k BETWEEN 120010 AND 120020;
			ANALYZE $t;")
		case "$out" in *ERROR*) echo "  $t: $out"; fail=1 ;; esac
	done
	echo "  parts: $(sql "SELECT count(*) FROM ch.parts('$f')") of $f, $(sql "SELECT count(*) FROM ch.parts('$nf')") of $nf"

	few="SELECT f.k, f.a, f.w FROM $f f JOIN dim d ON f.k = d.k WHERE d.tag = 'few'"
	echo "  a selective join, the keys given: $(given "" "$few")"
	echo "  its rows handed: $(handed "$f" "" "$few") with the keys, $(handed "$f" "SET vexec.enable_source_join_keys = off;" "$few") without"
	check "$f: a selective join, the rows handed with the keys" "$(fewer "$f" "$few")" "fewer"
	check "$nf: its parts' ranges apart, unordered" \
		"$(fewer "$nf" "SELECT f.k, f.w FROM $nf f JOIN dim d ON f.k = d.k WHERE d.tag = 'few'")" "fewer"
	check "$f: a join to a mergetree table" \
		"$(fewer "$f" "SELECT f.k, d.tag FROM $f f JOIN $md d ON f.k = d.k WHERE d.tag = 'few'")" "fewer"
	cl="SELECT f.k, f.w FROM $f f JOIN dim d ON f.k = d.k WHERE d.tag = 'clusters'"
	read -r on <<< "$(handed "$f" "" "$cl")"
	read -r off <<< "$(handed "$f" "SET vexec.enable_source_join_keys = off;" "$cl")"
	echo "  keys of three clusters, the keys given: $(given "" "$cl"); rows handed $on with them, $off without"
	check "$f: three clusters, a tenth of the rows at most" \
		"$([[ "$on" =~ ^[0-9]+$ ]] && [[ "$off" =~ ^[0-9]+$ ]] && [ $((on * 10)) -le "$off" ] && echo yes || echo no)" "yes"
	check "$f: more values than an IN list takes, the range alone" \
		"$(fewer "$f" "SELECT f.k FROM $f f JOIN dim d ON f.k = d.k WHERE d.tag = 'other' AND d.k BETWEEN 50000 AND 60000")" "fewer"
	for qq in "$few" \
		"SELECT f.k, f.a FROM $f f JOIN dim d ON f.k = d.k WHERE d.tag = 'spread'" \
		"$cl" \
		"SELECT f.k, f.w FROM $nf f JOIN dim d ON f.k = d.k WHERE d.tag = 'clusters'" \
		"SELECT f.k, f.w FROM $f f JOIN dim d ON f.k8 = d.k8 WHERE d.tag IN ('few', 'far')" \
		"SELECT f.k, f.k2 FROM $f f JOIN dim d ON f.k2 = d.k2 WHERE d.tag = 'few'" \
		"SELECT count(*), sum(f.a) FROM $f f JOIN dim d ON f.k2 = d.k8 WHERE d.tag = 'far'" \
		"SELECT f.k, f.w FROM $f f JOIN dim d ON f.k = d.k WHERE d.tag = 'few' AND f.a < 50" \
		"SELECT k, w FROM $f WHERE k IN (SELECT k FROM dim WHERE tag = 'few')" \
		"SELECT f.k, d.k, d.tag FROM $f f RIGHT JOIN dim d ON f.k = d.k WHERE d.tag IN ('few', 'far')" \
		"SELECT f.k, d.tag FROM $f f JOIN $md d ON f.k = d.k WHERE d.tag = 'few'" \
		"SELECT f.k, f.w FROM $nf f JOIN dim d ON f.k = d.k WHERE d.tag = 'few'" \
		"SELECT f.k, d.tag FROM $f f LEFT JOIN dim d ON f.k = d.k WHERE f.a = 7 AND f.k < 130000" \
		"SELECT count(*) FROM $f f WHERE NOT EXISTS (SELECT 1 FROM dim d WHERE d.k = f.k AND d.tag = 'few')" \
		"SELECT f.k, f.w FROM $f f JOIN dim d ON f.k = d.k WHERE d.tag = 'few' AND f.k8 / (f.a + 1) >= 0" \
		"SELECT f.k FROM $f f JOIN dim d ON f.k = d.k WHERE d.tag = 'none'"; do
		check "$f: ${qq#SELECT }" "$(same "" "$qq")" "same"
	done
	# none lent: the rows handed the same
	check "$f: a left join, none lent" "$(equal "$f" "SELECT f.k, d.tag FROM $f f LEFT JOIN dim d ON f.k = d.k WHERE f.a = 7")" "equal"
	check "$f: a qual that may raise, none lent" \
		"$(equal "$f" "SELECT f.k FROM $f f JOIN dim d ON f.k = d.k WHERE d.tag = 'few' AND f.k8 / (f.a + 1) >= 0")" "equal"
	# a join rescanned for each outer row: its keys given again, each time
	rq="SELECT o.lim, (SELECT count(*) FROM $f f JOIN dim d ON f.k = d.k WHERE d.tag = 'few' AND d.k < o.lim) FROM (VALUES (120005), (120040), (5)) o(lim)"
	echo "  a rescanned join, the keys given: $(given "" "$rq")"
	check "$f, rescanned" "$(same "" "$rq")" "same"
	# in parallel, each participant's units
	par="SET max_parallel_workers_per_gather = 4; SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0; SET vexec.scan_rows_per_worker = 10000;"
	check "$f, in parallel" "$(same "$par" "$few")" "same"
	check "$f, in parallel, a semi join" "$(same "$par" "SELECT k, w FROM $f WHERE k IN (SELECT k FROM dim WHERE tag = 'few')")" "same"
done

echo
if grep -qE "TRAP:|PANIC|terminated by signal" "$D/log"; then
	echo "joinkeys: the server log has failures"
	fail=1
fi
[ $fail -eq 0 ] && echo "joinkeys: every check passed" || echo "joinkeys: a check FAILED"
exit $fail
