#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# ClickBench's suite for pg_accel, on the host: the images, the data, the runs
# and the baseline of pg_vector_executor.md §6.9 (phase VB).  Every server,
# DuckDB and the data's tools run in containers (docker/compose.yml); the host
# needs docker, curl and python3.
#
#   run.sh images             the images, built from the port's Dockerfiles in
#                             its checkout and tagged by the commits they are
#                             built from (the port's own tags are not touched)
#   run.sh fetch              hits.tsv.gz into the cache, from ClickBench's own
#                             address (lib/download-hits-tsv); 16.3 GB
#   run.sh prepare            the 1M- and 10M-row subsets, and DuckDB's answers
#                             over each (CLICKBENCH_FULL=1: the full table too)
#   run.sh run <mode> <load>...
#                             loads run in <mode>, check or time, into a new run
#                             of the cache, each then reported; a load is
#                             vanilla-heap, vanilla-heap_pk, or
#                             port-<aoco|porc|porc_vec>-s<segments>
#   run.sh baseline           the baseline, before development: every load in
#                             check mode on 1M rows and in time mode on 10M rows,
#                             ClickBench's protocol three times over, saved into
#                             baseline/<host>/<date>/; then the protocol once
#                             more, compared with it
#   run.sh report <run>       a run's reports again
#   run.sh save <dest> <run>...   what the tree keeps of runs
#   run.sh compare <a> <b>    clickbench.py compare: a baseline directory, a
#                             run of the cache, or a load of a run
#
# Settings, from the environment:
#   CLICKBENCH_CACHE   the data, DuckDB's answers, and the runs, with the
#                      answers PostgreSQL gave: ~/.cache/pg_accel/clickbench
#   CLICKBENCH_SRC     ClickBench's checkout: ../../../ClickBench (dfe44c96)
#   CB_SRC, PG_SRC     the port's and PostgreSQL's checkouts: ../../cloudberry,
#                      ../../postgres
#   CB_TESTS_MEM       each container's memory, 40g (the port's tests' cap)
#   CB_SUBSET, CB_REPS, CB_TRIES, CB_TIMEOUT, CB_COLD, CB_QUERIES, CB_WORKERS,
#   CB_STATS           passed to bench.sh, which says what they do
#   LOADS              the loads of "baseline": all eleven
#   CHECK_RUN, TIME_RUN, AGAIN_RUN
#                      runs of the cache "baseline" goes on with, rather than
#                      making new ones: a load a run has reported is not run
#                      again, so a baseline stopped part way resumes
#   BASELINE_DATE      the baseline's date, today's; a baseline resumed on a
#                      later day names the day it began
#   CB_COMMIT, PG_PATCHED_COMMIT
#                      the builds, the branches' heads; a baseline already
#                      begun keeps the ones its environment.json names
#   CB_VEXEC           vexec's sessions for "run" (bench.sh says which): vexec
#                      built by PGXS in vexec's own images (test/vexec/run.sh
#                      images) into the cache, with the port's modules of the
#                      phase's worktree (VEXEC_PORT_STAGE, as
#                      test/vexec/run.sh portbuild stages them), and each
#                      load's container started through vexec-entry.sh.  In
#                      time mode both are the builds without assertions
#                      (VEXEC_NOASSERT=1 test/vexec/run.sh images and
#                      portbuild), as the timed servers are
#
# One container runs at a time, and nothing else should: a timed run measures
# the host.

set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$here/../.." && pwd)"
COMPOSE="$ROOT/docker/compose.yml"

export CLICKBENCH_CACHE="${CLICKBENCH_CACHE:-$HOME/.cache/pg_accel/clickbench}"
export CLICKBENCH_SRC="$(cd "${CLICKBENCH_SRC:-$ROOT/../../ClickBench}" && pwd)"
export CB_SRC="$(cd "${CB_SRC:-$ROOT/../cloudberry}" && pwd)"
export PG_SRC="$(cd "${PG_SRC:-$ROOT/../postgres}" && pwd)"
export CLICKBENCH_WORK="$CLICKBENCH_CACHE/work"
export CLICKBENCH_RUNS="$CLICKBENCH_CACHE/runs"
export CLICKBENCH_HOST="${CLICKBENCH_HOST:-$(hostname -s)}"
export CB_TESTS_MEM="${CB_TESTS_MEM:-40g}"
mkdir -p "$CLICKBENCH_CACHE" "$CLICKBENCH_WORK" "$CLICKBENCH_RUNS"

