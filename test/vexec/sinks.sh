#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The batch sinks (pg_vector_executor.md §3.16, §5 VI): one table in each
# storage -- heap and ao_row through table_multi_insert(), gp_ao's ao_column
# and PAX's porc and porc_vec through their registered sinks -- on a single
# node of the port with gp_core, gp_orca, gp_sql, gp_ao, PAX and vexec
# preloaded, loaded by VecInsert and compared, row for row, with the same
# table loaded by COPY from the same data and by ModifyTable:
#
#   loads    VecInsert under PostgreSQL's planner in force mode in the
#            PostgreSQL and the Arrow formats, and under ORCA, whose
#            translation builds it; a row node's rows gathered into batches;
#            the client's stream through VecIngest (vexec_test's
#            ingest_begin(), as vexec_flight hands a stream to vexec); each
#            storage's VecInsert naming its sink in EXPLAIN, or
#            table_multi_insert()
#   errors   NOT NULL, CHECK, a unique index's violation and a domain's
#            check, in SQLSTATE, message and detail as ModifyTable raises
#            them, and the table as it was after each
#   aborts   a statement that fails in the middle of a batch, and one
#            cancelled by a statement timeout, leave the table as it was
#   indexes  a unique index's entries, made from the sinks' TIDs, find each
#            row loaded
#
# The data holds every type class of §3.4 with NULLs at the bitmaps' word
# edges, values long enough to be compressed and to be stored out of line,
# and a dropped column.  A table is compared through each row's text, ordered
# by its key: json has no equality.
#
#   VEXEC_ROWS        the table's rows: 30000
#   VEXEC_STORAGES    "heap ao_row ao_column pax pax_porc_vec"
#   VEXEC_SINKS       "ao_column=gp_ao pax=pax pax_porc_vec=pax": the sink
#                     each storage's VecInsert must name, table_multi_insert
#                     for one not listed
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
. "$here/lib.sh"

ROWS="${VEXEC_ROWS:-30000}"
STORAGES="${VEXEC_STORAGES:-heap ao_row ao_column pax pax_porc_vec}"
SINKS="${VEXEC_SINKS:-ao_column=gp_ao pax=pax pax_porc_vec=pax}"
D="$(mktemp -d "${TMPDIR:-/tmp}/vexec-sinks-XXXXXX")"
fail=0

server_init "$D" "shared_preload_libraries = 'gp_core,gp_orca,gp_sql,gp_ao,pax,vexec'" \
	"max_parallel_workers_per_gather = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/sinks-server.log"' EXIT

out=$(q "$D" postgres "SET client_min_messages = warning; CREATE EXTENSION gp_ao CASCADE; CREATE EXTENSION gp_orca; CREATE EXTENSION gp_sql; CREATE EXTENSION pax; CREATE EXTENSION vexec; CREATE EXTENSION vexec_test")
case "$out" in *ERROR*) echo "the extensions: $out"; exit 1 ;; esac
echo "the batch sinks: $(q "$D" postgres "SELECT string_agg(sink || ' for ' || coalesce(access_method, '?'), ', ' ORDER BY sink) FROM vexec.sinks()")"

ok() { echo "  ok $1"; }
notok() { echo "  FAILED $1"; [ $# -gt 1 ] && printf '%s\n' "$2" | sed 's/^/      /'; fail=1; }

storage_clause() {
	case "$1" in
		heap) echo "USING heap" ;;
		ao_row) echo "USING ao_row WITH (compresstype=zlib, compresslevel=1)" ;;
		ao_column) echo "USING ao_column WITH (compresstype=zstd, blocksize=32768)" ;;
		pax) echo "USING pax WITH (storage_format=porc)" ;;
		pax_porc_vec) echo "USING pax WITH (storage_format=porc_vec)" ;;
	esac
}

