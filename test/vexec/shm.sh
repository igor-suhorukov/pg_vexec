#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The shm leg (pg_vector_executor.md §3.10, "A transport through shared
# memory", §6.5, §5 V7): the port's shm transport on a coordinator and
# VEXEC_SEGMENTS segments in one container, shm in every node's
# shared_preload_libraries after gp_core and gp.interconnect_type = shm in
# every node's configuration, ORCA -- whose plans alone stream -- on in every
# session.  On it:
#
#   1. Motions carry the same rows under shm as under tcp, and as the
#      planner's gathers, which stream nothing, as EXPLAIN shows them: a
#      Redistribute of narrow rows and of wide ones, rows longer than the
#      ring among them; a Broadcast; a random redistribution, an INSERT into
#      a table distributed randomly; a Gather into one segment, and a merge
#      of one; each through rings of 16 kB too, and through POSIX shared
#      memory (gp.shm_debug_shm_open), which leaves nothing in /dev/shm;
#   2. SET gp.interconnect_type moves the next statement between shm and
#      tcp, a prepared statement's generic plan included, as the segments'
#      logs say (gp.log_interconnect = verbose);
#   3. the remote path: pairs counted as on two hosts (gp.shm_debug_remote),
#      every slice with one over tcp through shm's delegation -- all, and a
#      mix, a statement with a slice through rings and one over tcp;
#   4. the hang tests of §6.5: a hash join whose build side is empty over a
#      Redistribute, under an Append whose next branch needs the same
#      senders, in vexec's off and force modes; a LIMIT over Motions; a CTE's
#      Sequence and Shared Scan read in two slices; cursors read by FETCH n
#      and closed early -- each statement ends, and no server process keeps
#      a mapping of shm's after it;
#   5. a sender or a receiver killed (SIGKILL) in the middle of a long
#      Redistribute fails the statement as under tcp, the cluster comes
#      back, and no process keeps a mapping, nor /dev/shm a file -- through
#      POSIX shared memory too;
#   6. the port's own interconnect test (pg19/test/ic), which carries shm
#      too, where the port's worktree is mounted (/cbsrc);
#   7. informally, a large Redistribute timed under tcp and under shm.
#
#   VEXEC_SEGMENTS   4
#   VEXEC_ROWS       rows of the narrow table: 300000
#   VEXEC_SHM_PARTS  the parts run: "motions switch remote hangs kills ic timing"
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
. "$here/lib.sh"

SEGMENTS="${VEXEC_SEGMENTS:-4}"
ROWS="${VEXEC_ROWS:-300000}"
PARTS=" ${VEXEC_SHM_PARTS:-motions switch remote hangs kills ic timing} "
PRELOAD="gp_core,shm,gp_orca,gp_sql,vexec"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/vexec-shm-XXXXXX")"
SOCK="$(mktemp -d /tmp/vxs-XXXXXX)"
BASEPORT=$((7500 + RANDOM % 200))
SECRET="vexec-$(od -An -tx8 -N16 /dev/urandom | tr -d " \n")"	# gp_core takes 16 characters at least
NODES="$(seq 0 "$SEGMENTS")"
DB=vexec_shm
datadir() { echo "$ROOT/node$1"; }
sockdir() { echo "$SOCK/n$1"; }
port()    { echo $((BASEPORT + $1)); }
fail=0

cleanup() {
	for n in $NODES; do
		[ -n "${RESULTS_DIR:-}" ] && cp "$ROOT/node$n.log" "$RESULTS_DIR/shm-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${KEEP:-}" ] && echo "kept: $ROOT" || rm -rf "$ROOT"
	rm -rf "$SOCK"
}
trap cleanup EXIT

