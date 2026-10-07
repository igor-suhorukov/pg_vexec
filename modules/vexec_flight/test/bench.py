#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# TPC-H Q1 through adbc_driver_flightsql against adbc_driver_postgresql, which
# reads binary COPY, for the same query on the same server
# (pg_vector_executor.md §5, V10, "Cost"), run by flight.sh's bench:
#
#   the data      lineitem at FLIGHT_SF, from DuckDB's tpch extension as the
#                 port pins it (/opt/duckdb), loaded by COPY and analysed
#   the sessions  adbc_driver_postgresql with vexec.mode off -- PostgreSQL as
#                 it is -- and auto; adbc_driver_flightsql, auto and force
#   the measure   each query's wall time from execute to the whole Arrow
#                 table in the client, a warm run first, then FLIGHT_RUNS runs:
#                 their median and spread; and the answers, equal
#
# Also a query whose result is large, lineitem's first FLIGHT_ROWS rows, for
# what the transports cost.

import os
import statistics
import subprocess
import sys
import tempfile
import time

import adbc_driver_flightsql.dbapi as fsql
import adbc_driver_postgresql.dbapi as pgdb

PORT = int(os.environ['FLIGHT_PORT'])
SF = float(os.environ.get('FLIGHT_SF', '1'))
RUNS = int(os.environ.get('FLIGHT_RUNS', '5'))
ROWS = int(os.environ.get('FLIGHT_ROWS', '1000000'))
PG_URI = 'postgresql:///flight?host=%s&port=%s&user=postgres' % (os.environ['PGHOST'], os.environ['PGPORT'])
PASSWORD = 'bench-pass-0123456789'

Q1 = """SELECT l_returnflag, l_linestatus,
       sum(l_quantity) AS sum_qty,
       sum(l_extendedprice) AS sum_base_price,
       sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
       sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
       avg(l_quantity) AS avg_qty,
       avg(l_extendedprice) AS avg_price,
       avg(l_discount) AS avg_disc,
       count(*) AS count_order
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus"""

LINEITEM = """CREATE TABLE lineitem (
    l_orderkey bigint NOT NULL, l_partkey bigint NOT NULL, l_suppkey bigint NOT NULL,
    l_linenumber integer NOT NULL, l_quantity numeric(15,2) NOT NULL,
    l_extendedprice numeric(15,2) NOT NULL, l_discount numeric(15,2) NOT NULL,
    l_tax numeric(15,2) NOT NULL, l_returnflag char(1) NOT NULL, l_linestatus char(1) NOT NULL,
    l_shipdate date NOT NULL, l_commitdate date NOT NULL, l_receiptdate date NOT NULL,
    l_shipinstruct char(25) NOT NULL, l_shipmode char(10) NOT NULL, l_comment varchar(44) NOT NULL)"""


