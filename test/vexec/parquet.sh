#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# vexec_parquet's leg: the table access method of Parquet files read and
# written by Apache Arrow C++ alone (modules/vexec_parquet), with vexec's
# batch source and sink, on vanilla PostgreSQL 19, in a container of
# docker/Dockerfile.parquet's image -- Arrow C++ with Parquet and S3, pyarrow,
# and moto's S3 server:
#
#   suite         vexec's own regression suite, vexec_parquet preloaded
#                 beside vexec
#   parquet       the module's suite (test/sql), in an instance of its own
#                 with both preloaded: its tests of types, writes, MVCC,
#                 options, tables that read files, vexec and fetches by TID;
#                 external_arrow over files pyarrow writes
#                 (test/data/make_files.py) and encodings, of their text in
#                 databases not UTF-8; geo, GeoParquet's files read as
#                 PostGIS's types, where the image has PostGIS; nested,
#                 lists and maps read as arrays, hstore and jsonb; s3
#                 over moto's server on the loopback; and its isolation specs
#                 (test/specs) -- concurrent writers, row locks, upserts,
#                 SERIALIZABLE
#   differential  the differential runner's sessions over tables of their own
#                 files, PostgreSQL's schedule and vexec's corpus
#                 (differential.sh's pg_parquet and vexec_parquet)
#
#   PARQUET_PARTS  the parts run: "suite parquet differential"
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
pkg-config --exists arrow parquet arrow-s3 ||
	{ echo "no Arrow C++ with Parquet and S3 here: docker/Dockerfile.parquet's image"; exit 1; }
"$here/build.sh" > "${RESULTS_DIR:-/tmp}/vexec-build.log" 2>&1 || { tail -30 "${RESULTS_DIR:-/tmp}/vexec-build.log"; exit 1; }
. "$here/lib.sh"
BUILD="${VEXEC_BUILD:-/tmp/vexec-build}"
MOD="$BUILD/modules/vexec_parquet"
PYTHON="${VPQ_PYTHON:-/opt/arrow/bin/python}"
echo "vexec and vexec_parquet on $("$BINDIR/pg_config" --version), Arrow C++ $(pkg-config --modversion arrow)"

# moto's S3 server on the loopback, its bucket vpq-test
s3_up() {
	"$(dirname "$PYTHON")/moto_server" -H 127.0.0.1 -p 9000 > "$W/moto.log" 2>&1 &
	MOTO_PID=$!
	for _ in $(seq 100); do
		"$PYTHON" -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:9000/')" 2> /dev/null && break
		sleep 0.2
	done
	AWS_ACCESS_KEY_ID=test AWS_SECRET_ACCESS_KEY=test "$PYTHON" -I -c "
import boto3
s3 = boto3.client('s3', endpoint_url='http://127.0.0.1:9000', region_name='us-east-1')
if 'vpq-test' not in [b['Name'] for b in s3.list_buckets()['Buckets']]:
    s3.create_bucket(Bucket='vpq-test')"
}

W="$(mktemp -d "${TMPDIR:-/tmp}/vexec-parquet-XXXXXX")"
MOTO_PID=""
trap '[ -n "$MOTO_PID" ] && kill "$MOTO_PID" 2> /dev/null' EXIT
PG_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/regress/pg_regress"
PG_ISOLATION_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/isolation/pg_isolation_regress"
rc=0
for part in ${PARQUET_PARTS:-suite parquet differential}; do
	case "$part" in
		suite)
			echo "== vexec's suite, vexec_parquet preloaded beside vexec"
			VEXEC_PRELOAD=vexec_testpack,vexec,vexec_test,vexec_parquet "$here/suite.sh" > "${RESULTS_DIR:-/tmp}/suite.out" 2>&1
			r=$?
			grep -E "^(ok|not ok)|^# (All|[0-9]+ of)" "${RESULTS_DIR:-/tmp}/suite.out" | grep -E "not ok|^#"
			[ $r -eq 0 ] || { rc=1; tail -60 "${RESULTS_DIR:-/tmp}/suite.out"; }
			;;
		parquet)
			echo "== vexec_parquet's suite"
			"$PYTHON" -I "$MOD/test/data/make_files.py" "$W/data" > /dev/null || { rc=1; continue; }
			s3_up || { rc=1; continue; }
			geo=""
			if [ -f "$("$BINDIR/pg_config" --sharedir)/extension/postgis.control" ]; then
				geo=geo
			else
				echo "   geo: not run, no PostGIS here"
			fi
			( cd "$MOD/test" && VPQ_TEST_DATA="$W/data" "$PG_REGRESS" \
				--temp-instance="$W/instance" --temp-config="$MOD/test/regress.conf" --bindir="$BINDIR" \
				--inputdir="$MOD/test" --expecteddir="$MOD/test" --outputdir="$W/regress" \
				--encoding=UTF8 --no-locale \
				setup types dml mvcc options external vexec otel fetch footers cpu external_arrow encodings $geo nested s3 ) > "$W/regress.out" 2>&1
			r=$?
			grep -E "^(ok|not ok)|^# (All|[0-9]+ of)" "$W/regress.out" | grep -E "not ok|^#"
			[ $r -eq 0 ] || { rc=1; head -200 "$W/regress/regression.diffs"; }
			( cd "$MOD/test" && "$PG_ISOLATION_REGRESS" \
				--temp-instance="$W/iso-instance" --temp-config="$MOD/test/regress.conf" --bindir="$BINDIR" \
				--inputdir="$MOD/test" --expecteddir="$MOD/test" --outputdir="$W/isolation" \
				--encoding=UTF8 --no-locale --load-extension=vexec_parquet \
				vpq_update vpq_locks vpq_upsert vpq_serializable ) > "$W/isolation.out" 2>&1
			r=$?
			grep -E "^(ok|not ok)|^# (All|[0-9]+ of)" "$W/isolation.out" | grep -E "not ok|^#"
			[ $r -eq 0 ] || { rc=1; head -200 "$W/isolation/regression.diffs"; }
			kill "$MOTO_PID" 2> /dev/null; MOTO_PID=""
			if [ -n "${RESULTS_DIR:-}" ]; then
				mkdir -p "$RESULTS_DIR/parquet"
				cp -r "$W/regress" "$W/isolation" "$W/regress.out" "$W/isolation.out" "$RESULTS_DIR/parquet/" 2> /dev/null
			fi
			;;
		differential)
			echo "== the differential runner over tables of their own files"
			VEXEC_CORPORA="pg_parquet vexec_parquet" "$here/differential.sh" || rc=1
			;;
		*)
			echo "no part $part"; rc=1 ;;
	esac
done
echo
echo "parquet: $([ $rc -eq 0 ] && echo "every part passed" || echo "a part FAILED")"
exit $rc
