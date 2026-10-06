#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""vexec's Arrow IPC codec against Arrow's own implementation, pyarrow
(pg_vector_executor.md §5, V7_0).  test/vexec/ipc.sh runs it against a
server with vexec preloaded, vexec_test made and the layouts' corpus loaded.

  read    pyarrow reads every stream vexec_test.ipc_stream makes of the corpus,
          in both formats and several layout settings: full validation; each
          field's type against the type the export's format string names, by
          a mapping of this file's own, so that a mistake the writer and the
          reader share is caught; and every value against PostgreSQL's own
          bytes, vexec_test.raw(), and numeric's text
  write   streams pyarrow writes of every type the codec reads, read and
          written back by vexec_test.ipc_reencode, and read by pyarrow again:
          equal to what pyarrow read from its own, metadata included
  fuzz    valid streams cut short, with bytes changed, and with lengths and
          offsets of their flatbuffers and bodies changed, through
          ipc_reencode: each must read, and its output pass pyarrow's full
          validation, or fail with SQLSTATE 22P03 or 0A000

Bytes go to the server as files it reads with pg_read_binary_file(), and come
back hex-encoded through psql.
"""
import argparse
import ctypes
import decimal
import os
import random
import re
import struct
import subprocess
import sys

import pyarrow as pa

ARGS = None
FAILURES = []
decimal.getcontext().prec = 200


def printable(msg):
    return "".join(c if c.isprintable() else "?" for c in msg)


def fail(msg):
    FAILURES.append(msg)
    print("  FAIL " + printable(msg), flush=True)


def psql(sql):
    r = subprocess.run([ARGS.psql, "-X", "-q", "-At", "-v", "ON_ERROR_STOP=1", "-F", "\x1f",
                        "-h", ARGS.host, "-U", "postgres", "-d", ARGS.db, "-c", sql],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip())
    return [line.split("\x1f") for line in r.stdout.splitlines()]


def lit(s):
    return "'" + s.replace("'", "''") + "'"


# ---- the C Data Interface's format strings, mapped here on their own ----

UNITS = {"s": "s", "m": "ms", "u": "us", "n": "ns"}
SIMPLE = {
    "n": pa.null(), "b": pa.bool_(), "c": pa.int8(), "C": pa.uint8(), "s": pa.int16(),
    "S": pa.uint16(), "i": pa.int32(), "I": pa.uint32(), "l": pa.int64(), "L": pa.uint64(),
    "e": pa.float16(), "f": pa.float32(), "g": pa.float64(), "z": pa.binary(),
    "Z": pa.large_binary(), "u": pa.string(), "U": pa.large_string(), "vz": pa.binary_view(),
    "vu": pa.string_view(), "tdD": pa.date32(), "tdm": pa.date64(), "tts": pa.time32("s"),
    "ttm": pa.time32("ms"), "ttu": pa.time64("us"), "ttn": pa.time64("ns"),
    "tin": pa.month_day_nano_interval(),
}


def format_type(fmt):
    """The Arrow type a flat format string names (CDataInterface.rst:95-250)."""
    if fmt in SIMPLE:
        return SIMPLE[fmt]
    if fmt.startswith("d:"):
        p, s, *bw = (int(x) for x in fmt[2:].split(","))
        bits = bw[0] if bw else 128
        return {32: pa.decimal32, 64: pa.decimal64, 128: pa.decimal128, 256: pa.decimal256}[bits](p, s)
    if fmt.startswith("w:"):
        return pa.binary(int(fmt[2:]))
    if fmt.startswith("ts") and fmt[3] == ":":
        return pa.timestamp(UNITS[fmt[2]], fmt[4:] or None)
    if fmt.startswith("tD"):
        return pa.duration(UNITS[fmt[2]])
    raise ValueError("format " + fmt)


def storage_type(t):
    return t.storage_type if isinstance(t, pa.BaseExtensionType) else t


def slots(arr):
    """Each row's value, None for NULL: a fixed-width slot's bytes, a
    variable-width value's bytes, a bool as the byte PostgreSQL holds."""
    if isinstance(arr, pa.ExtensionArray):
        arr = arr.storage
    t = arr.type
    nulls = arr.is_null().to_pylist()
    if pa.types.is_boolean(t):
        return [None if v is None else bytes([v]) for v in arr.to_pylist()]
    if (pa.types.is_binary(t) or pa.types.is_string(t) or pa.types.is_binary_view(t) or
            pa.types.is_string_view(t) or pa.types.is_large_binary(t) or
            pa.types.is_large_string(t)):
        return [v.encode() if isinstance(v, str) else v for v in arr.to_pylist()]
    w = t.bit_width // 8
    buf = arr.buffers()[1].to_pybytes()
    return [None if nulls[i] else buf[(arr.offset + i) * w:(arr.offset + i + 1) * w]
            for i in range(len(arr))]


