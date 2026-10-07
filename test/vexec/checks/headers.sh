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
#     the port's storage modules compile against in pg19/include (V1), and
#     its batch-sink contract, include/vexec_sink.h, the same way (VI).
#
# A change to one fails here until its copy is refreshed in the same commit.
# On the host.  The port's sources to compare with are VEXEC_PORT_SRC, a
# phase's worktree of the port, where it is, else the build's checkout of
# the port, CB_SRC (~/.cache/pg_accel/cloudberry, test/vexec/checkouts.sh).
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$here/../../.." && pwd)"
CB_SRC="${CB_SRC:-$HOME/.cache/pg_accel/cloudberry}"
PORT_SRC="${VEXEC_PORT_SRC:-$CB_SRC}"
if [ -d "$PORT_SRC/pg19/include" ]; then
	CB_SRC="$PORT_SRC"
fi
COPIES="$ROOT/modules/vexec/pgxs/include"

if [ ! -d "$CB_SRC/pg19/include" ]; then
	echo "  skipped: no port checkout at $CB_SRC (test/vexec/run.sh cloudberry)"
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
for contract in vexec_source.h vexec_sink.h; do
	[ -f "$CB_SRC/pg19/include/$contract" ] || continue
	if cmp -s "$ROOT/include/$contract" "$CB_SRC/pg19/include/$contract"; then
		echo "  ok $contract, the port's copy"
	else
		echo "  DIFFERS $contract: refresh the port's pg19/include copy from include/$contract"
		fail=1
	fi
done
exit $fail
