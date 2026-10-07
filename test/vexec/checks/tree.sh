#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The tree check (pg_vector_executor.md §3.1 principle 11): this work
# changes Cloudberry's sources only under pg19/.  In the port's checkout
# where this work's branches are, the working tree's changes, and each branch
# of this work against the base it was made from, must name no path outside
# pg19/.  On the host.  The build's own checkout of the port (CB_SRC,
# test/vexec/checkouts.sh) holds the published branch alone, and is not the
# one this looks at.
#
#   CB_WORK_SRC   the port's checkout where this work's branches are:
#                 ../cloudberry; where there is none, the check is skipped
#   CB_BASE       the base: extension_postgresql_19
#   CB_BRANCHES   the branches of this work: the local branches named vexec*
#                 and the one checked out
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$here/../../.." && pwd)"
CB_SRC="${CB_WORK_SRC:-$ROOT/../cloudberry}"
BASE="${CB_BASE:-extension_postgresql_19}"

if ! git -C "$CB_SRC" rev-parse --git-dir > /dev/null 2>&1; then
	echo "  skipped: no checkout of the port with this work's branches at $CB_SRC (set CB_WORK_SRC)"
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

# the working trees of this work's worktrees, whose changes are not
# committed yet (V1's: ../cloudberry-vexec/wt and the readers')
git -C "$CB_SRC" worktree list --porcelain | awk '/^worktree /{w=$2} /^branch refs\/heads\/vexec/{print w}' |
while read -r wt; do
	changed=$(git -C "$wt" status --porcelain --untracked-files=all | cut -c4- | sed 's/.* -> //' | outside)
	if [ -n "$changed" ]; then
		echo "  $wt's working tree changes paths outside pg19/:"
		echo "$changed" | sed 's/^/    /' | head -20
		echo FAIL
	else
		echo "  ok $wt: $(git -C "$wt" status --porcelain --untracked-files=all | wc -l) paths changed in its working tree, all under pg19/"
	fi
done > "${TMPDIR:-/tmp}/vexec-tree-wt.$$"
cat "${TMPDIR:-/tmp}/vexec-tree-wt.$$" | grep -v '^FAIL$'
grep -q '^FAIL$' "${TMPDIR:-/tmp}/vexec-tree-wt.$$" && fail=1
rm -f "${TMPDIR:-/tmp}/vexec-tree-wt.$$"

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
