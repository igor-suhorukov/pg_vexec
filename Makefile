# SPDX-License-Identifier: Apache-2.0
#
# vexec_postgis: a kernel pack of vexec's for PostGIS (pg_vector_executor.md
# §3.17, VK).  Built by PGXS, against vanilla PostgreSQL 19 or the Cloudberry
# port's server, with vexec installed there: it compiles against vexec's
# installed header, vexec_kernels.h, and registers in vexec's registry at run
# time through its rendezvous variable.  It needs nothing of PostGIS's to
# build: it reads the serialized format with code of its own (src/reader.c).
#
#   make PG_CONFIG=/usr/local/pgsql/bin/pg_config
#   make PG_CONFIG=... install
#   make PG_CONFIG=... installcheck   (a server with vexec_postgis and vexec
#                                      preloaded, PostGIS and dblink
#                                      installed: test/run.sh makes one)

MODULE_big = vexec_postgis
OBJS = src/reader.o src/vexec_postgis.o
PGFILEDESC = "vexec_postgis - vexec's kernel pack for PostGIS"

EXTENSION = vexec_postgis
DATA = vexec_postgis--1.0.sql

REGRESS = reader declarations answers plans versions
REGRESS_OPTS = --inputdir=test --outputdir=test

PG_CONFIG ?= pg_config
VEXEC_INCLUDE ?= $(shell $(PG_CONFIG) --includedir-server)/extension/vexec

PG_CPPFLAGS = -I$(srcdir)/src -I$(VEXEC_INCLUDE)
SHLIB_LINK = -lm
EXTRA_CLEAN = test/results test/regression.diffs test/regression.out

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
