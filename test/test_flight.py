#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# vexec_flight's tests from its clients (pg_vector_executor.md §5, V10, "Done
# when"), run by flight.sh against a server with vexec and vexec_flight
# preloaded, the endpoint on FLIGHT_PORT:
#
#   clients     adbc_driver_flightsql's Arrow types for every type of vexec's
#               semantics corpus, its values against adbc_driver_postgresql's
#               and psql's; pyarrow.flight listing catalogs, schemas and
#               tables; prepared statements with DoPut parameters; a Flight
#               transaction that rolls back
#   logins      reject and hostssl lines, rolcanlogin, an expired
#               rolvaliduntil, rolconnlimit, with the client's address
#   sessions    pg_stat_activity, pg_stat_ssl; pg_cancel_backend(),
#               pg_terminate_backend() and CancelFlightInfo ending a stream
#   only with   vexec.mode off fails every statement with FAILED_PRECONDITION;
#   the vector  an empty vexec_flight.listen_addresses starts no acceptor, and
#   executor    without vexec the acceptor exits with its WARNING (servers of
#               their own, in scratch directories)
#   cost        a slow client holds its session's memory at about one batch
#   a cluster   on the port's coordinator, the corpus gathered from every
#               segment
#
# Each test prints "ok" or "FAILED" with its reason; the exit status is 1 when
# one failed.  Tests named on the command line run alone.

import base64
import decimal
import os
import subprocess
import sys
import threading
import time
import traceback

import pyarrow as pa
import pyarrow.compute as pc
import pyarrow.flight as fl
import adbc_driver_flightsql.dbapi as fsql
import adbc_driver_postgresql.dbapi as pgdb

PORT = int(os.environ['FLIGHT_PORT'])
CA = open(os.environ['FLIGHT_CA'], 'rb').read()
PLAIN = 'grpc://127.0.0.1:%d' % PORT
TLS = 'grpc+tls://127.0.0.1:%d' % PORT
DB = 'flight'
SEGMENTS = int(os.environ.get('FLIGHT_SEGMENTS', '0'))
PG_URI = 'postgresql:///%s?host=%s&port=%s&user=postgres' % (DB, os.environ['PGHOST'], os.environ['PGPORT'])

PASSWORD = 'flight-pass-0123456789'


# ---------------------------------------------------------------------------
# psql, and the roles
# ---------------------------------------------------------------------------

