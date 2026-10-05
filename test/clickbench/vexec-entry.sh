#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# A load's container start when it runs with vexec (CB_VEXEC, phase VH of
# pg_vector_executor.md §5, from V1 on): as root, the server's installation
# is given vexec, which run.sh built by PGXS in vexec's own images -- whose
# servers are this image's builds -- into the cache, and on the port's route
# the port's modules of V1's worktree, which test/vexec/run.sh portbuild
# staged; then bench.sh runs as postgres, as it does without vexec.  The
# installation is the container's own, gone with it.
#
# Check mode only: vexec's builds are against the servers built with
# assertions.
set -eu
die() { echo "vexec-entry.sh: $*" >&2; exit 1; }

[ "${CB_MODE:-}" = check ] || die "vexec runs in check mode only, against the servers built with assertions"
case "${CB_ROUTE:-}" in
	vanilla) prefix=/pg/vanilla; leg=vanilla ;;
	port) prefix=/usr/local/pgsql; leg=port ;;
	*) die "CB_ROUTE is vanilla or port" ;;
esac
src="/cache/vexec/$leg"
[ -f "$src/lib/vexec.so" ] || die "no $src/lib/vexec.so: run.sh builds it when CB_VEXEC is set"

if [ "$leg" = port ]; then
	[ -d "$src/stage/usr/local/pgsql" ] || die "no port stage in $src/stage: test/vexec/run.sh portbuild makes it"
	cp -a "$src/stage/usr/local/pgsql/." "$prefix/"
	echo "  the port's modules from V1's stage: $(cat "$src/stage/COMMIT" 2> /dev/null || echo '?')"
fi
cp "$src/lib/vexec.so" "$("$prefix/bin/pg_config" --pkglibdir)/"
cp "$src"/extension/vexec* "$("$prefix/bin/pg_config" --sharedir)/extension/"
echo "  vexec, built against $(cat "$src/BUILT" 2> /dev/null || echo '?')"

export HOME=/home/postgres
exec setpriv --reuid=postgres --regid=postgres --init-groups /clickbench/bench.sh
