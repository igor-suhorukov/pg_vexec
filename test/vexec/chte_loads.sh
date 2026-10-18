#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# chte.sh's loads (ch_table_engines.md §3, "Done when"): a table of every
# type class -- NULLs at the bitmaps' word edges, long values, a numeric's
# NaN in one batch -- loaded into mergetree tables of both stores three
# ways: COPY of its binary dump; INSERT ... SELECT with vexec off, through
# the access method's multi_insert; and INSERT ... SELECT in vexec's force
# mode, VecInsert writing through the extension's sink.  Every table read
# back four ways -- the plain sequential scan, ChScan, and vexec's source in
# force mode in both formats -- and every read of every load must be the
# heap table's rows; VecInsert's batches must have gone through the sink.
# Then each table's rows are deleted and updated as a heap copy's are (the
# extension's CH2), vexec in force mode beneath the statements, and every
# read must be the copy's rows.  Each read is also asked the aggregates a
# source may answer from a unit's statistics -- count(*) alone, which asks
# a source for no more than a unit's rows, then count, min and max -- and
# they must be heap's too.  Then the same again for tables whose varlena
# columns are LowCardinality (option low_cardinality, VC5), read by vexec
# with vexec.batch_dictionary off and on: the source hands those columns as
# their dictionaries, and queries over them -- quals, targets, groups, a
# sort and a join of such a column -- must give heap's rows.
#
#   VEXEC_ROWS   the table's rows: 30000
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/lib.sh"
ROWS="${VEXEC_ROWS:-30000}"
D="$(mktemp -d "${TMPDIR:-/tmp}/chte-loads-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec,ch_table_engines'" "vexec.min_rows = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/loads-server.log"' EXIT

out=$(q "$D" postgres "SET client_min_messages = warning; CREATE EXTENSION vexec; CREATE EXTENSION ch_table_engines;
CREATE TABLE src AS
SELECT g AS id,
       (g % 30000 - 15000)::int2 AS i2, (g * 7919) % 100003 - 50000 AS i4, g::int8 * 1000003 - 9000000000 AS i8,
       (g / 7.0)::float4 AS f4, CASE WHEN g % 997 = 0 THEN 'NaN'::float8 ELSE g / 13.0 END AS f8,
       (CASE WHEN g = 777 THEN 'NaN' ELSE (g % 1000) / 7.0 END)::numeric(12,4) AS n,
       (g % 100 / 3.0)::numeric(6,2) AS n32, (g * 1.5)::numeric AS nfree,
       'name ' || (g % 503) AS t, ('x' || g)::varchar(20) AS v, lpad((g % 97)::text, 3)::char(6) AS bp,
       DATE '2020-01-01' + g % 1000 AS d, TIMESTAMP '2020-01-01' + g * interval '1 minute' AS ts,
       TIMESTAMPTZ '2020-01-01 00:00:00+00' + g * interval '1 second' AS tz, time '00:00' + g * interval '1 second' AS tm,
       g % 3 = 0 AS b, md5(g::text)::uuid AS u, decode(md5(g::text), 'hex') AS by,
       CASE WHEN g % 50 = 0 THEN repeat(md5(g::text), 400) ELSE repeat('ab', g % 40) END AS longt,
       (g % 10)::\"char\" AS ch, g::oid AS o, ('{' || g % 5 || ',' || g % 7 || '}')::int[] AS arr,
       jsonb_build_object('k', g % 11) AS j, (g % 13) * interval '1 hour' AS iv
FROM generate_series(1, $ROWS) g;
UPDATE src SET i2 = NULL, f8 = NULL, t = NULL, d = NULL, u = NULL WHERE id % 64 IN (0, 63, 1);
UPDATE src SET i4 = NULL, n = NULL, b = NULL, ts = NULL, by = NULL WHERE id % 101 = 7;
COPY src TO '$D/src.copy' (FORMAT binary);")
case "$out" in *ERROR*) echo "the data: $out"; exit 1 ;; esac

