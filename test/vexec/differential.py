#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The differential runner's comparison (pg_vector_executor.md §6.1).

  differential.py <corpus dir> <session>... [--kept <dir>] [--keep <test>,...]

Each session is a directory of pg_regress's results/ for one corpus, run
with vexec in one configuration, beside its server's log.  The first
session is the reference -- vexec.mode = off, PostgreSQL's own executor --
and every other is compared with it, test by test:

  - the rows of every result, sorted unless the statement orders them
    (an ORDER BY in its text outside parentheses: one in a subquery, a
    CTE, a window or an aggregate orders nothing the statement returns);
  - EXPLAIN's output is dropped: plans are where the sessions are meant to
    differ;
  - the primary error messages, as psql prints them, in order;
  - the SQLSTATEs of the errors each test raised, from the server's log,
    whose lines carry the test's application_name and the SQLSTATE
    (log_line_prefix '%a|%e|').

A difference a corpus keeps on purpose is a file <kept>/<session>/<test>.diff
holding exactly the difference the comparison prints; any other difference
fails the run.  Exits 1 when one does.

--keep writes the named tests' differences, as this run finds them, into
<kept>/<session>/, once they are reviewed: never a test that is not named.

A corpus whose output varies from run to run of the same settings -- object
ids, temporary schemas' numbers -- names what varies in <kept>/volatile: a
regular expression a line, each match -- or each of its groups, when it has
them -- replaced by "#" in every session's results before they are compared;
"@<test> <expression>" for one test's results alone.
"""

import collections
import difflib
import os
import re
import sys

TABLE_SEP = re.compile(r'^-+(\+-+)*$')
FOOTER = re.compile(r'^\(\d+ rows?\)$')
ORDER_BY = re.compile(r'\border\s+by\b', re.I)
EXPLAIN = re.compile(r'^\s*explain\b', re.I)
LOG_ERROR = re.compile(r'^(?P<app>[^|]*)\|(?P<state>[0-9A-Z]{5})\|.*?\b(ERROR|FATAL|PANIC):\s+(?P<msg>.*)$')


def logical_rows(lines):
    """Rows of an aligned table, a wrapped cell's lines kept together."""
    rows, cur = [], []
    for line in lines:
        cur.append(line)
        if not line.rstrip().endswith('+'):
            rows.append('\n'.join(cur))
            cur = []
    if cur:
        rows.append('\n'.join(cur))
    return rows


def orders_rows(text):
    """Whether a statement orders its rows: an ORDER BY at its top level."""
    text = re.sub(r'--[^\n]*', ' ', text)
    text = re.sub(r"'(?:[^']|'')*'", "''", text)
    inner = re.compile(r'\([^()]*\)')
    while inner.search(text):
        text = inner.sub(' ', text)
    return bool(ORDER_BY.search(text))


def is_table_header(line, sep):
    """A result's header: a separator line as wide as the header, as psql
    prints its aligned format.  A SQL comment of dashes alone ("--") after a
    comment line is not one."""
    if not line.strip() or not TABLE_SEP.match(sep) or TABLE_SEP.match(line):
        return False
    if len(sep) == len(line):
        return True
    # a header of characters wider than one column: psql pads by display width
    return any(ord(c) > 127 for c in line) and len(sep) >= len(line.strip())


def volatile_mark(m):
    """A volatile match with "#" for it, or for each of its groups."""
    if not m.re.groups:
        return '#'
    text, at = '', m.start()
    for g in range(1, m.re.groups + 1):
        if m.start(g) < 0:
            continue
        text += m.string[at:m.start(g)] + '#'
        at = m.end(g)
    return text + m.string[at:m.end()]


def normalize(path, volatile=()):
    """A results file with plans dropped and unordered results sorted."""
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')
    for v in volatile:
        lines = [v.sub(volatile_mark, line) for line in lines]
    out = []
    query = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if i + 1 < n and is_table_header(line, lines[i + 1]):
            j = i + 2
            body = []
            while j < n and not FOOTER.match(lines[j]) and lines[j] != '':
                body.append(lines[j])
                j += 1
            text = '\n'.join(query)
            if 'QUERY PLAN' in line or EXPLAIN.search(text):
                out.append('<plan>')
            else:
                rows = logical_rows(body)
                if not orders_rows(text):
                    rows.sort()
                out.append(line)
                out.append(lines[i + 1])
                out.extend(rows)
                out.append(lines[j] if j < n else '')
            query = []
            i = j + 1
            continue
        out.append(line)
        if line.startswith(('ERROR:', 'NOTICE:', 'WARNING:', 'DETAIL:', 'HINT:', 'CONTEXT:')):
            query = []
        else:
            query.append(line)
        i += 1
    return out


