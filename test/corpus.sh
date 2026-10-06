#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# PostGIS's own regression suite as a corpus of pg_accel's differential
# runner's kind (pg_vector_executor.md §6.1, §3.17's "the PostGIS ... corpora
# with each pack loaded and not loaded"), inside a container of the port's
# image, whose build tree of PostGIS (/postgis) carries the suite and the
# flags its configure chose (regress/port-regress.txt, Dockerfile.cbext):
# the core tests, run by PostGIS's run_test.pl, in five sessions:
#
#   off             vexec.mode = off
#   postgres        vexec.mode = force, vexec.batch_format = postgres
#   arrow           vexec.mode = force, vexec.batch_format = arrow
#   postgres-pack   postgres's, with vexec_postgis preloaded
#   arrow-pack      arrow's, with vexec_postgis preloaded
#
# A test's answer in a session is its output's difference from PostGIS's
# expected output -- none where it passes -- as run_test.pl keeps it, its
# two header lines, which name temporary files, left out.  Each session's
# answers are compared with off's, test by test; a difference fails the run
# unless test/corpus-kept/<session>/<test>.diff holds it, reviewed.  Each
# session's tests are dealt out among PACK_SHARDS servers, each run_test.pl
# making its own database.
set -u
. /src/test/vexec/lib.sh
export PATH="$BINDIR:$PATH"

TREE="${POSTGIS_BUILD_TREE:-/postgis}"
[ -f "$TREE/regress/run_test.pl" ] && [ -f "$TREE/regress/port-regress.txt" ] \
	|| { echo "no PostGIS build tree at $TREE"; exit 1; }
SHARDS="${PACK_SHARDS:-4}"
KEPT="/pack/test/corpus-kept"
W="$(mktemp -d "${TMPDIR:-/tmp}/pack-corpus-XXXXXX")"
SOCK="$(mktemp -d /tmp/vpg-XXXXXX)"
PORT=$((7700 + RANDOM % 200))

cleanup() {
	for k in $(seq 1 "$SHARDS"); do
		"$BINDIR/pg_ctl" -D "$W/s$k/data" -m immediate stop > /dev/null 2>&1
	done
	[ -n "${RESULTS_DIR:-}" ] && cp -r "$W/answers" "$RESULTS_DIR/corpus-answers" 2> /dev/null
	rm -rf "$W" "$SOCK"
}
trap cleanup EXIT

# the core tests, and the flags and hooks that are theirs
{ read -r FLAGS; read -r HOOKS; read -r TESTS; } < "$TREE/regress/port-regress.txt"
FLAGS="--extension"
HOOKS="$(echo "$HOOKS" | grep -oE -- '--[a-z-]+-script +\./regress/hooks/[^ ]+' | grep -v upgrade | tr '\n' ' ')"
TESTS="$(echo "$TESTS" | tr ' ' '\n' | grep '^\./regress/core/' | tr '\n' ' ')"
echo "PostGIS's core tests: $(echo "$TESTS" | wc -w), PostGIS $(cat "$TREE/.postgis_commit" 2> /dev/null || echo '?'), over $SHARDS servers"

servers() {					# servers <preload>: SHARDS servers, made anew and started
	local k
	for k in $(seq 1 "$SHARDS"); do
		"$BINDIR/pg_ctl" -D "$W/s$k/data" -m fast stop > /dev/null 2>&1
		rm -rf "$W/s$k"
		mkdir -p "$W/s$k" "$SOCK/$k"
		"$BINDIR/initdb" -D "$W/s$k/data" -N --locale=C --encoding=UTF8 -U postgres > "$W/s$k/initdb.log" 2>&1 \
			|| { echo "initdb failed for server $k"; return 1; }
		{
			echo "unix_socket_directories = '$SOCK/$k'"
			echo "listen_addresses = ''"
			echo "port = $PORT"
			echo "fsync = off"
			echo "shared_preload_libraries = '$1'"
		} >> "$W/s$k/data/postgresql.conf"
		"$BINDIR/pg_ctl" -D "$W/s$k/data" -l "$W/s$k/log" -w -t 60 start > /dev/null 2>&1 \
			|| { echo "server $k did not start"; tail -20 "$W/s$k/log"; return 1; }
	done
}

