# SPDX-License-Identifier: Apache-2.0
#
# vexec, built by PGXS against an installed PostgreSQL 19 (pg_vector_executor.md
# §3.2): vanilla REL_19_STABLE, or the port's patched server.  It needs none
# of the port's modules to build, and finds them only through rendezvous
# variables at run time.  The port's headers it compiles against are copies,
# in pgxs/include, which test/vexec/checks/headers.sh keeps equal to the
# port's originals byte for byte.
#
#   make PG_CONFIG=/usr/local/pgsql/bin/pg_config
#   make PG_CONFIG=... install
#   make PG_CONFIG=... installcheck    (against a server with vexec preloaded)

MODULE_big = vexec
OBJS = \
	vexec.o \
	batch/batch.o \
	batch/convert.o \
	batch/export.o \
	batch/numeric.o \
	batch/rows.o \
	batch/types.o \
	exec/aggtrans.o \
	exec/methods.o \
	exec/node.o \
	exec/vecagg.o \
	exec/vechashjoin.o \
	exec/vecrepart.o \
	exec/vecresult.o \
	exec/vecscan.o \
	exec/vecsort.o \
	expr/compile.o \
	expr/eval.o \
	expr/k_arith.o \
	expr/k_compare.o \
	expr/k_datetime.o \
	expr/k_numeric.o \
	expr/k_text.o \
	expr/kernels.o \
	plan/agg.o \
	plan/build.o \
	plan/check.o \
	plan/cost.o \
	plan/explain.o \
	plan/join.o \
	plan/oracle.o \
	plan/orca.o \
	plan/paths.o \
	plan/reasons.o \
	plan/sort.o \
	source/heap.o \
	source/registry.o
PGFILEDESC = "vexec - a vectorized planner and executor for PostgreSQL 19"

EXTENSION = vexec
DATA = vexec--1.0.sql

# The batch-source contract, for storage modules built elsewhere: installed
# into the server's include directory, under extension/vexec/.
HEADERS = ../../include/vexec_source.h

REGRESS = vexec_settings vexec_oracle vexec_explain vexec_layouts vexec_scan vexec_agg vexec_join vexec_heap vexec_sort

PG_CPPFLAGS = -I$(srcdir) -I$(srcdir)/pgxs/include -I$(srcdir)/../../include

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
