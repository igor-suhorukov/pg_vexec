#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# vexec's Arrow IPC codec against Arrow's own implementation, pyarrow
# (pg_vector_executor.md §5 V7_0), in a leg's container with pyarrow
# (docker/Dockerfile.arrow): built, a server made with vexec preloaded,
# vexec_test and the layouts' corpus made, and ipc_check.py run against it --
# pyarrow reading vexec's streams, vexec reading pyarrow's, and malformed
# streams through vexec's reader.  The server must still be up after it, and
# its log show no crash.  The server's log is kept in RESULTS_DIR when it is
# set.
#
#   VEXEC_IPC_FUZZ   the malformed streams: 6000
#   VEXEC_IPC_SEED   their random seed: 20261006
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
. "$here/lib.sh"
python3 -c 'import pyarrow' 2> /dev/null || { echo "no pyarrow here: run.sh ipc runs this in a leg's -arrow container"; exit 1; }

D="$(mktemp -d "${TMPDIR:-/tmp}/vexec-ipc-XXXXXX")"
server_init "$D" "shared_preload_libraries = 'vexec'" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
"$BINDIR/psql" -X -q -h "$D/sock" -U postgres -d postgres -v ON_ERROR_STOP=1 \
	-f "$here/../../modules/vexec/sql/vexec_corpus_setup.sql" > "$D/corpus.log" 2>&1 \
	|| { echo "the corpus was not made"; cat "$D/corpus.log"; exit 1; }

python3 "$here/ipc_check.py" --host "$D/sock" --db postgres --psql "$BINDIR/psql" --work "$D/work" \
	--fuzz "${VEXEC_IPC_FUZZ:-6000}" --seed "${VEXEC_IPC_SEED:-20261006}"
rc=$?
if [ "$(q "$D" postgres "SELECT 1")" != 1 ]; then
	echo "FAIL: the server is not up after the checks"
	rc=1
fi
if grep -n -E "terminated by signal|PANIC|TRAP:" "$D/log"; then
	echo "FAIL: the server's log shows a crash"
	rc=1
fi
[ -n "${RESULTS_DIR:-}" ] && cp "$D/log" "$RESULTS_DIR/server.log"
server_stop "$D"
echo "ipc: $([ $rc -eq 0 ] && echo "every check passed" || echo FAILED)"
exit $rc
