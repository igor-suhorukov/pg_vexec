# SPDX-License-Identifier: Apache-2.0
#
# vexec_flight: Arrow Flight SQL for PostgreSQL 19, served only while vexec's
# vector executor is active (pg_vector_executor.md §3.15, V10).  Built by
# PGXS, against vanilla PostgreSQL 19 or the Cloudberry port's server, with
# vexec installed there: it compiles against vexec's installed header,
# vexec_egress.h, and finds vexec at run time through its rendezvous variable.
#
#   make PG_CONFIG=/usr/local/pgsql/bin/pg_config
#   make PG_CONFIG=... install
#
# It needs nghttp2, protobuf-c and its compiler, protobuf's well-known .proto
# files and OpenSSL (docker/Dockerfile).  The protocol's message code is
# generated at build time from Arrow's Flight.proto and FlightSql.proto
# (proto/README), into gen/.

MODULE_big = vexec_flight
OBJS = \
	src/acceptor.o \
	src/arrays.o \
	src/catalog.o \
	src/h2.o \
	src/hba.o \
	src/login.o \
	src/ingest.o \
	src/rpc.o \
	src/session.o \
	src/sql.o \
	src/tls.o \
	src/vexec_flight.o \
	gen/Flight.pb-c.o \
	gen/FlightSql.pb-c.o \
	gen/google/protobuf/timestamp.pb-c.o
PGFILEDESC = "vexec_flight - Arrow Flight SQL while vexec's vector executor is active"

PG_CONFIG ?= pg_config
PROTOC_C ?= protoc-c
PROTO_INCLUDE ?= /usr/include
VEXEC_INCLUDE ?= $(shell $(PG_CONFIG) --includedir-server)/extension/vexec

PG_CPPFLAGS = -I$(srcdir)/src -Igen -I$(VEXEC_INCLUDE)
SHLIB_LINK = -lnghttp2 -lprotobuf-c -lssl -lcrypto
EXTRA_CLEAN = gen

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# The generated code is protobuf-c's, as it is: not held to PostgreSQL's
# warnings.
GEN_OBJS = gen/Flight.pb-c.o gen/FlightSql.pb-c.o gen/google/protobuf/timestamp.pb-c.o
$(GEN_OBJS): CFLAGS += -w

gen/.stamp: $(srcdir)/proto/Flight.proto $(srcdir)/proto/FlightSql.proto $(srcdir)/proto/transform.sh
	sh $(srcdir)/proto/transform.sh $(srcdir)/proto gen
	$(PROTOC_C) -Igen -I$(PROTO_INCLUDE) --c_out=gen gen/Flight.proto gen/FlightSql.proto
	$(PROTOC_C) -I$(PROTO_INCLUDE) --c_out=gen $(PROTO_INCLUDE)/google/protobuf/timestamp.proto
	touch $@

gen/Flight.pb-c.c gen/FlightSql.pb-c.c gen/google/protobuf/timestamp.pb-c.c: gen/.stamp
	@test -f $@

$(filter src/%,$(OBJS)): gen/.stamp