def sqlstates(session_dir):
    """Per test, the SQLSTATEs and messages of its errors, from the log."""
    states = collections.defaultdict(list)
    log = os.path.join(session_dir, 'postmaster.log')
    if not os.path.exists(log):
        return states
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = LOG_ERROR.match(line.rstrip('\n'))
            if m and m.group('app').startswith('pg_regress/'):
                states[m.group('app')[len('pg_regress/'):]].append(m.group('state'))
    return states


def compare(reference, other, volatile=()):
    """The differences of one session's results from the reference's."""
    diffs = {}
    ref_res = os.path.join(reference, 'results')
    oth_res = os.path.join(other, 'results')
    ref_states = sqlstates(reference)
    oth_states = sqlstates(other)
    tests = sorted(f[:-4] for f in os.listdir(ref_res) if f.endswith('.out'))
    for t in tests:
        a_path = os.path.join(ref_res, t + '.out')
        b_path = os.path.join(oth_res, t + '.out')
        if not os.path.exists(b_path):
            diffs[t] = 'no results in this session\n'
            continue
        mine = [v for (test, v) in volatile if test in (None, t)]
        a = normalize(a_path, mine)
        b = normalize(b_path, mine)
        d = ''.join(difflib.unified_diff([x + '\n' for x in a], [x + '\n' for x in b],
                                         'reference', 'session', n=2))
        sa = sorted(ref_states.get(t, []))
        sb = sorted(oth_states.get(t, []))
        if sa != sb:
            d += 'SQLSTATEs: reference %s, session %s\n' % (
                collections.Counter(sa), collections.Counter(sb))
        if d:
            diffs[t] = d
    return diffs, tests


def main(argv):
    kept = None
    keep = set()
    if '--kept' in argv:
        k = argv.index('--kept')
        kept = argv[k + 1]
        argv = argv[:k] + argv[k + 2:]
    if '--keep' in argv:
        k = argv.index('--keep')
        keep = set(t for t in argv[k + 1].split(',') if t)
        argv = argv[:k] + argv[k + 2:]
        if kept is None:
            print('--keep needs --kept')
            return 2
    if len(argv) < 3:
        print(__doc__)
        return 2
    corpus, sessions = argv[0], argv[1:]
    reference = os.path.join(corpus, sessions[0])
    volatile = []
    vfile = os.path.join(kept, 'volatile') if kept else None
    if vfile and os.path.exists(vfile):
        with open(vfile) as f:
            for l in f:
                l = l.rstrip('\n')
                if not l.strip() or l.startswith('#'):
                    continue
                test = None
                if l.startswith('@'):
                    test, l = l[1:].split(' ', 1)
                volatile.append((test, re.compile(l)))
    failed = False
    for s in sessions[1:]:
        diffs, tests = compare(reference, os.path.join(corpus, s), volatile)
        unexpected = {}
        kept_ok = 0
        for t, d in diffs.items():
            kfile = os.path.join(kept, s, t + '.diff') if kept else None
            if t in keep:
                os.makedirs(os.path.dirname(kfile), exist_ok=True)
                with open(kfile, 'w') as f:
                    f.write(d)
                print('    %s: its difference kept in %s' % (t, kfile))
            if kfile and os.path.exists(kfile) and open(kfile).read() == d:
                kept_ok += 1
            else:
                unexpected[t] = d
        print('  %-10s %d tests: %d the same as %s, %d kept differences, %d differ' % (
            s, len(tests), len(tests) - len(diffs), sessions[0], kept_ok, len(unexpected)))
        for t, d in sorted(unexpected.items()):
            failed = True
            print('    %s differs:' % t)
            for line in d.splitlines()[:40]:
                print('      ' + line)
            out = os.path.join(corpus, s, 'differential.diffs')
            with open(out, 'a') as f:
                f.write('=== %s\n%s' % (t, d))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
