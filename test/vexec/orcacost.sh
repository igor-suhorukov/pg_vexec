#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# V5's leg (pg_vector_executor.md §5 V5): ORCA's vector cost model and its
# hashed window, on a single node of the port with gp_core, gp_orca, gp_ao,
# PAX and vexec preloaded.
#
#   windows   ORCA's hashed window as VecWindowHashAgg under PostgreSQL's
#             WindowAgg: every query of the corpus answers in each session
#             as ORCA's own plan answers it with vexec off, its sorted
#             window -- in auto mode, in force mode in each format and with
#             the per-structure layouts drawn at random, with a memory too
#             small for the table (its files, and past their last level its
#             sort), and with ORCA's own prices (vexec.orca_cost_model off,
#             no hashed window offered) -- and each force session's plan of
#             each query has the VecWindowHashAgg it could have
#   storages  the hashed window over each storage's batches: heap, ao_row,
#             ao_column, PAX porc and porc_vec
#   choices   ORCA's plan choices by cost alone: what CCostModelVec prices
#             otherwise than CCostModelGPDB changes ORCA's plan, the answers
#             the same
#   settings  vexec.orca_settings: ORCA's settings for the statements vexec
#             takes, set for their planning alone, and refused for a name
#             not ORCA's
#
#   VEXEC_ROWS   the window table's rows: 20000
#   VEXEC_SEED   the random session's seed: drawn and printed
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
. "$here/lib.sh"

ROWS="${VEXEC_ROWS:-20000}"
SEED="${VEXEC_SEED:-$(( (RANDOM << 15 | RANDOM) % 2147483646 + 1 ))}"
D="$(mktemp -d "${TMPDIR:-/tmp}/vexec-orcacost-XXXXXX")"
fail=0

server_init "$D" "shared_preload_libraries = 'gp_core,gp_orca,gp_sql,gp_ao,pax,vexec'" \
	"max_parallel_workers_per_gather = 0" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
trap 'server_stop "$D"; [ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/orcacost-server.log"' EXIT

out=$(q "$D" postgres "SET client_min_messages = warning; CREATE EXTENSION gp_ao CASCADE; CREATE EXTENSION gp_orca; CREATE EXTENSION gp_sql; CREATE EXTENSION pax; CREATE EXTENSION vexec")
case "$out" in *ERROR*) echo "the extensions: $out"; exit 1 ;; esac
echo "  random session's seed: $SEED (VEXEC_SEED=$SEED reruns it)"

# the data: partition keys of each kind -- integers, text, numerics, dates,
# NULLs among them, a partition of one row and one of most -- and values
# with ties, NULLs and NaN for the windows' orders and frames
out=$(q "$D" postgres "SET client_min_messages = warning;
CREATE TABLE w AS
SELECT g AS id,
       g % 10 AS k1,
       CASE WHEN g % 23 = 0 THEN NULL ELSE 'p' || (g % 7) END AS k2,
       (g % 4)::numeric(6,1) AS k3,
       DATE '2024-01-01' + (g % 3) AS k4,
       CASE WHEN g = 1 THEN 999 WHEN g % 5 = 0 THEN 1 ELSE 2 END AS big,
       g % 13 AS o1,
       CASE WHEN g % 11 = 0 THEN NULL ELSE (g * 7919) % 1000 END AS o2,
       (g % 100)::numeric(10,2) AS v,
       CASE WHEN g % 997 = 0 THEN 'NaN'::float8 ELSE (g % 37) / 4.0 END AS f,
       'name ' || (g % 211) AS t
FROM generate_series(1, $ROWS) g;
CREATE TABLE w_empty (LIKE w);
ANALYZE w; ANALYZE w_empty;")
case "$out" in *ERROR*) echo "the data: $out"; exit 1 ;; esac

