#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# PostgreSQL's own regression suite, its parallel_schedule against its
# unchanged expected output, on the container's server with vexec
# (pg_vector_executor.md §6.4, check 2; V0's first "done when"):
#
#   installed   vexec built and installed, and not preloaded
#   off         preloaded, vexec.mode = off
#   explain     preloaded, vexec.mode = explain: every alternative costed and
#               recorded, none chosen
#
# The suite comes from the commit the server was built from (the images'
# /cb/pgregress), and runs in a temporary instance pg_regress makes, as the
# port's singlenode suite runs it.  RESULTS_DIR keeps each mode's diffs.
#
#   VEXEC_PG_MODES       which: "installed off explain"
#   VEXEC_PG_MAXCONN     pg_regress's --max-connections: 0, its default
#   VEXEC_PG_SCHEDULE    the schedule: parallel_schedule
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$here/build.sh" || exit 1
. "$here/lib.sh"

PGSUITE="${PG_REGRESS_SUITE:-/cb/pgregress}"
PG_REGRESS="$("$BINDIR/pg_config" --pkglibdir)/pgxs/src/test/regress/pg_regress"
W="$(mktemp -d "${TMPDIR:-/tmp}/vexec-pgregress-XXXXXX")"
# what pg_regress runs a diff with must not be in /tmp if it is noexec
cd "$W" || exit 1

echo "PostgreSQL's regression suite at $(cat "$PGSUITE/.pg_ref_commit" 2>/dev/null), with vexec"
fail=0
for mode in ${VEXEC_PG_MODES:-installed off explain}; do
	conf="$W/$mode.conf"
	case "$mode" in
		installed) : > "$conf" ;;
		off) echo "shared_preload_libraries = 'vexec'" > "$conf" ;;
		explain) printf '%s\n' "shared_preload_libraries = 'vexec'" "vexec.mode = explain" > "$conf" ;;
		*) echo "unknown mode $mode"; exit 1 ;;
	esac
	start=$(date +%s)
	"$PG_REGRESS" --temp-instance="$W/$mode-instance" --temp-config="$conf" \
		--bindir="$BINDIR" --dlpath="$PGSUITE" --inputdir="$PGSUITE" --expecteddir="$PGSUITE" \
		--outputdir="$W/$mode" --schedule="$PGSUITE/${VEXEC_PG_SCHEDULE:-parallel_schedule}" \
		--encoding=UTF8 --no-locale \
		--max-connections="${VEXEC_PG_MAXCONN:-0}" > "$W/$mode.out" 2>&1
	rc=$?
	summary=$(grep -E "^# (All|[0-9]+ of [0-9]+)" "$W/$mode.out" | tail -1)
	echo "  $mode: ${summary:-no summary} ($(( $(date +%s) - start )) s)"
	if [ $rc -ne 0 ]; then
		fail=1
		grep -E "^not ok" "$W/$mode.out" | head -20
		[ -f "$W/$mode/regression.diffs" ] && head -100 "$W/$mode/regression.diffs"
	fi
	if [ -n "${RESULTS_DIR:-}" ]; then
		mkdir -p "$RESULTS_DIR/pgregress-$mode"
		cp "$W/$mode.out" "$RESULTS_DIR/pgregress-$mode/" 2>/dev/null
		cp "$W/$mode/regression.diffs" "$RESULTS_DIR/pgregress-$mode/" 2>/dev/null
		cp "$W/$mode/log/postmaster.log" "$RESULTS_DIR/pgregress-$mode/" 2>/dev/null
	fi
done
exit $fail