# The servers' builds: the port's branch, and the core series it is built on.
# A baseline already begun goes on with the builds it began with, which its
# environment.json names, wherever the branches have moved since: its loads
# are measured on one build, and a resumed baseline builds nothing.
if [ "${1:-}" = baseline ] && [ -z "${CB_COMMIT:-}${PG_PATCHED_COMMIT:-}" ]; then
	began="$here/baseline/$CLICKBENCH_HOST/${BASELINE_DATE:-$(date +%F)}/environment.json"
	if [ -s "$began" ]; then
		CB_COMMIT="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["commits"]["port"])' "$began")"
		PG_PATCHED_COMMIT="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["commits"]["postgresql_patched"])' "$began")"
		echo "  the baseline begun in $(dirname "$began") goes on with the port at $CB_COMMIT"
	fi
fi
export CB_COMMIT="${CB_COMMIT:-$(git -C "$CB_SRC" rev-parse extension_postgresql_19)}"
export PG_PATCHED_COMMIT="${PG_PATCHED_COMMIT:-$(git -C "$PG_SRC" rev-parse REL_19_STABLE_CLOUDBERRY)}"
ORCA_CHECK_IMAGE="pg_accel/cb-ext:$CB_COMMIT"
ORCA_TIME_IMAGE="pg_accel/cb-ext-noassert:$CB_COMMIT"
VANILLA_IMAGE="pg_accel/clickbench-vanilla:latest"
ALL_LOADS="vanilla-heap vanilla-heap_pk port-aoco-s4 port-aoco-s8 port-aoco-s16 port-porc-s4 port-porc-s8 port-porc-s16 port-porc_vec-s4 port-porc_vec-s8 port-porc_vec-s16"

die() { echo "run.sh: $*" >&2; exit 1; }
compose() { docker compose -f "$COMPOSE" "$@"; }
# py <args>: clickbench.py in the vanilla image, where DuckDB is
py() { compose --profile run run --rm -T --entrypoint /opt/duckdb/bin/python vanilla /clickbench/clickbench.py "$@"; }

images() {
	compose --profile build build pg19-patched pg19-patched-noassert clickbench-vanilla &&
	compose --profile build build cb-ext cb-ext-noassert
}

have_images() {
	local i
	for i in "$VANILLA_IMAGE" "$ORCA_CHECK_IMAGE" "$ORCA_TIME_IMAGE"; do
		docker image inspect "$i" > /dev/null 2>&1 || return 1
	done
}

fetch() {
	local gz="$CLICKBENCH_CACHE/hits.tsv.gz" url="https://datasets.clickhouse.com/hits_compatible/hits.tsv.gz"
	[ "$(stat -c %s "$gz" 2> /dev/null)" = 16298506510 ] && { echo "  $gz is there"; return 0; }
	curl -fL -C - --retry 5 -o "$gz.part" "$url" || die "could not download $url"
	[ "$(stat -c %s "$gz.part")" = 16298506510 ] || die "$gz.part is not 16298506510 bytes"
	mv "$gz.part" "$gz"
}

prepare() {
	fetch || exit 1
	py subsets /cache ${CLICKBENCH_FULL:+full} || die "the subsets"
	local s
	for s in 1m 10m ${CLICKBENCH_FULL:+full}; do
		if [ -s "$CLICKBENCH_CACHE/ref/$s/refs.json" ] &&
		   [ "$CLICKBENCH_CACHE/ref/$s/refs.json" -nt "$CLICKBENCH_CACHE/hits_$s.tsv" ]; then
			echo "  DuckDB's answers over $s are there"
		else
			py refs /cache /clickbench-src "$s" || die "DuckDB's answers over $s"
		fi
	done
}

