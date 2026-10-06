#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The port's own suites with vexec.mode = force, against vexec.mode = off
# (pg_vector_executor.md §6.1, §5 V1): the singlenode suite and the
# greenplum suite, each in its planner pass and its ORCA pass, run in the
# port's image as the port runs them, once a session:
#
#   off        vexec preloaded, off: the reference
#   off-again  the reference once more: what differs between two runs of
#              the same settings is the suites' own noise
#   postgres   vexec.mode = force, the PostgreSQL format
#   arrow      vexec.mode = force, the Arrow format
#   random     vexec.mode = force, the per-structure settings of §3.4.4
#              drawn at random each time a node reads them, from a seed
#              the run prints (vexec.debug_layout_seed)
#
# Every server a suite starts has vexec after its own preload list and the
# session's settings, as the full run's wrapper gives them (fullrun.sh):
# the real binary keeps its name in a directory of its own, and runs with
# the wrapper's name, so that gpMgmt finds it.  Each run's results are
# compared with the reference's by the differential runner's comparison
# (differential.py): rows, plans dropped
# and unordered results sorted, and the SQLSTATEs of each test's errors from
# the servers' logs.  The greenplum suite's results are first read as
# Cloudberry's gpdiff.pl reads them, through atmsort.pm with Cloudberry's and
# the port's init files: a cluster's tests answer alike in other words from
# run to run -- which segment raised an error, a socket's path -- and their
# files say where, as Cloudberry's own comparison needs.  A difference kept
# on purpose is a file of differential/kept/<suite>-<pass>/<session>/.  The suites' own verdicts,
# against Cloudberry's expected output, are not the measure here: a vector
# plan prints otherwise.
#
# A suite keeps its work directory (KEEP=1) until its results and logs are
# read; then the session's clusters are deleted, and their servers' logs and
# the suite's diffs stay in logs/ of its work directory.  A greenplum
# session's databases take ~10 GB and a whole run's ~117 GB; kept runs filled
# the host's disk on 2026-10-06.
#
#   VEXEC_PORT_SUITES    "singlenode greenplum"
#   VEXEC_PORT_PASSES    "planner orca"
#   VEXEC_PORT_SESSIONS  "off off-again postgres arrow random"
#   VEXEC_PORT_KEEP      set: each session's clusters kept, databases and all
#   VEXEC_SEED           the random session's seed: drawn and printed
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
BINDIR="$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")"
OUT="${RESULTS_DIR:-/work}/portsuites"
CMP="$OUT/compare"
mkdir -p "$OUT" "$CMP"

# The wrapper: vexec after the server's preload list, and the session's
# settings, from a file it reads each time a server starts.
WRAPENV="$HOME/vexec-wrap.env"
if [ ! -f "$BINDIR/vexec-real/postgres" ]; then
	mkdir -p "$BINDIR/vexec-real"
	mv "$BINDIR/postgres" "$BINDIR/vexec-real/postgres"
	cat > "$BINDIR/postgres" <<WRAPPER
#!/bin/bash
# vexec's suite wrapper (pg_accel test/vexec/portsuites.sh): this server with
# vexec appended to its shared_preload_libraries and the session's settings.
real="\$(dirname "\$0")/vexec-real/postgres"
for a in "\$@"; do
	case "\$a" in
		--boot|--single|-C|--describe-config|-V|--version|--check|--help|'-?')
			exec -a "\$0" "\$real" "\$@" ;;
	esac
done
. "$WRAPENV"
preload="\$(exec -a "\$0" "\$real" "\$@" -C shared_preload_libraries 2> /dev/null)"
case ",\${preload// /}," in
	*,vexec,*) ;;
	*) preload="\${preload:+\$preload,}vexec" ;;
esac
exec -a "\$0" "\$real" "\$@" -c "shared_preload_libraries=\$preload" \\
	-c "vexec.mode=\$VEXEC_WRAP_MODE" -c "vexec.batch_format=\$VEXEC_WRAP_FORMAT" \\
	-c "vexec.debug_layout_seed=\${VEXEC_WRAP_SEED:-0}" -c "log_line_prefix=%a|%e|"
WRAPPER
	chmod 755 "$BINDIR/postgres"
fi

