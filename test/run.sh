#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# vexec_flight's tests on the host (pg_vector_executor.md §5, V10): its
# images, and each leg in a container of its own, its results in a run of
# the cache.  Nothing is installed on the host; it needs docker.
#
#   run.sh images [leg...]   vexec_flight/dev:<leg>, on pg_accel's
#                            pg_accel/vexec-dev:<leg>-arrow image
#   run.sh test [leg...]     the suite (test_flight.py, FlightJdbcTest.java)
#                            against a server with vexec and vexec_flight
#                            preloaded: vanilla PostgreSQL 19, or the port's
#                            coordinator with FLIGHT_SEGMENTS segments
#   run.sh bench [leg...]    TPC-H Q1 through adbc_driver_flightsql, against
#                            adbc_driver_postgresql over the same server
#   run.sh ingest [leg...]   lineitem into tables through the paths a client
#                            with Arrow has (ingest.py): Flight SQL's
#                            ingest, DoPut's parameters to a prepared
#                            INSERT, adbc_driver_postgresql's binary COPY
#                            and a CSV file's COPY; the port leg by default,
#                            as PAX is the port's
#   run.sh serve [leg]       the test's server with its corpus, left running
#                            in container vexec_flight-serve-<leg> for
#                            docker exec (. /work/env first); stopped by
#                            docker stop
#
# A leg is vanilla (REL_19_STABLE) or port (the patched server and the
# port's modules); both when not given.
#
# Settings, from the environment:
#   VEXEC_SRC         pg_accel's tree, whose vexec the legs build: ../pg_accel-v10
#   CB_COMMIT         the port's build: the newest pg_accel/cb-ext image's
#   VEXEC_PORT_STAGE  the port's modules the port leg installs (pg_accel's
#                     test/vexec/run.sh portbuild): ~/.cache/pg_accel/vexec/portbuild-v10/stage,
#                     or portbuild-v10-noassert's
#   FLIGHT_SEGMENTS   the port leg's segments: 2; 0 is one node of the port
#   FLIGHT_CPUS       each container's CPUs: 4
#   FLIGHT_MEM        each container's memory: 8g
#   FLIGHT_SF         the bench's TPC-H scale factor: 1
#   FLIGHT_RUNS       the bench's timed runs of each query: 5
#   FLIGHT_ROWS       the bench's large result, in rows: 1000000
#   FLIGHT_INGEST_ROWS, FLIGHT_INGEST_BATCH, FLIGHT_INGEST_WARM,
#   FLIGHT_INGEST_FORMATS, FLIGHT_INGEST_PATHS
#                     ingest's rows (1000000), Arrow batch (65536), warm
#                     load's rows (10000), tables ("porc_vec porc"; also
#                     aoco, heap) and paths ("csv copy flight"; also
#                     ingest); its loads of each are FLIGHT_RUNS
#   FLIGHT_TESTS      test_flight.py's tests to run, by name: all
#   FLIGHT_CACHE      the runs: ~/.cache/pg_accel/flight
#   FLIGHT_NOASSERT   1: the servers without their assertions, as a timed run
#                     wants them (pg_accel's vanilla-noassert and portnoassert
#                     images, and the stage portbuild-v10-noassert); images
#                     of their own, built by images with it set
#
# A leg's outcome is its container's exit status, through tee (pipefail).
set -u -o pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$here/.." && pwd)"
VEXEC_SRC="${VEXEC_SRC:-$(cd "$ROOT/.." && pwd)/pg_accel-v10}"
FLIGHT_CACHE="${FLIGHT_CACHE:-$HOME/.cache/pg_accel/flight}"
FLIGHT_CPUS="${FLIGHT_CPUS:-4}"
FLIGHT_MEM="${FLIGHT_MEM:-8g}"
FLAVOR_VANILLA=vanilla
FLAVOR_PORT=port
STAGE=portbuild-v10
if [ "${FLIGHT_NOASSERT:-0}" = 1 ]; then
	FLAVOR_VANILLA=vanilla-noassert
	FLAVOR_PORT=portnoassert
	STAGE=portbuild-v10-noassert
fi
VEXEC_PORT_STAGE="${VEXEC_PORT_STAGE:-$HOME/.cache/pg_accel/vexec/$STAGE/stage}"
if [ -z "${CB_COMMIT:-}" ]; then
	CB_COMMIT="$(docker images pg_accel/cb-ext --format '{{.CreatedAt}} {{.Tag}}' 2> /dev/null | sort -r | awk 'NR == 1 {print $NF}')"
fi

