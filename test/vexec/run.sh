#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# vexec's tests on the host (pg_vector_executor.md §5 V0, §6): the images,
# and each leg in a container of its own (docker/vexec.yml), its results in
# a run of the cache.  Nothing is installed on the host; it needs docker,
# git and python3.
#
#   run.sh images                the dev images: the vanilla leg's, from the
#                                port's cloudberry/pg19-vanilla; the port's,
#                                from VB's pg_accel/cb-ext at CB_COMMIT, and
#                                the port's build tools on it (portdev)
#   run.sh portbuild             the port's modules built from VEXEC_PORT_SRC,
#                                V1's worktree of the port, into
#                                VEXEC_PORT_BUILD, and staged there: the port
#                                leg's containers install the stage over
#                                their image's modules (VEXEC_PORT_STAGE)
#   run.sh checks                the header copies, the notices, the tree
#                                (on the host)
#   run.sh suite [leg]           vexec's own regression suite
#   run.sh ipc [leg]             the IPC codec against pyarrow (V7_0), in
#                                the leg's -arrow container: ipc.sh
#   run.sh states [leg]          vexec's states of §1.2: installed and not
#                                preloaded, its objects without its library,
#                                its library removed
#   run.sh pgregress [leg]       PostgreSQL's regression suite, unchanged,
#                                with vexec installed, preloaded off, and
#                                preloaded in explain mode
#   run.sh differential [leg]    the differential runner's four sessions
#   run.sh sources               the batch sources on a single node of the
#                                port: heap, ao_row, ao_column, PAX porc and
#                                porc_vec, each query off and in force mode
#   run.sh sinks                 the batch sinks on a single node of the port
#                                (VI): each storage loaded by VecInsert,
#                                against COPY's load and ModifyTable's, its
#                                errors and aborts
#   run.sh portsuites            the port's singlenode and greenplum suites,
#                                each pass, with vexec.mode = force in both
#                                formats, against off (VEXEC_PORT_SUITES,
#                                VEXEC_PORT_PASSES, VEXEC_PORT_SESSIONS)
#   run.sh cluster               the cluster leg, four segments of the port;
#                                VEXEC_INTERCONNECT=shm runs it over the shm
#                                transport, preloaded on every node (V7)
#   run.sh shm                   the shm transport's leg (V7): its Motions,
#                                switches, remote path, hang tests and kills,
#                                and the port's interconnect test from
#                                VEXEC_PORT_SRC (shm.sh)
#   run.sh orcacost              V5's leg: ORCA's vector cost model and its
#                                hashed window, VecWindowHashAgg, on a node of
#                                the port
#   run.sh orcasuite             the port's orca suite (pg19/test/orca) on the
#                                port's modules of the stage, without vexec:
#                                gp_orca's API in place and no engine
#   run.sh tpc [storage...]      the tpc suite on four segments (CB_TPC=check)
#                                in each storage: heap ao_column pax
#                                pax_porc_vec, vexec preloaded in
#                                TPC_VEXEC_MODE (off) and TPC_VEXEC_FORMAT,
#                                with TPC_WORKERS parallel workers a
#                                segment ("0"); TPC_INTERCONNECT tcp or
#                                shm, every node's transport, shm
#                                preloaded (V7)
#   run.sh fullrun [suite...]    the port's full run with vexec on every
#                                node, and without it, compared: each
#                                run's latest of VEXEC_FULLRUN_RUNS
#   run.sh v0                    V0's "done when": checks; the suite, the
#                                states, pgregress and differential on both
#                                legs; the cluster leg
#
# A leg is vanilla (REL_19_STABLE as it is, vexec built by PGXS) or port (the
# patched server with the port's modules); both when not given.
#
# Settings, from the environment:
#   VEXEC_CACHE   the runs: ~/.cache/pg_accel/vexec
#   CB_SRC, PG_SRC the port's and PostgreSQL's checkouts: ../../cloudberry,
#                 ../../postgres
#   CB_COMMIT     the port's build: the newest pg_accel/cb-ext image's
#   VEXEC_CPUS    each container's CPUs: 0, as many as there are; 1 beside a
#                 timed run
#   CB_TESTS_MEM  each container's memory: 40g
#   VEXEC_FULLRUN_RUNS  the full runs to make: "0 1", without vexec and
#                 with it; "1" compares a new run with vexec with the
#                 latest run without it
#   VEXEC_PORT_SRC  V1's worktree of the port: ../../cloudberry-vexec/wt
#   VEXEC_PORT_BUILD  its build and stage: $VEXEC_CACHE/portbuild
#   VEXEC_PORT_STAGE  the stage the port leg installs: VEXEC_PORT_BUILD's,
#                 when it has one; "none" for the image's own modules
#   VEXEC_PAX_CONTRIB  clones of PAX's two submodules at the commits the
#                 tree pins: $VEXEC_CACHE/pax-contrib
#
# Timed runs measure the host: ClickBench's time mode (test/clickbench) asks
# for nothing else to run.  Beside one, give VEXEC_CPUS=1, and leave tpc and
# fullrun, which want the whole machine, for after it.
#
# A leg's outcome is its container's exit status, through tee (pipefail).
set -u -o pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$here/../.." && pwd)"
COMPOSE="$ROOT/docker/vexec.yml"
export VEXEC_CACHE="${VEXEC_CACHE:-$HOME/.cache/pg_accel/vexec}"
export CB_SRC="${CB_SRC:-$(cd "$ROOT/.." && pwd)/cloudberry}"
export PG_SRC="${PG_SRC:-$(cd "$ROOT/.." && pwd)/postgres}"
export VEXEC_CPUS="${VEXEC_CPUS:-0}"
export VEXEC_PORT_SRC="${VEXEC_PORT_SRC:-$(cd "$ROOT/.." && pwd)/cloudberry-vexec/wt}"
# A timed run measures the server without its assertions (§6.3): the port's
# images built without them, and the port's modules built against those.
[ "${CB_TPC:-}" = time ] && [ -z "${VEXEC_NOASSERT:-}" ] && VEXEC_NOASSERT=1
if [ "${VEXEC_NOASSERT:-0}" = 1 ]; then
	export VEXEC_PORT_FLAVOR=portnoassert VEXEC_VANILLA_FLAVOR=vanilla-noassert VEXEC_CBEXT_SUFFIX=-noassert
	export VEXEC_PORT_BUILD="${VEXEC_PORT_BUILD:-$VEXEC_CACHE/portbuild-noassert}"
	export VEXEC_ORCA_BUILD="${VEXEC_ORCA_BUILD:-$VEXEC_CACHE/orcabuild-noassert}"
