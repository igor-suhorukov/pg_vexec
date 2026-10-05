#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The headers vexec shares with the port, kept equal byte for byte
# (pg_vector_executor.md §3.2, "Kept equal"):
#
#   - the port's headers vexec's PGXS build compiles against, copies in
#     modules/vexec/pgxs/include of the originals in the port's pg19/include:
#     cb_module.h, cb_explain.h (V0, V1), and gp_orca_vec.h, gp_orca's API,
#     which V1 adds to the port;
#   - vexec's own batch-source contract, include/vexec_source.h, whose copy
#     the port's storage modules compile against in pg19/include (V1).
#
# A change to one fails here until its copy is refreshed in the same commit.
# On the host.  The port's checkout to compare with is V1's worktree,
# VEXEC_PORT_SRC (../cloudberry-vexec/wt), where it is, else CB_SRC
# (../cloudberry).
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$here/../../.." && pwd)"
PORT_SRC="${VEXEC_PORT_SRC:-$ROOT/../cloudberry-vexec/wt}"
if [ -d "$PORT_SRC/pg19/include" ]; then
	CB_SRC="$PORT_SRC"
else
	CB_SRC="${CB_SRC:-$ROOT/../cloudberry}"
fi
COPIES="$ROOT/modules/vexec/pgxs/include"

if [ ! -d "$CB_SRC/pg19/include" ]; then
	echo "  skipped: no port checkout at $CB_SRC (set CB_SRC)"
	exit 77
fi
echo "headers: the copies against $CB_SRC at $(git -C "$CB_SRC" rev-parse --short HEAD 2>/dev/null)"
fail=0
for copy in "$COPIES"/*.h; do
	name="$(basename "$copy")"
	if cmp -s "$copy" "$CB_SRC/pg19/include/$name"; then
		echo "  ok $name"
	else
		echo "  DIFFERS $name: refresh it from pg19/include/$name"
		fail=1
	fi
done
if [ -f "$CB_SRC/pg19/include/vexec_source.h" ]; then
	if cmp -s "$ROOT/include/vexec_source.h" "$CB_SRC/pg19/include/vexec_source.h"; then
		echo "  ok vexec_source.h, the port's copy"
	else
		echo "  DIFFERS vexec_source.h: refresh the port's pg19/include copy from include/vexec_source.h"
		fail=1
	fi
fi
exit $fail