def psql(sql, db=DB, check=True):
    r = subprocess.run(['psql', '-X', '-q', '-At', '-v', 'ON_ERROR_STOP=1', '-d', db, '-c', sql],
                       capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError('psql: %s: %s' % (sql, r.stderr.strip()))
    return r.stdout.strip()


def setup_roles():
    stmts = [
        "CREATE ROLE flight LOGIN PASSWORD '%s'" % PASSWORD,
        "GRANT ALL ON DATABASE flight TO flight",
        "CREATE ROLE reject_user LOGIN PASSWORD 'reject-pass'",
        "CREATE ROLE ssl_user LOGIN PASSWORD 'ssl-pass'",
        "CREATE ROLE nologin_user NOLOGIN PASSWORD 'nologin-pass'",
        "CREATE ROLE expired_user LOGIN PASSWORD 'expired-pass' VALID UNTIL '2000-01-01'",
        "CREATE ROLE limited_user LOGIN PASSWORD 'limited-pass' CONNECTION LIMIT 1",
        "SET password_encryption = 'md5'; CREATE ROLE md5_user LOGIN PASSWORD 'md5-pass'",
        "CREATE ROLE pw_user LOGIN PASSWORD 'pw-pass'",
        "CREATE ROLE trust_user LOGIN",
        "CREATE ROLE cert_user LOGIN PASSWORD 'cert-pass'",
        "CREATE ROLE off_user LOGIN PASSWORD 'off-pass'",
        "ALTER ROLE off_user SET vexec.mode = off",
    ]
    if psql("SELECT 1 FROM pg_roles WHERE rolname = 'flight'", db='postgres') == '1':
        return      # a server kept running (run.sh serve) has them already
    for s in stmts:
        psql(s, db='postgres')
    psql("GRANT SELECT ON corpus TO PUBLIC; GRANT ALL ON SCHEMA public TO flight")


# ---------------------------------------------------------------------------
# Flight SQL's commands by hand, for pyarrow.flight
# ---------------------------------------------------------------------------

def varint(n):
    out = bytearray()
    while n >= 0x80:
        out.append((n & 0x7f) | 0x80)
        n >>= 7
    out.append(n)
    return bytes(out)


def fbytes(num, b):
    return varint(num << 3 | 2) + varint(len(b)) + b


def fstr(num, s):
    return fbytes(num, s.encode())


def fbool(num, v):
    return varint(num << 3) + varint(1 if v else 0)


def any_cmd(name, body=b''):
    return fstr(1, 'type.googleapis.com/arrow.flight.protocol.sql.' + name) + fbytes(2, body)


def flight_client(user='flight', password=PASSWORD, tls=False, database=DB, appname=None):
    client = fl.FlightClient(TLS if tls else PLAIN, tls_root_certs=CA if tls else None)
    headers = [(b'database', database.encode())]
    if appname:
        headers.append((b'application_name', appname.encode()))
    token = client.authenticate_basic_token(user, password, fl.FlightCallOptions(headers=headers))
    return client, fl.FlightCallOptions(headers=[token])


def raw_command(client, opts, cmd):
    info = client.get_flight_info(fl.FlightDescriptor.for_command(cmd), opts)
    return info, client.do_get(info.endpoints[0].ticket, opts).read_all()


def adbc(user='flight', password=PASSWORD, tls=False, autocommit=True, database=DB, session=None):
    kw = {'username': user, 'password': password,
          'adbc.flight.sql.rpc.call_header.database': database}
    if tls:
        kw['adbc.flight.sql.client_option.tls_root_certs'] = CA.decode()
    conn_kw = {}
    for k, v in (session or {}).items():
        conn_kw['adbc.flight.sql.session.option.' + k] = v
    return fsql.connect(TLS if tls else PLAIN, db_kwargs=kw, conn_kwargs=conn_kw, autocommit=autocommit)


def fetch(conn, sql, params=None):
    with conn.cursor() as cur:
        cur.execute(sql, params) if params is not None else cur.execute(sql)
        return cur.fetch_arrow_table()


def pg_fetch(sql, settings=()):
    with pgdb.connect(PG_URI, autocommit=True) as conn:
        with conn.cursor() as cur:
            for s in settings:
                cur.execute(s)
            cur.execute(sql)
            return cur.fetch_arrow_table()


def expect_error(f, *fragments):
    try:
        f()
    except Exception as e:      # the clients raise their own types
        text = str(e)
        for frag in fragments:
            if frag not in text:
                raise AssertionError('error without %r: %s' % (frag, text))
        return text
    raise AssertionError('no error, expected one with %r' % (fragments,))


# ---------------------------------------------------------------------------
# The tests
# ---------------------------------------------------------------------------

TESTS = []


def test(f):
    TESTS.append(f)
    return f


# rows of the corpus whose values Arrow's types hold (vexec_egress.h): not
# the infinities, 24:00:00, timestamps and intervals past int64, NaN in a
# bounded numeric
REPRESENTABLE = 'id % 50 NOT IN (15, 16, 19, 20, 21, 23, 24, 26, 27, 28, 36)'

EXPECTED_TYPES = {
    'id': pa.int32(), 'b': pa.bool_(), 'i2': pa.int16(), 'i4': pa.int32(), 'i8': pa.int64(),
    'f4': pa.float32(), 'f8': pa.float64(), 'o': pa.uint32(), 'm': pa.string(), 'ch': pa.int8(),
    'd': pa.date32(), 't': pa.time64('us'), 'ts': pa.timestamp('us'),
    'tstz': pa.timestamp('us', tz='UTC'), 'iv': pa.month_day_nano_interval(), 'tz': pa.string(),
    'n': pa.string(), 'n10_2': pa.decimal128(10, 2), 'n18_0': pa.decimal128(18, 0),
    'n38_5': pa.decimal128(38, 5), 'n5_m2': pa.decimal128(7, 0), 'n3_10': pa.decimal128(10, 10),
    'n40_2': pa.string(), 'tx': pa.string(), 'vc': pa.string(), 'bp': pa.string(),
    'bt': pa.binary(), 'js': pa.string(), 'jb': pa.string(), 'u': pa.binary(16),
    'nm': pa.string(), 'mac': pa.string(), 'mac8': pa.string(), 'ip': pa.string(),
    'pt': pa.string(), 'a4': pa.string(), 'at': pa.string(), 'k': pa.int32(), 'big': pa.string(),
}

# the columns whose text the egress takes from the type's output function, as psql shows it
TEXT_COLUMNS = ('m', 'tz', 'n', 'n40_2', 'mac', 'mac8', 'ip', 'pt', 'a4', 'at')


def storage(t):
    # pa.BaseExtensionType: pyarrow's canonical extensions (arrow.json,
    # arrow.uuid) are not Python-defined pa.ExtensionType subclasses
    return t.storage_type if isinstance(t, pa.BaseExtensionType) else t


@test
def corpus_types():
    """every column of the corpus, by adbc_driver_flightsql, in its Arrow type"""
    with adbc() as conn:
        t = fetch(conn, 'SELECT * FROM corpus WHERE %s ORDER BY id' % REPRESENTABLE)
    want = int(psql('SELECT count(*) FROM corpus WHERE %s' % REPRESENTABLE))
    assert t.num_rows == want, 'rows: %d, want %d' % (t.num_rows, want)
    for f in t.schema:
        exp = EXPECTED_TYPES[f.name]
        if storage(f.type) != exp:
            raise AssertionError('%s: %s, want %s' % (f.name, f.type, exp))
        md = f.metadata or {}
        assert b'pg_type' in md, '%s has no pg_type metadata' % f.name
        assert b'ARROW:FLIGHT:SQL:TYPE_NAME' in md, '%s has no Flight SQL type name' % f.name
    for name, ext in (('js', b'arrow.json'), ('jb', b'arrow.json'), ('u', b'arrow.uuid')):
        f = t.schema.field(name)
        got = f.type.extension_name.encode() if isinstance(f.type, pa.BaseExtensionType) else \
            (f.metadata or {}).get(b'ARROW:extension:name')
        assert got == ext, '%s: extension %r, want %r' % (name, got, ext)


@test
def corpus_values_against_postgresql_driver():
    """the values equal adbc_driver_postgresql's, column by column"""
    native = [c for c in EXPECTED_TYPES if c not in TEXT_COLUMNS and c not in ('js', 'jb', 'u', 'ch', 'o')]
    with adbc() as conn:
        ours = fetch(conn, 'SELECT %s FROM corpus WHERE %s ORDER BY id' % (', '.join(native), REPRESENTABLE))
    theirs = pg_fetch('SELECT %s FROM corpus WHERE %s ORDER BY id' % (', '.join(native), REPRESENTABLE))
    for name in native:
        a = ours.column(name).combine_chunks()
        b = theirs.column(name).combine_chunks()
        a_t = storage(a.type)
        if a_t != b.type:
            # adbc_driver_postgresql reads numeric as text; compare as text
            if pa.types.is_decimal(a_t):
                a = pa.array([None if v is None else format(v, 'f') for v in a.to_pylist()], pa.string())
                b = b.cast(pa.string())
            else:
                b = b.cast(a_t)
        if not a.equals(b):
            for i, (x, y) in enumerate(zip(a.to_pylist(), b.to_pylist())):
                if x != y and not (x != x and y != y):      # NaN
                    raise AssertionError('%s row %d: %r, adbc_driver_postgresql %r' % (name, i, x, y))


@test
def corpus_text_columns_against_psql():
    """the columns sent as text are psql's text, and "char", oid, json, jsonb and uuid as psql shows them"""
    cols = list(TEXT_COLUMNS) + ['js', 'jb', 'u', 'ch', 'o']
    with adbc() as conn:
        ours = fetch(conn, 'SELECT %s FROM corpus WHERE %s ORDER BY id' % (', '.join(cols), REPRESENTABLE))
    for name in cols:
        want = psql("SELECT coalesce(%s::text, '<NULL>') FROM corpus WHERE %s ORDER BY id"
                    % (name, REPRESENTABLE)).split('\n')
        got = []
        for v in ours.column(name).to_pylist():
            if v is None:
                got.append('<NULL>')
            elif name == 'u':
                h = v.hex() if isinstance(v, bytes) else v.bytes.hex()
                got.append('%s-%s-%s-%s-%s' % (h[:8], h[8:12], h[12:16], h[16:20], h[20:]))
            elif name == 'ch':
                got.append(chr(v & 0xff))
            else:
                got.append(str(v))
        if name == 'ch':
            # psql prints a "char" outside ASCII as an octal escape: compare the printable ones
            pairs = [(g, w) for g, w in zip(got, want) if not w.startswith('\\')]
            got, want = [g for g, _ in pairs], [w for _, w in pairs]
        if got != want:
            for i, (g, w) in enumerate(zip(got, want)):
                if g != w:
                    raise AssertionError('%s row %d: %r, psql %r' % (name, i, g, w))
            raise AssertionError('%s: %d rows, psql %d' % (name, len(got), len(want)))


@test
def values_arrow_cannot_hold_fail_closed():
    """a value its column's Arrow type cannot hold fails the statement, naming the column"""
    cases = [('d', 15, 'infinite date'), ('t', 19, '24:00:00'), ('ts', 20, 'infinite timestamp'),
             ('ts', 24, "past Arrow's last microsecond"), ('iv', 26, 'infinite interval'),
             ('iv', 28, 'nanoseconds'), ('n38_5', 36, 'NaN')]
    with adbc() as conn:
        for col, rem, frag in cases:
            sql = 'SELECT %s FROM corpus WHERE id %% 50 = %d AND id > 2000 AND %s IS NOT NULL' % (col, rem, col)
            expect_error(lambda: fetch(conn, sql), 'column "%s"' % col, frag)
    # 294247-01-10 04:00:54.775807 shifts to int64's last microsecond: Arrow holds it
    with adbc() as conn:
        t = fetch(conn, "SELECT ts FROM corpus WHERE id % 50 = 25")
    assert t.num_rows > 0


@test
def batches_and_rows_alike():
    """a vector node's batches and a row node's rows give the same stream"""
    q = 'SELECT id, i4, i8, f8, d, ts, tstz, n10_2, n38_5, tx, bt FROM corpus WHERE %s AND i4 > 0' % REPRESENTABLE
    with adbc(session={'vexec.mode': 'force'}) as conn:
        a = fetch(conn, q)      # a Vec Seq Scan at the top: its batches go out
        # a CTE Scan at the top, a row node: its rows go out (vexec's
        # vexec_egress test counts both paths' batches)
        b = fetch(conn, 'WITH c AS MATERIALIZED (%s) SELECT * FROM c' % q)
    assert a.num_rows == b.num_rows > 0
    assert a.schema.equals(b.schema, check_metadata=True), 'the two schemas differ'
    a, b = a.sort_by('id'), b.sort_by('id')
    for name in a.column_names:
        x, y = a.column(name).combine_chunks(), b.column(name).combine_chunks()
        if pa.types.is_floating(x.type):
            # NaN is not equal to itself: the bits, then
            bits = pa.int64() if x.type.bit_width == 64 else pa.int32()
            x, y = x.view(bits), y.view(bits)
        assert x.equals(y), 'the two paths differ in %s' % name


@test
def cluster_rows_come_from_the_segments():
    """on the port's coordinator: the corpus lies on every segment, and a Flight query gathers it"""
    if SEGMENTS == 0:
        return
    assert psql('SELECT count(DISTINCT gp_segment_id) FROM corpus') == str(SEGMENTS)
    plan = psql('SET vexec.mode = force; EXPLAIN (COSTS OFF) SELECT id, i4, tx FROM corpus WHERE i4 > 0')
    assert 'Gather Motion' in plan and 'Vec' in plan, plan
    with adbc(session={'vexec.mode': 'force'}) as conn:
        t = fetch(conn, 'SELECT gp_segment_id AS seg, count(*) AS n FROM corpus GROUP BY 1 ORDER BY 1')
        assert t.num_rows == SEGMENTS, t
        assert sum(t.column('n').to_pylist()) == int(psql('SELECT count(*) FROM corpus'))
        t = fetch(conn, 'SELECT id, i4, tx FROM corpus WHERE i4 > 0')
    assert t.num_rows == int(psql('SELECT count(*) FROM corpus WHERE i4 > 0'))


@test
def pyarrow_flight_lists_catalogs_schemas_tables():
    """pyarrow.flight lists catalogs, schemas and tables, and a table's schema"""
    client, opts = flight_client()
    _, t = raw_command(client, opts, any_cmd('CommandGetCatalogs'))
    assert t.column('catalog_name').to_pylist() == [DB], t
    _, t = raw_command(client, opts, any_cmd('CommandGetDbSchemas', fstr(2, 'pub%')))
    assert t.column('db_schema_name').to_pylist() == ['public'], t
    assert not t.schema.field('db_schema_name').nullable
    _, t = raw_command(client, opts, any_cmd('CommandGetTables', fstr(3, 'corpus') + fbool(5, True)))
    assert t.column('table_name').to_pylist() == ['corpus'], t
    assert t.column('table_type').to_pylist() == ['TABLE'], t
    schema = pa.ipc.read_schema(pa.py_buffer(t.column('table_schema')[0].as_py()))
    assert schema.names == list(EXPECTED_TYPES.keys()), schema.names
    assert schema.field('i4').metadata[b'ARROW:FLIGHT:SQL:TABLE_NAME'] == b'corpus'
    _, t = raw_command(client, opts, any_cmd('CommandGetTableTypes'))
    assert 'TABLE' in t.column('table_type').to_pylist()
    # a query by pyarrow.flight: GetFlightInfo's schema, DoGet's batches
    info, t = raw_command(client, opts, any_cmd('CommandStatementQuery', fstr(1, 'SELECT i4, tx FROM corpus WHERE id <= 10 ORDER BY id')))
    assert info.schema.names == ['i4', 'tx'] and t.num_rows == 10
    # GetSchema, and the methods' list
    s = client.get_schema(fl.FlightDescriptor.for_command(any_cmd('CommandStatementQuery', fstr(1, 'SELECT d FROM corpus'))), opts)
    assert s.schema.field('d').type == pa.date32()
    actions = [a.type for a in client.list_actions(opts)]
    assert 'CancelFlightInfo' in actions and 'CreatePreparedStatement' in actions, actions
    client.close()


@test
def adbc_metadata():
    """adbc_driver_flightsql's catalog calls: objects, a table's schema, server information"""
    with adbc() as conn:
        objs = conn.adbc_get_objects(depth='tables', table_name_filter='corpus').read_all().to_pylist()
        names = [t['table_name'] for c in objs for s in (c['catalog_db_schemas'] or [])
                 for t in (s['db_schema_tables'] or [])]
        assert names == ['corpus'], names
        schema = conn.adbc_get_table_schema('corpus', db_schema_filter='public')
        assert storage(schema.field('n10_2').type) == pa.decimal128(10, 2)
        info = conn.adbc_get_info()
        assert info.get('vendor_name') == 'PostgreSQL', info
        assert 'TABLE' in conn.adbc_get_table_types()


@test
def prepared_statements_with_doput_parameters():
    """prepared statements whose parameters come by DoPut: a query, and updates a set a row"""
    psql('DROP TABLE IF EXISTS params; CREATE TABLE params (i int4, s text, f float8, d date, b bytea, ts timestamptz)'
         '; GRANT ALL ON params TO flight')
    with adbc() as conn:
        t = fetch(conn, 'SELECT i4, tx FROM corpus WHERE id = $1', (42,))
        assert t.num_rows == 1 and t.column('i4')[0].as_py() == int(psql('SELECT i4 FROM corpus WHERE id = 42'))
        import datetime
        rows = [(1, 'one', 1.5, datetime.date(2026, 10, 6), b'\x00\x01', datetime.datetime(2026, 1, 2, 3, 4, 5, tzinfo=datetime.timezone.utc)),
                (2, None, None, None, None, None),
                (3, 'three', -0.25, datetime.date(1900, 1, 1), b'', datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc))]
        with conn.cursor() as cur:
            cur.executemany('INSERT INTO params VALUES ($1, $2, $3, $4, $5, $6)', rows)
        t = fetch(conn, 'SELECT * FROM params ORDER BY i')
    assert t.num_rows == 3, t
    assert t.column('s').to_pylist() == ['one', None, 'three']
    assert t.column('d').to_pylist() == [datetime.date(2026, 10, 6), None, datetime.date(1900, 1, 1)]
    assert psql("SELECT string_agg(i || ':' || coalesce(encode(b, 'hex'), '-'), ',' ORDER BY i) FROM params") == '1:0001,2:-,3:'
    # a string read by the parameter type's input function, as text-format parameters are
    with adbc() as conn:
        t = fetch(conn, 'SELECT $1::date + 1 AS d', ('2026-10-06',))
    assert str(t.column('d')[0].as_py()) == '2026-10-07'