cq() {						# cq <node> <db> <sql> [psql options]: -At
	local n="$1" db="$2" sql="$3"
	shift 3
	"$BINDIR/psql" -X -q -At -h "$(sockdir "$n")" -p "$(port "$n")" -U postgres -d "$db" "$@" -c "$sql" 2>&1
}
qf() {						# qf: statements on stdin, in one session of the coordinator's
	"$BINDIR/psql" -X -q -At -h "$(sockdir 0)" -p "$(port 0)" -U postgres -d $DB -f - 2>&1
}
check() {					# check <what> <got> <want>
	if [ "$2" = "$3" ]; then
		echo "  ok $1"
	else
		echo "  FAILED $1: got [$2], want [$3]"
		fail=1
	fi
}
# the server processes that map a channel of shm's, as pids
mapped() {
	local m
	for m in /proc/[0-9]*/maps; do
		grep -q -E 'memfd:gp_shm|/dev/shm/gp_shm' "$m" 2> /dev/null && basename "$(dirname "$m")"
	done | tr '\n' ' ' | sed 's/ $//'
}
# shm's files in /dev/shm, which there never are
devshm() { ls /dev/shm 2> /dev/null | grep -c '^gp_shm'; }
# shm's lines in the segments' logs, of a pattern
shm_lines() { cat "$ROOT"/node[1-9]*.log 2> /dev/null | grep -c "interconnect shm: .*$1"; }
# the server processes that have crashed
crashes() { cat "$ROOT"/node*.log 2> /dev/null | grep -c "terminated by signal"; }

echo "the shm leg: a coordinator and $SEGMENTS segments, $PRELOAD on every node, gp.interconnect_type = shm"
CONF="$ROOT/gp_cluster.conf"
for n in $NODES; do
	echo "$((n + 1)) $((n - 1)) p $(sockdir "$n") $(port "$n") $(datadir "$n")"
done > "$CONF"
for n in $NODES; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N -U postgres --locale=C --encoding=UTF8 > "$ROOT/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n"; tail -20 "$ROOT/initdb$n.log"; exit 1; }
	{
		echo "shared_preload_libraries = '$PRELOAD'"
		echo "unix_socket_directories = '$(sockdir "$n")'"
		echo "listen_addresses = ''"
		echo "port = $(port "$n")"
		echo "gp.cluster_config = '$CONF'"
		echo "gp.dbid = $((n + 1))"
		echo "gp.cluster_secret = '$SECRET'"
		echo "gp.interconnect_type = 'shm'"
		echo "gp.log_interconnect = verbose"
		echo "max_prepared_transactions = 100"
		echo "max_connections = 200"
		echo "shared_buffers = 128MB"
		echo "fsync = off"
		echo "max_parallel_workers_per_gather = 0"
		[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
	} >> "$(datadir "$n")/postgresql.auto.conf"
done
for n in $(echo $NODES | tr ' ' '\n' | sort -rn); do
	"$BINDIR/pg_ctl" -D "$(datadir "$n")" -l "$ROOT/node$n.log" -w -t 120 start > /dev/null 2>&1 \
		|| { echo "node $n did not start:"; tail -20 "$ROOT/node$n.log"; exit 1; }
done
for db in template1 postgres; do
	out=$(cq 0 "$db" "SET client_min_messages = warning; CREATE EXTENSION gp_core")
	[ -n "$out" ] && { echo "gp_core in $db: $out"; exit 1; }
done
out=$(cq 0 postgres "CREATE DATABASE $DB")
[ -n "$out" ] && { echo "CREATE DATABASE: $out"; exit 1; }
out=$(cq 0 $DB "SET client_min_messages = warning; CREATE EXTENSION gp_sql; CREATE EXTENSION gp_orca; CREATE EXTENSION vexec; CREATE EXTENSION gp_inject_fault")
[ -n "$out" ] && { echo "the extensions: $out"; exit 1; }
for n in $NODES; do
	check "node $n has shm loaded, and gp.interconnect_type = shm" \
		"$(cq "$n" postgres "SELECT current_setting('shared_preload_libraries') LIKE '%shm%', current_setting('gp.interconnect_type')")" "t|shm"
done
check "a session's SET gp.shm_ring_size reaches every segment" \
	"$(cq 0 $DB "SET gp.shm_ring_size = '64kB'; SELECT count(*) FROM gp.exec_on_segments('SELECT current_setting(''gp.shm_ring_size'')') WHERE result = '64kB'")" "$SEGMENTS"
out=$(cq 0 $DB "SET gp.shm_debug_remote = '1:x'")
case "$out" in
	*"invalid value for parameter"*) echo "  ok gp.shm_debug_remote refuses a pair it cannot read" ;;
	*) echo "  FAILED gp.shm_debug_remote took '1:x': $out"; fail=1 ;;
esac

