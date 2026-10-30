# SPDX-License-Identifier: Apache-2.0
#
# vexec_parquet: a table access method of Parquet files, read and written by
# Apache Arrow C++ alone, on the local filesystem or S3, with vexec's batch
# source and sink.  Built by PGXS against an installed PostgreSQL 19 --
# vanilla REL_19_STABLE -- vexec's headers in pg_accel's include/, and Arrow
# C++ with Parquet (pkg-config arrow parquet; Apache's Debian packages
# libarrow-dev and libparquet-dev, docker/Dockerfile.parquet):
#
#   make PG_CONFIG=/usr/local/pgsql/bin/pg_config
#   make PG_CONFIG=... install
#   make PG_CONFIG=... installcheck   (a server with vexec and vexec_parquet
#                                      preloaded, max_prepared_transactions
#                                      above 0: test/regress.conf's)
#
# installcheck runs the tests that need nothing else; test/vexec/parquet.sh
# runs them in an instance of its own with the rest: files of another writer
# (external_arrow, pyarrow's; encodings, their text in databases not UTF-8;
# geo, GeoParquet's read as PostGIS's types, where PostGIS is installed;
# nested, lists and maps read as arrays, hstore and jsonb), S3 (s3, moto's
# server), and the isolation specs.
#
# The C++ is engine/ alone, behind its C interface (engine/engine.h).

MODULE_big = vexec_parquet
OBJS = \
	engine/engine.o \
	src/am.o \
	src/catalog.o \
	src/convert.o \
	src/dml.o \
	src/filter.o \
	src/footers.o \
	src/functions.o \
	src/geo.o \
	src/listing.o \
	src/module.o \
	src/nested.o \
	src/options.o \
	src/read.o \
	src/scan.o \
	src/stats.o \
	src/table.o \
	src/tombstones.o \
	src/vexec.o \
	src/write.o
PGFILEDESC = "vexec_parquet - Parquet tables through Apache Arrow C++"

EXTENSION = vexec_parquet
DATA = vexec_parquet--1.0.sql

REGRESS = setup types dml mvcc options external vexec fetch footers cpu
REGRESS_OPTS = --inputdir=test --outputdir=test
ISOLATION = vpq_update vpq_locks vpq_upsert vpq_serializable
ISOLATION_OPTS = --inputdir=test --outputdir=test/isolation --load-extension=vexec_parquet
EXTRA_CLEAN = test/results test/regression.diffs test/regression.out test/isolation

VEXEC_INCLUDE ?= $(srcdir)/../../include
ARROW_CFLAGS ?= $(shell pkg-config --cflags arrow parquet arrow-s3)
ARROW_LIBS ?= $(shell pkg-config --libs arrow parquet arrow-s3)

PG_CPPFLAGS = -I$(srcdir)/src -I$(srcdir)/engine -I$(VEXEC_INCLUDE)
SHLIB_LINK = $(ARROW_LIBS) -lstdc++

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Arrow's headers need C++20; the engine names no PostgreSQL header
engine/engine.o: CXXFLAGS += -std=c++20 $(ARROW_CFLAGS)