fi
export VEXEC_PORT_BUILD="${VEXEC_PORT_BUILD:-$VEXEC_CACHE/portbuild}"
export VEXEC_ORCA_BUILD="${VEXEC_ORCA_BUILD:-$VEXEC_CACHE/orcabuild}"
export VEXEC_PAX_CONTRIB="${VEXEC_PAX_CONTRIB:-$VEXEC_CACHE/pax-contrib}"
if [ "${VEXEC_PORT_STAGE:-}" = none ]; then
	unset VEXEC_PORT_STAGE
elif [ -z "${VEXEC_PORT_STAGE:-}" ] && [ -d "$VEXEC_PORT_BUILD/stage/usr/local/pgsql" ]; then
	export VEXEC_PORT_STAGE="$VEXEC_PORT_BUILD/stage"
fi
if [ -z "${VEXEC_ORCA_STAGE:-}" ] && [ -d "$VEXEC_ORCA_BUILD/stage/usr/local/pgsql" ]; then
	export VEXEC_ORCA_STAGE="$VEXEC_ORCA_BUILD/stage"
fi

if [ -z "${CB_COMMIT:-}" ]; then
	CB_COMMIT="$(docker images "pg_accel/cb-ext${VEXEC_CBEXT_SUFFIX:-}" --format '{{.CreatedAt}} {{.Tag}}' 2> /dev/null | sort -r | awk 'NR == 1 {print $NF}')"
