#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Arrow data into a PAX table through the paths a client has today
# (pg_vector_executor.md §3.16: VI's baseline), run by flight.sh's ingest:
#
#   flight   adbc_driver_flightsql's executemany(): a prepared INSERT whose
#            parameters go by DoPut as Arrow record batches
#            (CommandPreparedStatementUpdate) -- before VI a statement a row,
#            since VI one INSERT ... SELECT over vexec's ingest stream
#   ingest   adbc_driver_flightsql's adbc_ingest(): Flight SQL's bulk
#            ingestion (CommandStatementIngest), from VI on
#   copy     adbc_driver_postgresql's adbc_ingest(): the same Arrow table as
#            COPY FROM STDIN (FORMAT binary)
#   csv      COPY FROM a CSV file of the same rows, read by the server: the
#            way the tpc and ClickBench suites load
#
#   the data     lineitem at FLIGHT_SF from DuckDB's tpch extension, ordered by
#                its key, its first FLIGHT_INGEST_ROWS rows: one Arrow table in
#                the client, in batches of FLIGHT_INGEST_BATCH rows, and a CSV
#                file of the same rows
#   the tables   in each of FLIGHT_INGEST_FORMATS -- PAX's porc_vec and porc,
#                ao_column ("aoco"), heap -- made afresh before each load;
#                DISTRIBUTED BY (l_orderkey) on a cluster
#   the measure  each load's wall time in the client, from the call to its
#                commit; after a small warm load of each path, FLIGHT_RUNS
#                loads of each path and format, the paths in turn: their
#                median, spread and rows a second; each table checked
#                against the csv load's by its count and sums
#
# The results also go to RESULTS_DIR/ingest.json.

import json
import os
import statistics
import subprocess
import sys
import tempfile
import time

import pyarrow as pa
import pyarrow.csv as pacsv
import adbc_driver_flightsql.dbapi as fsql
import adbc_driver_postgresql.dbapi as pgdb

PORT = int(os.environ['FLIGHT_PORT'])
SF = float(os.environ.get('FLIGHT_SF', '1'))
RUNS = int(os.environ.get('FLIGHT_RUNS', '3'))
ROWS = int(os.environ.get('FLIGHT_INGEST_ROWS', '1000000'))
BATCH = int(os.environ.get('FLIGHT_INGEST_BATCH', '65536'))
WARM = int(os.environ.get('FLIGHT_INGEST_WARM', '10000'))
FORMATS = os.environ.get('FLIGHT_INGEST_FORMATS', 'porc_vec porc').split()
PATHS = os.environ.get('FLIGHT_INGEST_PATHS', 'csv copy flight').split()
SEGMENTS = int(os.environ.get('FLIGHT_SEGMENTS', '0'))
PG_URI = 'postgresql:///flight?host=%s&port=%s&user=postgres' % (os.environ['PGHOST'], os.environ['PGPORT'])
ROLE = 'ingest'
PASSWORD = 'ingest-pass-0123456789'

# lineitem: each column's name, PostgreSQL type and Arrow type
COLUMNS = [
    ('l_orderkey', 'bigint', pa.int64()),
    ('l_partkey', 'bigint', pa.int64()),
    ('l_suppkey', 'bigint', pa.int64()),
    ('l_linenumber', 'integer', pa.int32()),
    ('l_quantity', 'numeric(15,2)', pa.decimal128(15, 2)),
    ('l_extendedprice', 'numeric(15,2)', pa.decimal128(15, 2)),
    ('l_discount', 'numeric(15,2)', pa.decimal128(15, 2)),
    ('l_tax', 'numeric(15,2)', pa.decimal128(15, 2)),
    ('l_returnflag', 'char(1)', pa.string()),
    ('l_linestatus', 'char(1)', pa.string()),
    ('l_shipdate', 'date', pa.date32()),
    ('l_commitdate', 'date', pa.date32()),
    ('l_receiptdate', 'date', pa.date32()),
    ('l_shipinstruct', 'char(25)', pa.string()),
    ('l_shipmode', 'char(10)', pa.string()),
    ('l_comment', 'varchar(44)', pa.string()),
]

