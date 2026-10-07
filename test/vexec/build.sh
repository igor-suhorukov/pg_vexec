#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Build vexec, its tests' modules and the extensions beside it -- the kernel
# packs, and vexec_flight where its dependencies are -- by PGXS against the
# server of the container this runs in, and install them into it
# (pg_vector_executor.md §3.2).  The tree is read where it is mounted and
# built in a copy, so that nothing is written into it.  Warnings are errors.
#
#   VEXEC_SRC     the tree: /src
#   VEXEC_BUILD   where it is built: /tmp/vexec-build
#   PG_CONFIG     the server's: /usr/local/pgsql/bin/pg_config
#   MAKE_JOBS     1
#   VEXEC_SKIP_MODULES  the modules left out: a leg of a pack or of
#                 vexec_flight builds its module from its own mount
set -eu

SRC="${VEXEC_SRC:-/src}"
BUILD="${VEXEC_BUILD:-/tmp/vexec-build}"
PG_CONFIG="${PG_CONFIG:-/usr/local/pgsql/bin/pg_config}"

rm -rf "$BUILD"
mkdir -p "$BUILD"
cp -a "$SRC/include" "$SRC/modules" "$BUILD/"

# The port's modules built from V1's worktree (portbuild.sh), when a leg is
# given their stage: installed over the image's own before vexec is built --
# on the port's image alone, the vanilla leg's PostgreSQL staying as it is.
if [ -d /cbstage/usr/local/pgsql ] && [ -d /cb/pg19 ] && [ ! -f /tmp/.vexec-port-staged ]; then
	cp -a /cbstage/usr/local/pgsql/. "$(dirname "$("$PG_CONFIG" --bindir)")/"
	touch /tmp/.vexec-port-staged
	echo "the port's modules from the stage: $(cat /cbstage/COMMIT 2> /dev/null || echo '?')"
fi

# gp_orca built alone, for one node without gp_core (orcabuild.sh, V6), when
# a leg is given its stage: installed over the vanilla image's server.
if [ -d /orcastage/usr/local/pgsql ] && [ ! -d /cb/pg19 ] && [ ! -f /tmp/.vexec-orca-staged ]; then
	cp -a /orcastage/usr/local/pgsql/. "$(dirname "$("$PG_CONFIG" --bindir)")/"
	touch /tmp/.vexec-orca-staged
	echo "gp_orca built alone, from the stage: $(cat /orcastage/COMMIT 2> /dev/null || echo '?')"
fi

echo "building against $("$PG_CONFIG" --version) at $(cat "$("$PG_CONFIG" --bindir)/../.pg_ref_commit" 2>/dev/null || echo '?')"
# vexec, the tests' modules, and the extensions beside vexec, against
# include/ here: the kernel packs on every image, vexec_flight where its
# dependencies are -- its own dev images (modules/vexec_flight/docker).
modules="vexec vexec_test vexec_testpack vexec_pgvector vexec_postgis"
command -v protoc-c > /dev/null && modules="$modules vexec_flight"
for m in $modules; do
	[ -f "$BUILD/modules/$m/Makefile" ] || continue
	case " ${VEXEC_SKIP_MODULES:-} " in *" $m "*) continue ;; esac
	make -s -C "$BUILD/modules/$m" PG_CONFIG="$PG_CONFIG" VEXEC_INCLUDE="$BUILD/include" COPT=-Werror -j"${MAKE_JOBS:-1}"
	make -s -C "$BUILD/modules/$m" PG_CONFIG="$PG_CONFIG" VEXEC_INCLUDE="$BUILD/include" install > /dev/null
	echo "  $m built and installed"
done
