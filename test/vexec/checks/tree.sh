#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The tree check (pg_vector_executor.md §3.1 principle 11): this work
# changes Cloudberry's sources only under pg19/.  In the port's checkout,
# the working tree's changes, and each branch of this work against the base
# it was made from, must name no path outside pg19/.  On the host; CB_SRC is
# the port's checkout.
#
#   CB_BASE       the base: extension_postgresql_19
#   CB_BRANCHES   the branches of this work: the local branches named vexec*
#                 and the one checked out
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$here/../../.." && pwd)"
CB_SRC="${CB_SRC:-$ROOT/../cloudberry}"
BASE="${CB_BASE:-extension_postgresql_19}"

if ! git -C "$CB_SRC" rev-parse --git-dir > /dev/null 2>&1; then
	echo "  skipped: no port checkout at $CB_SRC (set CB_SRC)"
	exit 77
fi
fail=0
outside() { grep -v -E '^pg19/' | sed '/^$/d'; }

# the working tree, staged, unstaged and untracked
changed=$(git -C "$CB_SRC" status --porcelain --untracked-files=all | cut -c4- | sed 's/.* -> //' | outside)
if [ -n "$changed" ]; then
	echo "  the port's working tree changes paths outside pg19/:"
	echo "$changed" | sed 's/^/    /' | head -20
	fail=1
fi

branches="${CB_BRANCHES:-$( (git -C "$CB_SRC" branch --format='%(refname:short)' --list 'vexec*'; git -C "$CB_SRC" branch --show-current) | sort -u)}"
for b in $branches; do
	paths=$(git -C "$CB_SRC" diff --name-only "$BASE...$b" | outside)
	if [ -n "$paths" ]; then
		echo "  $b, against $BASE, changes paths outside pg19/:"
		echo "$paths" | sed 's/^/    /' | head -20
		fail=1
	else
		echo "  ok $b: $(git -C "$CB_SRC" diff --name-only "$BASE...$b" | wc -l) paths changed against $BASE, all under pg19/"
	fi
done
echo "tree: $([ $fail -eq 0 ] && echo "nothing of Cloudberry's changed outside pg19/" || echo "FAILED")"
exit $fail
