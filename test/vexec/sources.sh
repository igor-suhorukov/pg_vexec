#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The batch sources (pg_vector_executor.md §3.5, §5 V1): one table in each
# storage -- heap through the slot path, gp_ao's ao_row and ao_column, PAX's
# porc and porc_vec through their registered batch readers -- on a single
# node of the port with gp_core, gp_orca, gp_ao, PAX and vexec preloaded,
# and every query of the corpus answered alike with vexec.mode off, in force
# mode in the PostgreSQL format, in force mode in the Arrow format, and in
# force mode with the per-structure layouts drawn at random; and under ORCA,
# where a Result over a scan becomes a VecResult, in force mode in both
# formats alike with ORCA's plan in off mode.  In force mode each storage's
# scan must name its source in EXPLAIN (VERBOSE).
#
# A query the reference answers with an error fails the run, unless it says
# it means to: its text begins with "/* error */".  So a table a module's
# queries never made fails, rather than an error every session repeats.
#
# V3: hash joins over each storage's batches, of each join type, keys of
# each layout.
#
# V4: heap read through heap's page reader, and through the slot path in a
# session of its own (vexec.heap_page_reader = off); every storage's scan
# in parallel, a session with workers whose VecScans share each table among
# a Gather's participants through its source; and sorts, VecSort under both
# planners, bounded by a LIMIT.
#
# The table holds every type class of §3.4 with NULLs at the bitmaps' word
# edges, rows deleted, values long enough to be compressed and to be stored
# out of line, a dropped column and one added after the rows were written.
# A storage module's own queries are in sources/<module>.queries, a query a
# line, %t its table.
#
# H2: on PAX, aggregates answered from its statistics, a file or a group at
# a time (vexec.aggregate_statistics): a table keeping min and max of its
# integer and time columns, in files of many groups, answers alike with
# vexec off, with the statistics, without them and under ORCA -- before and
# after rows are deleted, with a column added since, a column of NULLs, an
# aggregate the statistics cannot give and a qual -- and its plans show the
# units the statistics answered, their rows counted as the scan's.
#
# H6: a VecSort's running bound over each storage's scan, a table of rows
# in the order of their sort key: every session answers alike, ties at the
# bound's key, NULLs first and last, and an error PostgreSQL raises past the
# bound among them; and on PAX, which keeps the key's minimum and maximum,
# the groups past the bound are passed over unread (vexec_source.h,
# set_keys): the rows its source hands the scan -- those it keeps and those
# it drops by the bound -- are then fewer than two groups', where the other
# storages hand the table's and the scan drops nearly all of them.
#
#   VEXEC_ROWS        the table's rows: 30000
#   VEXEC_STORAGES    "heap ao_row ao_column pax pax_porc_vec"
#   VEXEC_SOURCES     "ao_row=gp_ao ao_column=gp_ao pax=pax pax_porc_vec=pax":
#                     the source each storage's VecScan must name (heap's
#                     "heap's pages", and "the slot path" for one not listed)
#   VEXEC_SEED        the random session's seed: drawn and printed
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
. "$here/lib.sh"

ROWS="${VEXEC_ROWS:-30000}"
STORAGES="${VEXEC_STORAGES:-heap ao_row ao_column pax pax_porc_vec}"
SOURCES="${VEXEC_SOURCES:-ao_row=gp_ao ao_column=gp_ao pax=pax pax_porc_vec=pax}"
SEED="${VEXEC_SEED:-$(( (RANDOM << 15 | RANDOM) % 2147483646 + 1 ))}"
D="$(mktemp -d "${TMPDIR:-/tmp}/vexec-sources-XXXXXX")"
fail=0

server_init "$D" "shared_preload_libraries = 'gp_core,gp_orca,gp_sql,gp_ao,pax,vexec'" \
	"max_parallel_workers_per_gather = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/sources-server.log"' EXIT

out=$(q "$D" postgres "SET client_min_messages = warning; CREATE EXTENSION gp_ao CASCADE; CREATE EXTENSION gp_orca; CREATE EXTENSION gp_sql; CREATE EXTENSION pax; CREATE EXTENSION vexec")
case "$out" in *ERROR*) echo "the extensions: $out"; exit 1 ;; esac
echo "the batch sources: $(q "$D" postgres "SELECT string_agg(source || ' for ' || coalesce(access_method, '?'), ', ' ORDER BY source) FROM vexec.sources()")"
echo "  random session's seed: $SEED (VEXEC_SEED=$SEED reruns it)"

