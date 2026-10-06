#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The differential runner (pg_vector_executor.md §6.1): each corpus run in
# four sessions, and every session's rows and SQLSTATEs compared with the
# first's, PostgreSQL's own executor's:
#
#   off       vexec.mode = off
#   postgres  vexec.mode = force, vexec.batch_format = postgres
#   arrow     vexec.mode = force, vexec.batch_format = arrow
#   random    vexec.mode = force, the per-structure settings of §3.4.4 drawn
#             at random each time a node reads them, from a seed the run
#             prints (vexec.debug_layout_seed; VEXEC_SEED to give one)
#
# and, for the kernel packs (§3.17, VK), postgres-packs and arrow-packs:
# postgres's and arrow's settings in an instance with the packs VEXEC_PACKS
# names preloaded before vexec, so that their declared calls run a batch's
# rows at a time where the other sessions run them row by row.
#
# The corpora:
#
#   pg        PostgreSQL's own parallel_schedule, from the commit the server
#             was built from, less the tests listed in differential/excluded
#             with their reasons; the off session must also pass against its
#             unchanged expected output
#   vexec     vexec's own tests that every session must answer alike
#             (differential/vexec_schedule)
#   pgvector  pgvector's own regression tests, where the image carries them
#             (the port's, in /pgvector/test, of the pgvector it installs;
#             VEXEC_PGVECTOR_TESTS), every one, with the extension loaded
#
# Each session runs in a temporary instance pg_regress makes, with vexec
# preloaded and the session's settings in PGOPTIONS, so that every
# connection a test makes has them; its databases are UTF8, in the C locale,
# as the other suites' are.  The comparison is differential.py's;
# a difference a corpus keeps on purpose is a file in
# differential/kept/<corpus>/<session>/<test>.diff.  In V0 no vector node
# exists, so every session must answer as the first: the runner is ready for
# V1, whose nodes it then checks.
#
#   VEXEC_CORPORA     "pg vexec"
#   VEXEC_SESSIONS    "off postgres arrow random"
#   VEXEC_SEED        the random session's seed: drawn and printed
#   VEXEC_PRELOAD     the instance's shared_preload_libraries: vexec
#   VEXEC_PACKS       the packs the -packs sessions preload, a comma-separated
#                     list: none
#   VEXEC_PG_MAXCONN  pg_regress's --max-connections: 0
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
. "$here/lib.sh"

PGSUITE="${PG_REGRESS_SUITE:-/cb/pgregress}"
PG_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/regress/pg_regress"
BUILD="${VEXEC_BUILD:-/tmp/vexec-build}"
W="$(mktemp -d "${TMPDIR:-/tmp}/vexec-differential-XXXXXX")"
cd "$W" || exit 1
SEED="${VEXEC_SEED:-$(( (RANDOM << 15 | RANDOM) % 2147483646 + 1 ))}"

session_options() {
	case "$1" in
		off) echo "-c vexec.mode=off" ;;
		postgres|postgres-packs) echo "-c vexec.mode=force -c vexec.batch_format=postgres" ;;
		arrow|arrow-packs) echo "-c vexec.mode=force -c vexec.batch_format=arrow" ;;
		random) echo "-c vexec.mode=force -c vexec.batch_format=postgres -c vexec.debug_layout_seed=$SEED" ;;
		*) echo "unknown session $1" >&2; return 1 ;;
	esac
}

# PostgreSQL's schedule, less the excluded tests
grep -v '^#' "$here/differential/excluded" | awk '{print $1}' | sed '/^$/d' > "$W/excluded"
awk -v excluded="$(tr '\n' ' ' < "$W/excluded")" '
	BEGIN { n = split(excluded, e, " "); for (i = 1; i <= n; i++) x[e[i]] }
	/^test:/ { line = "test:"; for (i = 2; i <= NF; i++) if (!($i in x)) line = line " " $i;
			   if (line != "test:") print line; next }
	{ print }' "$PGSUITE/${VEXEC_PG_SCHEDULE:-parallel_schedule}" > "$W/pg_schedule"

# pgvector's tests, every one, a group each
PGVECTOR_TESTS="${VEXEC_PGVECTOR_TESTS:-/pgvector/test}"
if [ -d "$PGVECTOR_TESTS/sql" ]; then
	ls "$PGVECTOR_TESTS/sql" | sed -n 's/\.sql$//p' | sort | sed 's/^/test: /' > "$W/pgvector_schedule"
