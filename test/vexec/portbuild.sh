#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The port's modules, built from its sources -- the build's checkout of the
# port, or a phase's worktree of it (pg_vector_executor.md §5 V1): the
# entrypoint of the portbuild service (docker/vexec.yml), in the
# portdev image, as postgres.  V1 changes the port's modules on a branch of
# its own -- PAX's and gp_ao's batch readers, gp_orca's API, gp_core's list of
# settings -- and the legs run them before an image of that branch exists.
#
#   /cbsrc       the sources, read-only, with PAX's two submodules mounted
#                over their directories
#   /cbbuild     the build directory and the stage, kept between runs, so that
#                ninja builds only what changed
#
# It configures once, builds, and stages an install of every module into
# /cbbuild/stage, which a leg copies over its image's /usr/local/pgsql
# (build.sh, VEXEC_PORT_STAGE).  /cbbuild/stage/COMMIT names the sources'
# commit and whether they had changes.
#
#   PORTBUILD_JOBS   ninja's jobs: 8, beside other sessions' tests
set -eu

SRC=/cbsrc
BUILD=/cbbuild/build
STAGE=/cbbuild/stage
PG_CONFIG=/usr/local/pgsql/bin/pg_config

[ -f "$SRC/pg19/meson.build" ] || { echo "portbuild: no sources of the port at $SRC"; exit 1; }
[ -f "$SRC/contrib/pax_storage/src/cpp/contrib/tabulate/CMakeLists.txt" ] \
	|| { echo "portbuild: PAX's submodules are not mounted"; exit 1; }

if [ ! -f "$BUILD/build.ninja" ]; then
	echo "portbuild: configuring $BUILD from $SRC/pg19"
	meson setup "$BUILD" "$SRC/pg19" -Dpg_config="$PG_CONFIG" --prefix=/usr/local/pgsql > /cbbuild/setup.log 2>&1 \
		|| { tail -40 /cbbuild/setup.log; exit 1; }
fi
start=$(date +%s)
ninja -C "$BUILD" -j"${PORTBUILD_JOBS:-8}" > /cbbuild/ninja.log 2>&1 \
	|| { grep -E -B2 -A12 'error|FAILED' /cbbuild/ninja.log | head -120; exit 1; }
echo "portbuild: built in $(( $(date +%s) - start )) s ($(grep -c '^\[' /cbbuild/ninja.log) steps)"
rm -rf "$STAGE"
DESTDIR="$STAGE" ninja -C "$BUILD" install > /cbbuild/install.log 2>&1 \
	|| { tail -40 /cbbuild/install.log; exit 1; }
echo "${PORTBUILD_COMMIT:-unknown}" > "$STAGE/COMMIT"
echo "portbuild: staged in $STAGE ($(cat "$STAGE/COMMIT"))"