SEED="${VEXEC_SEED:-$(( (RANDOM << 15 | RANDOM) % 2147483646 + 1 ))}"
echo "  random session's seed: $SEED (VEXEC_SEED=$SEED reruns it)"
session_env() {
	case "$1" in
		off|off-again) echo "VEXEC_WRAP_MODE=off VEXEC_WRAP_FORMAT=postgres VEXEC_WRAP_SEED=0" ;;
		postgres) echo "VEXEC_WRAP_MODE=force VEXEC_WRAP_FORMAT=postgres VEXEC_WRAP_SEED=0" ;;
		arrow) echo "VEXEC_WRAP_MODE=force VEXEC_WRAP_FORMAT=arrow VEXEC_WRAP_SEED=0" ;;
		random) echo "VEXEC_WRAP_MODE=force VEXEC_WRAP_FORMAT=postgres VEXEC_WRAP_SEED=$SEED" ;;
	esac
}

fail=0
for suite in ${VEXEC_PORT_SUITES:-singlenode greenplum}; do
	for pass in ${VEXEC_PORT_PASSES:-planner orca}; do
		sessions=""
		for s in ${VEXEC_PORT_SESSIONS:-off off-again postgres arrow random}; do
			work="$OUT/$suite-$pass-$s"
			rm -rf "$work"
			mkdir -p "$work"
			echo "export $(session_env "$s")" > "$WRAPENV"
			start=$(date +%s)
			KEEP=1 TMPDIR="$work" PASSES="$pass" /cb/pg19/test/"$suite"/run.sh > "$work/output" 2>&1
			rc=$?
			echo "  $suite $pass, $s: the suite's own verdict $rc ($(( $(date +%s) - start )) s)"

			# its results and its servers' logs, as the comparison reads them:
			# the session's work directory, which some tests print, as one
			# name; the greenplum suite's results as Cloudberry's gpdiff.pl
			# reads them before it compares (atmsort_canon.pl, through the
			# suite's own copy of atmsort.pm), less the lines gpdiff ignores,
			# and its coordinators' logs alone, where a test's errors are as
			# it sees them, whichever segment raised them
			dest="$CMP/$suite-$pass/$s"
			rm -rf "$dest"
			mkdir -p "$dest/results"
			gpdiff=""
			if [ "$suite" = greenplum ]; then
				gpdiff="$(find "$work" -maxdepth 2 -type d -name gpdiff | head -1)"
				[ -n "$gpdiff" ] || { echo "    no gpdiff/ in $work: nothing compared"; fail=1; continue; }
			fi
			find "$work" -path "*/$pass/results/*.out" | while read -r r; do
				sed -E "s#$work/cb-$suite-[A-Za-z0-9]+#<work>#g" "$r" |
				if [ -n "$gpdiff" ]; then
					perl "$here/atmsort_canon.pl" "$gpdiff" --gpd_init /cb/src/test/regress/init_file \
						--gpd_init /cb/pg19/test/greenplum/init_file | grep -v -E '^(GP_IGNORE:|HINT:|CONTEXT:)'
				else
					cat
				fi > "$dest/results/$(basename "$r")"
			done
			if [ -n "$gpdiff" ]; then
				find "$work" -name 'node0.log'
			else
				find "$work" -name '*.log' -o -name 'log'
			fi | while read -r l; do
				cat "$l"
			done > "$dest/postmaster.log" 2> /dev/null
			echo "    $(ls "$dest/results" | wc -l) results kept"

			# the session's clusters, once read: their servers' logs and the
			# suite's diffs kept in logs/, the rest deleted
			if [ -z "${VEXEC_PORT_KEEP:-}" ]; then
				for c in "$work"/cb-$suite-*; do
					[ -d "$c" ] || continue
					mkdir -p "$work/logs"
					(cd "$work" && find "${c##*/}" -type f \( -name '*.log' -o -name log -o -name '*.diffs' -o -path '*/log/*' \) \
						-exec cp --parents -t logs {} +)
					rm -rf "$c"
				done
			fi
			sessions="$sessions $s"
		done
		python3 "$here/differential.py" "$CMP/$suite-$pass" $sessions \
			--kept "$here/differential/kept/$suite-$pass" || fail=1
	done
done
echo "portsuites: $([ $fail -eq 0 ] && echo passed || echo FAILED)"
exit $fail
