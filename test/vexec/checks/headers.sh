#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The port's headers vexec's PGXS build compiles against are copies, in
# modules/vexec/pgxs/include, of the originals in the port's pg19/include
# (pg_vector_executor.md §3.2, "Kept equal"): each must equal its original
# byte for byte, so that a change to one fails here until the copy is
# refreshed in the same commit.  On the host; CB_SRC is the port's checkout.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$here/../../.." && pwd)"
CB_SRC="${CB_SRC:-$ROOT/../cloudberry}"
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
exit $fail