@test
def flight_transaction_rolls_back():
    """a Flight transaction: what it inserted is gone after its rollback, and kept after a commit"""
    psql('DROP TABLE IF EXISTS tx; CREATE TABLE tx (i int4); GRANT ALL ON tx TO flight')
    conn = adbc(autocommit=False)
    with conn.cursor() as cur:
        # sent as a query, run at its FlightInfo, its row count as total_records
        cur.execute('INSERT INTO tx VALUES (1), (2)')
        assert cur.rowcount == 2, cur.rowcount
    assert psql('SELECT count(*) FROM tx') == '0', 'the transaction\'s rows, seen outside it'
    conn.rollback()
    with conn.cursor() as cur:
        cur.execute('INSERT INTO tx VALUES (3)')
    conn.commit()
    conn.close()
    assert psql('SELECT string_agg(i::text, \',\') FROM tx') == '3'


def login(user, password, tls=False, database=DB):
    client, opts = flight_client(user, password, tls=tls, database=database)
    _, t = raw_command(client, opts, any_cmd('CommandStatementQuery', fstr(1, 'SELECT current_user::text AS u')))
    client.close()
    return t.column('u')[0].as_py()


@test
def logins_follow_pg_hba_and_roles():
    """pg_hba.conf's reject and hostssl lines, rolcanlogin, rolvaliduntil, rolconnlimit"""
    assert login('flight', PASSWORD) == 'flight'
    expect_error(lambda: login('flight', 'wrong'), 'password authentication failed')
    expect_error(lambda: login('reject_user', 'reject-pass'), 'rejects connection', '127.0.0.1')
    expect_error(lambda: login('ssl_user', 'ssl-pass'), 'rejects connection', 'no encryption')
    assert login('ssl_user', 'ssl-pass', tls=True) == 'ssl_user'
    expect_error(lambda: login('nologin_user', 'nologin-pass'), 'not permitted to log in')
    expect_error(lambda: login('expired_user', 'expired-pass'), 'password authentication failed')
    assert login('md5_user', 'md5-pass') == 'md5_user'
    assert login('pw_user', 'pw-pass') == 'pw_user'
    assert login('trust_user', 'anything') == 'trust_user'
    expect_error(lambda: login('cert_user', 'cert-pass', tls=True), 'client certificate')
    expect_error(lambda: login('flight', PASSWORD, database='nosuchdb'), 'does not exist')
    # rolconnlimit counts Flight sessions
    first, opts = flight_client('limited_user', 'limited-pass')
    try:
        expect_error(lambda: flight_client('limited_user', 'limited-pass'), 'too many connections for role')
    finally:
        first.close()
    time.sleep(0.5)
    assert login('limited_user', 'limited-pass') == 'limited_user'


