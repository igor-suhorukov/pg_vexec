#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build vexec, and its test module, by PGXS against the server of the
# container this runs in, and install them into it (pg_vector_executor.md
# §3.2).  The tree is read where it is mounted and built in a copy, so that
# nothing is written into it.  Warnings are errors.
#
#   VEXEC_SRC     the tree: /src
#   VEXEC_BUILD   where it is built: /tmp/vexec-build
#   PG_CONFIG     the server's: /usr/local/pgsql/bin/pg_config
#   MAKE_JOBS     1
set -eu

SRC="${VEXEC_SRC:-/src}"
BUILD="${VEXEC_BUILD:-/tmp/vexec-build}"
PG_CONFIG="${PG_CONFIG:-/usr/local/pgsql/bin/pg_config}"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cp -a "$SRC/include" "$SRC/modules" "$BUILD/"

echo "building against $("$PG_CONFIG" --version) at $(cat "$("$PG_CONFIG" --bindir)/../.pg_ref_commit" 2>/dev/null || echo '?')"
for m in vexec vexec_test; do
	[ -f "$BUILD/modules/$m/Makefile" ] || continue
	make -s -C "$BUILD/modules/$m" PG_CONFIG="$PG_CONFIG" COPT=-Werror -j"${MAKE_JOBS:-1}"
	make -s -C "$BUILD/modules/$m" PG_CONFIG="$PG_CONFIG" install > /dev/null
	echo "  $m built and installed"
done
