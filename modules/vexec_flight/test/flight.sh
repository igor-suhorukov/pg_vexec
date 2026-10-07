#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# vexec_flight's leg, inside a vexec_flight/dev container (run.sh): vexec,
# from pg_accel's tree at /src, and vexec_flight, from /flight, built by PGXS
# and installed into the container's server; a server with both preloaded --
# on the port leg, the port's coordinator and FLIGHT_SEGMENTS segments, the
# Flight endpoint on the coordinator; certificates, roles and pg_hba.conf
# lines for the logins' tests; then
#
#   flight.sh test    the clients' tests (test_flight.py, FlightJdbcTest.java)
#   flight.sh bench   TPC-H Q1 at FLIGHT_SF, through adbc_driver_flightsql and
#                     adbc_driver_postgresql (bench.py)
#   flight.sh ingest  lineitem into PAX tables, through DoPut's parameters,
#                     adbc_driver_postgresql's binary COPY and a CSV file's
#                     COPY (ingest.py)
#   flight.sh serve   the test's server and corpus, its environment in
#                     RESULTS_DIR/env, until the container is stopped
#
# The server's log and the tests' outputs go to RESULTS_DIR.
set -u
CMD="${1:-test}"
[ $# -gt 0 ] && shift
SEGMENTS="${FLIGHT_SEGMENTS:-0}"
[ "${FLIGHT_LEG:-vanilla}" = vanilla ] && SEGMENTS=0

/src/test/vexec/build.sh || exit 1
rm -rf /tmp/flight-build
cp -a /flight /tmp/flight-build
PG_CONFIG="$(command -v pg_config)"
make -s -C /tmp/flight-build PG_CONFIG="$PG_CONFIG" COPT=-Werror -j4 2>&1 | grep -v "protoc-c. is deprecated"
[ "${PIPESTATUS[0]}" -eq 0 ] || { echo "vexec_flight did not build"; exit 1; }
make -s -C /tmp/flight-build PG_CONFIG="$PG_CONFIG" install > /dev/null || exit 1
echo "  vexec_flight built and installed"

. /src/test/vexec/lib.sh

D="$(mktemp -d "${TMPDIR:-/tmp}/flight-XXXXXX")"
SOCK="$(mktemp -d /tmp/vxf-XXXXXX)"
FLIGHT_PORT=$((32000 + RANDOM % 2000))
BASEPORT=$((7600 + RANDOM % 200))
SECRET="vexec-$(od -An -tx8 -N16 /dev/urandom | tr -d ' \n')"	# gp_core takes 16 characters at least
NODES="$(seq 0 "$SEGMENTS")"
datadir() { echo "$D/node$1"; }
sockdir() { echo "$SOCK/n$1"; }
nodeport() { echo $((BASEPORT + $1)); }

cleanup() {
	for n in $NODES; do
		[ -n "${RESULTS_DIR:-}" ] && cp "$D/node$n.log" "$RESULTS_DIR/server-node$n.log" 2> /dev/null
		"$BINDIR/pg_ctl" -D "$(datadir "$n")" -m immediate stop > /dev/null 2>&1
	done
	rm -rf "$D" "$SOCK"
}
trap cleanup EXIT

# a CA, and a certificate of the server for 127.0.0.1 and localhost
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$D/ca.key" -out "$D/ca.crt" -days 2 \
	-subj "/CN=vexec_flight test CA" > /dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -keyout "$D/server.key" -out "$D/server.csr" -subj "/CN=localhost" > /dev/null 2>&1
printf 'subjectAltName=DNS:localhost,IP:127.0.0.1\n' > "$D/san.ext"
openssl x509 -req -in "$D/server.csr" -CA "$D/ca.crt" -CAkey "$D/ca.key" -CAcreateserial \
	-out "$D/server.crt" -days 2 -extfile "$D/san.ext" > /dev/null 2>&1 || { echo "no certificate"; exit 1; }
chmod 600 "$D/server.key"

if [ "$SEGMENTS" -gt 0 ]; then
	PRELOAD="gp_core,gp_orca,gp_sql,gp_ao,pax,vexec"
	CONF="$D/gp_cluster.conf"
	for n in $NODES; do
		echo "$((n + 1)) $((n - 1)) p $(sockdir "$n") $(nodeport "$n") $(datadir "$n")"
	done > "$CONF"
else
	PRELOAD="vexec"
	[ "${FLIGHT_LEG:-vanilla}" = port ] && PRELOAD="gp_core,gp_orca,gp_sql,gp_ao,pax,vexec"
fi
for n in $NODES; do
	mkdir -p "$(sockdir "$n")"
	"$BINDIR/initdb" -D "$(datadir "$n")" -N -U postgres --locale=C --encoding=UTF8 > "$D/initdb$n.log" 2>&1 \
		|| { echo "initdb failed for node $n"; tail -20 "$D/initdb$n.log"; exit 1; }
	{
		echo "unix_socket_directories = '$(sockdir "$n")'"
		echo "listen_addresses = ''"
		echo "port = $(nodeport "$n")"
		echo "fsync = off"
		echo "max_worker_processes = 48"
		echo "max_connections = 100"
		echo "password_encryption = 'scram-sha-256'"
		echo "vexec.mode = 'auto'"
		if [ "$n" -eq 0 ]; then
			# the server's own TLS, which a clientcert line of pg_hba.conf needs
			echo "ssl = on"
			echo "ssl_cert_file = '$D/server.crt'"
			echo "ssl_key_file = '$D/server.key'"
			echo "ssl_ca_file = '$D/ca.crt'"
			echo "shared_preload_libraries = '$PRELOAD,vexec_flight'"
			echo "vexec_flight.listen_addresses = '127.0.0.1'"
			echo "vexec_flight.port = $FLIGHT_PORT"
			echo "vexec_flight.ssl_cert_file = '$D/server.crt'"
			echo "vexec_flight.ssl_key_file = '$D/server.key'"
			echo "vexec_flight.max_sessions = 12"
		else
			echo "shared_preload_libraries = '$PRELOAD'"
		fi
		if [ "$SEGMENTS" -gt 0 ]; then
			echo "gp.cluster_config = '$CONF'"
			echo "gp.dbid = $((n + 1))"
			echo "gp.cluster_secret = '$SECRET'"
			echo "max_prepared_transactions = 100"
			[ "$n" -eq 0 ] && echo "gp.role = 'dispatch'"
		fi
	} >> "$(datadir "$n")/postgresql.auto.conf"
done

# the logins' lines (test_flight.py's roles)
cat > "$(datadir 0)/pg_hba.conf" <<'EOF'
local   all  all                         trust
host    all  reject_user   127.0.0.1/32  reject
hostssl all  ssl_user      127.0.0.1/32  scram-sha-256
hostnossl all ssl_user     127.0.0.1/32  reject
host    all  md5_user      127.0.0.1/32  md5
host    all  pw_user       127.0.0.1/32  password
host    all  trust_user    127.0.0.1/32  trust
hostssl all  cert_user     127.0.0.1/32  scram-sha-256  clientcert=verify-ca
host    all  all           127.0.0.1/32  scram-sha-256
EOF

for n in $(echo $NODES | tr ' ' '\n' | sort -rn); do
	"$BINDIR/pg_ctl" -D "$(datadir "$n")" -l "$D/node$n.log" -w -t 120 start > /dev/null 2>&1 \
		|| { echo "node $n did not start:"; tail -30 "$D/node$n.log"; exit 1; }
done
export PGHOST="$(sockdir 0)" PGPORT="$(nodeport 0)" PGUSER=postgres
sql() { "$BINDIR/psql" -X -q -At -v ON_ERROR_STOP=1 -d "$1" -c "$2" 2>&1; }

if [ "$SEGMENTS" -gt 0 ] || [ "${FLIGHT_LEG:-vanilla}" = port ]; then
	for db in template1 postgres; do
		out=$(sql "$db" "SET client_min_messages = warning; CREATE EXTENSION gp_core")
		[ -n "$out" ] && { echo "gp_core in $db: $out"; exit 1; }
	done
fi
out=$(sql postgres "CREATE DATABASE flight")
[ -n "$out" ] && { echo "CREATE DATABASE: $out"; exit 1; }
if [ "$SEGMENTS" -gt 0 ] || [ "${FLIGHT_LEG:-vanilla}" = port ]; then
	out=$(sql flight "SET client_min_messages = warning; CREATE EXTENSION gp_sql; CREATE EXTENSION gp_ao; CREATE EXTENSION pax")
	[ -n "$out" ] && { echo "the port's extensions: $out"; exit 1; }
fi

export FLIGHT_PORT FLIGHT_CA="$D/ca.crt" FLIGHT_DATA="$(datadir 0)" FLIGHT_LOG="$D/node0.log"
export FLIGHT_SEGMENTS="$SEGMENTS" FLIGHT_NODEPORT0="$(nodeport 0)"
# what a scratch server of test_flight.py preloads before vexec: the port's modules on the port
FLIGHT_PRELOAD_BASE=""
[ "${FLIGHT_LEG:-vanilla}" = port ] && FLIGHT_PRELOAD_BASE="gp_core,gp_orca,gp_sql,gp_ao,pax"
export FLIGHT_PRELOAD_BASE
export PATH="$BINDIR:$PATH"

# the acceptor is up when its port takes a connection
for i in $(seq 1 100); do
	(exec 3<> "/dev/tcp/127.0.0.1/$FLIGHT_PORT") 2> /dev/null && break
	sleep 0.1
done

corpus() {
	"$BINDIR/psql" -X -q -d flight -v ON_ERROR_STOP=1 -f /src/modules/vexec/sql/vexec_corpus_setup.sql > "$D/corpus.log" 2>&1 \
		|| { echo "the corpus did not load:"; tail -20 "$D/corpus.log"; exit 1; }
}

case "$CMD" in
	serve)
		corpus
		export -p | grep -E ' (PGHOST|PGPORT|PGUSER|PATH|FLIGHT_[A-Z0-9_]*)=' > "$RESULTS_DIR/env"
		echo "serving on $FLIGHT_PORT; the server's log: $FLIGHT_LOG"
		trap 'exit 0' TERM
		while :; do sleep 1; done
		;;
	test)
		rc=0
		corpus
		python3 /flight/test/test_flight.py ${FLIGHT_TESTS:-} 2>&1 | tee "${RESULTS_DIR:-/tmp}/test_flight.out"
		[ "${PIPESTATUS[0]}" -eq 0 ] || rc=1
		bash /flight/test/jdbc/run.sh 2>&1 | tee "${RESULTS_DIR:-/tmp}/jdbc.out"
		[ "${PIPESTATUS[0]}" -eq 0 ] || rc=1
		echo
		echo "vexec_flight: $([ $rc -eq 0 ] && echo "every test passed" || echo "a test FAILED")"
		exit $rc
		;;
	bench)
		python3 /flight/test/bench.py
		exit $?
		;;
	ingest)
		python3 /flight/test/ingest.py
		exit $?
		;;
	*)
		echo "flight.sh: test, bench, ingest or serve"
		exit 2
		;;
esac