@test
def session_in_pg_stat_activity():
    """a session is in pg_stat_activity, with the client's address; its TLS in pg_stat_ssl"""
    client, opts = flight_client(tls=True, appname='flight-activity-test')
    try:
        raw_command(client, opts, any_cmd('CommandStatementQuery', fstr(1, 'SELECT 1 AS one')))
        row = psql("SELECT backend_type, usename, datname, host(client_addr), s.ssl "
                   "FROM pg_stat_activity a JOIN pg_stat_ssl s USING (pid) "
                   "WHERE application_name = 'flight-activity-test'")
        assert row == 'vexec_flight session|flight|flight|127.0.0.1|t', row
    finally:
        client.close()


def session_pid(appname, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        pid = psql("SELECT pid FROM pg_stat_activity WHERE application_name = '%s' AND state = 'active'" % appname)
        if pid:
            return int(pid.split('\n')[0])
        time.sleep(0.05)
    raise AssertionError('no active session %s' % appname)


BIG = "SELECT g, repeat('x', 1000) AS pad FROM generate_series(1, 20000000) g"


def stream_slowly(client, opts, info, result, pause=0.05):
    n = 0
    try:
        reader = client.do_get(info.endpoints[0].ticket, opts)
        while True:
            chunk = reader.read_chunk()
            n += chunk.data.num_rows
            time.sleep(pause)
    except StopIteration:
        result['error'] = None
    except Exception as e:
        result['error'] = str(e)
    result['rows'] = n


def check_stream_ended(how, appname, end):
    client, opts = flight_client(appname=appname)
    info = client.get_flight_info(fl.FlightDescriptor.for_command(any_cmd('CommandStatementQuery', fstr(1, BIG))), opts)
    result = {}
    th = threading.Thread(target=stream_slowly, args=(client, opts, info, result))
    th.start()
    pid = session_pid(appname)
    time.sleep(0.3)
    end(pid, info)
    th.join(60)
    assert not th.is_alive(), '%s: the stream did not end' % how
    assert result.get('error'), '%s: the stream ended without an error' % how
    client.close()
    return result['error']


@test
def cancels_end_the_stream():
    """pg_cancel_backend(), CancelFlightInfo and pg_terminate_backend() end a stream with an error"""
    err = check_stream_ended('pg_cancel_backend', 'flight-cancel',
                             lambda pid, info: psql('SELECT pg_cancel_backend(%d)' % pid))
    assert 'canceling statement' in err, err

    def cancel_flight_info(pid, info):
        other, opts = flight_client()
        body = fbytes(1, info.serialize())     # CancelFlightInfoRequest{info}
        res = list(other.do_action(fl.Action('CancelFlightInfo', body), opts))
        other.close()
        assert res and res[0].body.to_pybytes() in (b'\x08\x02', b'\x08\x01'), res
    err = check_stream_ended('CancelFlightInfo', 'flight-cfi', cancel_flight_info)
    assert 'cancel' in err.lower(), err

    err = check_stream_ended('pg_terminate_backend', 'flight-terminate',
                             lambda pid, info: psql('SELECT pg_terminate_backend(%d)' % pid))
    assert err, err


@test
def cancel_on_the_same_connection():
    """CancelFlightInfo on the connection that streams: served between two messages"""
    client, opts = flight_client(appname='flight-cfi-same')
    info = client.get_flight_info(fl.FlightDescriptor.for_command(any_cmd('CommandStatementQuery', fstr(1, BIG))), opts)
    result = {}
    th = threading.Thread(target=stream_slowly, args=(client, opts, info, result))
    th.start()
    session_pid('flight-cfi-same')
    time.sleep(0.3)
    res = list(client.do_action(fl.Action('CancelFlightInfo', fbytes(1, info.serialize())), opts))
    th.join(60)
    assert not th.is_alive() and result.get('error'), result
    assert res and res[0].body.to_pybytes() == b'\x08\x01', res     # CANCEL_STATUS_CANCELLED
    client.close()


@test
def only_with_the_vector_executor():
    """with vexec.mode off every statement fails with FAILED_PRECONDITION, naming vexec.mode"""
    with adbc('off_user', 'off-pass') as conn:
        expect_error(lambda: fetch(conn, 'SELECT 1'), 'vexec.mode')
    client, opts = flight_client()
    q = any_cmd('CommandStatementQuery', fstr(1, 'SELECT 1 AS one'))
    raw_command(client, opts, q)
    # SetSessionOptions {"vexec.mode": "off"}: a map entry, its value a string
    entry = fstr(1, 'vexec.mode') + fbytes(2, fstr(1, 'off'))
    list(client.do_action(fl.Action('SetSessionOptions', fbytes(1, entry)), opts))
    err = expect_error(lambda: raw_command(client, opts, q), 'vexec.mode')
    entry = fstr(1, 'vexec.mode') + fbytes(2, fstr(1, 'force'))
    list(client.do_action(fl.Action('SetSessionOptions', fbytes(1, entry)), opts))
    _, t = raw_command(client, opts, q)
    assert t.column('one').to_pylist() == [1]
    client.close()


def scratch_server(name, conf):
    """a server of its own, in a scratch directory, started: its directory"""
    d = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'flight-%s' % name)
    subprocess.run(['rm', '-rf', d], check=True)
    os.makedirs(d)
    r = subprocess.run(['initdb', '-D', d + '/data', '-N', '-U', 'postgres', '--locale=C', '-E', 'UTF8'],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    with open(d + '/data/postgresql.auto.conf', 'a') as f:
        f.write("listen_addresses = ''\nunix_socket_directories = '%s'\nport = %d\n%s\n"
                % (d, int(os.environ['FLIGHT_NODEPORT0']) + 100, conf))
    r = subprocess.run(['pg_ctl', '-D', d + '/data', '-l', d + '/log', '-w', 'start'], capture_output=True, text=True)
    assert r.returncode == 0, open(d + '/log').read()
    return d


def scratch_stop(d):
    subprocess.run(['pg_ctl', '-D', d + '/data', '-m', 'immediate', 'stop'], capture_output=True)
    subprocess.run(['rm', '-rf', d])


def scratch_psql(d, sql):
    r = subprocess.run(['psql', '-X', '-At', '-h', d, '-p', str(int(os.environ['FLIGHT_NODEPORT0']) + 100),
                        '-d', 'postgres', '-c', sql], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return r.stdout.strip()


def refused(port):
    import socket
    try:
        socket.create_connection(('127.0.0.1', port), timeout=2).close()
        return False
    except OSError:
        return True


@test
def no_endpoint_without_address_or_vexec():
    """an empty vexec_flight.listen_addresses starts no acceptor; without vexec the acceptor exits with a WARNING, once"""
    base = os.environ.get('FLIGHT_PRELOAD_BASE', '')
    port = PORT + 1
    acceptors = "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'vexec_flight acceptor'"
    assert psql(acceptors, db='postgres') == '1', 'the suite\'s server has no acceptor'
    # no address: vexec and vexec_flight loaded, and no acceptor
    d = scratch_server('noaddress', "shared_preload_libraries = '%svexec,vexec_flight'\nvexec_flight.port = %d"
                       % (base + ',' if base else '', port))
    try:
        time.sleep(2)
        assert scratch_psql(d, acceptors) == '0', 'an acceptor without an address'
        assert scratch_psql(d, "SHOW vexec_flight.listen_addresses") == ''
        assert refused(port), 'something listens on the Flight port'
    finally:
        scratch_stop(d)
    # an address, and no vexec: the acceptor's WARNING, and no restart
    d = scratch_server('novexec', "shared_preload_libraries = '%svexec_flight'\n"
                       "vexec_flight.listen_addresses = '127.0.0.1'\nvexec_flight.port = %d"
                       % (base + ',' if base else '', port))
    try:
        want = 'WARNING:  vexec_flight serves Flight only with vexec, which is not loaded'
        deadline = time.time() + 10
        while want not in open(d + '/log').read() and time.time() < deadline:
            time.sleep(0.2)
        time.sleep(6)       # the acceptor's restart time is 5 s: it is not started again
        log = open(d + '/log').read()
        assert log.count(want) == 1, log
        assert scratch_psql(d, acceptors) == '0'
        assert refused(port), 'something listens on the Flight port'
    finally:
        scratch_stop(d)


def rss_kb(pid):
    with open('/proc/%d/status' % pid) as f:
        for line in f:
            if line.startswith('VmRSS:'):
                return int(line.split()[1])
    return 0


@test
def slow_client_holds_one_batch():
    """a slow client: the session waits, its memory near one batch, on a result of 20 GB"""
    client, opts = flight_client(appname='flight-slow')
    info = client.get_flight_info(fl.FlightDescriptor.for_command(any_cmd('CommandStatementQuery', fstr(1, BIG))), opts)
    result = {}
    th = threading.Thread(target=stream_slowly, args=(client, opts, info, result, 0.02))
    th.start()
    pid = session_pid('flight-slow')
    samples = []
    for _ in range(40):
        samples.append(rss_kb(pid))
        time.sleep(0.1)
    psql('SELECT pg_cancel_backend(%d)' % pid)
    th.join(60)
    client.close()
    growth = max(samples) - min(samples)
    print('    the session\'s resident memory: %d to %d kB over %d samples in %.0f s, while the client read %d rows'
          % (min(samples), max(samples), len(samples), len(samples) * 0.1, result.get('rows', 0)))
    assert growth < 64 * 1024, 'the session grew by %d kB while the client read slowly' % growth


def main():
    setup_roles()
    failed = 0
    tests = [t for t in TESTS if not sys.argv[1:] or t.__name__ in sys.argv[1:]]
    for t in tests:
        start = time.time()
        try:
            t()
            print('  ok %s (%.1f s): %s' % (t.__name__, time.time() - start, t.__doc__))
        except Exception as e:
            failed += 1
            print('  FAILED %s: %s' % (t.__name__, t.__doc__))
            traceback.print_exc()
            sys.stdout.flush()
    print('%d of %d tests passed' % (len(tests) - failed, len(tests)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