WINDOW_QUERIES=(
	"SELECT id, k1, row_number() OVER (PARTITION BY k1 ORDER BY o1, id) FROM w"
	"SELECT id, k1, rank() OVER (PARTITION BY k1 ORDER BY o1), dense_rank() OVER (PARTITION BY k1 ORDER BY o1) FROM w"
	"SELECT id, k2, percent_rank() OVER (PARTITION BY k2 ORDER BY o2), cume_dist() OVER (PARTITION BY k2 ORDER BY o2) FROM w"
	"SELECT id, k1, ntile(7) OVER (PARTITION BY k1 ORDER BY o2 NULLS FIRST, id) FROM w"
	"SELECT id, k2, lag(v) OVER (PARTITION BY k2 ORDER BY id), lead(t, 2, 'none') OVER (PARTITION BY k2 ORDER BY id) FROM w"
	"SELECT id, k1, first_value(id) OVER (PARTITION BY k1 ORDER BY o1, id), last_value(id) OVER (PARTITION BY k1 ORDER BY o1, id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) FROM w"
	"SELECT id, k3, nth_value(o2, 3) OVER (PARTITION BY k3 ORDER BY id ROWS BETWEEN 2 PRECEDING AND 2 FOLLOWING) FROM w"
	"SELECT k1, sum(v) OVER (PARTITION BY k1), count(*) OVER (PARTITION BY k1), avg(f) OVER (PARTITION BY k1) FROM w"
	"SELECT id, sum(v) OVER (PARTITION BY k1 ORDER BY o1) FROM w"
	"SELECT id, sum(v) OVER (PARTITION BY k1 ORDER BY id ROWS BETWEEN 3 PRECEDING AND CURRENT ROW) FROM w"
	"SELECT id, sum(v) OVER (PARTITION BY k2 ORDER BY o1 RANGE BETWEEN 2 PRECEDING AND 1 FOLLOWING) FROM w"
	"SELECT id, count(*) OVER (PARTITION BY k1 ORDER BY o1 GROUPS BETWEEN 1 PRECEDING AND CURRENT ROW EXCLUDE TIES) FROM w"
	"SELECT id, sum(o1) OVER (PARTITION BY k4 ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW EXCLUDE CURRENT ROW) FROM w"
	"SELECT id, min(t) OVER (PARTITION BY k1, k2 ORDER BY id), max(o2) OVER (PARTITION BY k1, k2) FROM w"
	"SELECT id, k3, k4, sum(v) OVER (PARTITION BY k3, k4 ORDER BY o2 DESC NULLS LAST, id) FROM w"
	"SELECT id, string_agg(t, ',') OVER (PARTITION BY big ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) FROM w WHERE id < 2000"
	"SELECT id, max(f) OVER (PARTITION BY k1 ORDER BY f, id), min(f) OVER (PARTITION BY k1) FROM w"
	"SELECT id, k2, row_number() OVER (PARTITION BY k2 ORDER BY t DESC, id) FROM w"
	"SELECT * FROM (SELECT id, k1, rank() OVER (PARTITION BY k1 ORDER BY o2 DESC NULLS LAST) AS r FROM w) s WHERE r <= 3"
	"SELECT k1, count(*), sum(r) FROM (SELECT k1, row_number() OVER (PARTITION BY k1 ORDER BY id) AS r FROM w) s GROUP BY k1 ORDER BY k1"
	"SELECT w1.id, sum(w2.v) OVER (PARTITION BY w1.k1 ORDER BY w1.id) FROM w w1 JOIN w w2 ON w2.id = w1.id + 1 WHERE w1.id % 3 = 0"
	"SELECT k1, s, rank() OVER (PARTITION BY k1 % 2 ORDER BY s) FROM (SELECT k1, sum(v) AS s FROM w GROUP BY k1) a"
	"SELECT id, sum(v) OVER (PARTITION BY k1 ORDER BY id), sum(v) OVER (PARTITION BY k2 ORDER BY id), row_number() OVER (PARTITION BY k3 ORDER BY id) FROM w"
	"SELECT id, sum(v) OVER (PARTITION BY k1) FROM w_empty"
	# one partition, whose sort is the whole sort: ORCA keeps its sorted
	# window by cost
	"/* any */ SELECT id, k1, rank() OVER (PARTITION BY k1 ORDER BY o1) FROM w WHERE k1 = 3 AND id < 100"
	# a frame offset ORCA would read as a column: the planner plans it
	"/* any */ SELECT id, sum(v) OVER (PARTITION BY k1 ORDER BY id ROWS BETWEEN (SELECT max(k1) FROM w) PRECEDING AND CURRENT ROW) FROM w"
	# a window in a correlated SubPlan, which ORCA keeps sorted
	"/* any */ SELECT a.k1, (SELECT max(r) FROM (SELECT row_number() OVER (PARTITION BY b.k2 ORDER BY b.id) AS r FROM w b WHERE b.k1 = a.k1) s) FROM (SELECT DISTINCT k1 FROM w) a"
	"SELECT id, k1, sum(v) OVER (PARTITION BY k1 ORDER BY id) FROM w ORDER BY id LIMIT 40"
	"/* error */ SELECT id, sum(1 / (o1 - o1)) OVER (PARTITION BY k1) FROM w"
)

