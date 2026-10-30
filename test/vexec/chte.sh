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
#   orders        groupings in a table's key's order, where its parts keep
#                 it and where they do not, and a VecSort's late columns
#                 fetched by TID, the columns named (chte_orders.sh; VC4,
#                 VC16)
#   steps         a scan's columns in steps, a later step's read only for
#                 the batches where a row is left (chte_steps.sh; VC2)
#   sink          VecInsert's batches into ClickHouse's columns directly, the
#                 pages store's parts into memory: buffered rows fetched by
#                 TID, signs, savepoints, aborts (chte_sink.sh; VC18)
#   joinkeys      a hash join's inner keys lent to the source on its outer
#                 side, which passes over the parts and granules no inner
#                 row meets (chte_joinkeys.sh; VC3)
#   condcache     the condition cache: a part's granules no row of which is
#                 true of a scan's immutable quals, passed over by the next
#                 scan of them (chte_condcache.sh; pg_vector_executor.md
#                 §3.28's F7, VC30)
#   bound         a bounded sort's running bound passing over the granules of
#                 a unit by the parts' min/max skip indexes as it moves
#                 (chte_bound.sh; pg_vector_executor.md §3.38's F25, VC43)
#   units         a parallel scan's units cut over the granules its quals
#                 keep, and its workers by the bytes a row decodes to
#                 (chte_units.sh; pg_vector_executor.md §3.38's F28, VC44)
#   sparse        a part's sparse column's rows of the default left out of the
#                 source's batches where a qual is false of the default
#                 (chte_sparse.sh; pg_vector_executor.md §3.38, VC45)
#   pool          vexec's long-lived parallel workers taken for a Gather's
#                 (chte_pool.sh; pg_vector_executor.md §3.28, F3)
#   chars         a text column read for its values' characters alone, from the
#                 column of them a storage keeps beside it (chte_chars.sh;
#                 pg_vector_executor.md §3.38's F35 and F37, VC47, VC48)
#
#   CHTE_PARTS    the parts run: "suite chte differential loads orders steps
#                 sink joinkeys condcache bound units sparse pool chars"
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" > "${RESULTS_DIR:-/tmp}/vexec-build.log" 2>&1 || { tail -30 "${RESULTS_DIR:-/tmp}/vexec-build.log"; exit 1; }
CHTE_SRC=/chte /chte/test/build.sh > "${RESULTS_DIR:-/tmp}/chte-build.log" 2>&1 ||
	{ tail -30 "${RESULTS_DIR:-/tmp}/chte-build.log"; exit 1; }
. "$here/lib.sh"
echo "vexec and ch_table_engines $(git -C /chte rev-parse --short HEAD 2> /dev/null || echo '?') on $("$BINDIR/pg_config" --version)"

rc=0
for part in ${CHTE_PARTS:-suite chte differential loads orders steps sink joinkeys condcache bound units sparse pool chars}; do
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
		orders)
			echo "== groupings in the keys' order, and late columns' fetches, over mergetree tables"
			"$here/chte_orders.sh" || rc=1
			;;
		steps)
			echo "== columns in steps over mergetree tables"
			"$here/chte_steps.sh" || rc=1
			;;
		sink)
			echo "== vexec's sink into mergetree tables"
			"$here/chte_sink.sh" || rc=1
			;;
		joinkeys)
			echo "== a hash join's keys lent to the source over mergetree tables"
			"$here/chte_joinkeys.sh" || rc=1
			;;
		condcache)
			echo "== the condition cache over mergetree tables"
			"$here/chte_condcache.sh" || rc=1
			;;
		bound)
			echo "== a running bound passing over granules of mergetree tables"
			"$here/chte_bound.sh" || rc=1
			;;
		units)
			echo "== a parallel scan's units and workers over mergetree tables"
			"$here/chte_units.sh" || rc=1
			;;
		sparse)
			echo "== a sparse column's rows of the default left out over mergetree tables"
			"$here/chte_sparse.sh" || rc=1
			;;
		pool)
			echo "== vexec's long-lived parallel workers over heap and mergetree tables"
			"$here/chte_pool.sh" || rc=1
			;;
		chars)
			echo "== text columns read for their characters alone over mergetree tables"
			"$here/chte_chars.sh" || rc=1
			;;
		*)
			echo "no part $part"; rc=1 ;;
	esac
done
echo
echo "chte: $([ $rc -eq 0 ] && echo "every part passed" || echo "a part FAILED")"
exit $rc
