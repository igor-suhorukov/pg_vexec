# SPDX-License-Identifier: Apache-2.0
#
# pg_vexec's extensions, each built by PGXS in its own directory under
# modules/, against vanilla PostgreSQL 19 or the Cloudberry port's server:
#
#   vexec            the vectorized planner and executor
#   vexec_pgvector   its kernel pack for pgvector (VK)
#   vexec_postgis    its kernel pack for PostGIS (VK)
#   vexec_flight     Arrow Flight SQL while vexec's executor is active (V10, VI);
#                    it needs nghttp2, protobuf-c and its compiler, protobuf's
#                    well-known .proto files and OpenSSL
#
# The three beside vexec compile against vexec's headers in include/ here,
# not against the installed ones, so that a commit builds as one: they need
# vexec installed only to run.
#
#   make PG_CONFIG=/usr/local/pgsql/bin/pg_config
#   make PG_CONFIG=... install
#   make PG_CONFIG=... EXTENSIONS="vexec vexec_pgvector vexec_postgis"
#                                       (without Flight's dependencies)
#
# The tests' own modules, vexec_test and vexec_testpack, are the legs' to
# build (test/vexec/build.sh), and each module's installcheck its own.

PG_CONFIG ?= pg_config
PROTOC_C ?= protoc-c
EXTENSIONS ?= vexec vexec_pgvector vexec_postgis vexec_flight

all install uninstall clean:
	@set -e; for m in $(EXTENSIONS); do \
		if [ $$m = vexec_flight ] && [ $@ != clean ] && ! command -v $(PROTOC_C) > /dev/null; then \
			echo "vexec_flight needs $(PROTOC_C), nghttp2, protobuf-c and OpenSSL (modules/vexec_flight/README.md);" \
				"without it: make EXTENSIONS=\"$(filter-out vexec_flight,$(EXTENSIONS))\"" >&2; \
			exit 1; \
		fi; \
		$(MAKE) -C modules/$$m PG_CONFIG='$(PG_CONFIG)' PROTOC_C='$(PROTOC_C)' VEXEC_INCLUDE='$(CURDIR)/include' $@; \
	done

.PHONY: all install uninstall clean
