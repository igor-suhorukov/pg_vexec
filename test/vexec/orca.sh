#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# ORCA on vanilla PostgreSQL 19 (pg_vector_executor.md §3.3.5, V6): the
# port's gp_orca built alone for one node without gp_core (orcabuild.sh), on
# REL_19_STABLE as it is, with vexec beside it.  In the vanilla-orca image,
# the stage installed over the server (build.sh), and the port's worktree it
# was built from at /cbsrc.
#
#   module        gp_orca alone: it loads with no gp_core and no core series,
#                 its extension is made, ORCA plans queries and writes,
#                 counts them, and has every relation the node's; it refuses
#                 gp_core loaded before it or after it (vexec_fake_gp_core,
#                 a module of the tests', publishes gp_core's rendezvous);
#                 and with vexec, in either preload order, ORCA's plans carry
#                 vector nodes, answering as ORCA's row plans and PostgreSQL's
#                 planner answer
#   pg            PostgreSQL's own suite, its parallel_schedule, with ORCA on
#                 and vexec preloaded off, against its unchanged expected
#                 output, compared as the port's singlenode suite compares it
#                 under ORCA (pg19/test/singlenode/run.sh): gpdiff.pl, plans
#                 not compared, gp_orca's Optimizer line dropped -- or exactly
#                 a difference reviewed and kept: the port's
#                 (pg19/test/singlenode/orca/) or this leg's (orca/kept/)
#   differential  the same suite in more sessions, each compared with pg's by
#                 differential.py, its kept differences in
#                 differential/kept/pg-orca/<session>/: off once more, the
#                 suite's own noise; vexec.mode = force in each format; and
#                 force with the per-structure settings drawn at random
#
# Each session runs on a server of its own, made afresh, as the port's suite
# runs PostgreSQL's tests on one, with every connection's settings in
# PGOPTIONS.  A watchdog cancels a statement that runs past a minute, and at
# once the one statement of subselect's that ORCA is known not to finish
# (the port's orca/README).
#
#   VEXEC_ORCA_SECTIONS  "module pg differential"; differential runs pg's
#                        session as its reference
#   VEXEC_SESSIONS       differential's other sessions: "off-again postgres
#                        arrow random"
#   VEXEC_SEED           the random session's seed: drawn and printed
#   VEXEC_ROWS           the module section's table: 20000 rows
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
. "$here/lib.sh"

PKGLIB="$("$BINDIR/pg_config" --pkglibdir)"
[ -f "$PKGLIB/gp_orca.so" ] \
	|| { echo "orca: this server has no gp_orca; give the leg its stage (run.sh orcabuild)"; exit 1; }
[ -f /cbsrc/pg19/test/singlenode/canon.pl ] && [ -f /cbsrc/src/test/regress/gpdiff.pl ] \
	|| { echo "orca: no worktree of the port at /cbsrc (VEXEC_PORT_SRC)"; exit 1; }
[ ! -f "$PKGLIB/gp_core.so" ] || { echo "orca: this server has gp_core; the leg is the vanilla one's"; exit 1; }

PGSUITE="${PG_REGRESS_SUITE:-/cb/pgregress}"
PG_REGRESS="$PKGLIB/pgxs/src/test/regress/pg_regress"
SN=/cbsrc/pg19/test/singlenode
W="$(mktemp -d "${TMPDIR:-/tmp}/vexec-orca-XXXXXX")"
SECTIONS="${VEXEC_ORCA_SECTIONS:-module pg differential}"
SEED="${VEXEC_SEED:-$(( (RANDOM << 15 | RANDOM) % 2147483646 + 1 ))}"
ROWS="${VEXEC_ROWS:-20000}"