# each session's settings, as options, so that an error's text is the
# statement's alone
spill="-c work_mem=64kB -c hash_mem_multiplier=1"
session_opts() {
	case "$1" in
		orca-off) echo "-c gp.optimizer=on -c vexec.mode=off" ;;
		orca-auto) echo "-c gp.optimizer=on -c vexec.mode=auto -c vexec.min_rows=0" ;;
		orca-postgres) echo "-c gp.optimizer=on -c vexec.mode=force -c vexec.batch_format=postgres" ;;
		orca-arrow) echo "-c gp.optimizer=on -c vexec.mode=force -c vexec.batch_format=arrow" ;;
		orca-random) echo "-c gp.optimizer=on -c vexec.mode=force -c vexec.debug_layout_seed=$SEED" ;;
		orca-spill) echo "-c gp.optimizer=on -c vexec.mode=force $spill" ;;
		orca-own-prices) echo "-c gp.optimizer=on -c vexec.mode=force -c vexec.orca_cost_model=off" ;;
	esac
}
WINDOW_SESSIONS=(orca-off orca-auto orca-postgres orca-arrow orca-random orca-spill orca-own-prices)

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
# whether a statement means to fail; whether its plan may keep ORCA's
# sorted window, by cost, or be the planner's
means_error() { [[ "$1" == "/* error */"* ]]; }
any_plan() { [[ "$1" == "/* any */"* ]]; }

echo "== windows: ${#WINDOW_QUERIES[@]} queries in ${#WINDOW_SESSIONS[@]} sessions"
nq=0
nhashed=0
for sql in "${WINDOW_QUERIES[@]}"; do
	ref=""
	for sess in "${WINDOW_SESSIONS[@]}"; do
		res=$(comparable "$sql" "$(PGOPTIONS="$(session_opts "$sess")" q "$D" postgres "$sql")")
		if [ "$sess" = orca-off ]; then
			ref="$res"
			if ! means_error "$sql" && [[ "$res" == *ERROR* ]]; then
				echo "  FAILED the reference errs: $sql"
				echo "$res" | grep ERROR | head -3 | sed 's/^/      /'
				fail=1
			fi
			continue
		fi
		if [ "$res" != "$ref" ]; then
			echo "  FAILED $sess differs from orca-off: $sql"
			diff <(echo "$ref") <(echo "$res") | head -8 | sed 's/^/      /'
			fail=1
		fi
	done
	nq=$((nq + 1))
	# each force session's plan has a VecWindowHashAgg where the query has a
	# window with a partition
	if [[ "$sql" == *"PARTITION BY"* ]] && [[ "$sql" != *w_empty* ]] && ! means_error "$sql" &&
		! any_plan "$sql"; then
		plan=$(PGOPTIONS="$(session_opts orca-postgres)" q "$D" postgres "EXPLAIN (COSTS OFF) $sql")
		if echo "$plan" | grep -q "Vec Window Hash Agg" && echo "$plan" | grep -q "Optimizer: GPORCA"; then
			nhashed=$((nhashed + 1))
		else
			echo "  FAILED no VecWindowHashAgg in ORCA's plan of: $sql"
			echo "$plan" | sed 's/^/      /'
			fail=1
		fi
	fi