fail=0
sum="SELECT md5(string_agg(x::text, '|' ORDER BY x.id)) FROM (SELECT * FROM %s) x"
aggs() {						# aggs <settings> <table>: count(*) alone, then the others
	echo "$(q "$D" postgres "$1 SELECT count(*) FROM $2")|$(q "$D" postgres "$1 SELECT count(i4), min(i4), max(i4), min(d), max(ts) FROM $2")"
}
ref=$(q "$D" postgres "$(printf "$sum" src)")
ref_aggs=$(aggs "" src)
# the deletions and updates, and a heap copy's rows after them
dml="DELETE FROM %t WHERE id % 7 = 0; UPDATE %t SET t = coalesce(t, '') || '!', i4 = i4 + 1, n = NULL WHERE id % 11 = 0;"
out=$(q "$D" postgres "CREATE TABLE src_dml AS SELECT * FROM src; ${dml//%t/src_dml}")
case "$out" in *ERROR*) echo "the heap copy's deletions and updates: $out"; exit 1 ;; esac
ref_dml=$(q "$D" postgres "$(printf "$sum" src_dml)")
ref_dml_aggs=$(aggs "" src_dml)

READS="plain chscan vexec-postgres vexec-arrow"
reads() {						# reads <table> <heap's rows> <heap's aggregates> <what>: every read of it
	local read set got
	for read in $READS; do
		case "$read" in
			plain) set="SET vexec.mode = off; SET ch.enable_chscan = off;" ;;
			chscan) set="SET vexec.mode = off;" ;;
			vexec-postgres) set="SET vexec.mode = force; SET vexec.batch_format = postgres; SET vexec.batch_dictionary = off;" ;;
			vexec-arrow) set="SET vexec.mode = force; SET vexec.batch_format = arrow; SET vexec.batch_dictionary = off;" ;;
			dict-postgres) set="SET vexec.mode = force; SET vexec.batch_format = postgres; SET vexec.batch_dictionary = on;" ;;
			dict-arrow) set="SET vexec.mode = force; SET vexec.batch_format = arrow; SET vexec.batch_dictionary = on;" ;;
			dict-datums) set="SET vexec.mode = force; SET vexec.enable_source_layouts = off; SET vexec.batch_dictionary = on;" ;;
		esac
		got=$(q "$D" postgres "$set $(printf "$sum" "$1")")
		if [ "$got" = "$2" ]; then
			echo "  $1 read by $read$4: the same as heap's"
		else
			echo "  $1 read by $read$4: DIFFERENT: $got, heap's $2"
			fail=1
		fi
		got=$(aggs "$set" "$1")
		if [ "$got" != "$3" ]; then
			echo "  $1 read by $read$4: aggregates DIFFERENT: $got, heap's $3"
			fail=1
		fi
	done
}
# queries over LowCardinality columns: quals, targets, groups, a sort and a join
QUERIES=(
	"SELECT count(*), count(t), count(DISTINCT t) FROM %t WHERE t <> 'name 7' AND t LIKE 'name 1%'"
	"SELECT t, count(*), min(id), max(v) FROM %t GROUP BY t"
	"SELECT id, upper(t), length(longt), bp || '|', j->>'k' FROM %t WHERE id % 1009 = 3"
	"SELECT nfree, count(*) FROM %t WHERE nfree::text LIKE '%.5' GROUP BY nfree"
	"SELECT t, id FROM %t WHERE t IS NULL OR t IN ('name 1', 'name 500') ORDER BY t DESC NULLS FIRST, id LIMIT 20"
	"SELECT a.id, b.t FROM %t a JOIN %t b ON a.t = b.t AND a.id = b.id + 503 WHERE a.id < 3000"
)
queries() {						# queries <table> <what>: QUERIES, by vexec with dictionaries and without, against heap's
	local k ref_q read set got
	for k in "${!QUERIES[@]}"; do
		ref_q=$(q "$D" postgres "SET vexec.mode = off; SELECT md5(coalesce(string_agg(x::text, '|' ORDER BY x::text), '')) FROM (${QUERIES[$k]//%t/src_q}) x")
		for read in vexec-postgres dict-postgres dict-arrow dict-datums; do
			case "$read" in
				vexec-postgres) set="SET vexec.mode = force; SET vexec.batch_dictionary = off;" ;;
				dict-postgres) set="SET vexec.mode = force; SET vexec.batch_dictionary = on;" ;;
				dict-arrow) set="SET vexec.mode = force; SET vexec.batch_format = arrow; SET vexec.batch_dictionary = on;" ;;
				dict-datums) set="SET vexec.mode = force; SET vexec.enable_source_layouts = off; SET vexec.batch_dictionary = on;" ;;
			esac
			got=$(q "$D" postgres "$set SELECT md5(coalesce(string_agg(x::text, '|' ORDER BY x::text), '')) FROM (${QUERIES[$k]//%t/$1}) x")
			if [ "$got" != "$ref_q" ]; then
				echo "  $1, query $((k + 1)) read by $read$2: DIFFERENT: $got, heap's $ref_q"
				fail=1
			fi
		done
	done
	echo "  $1: ${#QUERIES[@]} queries over its LowCardinality columns$2, each read 4 ways"
}
LC="t, v, bp, nfree, by, longt, arr, j"
for lc in off on; do
for store in pages files; do
	for load in copy insert vecinsert; do
		t="m_${load}_$store"
		with=""
		if [ $lc = on ]; then
			t="${t}_lc"
			with=", low_cardinality = '$LC'"
			READS="plain chscan vexec-postgres dict-postgres dict-arrow dict-datums"
		fi
		out=$(q "$D" postgres "SET client_min_messages = warning;
			CREATE TABLE $t (LIKE src) USING mergetree WITH (storage = '$store', order_by = 'id'$with);")
		case "$out" in *ERROR*) echo "  $t: $out"; fail=1; continue ;; esac
		case "$load" in
			copy) out=$(q "$D" postgres "COPY $t FROM '$D/src.copy' (FORMAT binary)") ;;
			insert) out=$(q "$D" postgres "SET vexec.mode = off; INSERT INTO $t SELECT * FROM src") ;;
			vecinsert)
				out=$(q "$D" postgres "SET vexec.mode = force; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) INSERT INTO $t SELECT * FROM src")
				sink=$(echo "$out" | sed -n 's/.*Batches Through the Sink: *\([0-9]*\).*/\1/p')
				rows=$(echo "$out" | sed -n 's/.*Batches Through table_multi_insert: *\([0-9]*\).*/\1/p')
				echo "  $t: $out" | grep -q "Write: sink MergeTree" || { echo "  $t: VecInsert did not write through the sink: $out"; fail=1; }
				echo "  $t: VecInsert's batches: ${sink:-0} through the sink, ${rows:-0} by table_multi_insert (a numeric's NaN)"
				[ "${sink:-0}" -gt 0 ] || fail=1 ;;
		esac
		case "$out" in *ERROR*) echo "  $t: $out"; fail=1; continue ;; esac
		reads "$t" "$ref" "$ref_aggs" ""
		if [ $lc = on ]; then
			# the source handed dictionaries: a scan of a LowCardinality column says so
			dicts=$(q "$D" postgres "SET vexec.mode = force; SET vexec.batch_dictionary = on;
				EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
				SELECT count(*) FROM $t WHERE t <> 'name 7'" | sed -n 's/.*Batches With Dictionaries: *\([0-9]*\).*/\1/p')
			echo "  $t: batches of its scan with dictionaries: ${dicts:-0}"
			[ "${dicts:-0}" -gt 0 ] || fail=1
			q "$D" postgres "DROP TABLE IF EXISTS src_q; CREATE TABLE src_q AS SELECT * FROM src" > /dev/null
			queries "$t" ""
		fi
		out=$(q "$D" postgres "SET vexec.mode = force; SET vexec.batch_dictionary = $lc; ${dml//%t/$t}")
		case "$out" in *ERROR*) echo "  $t, its deletions and updates: $out"; fail=1; continue ;; esac
		reads "$t" "$ref_dml" "$ref_dml_aggs" " after deletions and updates"
		if [ $lc = on ]; then
			q "$D" postgres "DROP TABLE IF EXISTS src_q; CREATE TABLE src_q AS SELECT * FROM src_dml" > /dev/null
			queries "$t" " after deletions and updates"
		fi
	done
done
done
exit $fail