# The host and the builds, as a baseline keeps them.
environment() {
	local f="$1" img
	{
		echo "{"
		echo " \"host\": {\"name\": \"$CLICKBENCH_HOST\","
		echo "   \"cpu\": \"$(lscpu | sed -n 's/^Model name: *//p')\","
		echo "   \"threads\": $(nproc), \"cores\": $(lscpu -p=core | grep -v '^#' | sort -u | wc -l),"
		echo "   \"memory_kb\": $(awk '/MemTotal/ { print $2 }' /proc/meminfo),"
		echo "   \"kernel\": \"$(uname -r)\","
		echo "   \"governor\": \"$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2> /dev/null)\","
		echo "   \"energy_performance_preference\": \"$(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference 2> /dev/null)\","
		echo "   \"filesystem\": \"$(findmnt -no FSTYPE,SOURCE -T "$CLICKBENCH_WORK" | tr -s ' ')\","
		echo "   \"docker\": \"$(docker version -f '{{.Server.Version}}' 2> /dev/null)\","
		echo "   \"container_memory\": \"$CB_TESTS_MEM\"},"
		echo " \"commits\": {\"port\": \"$CB_COMMIT\", \"postgresql_patched\": \"$PG_PATCHED_COMMIT\","
		echo "   \"postgresql_vanilla\": \"$(docker run --rm --entrypoint cat "$VANILLA_IMAGE" /pg/vanilla-noassert/.pg_ref_commit)\","
		echo "   \"clickbench\": \"$(git -C "$CLICKBENCH_SRC" rev-parse HEAD)\","
		echo "   \"pg_accel\": \"$(git -C "$ROOT" rev-parse HEAD 2> /dev/null || echo uncommitted)\","
		echo "   \"suite_sha256\": \"$(cat "$here/run.sh" "$here/bench.sh" "$here/clickbench.py" "$COMPOSE" "$ROOT/docker/Dockerfile.clickbench" | sha256sum | cut -c1-16)\"},"
		echo " \"images\": {"
		for img in "$VANILLA_IMAGE" "$ORCA_CHECK_IMAGE" "$ORCA_TIME_IMAGE"; do
			echo "   \"$img\": \"$(docker image inspect -f '{{.Id}}' "$img")\","
		done
		echo "   \"vanilla from\": \"$(docker image inspect -f '{{.Id}}' cloudberry/pg19-vanilla:latest) $(docker image inspect -f '{{.Id}}' cloudberry/pg19-vanilla-noassert:latest)\"},"
		echo " \"data\": $(cat "$CLICKBENCH_CACHE/subsets.json")"
		echo "}"
	} > "$f"
	python3 -m json.tool "$f" > /dev/null || die "$f is not JSON"
}

# vexec for the loads (CB_VEXEC): built in vexec's own images, whose servers
# are these images' builds -- with assertions for check mode, without them
# for time mode -- and copied into the cache with the port's stage, where
# vexec-entry.sh installs them in each container.  ASSERTS says which build
# it was made against, which vexec-entry.sh checks against the server's.
vexec_build() {
	local mode="$1" leg dest stage flavors=() asserts=yes
	if [ "$mode" = time ]; then
		stage="${VEXEC_PORT_STAGE:-$HOME/.cache/pg_accel/vexec/portbuild-noassert/stage}"
		flavors=(VEXEC_PORT_FLAVOR=portnoassert VEXEC_VANILLA_FLAVOR=vanilla-noassert)
		asserts=no
	else
		stage="${VEXEC_PORT_STAGE:-$HOME/.cache/pg_accel/vexec/portbuild/stage}"
	fi
	[ -d "$stage/usr/local/pgsql" ] || die "no port stage at $stage: test/vexec/run.sh portbuild makes it"
	for leg in vanilla port; do
		dest="$CLICKBENCH_CACHE/vexec/$leg"
		rm -rf "$dest"
		mkdir -p "$dest"
		env "${flavors[@]}" VEXEC_WORK="$dest" VEXEC_PORT_STAGE="$stage" CB_COMMIT="$CB_COMMIT" \
			docker compose -f "$ROOT/docker/vexec.yml" --profile run run --rm -T "$leg" bash -c '
				/src/test/vexec/build.sh > /work/build.log 2>&1 || { cat /work/build.log; exit 1; }
				mkdir -p /work/lib /work/extension
				cp "$(pg_config --pkglibdir)/vexec.so" /work/lib/
				cp "$(pg_config --sharedir)"/extension/vexec* /work/extension/
				sed -n "s/^building against //p" /work/build.log > /work/BUILT
				if grep -q "^#define USE_ASSERT_CHECKING" "$(pg_config --includedir-server)/pg_config.h"; then
					echo yes; else echo no; fi > /work/ASSERTS' 2>&1 |
			grep -v -E '^ (Container|Network) '
		[ -f "$dest/lib/vexec.so" ] || die "vexec for the $leg route did not build"
		[ "$(cat "$dest/ASSERTS" 2> /dev/null)" = "$asserts" ] ||
			die "vexec for the $leg route was built against a server whose assertions are $(cat "$dest/ASSERTS" 2> /dev/null), not $asserts"
		[ "$leg" = port ] && cp -a "$stage" "$dest/stage"
		echo "  vexec for the $leg route: $(cat "$dest/BUILT"), assertions $asserts"
	done
}