done
echo "  $nq queries answered alike in every session; $nhashed plans with a VecWindowHashAgg"

# a window over a CTE's consumer, which the translator takes to be in its
# producer's order and would refuse hashed: ORCA keeps its sorted window
# there, priced so, and plans the statement
cte="WITH c AS (SELECT id, k1, o1 FROM w WHERE id < 5000) SELECT id, rank() OVER (PARTITION BY k1 ORDER BY o1) FROM c UNION ALL SELECT id, 0 FROM c"
for sess in orca-auto orca-postgres; do
	plan=$(PGOPTIONS="$(session_opts "$sess")" q "$D" postgres "EXPLAIN (COSTS OFF) $cte")
	if echo "$plan" | grep -q "Optimizer: GPORCA" && ! echo "$plan" | grep -q "Vec Window Hash Agg"; then
		echo "  ok $sess: a window over a CTE's consumer stays ORCA's sorted window, and ORCA plans it"
	else
		echo "  FAILED $sess: a window over a CTE's consumer:"; echo "$plan" | sed 's/^/      /'; fail=1
	fi
done
check_same() {					# check_same <what> <sql>
	local ref res
	ref=$(comparable "$2" "$(PGOPTIONS="$(session_opts orca-off)" q "$D" postgres "$2")")
	res=$(comparable "$2" "$(PGOPTIONS="$(session_opts orca-postgres)" q "$D" postgres "$2")")
	if [ "$ref" = "$res" ]; then echo "  ok $1"; else echo "  FAILED $1"; diff <(echo "$ref") <(echo "$res") | head -6 | sed 's/^/      /'; fail=1; fi
}
check_same "the window over a CTE's consumer answers as off" "$cte"

# the spill: the table's files and, past their last level, its sort
plan=$(PGOPTIONS="$(session_opts orca-spill)" q "$D" postgres "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id, row_number() OVER (PARTITION BY id % 5000 ORDER BY t, id) FROM w")
if echo "$plan" | grep -q "Spill Files: [1-9]" && echo "$plan" | grep -q "Vec Window Hash Agg"; then
	echo "  ok past memory: $(echo "$plan" | grep -E 'Spill Files|Rows Spilled|Spill Depth' | tr -s ' ' | tr '\n' ' ')"
else
	echo "  FAILED past memory, no spill:"; echo "$plan" | sed 's/^/      /'; fail=1
fi
plan=$(PGOPTIONS="$(session_opts orca-spill)" q "$D" postgres "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT id, row_number() OVER (PARTITION BY big ORDER BY t, id) FROM w")
if echo "$plan" | grep -q "Rows Sorted Past the Last Level: [1-9]"; then
	echo "  ok one partition past memory, sorted past the last level: $(echo "$plan" | grep -E 'Rows Sorted Past' | tr -s ' ')"
else
	echo "  FAILED one partition past memory, not sorted past the last level:"; echo "$plan" | sed 's/^/      /'; fail=1
fi

# a rescan with nothing changed gives the partitions again; a changed
# parameter reads the input again (the correlated subquery above, and here
# a window under a nested loop's inner side)
res_off=$(PGOPTIONS="$(session_opts orca-off)" q "$D" postgres "SELECT a.k1, s.r FROM (SELECT DISTINCT k1 FROM w) a, LATERAL (SELECT max(r) AS r FROM (SELECT row_number() OVER (PARTITION BY b.k2 ORDER BY b.id) AS r FROM w b WHERE b.k1 = a.k1) x) s ORDER BY 1")
res_force=$(PGOPTIONS="$(session_opts orca-postgres)" q "$D" postgres "SELECT a.k1, s.r FROM (SELECT DISTINCT k1 FROM w) a, LATERAL (SELECT max(r) AS r FROM (SELECT row_number() OVER (PARTITION BY b.k2 ORDER BY b.id) AS r FROM w b WHERE b.k1 = a.k1) x) s ORDER BY 1")
if [ "$res_off" = "$res_force" ]; then
	echo "  ok rescans with a changed parameter answer as ORCA's sorted window"
