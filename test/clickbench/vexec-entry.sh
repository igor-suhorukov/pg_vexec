#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# A load's container start when it runs with vexec (CB_VEXEC, phase VH of
# pg_vector_executor.md §5, from V1 on), or on V6's vanilla-orca route: as
# root, the server's installation is given vexec, which run.sh built by PGXS
# in vexec's own images -- whose servers are this image's builds -- into the
# cache; on the port's route the port's modules of the phase's worktree,
# which test/vexec/run.sh portbuild staged; and on the vanilla-orca route
# gp_orca built alone for one node (test/vexec/run.sh orcabuild), whose stage
# run.sh mounts at /orcastage.  Then bench.sh runs as postgres, as it does
# without vexec.  The installation is the container's own, gone with it.
#
# Check mode installs into the servers built with assertions, time mode
# into those without (from V2): vexec's build must be against the same, as
# its ASSERTS says.  The vanilla-orca route's server is its image's own, the
# image run.sh picks by mode, and its vexec is the vanilla route's build.
set -eu
die() { echo "vexec-entry.sh: $*" >&2; exit 1; }

case "${CB_MODE:-}:${CB_ROUTE:-}" in
	check:vanilla) prefix=/pg/vanilla; leg=vanilla ;;
	time:vanilla) prefix=/pg/vanilla-noassert; leg=vanilla ;;
	check:vanilla-orca|time:vanilla-orca) prefix=/usr/local/pgsql; leg=vanilla ;;
	check:port|time:port) prefix=/usr/local/pgsql; leg=port ;;
	*) die "CB_MODE is check or time, CB_ROUTE vanilla, vanilla-orca or port" ;;
esac
if grep -q "^#define USE_ASSERT_CHECKING" "$("$prefix/bin/pg_config" --includedir-server)/pg_config.h"; then
	asserts=yes
else
	asserts=no
fi

if [ "$CB_ROUTE" = vanilla-orca ]; then
	[ -d /orcastage/usr/local/pgsql ] || die "no gp_orca stage at /orcastage: run.sh mounts CB_ORCA_STAGE"
	cp -a /orcastage/usr/local/pgsql/. "$prefix/"
	echo "  gp_orca built alone, from the stage: $(cat /orcastage/COMMIT 2> /dev/null || echo '?')"
fi

if [ -n "${CB_VEXEC:-}" ]; then
	src="/cache/vexec/$leg"
	[ -f "$src/lib/vexec.so" ] || die "no $src/lib/vexec.so: run.sh builds it when CB_VEXEC is set"
	[ "$(cat "$src/ASSERTS" 2> /dev/null)" = "$asserts" ] ||
		die "vexec in $src was built against a server whose assertions are $(cat "$src/ASSERTS" 2> /dev/null || echo unknown), and $prefix's are $asserts"
	if [ "$leg" = port ]; then
		[ -d "$src/stage/usr/local/pgsql" ] || die "no port stage in $src/stage: test/vexec/run.sh portbuild makes it"
		cp -a "$src/stage/usr/local/pgsql/." "$prefix/"
		echo "  the port's modules from the stage: $(cat "$src/stage/COMMIT" 2> /dev/null || echo '?')"
	fi
	cp "$src/lib/vexec.so" "$("$prefix/bin/pg_config" --pkglibdir)/"
	cp "$src"/extension/vexec* "$("$prefix/bin/pg_config" --sharedir)/extension/"
	echo "  vexec, built against $(cat "$src/BUILT" 2> /dev/null || echo '?')"
fi

export HOME=/home/postgres
exec setpriv --reuid=postgres --regid=postgres --init-groups /clickbench/bench.sh
