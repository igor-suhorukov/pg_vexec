#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ClickBench's suite for pg_accel: its data, DuckDB's reference answers, the
rules a run's answers are checked by, its reports, and the comparison of a run
with a baseline (pg_vector_executor.md §6.9, phase VB).

  clickbench.py subsets <cache> [full]
      The subsets of ClickBench's table that the suite runs on, from
      <cache>/hits.tsv.gz: hits_10m.tsv keeps every 10th row (9,999,750) and
      hits_1m.tsv every 100th (999,975), so that a counter's month keeps its
      share of the rows, as a prefix would not; with "full", hits.tsv too.

  clickbench.py refs <cache> <clickbench> <subset>
      DuckDB's answers to the 43 queries over a subset, the reference every run
      is checked against: <cache>/ref/<subset>/hits.duckdb, DuckDB's copy of
      the subset with PostgreSQL's types, and refs.json, each query's rows and
      what the rules below need of them.  <clickbench> is ClickBench's checkout.

  clickbench.py report <run> <cache> <clickbench>
      A run of bench.sh: each answer checked against DuckDB's, put in canonical
      form and hashed; each query's times; and the files a baseline keeps.

  clickbench.py save <baseline dir> <run>...
      The parts of runs a baseline keeps in the tree: metrics, plans, answers'
      hashes.  The answers themselves stay in the cache.

  clickbench.py compare <baseline dir> <baseline dir or run>
      Query by query, each configuration of the second against the first: the
      hot times and their ratio, regressions, answers whose hashes differ, and
      plans that differ.  Exits 1 where any is found.  A regression is a hot
      time slower than the first's by more than the first's spread -- the
      range of all its warm tries, over every repetition -- or than 10 ms,
      ClickBench's own allowance for noise (README, "Results Usage").

  clickbench.py evict <dir>...
      Drops every file's pages under the directories from the page cache, for a
      cold run that leaves the rest of the host's cache as it is.

  clickbench.py stats-sql <stats.json>
      The SQL that restores the statistics a run kept, so that a later run of
      the same load plans as that one did (bench.sh, CB_STATS).

The rules for answers (§6.9).  ClickBench fixes the rows of most queries but not
all of them, so an answer is compared by the shape of its query (SPEC below):
  - without ORDER BY, rows as a multiset;
  - with ORDER BY on columns of the output, the sequence of their values, and
    the rows themselves of each run of equal keys that the LIMIT and OFFSET do
    not cut; of a run they cut, only its keys, as PostgreSQL does not fix which
    of the tied rows are returned (DuckDB says which runs are cut);
  - Q17, LIMIT without ORDER BY: ten groups, each with its count;
  - Q23, SELECT *: as above, each row also satisfying the qual;
  - Q24, ORDER BY a column not in the output: the rows before the last key's
    run, and the rest drawn from that run;
  - Q26, ORDER BY a hidden key and then the output: one sequence only.
Against DuckDB, numbers are compared as numbers -- an average to within 1e-9 of
DuckDB's double, PostgreSQL's being numeric -- and a character(1) without its
padding; against another PostgreSQL run, canonical forms are compared exactly.

ClickBench is CC BY-NC-SA 4.0: nothing of it is copied here.  The queries, the
table's definition and the data are read from its pinned checkout and from the
cache; QUERIES_SHA256 pins the queries the rules below describe.
"""

import collections
import datetime
import decimal
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import time

# ClickBench's postgresql/queries.sql at dfe44c96, which SPEC describes.
QUERIES_SHA256 = 'a7d6673357348ee9680443216b6f26f30d1dce9f313b419d38502417b2c2a219'
DATASET_URL = 'https://datasets.clickhouse.com/hits_compatible/hits.tsv.gz'
DATASET_BYTES = 16298506510
SUBSETS = {'1m': (100, 999975), '10m': (10, 9999750), 'full': (1, 99997497)}
# hits_sample.tsv, any rows of the dataset: for working on the suite itself
SAMPLE = 'sample'

# Each query's shape, Q0 to Q42 in the file's order:
#   ('set',)                 no ORDER BY: rows as a multiset
#   ('ordered', [k...])      ORDER BY the output columns k (0-based)
#   ('any', [g...], c)       LIMIT without ORDER BY: groups g, each with count c
#   ('hidden', ['expr'...])  ORDER BY expressions not in the output
#   ('exact',)               the rows' sequence is fixed: ties are equal rows
SPEC = {
    0: ('set',), 1: ('set',), 2: ('set',), 3: ('set',), 4: ('set',), 5: ('set',), 6: ('set',),
    7: ('ordered', [1]), 8: ('ordered', [1]), 9: ('ordered', [2]), 10: ('ordered', [1]),
    11: ('ordered', [2]), 12: ('ordered', [1]), 13: ('ordered', [1]), 14: ('ordered', [2]),
    15: ('ordered', [1]), 16: ('ordered', [2]), 17: ('any', [0, 1], 2), 18: ('ordered', [3]),
    19: ('set',), 20: ('set',), 21: ('ordered', [2]), 22: ('ordered', [3]),
    23: ('ordered', [4]), 24: ('hidden', ['EventTime']), 25: ('ordered', [0]), 26: ('exact',),
    27: ('ordered', [1]), 28: ('ordered', [1]), 29: ('set',), 30: ('ordered', [2]),
    31: ('ordered', [2]), 32: ('ordered', [2]), 33: ('ordered', [1]), 34: ('ordered', [2]),
    35: ('ordered', [4]), 36: ('ordered', [1]), 37: ('ordered', [1]), 38: ('ordered', [1]),
    39: ('ordered', [5]), 40: ('ordered', [2]), 41: ('ordered', [2]), 42: ('ordered', [0]),
}
# Q23's qual, checked on each row it returns: URL, its 14th column, has "google".
QUALS = {23: (13, 'google')}
# A tie set larger than this is not kept: its rows are then checked by count.
TIE_SET_MAX = 100000
NULL = '\\N'                 # psql's null string, as bench.sh sets it


def die(msg):
    sys.exit('clickbench.py: ' + msg)


def sha256_file(path, limit=None):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def queries(src):
    path = os.path.join(src, 'postgresql', 'queries.sql')
    digest = sha256_file(path)
    if digest != QUERIES_SHA256:
        die('%s is not the queries the rules describe (sha256 %s, want %s): review SPEC'
            % (path, digest, QUERIES_SHA256))
    with open(path) as f:
        qs = [line.strip() for line in f if line.strip()]
    if len(qs) != 43:
        die('%d queries in %s, want 43' % (len(qs), path))
    for n, q in enumerate(qs):
        kind = SPEC[n][0]
        ordered = ' ORDER BY ' in q.upper()
        if ordered != (kind in ('ordered', 'hidden', 'exact')) or (kind == 'any') != (' LIMIT ' in q.upper() and not ordered):
            die('Q%d is not the shape SPEC says (%s): %s' % (n, kind, q))
    return qs


def columns(src):
    """ClickBench's table for PostgreSQL: [(name, type)], its order."""
    cols = []
    with open(os.path.join(src, 'postgresql', 'create.sql')) as f:
        for line in f:
            m = re.match(r'\s+(\w+)\s+([A-Za-z]+(?:\(\d+\))?)\s+NOT NULL', line)
            if m:
                cols.append((m.group(1), m.group(2).upper()))
    if len(cols) != 105:
        die('%d columns in create.sql, want 105' % len(cols))
    return cols


