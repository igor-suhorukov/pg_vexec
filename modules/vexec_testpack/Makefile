# SPDX-License-Identifier: Apache-2.0
#
# vexec_testpack: a kernel pack of pg_accel's tests (pg_vector_executor.md
# §3.17, VK).  An extension whose functions are its own, and a pack that
# declares them, in one library: vexec's suite binds, evaluates, names in
# EXPLAIN and unbinds its declarations on the vanilla leg, where neither
# pgvector nor PostGIS is installed.  It includes vexec_kernels.h alone of
# vexec's, as any pack does, and links nothing of vexec's: it is preloaded
# before vexec in the suite's server, which the registry allows.

MODULE_big = vexec_testpack
OBJS = vexec_testpack.o
PGFILEDESC = "vexec_testpack - a kernel pack of vexec's tests"

EXTENSION = vexec_testpack
DATA = vexec_testpack--1.0.sql vexec_testpack--1.0--1.1.sql vexec_testpack--1.1--1.0.sql

PG_CPPFLAGS = -I$(srcdir)/../../include

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
