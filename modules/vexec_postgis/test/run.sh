#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# vexec_postgis's legs on the host (pg_vector_executor.md §5, VK), each in
# a container of pg_accel's port image, pg_accel/vexec-dev:port-<CB_COMMIT>,
# which carries PostGIS as the port pins it -- 3.7.0rc2 -- and its build
# tree, with its own regression suite; the results in a run of the cache.
# Nothing is installed on the host; it needs docker.
#
#   run.sh test      the pack's suites on one node, with the pack preloaded
#                    and without it
#   run.sh corpus    PostGIS's own regression suite, its core tests, in
#                    sessions off, force, and force with the pack, each
#                    session's answers compared with off's (test/corpus.sh)
#   run.sh cluster   a coordinator and PACK_SEGMENTS segments of the port
#   run.sh all       the three
#
# Settings, from the environment:
#   VEXEC_SRC         the tree whose vexec the legs build: the pack's own, ../..
#   CB_COMMIT         the port's build: the newest pg_accel/cb-ext image's
#   VEXEC_PORT_STAGE  the port's modules the legs install: the stage
#                     test/vexec/run.sh portbuild makes from the port's
#                     checkout, ~/.cache/pg_accel/vexec/portbuild/stage
#   PACK_SEGMENTS     the cluster's segments: 2
#   PACK_CPUS         each container's CPUs: 4
#   PACK_MEM          each container's memory: 8g
#   PACK_CACHE        the runs: ~/.cache/pg_accel/vexec_postgis
#   PACK_SHARDS       the corpus's servers, side by side: PACK_CPUS
#
# A leg's outcome is its container's exit status, through tee (pipefail).
set -u -o pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$here/.." && pwd)"
VEXEC_SRC="${VEXEC_SRC:-$(cd "$ROOT/../.." && pwd)}"
VEXEC_PORT_STAGE="${VEXEC_PORT_STAGE:-$HOME/.cache/pg_accel/vexec/portbuild/stage}"
PACK_CACHE="${PACK_CACHE:-$HOME/.cache/pg_accel/vexec_postgis}"
if [ -z "${CB_COMMIT:-}" ]; then
	CB_COMMIT="$(docker images pg_accel/cb-ext --format '{{.CreatedAt}} {{.Tag}}' 2> /dev/null | sort -r | awk 'NR == 1 {print $NF}')"
fi

die() { echo "run.sh: $*" >&2; exit 1; }
[ -n "$CB_COMMIT" ] || die "no pg_accel/cb-ext image"
[ -d "$VEXEC_PORT_STAGE/usr/local/pgsql" ] || die "no stage of the port's modules at $VEXEC_PORT_STAGE (test/vexec/run.sh portbuild)"
[ -f "$VEXEC_SRC/include/vexec_kernels.h" ] || die "no vexec with the kernel packs' registry at $VEXEC_SRC"

leg() {						# leg <name>: in a container, its run's output
	local run="$PACK_CACHE/runs/$(date +%Y%m%dT%H%M%S)-$1"
	mkdir -p "$run" || die "cannot make $run"
	echo "== $1: $run"
	echo "  vexec from $VEXEC_SRC at $(git -C "$VEXEC_SRC" rev-parse --short HEAD)$( [ -n "$(git -C "$VEXEC_SRC" status --porcelain 2> /dev/null)" ] && echo '+changes'), the pack at $(git -C "$ROOT" rev-parse --short HEAD 2> /dev/null || echo 'no commit')$( [ -n "$(git -C "$ROOT" status --porcelain -- . 2> /dev/null)" ] && echo '+changes')"
	docker run --rm --network none --cpus "${PACK_CPUS:-4}" -m "${PACK_MEM:-8g}" --memory-swap "${PACK_MEM:-8g}" \
		--shm-size 2g --tmpfs /tmp:exec --user postgres \
		-v "$ROOT:/pack:ro" -v "$VEXEC_SRC:/src:ro" -v "$VEXEC_PORT_STAGE:/cbstage:ro" -v "$run:/work" \
		-e RESULTS_DIR=/work -e VEXEC_SRC=/src -e MAKE_JOBS="${PACK_CPUS:-4}" \
		-e "PACK_SEGMENTS=${PACK_SEGMENTS:-2}" -e "PACK_SHARDS=${PACK_SHARDS:-${PACK_CPUS:-4}}" \
		"pg_accel/vexec-dev:port-$CB_COMMIT" bash /pack/test/pack.sh "$1" 2>&1 | tee "$run/output"
}

cmd="${1:-}"
case "$cmd" in
	test|corpus|cluster)
		leg "$cmd"
		;;
	all)
		rc=0
		for c in test corpus cluster; do
			leg "$c" || rc=1
		done
		echo "vexec_postgis: $([ $rc -eq 0 ] && echo "every leg passed" || echo "a leg FAILED")"
		exit $rc
		;;
	*)
		sed -n "3,/^# A leg's outcome/p" "$0" | sed 's/^# \{0,1\}//'
		exit 2
		;;
esac
