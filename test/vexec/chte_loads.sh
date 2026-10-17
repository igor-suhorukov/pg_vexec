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
ref=$(q "$D" postgres "$(printf "$sum" src)")
for store in pages files; do
	for load in copy insert vecinsert; do
		t="m_${load}_$store"
		out=$(q "$D" postgres "SET client_min_messages = warning;
			CREATE TABLE $t (LIKE src) USING mergetree WITH (storage = '$store', order_by = 'id');")
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
		for read in plain chscan vexec-postgres vexec-arrow; do
			case "$read" in
				plain) set="SET vexec.mode = off; SET ch.enable_chscan = off;" ;;
				chscan) set="SET vexec.mode = off;" ;;
				vexec-postgres) set="SET vexec.mode = force; SET vexec.batch_format = postgres;" ;;
				vexec-arrow) set="SET vexec.mode = force; SET vexec.batch_format = arrow;" ;;
			esac
			got=$(q "$D" postgres "$set $(printf "$sum" "$t")")
			if [ "$got" = "$ref" ]; then
				echo "  $t read by $read: the same as heap's"
			else
				echo "  $t read by $read: DIFFERENT: $got, heap's $ref"
				fail=1
			fi
		done
	done
done
exit $fail
