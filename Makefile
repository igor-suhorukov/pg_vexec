# SPDX-License-Identifier: Apache-2.0
#
# vexec_pgvector: a kernel pack of vexec's for pgvector (pg_vector_executor.md
# §3.17, VK).  Built by PGXS, against vanilla PostgreSQL 19 or the Cloudberry
# port's server, with vexec installed there: it compiles against vexec's
# installed header, vexec_kernels.h, and registers in vexec's registry at run
# time through its rendezvous variable.  It needs nothing of pgvector's to
# build: it reads a vector's dimensions from the layout pgvector documents.
#
#   make PG_CONFIG=/usr/local/pgsql/bin/pg_config
#   make PG_CONFIG=... install
#   make PG_CONFIG=... installcheck   (a server with vexec_pgvector and vexec
#                                      preloaded, pgvector and dblink
#                                      installed: test/run.sh makes one)

MODULE_big = vexec_pgvector
OBJS = src/vexec_pgvector.o
PGFILEDESC = "vexec_pgvector - vexec's kernel pack for pgvector"

EXTENSION = vexec_pgvector
DATA = vexec_pgvector--1.0.sql

REGRESS = declarations answers versions
REGRESS_OPTS = --inputdir=test --outputdir=test

PG_CONFIG ?= pg_config
VEXEC_INCLUDE ?= $(shell $(PG_CONFIG) --includedir-server)/extension/vexec

PG_CPPFLAGS = -I$(VEXEC_INCLUDE)
EXTRA_CLEAN = test/results test/regression.diffs test/regression.out

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