# The tables: narrow rows; wide ones, stored as they are, so that a row
# crosses as long as it is -- up to 3 MB, past the ring's 256 kB, and a
# hundred times past a ring of 16 kB; a small table, for a Broadcast; one
# distributed randomly, which an INSERT redistributes to at random; and a
# large one, for the kills and the timing.
WROWS=$((ROWS / 3))
cat > "$ROOT/tables.sql" <<SQL
SET client_min_messages = warning;
CREATE TABLE n AS SELECT g AS id, g % 97 AS k, (g::bigint * 7919 % 100000)::int AS v, 'n' || (g % 503) AS s
FROM generate_series(1, $ROWS) g DISTRIBUTED BY (id);
CREATE TABLE w (id int, k int, payload text) DISTRIBUTED BY (id);
ALTER TABLE w ALTER COLUMN payload SET STORAGE EXTERNAL;
INSERT INTO w SELECT g, g % 89, repeat(md5(g::text), 4 + g % 29) FROM generate_series(1, $WROWS) g;
INSERT INTO w SELECT $WROWS + g, g, repeat(md5(g::text), 3200 * g) FROM generate_series(1, 16) g;
INSERT INTO w SELECT $WROWS + 16 + g, 17 + g, repeat(md5((g + 99)::text), 98304) FROM generate_series(1, 2) g;
CREATE TABLE sm AS SELECT g AS id, g % 97 AS k FROM generate_series(1, 2000) g DISTRIBUTED BY (id);
CREATE TABLE rnd (id int, k int, v int, s text) DISTRIBUTED RANDOMLY;
CREATE TABLE wu (a int PRIMARY KEY, b int) DISTRIBUTED BY (a);
INSERT INTO wu SELECT g, g FROM generate_series(1, 100) g;
CREATE TABLE e (k int, v int) DISTRIBUTED BY (k);
CREATE TABLE big AS SELECT g AS id, g % 100000 AS k, md5(g::text) || md5((g + 1)::text) AS s
FROM generate_series(1, 2000000) g DISTRIBUTED BY (id);
ANALYZE;
SQL
out=$("$BINDIR/psql" -X -q -At -v ON_ERROR_STOP=1 -h "$(sockdir 0)" -p "$(port 0)" -U postgres -d $DB -f "$ROOT/tables.sql" 2>&1)
[ -n "$out" ] && { echo "the tables: $out"; exit 1; }
crashes0=$(crashes)
# a statement whose rows cross a Redistribute, and its answer
SMQ="SELECT count(*) FROM sm a JOIN sm b ON a.k = b.id"
SMREF=$(cq 0 $DB "SET gp.optimizer = off; $SMQ")

###############################################################################
# 1. the same rows over shm as over tcp
###############################################################################
# same <what> <EXPLAIN's pattern> <sql>: ORCA's plan has the Motion, and the
# rows under tcp, under shm, through rings of 16 kB, through POSIX shared
# memory, every pair over tcp and a mix are the planner's.  The SQL is one
# statement a line, each ended by its semicolon, run in a session; its
# queries' plans are EXPLAIN's.  sets: the session's own settings.
same() {
	local what="$1" pattern="$2" sql="$3" plan want got variant bad=""
	plan=$(printf '%s\n' "SET gp.optimizer = on; ${sets:-}" \
		"$(echo "$sql" | sed -E 's/^(SELECT|UPDATE|INSERT|DELETE|WITH) /EXPLAIN (COSTS OFF) \1 /')" | qf)
	case "$plan" in
		*$pattern*"Optimizer: GPORCA"*) ;;
		*) echo "  FAILED $what: ORCA's plan has no $pattern:"; echo "$plan" | sed 's/^/    /'; fail=1; return ;;
	esac
	want=$(printf '%s\n' "SET gp.optimizer = off; ${sets:-}" "$sql" | qf | sort)
	for variant in "tcp" "shm" "shm; SET gp.shm_ring_size = '16kB'" "shm; SET gp.shm_debug_shm_open = on" \
		"shm; SET gp.shm_debug_remote = '*'" "shm; SET gp.shm_debug_remote = '*:0, 2:1'"; do
		got=$(printf '%s\n' "SET gp.optimizer = on; SET statement_timeout = '120s'; ${sets:-} SET gp.interconnect_type = $variant;" "$sql" | qf | sort)
		[ "$got" = "$want" ] || bad="$bad [$variant: $(echo "$got" | head -3 | tr '\n' ' ')]"
	done
	if [ -z "$bad" ] && [ -n "$want" ]; then
		echo "  ok $what: $(echo "$want" | head -1 | cut -c1-60)"
	else
		echo "  FAILED $what: the planner's $(echo "$want" | head -3 | tr '\n' ' ') /$bad"
		fail=1
	fi
}