# a loaded table's fingerprint: its count, and each column's sum
CHECK = ("SELECT count(*), sum(l_orderkey), sum(l_partkey), sum(l_suppkey), sum(l_linenumber), "
         "sum(l_quantity), sum(l_extendedprice), sum(l_discount), sum(l_tax), "
         "sum(ascii(l_returnflag)), sum(ascii(l_linestatus)), "
         "sum(l_shipdate - DATE '1990-01-01'), sum(l_commitdate - DATE '1990-01-01'), "
         "sum(l_receiptdate - DATE '1990-01-01'), sum(hashtext(l_shipinstruct::text)::bigint), "
         "sum(hashtext(l_shipmode::text)::bigint), sum(hashtext(l_comment)::bigint) FROM %s")


def psql(sql):
    r = subprocess.run(['psql', '-X', '-q', '-At', '-v', 'ON_ERROR_STOP=1', '-d', 'flight', '-c', sql],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError('psql: %s' % r.stderr.strip())
    return r.stdout.strip()


def data(dirname):
    """lineitem's first ROWS rows, by its key: a CSV file, and the same rows
    as an Arrow table in batches of BATCH rows"""
    csv = os.path.join(dirname, 'lineitem.csv')
    gen = ("import duckdb\n"
           "con = duckdb.connect(config={'extension_directory': '/opt/duckdb/extensions'})\n"
           "con.execute('LOAD tpch')\n"
           "con.execute('CALL dbgen(sf=%s)')\n"
           "con.execute(\"COPY (SELECT * FROM lineitem ORDER BY l_orderkey, l_linenumber LIMIT %d) "
           "TO '%s' (FORMAT csv, DELIMITER '|', HEADER false)\")\n" % (SF, ROWS, csv))
    subprocess.run(['/opt/duckdb/bin/python', '-c', gen], check=True)
    table = pacsv.read_csv(csv,
                           read_options=pacsv.ReadOptions(column_names=[c[0] for c in COLUMNS]),
                           parse_options=pacsv.ParseOptions(delimiter='|'),
                           convert_options=pacsv.ConvertOptions(column_types={c[0]: c[2] for c in COLUMNS}))
    table = pa.Table.from_batches(table.combine_chunks().to_batches(max_chunksize=BATCH))
    return csv, table


def storage_clause(fmt):
    if fmt == 'heap':
        return ''
    if fmt == 'aoco':
        return ' USING ao_column'
    return ' USING pax WITH (storage_format=%s)' % fmt


def make_table(name, fmt):
    psql('DROP TABLE IF EXISTS %s' % name)
    psql('CREATE TABLE %s (%s)%s%s; GRANT INSERT, SELECT ON %s TO %s'
         % (name, ', '.join('%s %s' % (c[0], c[1]) for c in COLUMNS), storage_clause(fmt),
            ' DISTRIBUTED BY (l_orderkey)' if SEGMENTS > 0 else '', name, ROLE))


def pg_connect():
    return pgdb.connect(PG_URI, autocommit=True)


def flight_connect():
    return fsql.connect('grpc://127.0.0.1:%d' % PORT, autocommit=True,
                        db_kwargs={'username': ROLE, 'password': PASSWORD,
                                   'adbc.flight.sql.rpc.call_header.database': 'flight'},
                        conn_kwargs={'adbc.flight.sql.session.option.vexec.mode': 'auto'})


def load(path, name, csv, table, conns):
    """one load into a fresh table: its wall time in seconds"""
    if path == 'csv':
        t = time.perf_counter()
        psql("COPY %s FROM '%s' (FORMAT csv, DELIMITER '|')" % (name, csv))
        return time.perf_counter() - t
    if path == 'copy':
        with conns['copy'].cursor() as cur:
            t = time.perf_counter()
            cur.adbc_ingest(name, table, mode='append')
            return time.perf_counter() - t
    if path == 'flight':
        sql = 'INSERT INTO %s VALUES (%s)' % (name, ', '.join('$%d' % (i + 1) for i in range(len(COLUMNS))))
        with conns['flight'].cursor() as cur:
            t = time.perf_counter()
            cur.executemany(sql, table)
            return time.perf_counter() - t
    if path == 'ingest':
        with conns['flight'].cursor() as cur:
            t = time.perf_counter()
            cur.adbc_ingest(name, table, mode='append')
            return time.perf_counter() - t
    raise ValueError(path)


def cpu_quota():
    """docker's --cpus, as the container's cgroup has it"""
    try:
        quota, period = open('/sys/fs/cgroup/cpu.max').read().split()
        return 'all %d' % os.cpu_count() if quota == 'max' else '%g' % (int(quota) / int(period))
    except (OSError, ValueError):
        return '?'


def main():
    print("the server: %s, debug_assertions %s, %d segments; the container's CPUs: %s"
          % (psql('SHOW server_version'), psql('SHOW debug_assertions'), SEGMENTS, cpu_quota()))
    psql("CREATE ROLE %s LOGIN PASSWORD '%s'" % (ROLE, PASSWORD))
    psql('CREATE EXTENSION IF NOT EXISTS vexec')     # its ingest stream, from VI on
    d = tempfile.mkdtemp(prefix='ingest-')
    t = time.time()
    csv, table = data(d)
    print('lineitem at SF %s, its first %d rows by its key: %d rows; the CSV file %.1f MB, '
          'the Arrow table %.1f MB in %d batches; made in %.1f s'
          % (SF, ROWS, table.num_rows, os.path.getsize(csv) / 1e6, table.nbytes / 1e6,
             len(table.to_batches()), time.time() - t))
    warm_csv = os.path.join(d, 'warm.csv')
    with open(csv) as src, open(warm_csv, 'w') as dst:
        for i, line in enumerate(src):
            if i >= WARM:
                break
            dst.write(line)
    warm_table = table.slice(0, WARM)

    conns = {'copy': pg_connect(), 'flight': flight_connect()}
    results = {'rows': table.num_rows, 'sf': SF, 'segments': SEGMENTS, 'cpus': cpu_quota(),
               'batch': BATCH, 'csv_bytes': os.path.getsize(csv), 'arrow_bytes': table.nbytes, 'formats': {}}
    ok = True
    for fmt in FORMATS:
        name = 'ingest_%s' % fmt
        times = {p: [] for p in PATHS}
        for p in PATHS:					# warm each path's code and caches
            make_table(name, fmt)
            load(p, name, warm_csv, warm_table, conns)
        reference = None
        for run in range(RUNS):
            for p in PATHS:
                make_table(name, fmt)
                times[p].append(load(p, name, csv, table, conns))
                got = psql(CHECK % name)
                if reference is None:
                    reference = (p, got)
                elif got != reference[1]:
                    ok = False
                    print('  NOT OK: %s run %d loaded a table unlike %s\'s:\n    %s\n    %s'
                          % (p, run + 1, reference[0], got, reference[1]))
                if not got.startswith('%d|' % table.num_rows):
                    ok = False
                    print('  NOT OK: %s run %d loaded %s rows, not %d' % (p, run + 1, got.split('|')[0], table.num_rows))
        psql('DROP TABLE IF EXISTS %s' % name)
        base = statistics.median(times[PATHS[0]])
        print('%s, %d rows, %d loads each after a warm one of %d rows:'
              % ('heap' if fmt == 'heap' else 'ao_column' if fmt == 'aoco' else 'PAX ' + fmt,
                 table.num_rows, RUNS, WARM))
        results['formats'][fmt] = {}
        for p in PATHS:
            med = statistics.median(times[p])
            results['formats'][fmt][p] = {'times_s': times[p], 'median_s': med, 'rows_per_s': table.num_rows / med}
            print('  %-7s median %8.2f s  [%.2f .. %.2f]  %10.0f rows/s  %6.2fx of %s\'s time'
                  % (p, med, min(times[p]), max(times[p]), table.num_rows / med, med / base, PATHS[0]))
    for c in conns.values():
        c.close()
    results['tables_equal'] = ok
    with open(os.path.join(os.environ.get('RESULTS_DIR', '/tmp'), 'ingest.json'), 'w') as f:
        json.dump(results, f, indent=1)
    print('every load: %s' % ('the same table, by count and sums' if ok else 'NOT the same table'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
