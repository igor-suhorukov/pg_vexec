#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The differential runner's comparison (pg_vector_executor.md §6.1).

  differential.py <corpus dir> <session>... [--kept <dir>]

Each session is a directory of pg_regress's results/ for one corpus, run
with vexec in one configuration, beside its server's log.  The first
session is the reference -- vexec.mode = off, PostgreSQL's own executor --
and every other is compared with it, test by test:

  - the rows of every result, sorted unless the statement orders them
    (an ORDER BY in its text);
  - EXPLAIN's output is dropped: plans are where the sessions are meant to
    differ;
  - the primary error messages, as psql prints them, in order;
  - the SQLSTATEs of the errors each test raised, from the server's log,
    whose lines carry the test's application_name and the SQLSTATE
    (log_line_prefix '%a|%e|').

A difference a corpus keeps on purpose is a file <kept>/<session>/<test>.diff
holding exactly the difference the comparison prints; any other difference
fails the run.  Exits 1 when one does.
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


def normalize(path):
    """A results file with plans dropped and unordered results sorted."""
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')
    out = []
    query = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if i + 1 < n and TABLE_SEP.match(lines[i + 1]) and line.strip():
            j = i + 2
            body = []
            while j < n and not FOOTER.match(lines[j]):
                body.append(lines[j])
                j += 1
            text = ' '.join(query)
            if 'QUERY PLAN' in line or EXPLAIN.search(text):
                out.append('<plan>')
            else:
                rows = logical_rows(body)
                if not ORDER_BY.search(text):
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


def compare(reference, other):
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
        a = normalize(a_path)
        b = normalize(b_path)
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
    if '--kept' in argv:
        k = argv.index('--kept')
        kept = argv[k + 1]
        argv = argv[:k] + argv[k + 2:]
    if len(argv) < 3:
        print(__doc__)
        return 2
    corpus, sessions = argv[0], argv[1:]
    reference = os.path.join(corpus, sessions[0])
    failed = False
    for s in sessions[1:]:
        diffs, tests = compare(reference, os.path.join(corpus, s))
        unexpected = {}
        kept_ok = 0
        for t, d in diffs.items():
            kfile = os.path.join(kept, s, t + '.diff') if kept else None
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
