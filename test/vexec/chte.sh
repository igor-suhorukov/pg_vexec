#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# ch_table_engines' leg (ch_table_engines.md §3, VC10): vexec beside
# ch_table_engines -- ClickHouse's MergeTree family as table access methods,
# with a batch source and sink of their own (its src/vexec.c) -- on vanilla
# PostgreSQL 19, in a container of the extension's dev image, its tree at
# /chte and its engine library at /engine (docker/vexec.yml's chte):
#
#   suite         vexec's own regression suite, the extension preloaded
#                 beside vexec
#   chte          the extension's regression suite, vexec preloaded beside it
#   differential  the differential runner's sessions over the extension's
#                 tables, PostgreSQL's schedule and vexec's corpus
#                 (differential.sh's pg_mergetree and vexec_mergetree)
#   loads         a table of every type class loaded into mergetree tables of
#                 both stores by COPY, by INSERT ... SELECT through the
#                 table access method, and by VecInsert through the sink;
#                 every load's rows COPY's, and read back alike through
#                 ChScan, the plain scan and vexec's source
#
#   CHTE_PARTS    the parts run: "suite chte differential loads"
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" > "${RESULTS_DIR:-/tmp}/vexec-build.log" 2>&1 || { tail -30 "${RESULTS_DIR:-/tmp}/vexec-build.log"; exit 1; }
CHTE_SRC=/chte /chte/test/build.sh > "${RESULTS_DIR:-/tmp}/chte-build.log" 2>&1 ||
	{ tail -30 "${RESULTS_DIR:-/tmp}/chte-build.log"; exit 1; }
. "$here/lib.sh"
echo "vexec and ch_table_engines $(git -C /chte rev-parse --short HEAD 2> /dev/null || echo '?') on $("$BINDIR/pg_config" --version)"

rc=0
for part in ${CHTE_PARTS:-suite chte differential loads}; do
	case "$part" in
		suite)
			echo "== vexec's suite, ch_table_engines preloaded beside vexec"
			VEXEC_PRELOAD=vexec_testpack,vexec,vexec_test,ch_table_engines "$here/suite.sh" > "${RESULTS_DIR:-/tmp}/suite.out" 2>&1
			r=$?
			grep -E "^(ok|not ok)|^# (All|[0-9]+ of)" "${RESULTS_DIR:-/tmp}/suite.out" | grep -E "not ok|^#"
			[ $r -eq 0 ] || { rc=1; tail -60 "${RESULTS_DIR:-/tmp}/suite.out"; }
			;;
		chte)
			echo "== ch_table_engines' suite, vexec preloaded beside it"
			mkdir -p "${RESULTS_DIR:-/tmp}/chte"
			CHTE_SRC=/chte CHTE_WORK="${RESULTS_DIR:-/tmp}/chte" CHTE_PRELOAD="vexec,ch_table_engines" /chte/test/suite.sh || rc=1
			;;
		differential)
			echo "== the differential runner over mergetree tables"
			VEXEC_CORPORA="pg_mergetree vexec_mergetree" "$here/differential.sh" || rc=1
			;;
		loads)
			echo "== loads into mergetree tables, both stores"
			"$here/chte_loads.sh" || rc=1
			;;
		*)
			echo "no part $part"; rc=1 ;;
	esac
done
echo
echo "chte: $([ $rc -eq 0 ] && echo "every part passed" || echo "a part FAILED")"
exit $rc