def limit_offset(sql):
    m = re.search(r'\bLIMIT\s+(\d+)(?:\s+OFFSET\s+(\d+))?\s*;?\s*$', sql, re.I)
    return (int(m.group(1)), int(m.group(2) or 0)) if m else (None, 0)


def strip_limit(sql):
    return re.sub(r'\s+LIMIT\s+\d+(\s+OFFSET\s+\d+)?\s*;?\s*$', '', sql, flags=re.I).rstrip().rstrip(';')


def with_columns(sql, exprs):
    """The query with expressions added to its output: ORDER BY's hidden keys."""
    m = re.match(r'(SELECT\s+.*?)(\s+FROM\s+.*)$', sql, re.I | re.S)
    return '%s, %s%s' % (m.group(1), ', '.join(exprs), m.group(2))


# ---------------------------------------------------------------------------
# The subsets


def subsets(cache, full=False):
    gz = os.path.join(cache, 'hits.tsv.gz')
    if not os.path.exists(gz) or os.path.getsize(gz) != DATASET_BYTES:
        die('%s is missing or incomplete (run.sh fetch downloads it)' % gz)
    want = {'10m': os.path.join(cache, 'hits_10m.tsv'), '1m': os.path.join(cache, 'hits_1m.tsv')}
    if full:
        want['full'] = os.path.join(cache, 'hits.tsv')
    todo = {k: p for k, p in want.items() if not os.path.exists(p)}
    if todo:
        t0 = time.time()
        tmp = {k: p + '.part' for k, p in todo.items()}
        # One pass: every 100th row is also a 10th, so the 1M subset is a subset of the 10M one.
        prog = ' '.join(['NR %% %d == 1 { print > "%s" }' % (SUBSETS[k][0], tmp[k]) for k in ('10m', '1m') if k in tmp]
                        + (['{ print > "%s" }' % tmp['full']] if 'full' in tmp else []))
        p1 = subprocess.Popen(['pigz', '-dc', gz], stdout=subprocess.PIPE)
        rc = subprocess.call(['mawk', prog], stdin=p1.stdout)
        p1.stdout.close()
        if p1.wait() != 0 or rc != 0:
            die('could not read %s' % gz)
        for k, p in tmp.items():
            os.rename(p, want[k])
        print('  subsets %s written in %.0f s' % (' '.join(sorted(todo)), time.time() - t0))
    info = {}
    for k, p in want.items():
        rows = int(subprocess.check_output(['wc', '-l', p]).split()[0])
        if rows != SUBSETS[k][1]:
            die('%s has %d rows, want %d' % (p, rows, SUBSETS[k][1]))
        info[k] = {'file': os.path.basename(p), 'rows': rows, 'bytes': os.path.getsize(p)}
    path = os.path.join(cache, 'subsets.json')
    old = json.load(open(path)) if os.path.exists(path) else {}
    for k in info:
        if old.get(k, {}).get('bytes') == info[k]['bytes'] and 'sha256' in old[k]:
            info[k]['sha256'] = old[k]['sha256']
        else:
            info[k]['sha256'] = sha256_file(want[k])
    info['source'] = old.get('source') or {'url': DATASET_URL, 'bytes': DATASET_BYTES, 'sha256': sha256_file(gz)}
    with open(path, 'w') as f:
        json.dump(info, f, indent=1, sort_keys=True)
    for k in sorted(want):
        print('  %-4s %s: %d rows, %.2f GB, sha256 %s' % (k, info[k]['file'], info[k]['rows'],
                                                          info[k]['bytes'] / 1e9, info[k]['sha256'][:16]))


# ---------------------------------------------------------------------------
# DuckDB's side

# COPY's text format, which hits.tsv is in, escapes a backslash, a tab, a
# newline and the like with a backslash; the dataset has \f, \', \r, \t and \n,
# which PostgreSQL reads as the characters, and DuckDB's CSV reader would keep
# as two.  The macro reads them as COPY does (copyfromparse.c, CopyReadAttributesText):
# \\ first, through a character the data does not hold, then the letters, then
# any other escaped character as itself.  Octal and hex escapes it does not
# read: refs() refuses data that has them.
UNESCAPE = r"""
CREATE OR REPLACE MACRO pg_unescape(s) AS CASE WHEN strpos(s, '\') = 0 THEN s ELSE
  replace(regexp_replace(
    replace(replace(replace(replace(replace(replace(replace(s, '\\', chr(57344)),
      '\b', chr(8)), '\f', chr(12)), '\n', chr(10)), '\r', chr(13)), '\t', chr(9)), '\v', chr(11)),
    '\\(.)', '\1', 'g'), chr(57344), '\') END
"""
DUCK_TYPES = {'BIGINT': 'BIGINT', 'INTEGER': 'INTEGER', 'SMALLINT': 'SMALLINT', 'TEXT': 'VARCHAR',
              'TIMESTAMP': 'TIMESTAMP', 'DATE': 'DATE', 'CHAR': 'VARCHAR', 'VARCHAR(255)': 'VARCHAR'}