# the data: every type class, NULLs at the bitmaps' word edges, long values
# compressed and stored out of line, a dropped column
q "$D" postgres "SET client_min_messages = warning;
CREATE TABLE src AS
SELECT g AS id,
       (g % 30000 - 15000)::int2 AS i2, (g * 7919) % 100003 - 50000 AS i4,
       g::int8 * 1000003 - 9000000000 AS i8,
       (g / 7.0)::float4 AS f4, CASE WHEN g % 997 = 0 THEN 'NaN'::float8 ELSE g / 13.0 END AS f8,
       ((g % 1000) / 7.0)::numeric(12,4) AS n, (g * 1.5)::numeric AS nfree,
       'name ' || (g % 503) AS t, ('x' || g)::varchar(20) AS v, lpad((g % 97)::text, 3)::char(6) AS bp,
       DATE '2020-01-01' + g % 1000 AS d, TIMESTAMP '2020-01-01' + g * interval '1 minute' AS ts,
       TIMESTAMPTZ '2020-01-01 00:00:00+00' + g * interval '1 second' AS tz,
       g % 3 = 0 AS b, md5(g::text)::uuid AS u,
       CASE WHEN g % 50 = 0 THEN repeat(md5(g::text), 400) WHEN g % 50 = 1 THEN repeat('compress ', 300)
            ELSE repeat('ab', g % 40) END AS longt,
       (g % 10)::\"char\" AS ch, g::oid AS o, ('{' || g % 5 || ',' || g % 7 || '}')::int[] AS arr,
       jsonb_build_object('k', g % 11) AS j, (g % 13) * interval '1 hour' AS iv,
       decode(lpad(to_hex(g), 8, '0'), 'hex') AS by, '08:00:2b:01:02:03'::macaddr AS mac,
       (g % 128)::int2 AS gone
FROM generate_series(1, $ROWS) g;
UPDATE src SET i2 = NULL, f8 = NULL, t = NULL, d = NULL, u = NULL, by = NULL WHERE id % 64 IN (0, 63, 1);
UPDATE src SET i4 = NULL, n = NULL, b = NULL, ts = NULL, mac = NULL WHERE id % 101 = 7;
ALTER TABLE src ALTER COLUMN longt SET STORAGE EXTERNAL;
UPDATE src SET longt = longt || '' WHERE id % 50 = 0;
-- porc_vec keeps a numeric only with a typmod of precision 35 at most
CREATE TABLE src_pv AS SELECT * FROM src;
ALTER TABLE src_pv ALTER COLUMN nfree TYPE numeric(30,4);
ANALYZE src;
ANALYZE src_pv;
COPY src TO '$D/src.csv' (FORMAT csv);
COPY src_pv TO '$D/src_pv.csv' (FORMAT csv);" > "$D/data.out"
grep -q ERROR "$D/data.out" && { echo "the data: $(cat "$D/data.out")"; exit 1; }

# a table's rows, as text, ordered by id: what two loads compare by
digest() { q "$D" postgres "SELECT count(*) || ' ' || md5(string_agg(r::text, '|' ORDER BY r.id)) FROM $1 r"; }

LOADS=("planner-postgres" "planner-arrow" "orca-postgres" "orca-arrow" "rows" "stream")
load_opts() {
	case "$1" in
		planner-postgres) echo "-c gp.optimizer=off -c vexec.mode=force -c vexec.batch_format=postgres" ;;
		planner-arrow) echo "-c gp.optimizer=off -c vexec.mode=force -c vexec.batch_format=arrow" ;;
		orca-postgres) echo "-c gp.optimizer=on -c vexec.mode=force -c vexec.batch_format=postgres" ;;
		orca-arrow) echo "-c gp.optimizer=on -c vexec.mode=force -c vexec.batch_format=arrow" ;;
		rows|stream) echo "-c gp.optimizer=off -c vexec.mode=force" ;;
		off) echo "-c gp.optimizer=off -c vexec.mode=off" ;;
	esac
}