def expected(fmt, raw, text):
    """What Arrow holds of a value PostgreSQL holds as raw, under a format."""
    if fmt == "tdD":
        return struct.pack("<i", struct.unpack("<i", raw)[0] + 10957)
    if fmt.startswith("tsu:"):
        return struct.pack("<q", struct.unpack("<q", raw)[0] + 946684800000000)
    if fmt == "tin":
        time, day, month = struct.unpack("<qii", raw)
        return struct.pack("<iiq", month, day, time * 1000)
    if fmt.startswith("d:"):
        _, s, *bw = (int(x) for x in fmt[2:].split(","))
        v = decimal.Decimal(text).scaleb(s)
        if v != v.to_integral_value():
            return b"not a value of scale %d" % s
        return int(v).to_bytes((bw[0] if bw else 128) // 8, "little", signed=True)
    return raw


# ---- read: pyarrow reads vexec's streams ----

CONFIGS = [
    ("postgres", "format", "format", "format", "format"),
    ("arrow", "format", "format", "format", "format"),
    ("postgres", "offsets", "bit", "arrow", "varlena"),
    ("arrow", "offsets", "byte", "postgres", "scaled"),
    ("arrow", "datum", "format", "arrow", "varlena"),
    ("postgres", "view", "format", "postgres", "scaled"),
]
QUERIES = [
    ("*", ""),
    # Arrow's own date32, time64, timestamps and month_day_nano
    ("d, t, ts, tstz, iv", "WHERE id % 50 NOT BETWEEN 15 AND 29"),
]


def check_read():
    print("== read: pyarrow reads vexec's streams of the corpus", flush=True)
    attrs = psql("SELECT attname, atttypid::regtype::text FROM pg_attribute "
                 "WHERE attrelid = 'corpus'::regclass AND attnum > 0 ORDER BY attnum")
    typeof = dict((a[0], a[1]) for a in attrs)
    nvalues = 0
    for cols, where in QUERIES:
        names = [a[0] for a in attrs] if cols == "*" else [c.strip() for c in cols.split(",")]
        exprs = []
        for c in names:
            exprs.append("coalesce(encode(vexec_test.raw(%s), 'hex'), 'N')" % c)
            exprs.append("coalesce(%s::text, 'N')" % c if typeof[c] == "numeric" else "''")
        rows = psql("SELECT %s FROM corpus %s ORDER BY id" % (", ".join(exprs), where))
        want = [[(None if r[2 * j] == "N" else bytes.fromhex(r[2 * j]), r[2 * j + 1])
                 for r in rows] for j in range(len(names))]
        query = "SELECT %s FROM corpus %s ORDER BY id" % (cols, where)
        for cfg in CONFIGS:
            streams = psql("SELECT encode(stream, 'hex'), array_to_string(formats, ' '), batches, rows "
                           "FROM vexec_test.ipc_stream(%s, %s)" % (lit(query), ", ".join(map(lit, cfg))))
            row = 0
            nbatches = 0
            try:
                for hexed, formats, batches, nrows in streams:
                    formats = formats.split(" ")
                    reader = pa.ipc.open_stream(bytes.fromhex(hexed))
                    schema = reader.schema
                    for j, f in enumerate(schema):
                        if f.name != names[j] or not f.nullable:
                            fail("%s %s: field %d is %s" % (query, cfg, j + 1, f))
                        if storage_type(f.type) != format_type(formats[j]):
                            fail("%s %s: field %s is %s, and its format %s" %
                                 (query, cfg, f.name, f.type, formats[j]))
                    for batch in reader:
                        batch.validate(full=True)
                        nbatches += 1
                        for j in range(batch.num_columns):
                            got = slots(batch.column(j))
                            for i, v in enumerate(got):
                                raw, text = want[j][row + i]
                                ok = (v is None) if raw is None else (
                                    v is not None and v == expected(formats[j], raw, text))
                                nvalues += 1
                                if not ok:
                                    fail("%s %s: column %s row %d is %r, PostgreSQL's %r (%s)" %
                                         (query, cfg, names[j], row + i, v, raw, text))
                                    break
                        row += batch.num_rows
                if row != len(rows):
                    fail("%s %s: %d rows, PostgreSQL's %d" % (query, cfg, row, len(rows)))
            except Exception as e:
                fail("%s %s: %s: %s" % (query, cfg, type(e).__name__, e))
            print("  %-60s %s: %d streams, %d batches, %d rows" %
                  (query[:60], "/".join(cfg), len(streams), nbatches, row), flush=True)
    print("  %d values compared with PostgreSQL's" % nvalues, flush=True)


# ---- write: pyarrow's streams of every type, through vexec and back ----

class ArrowSchemaC(ctypes.Structure):
    pass


class ArrowArrayC(ctypes.Structure):
    pass


SREL = ctypes.CFUNCTYPE(None, ctypes.POINTER(ArrowSchemaC))
AREL = ctypes.CFUNCTYPE(None, ctypes.POINTER(ArrowArrayC))
ArrowSchemaC._fields_ = [
    ("format", ctypes.c_char_p), ("name", ctypes.c_char_p), ("metadata", ctypes.c_char_p),
    ("flags", ctypes.c_int64), ("n_children", ctypes.c_int64),
    ("children", ctypes.POINTER(ctypes.POINTER(ArrowSchemaC))), ("dictionary", ctypes.c_void_p),
    ("release", SREL), ("private_data", ctypes.c_void_p)]
ArrowArrayC._fields_ = [
    ("length", ctypes.c_int64), ("null_count", ctypes.c_int64), ("offset", ctypes.c_int64),
    ("n_buffers", ctypes.c_int64), ("n_children", ctypes.c_int64),
    ("buffers", ctypes.POINTER(ctypes.c_void_p)),
    ("children", ctypes.POINTER(ctypes.POINTER(ArrowArrayC))), ("dictionary", ctypes.c_void_p),
    ("release", AREL), ("private_data", ctypes.c_void_p)]
KEEP = []


def _srelease(p):
    p.contents.release = SREL()


def _arelease(p):
    p.contents.release = AREL()


SRELEASE = SREL(_srelease)
ARELEASE = AREL(_arelease)


def intervals_batch():
    """month and day_time intervals, which pyarrow makes only through the C
    Data Interface: a record batch imported from structures made here."""
    def schema(fmt, name, children=()):
        s = ArrowSchemaC(format=fmt.encode(), name=name.encode(), metadata=None, flags=2,
                         n_children=len(children), dictionary=None, release=SRELEASE)
        kids = (ctypes.POINTER(ArrowSchemaC) * max(1, len(children)))(*map(ctypes.pointer, children))
        s.children = kids
        KEEP.extend([s, kids])
        return s

    def array(length, nulls, bufs, children=()):
        held = [None if b is None else ctypes.create_string_buffer(b, len(b)) for b in bufs]
        ptrs = (ctypes.c_void_p * max(1, len(bufs)))(
            *[None if b is None else ctypes.addressof(b) for b in held])
        kids = (ctypes.POINTER(ArrowArrayC) * max(1, len(children)))(*map(ctypes.pointer, children))
        a = ArrowArrayC(length=length, null_count=nulls, offset=0, n_buffers=len(bufs),
                        n_children=len(children), buffers=ptrs, children=kids, dictionary=None,
                        release=ARELEASE)
        KEEP.extend(held + [a, ptrs, kids])
        return a

    months = array(4, 1, [bytes([0b1011]), struct.pack("<4i", 1, -2, 0, 2 ** 31 - 1)])
    daytime = array(4, 0, [None, struct.pack("<8i", 1, 1000, -2, 5, 0, 0, -(2 ** 31), 86399999)])
    root = array(4, 0, [None], [months, daytime])
    s = schema("+s", "", [schema("tiM", "months"), schema("tiD", "day_time")])
    return pa.RecordBatch._import_from_c(ctypes.addressof(root), ctypes.addressof(s))


def bits(valid):
    out = bytearray((len(valid) + 7) // 8)
    for i, v in enumerate(valid):
        if v:
            out[i // 8] |= 1 << (i % 8)
    return bytes(out)


def float16(values):
    data = b"".join(struct.pack("<e", 0.0 if v is None else v) for v in values)
    valid = [v is not None for v in values]
    return pa.Array.from_buffers(pa.float16(), len(values), [pa.py_buffer(bits(valid)), pa.py_buffer(data)],
                                 null_count=valid.count(False))


class Point(pa.ExtensionType):
    """An extension type of these tests' own, over a struct."""

    def __init__(self):
        super().__init__(pa.struct([("x", pa.float64()), ("y", pa.float64())]), "vexec.test.point")

    def __arrow_ext_serialize__(self):
        return b'{"crs": "none"}'

    @classmethod
    def __arrow_ext_deserialize__(cls, storage_type, serialized):
        if storage_type != Point().storage_type:
            raise TypeError("vexec.test.point over %s" % storage_type)
        return Point()


def batch(**cols):
    return pa.record_batch(list(cols.values()), names=list(cols.keys()))


def write_cases():
    """(name, schema, batches, write options) of every type the codec reads."""
    long = ["a value of more than twelve bytes, number %d" % i for i in range(300)]
    views = pa.concat_arrays([pa.array(["short", None, long[0], "", "twelve bytes"], pa.string_view()),
                              pa.array(long[1:200] + ["äöü €" * 4, None], pa.string_view())])
    bviews = pa.concat_arrays([pa.array([b"\x00\xff" * 9, None, b"inline"], pa.binary_view()),
                               pa.array([b"\x01" * 20, b"", b"\x02" * 100] * 67 + [None, b"\x03" * 13],
                                        pa.binary_view())])
    ints = batch(
        i8=pa.array([-128, 127, None, 0], pa.int8()), u8=pa.array([0, 255, None, 1], pa.uint8()),
        i16=pa.array([-2 ** 15, 2 ** 15 - 1, None, 0], pa.int16()),
        u16=pa.array([0, 2 ** 16 - 1, None, 1], pa.uint16()),
        i32=pa.array([-2 ** 31, 2 ** 31 - 1, None, 0], pa.int32()),
        u32=pa.array([0, 2 ** 32 - 1, None, 1], pa.uint32()),
        i64=pa.array([-2 ** 63, 2 ** 63 - 1, None, 0], pa.int64()),
        u64=pa.array([0, 2 ** 64 - 1, None, 1], pa.uint64()),
        f16=float16([1.5, None, -0.0, 65504.0]),
        f32=pa.array([float("nan"), -0.0, None, float("inf")], pa.float32()),
        f64=pa.array([1e308, -0.0, None, float("-inf")], pa.float64()),
        b=pa.array([True, False, None, True]), n=pa.nulls(4))
    strings = batch(
        bin=pa.array([b"", b"\x00\xff", None, b"x" * 100]),
        str=pa.array(["", "äöü €", None, "nul \x00 inside"]),
        lbin=pa.array([b"a", None, b"", b"\xfe" * 30], pa.large_binary()),
        lstr=pa.array(["a", None, "", "large"], pa.large_string()),
        fsb=pa.array([b"abc", None, b"\x00\x00\x00", b"xyz"], pa.binary(3)),
        fsb0=pa.array([b"", b"", None, b""], pa.binary(0)))
    decimals = batch(
        d32=pa.array([decimal.Decimal("1234567.89"), None, decimal.Decimal("-0.01"), 0], pa.decimal32(9, 2)),
        d64=pa.array([decimal.Decimal("-999999999999999.999"), None, 1, 0], pa.decimal64(18, 3)),
        d128=pa.array([decimal.Decimal("12345678901234567890.0123456789"), None,
                       decimal.Decimal("-1E-10"), 0], pa.decimal128(38, 10)),
        d256=pa.array([decimal.Decimal("9" * 38 + "." + "9" * 38), None, decimal.Decimal("-1"), 0],
                      pa.decimal256(76, 38)),
        dneg=pa.array([decimal.Decimal("1E+5"), None, decimal.Decimal("-2E+3"), 0], pa.decimal128(5, -2)))
    temporal = batch(
        d32=pa.array([0, -719162, None, 2932896], pa.date32()),
        d64=pa.array([0, 86400000 * -5, None, 86400000 * 365], pa.date64()),
        t32s=pa.array([0, 86399, None, 1], pa.time32("s")),
        t32m=pa.array([0, 86399999, None, 1], pa.time32("ms")),
        t64u=pa.array([0, 86399999999, None, 1], pa.time64("us")),
        t64n=pa.array([0, 86399999999999, None, 1], pa.time64("ns")),
        tss=pa.array([0, -2 ** 40, None, 2 ** 40], pa.timestamp("s")),
        tsm=pa.array([0, -2 ** 50, None, 1], pa.timestamp("ms", "UTC")),
        tsu=pa.array([0, 2 ** 62, None, -1], pa.timestamp("us", "Europe/Paris")),
        tsn=pa.array([0, -2 ** 63 + 1, None, 2 ** 63 - 1], pa.timestamp("ns", "+07:30")),
        dus=pa.array([0, -1, None, 2 ** 63 - 1], pa.duration("s")),
        dum=pa.array([0, -1, None, 1], pa.duration("ms")),
        duu=pa.array([0, -1, None, 1], pa.duration("us")),
        dun=pa.array([0, -1, None, 1], pa.duration("ns")),
        mdn=pa.array([(1, 2, 3), None, (-1, -2, -3), (2 ** 31 - 1, -2 ** 31, 2 ** 63 - 1)],
                     pa.month_day_nano_interval()))
    lists = pa.array([[1, None, 3], None, [], [4]], pa.list_(pa.int32()))
    nested = batch(
        lst=lists,
        llst=pa.array([["a", None], None, [], ["bcd" * 10]], pa.large_list(pa.string())),
        fsl=pa.array([[1, 2, 3], None, [None, 5, 6], [7, 8, 9]], pa.list_(pa.int16(), 3)),
        st=pa.array([{"a": 1, "b": ["x", None]}, None, {"a": None, "b": None}, {"a": 4, "b": []}],
                    pa.struct([("a", pa.int32()), ("b", pa.list_(pa.string_view()))])),
        mp=pa.array([[("a", 1), ("b", None)], None, [], [("c", 3)]],
                    pa.map_(pa.string(), pa.int32(), keys_sorted=True)),
        mpu=pa.array([[("z", 1.5)], [], None, [("y", None), ("x", 2.0)]], pa.map_(pa.string(), pa.float64())),
        deep=pa.array([[{"x": decimal.Decimal("1.5"), "y": [[1, 2], None]}], None, [None],
                       [{"x": None, "y": []}]],
                      pa.list_(pa.struct([("x", pa.decimal128(10, 2)),
                                          ("y", pa.list_(pa.list_(pa.int64())))]))),
        empties=pa.array([[], [], None, []], pa.list_(pa.list_(pa.bool_()))))
    dense = pa.UnionArray.from_dense(
        pa.array([5, 17, 5, 2, 17, 2], pa.int8()), pa.array([0, 0, 1, 0, 1, 1], pa.int32()),
        [pa.array([1, None], pa.int32()), pa.array(["a", "a long string of a union"]),
         pa.array([[True], None], pa.list_(pa.bool_()))],
        ["i", "s", "l"], [5, 17, 2])
    sparse = pa.UnionArray.from_sparse(
        pa.array([3, 7, 7, 3, 3, 7], pa.int8()),
        [pa.array([1.5, None, 0.0, -1.0, None, 2.0]), pa.array([b"x", b"y", None, b"", b"z", b"w"])],
        ["f", "b"], [3, 7])
    unions = batch(dense=dense, sparse=sparse)
    sliced = batch(
        b=pa.array([True, False, None, True, True, False, None, True, False, True], pa.bool_()).slice(3, 5),
        s=pa.array(["zero", "one", None, "three", "four", "five", "six", "seven"]).slice(2, 5),
        sv=views.slice(1, 5),
        l=pa.array([[0], [1, 1], None, [3, 3, 3], [], [5], [6, 6], [7]], pa.list_(pa.int8())).slice(2, 5),
        st=pa.array([{"a": i, "b": str(i)} if i % 3 else None for i in range(8)],
                    pa.struct([("a", pa.int64()), ("b", pa.string())])).slice(3, 5),
        d=dense.slice(1, 5))
    meta_schema = pa.schema([pa.field("x", pa.int32(), metadata={"k": "v", "ünï": "çödé", "empty": ""}),
                             pa.field("y", pa.string(), nullable=False, metadata={b"bin\x00key": b"\xff\x00"})],
                            metadata={"schema key": "schema value", "ARROW:x": "y"})
    meta = pa.record_batch([pa.array([1, None, 3], pa.int32()), pa.array(["a", "b", "c"])], schema=meta_schema)
    ext = batch(
        uuid=pa.ExtensionArray.from_storage(pa.uuid(), pa.array([b"\x01" * 16, None, b"\x02" * 16], pa.binary(16))),
        point=pa.ExtensionArray.from_storage(Point(), pa.array(
            [{"x": 1.0, "y": 2.0}, None, {"x": -1.0, "y": None}], Point().storage_type)))
    allnull = batch(
        i=pa.nulls(5, pa.int32()), s=pa.nulls(5, pa.string()), sv=pa.nulls(5, pa.string_view()),
        l=pa.nulls(5, pa.list_(pa.int32())), st=pa.nulls(5, pa.struct([("a", pa.int8())])),
        d=pa.nulls(5, pa.decimal128(5, 2)), n=pa.nulls(5))
    cases = []
    for name, b in [("integers, floats, bool and null", ints), ("binary and utf8", strings),
                    ("views", batch(sv=views, bv=bviews)), ("decimals", decimals),
                    ("dates, times, timestamps, durations", temporal),
                    ("month and day_time intervals", intervals_batch()), ("lists, structs and maps", nested),
                    ("unions", unions), ("sliced arrays", sliced), ("metadata", meta),
                    ("extension types", ext), ("all NULL", allnull)]:
        empty = b.slice(0, 0)
        cases.append((name, b.schema, [b, empty, b], {}))
    cases.append(("no batch at all", ints.schema, [], {}))
    cases.append(("metadata V4", ints.schema, [ints], {"metadata_version": pa.ipc.MetadataVersion.V4}))
    cases.append(("unions, metadata V4", unions.schema, [unions], {"metadata_version": pa.ipc.MetadataVersion.V4}))
    cases.append(("the format before 0.15", strings.schema, [strings], {"use_legacy_format": True}))
    many = [batch(i=pa.array(range(k, k + 100)), s=pa.array([str(x) * (x % 7) for x in range(k, k + 100)]))
            for k in range(0, 1000, 100)]
    cases.append(("ten batches", many[0].schema, many, {}))
    return cases


def stream_of(schema, batches, opts):
    sink = pa.BufferOutputStream()
    with pa.ipc.new_stream(sink, schema, options=pa.ipc.IpcWriteOptions(**opts) if opts else None) as w:
        for b in batches:
            w.write_batch(b)
    return sink.getvalue().to_pybytes()


def same_array(x, y):
    """Equal, or equal bit for bit where equality says no: NaN."""
    if x.equals(y):
        return True
    one = lambda a: pa.record_batch([a], names=["x"]).serialize().to_pybytes()
    return x.type == y.type and one(x) == one(y)


def reencode(path, aligned):
    rows = psql("SELECT encode(vexec_test.ipc_reencode(pg_read_binary_file(%s), %s), 'hex')"
                % (lit(path), "true" if aligned else "false"))
    return bytes.fromhex(rows[0][0])


def check_write():
    print("== write: pyarrow's streams through vexec's reader and writer", flush=True)
    types = set()
    for n, (name, schema, batches, opts) in enumerate(write_cases()):
        data = stream_of(schema, batches, opts)
        path = os.path.join(ARGS.work, "write-%02d.arrows" % n)
        with open(path, "wb") as f:
            f.write(data)
        mine = pa.ipc.open_stream(data)
        want = list(mine)
        for f in schema:
            types.add(str(storage_type(f.type)).split("<")[0].split("[")[0].split("(")[0])
        for aligned in (True, False):
            try:
                back = pa.ipc.open_stream(reencode(path, aligned))
                got = list(back)
                if not back.schema.equals(mine.schema, check_metadata=True):
                    fail("%s: the schema came back as %s" % (name, back.schema))
                if len(got) != len(want):
                    fail("%s: %d batches came back of %d" % (name, len(got), len(want)))
                for k, (x, y) in enumerate(zip(want, got)):
                    y.validate(full=True)
                    if y.equals(x, check_metadata=True):
                        continue
                    for j in range(x.num_columns):
                        if not same_array(x.column(j), y.column(j)):
                            fail("%s: batch %d's column %s came back otherwise: %s, not %s" %
                                 (name, k, x.schema.field(j).name, y.column(j), x.column(j)))
            except Exception as e:
                fail("%s%s: %s: %s" % (name, "" if aligned else ", unaligned", type(e).__name__, e))
        print("  %-40s %d bytes, %d batches, %d fields" % (name, len(data), len(want), len(schema)),
              flush=True)
    print("  types: %s" % " ".join(sorted(types)), flush=True)


# ---- fuzz ----

def i32(b, p):
    return struct.unpack_from("<i", b, p)[0]


def u32(b, p):
    return struct.unpack_from("<I", b, p)[0]


def field_at(b, m, table, fid):
    """Where field fid of a valid flatbuffer's table at m + table lies, from m."""
    vt = table - i32(b, m + table)
    vtlen = struct.unpack_from("<H", b, m + vt)[0]
    if 4 + 2 * fid + 2 > vtlen:
        return None
    at = struct.unpack_from("<H", b, m + vt + 4 + 2 * fid)[0]
    return table + at if at else None


def messages(s):
    """Each message of a valid stream: its metadata's start and end, its
    body's start and end, and where its RecordBatch's lengths and offsets lie."""
    out = []
    pos = 0
    while pos + 4 <= len(s):
        pre = 8 if s[pos:pos + 4] == b"\xff\xff\xff\xff" else 4
        mlen = i32(s, pos + pre - 4)
        if mlen == 0:
            break
        m = pos + pre
        root = u32(s, m)
        at = field_at(s, m, root, 3)
        blen = struct.unpack_from("<q", s, m + at)[0] if at else 0
        targets = [m + at] if at else []
        hdr = field_at(s, m, root, 2)
        if s[m + field_at(s, m, root, 1)] == 3:
            rb = hdr + u32(s, m + hdr)
            at = field_at(s, m, rb, 0)
            targets += [m + at] if at else []
            for fid, size in ((1, 16), (2, 16), (4, 8)):
                at = field_at(s, m, rb, fid)
                if at:
                    vec = at + u32(s, m + at)
                    targets += [m + vec + 4 + 8 * k for k in range(u32(s, m + vec) * size // 8)]
        out.append((m, m + mlen, m + mlen, m + mlen + blen, targets))
        pos = m + mlen + blen
    return out


EOS = b"\xff\xff\xff\xff\x00\x00\x00\x00"
INT32 = [0, 1, -1, 2, 3, 7, 8, 12, 13, 16, 64, 127, 128, 255, 256, 4096, 65535, 65536,
         2 ** 31 - 1, -2 ** 31, -8, 2 ** 24]
INT64 = INT32 + [2 ** 31, 2 ** 32, 2 ** 32 + 8, 2 ** 40, 2 ** 62, 2 ** 63 - 1, -2 ** 63, -2 ** 40]


def wrap(v, bits):
    return (v + 2 ** (bits - 1)) % 2 ** bits - 2 ** (bits - 1)


def mutate(rng, s, msgs):
    b = bytearray(s)
    kind = rng.randrange(9)
    if kind == 0 or not msgs:
        return bytes(b[:rng.randrange(len(b))]), "cut"
    m0, m1, b0, b1, targets = rng.choice(msgs)
    if kind == 1:
        for _ in range(rng.randint(1, 3)):
            b[rng.randrange(len(b))] = rng.randrange(256)
        return bytes(b), "bytes"
    if kind == 2:
        for _ in range(rng.randint(1, 2)):
            b[rng.randrange(m0, m1)] = rng.randrange(256)
        return bytes(b), "metadata bytes"
    if kind == 3:
        p = m0 + rng.randrange((m1 - m0) // 4) * 4
        struct.pack_into("<i", b, p, rng.choice(INT32 + [rng.randrange(-2 ** 31, 2 ** 31)]))
        return bytes(b), "metadata int32"
    if kind in (4, 5) and targets:
        p = rng.choice(targets)
        v = rng.choice(INT64 + [len(s), rng.randrange(-64, 4096)])
        if kind == 5:
            v = wrap(struct.unpack_from("<q", b, p)[0] + rng.choice([-9, -8, -1, 1, 8, 9]), 64)
        struct.pack_into("<q", b, p, v)
        return bytes(b), "lengths and offsets"
    if b1 - b0 >= 4 and kind in (6, 7):
        p = b0 + rng.randrange((b1 - b0) // 4) * 4
        struct.pack_into("<i", b, p, rng.choice(INT32) if kind == 6 else wrap(i32(b, p) + rng.choice([-1, 1, 12]), 32))
        return bytes(b), "body int32"
    if b1 > b0:
        for _ in range(rng.randint(1, 3)):
            b[rng.randrange(b0, b1)] = rng.choice([0x00, 0x80, 0xc0, 0xed, 0xf4, 0xff, rng.randrange(256)])
        return bytes(b), "body bytes"
    return bytes(b[:rng.randrange(len(b))]), "cut"


# What pyarrow's full validation checks of values, and the codec does not:
# a decimal's digits against its precision, a time within a day, a date64
# a whole day; and a canonical extension type's storage type and metadata,
# which its own rules constrain.  Any other complaint of pyarrow's about what
# vexec wrote is a failure.
VALUES = re.compile(r"does not fit in precision|is not within the acceptable range|"
                    r"does not represent a whole number of days|Invalid storage type for \w+|"
                    r"Serialize data must be empty|Unexpected serialized metadata")


def check_fuzz():
    print("== fuzz: %d malformed streams through vexec's reader (seed %d)" % (ARGS.fuzz, ARGS.seed), flush=True)
    bases = []
    for name, schema, batches, opts in write_cases():
        bases.append(stream_of(schema, batches[:2], opts))
    for cfg in (("arrow", "offsets"), ("arrow", "view"), ("postgres", "datum"), ("postgres", "offsets")):
        for (h,) in psql("SELECT encode(stream, 'hex') FROM vexec_test.ipc_stream("
                         "'SELECT id, b, i2, f8, d, t, ts, tstz, iv, tz, n10_2, n38_5, n, tx, bp, bt, u, mac, "
                         "a4 FROM corpus WHERE id BETWEEN 30 AND 40 ORDER BY id', %s, %s)" % tuple(map(lit, cfg))):
            bases.append(bytes.fromhex(h))
    rng = random.Random(ARGS.seed)
    parsed = [(s, messages(s)) for s in bases]
    # what pyarrow reads back is checked as storage: the test's own extension
    # type is not to reinterpret a changed storage type
    pa.unregister_extension_type("vexec.test.point")
    path = os.path.join(ARGS.work, "fuzz.tsv")
    kinds = {}
    with open(path, "w") as f:
        for i in range(ARGS.fuzz):
            s, msgs = rng.choice(parsed)
            data, kind = mutate(rng, s, msgs)
            kinds[i] = kind
            f.write("%d\t%s\t\\\\x%s\n" % (i, "t" if rng.random() < 0.5 else "f", data.hex()))
    psql("DROP TABLE IF EXISTS fuzz; CREATE TABLE fuzz (id int PRIMARY KEY, aligned bool, input bytea, "
         "outcome text, message text, output bytea)")
    psql("COPY fuzz (id, aligned, input) FROM %s" % lit(path))
    chunk = 500
    for start in range(0, ARGS.fuzz, chunk):
        try:
            psql("""DO $$
DECLARE r record; o bytea;
BEGIN
	FOR r IN SELECT id, aligned, input FROM fuzz WHERE id >= %d AND id < %d ORDER BY id LOOP
		BEGIN
			o := vexec_test.ipc_reencode(r.input, r.aligned);
			UPDATE fuzz SET outcome = 'ok', output = o WHERE id = r.id;
		EXCEPTION WHEN OTHERS THEN
			UPDATE fuzz SET outcome = SQLSTATE, message = SQLERRM WHERE id = r.id;
		END;
	END LOOP;
END $$""" % (start, start + chunk))
        except RuntimeError as e:
            fail("fuzz: the inputs %d to %d took the session down: %s" % (start, start + chunk - 1, e))
            return
    outcomes = psql("SELECT outcome, count(*) FROM fuzz GROUP BY 1 ORDER BY 2 DESC")
    print("  outcomes: %s" % ", ".join("%s %s" % (o, n) for o, n in outcomes), flush=True)
    for i, outcome, message in psql("SELECT id, outcome, message FROM fuzz "
                                    "WHERE outcome NOT IN ('ok', '22P03', '0A000') ORDER BY id LIMIT 20"):
        fail("fuzz input %s (%s): SQLSTATE %s: %s" % (i, kinds[int(i)], outcome, message))
    out = os.path.join(ARGS.work, "fuzz-out.tsv")
    psql("COPY (SELECT id, encode(output, 'hex') FROM fuzz WHERE outcome = 'ok' ORDER BY id) TO %s" % lit(out))
    semantic = {}
    nok = 0
    with open(out) as f:
        for line in f:
            i, h = line.rstrip("\n").split("\t")
            nok += 1
            if h == EOS.hex():
                continue        # a stream cut before its schema: nothing to read
            try:
                for b in pa.ipc.open_stream(bytes.fromhex(h)):
                    b.validate(full=True)
            except Exception as e:
                msg = str(e).split("\n")[0]
                if not VALUES.search(msg):
                    fail("fuzz input %s (%s): vexec read it, and pyarrow refuses what it wrote: %s"
                         % (i, kinds[int(i)], msg))
                else:
                    msg = re.sub(r"In column \d+: |-?\d{3,}", "", msg)
                    semantic[msg[:100]] = semantic.get(msg[:100], 0) + 1
    print("  %d read and written back, each valid to pyarrow but %d whose values its full validation "
          "checks beyond the codec's checks:" % (nok, sum(semantic.values())), flush=True)
    for msg, n in sorted(semantic.items(), key=lambda x: -x[1]):
        print("    %4d  %s" % (n, printable(msg)), flush=True)
    by = {}
    for (k,) in psql("SELECT id FROM fuzz WHERE outcome <> 'ok'"):
        by[kinds[int(k)]] = by.get(kinds[int(k)], 0) + 1
    print("  refused, by mutation: %s" % ", ".join("%s %d" % kv for kv in sorted(by.items())), flush=True)


def main():
    global ARGS
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--host", required=True)
    p.add_argument("--db", default="postgres")
    p.add_argument("--psql", default="psql")
    p.add_argument("--work", required=True)
    p.add_argument("--fuzz", type=int, default=6000)
    p.add_argument("--seed", type=int, default=20261006)
    p.add_argument("checks", nargs="*", default=["read", "write", "fuzz"])
    ARGS = p.parse_args()
    os.makedirs(ARGS.work, exist_ok=True)
    pa.register_extension_type(Point())
    print("pyarrow %s" % pa.__version__, flush=True)
    for c in ARGS.checks:
        {"read": check_read, "write": check_write, "fuzz": check_fuzz}[c]()
    print("ipc_check: %s" % ("%d failures" % len(FAILURES) if FAILURES else "every check passed"), flush=True)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