fail=0
oks=0
ok() { printf '  ok     %s\n' "$1"; oks=$((oks + 1)); }
notok() {
	printf '  NOT OK %s\n' "$1"
	[ -n "${2:-}" ] && printf '%s\n' "$2" | head -12 | sed 's/^/         /'
	fail=1
}
is() { [ "$2" = "$3" ] && ok "$1" || notok "$1" "want [$3], got [$2]"; }
has() {
	case "$2" in
		*"$3"*) ok "$1" ;;
		*) notok "$1" "expected [$3] in: $2" ;;
	esac
}
# a server's log, for what no query returned: a crash, a failed assertion
log_clean() {				# log_clean <what> <dir>
	local bad
	bad=$(grep -E "PANIC|TRAP:|terminated by signal|server process .* was terminated" "$2/log" | head -5)
	[ -z "$bad" ] && ok "$1: no crash in the server's log" || notok "$1: the server's log" "$bad"
}

echo "orca: gp_orca built alone ($(cat /orcastage/COMMIT 2> /dev/null || echo '?')), on PostgreSQL $(cat "$("$BINDIR/pg_config" --bindir)/../.pg_ref_commit" 2> /dev/null)"
echo "  random session's seed: $SEED (VEXEC_SEED=$SEED reruns it)"

###############################################################################
# module
###############################################################################

# The workload of the cluster leg (cluster.sh), over one heap table: scans,
# filters, aggregates, hash joins of each type, a window, a top-N.
QUERIES=(
	"SELECT count(*), sum(v), min(s), max(d) FROM %t"
	"SELECT count(*), count(k), count(d) FROM %t"
	"SELECT k, count(*), sum(n), avg(v) FROM %t WHERE b GROUP BY k"
	"SELECT s, count(DISTINCT k) FROM %t GROUP BY s HAVING count(*) > 90"
	"SELECT a.k, count(*) FROM %t a JOIN %t b ON a.id = b.v WHERE b.k < 10 GROUP BY a.k"
	"SELECT a.id, b.id, b.s FROM %t a JOIN %t b ON a.k = b.k AND a.v < b.v WHERE a.id < 200 AND b.id < 300"
	"SELECT a.k, count(b.id), sum(b.v) FROM %t a LEFT JOIN %t b ON a.v = b.id AND b.b WHERE a.id % 50 = 0 GROUP BY a.k"
	"SELECT count(*), sum(a.v) FROM %t a WHERE EXISTS (SELECT 1 FROM %t b WHERE b.v = a.id AND b.k > a.k)"
	"SELECT count(*), sum(a.v) FROM %t a WHERE NOT EXISTS (SELECT 1 FROM %t b WHERE b.id = a.v AND b.d > a.d)"
	"SELECT b.id, a.id FROM %t a RIGHT JOIN %t b ON a.s = b.s AND a.id = b.k WHERE b.id < 120"
	"SELECT d, sum(v) OVER (PARTITION BY k ORDER BY id) FROM %t WHERE id % 1000 = 7"
	"SELECT id, v FROM %t WHERE v > 99990 ORDER BY v, id LIMIT 20"
)
SESSIONS=("off" "explain" "force-postgres" "force-arrow" "force-random")
session_sets() {
	case "$1" in
		off) echo "SET vexec.mode = off;" ;;
		explain) echo "SET vexec.mode = explain;" ;;
		force-postgres) echo "SET vexec.mode = force; SET vexec.batch_format = postgres; SET vexec.debug_require_vector = on;" ;;
		force-arrow) echo "SET vexec.mode = force; SET vexec.batch_format = arrow; SET vexec.debug_require_vector = on;" ;;
		force-random) echo "SET vexec.mode = force; SET vexec.debug_layout_seed = $SEED; SET vexec.debug_require_vector = on;" ;;
	esac
}