def psql(sql):
    r = subprocess.run(['psql', '-X', '-q', '-At', '-v', 'ON_ERROR_STOP=1', '-d', 'flight', '-c', sql],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('psql: %s' % r.stderr.strip())
    return r.stdout.strip()


def load():
    d = tempfile.mkdtemp(prefix='tpch-')
    csv = os.path.join(d, 'lineitem.csv')
    gen = ("import duckdb\n"
           "con = duckdb.connect(config={'extension_directory': '/opt/duckdb/extensions'})\n"
           "con.execute('LOAD tpch')\n"
           "con.execute('CALL dbgen(sf=%s)')\n"
           "con.execute(\"COPY lineitem TO '%s' (FORMAT csv, DELIMITER '|', HEADER false)\")\n" % (SF, csv))
    t = time.time()
    subprocess.run(['/opt/duckdb/bin/python', '-c', gen], check=True)
    psql(LINEITEM)
    psql("COPY lineitem FROM '%s' (FORMAT csv, DELIMITER '|')" % csv)
    psql('VACUUM ANALYZE lineitem')
    os.unlink(csv)
    print('lineitem at SF %s: %s rows, loaded in %.1f s'
          % (SF, psql('SELECT count(*) FROM lineitem'), time.time() - t))
    psql("CREATE ROLE bench LOGIN PASSWORD '%s'; GRANT SELECT ON lineitem TO bench" % PASSWORD)


def timed(connect, settings, sql):
    times = []
    table = None
    with connect() as conn:
        with conn.cursor() as cur:
            for s in settings:
                cur.execute(s)
            for i in range(RUNS + 1):
                t = time.perf_counter()
                cur.execute(sql)
                table = cur.fetch_arrow_table()
                if i > 0:
                    times.append(time.perf_counter() - t)
    return times, table


def pg():
    return pgdb.connect(PG_URI, autocommit=True)


def flight(mode):
    return fsql.connect('grpc://127.0.0.1:%d' % PORT, autocommit=True,
                        db_kwargs={'username': 'bench', 'password': PASSWORD,
                                   'adbc.flight.sql.rpc.call_header.database': 'flight'},
                        conn_kwargs={'adbc.flight.sql.session.option.vexec.mode': mode})


def answers(want, got, name):
    """the same rows, each column's values as text: numeric goes out as text
    both ways, as its output function writes it"""
    def text(t):
        return [[None if v is None else str(v) for v in t.column(i).to_pylist()] for i in range(t.num_columns)]
    ok = want.column_names == got.column_names and text(want) == text(got)
    if not ok:
        print('  %s answers differently:\n    %s\n    %s' % (name, want.to_pylist()[:2], got.to_pylist()[:2]))
    return ok


def report(name, times, base=None):
    med = statistics.median(times)
    print('  %-48s median %8.3f s  [%.3f .. %.3f]%s'
          % (name, med, min(times), max(times),
             '' if base is None else '  %.2fx of PostgreSQL as it is' % (base / med)))
    return med


def cpu_quota():
    """docker's --cpus, as the container's cgroup has it"""
    try:
        quota, period = open('/sys/fs/cgroup/cpu.max').read().split()
        return 'all %d' % os.cpu_count() if quota == 'max' else '%g' % (int(quota) / int(period))
    except (OSError, ValueError):
        return '?'


def main():
    print('the server: %s, debug_assertions %s, %d segments; the container\'s CPUs: %s'
          % (psql('SHOW server_version'), psql('SHOW debug_assertions'),
             int(os.environ.get('FLIGHT_SEGMENTS', '0')), cpu_quota()))
    load()
    print('TPC-H Q1, %d runs each after a warm one:' % RUNS)
    t_pg, a = timed(pg, ['SET vexec.mode = off'], Q1)
    base = report('adbc_driver_postgresql, vexec.mode off', t_pg)
    t_pga, b = timed(pg, ['SET vexec.mode = auto'], Q1)
    report('adbc_driver_postgresql, vexec.mode auto', t_pga, base)
    t_fl, c = timed(lambda: flight('auto'), [], Q1)
    fl_med = report('adbc_driver_flightsql, vexec.mode auto', t_fl, base)
    t_flf, d = timed(lambda: flight('force'), [], Q1)
    report('adbc_driver_flightsql, vexec.mode force', t_flf, base)
    same = all([answers(a, b, 'adbc_driver_postgresql auto'), answers(a, c, 'adbc_driver_flightsql auto'),
                answers(a, d, 'adbc_driver_flightsql force')])
    print('  the answers: %d rows each%s' % (c.num_rows, ', equal' if same else ', and they DIFFER'))

    print('lineitem\'s first %d rows, %d runs each after a warm one:' % (ROWS, RUNS))
    big = 'SELECT * FROM lineitem LIMIT %d' % ROWS
    t_pg2, _ = timed(pg, ['SET vexec.mode = off'], big)
    base2 = report('adbc_driver_postgresql, vexec.mode off', t_pg2)
    t_fl2, _ = timed(lambda: flight('auto'), [], big)
    report('adbc_driver_flightsql, vexec.mode auto', t_fl2, base2)

    ok = same and fl_med < base
    print('Q1 through adbc_driver_flightsql is %s than through adbc_driver_postgresql'
          % ('faster' if fl_med < base else 'NOT faster'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
