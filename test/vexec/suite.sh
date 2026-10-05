#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# vexec's own regression suite (modules/vexec/sql, expected), against a
# server of the container's PostgreSQL with vexec preloaded: built, a
# server made, the suite run by PGXS's installcheck, and the server's log
# and the diffs kept in RESULTS_DIR when it is set.
#
#   VEXEC_PRELOAD   the server's shared_preload_libraries: vexec
#   VEXEC_SETTINGS  more settings for it, one a line
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
. "$here/lib.sh"

D="$(mktemp -d "${TMPDIR:-/tmp}/vexec-suite-XXXXXX")"
settings=("shared_preload_libraries = '${VEXEC_PRELOAD:-vexec}'")
while IFS= read -r line; do [ -n "$line" ] && settings+=("$line"); done <<< "${VEXEC_SETTINGS:-}"
server_init "$D" "${settings[@]}" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }

BUILD="${VEXEC_BUILD:-/tmp/vexec-build}"
PGHOST="$D/sock" PGUSER=postgres make -C "$BUILD/modules/vexec" -s \
	PG_CONFIG="$BINDIR/pg_config" installcheck
rc=$?
if [ -n "${RESULTS_DIR:-}" ]; then
	cp -r "$BUILD/modules/vexec/results" "$RESULTS_DIR/" 2> /dev/null
	cp "$BUILD/modules/vexec/regression.diffs" "$RESULTS_DIR/" 2> /dev/null
	cp "$D/log" "$RESULTS_DIR/server.log"
fi
[ $rc -ne 0 ] && [ -f "$BUILD/modules/vexec/regression.diffs" ] && head -200 "$BUILD/modules/vexec/regression.diffs"
server_stop "$D"
exit $rc