if [[ "$PARTS" == *" motions "* ]]; then
	echo "1. Motions carry the same rows under shm as under tcp"
	lines0=$(shm_lines "through shared memory")
	same "a Redistribute of narrow rows, both sides of a join" "Redistribute Motion 4:4" \
		"SELECT count(*), sum(a.v), count(DISTINCT b.s) FROM n a JOIN n b ON a.k = b.v;"
	sets="SET gp.optimizer_enable_motion_broadcast = off;" same "a Redistribute of wide rows, up to 3 MB, past the ring" "Redistribute Motion 4:4" \
		"SELECT count(*), sum(length(w.payload)), sum(hashtext(w.payload)::bigint), max(length(w.payload)) FROM sm JOIN w ON sm.id = w.k;"
	same "a GROUP BY off the key" "Redistribute Motion 4:4" \
		"SELECT k % 7, count(*), sum(v), min(s), max(s) FROM n GROUP BY k % 7;"
	same "a Broadcast of the small side" "Broadcast Motion 4:4" \
		"SELECT count(*), sum(n.v) FROM n JOIN sm ON n.v = sm.k;"
	same "a random redistribution: an INSERT into a table distributed randomly, every segment given rows" "Redistribute Motion 4:4" \
		"BEGIN;
INSERT INTO rnd SELECT * FROM n;
SELECT count(*), sum(id), sum(hashtext(s)::bigint), count(DISTINCT gp_segment_id) FROM rnd;
ROLLBACK;"
	plan=$(cq 0 $DB "SET gp.optimizer = on; EXPLAIN (COSTS OFF) INSERT INTO rnd SELECT * FROM n")
	if echo "$plan" | grep -q "Redistribute Motion" && ! echo "$plan" | grep -q "Hash Key"; then
		echo "  ok the INSERT's Redistribute has no key: it sends each row to a segment in turn"
	else
		echo "  FAILED the INSERT into rnd has no random Redistribute:"; echo "$plan" | sed 's/^/    /'; fail=1
	fi
	same "a Gather to one segment, which an UPDATE's subquery reads" "Gather Motion 4:1" \
		"BEGIN;
UPDATE wu SET b = (SELECT max(v) FROM n WHERE k < 90) WHERE a < 50;
SELECT count(*), sum(b) FROM wu;
ROLLBACK;"
	same "a merge of a Gather to one segment, by its keys" "Merge Key" \
		"BEGIN;
UPDATE wu SET b = b + 1 WHERE a IN (SELECT k FROM n ORDER BY k, id LIMIT 30);
SELECT count(*), sum(b) FROM wu;
ROLLBACK;"
	same "a window partitioned off the key, merged in order" "Merge Key" \
		"SELECT id, rank() OVER (PARTITION BY k ORDER BY id DESC) FROM n WHERE id > $ROWS - 300 ORDER BY k, id;"
	check "the segments' logs have the senders through memfd rings, through POSIX ones, and over tcp" \
		"$( [ "$(shm_lines "(memfd)")" -gt 0 ] && [ "$(shm_lines "(POSIX)")" -gt 0 ] && [ "$(shm_lines "sends to $SEGMENTS receivers over tcp")" -gt 0 ] && echo yes)" "yes"
	check "nothing of shm's in /dev/shm" "$(devshm)" "0"
	check "no process maps a channel of shm's once the statements have ended" "$(mapped)" ""
fi