def duck_sql(n, sql):
    """A query as DuckDB is given it: PostgreSQL's, but where the two engines
    read the same text differently.  Q28's pattern: in PostgreSQL's regular
    expressions "." matches a newline, in RE2's it does not without (?s)."""
    if n == 28:
        new = sql.replace("'^https?://", "'(?s)^https?://")
        if new == sql:
            die('Q28 has no pattern to adapt')
        return new
    return sql


def value(v):
    """A DuckDB value as psql prints PostgreSQL's."""
    if v is None:
        return None
    if isinstance(v, bool):
        return 't' if v else 'f'
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, datetime.datetime):
        return v.isoformat(sep=' ')
    if isinstance(v, datetime.date):
        return v.isoformat()
    return str(v)


def kind_of(duck_type):
    t = str(duck_type).upper()
    if t in ('DOUBLE', 'FLOAT', 'REAL'):
        return 'float'
    if t in ('TINYINT', 'SMALLINT', 'INTEGER', 'BIGINT', 'HUGEINT', 'UTINYINT', 'USMALLINT',
             'UINTEGER', 'UBIGINT', 'UHUGEINT') or t.startswith('DECIMAL'):
        return 'int'
    return 'text'


def duck_connect(path, read_only=False):
    import duckdb
    return duckdb.connect(path, read_only=read_only)


def runs_of(rows, keys):
    """[start, end) of each run of rows with equal values at the key columns."""
    out, start = [], 0
    for i in range(1, len(rows) + 1):
        if i == len(rows) or [rows[i][k] for k in keys] != [rows[start][k] for k in keys]:
            out.append([start, i])
            start = i
    return out


def refs(cache, src, subset):
    import duckdb

    qs = queries(src)
    cols = columns(src)
    tsv = os.path.join(cache, 'hits.tsv' if subset == 'full' else 'hits_%s.tsv' % subset)
    if not os.path.exists(tsv):
        die('%s is missing (clickbench.py subsets)' % tsv)
    nrows = SUBSETS[subset][1] if subset != SAMPLE else int(subprocess.check_output(['wc', '-l', tsv]).split()[0])
    d = os.path.join(cache, 'ref', subset)
    os.makedirs(d, exist_ok=True)
    db = os.path.join(d, 'hits.duckdb')
    for p in (db, db + '.wal'):
        if os.path.exists(p):
            os.remove(p)
    con = duckdb.connect(db)
    t0 = time.time()
    con.execute(UNESCAPE)
    spec = ', '.join("'c%d': 'VARCHAR'" % i for i in range(len(cols)))
    raw = ("read_csv('%s', delim='\t', quote='', escape='', header=false, auto_detect=false, "
           "nullstr='\\N', max_line_size=67108864, columns={%s})" % (tsv, spec))
    # Octal and hex escapes, which the macro does not read; and the character
    # it stands \\ in for.
    texts = [i for i, (_, t) in enumerate(cols) if DUCK_TYPES[t] == 'VARCHAR']
    bad = con.execute('SELECT %s FROM %s' % (
        ', '.join("count(*) FILTER (WHERE regexp_matches(replace(c%d, '\\\\', ''), '\\\\[0-7x]') "
                  "OR strpos(c%d, chr(57344)) > 0)" % (i, i) for i in texts), raw)).fetchone()
    if any(bad):
        die('the data has escapes the macro does not read, in columns %s'
            % [cols[i][0] for i, b in zip(texts, bad) if b])
    sel = ', '.join(('pg_unescape(c%d)' if DUCK_TYPES[t] == 'VARCHAR' else 'CAST(c%d AS ' + DUCK_TYPES[t] + ')') % i
                    + ' AS ' + name for i, (name, t) in enumerate(cols))
    con.execute('CREATE TABLE hits AS SELECT %s FROM %s' % (sel, raw))
    got = con.execute('SELECT count(*) FROM hits').fetchone()[0]
    if got != nrows:
        die('DuckDB read %d rows of %s, want %d' % (got, tsv, nrows))
    load_s = time.time() - t0
    escapes = dict(zip([cols[i][0] for i in texts], con.execute('SELECT %s FROM %s' % (
        ', '.join("count(*) FILTER (WHERE strpos(c%d, '\\') > 0)" % i for i in texts), raw)).fetchone()))
    print('  %s: DuckDB %s read %d rows in %.0f s' % (subset, duckdb.__version__, got, load_s))

    out = []
    for n, sql in enumerate(qs):
        dsql = duck_sql(n, sql).rstrip(';')
        shape = SPEC[n]
        t0 = time.time()
        cur = con.execute(dsql)
        width = len(cur.description)
        types = [kind_of(d[1]) for d in cur.description]
        raw = cur.fetchall()
        ms = (time.time() - t0) * 1000
        rows = [[value(v) for v in r] for r in raw]
        if sql.upper().startswith('SELECT * '):
            types = ['bpchar' if t == 'CHAR' else kind for (_, t), kind in zip(cols, types)]
        lim, off = limit_offset(sql)
        entry = {'n': n, 'types': types, 'rows': rows, 'ms': round(ms, 1), 'limit': lim, 'offset': off}
        if dsql != sql.rstrip(';'):
            entry['duckdb_sql'] = dsql
        if shape[0] in ('ordered', 'hidden'):
            keys, base, ktypes = shape[1], dsql, types
            if shape[0] == 'hidden':
                # the hidden keys after the output's columns
                base = with_columns(dsql, keys)
                cur = con.execute(base)
                ktypes = types + [kind_of(d[1]) for d in cur.description[width:]]
                raw = cur.fetchall()
                keys = list(range(width, width + len(shape[1])))
                entry['window'] = [[value(v) for v in r] for r in raw]
            names = ['c%d' % i for i in range(len(ktypes))]
            runs = []
            if raw:
                con.execute('CREATE OR REPLACE TEMP TABLE full_q AS SELECT * FROM (%s) AS t(%s)'
                            % (strip_limit(base), ', '.join(names)))
                # A run of the window's equal keys is cut by the LIMIT or the
                # OFFSET where the whole result has more rows with its keys.
                # Keys as DuckDB has them; an average's to within 1e-9.
                for s, e in runs_of([[value(v) for v in r] for r in raw], keys):
                    cond = ' AND '.join(
                        ('abs(c%d - $%d) <= 1e-9 * greatest(abs($%d), 1)' % (k, j + 1, j + 1)) if ktypes[k] == 'float'
                        else ('c%d IS NOT DISTINCT FROM $%d' % (k, j + 1)) for j, k in enumerate(keys))
                    params = [raw[s][k] for k in keys]
                    total = con.execute('SELECT count(*) FROM full_q WHERE ' + cond, params).fetchone()[0]
                    run = {'start': s, 'end': e, 'total': total, 'complete': total == e - s}
                    if shape[0] == 'hidden' and not run['complete'] and total <= TIE_SET_MAX:
                        ties = con.execute('SELECT %s, count(*) FROM full_q WHERE %s GROUP BY ALL'
                                           % (', '.join(names[:width]), cond), params).fetchall()
                        run['ties'] = [[[value(v) for v in t[:-1]], t[-1]] for t in ties]
                    runs.append(run)
            entry['runs'] = runs
        out.append(entry)
        cut = sum(1 for r in entry.get('runs', []) if not r['complete'])
        print('  %s Q%d: %d rows in %.0f ms%s' % (subset, n, len(rows), ms,
                                                 ', %d run%s of tied keys cut by LIMIT or OFFSET' % (cut, 's' if cut > 1 else '')
                                                 if cut else ''))
    with open(os.path.join(d, 'refs.json'), 'w') as f:
        json.dump({'subset': subset, 'rows': nrows, 'duckdb': duckdb.__version__,
                   'queries_sha256': QUERIES_SHA256, 'load_s': round(load_s, 1),
                   'values_with_escapes': escapes, 'q': out}, f)
    con.close()