section_module() {
	local D plan ref got sql nq nsame norca kinds order i

	echo
	echo "== module: gp_orca alone"
	D="$W/alone"
	server_init "$D" "shared_preload_libraries = 'gp_orca'" || { notok "a server with gp_orca alone is made"; return; }
	server_start "$D" || { notok "a server with gp_orca alone starts" "$(tail -5 "$D/log")"; return; }
	ok "a vanilla server starts with gp_orca alone in shared_preload_libraries"
	is "no gp_core is there to load" \
		"$(q "$D" postgres "SELECT count(*) FROM pg_available_extensions WHERE name = 'gp_core'")" "0"
	is "gp_orca's extension is made, and requires nothing" \
		"$(q "$D" postgres "SET client_min_messages = warning; CREATE EXTENSION gp_orca; SELECT string_agg(extname, ',' ORDER BY extname) FROM pg_extension WHERE extname <> 'plpgsql'")" "gp_orca"
	is "ORCA comes up in a backend" \
		"$(q "$D" postgres "SELECT source, xforms > 100, initialized FROM gp_orca.version()")" "Apache Cloudberry|t|t"
	is "it is Cloudberry's ORCA, with the rules only Cloudberry's has" \
		"$(q "$D" postgres "SELECT count(*) FROM gp_orca.xforms() x WHERE x IN ('CXformGet2ParallelTableScan', 'CXformPushPartialAggBelowJoin', 'CXformImplementHashSequenceProject')")" "3"
	is "gp.optimizer is on" "$(q "$D" postgres "SHOW gp.optimizer")" "on"

	q "$D" postgres "
		CREATE TABLE t (id int PRIMARY KEY, k int, v int, s text);
		INSERT INTO t SELECT g, g % 97, (g * 7919) % 100000, 'name ' || (g % 503) FROM generate_series(1, $ROWS) g;
		CREATE TABLE p (id int, k int) PARTITION BY RANGE (id);
		CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (10000);
		CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (10000) TO (MAXVALUE);
		INSERT INTO p SELECT g, g % 13 FROM generate_series(1, $ROWS) g;
		CREATE TABLE w AS SELECT * FROM t;
		ANALYZE" > /dev/null
	sql="SELECT a.k, count(*), sum(b.v) FROM t a JOIN t b ON a.id = b.v WHERE b.k < 10 GROUP BY a.k"
	plan=$(q "$D" postgres "EXPLAIN (COSTS OFF) $sql")
	has "ORCA plans a join and an aggregate" "$plan" "Optimizer: GPORCA"
	is "and answers as PostgreSQL's planner answers" \
		"$(q "$D" postgres "$sql" | sort | md5sum)" "$(q "$D" postgres "SET gp.optimizer = off; $sql" | sort | md5sum)"
	is "every relation is the node's: ORCA is told of no policy" \
		"$(q "$D" postgres "SELECT kind IS NULL FROM gp_orca.relation_policy('t')")" "t"
	plan=$(q "$D" postgres "EXPLAIN (COSTS OFF) SELECT count(*), sum(k) FROM p WHERE id > 15000")
	has "ORCA plans a partitioned table's scan" "$plan" "Optimizer: GPORCA"
	is "and answers as the planner" \
		"$(q "$D" postgres "SELECT count(*), sum(k) FROM p WHERE id > 15000")" \
		"$(q "$D" postgres "SET gp.optimizer = off; SELECT count(*), sum(k) FROM p WHERE id > 15000")"
	for sql in "INSERT INTO t SELECT id + 1000000, k, v, s FROM t WHERE id < 50" \
	           "UPDATE t SET v = v + 1, s = s || '!' WHERE k = 3" \
	           "DELETE FROM t WHERE k = 5 AND id % 2 = 0"; do
		has "ORCA plans a write: $(echo "$sql" | cut -d' ' -f1)" "$(q "$D" postgres "EXPLAIN (COSTS OFF) $sql")" "Optimizer: GPORCA"
		q "$D" postgres "$sql" > /dev/null
	done
	# the same writes into w, under the planner, give the same table
	q "$D" postgres "SET gp.optimizer = off;
		INSERT INTO w SELECT id + 1000000, k, v, s FROM w WHERE id < 50;
		UPDATE w SET v = v + 1, s = s || '!' WHERE k = 3;
		DELETE FROM w WHERE k = 5 AND id % 2 = 0" > /dev/null
	is "and writes what the planner's plans write" \
		"$(q "$D" postgres "SELECT count(*) FROM (SELECT * FROM t EXCEPT ALL SELECT * FROM w) x")|$(q "$D" postgres "SELECT count(*) FROM t")" \
		"0|$(q "$D" postgres "SELECT count(*) FROM w")"
	is "it counts what it planned" \
		"$(q "$D" postgres "SELECT count > 0 FROM gp_orca.fallbacks() WHERE reason = 'planned'")" "t"
	server_stop "$D"
	log_clean "gp_orca alone" "$D"

	echo
	echo "== module: gp_orca refuses gp_core"
	rm -rf "$W/fake"
	cp -r "$here/orca/fake_gp_core" "$W/fake"
	if make -s -C "$W/fake" PG_CONFIG="$BINDIR/pg_config" VEXEC_PGXS_INCLUDE=/src/modules/vexec/pgxs/include > "$W/fake.log" 2>&1 &&
	   make -s -C "$W/fake" PG_CONFIG="$BINDIR/pg_config" VEXEC_PGXS_INCLUDE=/src/modules/vexec/pgxs/include install >> "$W/fake.log" 2>&1; then
		i=0
		for order in "vexec_fake_gp_core,gp_orca" "gp_orca,vexec_fake_gp_core"; do
			i=$((i + 1))
			D="$W/refuse$i"
			server_init "$D" "shared_preload_libraries = '$order'" || { notok "a server is made"; continue; }
			if server_start "$D"; then
				server_stop "$D"
				notok "with gp_core's rendezvous published ($order), the server does not start"
			else
				has "with gp_core's rendezvous published ($order), the server does not start" \
					"$(cat "$D/log")" "this gp_orca is built for a server without \"gp_core\", which is loaded"
			fi
		done
	else
		notok "vexec_fake_gp_core builds" "$(tail -5 "$W/fake.log")"
	fi

	i=0
	for order in "gp_orca,vexec" "vexec,gp_orca"; do
		i=$((i + 1))
		echo
		echo "== module: vexec under ORCA, preloaded $order"
		D="$W/vexec$i"
		server_init "$D" "shared_preload_libraries = '$order'" || { notok "a server is made"; continue; }
		server_start "$D" || { notok "it starts" "$(tail -5 "$D/log")"; continue; }
		q "$D" postgres "
			CREATE TABLE t_heap AS
			SELECT g AS id, g % 97 AS k, (g * 7919) % 100000 AS v, ((g % 1000) / 7.0)::numeric(12,4) AS n,
			       'name ' || (g % 503) AS s, DATE '2020-01-01' + g % 1000 AS d, g % 3 = 0 AS b
			FROM generate_series(1, $ROWS) g;
			ANALYZE t_heap" > /dev/null
		nq=0; nsame=0; norca=0; kinds=""
		for qt in "${QUERIES[@]}"; do
			sql="${qt//%t/t_heap}"
			ref=$(q "$D" postgres "SET gp.optimizer = off; SET vexec.mode = off; $sql" | sort)
			for s in "${SESSIONS[@]}"; do
				got=$(q "$D" postgres "SET gp.optimizer = on; $(session_sets "$s") $sql" | sort)
				nq=$((nq + 1))
				if [ "$got" = "$ref" ]; then
					nsame=$((nsame + 1))
				else
					notok "under ORCA, $s answers as PostgreSQL's planner: $sql" "$(diff <(echo "$ref") <(echo "$got") | head -8)"
				fi
			done
			plan=$(q "$D" postgres "SET gp.optimizer = on; $(session_sets force-postgres) EXPLAIN (COSTS OFF) $sql")
			if echo "$plan" | grep -q "Optimizer: GPORCA"; then
				norca=$((norca + 1))
			else
				notok "ORCA plans, in force mode: $sql" "$plan"
			fi
			kinds="$kinds$(echo "$plan" | grep -o -E 'Vec [A-Za-z ]+' | sed -E 's/ on .*//; s/ +$//')"$'\n'
		done
		is "every query of the workload, in each session, answers as PostgreSQL's planner does" "$nsame of $nq" "$nq of $nq"
		is "ORCA plans each in force mode, every plan with the vector nodes it could have (vexec.debug_require_vector)" \
			"$norca of ${#QUERIES[@]}" "${#QUERIES[@]} of ${#QUERIES[@]}"
		for k in "Vec Seq Scan" "Vec Hash Join" "Vec Hash Left Join" "Vec Hash Semi Join" "Vec Hash Anti Join" "Vec HashAggregate" "Vec Aggregate"; do
			echo "$kinds" | grep -q -x "$k" && ok "ORCA's plans hold a $k" \
				|| notok "ORCA's plans hold a $k" "$(echo "$kinds" | sort | uniq -c | sort -rn | head -12)"
		done
		echo "    the vector nodes of ORCA's plans: $(echo "$kinds" | sed '/^$/d' | sort | uniq -c | sort -rn | awk '{ n = $1; $1 = ""; printf "%s%s %d", sep, substr($0, 2), n; sep = ", " }')"
		plan=$(q "$D" postgres "SET gp.optimizer = on; SET vexec.mode = force; EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT k, count(*) FROM t_heap WHERE v > 1000 GROUP BY k")
		if echo "$plan" | grep -q "GPORCA" && echo "$plan" | grep -q -E "Vec Seq Scan on t_heap \(actual rows=[1-9]"; then
			ok "EXPLAIN ANALYZE shows ORCA's vector scan run, with its rows"
		else
			notok "EXPLAIN ANALYZE shows ORCA's vector scan run, with its rows" "$plan"
		fi
		server_stop "$D"
		log_clean "vexec under ORCA ($order)" "$D"
	done
}