###############################################################################
# 2. a session's statements, between the transports
###############################################################################
if [[ "$PARTS" == *" switch "* ]]; then
	echo "2. SET gp.interconnect_type moves the next statement between shm and tcp"
	counts="$ROOT/counts"
	rm -f "$counts"
	logs="$(echo "$ROOT"/node[1-9]*.log)"
	count_cmd="\\! cat $logs | grep -c 'interconnect shm: slice' >> $counts"
	out=$(qf <<SQL
SET gp.optimizer = on;
SET gp.log_interconnect = verbose;
SET plan_cache_mode = force_generic_plan;
PREPARE p(int) AS SELECT count(*), sum(a.v) FROM n a JOIN n b ON a.k = b.v WHERE a.id > \$1;
$count_cmd
SET gp.interconnect_type = shm;
EXECUTE p(1000);
$count_cmd
SET gp.interconnect_type = tcp;
EXECUTE p(1000);
$count_cmd
SET gp.interconnect_type = shm;
EXECUTE p(1000);
$count_cmd
SET gp.interconnect_type = tcp;
EXECUTE p(1000);
$count_cmd
SELECT generic_plans, custom_plans FROM pg_prepared_statements WHERE name = 'p';
SET gp.interconnect_type = shm;
SELECT count(*), sum(a.v) FROM n a JOIN n b ON a.k = b.v WHERE a.id > 1000;
$count_cmd
SET gp.interconnect_type = tcp;
SELECT count(*), sum(a.v) FROM n a JOIN n b ON a.k = b.v WHERE a.id > 1000;
$count_cmd
SQL
)
	read -r c0 c1 c2 c3 c4 c5 c6 <<< "$(tr '\n' ' ' < "$counts")"
	rows=$(echo "$out" | grep '|' | sort -u)
	echo "    (shm's log lines after each statement: $c0 $c1 $c2 $c3 $c4 $c5 $c6)"
	if [ "$(echo "$rows" | grep -c .)" = 2 ] && echo "$rows" | grep -q '^4|0$' &&
		[ "$c1" -gt "$c0" ] && [ "$c2" = "$c1" ] && [ "$c3" -gt "$c2" ] && [ "$c4" = "$c3" ] &&
		[ "$c5" -gt "$c4" ] && [ "$c6" = "$c5" ]; then
		echo "  ok shm, tcp, shm, tcp: each statement over the transport set before it, the prepared one's generic plan, executed four times, too; the same rows"
	else
		echo "  FAILED the statements did not follow gp.interconnect_type: [$out] counts [$c0 $c1 $c2 $c3 $c4 $c5 $c6]"
		fail=1
	fi
fi

###############################################################################
# 3. the remote path, through shm's delegation to tcp
###############################################################################
if [[ "$PARTS" == *" remote "* ]]; then
	echo "3. pairs on two hosts, as gp.shm_debug_remote says, over tcp"
	# one Redistribute: n a, by k, to the segments of b's ids
	q="SELECT count(*), sum(a.v), sum(hashtext(b.s)::bigint) FROM n a JOIN n b ON a.k = b.id"
	plan=$(cq 0 $DB "SET gp.optimizer = on; EXPLAIN (COSTS OFF) $q")
	check "its plan has one Redistribute" "$(echo "$plan" | grep -c "Redistribute Motion")" "1"
	want=$(cq 0 $DB "SET gp.optimizer = on; SET gp.interconnect_type = tcp; $q")
	for remote in "*" "*:0" "0:*, 3:2" "*:0, *:1, *:2, *:3"; do
		r0=$(shm_lines "receives from $SEGMENTS senders over tcp")
		m0=$(shm_lines "receives from $SEGMENTS senders through shared memory")
		t0=$(shm_lines "sends to $SEGMENTS receivers over tcp")
		got=$(cq 0 $DB "SET gp.optimizer = on; SET gp.shm_debug_remote = '$remote'; $q")
		r1=$(shm_lines "receives from $SEGMENTS senders over tcp")
		m1=$(shm_lines "receives from $SEGMENTS senders through shared memory")
		t1=$(shm_lines "sends to $SEGMENTS receivers over tcp")
		check "every pair's rows, gp.shm_debug_remote = '$remote', as tcp's" "$got" "$want"
		check "  its Redistribute over tcp: receivers over tcp, through rings, senders over tcp" \
			"$((r1 - r0)) $((m1 - m0)) $((t1 - t0))" "$SEGMENTS 0 $SEGMENTS"
	done
	# a mix: the merge of a Gather into one segment, its rows redistributed
	# from there -- the pairs to a segment other than the Gather's counted as
	# on two hosts, the Redistribute goes over tcp and the Gather through
	# rings, in one statement
	q="BEGIN;