# ---------------------------------------------------------------------------
# Answers


def parse_answer(path):
    """psql -A -t -F US -0: fields separated by 0x1f, every row ended by a NUL."""
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except FileNotFoundError:
        return None
    if not data:
        return []
    recs = data.split(b'\0')
    if recs[-1] == b'':
        recs = recs[:-1]
    return [[None if v == NULL.encode() else v.decode('utf-8', 'surrogateescape') for v in r.split(b'\x1f')]
            for r in recs]


def num(s):
    try:
        return decimal.Decimal(s)
    except (decimal.InvalidOperation, TypeError, ValueError):
        return None


def eq(a, b, kind):
    """A value of PostgreSQL's against DuckDB's."""
    if a is None or b is None:
        return a is None and b is None
    if kind == 'int':
        x, y = num(a), num(b)
        return x is not None and x == y
    if kind == 'float':
        x, y = num(a), num(b)
        if x is None or y is None:
            return False
        return abs(x - y) <= max(abs(y), decimal.Decimal(1)) * decimal.Decimal('1e-9')
    if kind == 'bpchar':
        return a.rstrip(' ') == b.rstrip(' ')
    return a == b


def row_eq(r, s, types):
    return len(r) == len(s) and all(eq(a, b, t) for a, b, t in zip(r, s, types))


def sort_key(row):
    return [(0, '') if v is None else (1, v) for v in row]


def ref_sort_key(row, types):
    """PostgreSQL's rows and DuckDB's in one order: numbers by value, and an
    average by its first digits, which both engines' agree in."""
    def k(v, t):
        if v is None:
            return (0, '')
        if t in ('int', 'float') and num(v) is not None:
            return (1, num(v) if t == 'int' else decimal.Decimal('%.6e' % float(v)))
        return (2, v)
    return [k(v, t) for v, t in zip(row, types)]


def multiset_eq(a, b, types):
    if len(a) != len(b):
        return False
    a = sorted(a, key=lambda r: ref_sort_key(r, types))
    b = sorted(b, key=lambda r: ref_sort_key(r, types))
    return all(row_eq(x, y, types) for x, y in zip(a, b))


def check(n, rows, ref, duck=None):
    """Does PostgreSQL's answer agree with DuckDB's, by the rules?  (ok, why)."""
    shape, types, want = SPEC[n], ref['types'], ref['rows']
    if rows is None:
        return False, 'no answer'
    if any(len(r) != len(types) for r in rows):
        return False, 'rows of %s columns, DuckDB\'s of %d' % (sorted(set(len(r) for r in rows)), len(types))
    kind = shape[0]
    if kind == 'set':
        return (True, '') if multiset_eq(rows, want, types) else (False, 'rows differ')
    if kind == 'exact':
        if len(rows) != len(want):
            return False, '%d rows, DuckDB %d' % (len(rows), len(want))
        return (True, '') if all(row_eq(r, s, types) for r, s in zip(rows, want)) else (False, 'sequence differs')
    if kind == 'ordered':
        keys = shape[1]
        if len(rows) != len(want):
            return False, '%d rows, DuckDB %d' % (len(rows), len(want))
        for i, (r, s) in enumerate(zip(rows, want)):
            if not all(eq(r[k], s[k], types[k]) for k in keys):
                return False, 'row %d: keys %s, DuckDB %s' % (i, [r[k] for k in keys], [s[k] for k in keys])
        for run in ref['runs']:
            if run['complete'] and not multiset_eq(rows[run['start']:run['end']], want[run['start']:run['end']], types):
                return False, 'rows %d to %d, whose keys tie, differ' % (run['start'], run['end'] - 1)
        if n in QUALS:
            col, needle = QUALS[n]
            for i, r in enumerate(rows):
                if r[col] is None or needle not in r[col]:
                    return False, 'row %d does not satisfy the qual' % i
        return True, ''
    if kind == 'hidden':
        return hidden(rows, ref, types)[0:2]
    if kind == 'any':
        groups, count = shape[1], shape[2]
        if len(rows) != len(want):
            return False, '%d rows, DuckDB %d' % (len(rows), len(want))
        if len(set(tuple(r[g] for g in groups) for r in rows)) != len(rows):
            return False, 'a group returned twice'
        if duck is None:
            return False, 'no DuckDB to look the groups up in'
        names = [re.match(r'SELECT\s+(.*?)\s+FROM', ref['sql'], re.I).group(1).split(',')[g].strip() for g in groups]
        for r in rows:
            cond = ' AND '.join('%s = $%d' % (nm, j + 1) for j, nm in enumerate(names))
            got = duck.execute('SELECT count(*) FROM hits WHERE ' + cond,
                               [int(r[g]) if types[g] == 'int' else r[g] for g in groups]).fetchone()[0]
            if not eq(r[count], str(got), 'int'):
                return False, 'group %s has %s rows, DuckDB %d' % ([r[g] for g in groups], r[count], got)
        return True, ''
    return False, 'no rule for Q%d' % n


