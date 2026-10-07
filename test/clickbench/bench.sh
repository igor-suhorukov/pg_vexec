#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# bench.sh: one load of ClickBench's table, and its 43 queries run on it --
# the part of the suite that runs inside a container (run.sh starts it, as
# docker/compose.yml's vanilla, orca and vanillaorca services).  A load is the
# table in one storage on one server or cluster; each planner of CB_PLANNERS
# is then one configuration of pg_vector_executor.md §6.9, run on that load.
#
#   CB_MODE      check: each query once and its answer kept, on the servers
#                built with assertions; time: ClickBench's protocol, CB_REPS
#                times over, on the servers built without them
#   CB_ROUTE     vanilla: vanilla PostgreSQL 19, one server, PostgreSQL's
#                planner with parallel workers; port: the port, a coordinator
#                and CB_SEGMENTS segments; vanilla-orca: V6's, vanilla
#                PostgreSQL 19 with the port's gp_orca built alone for one
#                node preloaded, the vanilla route's settings, in the image's
#                own server (vexec-entry.sh installs gp_orca over it)
#   CB_STORAGE   vanilla and vanilla-orca: heap, heap_pk (with the primary
#                key's btree); port: aoco, porc, porc_vec
#   CB_PLANNERS  vanilla: planner; port and vanilla-orca: "orca planner" --
#                ORCA, and the planner's route on the same tables
#   CB_SUBSET    1m, 10m, full: the rows loaded (clickbench.py subsets)
#   CB_SEGMENTS  4
#   CB_REPS      time mode: how many times the protocol runs over, 3
#   CB_TRIES     each query's runs in a protocol, 3: the first after the
#                servers are restarted and their files dropped from the page
#                cache (CB_COLD), the faster of the other two the hot time
#   CB_TIMEOUT   each statement's limit, in seconds: 1800; a query that runs
#                out of it is not tried again in that protocol
#   CB_COLD      evict: the servers' files are dropped from the page cache
#                before a cold run, with posix_fadvise, as their only pages
#                that matter; it needs no privilege and leaves the host's
#                other pages be.  none: the servers are restarted only
#   CB_STATS     a stats.json an earlier run of this load kept: its
#                statistics replace ANALYZE's after the load, so that the
#                planners see what they saw then, and plan as they did
#   CB_QUERIES   the queries to run, "0 23 28" (all 43)
#   CB_WORKERS   port: parallel workers per segment, 0 (as the tpc suite's
#                TPC_WORKERS, gp.enable_parallel on above 0)
#   CB_PAX_WITH  PAX's table options in place of the suite's
#   CB_VEXEC     vexec's sessions, each one more configuration of each
#                planner, "<planner>+<session>": force-postgres, force-arrow
#                (vexec.mode = force in that format), auto-postgres,
#                auto-arrow, and each with -serial, without parallel workers
#                -- V1's vector scans have no partial path (V4 gives them
#                one), so in force mode a parallel row scan still wins where
#                workers are allowed.  vexec is then preloaded on every node,
#                and the planners without a session run it in off mode.  The
#                container starts through vexec-entry.sh, which installs it
#   CB_OUT       the run's directory under /runs, which run.sh names
#   KEEP         keep the servers' data directories
#
# Written into CB_OUT: config.json, env.json, load.tsv (each step's ms),
# size (the data directories' bytes), stats.json (the statistics, which
# hold values of the data: it stays in the cache), times.tsv (rep, planner,
# query, try, ms, status), out/<planner>/r<rep>/qNN.t<try>.out and .err
# (each answer, as psql -A -t -F US -0 prints it), plans/<planner>/qNN.json
# (EXPLAIN), analyze/<planner>/qNN.json and analyze/temp.tsv (time mode:
# EXPLAIN ANALYZE once a query, and its temporary files' bytes from the
# servers' logs), logs/.  clickbench.py report reads them.

set -u

die() { echo "bench.sh: $*" >&2; exit 1; }