###############################################################################
# pg and differential: PostgreSQL's suite under ORCA
###############################################################################

# The suite's comparison under ORCA, as the port's singlenode suite makes it
# for PostgreSQL's tests (pg19/test/singlenode/run.sh): gpdiff.pl, from
# Cloudberry's tree, with plans not compared under the port's init_file_pg,
# and gp_orca's Optimizer line taken out first (strip_optimizer.pl); a test
# that differs passes if what differs is exactly a difference reviewed and
# kept, in the form canon.pl gives it -- the port's, of its server, or this
# leg's own, of vanilla's.  Any other difference is left in canon/.
setup_compare() {
	mkdir -p "$W/gpdiff" "$W/bin"
	cp /cbsrc/src/test/regress/gpdiff.pl /cbsrc/src/test/regress/atmsort.pm /cbsrc/src/test/regress/explain.pm "$W/gpdiff/"
	sed 's/##Version: ##/Apache Cloudberry (the PostgreSQL 19 port)/' \
		/cbsrc/src/test/regress/GPTest.pm.in > "$W/gpdiff/GPTest.pm"
	cat > "$W/bin/diff" <<EOF
#!/bin/bash
# pg_regress runs: diff [options] expected results
n=\$#
exp="\${@:\$((n - 1)):1}"
res="\${@:\$n:1}"
opts=("\${@:1:\$((n - 2))}")
name=\$(basename "\$exp" .out)
stripped="$W/\$VEXEC_ORCA_SESSION/stripped/\$(basename "\$res")"
mkdir -p "\$(dirname "\$stripped")" "$W/\$VEXEC_ORCA_SESSION/canon"
perl "$SN/strip_optimizer.pl" "\$res" > "\$stripped"
res="\$stripped"
pg=(-I GP_IGNORE: --gpd_ignore_plans --gpd_init "$SN/init_file_pg")
canon="$W/\$VEXEC_ORCA_SESSION/canon/\$name.diff"
env PATH=/usr/bin:/bin perl "$W/gpdiff/gpdiff.pl" -U0 "\${pg[@]}" "\$exp" "\$res" 2> /dev/null |
	perl "$SN/canon.pl" > "\$canon"
st=("\${PIPESTATUS[@]}")
[ "\${st[0]}" -le 1 ] && [ "\${st[1]}" -eq 0 ] || echo "no comparison was made" >> "\$canon"
if [ ! -s "\$canon" ] || cmp -s "\$canon" "$SN/orca/\$name.diff" || cmp -s "\$canon" "$here/orca/kept/\$name.diff"; then
	rm -f "\$canon"
	exit 0
fi
exec env PATH=/usr/bin:/bin perl "$W/gpdiff/gpdiff.pl" "\${opts[@]}" "\${pg[@]}" "\$exp" "\$res"
EOF
	chmod +x "$W/bin/diff"
}

