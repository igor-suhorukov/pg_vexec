#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The port's full run with vexec on every node against the run without it
(pg_vector_executor.md §5 V0: "the port's full run is unchanged").

  fullrun_compare.py <output without vexec> <output with vexec>

Each output is pg19/test/run.sh's: every job's output, printed whole as it
finishes under a "suite: <job> -- <outcome> in <n> s" banner, and a summary
that lists every job with its outcome: passed, skipped or FAILED.  A job is
named for its suite and, in parentheses, its pass: "pax_regress (orca)".

Unchanged is every job's outcome the same in both, and in a job that failed
in both, the same tests failing: those the job's output marks "not ok"
(pg_regress, TAP) or "NOT OK" (the port's own scripts).  Such a job is the
port's own failure, and is named with its tests.  Exits 1 when anything
differs.
"""

import re
import sys

JOB = re.compile(r'^  (?P<job>\S.*?)\s+\d+ s  (?P<result>passed|skipped|FAILED)\b')
BANNER = re.compile(r'^suite: (?P<job>.*) -- (?P<result>\w+) in \d+ s$')
NOT_OK = (re.compile(r'^\s*not ok\s+\d+\s+[-+]?\s*(?P<test>.*?)(?:\s+\d+ ms)?\s*$'),
          re.compile(r'^\s*NOT OK\s+(?P<test>.*?)(?:\s+\d+ ms)?\s*$'))


def read_run(path):
    """The run's outcomes from its summary, and each job's failing tests."""
    with open(path, errors='replace') as f:
        lines = f.read().split('\n')
    start = max((i for i, l in enumerate(lines) if re.match(r'^\d+ jobs, ', l)), default=None)
    if start is None:
        return None, None
    outcomes = {}
    for line in lines[start + 1:]:
        m = JOB.match(line)
        if m:
            outcomes[m.group('job')] = m.group('result')
    failing = {}
    job = None
    for i, line in enumerate(lines[:start]):
        m = BANNER.match(line)
        if m and i > 0 and lines[i - 1].startswith('====='):
            job = m.group('job')
            failing.setdefault(job, set())
            continue
        if job is None:
            continue
        for pattern in NOT_OK:
            t = pattern.match(line)
            if t:
                failing[job].add(t.group('test'))
                break
    return outcomes, failing


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    without, failing0 = read_run(argv[0])
    with_, failing1 = read_run(argv[1])
    if without is None or with_ is None:
        print('fullrun: a run has no summary: %s' % (argv[0] if without is None else argv[1]))
        return 1
    changed = []
    own = []
    for job in sorted(set(without) | set(with_)):
        a, b = without.get(job, 'absent'), with_.get(job, 'absent')
        if a != b:
            changed.append('  %-32s %s without vexec, %s with it' % (job, a, b))
            for t in sorted(failing1.get(job, set()) - failing0.get(job, set())):
                changed.append('  %-32s   failing with vexec only: %s' % ('', t))
        elif a == 'FAILED':
            t0, t1 = failing0.get(job, set()), failing1.get(job, set())
            if t0 != t1:
                changed.append('  %-32s FAILED in both, other tests: %s without vexec, %s with it'
                               % (job, ', '.join(sorted(t0)) or 'none named',
                                  ', '.join(sorted(t1)) or 'none named'))
            else:
                own.append('  %-32s %s' % (job, ', '.join(sorted(t0)) or 'no test named'))
    print('fullrun: %d jobs; %d passed with vexec on every node, %d skipped, %d failed'
          % (len(with_), sum(r == 'passed' for r in with_.values()),
             sum(r == 'skipped' for r in with_.values()), sum(r == 'FAILED' for r in with_.values())))
    if own:
        print('  failed without vexec too, the same tests, the port\'s own:')
        print('\n'.join(own))
    if changed:
        print('  what vexec changed:')
        print('\n'.join(changed))
        return 1
    print('  every job\'s outcome is the same as without vexec')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