MODE="${CB_MODE:-}"
ROUTE="${CB_ROUTE:-}"
STORAGE="${CB_STORAGE:-}"
SUBSET="${CB_SUBSET:-}"
SEGMENTS="${CB_SEGMENTS:-4}"
REPS="${CB_REPS:-3}"
TRIES="${CB_TRIES:-3}"
TIMEOUT="${CB_TIMEOUT:-1800}"
COLD="${CB_COLD:-evict}"
WORKERS="${CB_WORKERS:-0}"
OUT="${CB_OUT:-}"
case "$MODE" in check|time) ;; *) die "CB_MODE is check or time" ;; esac
case "$ROUTE:$STORAGE" in
	vanilla:heap|vanilla:heap_pk) PLANNERS="${CB_PLANNERS:-planner}" ;;
	port:aoco|port:porc|port:porc_vec) PLANNERS="${CB_PLANNERS:-orca planner}" ;;
	vanilla-orca:heap|vanilla-orca:heap_pk) PLANNERS="${CB_PLANNERS:-orca planner}" ;;
	*) die "CB_ROUTE and CB_STORAGE: vanilla or vanilla-orca with heap or heap_pk, port with aoco, porc or porc_vec" ;;
esac
VEXEC="${CB_VEXEC:-}"
if [ -n "$VEXEC" ]; then
	all=""
	for p in $PLANNERS; do
		all="$all $p"
		for v in $VEXEC; do
			case "$v" in force-postgres|force-arrow|auto-postgres|auto-arrow) ;;
				force-postgres-serial|force-arrow-serial|auto-postgres-serial|auto-arrow-serial) ;;
				*) die "no vexec session $v" ;;
			esac
			all="$all $p+$v"
		done
	done
	PLANNERS="${all# }"
fi
case "$COLD" in evict|none) ;; *) die "CB_COLD is evict or none" ;; esac
[ -n "$OUT" ] || die "CB_OUT is not set"
[ "$MODE" = check ] && { REPS=1; TRIES=1; }

SRC=/clickbench-src
HERE=/clickbench
CACHE=/cache
PY=/opt/duckdb/bin/python
[ -x "$PY" ] || PY=python3
case "$SUBSET" in
	1m|10m|sample) TSV="$CACHE/hits_$SUBSET.tsv" ;;
	full) TSV="$CACHE/hits.tsv" ;;
	*) die "CB_SUBSET is 1m, 10m, full or sample" ;;
esac
[ -r "$TSV" ] || die "no $TSV: run.sh prepare makes it"
ROWS=$(case "$SUBSET" in 1m) echo 999975 ;; 10m) echo 9999750 ;; full) echo 99997497 ;; *) wc -l < "$TSV" ;; esac)

# The servers: vanilla's two builds are in one image, the port's in two, and
# vanilla-orca's in two (vexec's vanilla images, run.sh picking by mode).
if [ "$ROUTE" = vanilla ]; then
	PREFIX=/pg/vanilla
	[ "$MODE" = time ] && PREFIX=/pg/vanilla-noassert
	NODES=0
elif [ "$ROUTE" = vanilla-orca ]; then
	PREFIX=/usr/local/pgsql
	NODES=0
else
	PREFIX=/usr/local/pgsql
	NODES="$(seq 0 "$SEGMENTS")"
fi
BINDIR="$PREFIX/bin"
PSQL="$BINDIR/psql"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir):$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
if [ "$ROUTE" = vanilla-orca ]; then
	[ -f "$("$BINDIR/pg_config" --pkglibdir)/gp_orca.so" ] \
		|| die "the server has no gp_orca: vexec-entry.sh installs it from the stage (CB_ORCA_STAGE)"
	grep -q "^#define USE_ASSERT_CHECKING" "$("$BINDIR/pg_config" --includedir-server)/pg_config.h" \
		&& asserts=yes || asserts=no
	[ "$asserts" = "$([ "$MODE" = check ] && echo yes || echo no)" ] \
		|| die "$MODE mode on a server whose assertions are $asserts: run.sh picks the image"
fi

mapfile -t Q < <(grep -v '^[[:space:]]*$' "$SRC/postgresql/queries.sql")
[ "${#Q[@]}" -eq 43 ] || die "${#Q[@]} queries in $SRC/postgresql/queries.sql"
QUERIES="${CB_QUERIES:-$(seq -s ' ' 0 42)}"