fi
export CB_COMMIT

die() { echo "run.sh: $*" >&2; exit 1; }

new_run() {					# new_run <name>: a directory of the cache
	local run="$VEXEC_CACHE/runs/$(date +%Y%m%dT%H%M%S)-$1"
	mkdir -p "$run" || die "cannot make $run"
	echo "$run"
}

in_leg() {					# in_leg <leg> <run dir> <command>: RESULTS_DIR, the run, for every command of it
	local leg="$1" run="$2" cmd="$3"
	[ "$leg" = port ] && [ -z "$CB_COMMIT" ] && die "no pg_accel/cb-ext image: run test/clickbench/run.sh images"
	VEXEC_WORK="$run" docker compose -f "$COMPOSE" --profile run run --rm -T "$leg" \
		bash -c "export RESULTS_DIR=/work; $cmd" 2>&1 | grep -v -E '^ (Container|Network) '
	return "${PIPESTATUS[0]}"
}

legs() { [ $# -gt 0 ] && echo "$@" || echo "vanilla port"; }

cmd="${1:-}"
[ $# -gt 0 ] && shift
case "$cmd" in
	images)
		docker compose -f "$COMPOSE" --profile build build vexec-dev-vanilla || exit 1
		[ -n "$CB_COMMIT" ] || die "no pg_accel/cb-ext image: run test/clickbench/run.sh images"
		docker compose -f "$COMPOSE" --profile build build vexec-dev-port || exit 1
		docker compose -f "$COMPOSE" --profile build build vexec-dev-portdev
		;;
	portbuild)
		[ -n "$CB_COMMIT" ] || die "no pg_accel/cb-ext image: run test/clickbench/run.sh images"
		[ -d "$VEXEC_PORT_SRC/pg19" ] || die "no worktree of the port at $VEXEC_PORT_SRC"
		mkdir -p "$VEXEC_PORT_BUILD"
		PORTBUILD_COMMIT="$(git -C "$VEXEC_PORT_SRC" rev-parse --short HEAD)$( [ -n "$(git -C "$VEXEC_PORT_SRC" status --porcelain -- pg19)" ] && echo +changes)" \
			docker compose -f "$COMPOSE" --profile run run --rm -T portbuild 2>&1 | grep -v -E '^ (Container|Network) '
		exit "${PIPESTATUS[0]}"
		;;
	orcaimages)
		[ -n "$CB_COMMIT" ] || die "no pg_accel/cb-ext image, whose DuckDB the image takes: run test/clickbench/run.sh images"
		docker compose -f "$COMPOSE" --profile build build vexec-dev-vanillaorca
		;;
	orcabuild)
		[ -d "$VEXEC_PORT_SRC/pg19" ] || die "no worktree of the port at $VEXEC_PORT_SRC"
		mkdir -p "$VEXEC_ORCA_BUILD"
		PORTBUILD_COMMIT="$(git -C "$VEXEC_PORT_SRC" rev-parse --short HEAD)$( [ -n "$(git -C "$VEXEC_PORT_SRC" status --porcelain -- pg19)" ] && echo +changes)" \
			docker compose -f "$COMPOSE" --profile run run --rm -T orcabuild 2>&1 | grep -v -E '^ (Container|Network) '
		exit "${PIPESTATUS[0]}"
		;;
	orca)
		[ -n "${VEXEC_ORCA_STAGE:-}" ] || die "no gp_orca built alone at $VEXEC_ORCA_BUILD: run.sh orcabuild"
		run="$(new_run orca)"
		echo "== ORCA on vanilla PostgreSQL 19 (V6): $run"
		echo "  gp_orca from $(cat "$VEXEC_ORCA_STAGE/COMMIT" 2> /dev/null || echo '?'), the port's worktree $VEXEC_PORT_SRC"
		in_leg vanilla-orca "$run" "/src/test/vexec/orca.sh" | tee "$run/output"
		exit "${PIPESTATUS[0]}"
		;;
	checks)
		rc=0
		"$here/checks/headers.sh" || rc=1
		"$here/checks/notices.sh" || rc=1
		"$here/checks/tree.sh" || rc=1
		exit $rc
		;;
	suite|states|pgregress|differential)
		rc=0
		for leg in $(legs "$@"); do
			run="$(new_run "$leg-$cmd")"
			echo "== $cmd on the $leg leg: $run"
			in_leg "$leg" "$run" "/src/test/vexec/$cmd.sh" | tee "$run/output" || rc=1
		done
		exit $rc
		;;
	ipc)
		rc=0
		for leg in $(legs "$@"); do
			[ "$leg" = port ] && [ -z "$CB_COMMIT" ] && die "no pg_accel/cb-ext image: run test/clickbench/run.sh images"
			run="$(new_run "$leg-ipc")"
			echo "== ipc on the $leg leg, against pyarrow: $run"
			in_leg "$leg-arrow" "$run" "/src/test/vexec/ipc.sh" | tee "$run/output" || rc=1
		done
		exit $rc
		;;
	cluster)
		run="$(new_run cluster)"
		echo "== the cluster leg: $run"
		in_leg port "$run" "/src/test/vexec/cluster.sh" | tee "$run/output"
		exit "${PIPESTATUS[0]}"
		;;
	shm)
		run="$(new_run shm)"
		echo "== the shm transport: $run"
		echo "  the port's modules from $(cat "${VEXEC_PORT_STAGE:-/nonexistent}/COMMIT" 2> /dev/null || echo 'the image'), the port's tests from $VEXEC_PORT_SRC"
		in_leg port "$run" "/src/test/vexec/shm.sh" | tee "$run/output"
		exit "${PIPESTATUS[0]}"
		;;
	portsuites)
		run="$(new_run portsuites)"
		echo "== the port's suites, force against off: $run"
		in_leg port "$run" "/src/test/vexec/portsuites.sh" | tee "$run/output"
		exit "${PIPESTATUS[0]}"
		;;
	orcasuite)
		run="$(new_run orcasuite)"
		echo "== the port's orca suite, without vexec: $run"
		in_leg port "$run" "/src/test/vexec/build.sh > /dev/null && bash /cb/pg19/test/orca/run.sh" | tee "$run/output"
		exit "${PIPESTATUS[0]}"
		;;
	orcacost)
		run="$(new_run orcacost)"
		echo "== ORCA's vector cost model and its hashed window: $run"
		in_leg port "$run" "/src/test/vexec/orcacost.sh" | tee "$run/output"
		exit "${PIPESTATUS[0]}"
		;;
	sources)
		run="$(new_run sources)"
		echo "== the batch sources on the port: $run"
		in_leg port "$run" "/src/test/vexec/sources.sh" | tee "$run/output"
		exit "${PIPESTATUS[0]}"
		;;
	sinks)
		run="$(new_run sinks)"
		echo "== the batch sinks on the port: $run"
		in_leg port "$run" "/src/test/vexec/sinks.sh" | tee "$run/output"
		exit "${PIPESTATUS[0]}"
		;;
	tpc)
		# TPC_LEG: port, or vanilla-orca -- V6's, one node of vanilla
		# PostgreSQL 19 with gp_orca built alone (VEXEC_ORCA_STAGE), heap
		rc=0
		tpc_leg="${TPC_LEG:-port}"
		[ "$tpc_leg" = vanilla-orca ] && [ -z "${VEXEC_ORCA_STAGE:-}" ] && die "no gp_orca built alone at $VEXEC_ORCA_BUILD: run.sh orcabuild"
		for storage in ${*:-$( [ "$tpc_leg" = vanilla-orca ] && echo heap || echo heap ao_column pax pax_porc_vec)}; do
			run="$(new_run "tpc-$( [ "$tpc_leg" = vanilla-orca ] && echo vanilla-orca- )$storage")"
			echo "== tpc on $storage: $run"
			if [ "$tpc_leg" = vanilla-orca ]; then
				echo "  image pg_accel/vexec-dev:${VEXEC_VANILLA_FLAVOR:-vanilla}-orca, gp_orca built alone from $(cat "$VEXEC_ORCA_STAGE/COMMIT" 2> /dev/null || echo '?')"
			else
				echo "  image pg_accel/vexec-dev:${VEXEC_PORT_FLAVOR:-port}-$CB_COMMIT, the port's modules from $(cat "${VEXEC_PORT_STAGE:-/nonexistent}/COMMIT" 2> /dev/null || echo 'the image')"
			fi
			in_leg "$tpc_leg" "$run" "/src/test/vexec/build.sh > /dev/null && { grep -q '^#define USE_ASSERT_CHECKING' \"\$(pg_config --includedir-server)/pg_config.h\" && echo '  server with assertions' || echo '  server without assertions'; } && CB_TPC=${CB_TPC:-check} TPC_STORAGE=$storage TPC_VEXEC_MODE=${TPC_VEXEC_MODE:-off} TPC_VEXEC_FORMAT=${TPC_VEXEC_FORMAT:-postgres} TPC_VEXEC_NUMERIC=${TPC_VEXEC_NUMERIC:-} TPC_VEXEC_SETTINGS='${TPC_VEXEC_SETTINGS:-}' TPC_PLANNER_ROUTE=${TPC_PLANNER_ROUTE:-1} TPC_SEGMENTS=${TPC_SEGMENTS:-$( [ "$tpc_leg" = vanilla-orca ] && echo 0 || echo 4)} TPC_ROUNDS=${TPC_ROUNDS:-1} TPC_WORKERS='${TPC_WORKERS:-0}' TPC_KINDS='${TPC_KINDS:-h ds}' TPC_QUERIES='${TPC_QUERIES:-}' TPC_SF=${TPC_SF:-1} TPC_TIMEOUT=${TPC_TIMEOUT:-120} TPC_PLANNING=${TPC_PLANNING:-0} TPC_PLANS=${TPC_PLANS:-0} TPC_INTERCONNECT=${TPC_INTERCONNECT:-} /src/test/tpc/run.sh" \
				| tee "$run/output" || rc=1
		done
		exit $rc
		;;
	fullrun)
		rc=0
		for preload in ${VEXEC_FULLRUN_RUNS:-0 1}; do
			run="$(new_run "fullrun-$preload")"
			echo "== the port's full run, vexec $( [ $preload = 1 ] && echo "on every node" || echo "not loaded"): $run"
			VEXEC_WORK="$run" VEXEC_FULLRUN_PRELOAD=$preload docker compose -f "$COMPOSE" --profile run \
				run --rm -T fullrun "$@" 2>&1 | grep -v -E '^ (Container|Network) ' | tee "$run/output"
		done
		python3 "$here/fullrun_compare.py" "$(ls -d "$VEXEC_CACHE"/runs/*-fullrun-0 | tail -1)/output" \
			"$(ls -d "$VEXEC_CACHE"/runs/*-fullrun-1 | tail -1)/output" || rc=1
		exit $rc
		;;
	v0)
		rc=0
		"$0" checks || rc=1
		for c in suite states pgregress differential; do
			"$0" "$c" || rc=1
		done
		"$0" cluster || rc=1
		echo
		echo "V0: $([ $rc -eq 0 ] && echo "every leg passed" || echo "a leg FAILED")"
		exit $rc
		;;
	*)
		sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'
		exit 2
		;;
esac