else
	echo "  FAILED rescans:"; diff <(echo "$res_off") <(echo "$res_force") | head | sed 's/^/      /'; fail=1
fi

# ---------------------------------------------------------------------
# storages: the hashed window over each storage's batches
# ---------------------------------------------------------------------
echo "== storages"
for s in heap ao_row ao_column pax pax_porc_vec; do
	case "$s" in
		heap) clause="USING heap" ;;
		ao_row) clause="USING ao_row" ;;
		ao_column) clause="USING ao_column" ;;
		pax) clause="USING pax WITH (storage_format=porc)" ;;
		pax_porc_vec) clause="USING pax WITH (storage_format=porc_vec)" ;;
	esac
	out=$(q "$D" postgres "SET client_min_messages = warning; CREATE TABLE ws_$s (LIKE w) $clause; INSERT INTO ws_$s SELECT * FROM w; ANALYZE ws_$s;")
	case "$out" in *ERROR*) echo "  FAILED the table in $s: $out"; fail=1; continue ;; esac
	sql="SELECT id, k2, rank() OVER (PARTITION BY k2, k4 ORDER BY v DESC, o2), sum(f) OVER (PARTITION BY k1) FROM ws_$s"
	ref=$(comparable "$sql" "$(PGOPTIONS="$(session_opts orca-off)" q "$D" postgres "$sql")")
	ok=1
	for sess in orca-auto orca-postgres orca-arrow orca-random orca-spill; do
		res=$(comparable "$sql" "$(PGOPTIONS="$(session_opts "$sess")" q "$D" postgres "$sql")")
		if [ "$res" != "$ref" ]; then
			echo "  FAILED $s, $sess differs from orca-off"; diff <(echo "$ref") <(echo "$res") | head -6 | sed 's/^/      /'
			ok=0; fail=1
		fi
	done
	plan=$(PGOPTIONS="$(session_opts orca-postgres)" q "$D" postgres "EXPLAIN (VERBOSE, COSTS OFF) $sql")
	if [ $ok = 1 ] && echo "$plan" | grep -q "Vec Window Hash Agg" && echo "$plan" | grep -q "Input: batches"; then
		echo "  ok $s: a VecWindowHashAgg reads its VecScan's batches, and answers as ORCA's sorted window"
	elif [ $ok = 1 ]; then
		echo "  FAILED $s: no VecWindowHashAgg over batches:"; echo "$plan" | sed 's/^/      /'; fail=1
	fi
done