mkdir -p "$OUT" || die "cannot make $OUT"
WORK="$(mktemp -d /work/cb-XXXXXX)" || die "cannot make a directory in /work"
SOCK="$(mktemp -d /tmp/cbs-XXXXXX)"
BASEPORT=$((7600 + RANDOM % 300))
SECRET="clickbench-$RANDOM$RANDOM"
datadir() { echo "$WORK/node$1"; }
sockdir() { echo "$SOCK/n$1"; }
port()    { echo $((BASEPORT + $1)); }

stop_all() {
	local m="${1:-fast}" n
	for n in $(echo $NODES | tr ' ' '\n' | sort -n); do	# the coordinator first
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m "$m" -w -t 300 stop > /dev/null 2>&1
	done
}
start_all() {
	local n
	for n in $(echo $NODES | tr ' ' '\n' | sort -rn); do	# the segments first
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -l "$WORK/node$n.log" -w -t 300 start > /dev/null 2>&1 \
			|| { echo "node $n did not start:"; tail -20 "$WORK/node$n.log"; return 1; }
	done
	for _ in $(seq 300); do
		q clickbench "SELECT 1" > /dev/null 2>&1 && return 0
		q postgres "SELECT 1" > /dev/null 2>&1 && return 0
		sleep 0.2
	done
	echo "the servers do not answer"; return 1
}
cleanup() {
	mkdir -p "$OUT/logs"
	for n in $NODES; do
		cp "$WORK/node$n.log" "$OUT/logs/node$n.log" 2> /dev/null
	done
	stop_all immediate
	[ -n "${KEEP:-}" ] && echo "kept: $WORK" || rm -rf "$WORK"
	rm -rf "$SOCK"
}
trap cleanup EXIT

q() {						# q <db> <sql>: on the coordinator
	"$PSQL" -X -q -t -A -v ON_ERROR_STOP=1 -h "$(sockdir 0)" -p "$(port 0)" -d "$1" -c "$2" 2>&1
}
qf() {						# qf <db> <file>
	"$PSQL" -X -q -t -A -v ON_ERROR_STOP=1 -h "$(sockdir 0)" -p "$(port 0)" -d "$1" -f "$2" 2>&1
}
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }

# --- the servers -------------------------------------------------------------

# Memory is the container's cap, as ClickBench's settings take the machine's.
MEM_KB=$(awk '{ print int($1 / 1024) }' /sys/fs/cgroup/memory.max 2> /dev/null)
[ -n "$MEM_KB" ] && [ "$MEM_KB" -gt 0 ] 2> /dev/null || MEM_KB=$(awk '/MemTotal/ { print $2 }' /proc/meminfo)
THREADS=$(nproc)

if [ "$ROUTE" = port ]; then
	CONF="$WORK/gp_cluster.conf"
	for n in $NODES; do
		echo "$((n + 1)) $((n - 1)) p $(sockdir "$n") $(port "$n") $(datadir "$n")"
	done > "$CONF"
