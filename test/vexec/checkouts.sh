#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The checkouts the build uses, each a clone of its own in the cache, made
# from GitHub where there is none and moved on only when asked:
#
#   cloudberry   the Cloudberry port: the branch extension_postgresql_19 of
#                https://github.com/igor-suhorukov/cloudberry, with the two
#                submodules of PAX's that its build needs (tabulate,
#                googletest).  The port's images are built from it
#                (test/clickbench/run.sh images), and the legs mount it,
#                read-only, as the port's sources (test/vexec/run.sh
#                portbuild, orcabuild, shm, orca).
#   postgres     the port's fork of PostgreSQL: the branch
#                REL_19_STABLE_CLOUDBERRY of
#                https://github.com/igor-suhorukov/postgres, the core series
#                the port's servers are built from, and REL_19_STABLE,
#                vanilla PostgreSQL, as origin/REL_19_STABLE.  The servers of
#                ClickBench's images are built from it, and the dev images
#                take PostgreSQL's regression suite from it at their servers'
#                commits (test/vexec/run.sh images).
#
# Clones, not worktrees: the Dockerfiles take them as build contexts and
# read them with git archive, which a worktree's .git file breaks.  On the
# host.
#
#   checkouts.sh <name>          the checkout, cloned where there is none;
#                                prints its path and commit
#   checkouts.sh <name> update   fetched, and its branch moved on to
#                                origin's, a fast-forward only
#
#   CB_SRC, CB_URL, CB_BRANCH    ~/.cache/pg_accel/cloudberry,
#                                https://github.com/igor-suhorukov/cloudberry.git,
#                                extension_postgresql_19
#   PG_SRC, PG_URL, PG_BRANCH, PG_VANILLA_BRANCH
#                                ~/.cache/pg_accel/postgres,
#                                https://github.com/igor-suhorukov/postgres.git,
#                                REL_19_STABLE_CLOUDBERRY, REL_19_STABLE
set -eu

name="${1:-}"
case "$name" in
	cloudberry)
		src="${CB_SRC:-$HOME/.cache/pg_accel/cloudberry}"
		url="${CB_URL:-https://github.com/igor-suhorukov/cloudberry.git}"
		branch="${CB_BRANCH:-extension_postgresql_19}"
		also=""
		;;
	postgres)
		src="${PG_SRC:-$HOME/.cache/pg_accel/postgres}"
		url="${PG_URL:-https://github.com/igor-suhorukov/postgres.git}"
		branch="${PG_BRANCH:-REL_19_STABLE_CLOUDBERRY}"
		also="${PG_VANILLA_BRANCH:-REL_19_STABLE}"
		;;
	*)
		sed -n '3,/^set -eu/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
		exit 2
		;;
esac

die() { echo "checkouts.sh: $*" >&2; exit 1; }

if [ ! -e "$src" ]; then
	# cloned beside its place and moved there whole, so that a clone cut short
	# is never taken for the checkout
	echo "checkouts.sh: cloning $url, branch $branch${also:+ and $also}, into $src" >&2
	mkdir -p "$(dirname "$src")"
	rm -rf "$src.part"
	git clone --quiet --single-branch --branch "$branch" "$url" "$src.part" \
		|| die "could not clone $url"
	for b in $also; do
		git -C "$src.part" config --add remote.origin.fetch "+refs/heads/$b:refs/remotes/origin/$b"
	done
	if [ -n "$also" ]; then
		git -C "$src.part" fetch --quiet origin || die "could not fetch $also from $url"
	fi
	mv "$src.part" "$src"
elif [ ! -d "$src/.git" ]; then
	die "$src is not a clone: the Dockerfiles that read it need one, not a worktree"
fi

if [ "${2:-}" = update ]; then
	[ "$(git -C "$src" branch --show-current)" = "$branch" ] \
		|| die "$src is not on $branch: move it by hand"
	git -C "$src" fetch --quiet origin || die "could not fetch from $url"
	git -C "$src" merge --quiet --ff-only "origin/$branch" \
		|| die "$src's $branch does not fast-forward to origin's"
fi

if [ "$name" = cloudberry ]; then
	# PAX's submodules at the commits the checkout names: cloned the first
	# time, and moved when an update changes them
	git -C "$src" submodule --quiet update --init \
		contrib/pax_storage/src/cpp/contrib/tabulate contrib/pax_storage/src/cpp/contrib/googletest \
		|| die "could not check out PAX's submodules"
fi
echo "$src $(git -C "$src" rev-parse --short HEAD)"
