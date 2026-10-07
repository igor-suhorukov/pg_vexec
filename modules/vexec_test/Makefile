# SPDX-License-Identifier: Apache-2.0
#
# vexec_test: vexec's batch layer tested from SQL, with nanoarrow vendored
# for the export check (pg_vector_executor.md §5 V0).  A module of the tests
# alone: vexec itself never links nanoarrow.  Built as vexec is, against the
# same server; it reaches vexec's batch layer by symbol, so vexec must be
# preloaded where it runs.

MODULE_big = vexec_test
OBJS = vexec_test.o egress_test.o ipc_test.o frame_test.o nanoarrow/nanoarrow.o
PGFILEDESC = "vexec_test - vexec's batch layer, tested from SQL"

EXTENSION = vexec_test
DATA = vexec_test--1.0.sql

PG_CPPFLAGS = -I$(srcdir) -I$(srcdir)/../vexec -I$(srcdir)/../vexec/pgxs/include \
	-I$(srcdir)/../../include -DNANOARROW_NAMESPACE=VexecTest

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# nanoarrow is Arrow's code, as it is: not held to PostgreSQL's warnings.
nanoarrow/nanoarrow.o: CFLAGS += -w