fi
for n in $NODES; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N --locale=C --encoding=UTF8 > "$WORK/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n"; tail -20 "$WORK/initdb$n.log"; exit 1; }
	{
		echo "unix_socket_directories = '$(sockdir "$n")'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		# the temporary files of EXPLAIN ANALYZE's queries, from the log
		echo "log_temp_files = 0"
		[ -n "$VEXEC" ] && [ "$ROUTE" = vanilla ] && echo "shared_preload_libraries = 'vexec'"
		[ "$ROUTE" = vanilla-orca ] && echo "shared_preload_libraries = 'gp_orca${VEXEC:+,vexec}'"
		if [ "$ROUTE" != port ]; then
			# ClickBench's PostgreSQL settings (postgresql/install), the
			# container's memory for the machine's
			echo "shared_buffers = $((MEM_KB / 4))kB"
			echo "effective_cache_size = $((MEM_KB - MEM_KB / 4))kB"
			echo "max_worker_processes = $((THREADS + 15))"
			echo "max_parallel_workers = $THREADS"
			echo "max_parallel_maintenance_workers = $((THREADS / 2))"
			echo "max_parallel_workers_per_gather = $((THREADS / 2))"
			echo "max_wal_size = 32GB"
			echo "work_mem = 64MB"
		else
			# the port's, as its tpc suite runs a cluster
			echo "shared_preload_libraries = 'gp_core,gp_orca,gp_sql,gp_ao,pax${VEXEC:+,vexec}'"
			echo "gp.cluster_config = '$CONF'"
			echo "gp.dbid = $((n + 1))"
			echo "gp.cluster_secret = '$SECRET'"
			echo "max_prepared_transactions = 100"
			echo "max_connections = 300"
			[ "$MODE" = time ] && echo "shared_buffers = 1GB" || echo "shared_buffers = 256MB"
			echo "work_mem = 64MB"
			echo "maintenance_work_mem = 256MB"
			echo "max_parallel_workers_per_gather = 0"
			if [ "$WORKERS" -gt 0 ]; then
				echo "max_parallel_workers = $WORKERS"
				echo "max_worker_processes = $((WORKERS + 8))"
			fi
			[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
		fi
	} >> "$(datadir "$n")/postgresql.auto.conf"
done
start_all || exit 1
if [ "$ROUTE" = port ]; then
	for db in template1 postgres; do
		out=$(q "$db" "SET client_min_messages = warning; CREATE EXTENSION gp_core")
		[ -n "$out" ] && { echo "gp_core in $db: $out"; exit 1; }
	done
fi
out=$(q postgres "CREATE DATABASE clickbench")
[ -n "$out" ] && { echo "CREATE DATABASE: $out"; exit 1; }
if [ "$ROUTE" = port ]; then
	out=$(q clickbench "SET client_min_messages = warning; CREATE EXTENSION gp_sql; CREATE EXTENSION gp_ao; CREATE EXTENSION pax")
	[ -n "$out" ] && { echo "the port's extensions: $out"; exit 1; }
fi

# --- the load ----------------------------------------------------------------

# ClickBench's table for PostgreSQL, and each storage's clauses.  PAX's table
# is clustered on the primary key's leading columns, with min and max for the
# columns the queries' quals compare and bloom filters for the equality ones
# (pg_vector_executor.md §3.14, H9); AOCO's has the primary key's btree before
# the load, as ClickBench's Cloudberry entry does (cloudberry/create.sql).
# PAX's min and max leave out the bigint columns: the port's PAX fails a load
# that keeps them for a bigint column, its per-group sum ending in "function
# 3388 returned NULL", numeric_poly_sum; their bloom filters stay.
PK="CounterID, EventDate, UserID, EventTime, WatchID"
MINMAX="advengineid,counterid,eventdate,dontcounthits,isrefresh,islink,isdownload,traficsourceid"
case "$STORAGE" in
	heap|heap_pk) CLAUSE="" ;;
	aoco) CLAUSE="USING ao_column WITH (compresstype=zstd) DISTRIBUTED BY (UserID)" ;;
	porc|porc_vec)
		CLAUSE="USING pax WITH (${CB_PAX_WITH:-storage_format=$STORAGE, compresstype=zstd, cluster_type='lexical', cluster_columns='counterid,eventdate', minmax_columns='$MINMAX', bloomfilter_columns='userid,urlhash,refererhash'}) DISTRIBUTED BY (UserID)" ;;
esac
sed -e '/^[[:space:]]*$/d' -e 's/;[[:space:]]*$//' "$SRC/postgresql/create.sql" > "$WORK/create.sql"
echo " $CLAUSE;" >> "$WORK/create.sql"
[ "$STORAGE" = aoco ] && echo "CREATE INDEX hits_pk ON hits USING btree ($PK);" >> "$WORK/create.sql"