for s in $STORAGES; do
	from=src
	[ "$s" = pax_porc_vec ] && from=src_pv
	cols=$(q "$D" postgres "SELECT string_agg(quote_ident(attname), ', ' ORDER BY attnum) FROM pg_attribute WHERE attrelid = '$from'::regclass AND attnum > 0 AND NOT attisdropped AND attname <> 'gone'")
	want="table_multi_insert"
	for m in $SINKS; do
		[ "${m%%=*}" = "$s" ] && want="sink ${m#*=}"
	done

	# the references: COPY, and ModifyTable
	out=$(q "$D" postgres "SET client_min_messages = warning;
		CREATE TABLE ref_copy_$s (LIKE $from) $(storage_clause "$s");
		COPY ref_copy_$s FROM '$D/$from.csv' (FORMAT csv);
		ALTER TABLE ref_copy_$s DROP COLUMN gone;
		CREATE TABLE ref_mt_$s (LIKE ref_copy_$s) $(storage_clause "$s");")
	case "$out" in *ERROR*) notok "$s: its reference tables" "$out"; continue ;; esac
	PGOPTIONS="$(load_opts off)" q "$D" postgres "INSERT INTO ref_mt_$s SELECT $cols FROM $from" > /dev/null
	ref=$(digest "ref_copy_$s")
	if [ "$(digest "ref_mt_$s")" = "$ref" ]; then
		ok "$s: ModifyTable's load equals COPY's"
	else
		notok "$s: ModifyTable's load differs from COPY's"
	fi

	plan=$(PGOPTIONS="$(load_opts planner-postgres)" q "$D" postgres "EXPLAIN (COSTS OFF) INSERT INTO ref_mt_$s SELECT $cols FROM $from")
	if echo "$plan" | grep -q "Vec Insert on ref_mt_$s" && echo "$plan" | grep -q "Write: $want"; then
		ok "$s: VecInsert writes through $want"
	else
		notok "$s: VecInsert does not write through $want" "$plan"
	fi

	for l in "${LOADS[@]}"; do
		t="t_${s}_${l//-/_}"
		q "$D" postgres "SET client_min_messages = warning; CREATE TABLE $t (LIKE ref_copy_$s) $(storage_clause "$s")" > /dev/null
		case "$l" in
			rows)
				# a row node's rows, a CTE Scan's, gathered into batches
				sql="WITH x AS MATERIALIZED (SELECT $cols FROM $from) INSERT INTO $t SELECT * FROM x" ;;
			stream)
				# the client's stream: the source's values that Arrow holds
				# as they are, the rest as text cast back by the INSERT
				sql="BEGIN; SELECT handle FROM vexec_test.ingest_begin((SELECT stream FROM vexec_test.egress(\$q\$SELECT id, i2, i4, i8, f4, f8, n, nfree::text, t, v, bp::text, d, ts, tz, b, u, longt, ch::text, o::int8, arr::text, j::text, iv::text, by, mac::text FROM $from\$q\$))) \\gset