session_options() {
	case "$1" in
		off|off-again) echo "-c gp.optimizer=on -c vexec.mode=off" ;;
		postgres) echo "-c gp.optimizer=on -c vexec.mode=force -c vexec.batch_format=postgres" ;;
		arrow) echo "-c gp.optimizer=on -c vexec.mode=force -c vexec.batch_format=arrow" ;;
		random) echo "-c gp.optimizer=on -c vexec.mode=force -c vexec.batch_format=postgres -c vexec.debug_layout_seed=$SEED" ;;
		*) echo "unknown session $1" >&2; return 1 ;;
	esac
}

# A statement that runs for minutes is a finding, not something to wait for:
# cancelled after a minute, and subselect's that ORCA does not finish as soon
# as it is seen, found by its innermost subquery (the port's run.sh).
HANGS='not exists \( select 1 from tenk1 d\s+where a\.thousand = d\.thousand \)'
watchdog() {				# watchdog <server dir> <file of what it cancelled>
	local pid query
	while :; do
		sleep 2
		PGOPTIONS="-c gp.optimizer=off -c vexec.mode=off" "$BINDIR/psql" -X -q -t -A -F ' ' -h "$1/sock" -U postgres -d postgres -c "
			SELECT pid, regexp_replace(left(query, 300), '\\s+', ' ', 'g')
			  FROM pg_stat_activity
			 WHERE datname = 'regression' AND state = 'active'
			   AND (now() - query_start > interval '60 seconds'
			        OR (query ~ '$HANGS' AND now() - query_start > interval '2 seconds'))" 2> /dev/null |
		while read -r pid query; do
			[ -n "$pid" ] || continue
			PGOPTIONS="-c gp.optimizer=off -c vexec.mode=off" "$BINDIR/psql" -X -q -t -A -h "$1/sock" -U postgres -d postgres \
				-c "SELECT pg_cancel_backend($pid)" > /dev/null 2>&1 &&
				echo "$(date +%T)  $query" >> "$2"
		done
	done
}