: > "$OUT/load.tsv"
step() {					# step <name> <psql args...>: timed
	local s e out
	s=$(now_ms)
	out=$("$PSQL" -X -q -t -A -v ON_ERROR_STOP=1 -h "$(sockdir 0)" -p "$(port 0)" -d clickbench "${@:2}" 2>&1) \
		|| { echo "loading, $1: $out"; exit 1; }
	e=$(now_ms)
	printf '%s\t%d\n' "$1" $((e - s)) >> "$OUT/load.tsv"
}
echo "ClickBench, $MODE mode: $ROUTE, $STORAGE, $SUBSET rows ($ROWS)$([ "$ROUTE" = port ] && echo ", $SEGMENTS segments"), planners: $PLANNERS"
echo "  bindir $BINDIR, data in $WORK"
t0=$(now_ms)
step create -f "$WORK/create.sql"
if [ "$ROUTE" != port ]; then
	# as ClickBench's own load: TRUNCATE and COPY FREEZE in one transaction
	step copy -c "BEGIN" -c "TRUNCATE TABLE hits" -c "\\copy hits FROM '$TSV' WITH (FREEZE)" -c "COMMIT"
	[ "$STORAGE" = heap_pk ] && step index -c "CREATE INDEX hits_pk ON hits USING btree ($PK)"
	step analyze -c "VACUUM ANALYZE hits"
else
	step copy -c "\\copy hits FROM '$TSV'"
	case "$STORAGE" in porc|porc_vec) step cluster -c "CLUSTER hits" ;; esac
	step analyze -c "ANALYZE hits"
fi
s=$(now_ms); sync; printf 'sync\t%d\n' $(( $(now_ms) - s )) >> "$OUT/load.tsv"
printf 'total\t%d\n' $(( $(now_ms) - t0 )) >> "$OUT/load.tsv"
got=$(q clickbench "SELECT count(*) FROM hits")
[ "$got" = "$ROWS" ] || { echo "the table has $got rows, want $ROWS"; exit 1; }
du -sbc $(for n in $NODES; do datadir "$n"; done) | awk '/total$/ { print $1 }' > "$OUT/size"
echo "  loaded in $(( $(now_ms) - t0 )) ms: $(awk '$1 != "total" { printf "%s %d ms, ", $1, $2 }' "$OUT/load.tsv" | sed 's/, $//'); $(cat "$OUT/size") bytes"

# The statistics: ANALYZE's kept, or an earlier run's restored.
STATS_SQL="SELECT json_build_object(
  'relations', (SELECT json_agg(json_build_object('relname', relname, 'relpages', relpages,
       'reltuples', reltuples, 'relallvisible', relallvisible, 'relallfrozen', relallfrozen) ORDER BY relname)
     FROM pg_class WHERE relname IN ('hits', 'hits_pk') AND relnamespace = 'public'::regnamespace),
  'attributes', (SELECT json_agg(json_build_object('attname', attname, 'inherited', inherited,
       'null_frac', null_frac, 'avg_width', avg_width, 'n_distinct', n_distinct,
       'most_common_vals', most_common_vals::text, 'most_common_freqs', most_common_freqs,
       'histogram_bounds', histogram_bounds::text, 'correlation', correlation,
       'most_common_elems', most_common_elems::text, 'most_common_elem_freqs', most_common_elem_freqs,
       'elem_count_histogram', elem_count_histogram) ORDER BY attname, inherited)
     FROM pg_stats WHERE schemaname = 'public' AND tablename = 'hits'))"
if [ -n "${CB_STATS:-}" ]; then
	"$PY" "$HERE/clickbench.py" stats-sql "$CB_STATS" > "$WORK/stats.sql" || exit 1
	out=$(qf clickbench "$WORK/stats.sql" | grep -v '^t$')
	[ -n "$out" ] && { echo "restoring $CB_STATS: $out"; exit 1; }
	echo "  statistics restored from $CB_STATS"
fi
q clickbench "$STATS_SQL" > "$OUT/stats.json"

# What the servers are: version, settings, the database's collation.
q clickbench "SELECT json_build_object('version', version(),
  'encoding', pg_encoding_to_char(encoding), 'datcollate', datcollate, 'datctype', datctype,
  'datlocprovider', datlocprovider,
  'settings', (SELECT json_object_agg(name, setting || coalesce(' ' || unit, '') ORDER BY name) FROM pg_settings
               WHERE source NOT IN ('default', 'override')
                  OR name IN ('jit', 'max_parallel_workers_per_gather', 'shared_buffers', 'work_mem',
                              'hash_mem_multiplier', 'effective_cache_size', 'random_page_cost',
                              'default_statistics_target', 'huge_pages')))
  FROM pg_database WHERE datname = current_database()" > "$WORK/server.json"
{
	echo "{\"server\": $(cat "$WORK/server.json"),"
	echo " \"bindir\": \"$BINDIR\","
	echo " \"pg_ref_commit\": \"$(cat "$PREFIX/.pg_ref_commit" 2> /dev/null)\","
	echo " \"pg_test_patches\": \"$(tr '\n' ';' < "$PREFIX/.pg_test_patches" 2> /dev/null)\","
	echo " \"duckdb\": \"$("$PY" -c 'import duckdb; print(duckdb.__version__)' 2> /dev/null)\","
	echo " \"threads\": $THREADS, \"memory_kb\": $MEM_KB}"
} > "$OUT/env.json"