def hidden(rows, ref, types):
    """Q24: ORDER BY a column the output does not have.  (ok, why, the rows
    before the last key's run, how many rows of that run)"""
    width = len(types)
    window, runs = ref['window'], ref['runs']
    if len(rows) != len(window):
        return False, '%d rows, DuckDB %d' % (len(rows), len(window)), None, None
    required = [w[:width] for run in runs if run['complete'] for w in window[run['start']:run['end']]]
    left, matched = list(rows), []
    for want in required:
        for i, r in enumerate(left):
            if row_eq(r, want, types):
                matched.append(left.pop(i))
                break
        else:
            return False, 'a row DuckDB returns before the last key is missing: %s' % want, None, None
    pool = []
    for run in runs:
        if not run['complete']:
            if 'ties' not in run:
                pool = None
                break
            pool += [list(t) for t in run['ties']]
    if pool is not None:
        for r in left:
            for t in pool:
                if t[1] > 0 and row_eq(r, t[0], types):
                    t[1] -= 1
                    break
            else:
                return False, 'a row that is not among the rows that tie at the last key: %s' % r, None, None
    return True, '', matched, len(left)


def canonical(n, sql, rows, ref, ok):
    """The answer in a form two correct answers share: what is hashed, and
    what a later run's answers are compared with.  From PostgreSQL's own values;
    DuckDB's runs where the keys are DuckDB's."""
    if rows is None:
        return None
    shape = SPEC[n]
    kind = shape[0]
    if kind == 'set':
        return {'rows': sorted(rows, key=sort_key)}
    if kind == 'exact':
        return {'rows': rows}
    if kind == 'any':
        return {'groups': len(rows), 'counts_checked': bool(ok)}
    if kind == 'hidden':
        res = hidden(rows, ref, ref['types']) if ref else (False, '', None, None)
        if res[0]:
            return {'rows': sorted(res[2], key=sort_key), 'tied': res[3]}
        return {'rows': sorted(rows, key=sort_key), 'unchecked': True}
    keys = shape[1]
    lim, off = limit_offset(sql)
    if ref and ok:
        runs = [(r['start'], r['end'], r['complete']) for r in ref['runs']]
    else:
        # PostgreSQL's own runs; the first is cut by an OFFSET, the last by a full LIMIT
        rr = runs_of(rows, keys)
        runs = [(s, e, not ((i == 0 and off) or (e == len(rows) and lim is not None and len(rows) == lim)))
                for i, (s, e) in enumerate(rr)]
    out = []
    for s, e, complete in runs:
        part = rows[s:e]
        run = {'keys': [[r[k] for k in keys] for r in part]}
        if complete:
            run['rows'] = sorted(part, key=sort_key)
        out.append(run)
    return {'runs': out}


def digest(obj):
    return hashlib.sha256(json.dumps(obj, sort_keys=True, ensure_ascii=False,
                                     separators=(',', ':')).encode('utf-8', 'surrogateescape')).hexdigest()[:16]


# ---------------------------------------------------------------------------
# A run's report


def load_tsv(path):
    try:
        with open(path) as f:
            return [line.rstrip('\n').split('\t') for line in f if line.strip()]
    except FileNotFoundError:
        return []


def r3(x):
    return None if x is None else round(x, 3)


def median(xs):
    xs = sorted(xs)
    if not xs:
        return None
    m = len(xs) // 2
    return xs[m] if len(xs) % 2 else (xs[m - 1] + xs[m]) / 2


def gmean_10ms(ms):
    xs = [x for x in ms if x is not None]
    return math.exp(sum(math.log(x + 10) for x in xs) / len(xs)) - 10 if xs else None