storage_clause() {
	case "$1" in
		heap) echo "USING heap" ;;
		ao_row) echo "USING ao_row WITH (compresstype=zlib, compresslevel=1)" ;;
		ao_column) echo "USING ao_column WITH (compresstype=zstd, blocksize=32768)" ;;
		pax) echo "USING pax WITH (storage_format=porc)" ;;
		pax_porc_vec) echo "USING pax WITH (storage_format=porc_vec)" ;;
	esac
}

# the data: every type class, NULLs at the bitmaps' word edges (rows 63, 64,
# 65 of each 64-row word), long values, a dropped column and an added one
cat > "$D/data.sql" <<SQL
SET client_min_messages = warning;
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
       CASE WHEN g % 50 = 0 THEN repeat(md5(g::text), 400) ELSE repeat('ab', g % 40) END AS longt,
       (g % 10)::"char" AS ch, g::oid AS o, ('{' || g % 5 || ',' || g % 7 || '}')::int[] AS arr,
       jsonb_build_object('k', g % 11) AS j, (g % 13) * interval '1 hour' AS iv,
       (g % 128)::int2 AS gone
FROM generate_series(1, $ROWS) g;
UPDATE src SET i2 = NULL, f8 = NULL, t = NULL, d = NULL, u = NULL WHERE id % 64 IN (0, 63, 1);
UPDATE src SET i4 = NULL, n = NULL, b = NULL, ts = NULL WHERE id % 101 = 7;
-- porc_vec keeps a numeric only with a typmod of precision 35 at most
CREATE TABLE src_pv AS SELECT * FROM src;
ALTER TABLE src_pv ALTER COLUMN nfree TYPE numeric(30,4);
SQL
q "$D" postgres "$(cat "$D/data.sql")" > /dev/null
for s in $STORAGES; do
	from=src
	[ "$s" = pax_porc_vec ] && from=src_pv
	out=$(q "$D" postgres "SET client_min_messages = warning;
		CREATE TABLE t_$s (LIKE $from) $(storage_clause "$s");
		INSERT INTO t_$s SELECT * FROM $from;
		ALTER TABLE t_$s DROP COLUMN gone;
		ALTER TABLE t_$s ADD COLUMN later int DEFAULT 42;
		INSERT INTO t_$s (id, i4, t) SELECT $ROWS + g, g, 'late ' || g FROM generate_series(1, 100) g;
		DELETE FROM t_$s WHERE id % 97 = 0;
		ANALYZE t_$s;")
	case "$out" in *ERROR*) echo "the table in $s: $out"; fail=1 ;; esac
done