cat > "$OUT/config.json" <<EOF
{"mode": "$MODE", "route": "$ROUTE", "storage": "$STORAGE", "segments": $([ "$ROUTE" = port ] && echo "$SEGMENTS" || echo 0),
 "planners": "$PLANNERS", "vexec": "$VEXEC", "subset": "$SUBSET", "rows": $ROWS, "reps": $REPS, "tries": $TRIES,
 "timeout_s": $TIMEOUT, "cold": "$COLD", "stats": "${CB_STATS:-}", "workers": $WORKERS,
 "queries": [$(echo $QUERIES | tr ' ' ',')], "date": "$(date +%F)", "host": "${CLICKBENCH_HOST:-}",
 "table": $(printf '%s' "$(cat "$WORK/create.sql")" | "$PY" -c 'import json, sys; print(json.dumps(sys.stdin.read()))')}
EOF

# --- the queries ---------------------------------------------------------------

opts() {					# opts <planner> [timeout]: the session's settings
	local o="" base="${1%%+*}" v=""
	[ "$1" != "$base" ] && v="${1#*+}"
	[ -n "${2:-}" ] && o="-c statement_timeout=${2}s"
	if [ "$ROUTE" = port ]; then
		case "$base" in
			orca) o="$o -c gp.optimizer=on -c gp.optimizer_trace_fallback=on" ;;
			planner) o="$o -c gp.optimizer=off" ;;
		esac
		[ "$WORKERS" -gt 0 ] && o="$o -c gp.enable_parallel=on -c max_parallel_workers_per_gather=$WORKERS"
	elif [ "$ROUTE" = vanilla-orca ]; then
		# gp_orca alone has no gp.enable_parallel (gp_core's): ORCA's plans
		# are serial, the planner's take the vanilla route's workers
		case "$base" in
			orca) o="$o -c gp.optimizer=on -c gp.optimizer_trace_fallback=on" ;;
			planner) o="$o -c gp.optimizer=off" ;;
		esac
	fi
	# vexec's session: its mode and format; off where the planner has none
	if [ -n "$VEXEC" ]; then
		case "$v" in
			'') o="$o -c vexec.mode=off" ;;
			*-serial) v="${v%-serial}"
				o="$o -c vexec.mode=${v%%-*} -c vexec.batch_format=${v#*-} -c max_parallel_workers_per_gather=0" ;;
			*) o="$o -c vexec.mode=${v%%-*} -c vexec.batch_format=${v#*-}" ;;
		esac
	fi
	echo "$o"
}

# A cold run's start: the servers restarted, their files out of the page cache.
cold() {
	stop_all
	sync
	[ "$COLD" = evict ] && "$PY" "$HERE/clickbench.py" evict $(for n in $NODES; do datadir "$n"; done) > /dev/null
	start_all
}

# try <rep> <planner> <query> <try>: one run, as ClickBench's psql query
# script times it -- psql's \timing, from the query sent to its last row
# received -- with the rows kept.
try() {
	local rep="$1" planner="$2" n="$3" t="$4" d base out rc ms st
	d="$OUT/out/$planner/r$rep"
	mkdir -p "$d"
	base="$d/$(printf 'q%02d.t%d' "$n" "$t")"
	out=$(printf '\\timing on\n%s\n' "${Q[$n]}" | PGOPTIONS="$(opts "$planner" "$TIMEOUT")" \
		"$PSQL" -X -q -A -t -F $'\x1f' -0 -P 'null=\N' -v ON_ERROR_STOP=1 \
		-h "$(sockdir 0)" -p "$(port 0)" -d clickbench -o "$base.out" 2> "$base.err")
	rc=$?
	ms=$(printf '%s\n' "$out" | sed -n 's/^Time: \([0-9.]*\) ms.*/\1/p' | tail -1)
	if [ $rc -eq 0 ] && [ -n "$ms" ]; then st=ok
	elif grep -q 'statement timeout' "$base.err"; then st=timeout
	else st=error; fi
	[ "$st" = ok ] || ms=null
	printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$rep" "$planner" "$n" "$t" "$ms" "$st" >> "$OUT/times.tsv"
	echo "$st"
}