INSERT INTO $t SELECT id, i2, i4, i8, f4, f8, n, nfree::numeric, t, v, bp, d, ts, tz, b, u, longt, ch::\"char\", o::oid, arr::int[], j::jsonb, iv::interval, by, mac::macaddr FROM vexec.ingest_stream(:handle) AS s(id int4, i2 int2, i4 int4, i8 int8, f4 float4, f8 float8, n numeric(12,4), nfree text, t text, v text, bp text, d date, ts timestamp, tz timestamptz, b bool, u uuid, longt text, ch text, o int8, arr text, j text, iv text, by bytea, mac text); COMMIT;" ;;
			*)
				sql="INSERT INTO $t SELECT $cols FROM $from" ;;
		esac
		out=$(printf '%s\n' "$sql" | PGOPTIONS="$(load_opts "$l")" "$BINDIR/psql" -X -q -At -h "$D/sock" -U postgres -d postgres 2>&1)
		case "$out" in *ERROR*) notok "$s, $l: the load" "$out"; continue ;; esac
		if [ "$(digest "$t")" = "$ref" ]; then
			ok "$s, $l: VecInsert's load equals COPY's, row for row"
		else
			notok "$s, $l: VecInsert's load differs from COPY's"
		fi
	done
	# under ORCA, its translation's VecInsert, with its sink
	plan=$(PGOPTIONS="$(load_opts orca-postgres)" q "$D" postgres "EXPLAIN (COSTS OFF) INSERT INTO ref_mt_$s SELECT $cols FROM $from")
	if echo "$plan" | grep -q "Optimizer: GPORCA" && echo "$plan" | grep -q "Vec Insert on ref_mt_$s" &&
		echo "$plan" | grep -q "Write: $want"; then
		ok "$s: under ORCA, its translation's VecInsert writes through $want"
	else
		notok "$s: under ORCA, no VecInsert writing through $want" "$plan"
	fi

	# errors: each as ModifyTable raises it, and the table as it was -- each
	# statement into tables of its own, as an aborted insert leaves index
	# entries of its rows, which PAX cannot have btree delete
	# (index_delete_tuples: "not supported on pax relations")
	errtable() {				# errtable <name>: a fresh table, a unique index
		q "$D" postgres "SET client_min_messages = warning;
			DROP TABLE IF EXISTS $1;
			CREATE TABLE $1 (id int4 NOT NULL, a int4 CHECK (a < 900), k dpos_$s, t text) $(storage_clause "$s");
			CREATE UNIQUE INDEX ON $1 (id);" > /dev/null
	}
	errors() {					# errors <options> <sql>: SQLSTATE, message, detail
		PGOPTIONS="$1 -c client_min_messages=warning" "$BINDIR/psql" -X -q -At -h "$D/sock" -U postgres -d postgres \
			-c "\\set VERBOSITY verbose" -c "$2" 2>&1 | grep -E '^(ERROR|DETAIL)'
	}
	q "$D" postgres "CREATE DOMAIN dpos_$s AS int4 CHECK (VALUE > -10)" > /dev/null
	n=0
	for sql in \
		"INSERT INTO %e SELECT CASE WHEN g = 2222 THEN NULL ELSE g END, 1, 1, 'x' || g FROM generate_series(1, 3000) g" \
		"INSERT INTO %e SELECT g, CASE WHEN g = 1777 THEN 5000 ELSE 1 END, 1, 'x' FROM generate_series(1, 3000) g" \
		"INSERT INTO %e SELECT g, 1, CASE WHEN g = 2999 THEN -50 ELSE 1 END, 'x' FROM generate_series(1, 3000) g" \
		"INSERT INTO %e SELECT g % 2500, 1, 1, 'x' FROM generate_series(1, 3000) g" \
		"INSERT INTO %e SELECT g, 1, 1, repeat('y', 100) || (g / (g - 1600)) FROM generate_series(1, 3000) g"; do
		n=$((n + 1))
		errtable "e_mt_$s"
		errtable "e_vec_$s"
		ref_err=$(errors "$(load_opts off)" "${sql//%e/e_mt_$s}" | sed "s/e_mt_$s/%e/g")
		got=$(errors "-c gp.optimizer=off -c vexec.mode=force" "${sql//%e/e_vec_$s}" | sed "s/e_vec_$s/%e/g")
		count=$(q "$D" postgres "SELECT count(*) FROM e_vec_$s")
		if [ -n "$got" ] && [ "$got" = "$ref_err" ] && [ "$count" = 0 ]; then
			ok "$s: error $n as ModifyTable raises it, the table as it was: $(echo "$got" | head -1 | cut -c1-80)"
		else
			notok "$s: error $n" "vecinsert: $got
modifytable: $ref_err
rows left: $count"
		fi
	done

	# an abort by a statement timeout, in the middle of the load
	errtable "e_vec_$s"
	got=$(PGOPTIONS="-c gp.optimizer=off -c vexec.mode=force -c statement_timeout=300" q "$D" postgres "INSERT INTO e_vec_$s SELECT g, 1, 1, md5(g::text) FROM generate_series(1, 20000000) g")
	count=$(q "$D" postgres "SELECT count(*) FROM e_vec_$s")
	case "$got" in
		*"canceling statement due to statement timeout"*)
			if [ "$count" = 0 ]; then
				ok "$s: a load cancelled in its middle leaves the table as it was"
			else
				notok "$s: a cancelled load left $count rows"
			fi ;;
		*) notok "$s: the load was not cancelled" "$got" ;;
	esac

	# the unique index's entries, made from the sink's TIDs, find each row
	errtable "e_vec_$s"
	PGOPTIONS="-c gp.optimizer=off -c vexec.mode=force" q "$D" postgres "INSERT INTO e_vec_$s SELECT g, 1, 1, 'r' || g FROM generate_series(1, 5000) g" > /dev/null
	got=$(q "$D" postgres "SET enable_seqscan = off; SET enable_bitmapscan = off; SELECT count(*) FROM generate_series(1, 5000) g WHERE (SELECT t FROM e_vec_$s WHERE id = g) = 'r' || g")
	if [ "$got" = 5000 ]; then
		ok "$s: the unique index finds each of 5,000 rows by its entry"
	else
		notok "$s: the unique index finds $got of 5,000 rows"
	fi
done

echo
if [ $fail -eq 0 ]; then
	echo "sinks: every load and every check passed"
else
	echo "sinks: a check FAILED"
fi
exit $fail
