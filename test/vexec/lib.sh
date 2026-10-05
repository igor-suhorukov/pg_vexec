# SPDX-License-Identifier: Apache-2.0
#
# What test/vexec's scripts share, sourced inside a vexec-dev container: a
# server of the container's PostgreSQL, made and started in a directory of
# its own, listening on a socket in it only.
#
#   server_init <dir> [setting...]   initdb, and the settings appended
#   server_start <dir>               start it; 0 if it came up
#   server_stop <dir>
#   q <dir> <db> <sql>               psql -At, errors included

BINDIR="${PG_BINDIR:-$(dirname "$(command -v pg_config || echo /usr/local/pgsql/bin/pg_config)")}"
export LD_LIBRARY_PATH="$("$BINDIR/pg_config" --libdir)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

server_init() {
	local dir="$1"; shift
	mkdir -p "$dir/sock"
	"$BINDIR/initdb" -D "$dir/data" -N -U postgres --locale=C --encoding=UTF8 > "$dir/initdb.log" 2>&1 \
		|| { echo "initdb failed"; tail -20 "$dir/initdb.log"; return 1; }
	{
		echo "unix_socket_directories = '$dir/sock'"
		echo "listen_addresses = ''"
		echo "fsync = off"
		echo "max_prepared_transactions = 2"
		local s
		for s in "$@"; do echo "$s"; done
	} >> "$dir/data/postgresql.conf"
}

server_start() {
	"$BINDIR/pg_ctl" -D "$1/data" -l "$1/log" -w -t 60 start > /dev/null 2>&1
}

server_stop() {
	"$BINDIR/pg_ctl" -D "$1/data" -m fast -w stop > /dev/null 2>&1
}

q() {
	"$BINDIR/psql" -X -q -At -h "$1/sock" -U postgres -d "$2" -c "$3" 2>&1
}
