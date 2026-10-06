#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""ORCA's plans of two tpc runs, compared shape by shape (pg_vector_executor.md
§6.8, V5): the plans each run's TPC_PLANS kept (tpc-plans/<kind>-<query>-on.txt),
reduced to their shape -- each node's kind and its depth, without costs,
properties or figures -- vector nodes read as the row nodes they stand for, a
Hash node left out (a VecHashJoin takes it into itself), and ORCA's hashed
window, VecWindowHashAgg, named for itself where the sorted window has a Sort.

    planshapes.py <run a> <run b> [--detail]

Prints, for each query whose shape differs, what differs; and a summary of
the kinds of change: a scan's access path, a join's method or order, an
aggregation's stages, a window hashed or sorted, Motions.
"""
import os
import re
import sys

ROW_NAMES = [
    (re.compile(r'^Vec Window Hash Agg'), 'Hashed Window Partitions'),
    (re.compile(r'^Parallel Vec '), 'Parallel '),
    (re.compile(r'^Vec '), ''),
]
KINDS = [
    ('scan', re.compile(r'Scan')),
    ('join', re.compile(r'Join|Nested Loop')),
    ('aggregation', re.compile(r'Aggregate')),
    ('window', re.compile(r'WindowAgg|Hashed Window Partitions|Sort')),
    ('motion', re.compile(r'Motion')),
]


def shape(path):
    nodes = []
    with open(path) as f:
        lines = f.read().splitlines()
    for i, line in enumerate(lines):
        if i > 0 and '->' not in line:
            continue
        depth = len(line) - len(line.lstrip(' -'))
        name = line.strip()
        if name.startswith('->'):
            name = name[2:].strip()
        name = re.sub(r'  \(.*$', '', name)
        if not name or name.startswith('Optimizer') or ':' in name.split(' ')[0]:
            continue
        for rx, repl in ROW_NAMES:
            name = rx.sub(repl, name)
        if name == 'Hash':
            continue
        # a relation's alias is the plan's, a scan's relation its own
        name = re.sub(r' (on|using) (\S+)( \S+)?$', r' \1 \2', name)
        nodes.append((depth, name))
    # depths relative: a Hash left out shifts its child, so compare the
    # order of the nodes and their names
    return [n for _, n in nodes]


def plans(run):
    d = os.path.join(run, 'tpc-plans')
    if not os.path.isdir(d):
        sys.exit('no tpc-plans/ in %s: a run with TPC_PLANS=1' % run)
    out = {}
    for f in sorted(os.listdir(d)):
        m = re.match(r'(\w+)-(\w+)-on\.txt$', f)
        if m:
            out[(m.group(1), m.group(2))] = shape(os.path.join(d, f))
    return out


def kinds_changed(a, b):
    changed = []
    for kind, rx in KINDS:
        if [n for n in a if rx.search(n)] != [n for n in b if rx.search(n)]:
            changed.append(kind)
    return changed or ['other']


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    detail = '--detail' in sys.argv
    a, b = plans(sys.argv[1]), plans(sys.argv[2])
    same = 0
    differ = []
    tally = {}
    for key in sorted(set(a) & set(b)):
        if not a[key] or not b[key]:
            continue
        if a[key] == b[key]:
            same += 1
            continue
        ks = kinds_changed(a[key], b[key])
        differ.append((key, ks))
        for k in ks:
            tally[k] = tally.get(k, 0) + 1
        if detail:
            print('%s %s: %s' % (key[0], key[1], ', '.join(ks)))
            import difflib
            for line in difflib.unified_diff(a[key], b[key], lineterm='', n=1):
                if line.startswith(('---', '+++')):
                    continue
                print('    ' + line)
    print('%d plans compared: %d of the same shape, %d differ' % (same + len(differ), same, len(differ)))
    if differ:
        print('  by what differs: ' + ', '.join('%s %d' % (k, n) for k, n in sorted(tally.items())))
        print('  ' + ' '.join('%s%s' % ('H' if k == 'h' else 'DS', q.lstrip('q').lstrip('0') or '0')
                              for (k, q), _ in differ))


if __name__ == '__main__':
    main()