# The plans, as EXPLAIN gives them in JSON.
for planner in $PLANNERS; do
	mkdir -p "$OUT/plans/$planner"
	for n in $QUERIES; do
		PGOPTIONS="$(opts "$planner")" "$PSQL" -X -q -t -A -h "$(sockdir 0)" -p "$(port 0)" -d clickbench \
			-c "EXPLAIN (FORMAT JSON) ${Q[$n]%;}" > "$OUT/plans/$planner/$(printf 'q%02d' "$n").json" 2> /dev/null
	done
done

: > "$OUT/times.tsv"
T0=$(now_ms)
for rep in $(seq 1 "$REPS"); do
	for planner in $PLANNERS; do
		s=$(now_ms)
		for n in $QUERIES; do
			[ "$MODE" = time ] && { cold || exit 1; }
			for t in $(seq 1 "$TRIES"); do
				st=$(try "$rep" "$planner" "$n" "$t")
				if [ "$st" = timeout ]; then
					for u in $(seq $((t + 1)) "$TRIES"); do
						printf '%s\t%s\t%s\t%s\tnull\tskipped\n' "$rep" "$planner" "$n" "$u" >> "$OUT/times.tsv"
					done
					break
				fi
			done
		done
		echo "  $MODE, $planner, run $rep of $REPS: $(echo $QUERIES | wc -w) queries in $(( ($(now_ms) - s) / 1000 )) s;" \
			"$(awk -F'\t' -v r="$rep" -v p="$planner" '$1 == r && $2 == p && $6 != "ok" { n++ } END { print n + 0 }' "$OUT/times.tsv") runs not ok"
	done
done

# Once more, in time mode: each query under EXPLAIN ANALYZE, and the bytes of
# the temporary files it wrote, which the servers log as they remove them.
if [ "$MODE" = time ]; then
	mkdir -p "$OUT/analyze"
	: > "$OUT/analyze/temp.tsv"
	for planner in $PLANNERS; do
		mkdir -p "$OUT/analyze/$planner"
		for n in $QUERIES; do
			declare -A off=()
			for m in $NODES; do off[$m]=$(stat -c %s "$WORK/node$m.log" 2> /dev/null || echo 0); done
			f="$OUT/analyze/$planner/$(printf 'q%02d' "$n").json"
			PGOPTIONS="$(opts "$planner" "$TIMEOUT")" "$PSQL" -X -q -t -A -v ON_ERROR_STOP=1 \
				-h "$(sockdir 0)" -p "$(port 0)" -d clickbench \
				-c "EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) ${Q[$n]%;}" > "$f" 2> "$f.err"
			rc=$?
			sleep 0.2
			bytes=0
			for m in $NODES; do
				b=$(tail -c +$((off[$m] + 1)) "$WORK/node$m.log" 2> /dev/null |
					sed -n 's/.*temporary file: path ".*", size \([0-9]*\).*/\1/p' | awk '{ s += $1 } END { print s + 0 }')
				bytes=$((bytes + b))
			done
			ms=$("$PY" -c 'import json, sys
try:
    p = json.load(open(sys.argv[1]))[0]
    print(round(p.get("Planning Time", 0) + p["Execution Time"], 3))
except Exception:
    print("")' "$f")
			printf '%s\t%s\t%s\t%s\t%s\n' "$planner" "$n" "$ms" "$bytes" "$([ $rc -eq 0 ] && echo ok || echo error)" >> "$OUT/analyze/temp.tsv"
			[ -s "$f.err" ] || rm -f "$f.err"
		done
	done
fi
echo "  done in $(( ($(now_ms) - T0) / 1000 )) s after the load"
