#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# vexec's states (pg_vector_executor.md §1.2, §4.3), each checked on the
# container's server:
#
#   2  installed, not preloaded: LOAD and CREATE EXTENSION fail with the
#      preload error before anything is registered; a vexec.* setting in a
#      session is a placeholder, not an error
#   3  vexec's objects exist, the library not preloaded: a call of one of
#      them is an error, never a silent misread
#   4  the library removed from disk: as 3, and the server stays up
#
# State 1, a server without vexec, is PostgreSQL's own suite on it
# (pgregress.sh's "installed" mode, which also covers 2 for the suite).
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" > /dev/null || exit 1
. "$here/lib.sh"

D="$(mktemp -d "${TMPDIR:-/tmp}/vexec-states-XXXXXX")"
fail=0
expect() {					# expect <what> <output> <pattern>
	if echo "$2" | grep -q -- "$3"; then
		echo "  ok $1"
	else
		echo "  FAILED $1: [$2]"
		fail=1
	fi
}
restart() {					# restart <shared_preload_libraries>
	server_stop "$D"
	sed -i '/^shared_preload_libraries/d' "$D/data/postgresql.conf"
	echo "shared_preload_libraries = '$1'" >> "$D/data/postgresql.conf"
	server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }
}

echo "vexec's states (§1.2)"
server_init "$D" "shared_preload_libraries = ''" || exit 1
server_start "$D" || { echo "the server did not start"; tail -20 "$D/log"; exit 1; }

# state 2
expect "state 2: LOAD fails with the preload error" "$(q "$D" postgres "LOAD 'vexec'")" \
	'vexec can only be loaded through "shared_preload_libraries"'
expect "state 2: CREATE EXTENSION fails with the preload error" "$(q "$D" postgres "CREATE EXTENSION vexec")" \
	'vexec can only be loaded through "shared_preload_libraries"'
expect "state 2: nothing of the extension was made" \
	"$(q "$D" postgres "SELECT count(*) FROM pg_extension WHERE extname = 'vexec'")" '^0$'
expect "state 2: a vexec.* setting is a placeholder" \
	"$(q "$D" postgres "SET vexec.mode = force; SHOW vexec.mode")" '^force$'

# state 3
restart vexec
expect "preloaded: the extension is made" "$(q "$D" postgres "CREATE EXTENSION vexec; SELECT (vexec.type_layouts('int4')).postgres")" \
	'^fixed(4)$'
restart ""
expect "state 3: a call of vexec's function is the preload error" \
	"$(q "$D" postgres "SELECT (vexec.type_layouts('int4')).postgres")" \
	'vexec can only be loaded through "shared_preload_libraries"'
expect "state 3: plans are PostgreSQL's" "$(q "$D" postgres "EXPLAIN (COSTS OFF) SELECT 1")" '^Result$'

# state 4
LIB="$("$BINDIR/pg_config" --pkglibdir)/vexec.so"
mv "$LIB" "$LIB.away"
expect "state 4: a call of vexec's function is an error" \
	"$(q "$D" postgres "SELECT (vexec.type_layouts('int4')).postgres")" 'could not access file'
expect "state 4: the server stays up" "$(q "$D" postgres "SELECT 1")" '^1$'
mv "$LIB.away" "$LIB"

server_stop "$D"
rm -rf "$D"
echo "states: $([ $fail -eq 0 ] && echo passed || echo FAILED)"
exit $fail