# one session of PostgreSQL's suite on a server of its own: its results, its
# server's log and pg_regress's verdicts in $W/pg-orca/<session>
run_session() {				# run_session <session>
	local s="$1" D="$W/server-$1" out="$W/pg-orca/$1" start rc dog
	mkdir -p "$out"
	server_init "$D" "shared_preload_libraries = 'gp_orca,vexec'" "log_line_prefix = '%a|%e|'" \
		"log_min_messages = warning" || return 1
	server_start "$D" || { echo "    $s: the server did not start"; tail -5 "$D/log"; return 1; }
	watchdog "$D" "$out/cancelled" &
	dog=$!
	start=$(date +%s)
	( cd "$PGSUITE" && PATH="$W/bin:$PATH" VEXEC_ORCA_SESSION="$s" PGOPTIONS="$(session_options "$s")" "$PG_REGRESS" \
		--bindir="$BINDIR" --inputdir="$PGSUITE" --expecteddir="$PGSUITE" --outputdir="$out" \
		--dlpath="$PGSUITE" --schedule="$W/pg_schedule" --host="$D/sock" --user=postgres \
		> "$out/pg_regress.out" 2>&1 )
	rc=$?
	kill "$dog" 2> /dev/null
	wait "$dog" 2> /dev/null
	server_stop "$D"
	cp "$D/log" "$out/postmaster.log"
	# a test that passed on an alternative expected output left a difference
	# from the first one it was compared with, which is no finding
	sed -nE 's/^ok +[0-9]+ +[-+] +([^ ]+) .*/\1/p' "$out/pg_regress.out" | while read -r t; do
		rm -f "$W/$s/canon/$t.diff" "$W/$s/canon/$t"_[0-9].diff
	done
	echo "    $s: $(grep -E "^# (All|[0-9]+ of [0-9]+)" "$out/pg_regress.out" | tail -1 | sed 's/^# //') against the expected output, under ORCA's comparison ($(( $(date +%s) - start )) s)"
	if [ -s "$out/cancelled" ]; then
		echo "      cancelled: $(wc -l < "$out/cancelled") statements"
		sed 's/^/        /' "$out/cancelled" | cut -c1-160
	fi
	return $rc
}

