#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The port's full run with vexec in every node's preload list
# (pg_vector_executor.md §5 V0, §6.4 check 4): the entrypoint of the
# fullrun service (docker/vexec.yml), as root in a container of the port's
# image, as the port's own tests service runs it (pg19/docker/compose.yml).
#
# With VEXEC_FULLRUN_PRELOAD=1 every server any suite starts has vexec
# appended to its shared_preload_libraries, in off mode: the suites make
# their own servers with their own lists (pg19/test/*/run.sh), so the
# server's binary is wrapped (below) rather than any suite changed.  With 0
# the same image runs the port's suites as they are, the run this one is
# compared with: "unchanged" is every job's outcome the same in both.
#
# The wrapper.  pg_ctl starts $bindir/postgres; it becomes a script that
# asks the real binary what shared_preload_libraries would be, from the
# data directory's files and the command line (postgres -C), and starts it
# with vexec after the rest.  initdb's bootstrap and single-user runs, -C,
# --describe-config, -V and --check go to the real binary untouched.
#
# The server must look as it does without the wrapper: gpMgmt finds a
# running one by "postgres -D <datadir>" in ps's output
# (gpMgmt/bin/gppylib/commands/gp.py:65, 76), and a binary renamed
# postgres.vexec-real in bin/ broke gpstop, gpstart and gpactivatestandby
# with a standby.  So the real binary keeps its name in a directory of its
# own, and runs as $bindir/postgres (exec -a): its process name, its command
# line and the path it finds its installation from (my_exec_path, which
# postmaster.opts keeps for pg_ctl restart) are the unwrapped server's.
#
#   VEXEC_FULLRUN_PRELOAD   1, or 0 for the run to compare with
#   the arguments           pg19/test/run.sh's: suites, or all of them
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINDIR=/usr/local/pgsql/bin

su postgres -c "VEXEC_BUILD=/home/postgres/vexec-build $here/build.sh" || exit 1

if [ "${VEXEC_FULLRUN_PRELOAD:-1}" = 1 ]; then
	if [ ! -f "$BINDIR/vexec-real/postgres" ]; then
		mkdir -p "$BINDIR/vexec-real"
		mv "$BINDIR/postgres" "$BINDIR/vexec-real/postgres"
		cat > "$BINDIR/postgres" <<'WRAPPER'
#!/bin/bash
# vexec's full-run wrapper (pg_accel test/vexec/fullrun.sh): this server with
# vexec appended to its shared_preload_libraries, run as this file's name.
real="$(dirname "$0")/vexec-real/postgres"
for a in "$@"; do
	case "$a" in
		--boot|--single|-C|--describe-config|-V|--version|--check|--help|'-?')
			exec -a "$0" "$real" "$@" ;;
	esac
done
preload="$(exec -a "$0" "$real" "$@" -C shared_preload_libraries 2> /dev/null)"
case ",${preload// /}," in
	*,vexec,*) exec -a "$0" "$real" "$@" ;;
esac
exec -a "$0" "$real" "$@" -c "shared_preload_libraries=${preload:+$preload,}vexec"
WRAPPER
		chmod 755 "$BINDIR/postgres"
		chown postgres "$BINDIR/postgres"
	fi
	echo "fullrun: every server starts with vexec appended to its shared_preload_libraries"
else
	echo "fullrun: the port's suites as they are, for comparison"
fi
mkdir -p "${RESULTS_DIR:-/work/fullrun}" && chown postgres "${RESULTS_DIR:-/work/fullrun}"
export RESULTS_DIR="${RESULTS_DIR:-/work/fullrun}"
exec /cb/pg19/test/cgroup.sh "$@"
