#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# transform.sh <proto dir> <out dir>: Arrow's Flight.proto and
# FlightSql.proto as protobuf-c 1.5.1 compiles them (proto/README): each
# proto3 "optional" field as the one-field oneof protoc makes of it, and the
# unused "experimental" option's declaration and import dropped.
set -eu
in="$1"
out="$2"
mkdir -p "$out"
for f in Flight FlightSql; do
	sed -E \
		-e 's/^([[:space:]]*)optional ([A-Za-z0-9_.]+) ([A-Za-z0-9_]+) = ([0-9]+);/\1oneof _\3 { \2 \3 = \4; }/' \
		-e '/^import "google\/protobuf\/descriptor.proto";/d' \
		-e '/^extend google\.protobuf\.MessageOptions \{/,/^\}/d' \
		"$in/$f.proto" > "$out/$f.proto.tmp"
	mv "$out/$f.proto.tmp" "$out/$f.proto"
done