# run_loads <mode> <run id> <load>...: each in its container, then reported.
run_loads() {
	local mode="$1" id="$2" load route storage seg service image rc failed=0
	shift 2
	mkdir -p "$CLICKBENCH_RUNS/$id"
	have_images || die "the images are missing: run.sh images"
	[ -s "$CLICKBENCH_CACHE/ref/${CB_SUBSET:-1m}/refs.json" ] || die "DuckDB's answers are missing: run.sh prepare"
	local others
	others=$(docker ps --format '{{.Names}}' | grep -v '^pg_accel' | tr '\n' ' ')
	[ -n "$others" ] && echo "  (other containers are running: $others-- timings measure the host)"
	for load in "$@"; do
		case "$load" in
			vanilla-heap|vanilla-heap_pk)
				route=vanilla; storage="${load#vanilla-}"; seg=0; service=vanilla; image="$VANILLA_IMAGE" ;;
			port-aoco-s*|port-porc-s*|port-porc_vec-s*)
				route=port; seg="${load##*-s}"; storage="${load#port-}"; storage="${storage%-s*}"; service=orca
				[ "$mode" = time ] && image="$ORCA_TIME_IMAGE" || image="$ORCA_CHECK_IMAGE" ;;
			*) die "no such load: $load" ;;
		esac
		local out="$CLICKBENCH_RUNS/$id/$load" stats=""
		# a load a run has already reported is kept: a run goes on where it stopped
		if [ -s "$out/report.json" ]; then
			echo "== $mode, $load: in $id already"
			continue
		fi
		rm -rf "$out"
		# a load's statistics from another run (CB_STATS_RUN): its plans then
		[ -n "${CB_STATS_RUN:-}" ] && stats="/runs/$CB_STATS_RUN/$load/stats.json"
		echo "== $mode, $load: $(date '+%F %T')"
		local start=()
		[ -n "${CB_VEXEC:-}" ] && start=(--user root --entrypoint /clickbench/vexec-entry.sh)
		ORCA_IMAGE="$image" compose --profile run run --rm -T "${start[@]}" \
			-e CB_MODE="$mode" -e CB_ROUTE="$route" -e CB_STORAGE="$storage" -e CB_SEGMENTS="$seg" \
			-e CB_VEXEC="${CB_VEXEC:-}" \
			-e CB_SUBSET="${CB_SUBSET:-1m}" -e CB_REPS="${CB_REPS:-3}" -e CB_TRIES="${CB_TRIES:-3}" \
			-e CB_TIMEOUT="${CB_TIMEOUT:-1800}" -e CB_COLD="${CB_COLD:-evict}" \
			-e CB_QUERIES="${CB_QUERIES:-}" -e CB_WORKERS="${CB_WORKERS:-0}" \
			-e CB_STATS="${CB_STATS:-$stats}" -e CB_OUT="/runs/$id/$load" -e KEEP="${KEEP:-}" \
			"$service" > "$CLICKBENCH_RUNS/$id/$load.log" 2>&1
		rc=$?
		sed 's/^/  /' "$CLICKBENCH_RUNS/$id/$load.log" | grep -v '^  \(Network\|Container\)'
		if [ $rc -ne 0 ]; then
			echo "  FAILED: $load, exit $rc"
			failed=1
			continue
		fi
		py report "/runs/$id/$load" /cache /clickbench-src > "$CLICKBENCH_RUNS/$id/$load.report" 2>&1 || failed=1
		head -3 "$CLICKBENCH_RUNS/$id/$load.report" | grep -v '^    Q'
	done
	return $failed
}