def report(run, cache, src):
    cfg = json.load(open(os.path.join(run, 'config.json')))
    qs = queries(src)
    subset = cfg['subset']
    refpath = os.path.join(cache, 'ref', subset, 'refs.json')
    refs_ = json.load(open(refpath)) if os.path.exists(refpath) else None
    duck = None
    if refs_ is not None:
        for e in refs_['q']:
            e['sql'] = qs[e['n']]
        try:
            duck = duck_connect(os.path.join(cache, 'ref', subset, 'hits.duckdb'), read_only=True)
        except Exception as ex:
            print('  (DuckDB\'s copy of the subset is not readable: %s)' % ex)
    times = load_tsv(os.path.join(run, 'times.tsv'))
    load = {r[0]: float(r[1]) for r in load_tsv(os.path.join(run, 'load.tsv'))}
    size = int(open(os.path.join(run, 'size')).read().split()[0]) if os.path.exists(os.path.join(run, 'size')) else None
    temp = {(r[0], int(r[1])): r for r in load_tsv(os.path.join(run, 'analyze', 'temp.tsv'))}
    planners = cfg['planners'].split()
    answers_dir = os.path.join(run, 'answers')
    os.makedirs(answers_dir, exist_ok=True)
    summary = {'config': cfg, 'load_ms': load, 'data_bytes': size, 'planners': {}}
    failures = 0
    for planner in planners:
        name = config_name(cfg, planner)
        per_q = []
        canon_all = {}
        plans = {}
        for n in range(len(qs)):
            if cfg.get('queries') and n not in cfg['queries']:
                continue
            ref = refs_['q'][n] if refs_ else None
            tries = collections.defaultdict(dict)        # rep -> try -> (ms, status)
            for r in times:
                if r[1] == planner and int(r[2]) == n:
                    tries[int(r[0])][int(r[3])] = (float(r[4]) if r[4] not in ('', 'null') else None, r[5])
            reps = []
            hashes = set()
            verdict = None
            canon = None
            fallback = False
            for rep in sorted(tries):
                t = [tries[rep].get(i, (None, 'missing')) for i in range(1, cfg['tries'] + 1)]
                ms = [x[0] if x[1] == 'ok' else None for x in t]
                hot = min(ms[1:]) if len(ms) > 1 and all(x is not None for x in ms[1:]) else (ms[0] if len(ms) == 1 else None)
                reps.append({'tries_ms': ms, 'status': [x[1] for x in t], 'cold_ms': ms[0], 'hot_ms': hot})
                for i in range(1, cfg['tries'] + 1):
                    base = os.path.join(run, 'out', planner, 'r%d' % rep, 'q%02d.t%d' % (n, i))
                    if os.path.exists(base + '.err'):
                        err = open(base + '.err', errors='replace').read()
                        if 'GPORCA failed to produce a plan' in err:
                            fallback = True
                    if t[i - 1][1] != 'ok':
                        continue
                    rows = parse_answer(base + '.out')
                    ok, why = check(n, rows, ref, duck) if ref else (None, 'no reference')
                    c = canonical(n, qs[n], rows, ref, ok)
                    h = digest(c)
                    hashes.add(h)
                    if verdict is None or (verdict[0] is not False and ok is False):
                        verdict = (ok, why)
                        canon = c
            # A query's hot time is the median of its repetitions' hot times,
            # each the faster of its warm tries; its spread is the range of
            # all its warm tries, which compare takes as its noise.
            hots = [r['hot_ms'] for r in reps if r['hot_ms'] is not None]
            warm = [x for r in reps for x in r['tries_ms'][1:] if x is not None]
            colds = [r['cold_ms'] for r in reps if r['cold_ms'] is not None]
            entry = {'q': n, 'reps': reps,
                     'hot_ms': r3(median(hots)), 'hot_min_ms': min(hots) if hots else None,
                     'hot_max_ms': max(hots) if hots else None,
                     'hot_spread_ms': r3(max(warm) - min(warm)) if len(warm) > 1 else None,
                     'rep_spread_ms': r3(max(hots) - min(hots)) if len(hots) > 1 else None,
                     'cold_ms': r3(median(colds)),
                     'cold_spread_ms': r3(max(colds) - min(colds)) if len(colds) > 1 else None,
                     'answer': sorted(hashes)[0] if len(hashes) == 1 else sorted(hashes) or None,
                     'duckdb': None if verdict is None else ('same' if verdict[0] else
                                                             'no reference' if verdict[0] is None else
                                                             'differs: ' + verdict[1])}
            if len(hashes) > 1:
                entry['answers_differ_between_runs'] = True
            if fallback:
                entry['orca_fallback'] = True
            pj = os.path.join(run, 'plans', planner, 'q%02d.json' % n)
            if os.path.exists(pj):
                try:
                    plan = json.load(open(pj))
                    plans['q%02d' % n] = plan
                    entry['plan'] = digest(plan_shape(plan))
                except ValueError:
                    entry['plan'] = 'unreadable'
            tk = temp.get((planner, n))
            if tk:
                entry['analyze'] = {'ms': float(tk[2]) if tk[2] else None, 'temp_bytes': int(tk[3]) if tk[3] else 0,
                                    'status': tk[4] if len(tk) > 4 else ''}
            if verdict is not None and verdict[0] is False:
                failures += 1
            canon_all['q%02d' % n] = canon
            per_q.append(entry)
        hot = [e['hot_ms'] for e in per_q]
        res = {'name': name, 'queries': per_q,
               'hot_gmean_ms': r3(gmean_10ms(hot)), 'cold_gmean_ms': r3(gmean_10ms([e['cold_ms'] for e in per_q])),
               'missing': sum(1 for x in hot if x is None),
               'duckdb_differs': [e['q'] for e in per_q if e['duckdb'] and e['duckdb'].startswith('differs')]}
        nreps = max((len(e['reps']) for e in per_q), default=0)
        res['rep_hot_gmean_ms'] = [r3(gmean_10ms([e['reps'][i]['hot_ms'] if i < len(e['reps']) else None
                                                  for e in per_q])) for i in range(nreps)]
        summary['planners'][planner] = res
        with open(os.path.join(answers_dir, '%s.json' % name), 'w') as f:
            json.dump(canon_all, f, ensure_ascii=False)
        with open(os.path.join(run, 'plans-%s.json' % name), 'w') as f:
            json.dump(plans, f, indent=1, sort_keys=True)
        if cfg['mode'] == 'time':
            write_clickbench(run, cfg, name, per_q, load, size)
        print_summary(cfg, name, res)
    with open(os.path.join(run, 'report.json'), 'w') as f:
        json.dump(summary, f, indent=1, sort_keys=True)
    return 1 if failures else 0


def config_name(cfg, planner):
    """A configuration's name: its route, planner and storage, and
    vexec's session after a "+" (bench.sh's CB_VEXEC)."""
    base, _, vexec = planner.partition('+')
    if cfg['route'] == 'vanilla':
        name = 'vanilla-%s' % cfg['storage']
    elif cfg['route'] == 'vanilla-orca':
        name = '%s-vanilla-%s' % ('orca' if base == 'orca' else 'planner', cfg['storage'])
    else:
        name = '%s-%s-s%d' % ('orca' if base == 'orca' else 'planner', cfg['storage'], cfg['segments'])
    return name + ('+' + vexec if vexec else '')


def write_clickbench(run, cfg, name, per_q, load, size):
    d = os.path.join(run, 'clickbench')
    os.makedirs(d, exist_ok=True)
    nreps = max((len(e['reps']) for e in per_q), default=0)
    host = cfg.get('host') or 'localhost'
    if cfg['route'] == 'vanilla-orca':
        system = 'PostgreSQL 19 (vanilla) with gp_orca (%s)' % ('ORCA' if name.startswith('orca-') else 'planner')
    else:
        system = {'vanilla': 'PostgreSQL 19 (vanilla)', 'orca': 'Cloudberry on PostgreSQL 19 (ORCA)',
                  'planner': 'Cloudberry on PostgreSQL 19 (planner)'}[name.split('-')[0]]
    for i in range(nreps):
        doc = {'system': '%s, %s, %s rows' % (system, cfg['storage'], cfg['subset']),
               'date': cfg['date'], 'machine': '%s r%d' % (host, i + 1),
               'cluster_size': 1, 'proprietary': 'no', 'hardware': 'cpu', 'tuned': 'no',
               'tags': ['C', 'PostgreSQL compatible', 'pg_accel baseline'],
               'load_time': round(load.get('total', 0) / 1000, 3), 'data_size': size,
               'result': [[None if t is None else round(t / 1000, 3) for t in e['reps'][i]['tries_ms']]
                          if i < len(e['reps']) else [None] * cfg['tries'] for e in per_q]}
        text = json.dumps(doc, indent=1)
        text = re.sub(r'\[\s+([-0-9.enul]+),\s+([-0-9.enul]+),\s+([-0-9.enul]+)\s+\]', r'[\1, \2, \3]', text)
        with open(os.path.join(d, '%s.r%d.json' % (name, i + 1)), 'w') as f:
            f.write(text + '\n')