# the queries; %t is each storage's table
QUERIES=(
	"SELECT count(*), count(i2), count(t), count(u), count(later) FROM %t"
	"SELECT md5(string_agg(concat_ws(',', id, i2, i4, i8, f4, f8, n, nfree, t, v, bp, d, ts, tz, b, u, length(longt), ch, o, arr, j, iv, later), '|' ORDER BY id)) FROM %t"
	"SELECT id, i4 + 1, i8 * 2, f8 / 2, n * 2, t, d FROM %t WHERE i4 > 49000 ORDER BY id"
	"SELECT count(*) FROM %t WHERE i2 BETWEEN -100 AND 100 AND b"
	"SELECT count(*) FROM %t WHERE t LIKE '%e 4%' OR t IS NULL"
	"SELECT count(*), sum(i4) FROM %t WHERE d >= '2021-01-01' AND ts < '2020-01-15'"
	"SELECT count(*) FROM %t WHERE n > 100.5 AND n <= 120"
	"SELECT count(*) FROM %t WHERE f8 > 100 OR f8 IS NULL OR f8 = 'NaN'"
	"SELECT count(*) FROM %t WHERE bp = '42' AND v <> 'x42'"
	"SELECT count(*) FROM %t WHERE u IS DISTINCT FROM 'c4ca4238-a0b9-2382-0dcc-509a6f75849b'"
	"SELECT count(*) FROM %t WHERE i4 IN (1, 2, 3, 49999, NULL) OR id IN (5, 6)"
	"SELECT id, length(longt), substr(longt, 1, 8) FROM %t WHERE id % 50 = 0 AND id < 2000 ORDER BY id"
	"SELECT count(*) FROM %t WHERE later = 42 AND id > $ROWS"
	"SELECT extract(year FROM d), count(*) FROM %t GROUP BY 1 ORDER BY 1"
	# DISTINCT aggregates: the planner's H3 in two VecAggs, and ORCA's own,
	# which keeps one Agg on a single node and VecAgg never takes
	"SELECT count(DISTINCT i4), sum(i4), count(DISTINCT t), max(d) FROM %t"
	"SELECT i2 % 7, count(DISTINCT i8), sum(n), count(*) FROM %t GROUP BY 1"
	"SELECT b, count(DISTINCT n), avg(i2) FROM %t GROUP BY b"
	# H7 and H8: length, date_trunc, SUM(x + k) over an int2
	"SELECT sum(length(t)), max(octet_length(t)), count(DISTINCT date_trunc('hour', ts)) FROM %t"
	"SELECT sum(i2), sum(i2 + 1), sum(i2 + 2), sum(i2 - 3), sum(7 + i2), count(i2) FROM %t"
	"SELECT ch, o, arr, j, iv FROM %t WHERE id < 20 ORDER BY id"
	"SELECT id, ctid IS NOT NULL, tableoid::regclass::text LIKE 't_%' FROM %t WHERE id < 5 ORDER BY id"
	"/* error */ SELECT i4 / (i2 - i2) FROM %t WHERE id = 3"
	"SELECT i4 / (i2 - i2) FROM %t WHERE id = 3 AND i2 IS NULL"
	# a kernel fails only on the deleted rows (id % 97 = 0): no error
	"SELECT count(*) FROM %t WHERE 100 / (id % 97) >= 0"
	"SELECT sum(100 / (id % 97)), sum((32767 + (id % 97 = 0)::int)::int2) FROM %t"
	# a projection fails only on the rows its scan's qual removed: under ORCA
	# a VecResult's, over the VecScan's batches
	"SELECT id, i2 + 30000::int2 FROM %t WHERE i2 < 2000 AND id % 100 = 3 ORDER BY id"
	# count(*) over a qual with a SubPlan: under ORCA a VecResult of no
	# columns, reading its child's rows
	"SELECT count(*) FROM %t t1 WHERE t1.id < 300 AND (t1.i4 % 1000 = 42 OR t1.i2 = (SELECT t2.i2 FROM %t t2 WHERE t2.id = t1.id + 1 LIMIT 1))"
	# V3: hash joins over each storage's batches -- keys of each layout:
	# integers of two widths, text in views and offsets, scaled numerics with
	# dates, uuids, bpchar, a column added later, timestamps, "char" and oid
	# -- over deleted rows and NULL keys, in each join type, and long and
	# compressed values carried through the join
	"SELECT count(*), sum(a.id), sum(b.id) FROM %t a JOIN %t b ON a.i2 = b.i4"
	"SELECT count(*), sum(b.id) FROM %t a JOIN %t b ON a.t = b.t AND a.id < b.id WHERE a.id % 50 = 0"
	"SELECT count(*), sum(a.id), sum(b.id) FROM %t a JOIN src b ON a.n = b.n AND a.d = b.d WHERE b.id % 5 = 0"
	"SELECT count(*), count(b.id), sum(b.id) FROM %t a LEFT JOIN %t b ON a.u = b.u AND b.id % 3 = 0"
	"SELECT count(*), sum(a.id) FROM %t a WHERE EXISTS (SELECT 1 FROM %t b WHERE b.bp = a.bp AND b.v > a.v)"
	"SELECT count(*), sum(a.id) FROM %t a WHERE NOT EXISTS (SELECT 1 FROM %t b WHERE b.later = a.i4 AND b.id <> a.id)"
	"SELECT count(*), count(a.id), sum(b.id) FROM %t a RIGHT JOIN %t b ON a.ts = b.ts + interval '1 minute' AND a.id = b.id + 1 WHERE b.id < 1500"
	"SELECT a.ch, count(*), sum(b.id) FROM %t a JOIN %t b ON a.ch = b.ch AND a.o = b.o GROUP BY a.ch ORDER BY a.ch"
	"SELECT md5(string_agg(a.id || ':' || coalesce(b.longt, '-'), ',' ORDER BY a.id, b.id)) FROM %t a LEFT JOIN %t b ON a.id = b.id + 25 WHERE a.id % 100 = 1"
	# V4: sorts -- keys of each layout, both directions, NULLs first and
	# last, a bound -- over each storage's batches, and in parallel
	"SELECT id, t, n, d FROM %t ORDER BY n DESC NULLS LAST, d, id LIMIT 25"
	"SELECT id, u, bp FROM %t WHERE id % 7 = 3 ORDER BY u NULLS FIRST, bp DESC, id"
	"SELECT t, count(*), sum(i8) FROM %t GROUP BY t ORDER BY count(*) DESC, t NULLS FIRST LIMIT 5"
	"SELECT id, ts, tz, f8 FROM %t ORDER BY f8 DESC, ts, id LIMIT 1030"
	"SELECT id, longt FROM %t WHERE id % 50 = 0 ORDER BY length(longt) DESC, id LIMIT 4"
	"SELECT a.id, b.t FROM %t a JOIN %t b ON a.i4 = b.id ORDER BY b.t, a.id LIMIT 40"
)
# V4: in parallel, with workers, where every answer is the serial one's
# whatever the order the participants read the rows in: no float sum, whose
# order of addition the participants change, no LIMIT without an ORDER BY
PARALLEL_QUERIES=(
	"SELECT count(*), count(i2), count(t), count(u), count(later) FROM %t"
	"SELECT count(*), sum(i2), sum(i4), sum(i8), sum(n), min(t), max(t), min(d), max(ts), count(DISTINCT bp) FROM %t"
	"SELECT md5(string_agg(concat_ws(',', id, i2, i4, i8, f4, f8, n, nfree, t, v, bp, d, ts, tz, b, u, length(longt), ch, o, arr, j, iv, later), '|' ORDER BY id)) FROM %t"
	"SELECT t, count(*), sum(i4), max(id) FROM %t GROUP BY t ORDER BY t NULLS FIRST"
	"SELECT id, i4, t FROM %t WHERE i4 > 49000 ORDER BY id"
	"SELECT count(*), sum(a.id), sum(b.id) FROM %t a JOIN %t b ON a.i2 = b.i4"
	"SELECT count(*), count(b.id), sum(b.id) FROM %t a LEFT JOIN %t b ON a.u = b.u AND b.id % 3 = 0"
	"SELECT count(*), sum(a.id) FROM %t a WHERE EXISTS (SELECT 1 FROM %t b WHERE b.bp = a.bp AND b.v > a.v)"
	"SELECT count(*), sum(a.id) FROM %t a WHERE NOT EXISTS (SELECT 1 FROM %t b WHERE b.later = a.i4 AND b.id <> a.id)"
	"SELECT id, t, n, d FROM %t ORDER BY n DESC NULLS LAST, d, id LIMIT 25"
	"SELECT count(*), sum(i4) FROM %t WHERE 100 / (id % 97) >= 0"
)
# and each storage module's own, a query a line, from sources/*.queries
for f in "$here"/sources/*.queries; do
	[ -f "$f" ] || continue
	while IFS= read -r line; do
		case "$line" in ''|'#'*) continue ;; esac
		QUERIES+=("$line")
	done < "$f"
done
# Results compare in their order where the statement orders them at its
# top, and sorted where it does not, as the differential runner compares
# them (differential.py): under PostgreSQL's planner a VecAgg's groups come
# in another order than an Agg's.
#
# each orca-off is the reference of the ORCA sessions after it.  The others
# are PostgreSQL's planner's: gp_orca, loaded, plans with ORCA by default
# (gp.optimizer), so they set it off -- until V4 they did not, and ran under
# ORCA too.
SESSIONS=("off" "force-postgres" "force-arrow" "force-random" "force-slots"
		  "orca-off" "orca-postgres" "orca-arrow")
PARALLEL="-c max_parallel_workers_per_gather=3 -c parallel_setup_cost=0 -c parallel_tuple_cost=0 -c min_parallel_table_scan_size=0"
# each session's settings, given as options so that an error's text is the
# statement's alone
session_opts() {
	case "$1" in
		off) echo "-c gp.optimizer=off -c vexec.mode=off" ;;
		force-postgres) echo "-c gp.optimizer=off -c vexec.mode=force -c vexec.batch_format=postgres" ;;
		force-arrow) echo "-c gp.optimizer=off -c vexec.mode=force -c vexec.batch_format=arrow" ;;
		force-random) echo "-c gp.optimizer=off -c vexec.mode=force -c vexec.debug_layout_seed=$SEED" ;;
		force-slots) echo "-c gp.optimizer=off -c vexec.mode=force -c vexec.heap_page_reader=off" ;;
		force-parallel) echo "-c gp.optimizer=off -c vexec.mode=force $PARALLEL" ;;
		off-parallel) echo "-c gp.optimizer=off -c vexec.mode=off $PARALLEL" ;;
		orca-off) echo "-c gp.optimizer=on -c vexec.mode=off" ;;
		orca-postgres) echo "-c gp.optimizer=on -c vexec.mode=force -c vexec.batch_format=postgres" ;;
		orca-arrow) echo "-c gp.optimizer=on -c vexec.mode=force -c vexec.batch_format=arrow" ;;
	esac
}

# A statement's result as it compares: sorted unless the statement orders
# its rows at its top -- an ORDER BY outside every parenthesis.
ordered() {
	local x="$1" y
	while :; do
		y=$(printf '%s' "$x" | sed 's/([^()]*)//g')
		[ "$y" = "$x" ] && break
		x="$y"
	done
	printf '%s' "$x" | grep -qi 'order by'
}
comparable() {					# comparable <sql> <result>
	if ordered "$1"; then printf '%s\n' "$2"; else printf '%s\n' "$2" | LC_ALL=C sort; fi
}

nq=0
for s in $STORAGES; do
	t="t_$s"
	want="the slot path"
	[ "$s" = heap ] && want="heap's pages"
	for m in $SOURCES; do
		[ "${m%%=*}" = "$s" ] && want="${m#*=}"
	done
	got=$(q "$D" postgres "SET vexec.mode = force; EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM $t" | sed -n 's/^ *Source: //p')
	if [ "$got" = "$want" ]; then
		echo "  ok $s: its VecScan reads through $want"
	else
		echo "  FAILED $s: its VecScan reads through [$got], not [$want]"
		fail=1
	fi
	# under ORCA: a VecResult over the VecScan's batches, from the source
	plan=$(q "$D" postgres "SET gp.optimizer = on; SET vexec.mode = force; EXPLAIN (VERBOSE, COSTS OFF) SELECT id, i2 + 1::int2 FROM $t WHERE i4 > 0")
	if echo "$plan" | grep -q "Optimizer: GPORCA" && echo "$plan" | grep -q "Vec Result" &&
		echo "$plan" | grep -q "Input: batches" && echo "$plan" | grep -q "Source: $want"; then
		echo "  ok $s: under ORCA a VecResult reads its VecScan's batches"
	else
		echo "  FAILED $s: ORCA's plan has no VecResult over a VecScan reading through $want:"
		echo "$plan" | sed 's/^/      /'
		fail=1
	fi
	# V3: under each planner a hash join of the table with itself, a
	# VecHashJoin reading both its VecScans' batches, from the source
	for o in off on; do
		route="under $( [ $o = on ] && echo ORCA || echo "the planner")"
		plan=$(q "$D" postgres "SET gp.optimizer = $o; SET vexec.mode = force; EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*), sum(a.id), sum(b.id) FROM $t a JOIN $t b ON a.i2 = b.i4")
		if echo "$plan" | grep -q "Vec Hash Join" && echo "$plan" | grep -q "Outer Input: batches" &&
			echo "$plan" | grep -q "Inner Input: batches" &&
			[ "$(echo "$plan" | grep -c "Source: $want")" -eq 2 ]; then
			echo "  ok $s: $route a VecHashJoin reads both its VecScans' batches"
		else
			echo "  FAILED $s: $route no VecHashJoin over two VecScans reading through $want:"
			echo "$plan" | sed 's/^/      /'
			fail=1
		fi
	done
	# V4: with workers, a parallel VecScan under a Gather, sharing the table
	# through its source, under a partial VecAgg
	plan=$(PGOPTIONS="$PARALLEL" q "$D" postgres "SET gp.optimizer = off; SET vexec.mode = force; EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*), sum(i4) FROM $t")
	if echo "$plan" | grep -q "Gather" && echo "$plan" | grep -q "Parallel Vec Seq Scan" &&
		echo "$plan" | grep -q "Vec Partial Aggregate" && echo "$plan" | grep -q "Source: $want"; then
		echo "  ok $s: with workers a parallel VecScan under a Gather reads through $want"
	else
		echo "  FAILED $s: with workers no parallel VecScan reading through $want under a Gather:"
		echo "$plan" | sed 's/^/      /'
		fail=1
	fi
	# V4: under each planner a VecSort over the VecScan's batches, bounded by
	# the LIMIT -- under ORCA by the Limit of its plan
	for o in off on; do
		route="under $( [ $o = on ] && echo ORCA || echo "the planner")"
		plan=$(q "$D" postgres "SET gp.optimizer = $o; SET vexec.mode = force; EXPLAIN (VERBOSE, COSTS OFF) SELECT id, t FROM $t ORDER BY t, id LIMIT 10")
		if echo "$plan" | grep -q "Vec Sort" && echo "$plan" | grep -q "Bound: 10" &&
			echo "$plan" | grep -q "Input: batches" && echo "$plan" | grep -q "Source: $want"; then
			echo "  ok $s: $route a bounded VecSort reads its VecScan's batches"
		else
			echo "  FAILED $s: $route no bounded VecSort over a VecScan reading through $want:"
			echo "$plan" | sed 's/^/      /'
			fail=1
		fi
	done
	for qt in "${QUERIES[@]}"; do
		sql="${qt//%t/$t}"
		ref=""
		for sess in "${SESSIONS[@]}"; do
			# a query that prints its scans' sources reads heap's otherwise
			# through the slot path, as it means to
			[ "$sess" = force-slots ] && [[ "$sql" == *Source:* ]] && continue
			res=$(comparable "$sql" "$(PGOPTIONS="$(session_opts "$sess")" q "$D" postgres "$sql")")
			if [ "$sess" = off ] || [ "$sess" = orca-off ]; then
				ref="$res"
				case "$qt" in "/* error */"*) meant=1 ;; *) meant=0 ;; esac
				case "$res" in *ERROR:*) erred=1 ;; *) erred=0 ;; esac
				if [ $erred != $meant ]; then
					echo "  FAILED $s, $sess $( [ $erred = 1 ] && echo "raised an error" || echo "raised no error"): $sql"
					echo "$res" | grep "ERROR:" | head -3 | sed 's/^/      /'
					fail=1
				fi
			elif [ "$res" != "$ref" ]; then
				echo "  FAILED $s, $sess differs from off: $sql"
				diff <(echo "$ref") <(echo "$res") | head -8 | sed 's/^/      /'
				fail=1
			fi
			nq=$((nq + 1))
		done
	done