baseline() {
	local date id_check id_time id_again dest
	date="${BASELINE_DATE:-$(date +%F)}"
	have_images || images || die "the images"
	prepare || exit 1
	dest="$here/baseline/$CLICKBENCH_HOST/$date"
	mkdir -p "$dest"
	environment "$dest/environment.json"
	local loads="${LOADS:-$ALL_LOADS}"
	# CHECK_RUN, TIME_RUN and AGAIN_RUN name runs to go on with: the loads
	# they have reported are kept, the others run.
	id_check="${CHECK_RUN:-$(date +%Y%m%dT%H%M%S)-check}"
	CB_SUBSET=1m run_loads check "$id_check" $loads
	id_time="${TIME_RUN:-$(date +%Y%m%dT%H%M%S)-time}"
	CB_SUBSET=10m CB_REPS="${CB_REPS:-3}" run_loads time "$id_time" $loads
	python3 "$here/clickbench.py" save "$dest" $(for l in $loads; do
		for id in "$id_check" "$id_time"; do
			[ -s "$CLICKBENCH_RUNS/$id/$l/report.json" ] && echo "$CLICKBENCH_RUNS/$id/$l"
		done; done)
	echo "$id_check $id_time" > "$dest/runs"
	# The protocol once more, on loads given the baseline's statistics: each
	# query within its spread, or explained (pg_vector_executor.md, VB).
	id_again="${AGAIN_RUN:-$(date +%Y%m%dT%H%M%S)-again}"
	CB_SUBSET=10m CB_REPS=1 CB_STATS_RUN="$id_time" run_loads time "$id_again" $loads
	python3 "$here/clickbench.py" compare "$dest" "$CLICKBENCH_RUNS/$id_again" > "$dest/again.txt"
	python3 "$here/clickbench.py" compare "$dest" "$dest" > "$dest/self.txt"
	echo "$id_check $id_time $id_again" > "$dest/runs"
	echo "baseline: $dest; the second run against it: $dest/again.txt"
}

cmd="${1:-}"
[ $# -gt 0 ] && shift
case "$cmd" in
	images) images ;;
	fetch) fetch ;;
	prepare) prepare ;;
	run)
		[ $# -ge 2 ] || die "run <check|time> <load>..."
		mode="$1"; shift
		[ "$mode" = time ] && export CB_SUBSET="${CB_SUBSET:-10m}"
		if [ -n "${CB_VEXEC:-}" ]; then
			vexec_build "$mode" || exit 1
		fi
		run_loads "$mode" "$(date +%Y%m%dT%H%M%S)-$mode${CB_VEXEC:+-vexec}" "$@" ;;
	baseline) baseline ;;
	report)
		[ $# -eq 1 ] || die "report <run>"
		for d in "$CLICKBENCH_RUNS/$1"/*/; do
			py report "/runs/$1/$(basename "$d")" /cache /clickbench-src
		done ;;
	save)
		[ $# -ge 2 ] || die "save <dest> <run dir>..."
		python3 "$here/clickbench.py" save "$@" ;;
	compare)
		[ $# -eq 2 ] || die "compare <a> <b>"
		python3 "$here/clickbench.py" compare "$@" ;;
	*) sed -n '3,46p' "$0"; exit 2 ;;
esac