# ---------------------------------------------------------------------
# choices: ORCA's plans by cost alone
# ---------------------------------------------------------------------
echo "== choices"
out=$(q "$D" postgres "SET client_min_messages = warning;
CREATE TABLE f_col (id int, a int, b int, c numeric(12,2), pad1 text, pad2 text, pad3 text, pad4 text) USING ao_column;
INSERT INTO f_col SELECT g, g % 1000, g % 37, g / 7.0, repeat('a', 40), repeat('b', 40), repeat('c', 40), repeat('d', 40) FROM generate_series(1, 300000) g;
CREATE TABLE f_pax (LIKE f_col) USING pax;
INSERT INTO f_pax SELECT * FROM f_col;
CREATE TABLE f_heap AS SELECT * FROM f_col;
CREATE INDEX ON f_heap (a);
CREATE INDEX ON f_col (a);
CREATE INDEX ON f_pax (a);
CREATE TABLE dim AS SELECT g AS a, 'd' || g AS name, g % 10 AS grp FROM generate_series(1, 1000) g;
ANALYZE f_col; ANALYZE f_pax; ANALYZE f_heap; ANALYZE dim;")
case "$out" in *ERROR*) echo "  FAILED the choices' data: $out"; fail=1 ;; esac
# shape: the plan's nodes, in order, without their costs and properties --
# a vector node named as the row node it stands for, and a Hash node, which
# a VecHashJoin takes into itself, left out; ORCA's hashed window keeps its
# own name, the sorted window's Sort its own
shape() {
	sed -E 's/  \(cost=.*//' | awk 'NR == 1 || /->/' | sed -E 's/^ *(-> *)?//; s/^Vec (Window Hash Agg)/Hashed Window Partitions/; s/^Vec //' |
		grep -vx 'Hash'
}
# each query a line: what CCostModelVec's prices must do to ORCA's plan --
# "changes" it, or keeps its "same" shape -- and the query
CHOICE_QUERIES=(
	# a columnar table's scan priced by the columns it reads: ORCA's bitmap
	# index scan gives way to a full vector scan of two of its eight columns
	"changes SELECT count(*), sum(c) FROM f_col WHERE a BETWEEN 100 AND 130"
	"changes SELECT count(*), sum(c) FROM f_pax WHERE a BETWEEN 100 AND 130"
	# heap's pages are read whole either way: its index scan stays
	"same SELECT count(*), sum(c) FROM f_heap WHERE a BETWEEN 100 AND 130"
	# ORCA's hashed window, which it offers with vexec's prices: no Sort of
	# all its input
	"changes SELECT a, b, rank() OVER (PARTITION BY b ORDER BY c) FROM f_heap WHERE id < 100000"
	# a hash join's sides, priced with vexec's kernels and crossings
	"any SELECT f.b, count(*) FROM f_col f JOIN f_col g ON g.id = f.id + 1 WHERE f.a < 50 GROUP BY f.b"
	# a nested loop over an index, for one row: it stays
	"same SELECT f.a, sum(f.c) FROM f_heap f JOIN dim d ON d.a = f.a WHERE d.name = 'd42' GROUP BY f.a"
	"any SELECT d.grp, count(*), sum(f.c) FROM f_col f JOIN dim d ON d.a = f.a WHERE f.b < 30 GROUP BY d.grp"
)
nchanged=0
for line in "${CHOICE_QUERIES[@]}"; do
	expect="${line%% *}"
	sql="${line#* }"
	p_off=$(PGOPTIONS="$(session_opts orca-off)" q "$D" postgres "EXPLAIN $sql" | shape)
	p_auto=$(PGOPTIONS="$(session_opts orca-auto) -c vexec.min_rows=10000" q "$D" postgres "EXPLAIN $sql" | shape)
	ref=$(comparable "$sql" "$(PGOPTIONS="$(session_opts orca-off)" q "$D" postgres "$sql")")
	res=$(comparable "$sql" "$(PGOPTIONS="$(session_opts orca-auto) -c vexec.min_rows=10000" q "$D" postgres "$sql")")
	if [ "$res" != "$ref" ]; then
		echo "  FAILED auto answers otherwise than off: $sql"; fail=1
	fi
	if [ "$p_off" != "$p_auto" ]; then
		nchanged=$((nchanged + 1))
		echo "  changed by cost: $sql"
		diff <(echo "$p_off") <(echo "$p_auto") | sed 's/^/      /'
		[ "$expect" = same ] && { echo "  FAILED its shape was to stay"; fail=1; }
	else
		echo "  the same shape: $sql"
		[ "$expect" = changes ] && { echo "  FAILED its shape was to change"; fail=1; }
	fi
done
echo "  $nchanged of ${#CHOICE_QUERIES[@]} plans changed by CCostModelVec's prices, every answer the same"
# the cost model is ORCA's own in off and explain mode, and with
# vexec.orca_cost_model off: the plans are those without vexec's prices
for sess_opts in "-c vexec.mode=explain" "-c vexec.mode=auto -c vexec.orca_cost_model=off -c vexec.min_rows=10000"; do
	same=1
	for line in "${CHOICE_QUERIES[@]}"; do
		sql="${line#* }"
		p_off=$(PGOPTIONS="-c gp.optimizer=on -c vexec.mode=off" q "$D" postgres "EXPLAIN $sql" | shape)
		p_x=$(PGOPTIONS="-c gp.optimizer=on $sess_opts" q "$D" postgres "EXPLAIN $sql" | shape)
		[ "$p_off" = "$p_x" ] || same=0
	done
	if [ $same = 1 ]; then
		echo "  ok $sess_opts: ORCA's own prices, every plan's shape off's"
	else
		echo "  FAILED $sess_opts: a plan's shape differs from off's"; fail=1
	fi