done
# V4: each storage's tables scanned in parallel: off with workers, then
# force with workers, against off serial
for s in $STORAGES; do
	t="t_$s"
	for qt in "${PARALLEL_QUERIES[@]}"; do
		sql="${qt//%t/$t}"
		ref=$(comparable "$sql" "$(PGOPTIONS="$(session_opts off)" q "$D" postgres "$sql")")
		for sess in off-parallel force-parallel; do
			res=$(comparable "$sql" "$(PGOPTIONS="$(session_opts "$sess")" q "$D" postgres "$sql")")
			if [ "$res" != "$ref" ]; then
				echo "  FAILED $s, $sess differs from off: $sql"
				diff <(echo "$ref") <(echo "$res") | head -8 | sed 's/^/      /'
				fail=1
			fi
			nq=$((nq + 1))
		done
	done
done

# H2: PAX's statistics.  h2_<storage>, in files of groups of 1,000 rows,
# then 102 rows deleted, from two of them; h2a_<storage>, a column added
# between its files, which no DELETE touches: PAX's own DELETE fails on a
# file of more than one group written before ALTER TABLE ... ADD COLUMN
# (pg_vector_executor.md V2, "Found").
H2_QUERIES=(
	"SELECT count(*), count(i2), min(i2), max(i2), sum(i2), avg(i2), count(i4), min(i4), max(i4), sum(i4), avg(i4) FROM %t"
	"SELECT min(i8), max(i8), sum(i8), avg(i8), min(d), max(d), min(ts), max(ts), min(tz), max(tz) FROM %t"
	"SELECT count(*) FROM %t"
	"SELECT count(nothing), min(nothing), max(nothing), sum(nothing), avg(nothing) FROM %t"
	"SELECT min(i4), sum(f8), count(*) FROM %t"
	"SELECT min(i4), max(i4), count(*) FROM %t WHERE i2 > 0"
	"SELECT count(later), min(later), max(later), sum(later), count(*), min(i4), sum(i2) FROM %a"
)
H2_SESSIONS=("off" "force-postgres" "force-arrow" "nostats" "force-parallel" "orca-postgres")
# a parallel session sums floats in another order: those queries skip it
h2_opts() {
	case "$1" in
		nostats) echo "-c gp.optimizer=off -c vexec.mode=force -c vexec.aggregate_statistics=off" ;;
		*) session_opts "$1" ;;
	esac
}
# h2_units <table> <orca on|off>: the units the statistics answered in the
# plan of a query they can answer, and whether its VecScan's rows are the
# table's
h2_units() {
	local plan rows units
	plan=$(q "$D" postgres "SET gp.optimizer = $2; SET vexec.mode = force; EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*), min(i4), max(i4), sum(i2) FROM $1")
	units=$(echo "$plan" | sed -n 's/^ *Units From Statistics: //p' | head -1)
	rows=$(echo "$plan" | grep -E "Vec (Seq )?Scan" | sed -n 's/.*actual rows=\([0-9]*\).*/\1/p' | head -1)
	if ! echo "$plan" | grep -q "From Statistics: pax"; then
		echo "none: no VecAgg asks pax"
		echo "$plan" | sed 's/^/      /' >&2
	elif [ "$rows" != "$(q "$D" postgres "SELECT count(*) FROM $1")" ]; then
		echo "none: its VecScan's rows are $rows"
	else
		echo "${units:-0}"
	fi
}
h2_table() {					# h2_table <table> <format> <later: 0 or 1>
	local cols="id, i2, i4, i8, d, ts, tz, f8, NULL"
	q "$D" postgres "SET client_min_messages = warning;
		CREATE TABLE $1 (id int, i2 int2, i4 int4, i8 int8, d date, ts timestamp, tz timestamptz,
		                 f8 float8, nothing int4)
			USING pax WITH (storage_format=$2, minmax_columns='i2,i4,i8,d,ts,tz,nothing')"
	q "$D" postgres "SET pax.max_tuples_per_group = 1000; INSERT INTO $1 SELECT $cols FROM src WHERE id <= $ROWS / 2"
	q "$D" postgres "SET pax.max_tuples_per_group = 1000; INSERT INTO $1 SELECT $cols FROM src WHERE id > $ROWS / 2"
	if [ "$3" = 1 ]; then
		q "$D" postgres "ALTER TABLE $1 ADD COLUMN later int DEFAULT 42"
		q "$D" postgres "INSERT INTO $1 (id, i2, i4, later) SELECT $ROWS + g, g, g, g FROM generate_series(1, 500) g"
	else
		q "$D" postgres "INSERT INTO $1 (id, i2, i4) SELECT $ROWS + g, g, g FROM generate_series(1, 500) g"
	fi
	q "$D" postgres "ANALYZE $1"
}
for s in $STORAGES; do
	case "$s" in pax) fmt=porc ;; pax_porc_vec) fmt=porc_vec ;; *) continue ;; esac
	t="h2_$s"
	a="h2a_$s"
	out="$(h2_table "$t" $fmt 0)$(h2_table "$a" $fmt 1)"
	case "$out" in *ERROR*) echo "the H2 tables in $s: $out"; fail=1; continue ;; esac
	for phase in written deleted; do
		if [ $phase = deleted ]; then
			before=$(q "$D" postgres "SELECT count(*) FROM $t")
			out=$(q "$D" postgres "DELETE FROM $t WHERE id BETWEEN 2000 AND 2100 OR id = $ROWS + 7")
			after=$(q "$D" postgres "SELECT count(*) FROM $t")
			if [ -n "$out" ] || [ $(( before - after )) -ne 102 ]; then
				echo "  FAILED $s: the DELETE of 102 rows from $t deleted $(( before - after )): $out"
				fail=1
			fi
		fi
		for orca in off on; do
			for tab in $t $a; do
				[ $phase = deleted ] && [ $tab = $a ] && continue
				units=$(h2_units "$tab" $orca)
				case "$units" in
					''|none*|0) echo "  FAILED $tab, $phase: under $([ $orca = on ] && echo ORCA || echo the planner) no unit was answered from PAX's statistics: $units"; fail=1 ;;
					*) echo "  ok $tab, $phase: under $([ $orca = on ] && echo ORCA || echo the planner) $units units answered from PAX's statistics" ;;
				esac
			done
		done
		for qt in "${H2_QUERIES[@]}"; do
			sql="${qt//%t/$t}"
			sql="${sql//%a/$a}"
			ref=""
			for sess in "${H2_SESSIONS[@]}"; do
				[ "$sess" = force-parallel ] && [[ "$sql" == *f8* ]] && continue
				res=$(PGOPTIONS="$(h2_opts "$sess")" q "$D" postgres "$sql")
				if [ "$sess" = off ]; then
					ref="$res"
					case "$res" in *ERROR:*) echo "  FAILED $s, $phase, off raised an error: $sql"; fail=1 ;; esac
				elif [ "$res" != "$ref" ]; then
					echo "  FAILED $s, $phase, $sess differs from off: $sql"
					diff <(echo "$ref") <(echo "$res") | head -8 | sed 's/^/      /'
					fail=1
				fi
				nq=$((nq + 1))
			done
		done
	done