section_pg() {
	local rc unreviewed

	echo
	echo "== pg: PostgreSQL's suite under ORCA, vexec preloaded off"
	run_session off
	rc=$?
	unreviewed=$(cd "$W/off/canon" 2> /dev/null && ls -- *.diff 2> /dev/null | sed 's/\.diff$//')
	if [ "$rc" -eq 0 ] && [ -z "$unreviewed" ]; then
		ok "PostgreSQL's suite passes with ORCA on, every difference from its expected output reviewed and kept"
	else
		notok "PostgreSQL's suite passes with ORCA on" "$(grep -E '^not ok' "$W/pg-orca/off/pg_regress.out" | head -20)
differences no one has reviewed: $unreviewed"
	fi
	echo "    the kept differences it met: $(grep -c -E '^ok ' "$W/pg-orca/off/pg_regress.out") tests passed of $(grep -c -E '^(not )?ok ' "$W/pg-orca/off/pg_regress.out")"
}

section_differential() {
	local s sessions="off"

	echo
	echo "== differential: PostgreSQL's suite under ORCA, vexec in each session against off"
	[ -d "$W/pg-orca/off/results" ] || run_session off
	for s in ${VEXEC_SESSIONS:-off-again postgres arrow random}; do
		run_session "$s"
		sessions="$sessions $s"
	done
	if python3 "$here/differential.py" "$W/pg-orca" $sessions --kept "$here/differential/kept/pg-orca"; then
		ok "every session of PostgreSQL's suite under ORCA answers as off, but for the kept differences"
	else
		notok "every session of PostgreSQL's suite under ORCA answers as off (differential.py, above)"
	fi
}

case " $SECTIONS " in
	*" pg "*|*" differential "*)
		setup_compare
		grep -v '^#' "$here/differential/excluded" | awk '{print $1}' | sed '/^$/d' > "$W/excluded"
		awk -v excluded="$(tr '\n' ' ' < "$W/excluded")" '
			BEGIN { n = split(excluded, e, " "); for (i = 1; i <= n; i++) x[e[i]] }
			/^test:/ { line = "test:"; for (i = 2; i <= NF; i++) if (!($i in x)) line = line " " $i;
					   if (line != "test:") print line; next }
			{ print }' "$PGSUITE/parallel_schedule" > "$W/pg_schedule"
		;;
esac
for section in $SECTIONS; do
	case "$section" in
		module) section_module ;;
		pg) section_pg ;;
		differential) section_differential ;;
		*) echo "orca: unknown section $section"; exit 1 ;;
	esac
done

if [ -n "${RESULTS_DIR:-}" ]; then
	mkdir -p "$RESULTS_DIR/orca"
	[ -d "$W/pg-orca" ] && cp -r "$W/pg-orca" "$RESULTS_DIR/orca/"
	for s in "$W"/*/canon; do
		[ -d "$s" ] && [ -n "$(ls "$s")" ] && mkdir -p "$RESULTS_DIR/orca/canon-$(basename "$(dirname "$s")")" &&
			cp "$s"/*.diff "$RESULTS_DIR/orca/canon-$(basename "$(dirname "$s")")/"
	done
	for d in "$W"/alone "$W"/vexec* "$W"/refuse*; do
		[ -f "$d/log" ] && cp "$d/log" "$RESULTS_DIR/orca/$(basename "$d").log"
	done
fi
echo
echo "orca: $oks checks passed; $([ $fail -eq 0 ] && echo "every section passed" || echo "FAILED")"
exit $fail