fi

echo "the differential runner: vexec $(grep -m1 VEXEC_VERSION "$BUILD/modules/vexec/vexec.h" | awk '{print $3}'), PostgreSQL $(cat "$("$BINDIR/pg_config" --bindir)/../.pg_ref_commit" 2>/dev/null)"
echo "  random session's seed: $SEED (VEXEC_SEED=$SEED reruns it)"
[ -n "${VEXEC_PACKS:-}" ] && echo "  the -packs sessions preload $VEXEC_PACKS"
fail=0
for corpus in ${VEXEC_CORPORA:-pg vexec}; do
	extra=()
	rundir=""
	case "$corpus" in
		pg) schedule="$W/pg_schedule"; inputdir="$PGSUITE"; dlpath="$PGSUITE" ;;
		vexec) schedule="$here/differential/vexec_schedule"; inputdir="$BUILD/modules/vexec"; dlpath="$PGSUITE" ;;
		pgvector)
			[ -f "$W/pgvector_schedule" ] || { echo "no pgvector tests at $PGVECTOR_TESTS"; exit 1; }
			schedule="$W/pgvector_schedule"; inputdir="$PGVECTOR_TESTS"; dlpath="$PGSUITE"
			extra=(--load-extension=vector)
			# its tests name files under results/ from where they run, as
			# pgvector's make runs them: from the output directory
			rundir=output
			echo "  pgvector's tests at $(cat "$PGVECTOR_TESTS/../.pgvector_commit" 2> /dev/null || echo '?')" ;;
		*) echo "unknown corpus $corpus"; exit 1 ;;
	esac
	mkdir -p "$W/$corpus"
	echo "  $corpus: $(grep -c '^test:' "$schedule") groups of tests"
	for s in ${VEXEC_SESSIONS:-off postgres arrow random}; do
		conf="$W/$corpus-$s.conf"
		preload="${VEXEC_PRELOAD:-vexec}"
		if [[ "$s" == *-packs ]]; then
			[ -n "${VEXEC_PACKS:-}" ] || { echo "session $s: no VEXEC_PACKS"; exit 1; }
			preload="$VEXEC_PACKS,$preload"
		fi
		{
			echo "shared_preload_libraries = '$preload'"
			echo "log_line_prefix = '%a|%e|'"
			echo "log_min_messages = warning"
		} > "$conf"
		start=$(date +%s)
		mkdir -p "$W/$corpus/$s"
		( cd "$([ "$rundir" = output ] && echo "$W/$corpus/$s" || echo "$inputdir")" &&
		  PGOPTIONS="$(session_options "$s")" "$PG_REGRESS" \
			--temp-instance="$W/$corpus-$s-instance" --temp-config="$conf" \
			--bindir="$BINDIR" --dlpath="$dlpath" --inputdir="$inputdir" --expecteddir="$inputdir" \
			--outputdir="$W/$corpus/$s" --schedule="$schedule" --encoding=UTF8 --no-locale \
			--max-connections="${VEXEC_PG_MAXCONN:-0}" "${extra[@]}" > "$W/$corpus/$s.out" 2>&1 )
		rc=$?
		cp "$W/$corpus/$s/log/postmaster.log" "$W/$corpus/$s/postmaster.log" 2>/dev/null
		summary=$(grep -E "^# (All|[0-9]+ of [0-9]+)" "$W/$corpus/$s.out" | tail -1)
		echo "    $s: ${summary:-no summary} against the expected output ($(( $(date +%s) - start )) s)"
		if [ "$s" = off ] && [ $rc -ne 0 ]; then
			fail=1
			grep -E "^not ok" "$W/$corpus/$s.out" | head -20
		fi
	done
	python3 "$here/differential.py" "$W/$corpus" ${VEXEC_SESSIONS:-off postgres arrow random} \
		--kept "$here/differential/kept/$corpus" || fail=1
	if [ -n "${RESULTS_DIR:-}" ]; then
		mkdir -p "$RESULTS_DIR/differential"
		cp -r "$W/$corpus" "$RESULTS_DIR/differential/" 2>/dev/null
	fi
done
exit $fail