def fmt_ms(x):
    return '-' if x is None else ('%.0f' % x if x >= 100 else '%.1f' % x)


def print_summary(cfg, name, res):
    print('  %s (%s, %s rows): hot geometric mean %s ms (+10 ms), cold %s ms; %d missing; '
          'DuckDB differs in %s' % (name, cfg['mode'], cfg['subset'], fmt_ms(res['hot_gmean_ms']),
                                    fmt_ms(res['cold_gmean_ms']), res['missing'],
                                    ', '.join('Q%d' % q for q in res['duckdb_differs']) or 'none'))
    for e in res['queries']:
        flags = []
        if e['duckdb'] and e['duckdb'] != 'same':
            flags.append('DuckDB ' + e['duckdb'])
        if e.get('answers_differ_between_runs'):
            flags.append('answers differ between runs')
        if e.get('orca_fallback'):
            flags.append('ORCA fell back to the planner')
        bad = [s for r in e['reps'] for s in r['status'] if s != 'ok']
        if bad:
            flags.append('not ok: %s' % ' '.join(sorted(set(bad))))
        print('    Q%-2d hot %8s ms (spread %6s)  cold %8s ms%s' % (
            e['q'], fmt_ms(e['hot_ms']), fmt_ms(e['hot_spread_ms']), fmt_ms(e['cold_ms']),
            ('  ' + '; '.join(flags)) if flags else ''))


# ---------------------------------------------------------------------------
# A baseline, and the comparison


def save(dest, runs):
    """What the tree keeps of runs: no data values (§6.9)."""
    os.makedirs(dest, exist_ok=True)
    for run in runs:
        rep = json.load(open(os.path.join(run, 'report.json')))
        cfg = rep['config']
        mode = cfg['mode']
        for planner, res in rep['planners'].items():
            name = res['name']
            d = os.path.join(dest, mode)
            os.makedirs(os.path.join(d, 'plans'), exist_ok=True)
            doc = {'config': cfg, 'planner': planner, 'name': name, 'load_ms': rep['load_ms'],
                   'data_bytes': rep['data_bytes'], 'env': json.load(open(os.path.join(run, 'env.json')))
                   if os.path.exists(os.path.join(run, 'env.json')) else None,
                   'run': os.path.basename(os.path.dirname(os.path.abspath(run))) + '/' + os.path.basename(run)}
            doc.update(res)
            with open(os.path.join(d, name + '.json'), 'w') as f:
                json.dump(doc, f, indent=1, sort_keys=True)
            shutil.copy(os.path.join(run, 'plans-%s.json' % name), os.path.join(d, 'plans', name + '.json'))
            an = os.path.join(run, 'analyze', planner)
            if os.path.isdir(an):
                os.makedirs(os.path.join(d, 'analyze'), exist_ok=True)
                plans = {}
                for f in sorted(os.listdir(an)):
                    if f.endswith('.json'):
                        try:
                            plans[f[:-5]] = json.load(open(os.path.join(an, f)))
                        except ValueError:
                            plans[f[:-5]] = None
                with open(os.path.join(d, 'analyze', name + '.json'), 'w') as f:
                    json.dump(plans, f, indent=1, sort_keys=True)
            cb = os.path.join(run, 'clickbench')
            if os.path.isdir(cb):
                os.makedirs(os.path.join(d, 'clickbench'), exist_ok=True)
                for f in os.listdir(cb):
                    if f.startswith(name + '.'):
                        shutil.copy(os.path.join(cb, f), os.path.join(d, 'clickbench', f))
        print('  saved %s' % run)


def load_configs(path):
    """name -> its file of the suite's own, from a baseline directory or a run."""
    out = {}
    if os.path.exists(os.path.join(path, 'report.json')):
        paths = [path]
    else:
        paths = [os.path.join(path, d) for d in sorted(os.listdir(path)) if os.path.isdir(os.path.join(path, d))]
    for p in paths:
        if os.path.exists(os.path.join(p, 'report.json')):
            rep = json.load(open(os.path.join(p, 'report.json')))
            for planner, res in rep['planners'].items():
                doc = dict(res)
                doc['config'] = rep['config']
                doc['plans'] = json.load(open(os.path.join(p, 'plans-%s.json' % res['name']))) \
                    if os.path.exists(os.path.join(p, 'plans-%s.json' % res['name'])) else {}
                out[(rep['config']['mode'], res['name'])] = doc
        elif os.path.basename(p) in ('check', 'time'):
            for f in sorted(os.listdir(p)):
                if f.endswith('.json'):
                    doc = json.load(open(os.path.join(p, f)))
                    pp = os.path.join(p, 'plans', f)
                    doc['plans'] = json.load(open(pp)) if os.path.exists(pp) else {}
                    out[(os.path.basename(p), doc['name'])] = doc
    return out


def plan_shape(plan):
    """A plan without what changes between runs of the same plan."""
    if isinstance(plan, dict):
        return {k: plan_shape(v) for k, v in plan.items()
                if k not in ('Planning Time', 'Execution Time', 'Planning', 'Triggers', 'JIT')}
    if isinstance(plan, list):
        return [plan_shape(v) for v in plan]
    return plan