session() {					# session <name> <options>: run, and keep its answers
	local name="$1" options="$2" k i=0 t
	local -a shard=()
	for t in $TESTS; do
		shard[$((i % SHARDS + 1))]+=" $t"
		i=$((i + 1))
	done
	mkdir -p "$W/$name" "$W/answers/$name"
	for k in $(seq 1 "$SHARDS"); do
		# shellcheck disable=SC2086 -- the flags, hooks and tests are lists
		( cd "$TREE" &&
		  PGHOST="$SOCK/$k" PGPORT="$PORT" PGUSER=postgres PGOPTIONS="$options" \
		  POSTGIS_TOP_BUILD_DIR="$TREE" PGIS_REG_TMPDIR="$W/$name/tmp$k" POSTGIS_REGRESS_DB="postgis_reg" \
			perl regress/run_test.pl $FLAGS $HOOKS ${shard[$k]} ) > "$W/$name/run_test.$k.out" 2>&1 &
	done
	wait
	cat "$W/$name"/run_test.*.out > "$W/$name/run_test.out"
	# each failing test's difference, its headers left out
	sed -nE 's#^ *(\./)?([^ ]+) \.\. failed \(diff expected obtained: ([^)]+)\)#\2 \3#p' "$W/$name/run_test.out" |
		while read -r test diff; do
			mkdir -p "$W/answers/$name/$(dirname "$test")"
			tail -n +3 "$diff" > "$W/answers/$name/$test.diff"
		done
	grep -E " failed \(" "$W/$name/run_test.out" | grep -v "diff expected obtained" |
		sed -E 's#^ *(\./)?([^ ]+) \.\. (.*)$#\2 \3#' | while read -r test rest; do
			mkdir -p "$W/answers/$name/$(dirname "$test")"
			echo "$rest" > "$W/answers/$name/$test.diff"
		done
	local run bad
	run=$(sed -n 's/^Run tests: //p' "$W/$name"/run_test.*.out | awk '{s += $1} END {print s}')
	bad=$(sed -n 's/^Failed: //p' "$W/$name"/run_test.*.out | awk '{s += $1} END {print s}')
	echo "  $name: ${run:-?} run, ${bad:-?} failed against PostGIS's expected output"
}

servers "vexec" || exit 1
session off "-c vexec.mode=off"
session postgres "-c vexec.mode=force -c vexec.batch_format=postgres"
session arrow "-c vexec.mode=force -c vexec.batch_format=arrow"
servers "vexec_postgis,vexec" || exit 1
session postgres-pack "-c vexec.mode=force -c vexec.batch_format=postgres"
session arrow-pack "-c vexec.mode=force -c vexec.batch_format=arrow"

fail=0
for s in postgres arrow postgres-pack arrow-pack; do
	same=0; kept=0; differ=0
	for t in $TESTS; do
		t="${t#./}"
		a="$W/answers/off/$t.diff"
		b="$W/answers/$s/$t.diff"
		if { [ ! -f "$a" ] && [ ! -f "$b" ]; } || { [ -f "$a" ] && [ -f "$b" ] && cmp -s "$a" "$b"; }; then
			same=$((same + 1))
		elif [ -f "$KEPT/$s/$t.diff" ] && diff <(cat "$a" 2> /dev/null) <(cat "$b" 2> /dev/null) | cmp -s - "$KEPT/$s/$t.diff"; then
			kept=$((kept + 1))
		else
			differ=$((differ + 1))
			echo "  DIFFERS: $s, $t"
			diff <(cat "$a" 2> /dev/null) <(cat "$b" 2> /dev/null) | head -20 | sed 's/^/      /'
		fi
	done
	echo "  $s: $same the same as off, $kept kept differences, $differ differ"
	[ "$differ" -eq 0 ] || fail=1
done
exit $fail
