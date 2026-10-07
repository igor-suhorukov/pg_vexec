#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The notices check (pg_vector_executor.md §3.1 principle 10; V0): every
# file of vexec and its tests carries pg_accel's notice,
# SPDX-License-Identifier: Apache-2.0, and no notice but those of the code
# §3.1 lets it copy, each in its own file, which the list below names:
#
#   modules/vexec/batch/arrow_abi.h     Arrow's C Data Interface, which Arrow
#                                       asks projects to copy (Apache-2.0)
#   modules/vexec/pgxs/include/*.h      the port's headers (Apache-2.0)
#   modules/vexec_test/nanoarrow/*      nanoarrow, the tests' alone (Apache-2.0)
#   modules/vexec_flight/src/arrow_abi.h, modules/vexec_flight/proto/*.proto
#                                       Arrow's C Data Interface, and Flight's
#                                       and Flight SQL's protocols, with
#                                       Arrow's NOTICE in proto/README
#                                       (Apache-2.0)
#
# and one file of both notices, Apache-2.0 AND PostgreSQL, PostgreSQL's
# functions copied into it carrying PostgreSQL's:
#
#   modules/vexec_flight/src/hba.c      hba.c's matching of a login against
#                                       pg_hba.conf's lines (PostgreSQL)
#
# No file may carry another's copyright or licence: openGauss, Hydra's and
# Citus's columnar and TimescaleDB's tsl/ are read for ideas, never copied
# (principle 10).  On the host.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$here/../../.." && pwd)"
cd "$ROOT" || exit 1

third_party() {
	case "$1" in
		modules/vexec/batch/arrow_abi.h|modules/vexec/pgxs/include/*.h|modules/vexec_test/nanoarrow/*) return 0 ;;
		modules/vexec_flight/src/arrow_abi.h|modules/vexec_flight/proto/*.proto|modules/vexec_flight/proto/README) return 0 ;;
	esac
	return 1
}

postgresql_portions() {
	[ "$1" = modules/vexec_flight/src/hba.c ]
}

fail=0
n=0
while IFS= read -r f; do
	n=$((n + 1))
	if third_party "$f"; then
		if ! grep -q "Licensed to the Apache Software Foundation" "$f" && [ "$(basename "$f")" != README ]; then
			echo "  $f: a copied file without its Apache-2.0 notice"
			fail=1
		fi
		continue
	fi
	if postgresql_portions "$f"; then
		if ! head -5 "$f" | grep -q "SPDX-License-Identifier: Apache-2.0 AND PostgreSQL" \
			|| ! grep -q "Portions Copyright (c) 1996-[0-9]*, PostgreSQL Global Development Group" "$f"; then
			echo "  $f: PostgreSQL's portions without both notices"
			fail=1
		fi
		continue
	fi
	if ! head -5 "$f" | grep -q "SPDX-License-Identifier: Apache-2.0"; then
		echo "  $f: no SPDX-License-Identifier: Apache-2.0 in its first lines"
		fail=1
	fi
	# this check names the notices it looks for
	[ "$f" = test/vexec/checks/notices.sh ] && continue
	if grep -n -i -E "copyright|licensed under|licensed to|GNU (Affero |Lesser )?General Public|AGPL|Timescale License|Huawei|openGauss Global" "$f" \
		| grep -v -i -E "openGauss's|openGauss (is|was|and|,)|from reading openGauss|reading openGauss|openGauss-server/" ; then
		echo "  $f: a notice that is not pg_accel's"
		fail=1
	fi
done < <(git ls-files --cached --others --exclude-standard modules include test/vexec docker/Dockerfile.vexec docker/Dockerfile.vanillaorca docker/vexec.yml \
	| grep -v -E '/(expected|results)/|\.out$|/(corpus-)?kept/|/(README\.md|\.gitignore)$')
echo "notices: $n files, $([ $fail -eq 0 ] && echo "each with its notice and no other" || echo "some failed")"
exit $fail