def compare(base_path, run_path, floor_ms=10.0):
    base, run = load_configs(base_path), load_configs(run_path)
    common = sorted(set(base) & set(run))
    if not common:
        die('no configuration in common between %s and %s' % (base_path, run_path))
    bad = 0
    for key in common:
        b, r = base[key], run[key]
        mode, name = key
        print('%s, %s mode (%s rows):' % (name, mode, r['config']['subset']))
        print('    %-4s %10s %10s %10s %7s  %s' % ('', 'baseline', 'spread', 'run', 'ratio', ''))
        bq = {e['q']: e for e in b['queries']}
        regress, answers, plans, ratios = [], [], [], []
        for e in r['queries']:
            q = e['q']
            f = bq.get(q)
            if f is None:
                continue
            flags = []
            bh, rh = f.get('hot_ms'), e.get('hot_ms')
            spread = f.get('hot_spread_ms') or 0.0
            ratio = None
            if bh is not None and rh is not None:
                ratio = (bh + 10) / (rh + 10)
                ratios.append(ratio)
                if mode == 'time' and rh - bh > max(spread, floor_ms):
                    flags.append('REGRESSION: +%s ms, beyond the spread' % fmt_ms(rh - bh))
                    regress.append(q)
            elif bh is not None and rh is None:
                flags.append('no result')
                regress.append(q)
            if f.get('answer') != e.get('answer'):
                flags.append('ANSWER differs from the baseline\'s')
                answers.append(q)
            if e.get('duckdb') and e['duckdb'] != 'same':
                flags.append('DuckDB ' + e['duckdb'])
            pk = 'q%02d' % q
            if pk in b.get('plans', {}) and pk in r.get('plans', {}) and \
                    plan_shape(b['plans'][pk]) != plan_shape(r['plans'][pk]):
                flags.append('PLAN differs')
                plans.append(q)
            print('    Q%-3d %10s %10s %10s %7s  %s' % (q, fmt_ms(bh), fmt_ms(f.get('hot_spread_ms')), fmt_ms(rh),
                                                     '-' if ratio is None else '%.2fx' % ratio, '; '.join(flags)))
        gm = math.exp(sum(math.log(x) for x in ratios) / len(ratios)) if ratios else None
        print('    hot geometric mean: baseline %s ms, run %s ms; speed-up %s (geometric mean of (10 ms + time) ratios)'
              % (fmt_ms(b.get('hot_gmean_ms')), fmt_ms(r.get('hot_gmean_ms')), '-' if gm is None else '%.3fx' % gm))
        print('    regressions: %s; answers that differ: %s; plans that differ: %s' % (
            ' '.join('Q%d' % q for q in regress) or 'none', ' '.join('Q%d' % q for q in answers) or 'none',
            ' '.join('Q%d' % q for q in plans) or 'none'))
        print()
        bad += len(regress) + len(answers) + len(plans)
    only = sorted(set(base) ^ set(run))
    if only:
        print('in one of them only: %s' % ', '.join('%s (%s)' % (n, m) for m, n in only))
    return 1 if bad else 0


# ---------------------------------------------------------------------------


def evict(dirs):
    n = pages = 0
    for root in dirs:
        for dirpath, _, files in os.walk(root):
            for name in files:
                p = os.path.join(dirpath, name)
                try:
                    fd = os.open(p, os.O_RDONLY | os.O_NOFOLLOW)
                except OSError:
                    continue
                try:
                    os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
                    n += 1
                finally:
                    os.close(fd)
    print('%d files' % n)


def stats_sql(path):
    """SQL that puts a run's statistics of hits back (bench.sh, CB_STATS): what
    pg_dump --statistics writes, from what bench.sh read of pg_class and pg_stats."""
    st = json.load(open(path))
    lit = lambda v: "'" + v.replace("'", "''") + "'"
    out = ['SET client_min_messages = warning;']
    for r in st.get('relations') or []:
        args = ["'schemaname', 'public'::text", "'relname', %s::text" % lit(r['relname']),
                "'relpages', %d::integer" % r['relpages'], "'reltuples', %r::real" % float(r['reltuples']),
                "'relallvisible', %d::integer" % r['relallvisible']]
        if r.get('relallfrozen') is not None:
            args.append("'relallfrozen', %d::integer" % r['relallfrozen'])
        out.append('SELECT pg_restore_relation_stats(%s);' % ', '.join(args))
    for a in st.get('attributes') or []:
        inh = 'true' if a['inherited'] else 'false'
        out.append("SELECT pg_clear_attribute_stats('public', 'hits', %s, %s);" % (lit(a['attname']), inh))
        args = ["'schemaname', 'public'::text", "'relname', 'hits'::text", "'attname', %s::text" % lit(a['attname']),
                "'inherited', %s::boolean" % inh]
        for k, t in (('null_frac', 'real'), ('avg_width', 'integer'), ('n_distinct', 'real'),
                     ('most_common_vals', 'text'), ('most_common_freqs', 'real[]'), ('histogram_bounds', 'text'),
                     ('correlation', 'real'), ('most_common_elems', 'text'), ('most_common_elem_freqs', 'real[]'),
                     ('elem_count_histogram', 'real[]')):
            v = a.get(k)
            if v is None:
                continue
            if t == 'real[]':
                val = lit('{' + ','.join(repr(float(x)) for x in v) + '}')
            elif t == 'text':
                val = lit(v)
            elif t == 'integer':
                val = '%d' % v
            else:
                val = repr(float(v))
            args.append("'%s', %s::%s" % (k, val, t))
        out.append('SELECT pg_restore_attribute_stats(%s);' % ', '.join(args))
    print('\n'.join(out))


def main(argv):
    if len(argv) >= 2 and argv[1] == 'subsets' and len(argv) in (3, 4):
        subsets(argv[2], full=len(argv) == 4 and argv[3] == 'full')
    elif len(argv) == 5 and argv[1] == 'refs':
        refs(argv[2], argv[3], argv[4])
    elif len(argv) == 5 and argv[1] == 'report':
        return report(argv[2], argv[3], argv[4])
    elif len(argv) >= 4 and argv[1] == 'save':
        save(argv[2], argv[3:])
    elif len(argv) == 4 and argv[1] == 'compare':
        return compare(argv[2], argv[3])
    elif len(argv) >= 3 and argv[1] == 'evict':
        evict(argv[2:])
    elif len(argv) == 3 and argv[1] == 'stats-sql':
        stats_sql(argv[2])
    else:
        sys.exit(__doc__)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
