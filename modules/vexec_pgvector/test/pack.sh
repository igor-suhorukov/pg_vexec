#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# vexec_pgvector's legs inside a container of pg_accel's port image
# (test/run.sh): vexec from pg_accel's tree at /src, the port's modules from
# the stage at /cbstage, and the pack from /pack, built by PGXS and
# installed into the container's server; then
#
#   pack.sh test      the pack's suites on one node: with the pack preloaded,
#                     declarations, answers, plans and versions; without it,
#                     declarations and answers, against the same expected
#                     output -- PostgreSQL's answers
#   pack.sh corpus    pgvector's own regression tests through pg_accel's
#                     differential runner (pg_vector_executor.md §6.1): off,
#                     force in both formats, and force in both formats with
#                     the pack preloaded, each compared with off
#   pack.sh cluster   test/cluster.sh: a coordinator and its segments
#
# For versions.sql, two empty update scripts of pgvector's are installed
# beside its own: to the installed version with "-vk" after it, and back.
# The servers' logs and the suites' outputs go to RESULTS_DIR.
set -u
CMD="${1:-test}"
# vexec_pgvector is built from /pack below, once: vexec's build leaves it out,
# here and in every leg's build this container runs.
export VEXEC_SKIP_MODULES=vexec_pgvector
/src/test/vexec/build.sh || exit 1
PG_CONFIG="$(command -v pg_config)"
rm -rf /tmp/pack-build
cp -a /pack /tmp/pack-build
make -s -C /tmp/pack-build PG_CONFIG="$PG_CONFIG" COPT=-Werror || { echo "vexec_pgvector did not build"; exit 1; }
make -s -C /tmp/pack-build PG_CONFIG="$PG_CONFIG" install > /dev/null || exit 1
echo "  vexec_pgvector built and installed"

ext="$("$PG_CONFIG" --sharedir)/extension"
v="$(sed -n "s/^default_version = '\(.*\)'/\1/p" "$ext/vector.control")"
echo "-- vexec_pgvector's tests: pgvector $v under a version the pack does not name" > "$ext/vector--$v--$v-vk.sql"
echo "-- vexec_pgvector's tests: back to pgvector $v" > "$ext/vector--$v-vk--$v.sql"
echo "  pgvector $v, at $(cat /pgvector/.pgvector_commit 2> /dev/null || echo '?')"

. /src/test/vexec/lib.sh

suite() {					# suite <name> <preload> <tests>
	local name="$1" preload="$2" tests="$3" d rc
	d="$(mktemp -d "${TMPDIR:-/tmp}/pack-$name-XXXXXX")"
	server_init "$d" "shared_preload_libraries = '$preload'" || return 1
	server_start "$d" || { echo "the server did not start"; tail -20 "$d/log"; return 1; }
	echo "== $name: shared_preload_libraries = '$preload'"
	rm -rf /tmp/pack-build/test/results /tmp/pack-build/test/regression.*
	PGHOST="$d/sock" PGUSER=postgres make -C /tmp/pack-build -s PG_CONFIG="$PG_CONFIG" \
		installcheck REGRESS="$tests"
	rc=$?
	if [ -n "${RESULTS_DIR:-}" ]; then
		mkdir -p "$RESULTS_DIR/$name"
		cp -r /tmp/pack-build/test/results "$RESULTS_DIR/$name/" 2> /dev/null
		cp /tmp/pack-build/test/regression.diffs "$RESULTS_DIR/$name/" 2> /dev/null
		cp "$d/log" "$RESULTS_DIR/$name/server.log"
	fi
	[ $rc -ne 0 ] && [ -f /tmp/pack-build/test/regression.diffs ] && head -100 /tmp/pack-build/test/regression.diffs
	server_stop "$d"
	return $rc
}

case "$CMD" in
	test)
		rc=0
		suite with-pack "vexec_pgvector,vexec" "declarations answers plans versions" || rc=1
		suite without-pack "vexec" "declarations answers" || rc=1
		exit $rc
		;;
	corpus)
		VEXEC_CORPORA=pgvector VEXEC_SESSIONS="off postgres arrow postgres-packs arrow-packs" \
			VEXEC_PACKS=vexec_pgvector /src/test/vexec/differential.sh
		;;
	cluster)
		bash /pack/test/cluster.sh
		;;
	*)
		echo "pack.sh: no such leg: $CMD"
		exit 2
		;;
esac