UPDATE wu SET b = b + 1 WHERE a IN (SELECT k FROM n ORDER BY k, id LIMIT 30);
SELECT count(*), sum(b) FROM wu;
ROLLBACK;"
	want=$(printf '%s\n' "SET gp.optimizer = on; SET gp.interconnect_type = tcp;" "$q" | qf)
	mixed=0
	for x in $(seq 0 $((SEGMENTS - 1))); do
		t0=$(shm_lines "over tcp")
		m0=$(shm_lines "through shared memory")
		got=$(printf '%s\n' "SET gp.optimizer = on; SET statement_timeout = '120s'; SET gp.shm_debug_remote = '*:$x';" "$q" | qf)
		t1=$(shm_lines "over tcp")
		m1=$(shm_lines "through shared memory")
		check "the merge's rows, the pairs to segment $x counted as on two hosts, as tcp's" "$got" "$want"
		[ $((t1 - t0)) -gt 0 ] && [ $((m1 - m0)) -gt 0 ] && mixed=$((mixed + 1))
	done
	check "for all segments but the Gather's, a statement with a slice over tcp and one through rings" "$mixed" "$((SEGMENTS - 1))"
	# two Motions, one below the other, over tcp, through rings of 16 kB too
	sets="SET gp.shm_debug_remote = '*:1';" same "two Motions, the pairs to segment 1 counted as on two hosts" \
		"(slice3; segments: 4)" "SELECT b.k, count(*), sum(a.v) FROM n a JOIN n b ON a.k = b.v GROUP BY b.k;"
	check "no process maps a channel of shm's" "$(mapped)" ""
fi

###############################################################################
# 4. the hang tests of §6.5, under shm
###############################################################################
if [[ "$PARTS" == *" hangs "* ]]; then
	echo "4. the hang tests: each statement ends, under shm"
	for mode in off force; do
		vs="SET vexec.mode = $mode;"
		# a hash join whose build side is empty, over a Redistribute: its
		# outer side never read, the next branch of the Append needing the
		# same senders
		q="SELECT count(*) FROM (SELECT a.k FROM n a JOIN (SELECT * FROM n WHERE v < 0) x ON a.v = x.k UNION ALL SELECT k FROM n WHERE v > 99000) u;"
		sets="$vs SET gp.optimizer_enable_motion_broadcast = off;" same "vexec $mode: an empty build side over a Redistribute, under an Append" \
			"Redistribute Motion" "$q"
		q="SELECT count(*) FROM n a JOIN e ON a.v = e.k;"
		sets="$vs SET gp.optimizer_enable_motion_broadcast = off;" same "vexec $mode: a hash join over an empty table's Redistribute" "Redistribute Motion" "$q"
		# a LIMIT over Motions, twice in a session, and the statement after
		out=$(qf <<SQL
SET gp.optimizer = on; SET statement_timeout = '60s'; $vs
SELECT count(*) FROM (SELECT a.id FROM n a JOIN n b ON a.k = b.v LIMIT 7) x;
SELECT count(*) FROM (SELECT w.id FROM w JOIN sm ON w.k = sm.k LIMIT 3) x;
SELECT count(*) FROM sm a JOIN sm b ON a.k = b.id;
SQL
)
		check "vexec $mode: a LIMIT stops its senders, twice, and the session goes on" "$(echo $out)" "7 3 $(cq 0 $DB "SET gp.optimizer = off; SELECT count(*) FROM sm a JOIN sm b ON a.k = b.id")"
		# a CTE made once and read in two slices: a Sequence, its Shared
		# Scan's consumers on either side of a Redistribute
		q="WITH c AS (SELECT * FROM n WHERE k < 40) SELECT count(*), sum(t1.v) FROM c t1 JOIN c t2 ON t1.id = t2.v;"
		sets="$vs SET gp.cte_sharing = on;" same "vexec $mode: a CTE's Sequence and Shared Scan, read in two slices" "Shared Scan" "$q"
		plan=$(cq 0 $DB "SET gp.optimizer = on; $vs SET gp.cte_sharing = on; EXPLAIN (COSTS OFF) ${q%;}")
		echo "$plan" | grep -q "Sequence" && [ "$(echo "$plan" | grep -c "Shared Scan")" -ge 3 ] \
			&& echo "  ok   its plan has the Sequence, the producer and two consumers" \
			|| { echo "  FAILED the CTE's plan:"; echo "$plan" | sed 's/^/    /'; fail=1; }
		# cursors: FETCH n, closed early, with another statement between;
		# and one whose COMMIT closes it
		ref=$(cq 0 $DB "SET gp.optimizer = off; SELECT count(*) FROM sm a JOIN sm b ON a.k = b.id")
		out=$(qf <<SQL
SET gp.optimizer = on; SET statement_timeout = '60s'; $vs
BEGIN;
DECLARE c1 CURSOR FOR SELECT a.id, b.s FROM n a JOIN n b ON a.k = b.v;
FETCH 5 FROM c1 \g /dev/null
\echo :ROW_COUNT
SELECT count(*) FROM sm a JOIN sm b ON a.k = b.id;
FETCH 50 FROM c1 \g /dev/null
\echo :ROW_COUNT
CLOSE c1;
DECLARE c2 CURSOR FOR SELECT w.id, length(w.payload) FROM w JOIN sm ON w.k = sm.k ORDER BY 2 DESC;
FETCH 3 FROM c2 \g /dev/null
\echo :ROW_COUNT
COMMIT;
SELECT count(*) FROM sm a JOIN sm b ON a.k = b.id;
SQL
)
		check "vexec $mode: cursors read by FETCH n and closed early, another statement between" "$(echo $out)" "5 $ref 50 3 $ref"
		check "vexec $mode: no process maps a channel of shm's after them" "$(mapped)" ""
	done
