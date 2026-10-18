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
#     its batch-sink contract, include/vexec_sink.h, the same way (VI);
#   - pg_otel_probe's API, pg_otel_probe_api.h, whose copy in the port's
#     pg19/include vexec's (in pgxs/include, above) and vexec_flight's
#     (modules/vexec_flight/src) equal;
#   - the two contracts' copies in ch_table_engines' include/vexec, which
#     its source and sink compile against (ch_table_engines.md §3, VC10),
#     where its tree is: CHTE_SRC, pg_ch_table_engines beside this checkout.
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

CHTE_SRC="${CHTE_SRC:-$(cd "$ROOT/.." && pwd)/pg_ch_table_engines}"
fail=0

# ch_table_engines' copies of the contracts
if [ -d "$CHTE_SRC/include/vexec" ]; then
	echo "headers: ch_table_engines' copies at $CHTE_SRC ($(git -C "$CHTE_SRC" rev-parse --short HEAD 2>/dev/null))"
	for contract in vexec_source.h vexec_sink.h; do
		if cmp -s "$ROOT/include/$contract" "$CHTE_SRC/include/vexec/$contract"; then
			echo "  ok $contract, ch_table_engines' copy"
		else
			echo "  DIFFERS $contract: refresh ch_table_engines' include/vexec copy from include/$contract"
			fail=1
		fi
	done
fi

if [ ! -d "$CB_SRC/pg19/include" ]; then
	echo "  the port's: skipped, no port checkout at $CB_SRC (test/vexec/run.sh cloudberry)"
	[ $fail = 0 ] && [ ! -d "$CHTE_SRC/include/vexec" ] && exit 77
	exit $fail
fi
echo "headers: the copies against $CB_SRC at $(git -C "$CB_SRC" rev-parse --short HEAD 2>/dev/null)"
for copy in "$COPIES"/*.h; do
	name="$(basename "$copy")"
	if cmp -s "$copy" "$CB_SRC/pg19/include/$name"; then
		echo "  ok $name"
	else
		echo "  DIFFERS $name: refresh it from pg19/include/$name"
		fail=1
	fi
done
if [ -f "$CB_SRC/pg19/include/pg_otel_probe_api.h" ]; then
	if cmp -s "$ROOT/modules/vexec_flight/src/pg_otel_probe_api.h" "$CB_SRC/pg19/include/pg_otel_probe_api.h"; then
		echo "  ok pg_otel_probe_api.h, vexec_flight's copy"
	else
		echo "  DIFFERS pg_otel_probe_api.h: refresh modules/vexec_flight/src's copy from pg19/include/pg_otel_probe_api.h"
		fail=1
	fi
fi
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