done

# ---------------------------------------------------------------------
# settings: vexec.orca_settings
# ---------------------------------------------------------------------
echo "== settings"
out=$(q "$D" postgres "SET vexec.orca_settings = 'gp.optimizer_force_multistage_agg=on, gp.optimizer_enable_materialize=off'; SET vexec.mode = force; SET gp.optimizer = on;
	EXPLAIN (COSTS OFF) SELECT b, count(*) FROM f_col GROUP BY b;
	SHOW gp.optimizer_force_multistage_agg; SHOW gp.optimizer_enable_materialize")
if echo "$out" | tail -2 | tr '\n' ' ' | grep -q "^off on $"; then
	echo "  ok the session's settings are back after the statement's planning"
else
	echo "  FAILED the session's settings after planning:"; echo "$out" | sed 's/^/      /'; fail=1
fi
out=$(q "$D" postgres "SET vexec.orca_settings = 'work_mem=1MB'")
if echo "$out" | grep -q "is not one of ORCA's settings"; then
	echo "  ok a setting not ORCA's is refused"
else
	echo "  FAILED a setting not ORCA's: $out"; fail=1
fi
# gp.optimizer_enable_groupagg off only where every aggregation can hash:
# an ordered aggregate keeps it on, and ORCA still plans the statement
out=$(q "$D" postgres "SET vexec.orca_settings = 'gp.optimizer_enable_groupagg=off'; SET vexec.mode = force; SET gp.optimizer = on;
	EXPLAIN (COSTS OFF) SELECT b, string_agg(pad1, ',' ORDER BY id) FROM f_col WHERE id < 10 GROUP BY b")
if echo "$out" | grep -q "Optimizer: GPORCA"; then
	echo "  ok gp.optimizer_enable_groupagg=off left aside for an ordered aggregate: ORCA plans it"
else
	echo "  FAILED an ordered aggregate under gp.optimizer_enable_groupagg=off:"; echo "$out" | sed 's/^/      /'; fail=1
fi

# ---------------------------------------------------------------------
# registers: the one type every boolean register of a backend shares, made
# once in TopMemoryContext -- made in a statement's memory (since V1), a
# later statement's register read it freed: bool_and(NOT b2) under ORCA in
# force mode, a second time in a session, raised "no conversion from bit"
# or crashed the server
# ---------------------------------------------------------------------
echo "== registers"
out=$(q "$D" postgres "SET client_min_messages = warning;
CREATE TABLE f_bool (b1 bool, b2 bool);
INSERT INTO f_bool VALUES (true, NULL), (false, true), (NULL, false)")
case "$out" in *ERROR*) echo "  FAILED the registers' data: $out"; fail=1 ;; esac
# each statement a query of its own, as a client sends them: in one query
# string the first's freed memory is taken again by the same type's
out=$(PGOPTIONS="$(session_opts orca-postgres)" "$BINDIR/psql" -X -q -At -h "$D/sock" -U postgres -d postgres \
	-c "SELECT bool_and(NOT b2) FROM f_bool" -c "SELECT bool_and(NOT b2) FROM f_bool" -c "SELECT bool_and(NOT b2) FROM f_bool" 2>&1)
if [ "$(echo "$out" | tr '\n' ' ')" = "f f f " ]; then
	echo "  ok a boolean register's type outlives its statement: bool_and(NOT b2) three times in a session under ORCA"
else
	echo "  FAILED bool_and(NOT b2) three times in a session under ORCA:"; echo "$out" | sed 's/^/      /'; fail=1
fi

[ -n "${RESULTS_DIR:-}" ] && echo "$fail" > "$RESULTS_DIR/orcacost.fail"
if [ $fail = 0 ]; then echo "orcacost: passed"; else echo "orcacost: FAILED"; fi
exit $fail
