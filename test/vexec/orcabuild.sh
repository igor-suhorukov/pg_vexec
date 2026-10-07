#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# gp_orca built alone, for one node of a PostgreSQL 19 without gp_core
# (pg_vector_executor.md §3.3.5, V6): the entrypoint of the orcabuild service
# (docker/vexec.yml), in the vanilla-orca image, as postgres.  The port's
# -Dorca_single_node builds gp_orca and nothing else -- ORCA's core,
# unmodified, its translator, and the stand-in that answers what gp_orca asks
# of gp_core on one node -- against the image's vanilla server.
#
#   /cbsrc       the port's sources, read-only: the build's checkout of the
#                port, or a phase's worktree of it
#   /cbbuild     the build directory and the stage, kept between runs, so that
#                ninja builds only what changed
#
# It configures once, builds, and stages an install into /cbbuild/stage, which
# a vanilla-orca leg copies over its image's /usr/local/pgsql (build.sh,
# VEXEC_ORCA_STAGE).  /cbbuild/stage/COMMIT names the sources' commit and
# whether it had changes.
#
#   PORTBUILD_JOBS   ninja's jobs: 8, beside other sessions' tests
set -eu

SRC=/cbsrc
BUILD=/cbbuild/build
STAGE=/cbbuild/stage
PG_CONFIG=/usr/local/pgsql/bin/pg_config

[ -f "$SRC/pg19/meson.build" ] || { echo "orcabuild: no sources of the port at $SRC"; exit 1; }
grep -q "orca_single_node" "$SRC/pg19/meson_options.txt" \
	|| { echo "orcabuild: the port at $SRC has no -Dorca_single_node"; exit 1; }

if [ ! -f "$BUILD/build.ninja" ]; then
	echo "orcabuild: configuring $BUILD from $SRC/pg19, gp_orca alone"
	meson setup "$BUILD" "$SRC/pg19" -Dpg_config="$PG_CONFIG" --prefix=/usr/local/pgsql \
		-Dorca_single_node=true > /cbbuild/setup.log 2>&1 \
		|| { tail -40 /cbbuild/setup.log; exit 1; }
fi
start=$(date +%s)
ninja -C "$BUILD" -j"${PORTBUILD_JOBS:-8}" > /cbbuild/ninja.log 2>&1 \
	|| { grep -E -B2 -A12 'error|FAILED' /cbbuild/ninja.log | head -120; exit 1; }
echo "orcabuild: built in $(( $(date +%s) - start )) s ($(grep -c '^\[' /cbbuild/ninja.log) steps)"
rm -rf "$STAGE"
DESTDIR="$STAGE" ninja -C "$BUILD" install > /cbbuild/install.log 2>&1 \
	|| { tail -40 /cbbuild/install.log; exit 1; }
echo "${PORTBUILD_COMMIT:-unknown}" > "$STAGE/COMMIT"
echo "orcabuild: staged in $STAGE ($(cat "$STAGE/COMMIT")): $(cd "$STAGE" && find . -type f ! -name COMMIT | sort | tr '\n' ' ')"