fi

###############################################################################
# 5. a sender or a receiver killed in the middle of a stream
###############################################################################
# A Redistribute whose receivers are slow -- a sleep on every other row in
# the join above it -- so that its senders wait on full rings; a process of
# segment 1 killed, a sender (a reader, sending its slice) or the receiver
# (the writer).  The statement fails, as under tcp; the statement's processes
# on the other segments end -- cancelled by the coordinator, from the port's
# 3eb3bf10859 (gp_dispatch.c, gang_cancel_running()), or as the loss reaches
# them through the transport -- as under tcp; segment 1 restarts, and the
# next statement answers; and no process keeps a mapping of shm's.
kill_one() {				# kill_one <transport> <sender|receiver> [settings]
	local t="$1" who="$2" sets="${3:-}" pid="" i out during st
	local q="SELECT count(*) FROM big a JOIN big b ON a.k = b.id WHERE pg_sleep(0.0001 * (a.id % 2) + 0 * b.k) IS NOT NULL"
	(cq 0 $DB "SET gp.optimizer = on; SET gp.interconnect_type = $t; $sets SET statement_timeout = '300s'; $q" > "$ROOT/kill.out" 2>&1) &
	local bg=$!
	for i in $(seq 1 300); do
		for p in $(cq 2 postgres "SELECT pid FROM pg_stat_activity WHERE pid <> pg_backend_pid() AND backend_type = 'client backend' AND state = 'active' AND query LIKE '%big a JOIN big b%'"); do
			case "$(tr '\0' ' ' < /proc/$p/cmdline 2> /dev/null)" in
				*FETCH*) [ "$who" = receiver ] && pid=$p ;;
				*SELECT*) [ "$who" = sender ] && pid=$p ;;
			esac
		done
		[ -n "$pid" ] && break
		sleep 0.1
	done
	sleep 2						# rows on the way, the rings full
	during="$(mapped | wc -w) $(devshm)"
	if [ -n "$pid" ]; then
		kill -9 "$pid"
	fi
	wait "$bg"
	out=$(cat "$ROOT/kill.out")
	# the statement's processes on the other segments end, and let go of
	# what they mapped
	local start=$(date +%s%N) left
	for i in $(seq 1 120); do
		left=$(for n in $(seq 1 "$SEGMENTS"); do
				cq "$n" postgres "SELECT count(*) FROM pg_stat_activity WHERE pid <> pg_backend_pid() AND backend_type = 'client backend' AND state = 'active' AND query LIKE '%big a JOIN big b%'"
			done | awk '/^[0-9]+$/ { s += $1 } END { print s + 0 }')
		[ "$left" = 0 ] && [ -z "$(mapped)" ] && break
		sleep 0.5
	done
	local ended=$(( ($(date +%s%N) - start) / 1000000 ))
	# the cluster comes back: a statement over the transport answers again
	for i in $(seq 1 120); do
		st=$(cq 0 $DB "SET gp.optimizer = on; SET gp.interconnect_type = $t; $sets $SMQ" 2>&1)
		[ "$st" = "$SMREF" ] && break
		sleep 1
	done
	echo "    $t$( [ -n "$sets" ] && echo " ($sets)"): the $who, pid ${pid:-none}, killed with $during processes mapping shm's memory and files in /dev/shm; the statement: $(echo "$out" | head -2 | tr '\n' ' ' | cut -c1-200)"
	[ -n "$pid" ] || { echo "  FAILED no $who of the statement found on segment 1"; fail=1; return; }
	# the check of mappings sees them while the statement runs, and only then
	case "$t:${during% *}" in
		tcp:0|shm:[1-9]*) ;;
		*) echo "  FAILED $t: $during processes mapped shm's memory while the statement ran"; fail=1 ;;
	esac
	case "$out" in
		*ERROR*) echo "  ok $t: the $who killed fails the statement" ;;
		*) echo "  FAILED $t: the statement went on when its $who was killed: $out"; fail=1 ;;
	esac
	check "$t: the statement's processes on the other segments ended within $ended ms of its failure" "$left" "0"
	check "$t: the cluster answers again after its $who was killed, after $i tries" "$st" "$SMREF"
	check "$t: no process maps a channel of shm's, nor has /dev/shm a file, after the $who was killed" "$(mapped)|$(devshm)" "|0"
}
if [[ "$PARTS" == *" kills "* ]]; then
	echo "5. a sender or a receiver killed in the middle of a stream"
	for t in tcp shm; do
		kill_one "$t" sender
		kill_one "$t" receiver
	done
	kill_one shm sender "SET gp.shm_debug_shm_open = on;"
	kill_one shm receiver "SET gp.shm_ring_size = '16kB';"
	check "the crashes are the kills' alone" "$(( $(crashes) - crashes0 ))" "6"
	crashes0=$(crashes)
