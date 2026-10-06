#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# FlightJdbcTest, compiled and run against the leg's endpoint (flight.sh),
# with the Flight SQL JDBC driver the image pins (/opt/jdbc).  Arrow's Java
# needs java.nio opened to it on a JDK past 16.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$(mktemp -d /tmp/jdbc-XXXXXX)"
JAR=/opt/jdbc/flight-sql-jdbc-driver.jar
javac -d "$build" -cp "$JAR" "$here/FlightJdbcTest.java" || exit 1
echo "the Flight SQL JDBC driver $(cat /opt/jdbc/VERSION)"
java --add-opens=java.base/java.nio=ALL-UNNAMED -cp "$build:$JAR" FlightJdbcTest \
	"$FLIGHT_PORT" flight flight-pass-0123456789 flight
