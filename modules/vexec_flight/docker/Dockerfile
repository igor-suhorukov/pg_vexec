# syntax=docker/dockerfile:1.7
# SPDX-License-Identifier: Apache-2.0
#
# vexec_flight's dev image: vexec's dev image with pyarrow (pg_accel's
# docker/Dockerfile.arrow), and what vexec_flight builds and is tested with.
#
#   to build      nghttp2, protobuf-c and its compiler, protobuf's own .proto
#                 files for the well-known types (google/protobuf/*.proto),
#                 OpenSSL's headers
#   to test       openssl, for the tests' certificates; the clients the plan
#                 names (pg_vector_executor.md §5, V10): adbc_driver_flightsql
#                 and adbc_driver_postgresql in pyarrow's environment, and the
#                 Flight SQL JDBC driver with a JDK to run it
#
# Everything is pinned, so that a run names what it ran against.
#
#   BASE_IMAGE    pg_accel/vexec-dev:vanilla-arrow, or the port's
#                 pg_accel/vexec-dev:port-arrow-<commit>
#   DUCKDB_IMAGE  an image of the port with /opt/duckdb, DuckDB and its tpch
#                 extension, as the port pins them: pg_accel/cb-ext:<commit>,
#                 for the bench's TPC-H data
#   ADBC_VERSION  1.12.0, the newest on 2026-10-06
#   JDBC_VERSION  19.0.0 (org.apache.arrow:flight-sql-jdbc-driver), the newest
#                 on 2026-10-06

ARG BASE_IMAGE=pg_accel/vexec-dev:vanilla-arrow
ARG DUCKDB_IMAGE=pg_accel/cb-ext:unset

FROM ${DUCKDB_IMAGE} AS duckdb

FROM ${BASE_IMAGE}
USER root
COPY --from=duckdb /opt/duckdb /opt/duckdb
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        libnghttp2-dev libprotobuf-c-dev protobuf-c-compiler libprotobuf-dev libssl-dev \
        openssl curl openjdk-21-jdk-headless \
    && rm -rf /var/lib/apt/lists/*
ARG ADBC_VERSION=1.12.0
RUN /opt/arrow/bin/pip install --no-cache-dir \
        "adbc-driver-manager==${ADBC_VERSION}" \
        "adbc-driver-flightsql==${ADBC_VERSION}" \
        "adbc-driver-postgresql==${ADBC_VERSION}" \
    && /opt/arrow/bin/python -c 'import adbc_driver_flightsql, adbc_driver_postgresql; print("adbc", adbc_driver_flightsql.__version__)'
ARG JDBC_VERSION=19.0.0
RUN mkdir -p /opt/jdbc && cd /opt/jdbc \
    && base="https://repo1.maven.org/maven2/org/apache/arrow/flight-sql-jdbc-driver/${JDBC_VERSION}" \
    && curl -fsSL -o flight-sql-jdbc-driver.jar "$base/flight-sql-jdbc-driver-${JDBC_VERSION}.jar" \
    && curl -fsSL -o flight-sql-jdbc-driver.jar.sha1 "$base/flight-sql-jdbc-driver-${JDBC_VERSION}.jar.sha1" \
    && echo "$(cat flight-sql-jdbc-driver.jar.sha1)  flight-sql-jdbc-driver.jar" | sha1sum -c - \
    && echo "${JDBC_VERSION}" > VERSION
USER postgres