done

# H6: the running bound.  h6_<storage>, its key k ascending with the rows,
# a NULL key every 997th row, in groups of 1,000 rows; PAX keeps k's minimum
# and maximum.
H6_QUERIES=(
	"SELECT id, k, t FROM %t ORDER BY k LIMIT 10"
	"SELECT id, k FROM %t ORDER BY k DESC NULLS LAST, id LIMIT 7"
	"SELECT id, k FROM %t ORDER BY k NULLS FIRST, id LIMIT 5"
	"SELECT id, k, t FROM %t WHERE t LIKE '%5%' ORDER BY k, id LIMIT 12"
	"SELECT k, count(*) OVER () FROM (SELECT k FROM %t ORDER BY k / 1000 LIMIT 20) s ORDER BY k"
	"/* error */ SELECT id, k FROM %t WHERE 100 / ($ROWS - id) <> 0 ORDER BY k LIMIT 3"
)
H6_SESSIONS=("off" "force-postgres" "force-arrow" "force-slots" "orca-off" "orca-postgres")
# h6_rows <table> <orca on|off>: "<rows its source handed the scan> <rows
# it dropped by the bound>", or nothing where the plan has no vector scan
h6_rows() {
	local plan kept removed
	plan=$(q "$D" postgres "SET gp.optimizer = $2; SET vexec.mode = force; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id, k, t FROM $1 ORDER BY k LIMIT 10")
	kept=$(echo "$plan" | grep -E "Vec Seq Scan" | sed -n 's/.*actual rows=\([0-9]*\).*/\1/p' | head -1)
	removed=$(echo "$plan" | sed -n 's/^ *Rows Removed by Bound: //p' | head -1)
	[ -n "$kept" ] && echo "$(( kept + ${removed:-0} )) ${removed:-0}"
}
for s in $STORAGES; do
	t="h6_$s"
	clause="$(storage_clause "$s")"
	case "$s" in pax|pax_porc_vec) clause="${clause%)}, minmax_columns='k')" ;; esac
	out=$(q "$D" postgres "SET client_min_messages = warning;
		CREATE TABLE $t (id int, k int, t text) $clause;
		SET pax.max_tuples_per_group = 1000;
		INSERT INTO $t SELECT g, CASE WHEN g % 997 = 0 THEN NULL ELSE g END, 'v' || (g % 1009)
			FROM generate_series(1, $ROWS) g;
		ANALYZE $t;")
	case "$out" in *ERROR*) echo "the H6 table in $s: $out"; fail=1; continue ;; esac
	for orca in off on; do
		read -r handed removed <<< "$(h6_rows "$t" $orca)"
		planner="$([ $orca = on ] && echo ORCA || echo the planner)"
		case "$s" in
			pax|pax_porc_vec)
				if [ -n "$handed" ] && [ "$handed" -lt 2000 ]; then
					echo "  ok $t: under $planner the bound passed PAX's groups over: $handed rows handed the scan, $removed dropped by it"
				else
					echo "  FAILED $t: under $planner the bound passed no PAX group over: ${handed:-no} rows handed the scan"
					fail=1
				fi ;;
			*)
				if [ -n "$removed" ] && [ "$removed" -gt $(( ROWS / 2 )) ]; then
					echo "  ok $t: under $planner $handed rows handed the scan, $removed dropped by the bound"
				else
					echo "  FAILED $t: under $planner the scan dropped ${removed:-no} rows by the bound"
					fail=1
				fi ;;
		esac
	done
	for qt in "${H6_QUERIES[@]}"; do
		sql="${qt//%t/$t}"
		ref=""
		for sess in "${H6_SESSIONS[@]}"; do
			[ "$sess" = orca-off ] && ref=""
			res=$(PGOPTIONS="$(session_opts "$sess")" q "$D" postgres "$sql" 2>&1)
			if [ "$sess" = off ] || [ "$sess" = orca-off ]; then
				ref="$res"
				case "$res" in *ERROR:*) [[ "$sql" == "/* error */"* ]] || { echo "  FAILED $s, $sess raised an error: $sql"; fail=1; } ;; esac
			elif [ "$res" != "$ref" ]; then
				echo "  FAILED $s, $sess differs from the reference: $sql"
				diff <(echo "$ref") <(echo "$res") | head -8 | sed 's/^/      /'
				fail=1
			fi
			nq=$((nq + 1))
		done
	done
done

echo "sources: $nq queries over $(echo $STORAGES | wc -w) storages; $([ $fail -eq 0 ] && echo passed || echo FAILED)"
exit $fail