die() { echo "run.sh: $*" >&2; exit 1; }
legs() { [ $# -gt 0 ] && echo "$@" || echo "vanilla port"; }

new_run() {					# new_run <name>: a directory of the cache
	local run="$FLIGHT_CACHE/runs/$(date +%Y%m%dT%H%M%S)-$1"
	mkdir -p "$run" || die "cannot make $run"
	echo "$run"
}

base_image() {				# base_image <leg>
	case "$1" in
		vanilla) echo "pg_accel/vexec-dev:$FLAVOR_VANILLA-arrow" ;;
		port) echo "pg_accel/vexec-dev:$FLAVOR_PORT-arrow-$CB_COMMIT" ;;
		*) die "no such leg: $1" ;;
	esac
}

image() {					# image <leg>
	case "$1" in
		vanilla) echo "vexec_flight/dev:$FLAVOR_VANILLA" ;;
		port) echo "vexec_flight/dev:$FLAVOR_PORT-$CB_COMMIT" ;;
	esac
}

in_leg() {					# in_leg <leg> <run dir> <command...>
	local leg="$1" run="$2"
	local stage=()
	shift 2
	if [ "$leg" = port ]; then
		[ -d "$VEXEC_PORT_STAGE/usr/local/pgsql" ] || die "no stage of the port's modules at $VEXEC_PORT_STAGE"
		stage=(-v "$VEXEC_PORT_STAGE:/cbstage:ro" -e "FLIGHT_SEGMENTS=${FLIGHT_SEGMENTS:-2}")
	fi
	docker run --rm "${DOCKER_RUN_OPTS[@]}" --network none --cpus "$FLIGHT_CPUS" -m "$FLIGHT_MEM" --memory-swap "$FLIGHT_MEM" \
		--shm-size 2g --tmpfs /tmp:exec --user postgres \
		-v "$ROOT:/flight:ro" -v "$VEXEC_SRC:/src:ro" -v "$run:/work" "${stage[@]}" \
		-e RESULTS_DIR=/work -e "FLIGHT_SF=${FLIGHT_SF:-1}" -e "FLIGHT_RUNS=${FLIGHT_RUNS:-5}" \
		-e "FLIGHT_ROWS=${FLIGHT_ROWS:-1000000}" -e "FLIGHT_LEG=$leg" -e "FLIGHT_TESTS=${FLIGHT_TESTS:-}" \
		-e "FLIGHT_INGEST_ROWS=${FLIGHT_INGEST_ROWS:-1000000}" -e "FLIGHT_INGEST_BATCH=${FLIGHT_INGEST_BATCH:-65536}" \
		-e "FLIGHT_INGEST_WARM=${FLIGHT_INGEST_WARM:-10000}" -e "FLIGHT_INGEST_FORMATS=${FLIGHT_INGEST_FORMATS:-porc_vec porc}" \
		-e "FLIGHT_INGEST_PATHS=${FLIGHT_INGEST_PATHS:-csv copy flight}" \
		"$(image "$leg")" "$@"
}

DOCKER_RUN_OPTS=()
cmd="${1:-}"
[ $# -gt 0 ] && shift
case "$cmd" in
	images)
		for leg in $(legs "$@"); do
			[ "$leg" = port ] || [ -n "$CB_COMMIT" ] || die "no pg_accel/cb-ext image"
			docker build -f "$ROOT/docker/Dockerfile" --build-arg BASE_IMAGE="$(base_image "$leg")" \
				--build-arg DUCKDB_IMAGE="pg_accel/cb-ext:$CB_COMMIT" \
				-t "$(image "$leg")" "$ROOT/docker" || exit 1
		done
		;;
	test|bench|ingest)
		rc=0
		[ "$cmd" = ingest ] && [ $# -eq 0 ] && set -- port
		for leg in $(legs "$@"); do
			run="$(new_run "$leg-$cmd")"
			echo "== $cmd on the $leg leg: $run"
			in_leg "$leg" "$run" bash /flight/test/flight.sh "$cmd" 2>&1 | tee "$run/output" || rc=1
		done
		exit $rc
		;;
	serve)
		leg="${1:-vanilla}"
		run="$(new_run "$leg-serve")"
		DOCKER_RUN_OPTS=(-d --name "vexec_flight-serve-$leg")
		in_leg "$leg" "$run" bash -c "bash /flight/test/flight.sh serve > /work/output 2>&1" > /dev/null || exit 1
		for i in $(seq 1 600); do
			grep -q '^serving' "$run/output" 2> /dev/null && break
			docker inspect "vexec_flight-serve-$leg" > /dev/null 2>&1 || { cat "$run/output"; exit 1; }
			sleep 1
		done
		tail -3 "$run/output"
		;;
	*)
		sed -n '3,53p' "$0" | sed 's/^# \{0,1\}//'
		exit 2
		;;
esac