fi

###############################################################################
# 6. the port's own interconnect test, shm among its transports
###############################################################################
if [[ "$PARTS" == *" ic "* ]]; then
	echo "6. the port's interconnect test (pg19/test/ic)"
	if [ -x /cbsrc/pg19/test/ic/run.sh ]; then
		out=$(bash /cbsrc/pg19/test/ic/run.sh 2>&1)
		echo "$out" | sed 's/^/    /' | grep -E "NOT OK|ok .*(shm|transports)|passed|failed" | head -60
		echo "$out" | grep -q "the transports: .*shm" || { echo "  FAILED the port's interconnect test has no shm"; fail=1; }
		echo "$out" | tail -1 | grep -q " 0 failed" && echo "  ok the port's interconnect test passes, shm among its transports" \
			|| { echo "  FAILED the port's interconnect test"; echo "$out" | grep -A2 "NOT OK" | sed 's/^/    /'; fail=1; }
	else
		echo "  (the port's worktree is not mounted at /cbsrc: skipped)"
	fi
fi

###############################################################################
# 7. informally: a large Redistribute, timed
###############################################################################
if [[ "$PARTS" == *" timing "* ]]; then
	echo "7. informally, Redistributes timed, each way, alternately, three times (one host; these legs' 8 CPUs)"
	out=$(cq 0 $DB "SET client_min_messages = warning;
		CREATE TABLE w2 AS SELECT g AS id, g % 100000 AS k, repeat(md5(g::text), 56) AS p FROM generate_series(1, 200000) g DISTRIBUTED BY (id);
		ANALYZE w2")
	[ -n "$out" ] && echo "    w2: $out"
	timed() {				# timed <what> <sql>
		local t start ms got all=""
		for t in tcp shm tcp shm tcp shm; do
			start=$(date +%s%N)
			got=$(cq 0 $DB "SET gp.optimizer = on; SET gp.optimizer_enable_motion_broadcast = off; SET gp.interconnect_type = $t; $2")
			ms=$(( ($(date +%s%N) - start) / 1000000 ))
			all="$all $t:$ms"
		done
		echo "    $1 ($got):$all ms"
	}
	timed "2,000,000 rows of ~100 bytes into a small hash table" "SELECT count(*) FROM big a JOIN sm ON a.k = sm.id"
	timed "200,000 rows of ~1.8 kB into a small hash table" "SELECT count(*), sum(length(w2.p)) FROM w2 JOIN sm ON w2.k = sm.id"
	timed "2,000,000 rows of ~100 bytes into a join of 2,000,000" "SELECT count(*), sum(length(a.s)) FROM big a JOIN big b ON a.k = b.id"
fi

check "no server process crashed but the ones killed" "$(( $(crashes) - crashes0 ))" "0"
echo "shm: $([ $fail -eq 0 ] && echo passed || echo FAILED)"
exit $fail
