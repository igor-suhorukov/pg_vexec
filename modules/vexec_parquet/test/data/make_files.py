# SPDX-License-Identifier: Apache-2.0
#
# Parquet files of another writer, pyarrow, for the external_arrow test
# (test/vexec/parquet.sh writes them before it runs): every type such a
# writer gives -- unsigned integers, half floats, large strings, dictionaries,
# every unit of time -- in several row groups, the same rows in three files of
# other codecs and orders of columns, and a nested column no table declares
# as text (an array, the nested test's).
#
#   python make_files.py <directory>
import datetime, decimal, os, sys, uuid
import pyarrow as pa, pyarrow.parquet as pq

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
n = 1000
ids = list(range(n))

def nul(vals, every=7):
    return [None if i % every == 3 else v for i, v in enumerate(vals)]

base = datetime.datetime(2020, 1, 1, 12, 0, 0)
cols = {
    "id": pa.array(ids, pa.int64()),
    "i8": pa.array(nul([i % 100 - 50 for i in ids]), pa.int8()),
    "i16": pa.array(nul([i * 7 - 3000 for i in ids]), pa.int16()),
    "i32": pa.array(nul([i * 1000003 % 2147483647 for i in ids]), pa.int32()),
    "u8": pa.array(nul([i % 256 for i in ids]), pa.uint8()),
    "u16": pa.array(nul([i * 60 % 65536 for i in ids]), pa.uint16()),
    "u32": pa.array(nul([i * 4000000 % 4294967296 for i in ids]), pa.uint32()),
    "u64": pa.array(nul([i * 10 for i in ids]), pa.uint64()),
    "f16": pa.array(nul([float(i) / 4 for i in ids]), pa.float16()) if hasattr(pa, "float16") else None,
    "f32": pa.array(nul([i / 3.0 for i in ids]), pa.float32()),
    "f64": pa.array(nul([i / 7.0 if i != 5 else float("nan") for i in ids]), pa.float64()),
    "b": pa.array(nul([i % 3 == 0 for i in ids]), pa.bool_()),
    "s": pa.array(nul([f"str {i} ü" for i in ids]), pa.string()),
    "ls": pa.array(nul([f"large {i}" for i in ids]), pa.large_string()),
    "bin": pa.array(nul([bytes([i % 256, 0, 255]) for i in ids]), pa.binary()),
    "dict": pa.array(nul([f"cat{i % 5}" for i in ids])).dictionary_encode(),
    "d32": pa.array(nul([datetime.date(2000, 1, 1) + datetime.timedelta(days=i) for i in ids]), pa.date32()),
    "d64": pa.array(nul([datetime.date(1960, 1, 1) + datetime.timedelta(days=i) for i in ids]), pa.date64()),
    "ts_s": pa.array(nul([base + datetime.timedelta(seconds=i) for i in ids]), pa.timestamp("s")),
    "ts_ms": pa.array(nul([base + datetime.timedelta(milliseconds=i * 1001) for i in ids]), pa.timestamp("ms")),
    "ts_us": pa.array(nul([base + datetime.timedelta(microseconds=i * 1000001) for i in ids]), pa.timestamp("us")),
    "ts_ns": pa.array(nul([pa.scalar(base + datetime.timedelta(microseconds=i), pa.timestamp("us")).cast(pa.timestamp("ns")).value + 7 for i in ids]), pa.timestamp("ns")),
    "tz_us": pa.array(nul([base + datetime.timedelta(hours=i) for i in ids]), pa.timestamp("us", tz="UTC")),
    "t32": pa.array(nul([(i * 1000) % 86400000 for i in ids]), pa.time32("ms")),
    "t64": pa.array(nul([(i * 1000000007) % 86400000000000 for i in ids]), pa.time64("ns")),
    "dec9": pa.array(nul([decimal.Decimal(i * 3) / 100 for i in ids]), pa.decimal128(9, 2)),
    "dec18": pa.array(nul([decimal.Decimal(i * 1234567) / 10000 for i in ids]), pa.decimal128(18, 4)),
    "dec38": pa.array(nul([decimal.Decimal(i) * decimal.Decimal("123456789012345678.0123456789") for i in ids]), pa.decimal128(38, 10)),
    "uu": pa.array(nul([uuid.UUID(int=i * 0x10000000000000001).bytes for i in ids]), pa.binary(16)),
    "nul": pa.nulls(n),
}
cols = {k: v for k, v in cols.items() if v is not None}
t = pa.table(cols)
pq.write_table(t, os.path.join(out, "all.parquet"), row_group_size=250, compression="zstd")
# the same rows in three files of a directory, other orders of columns, snappy and none
for k, (lo, hi) in enumerate([(0, 300), (300, 650), (650, 1000)]):
    part = t.slice(lo, hi - lo)
    names = part.column_names[::-1] if k == 1 else part.column_names
    pq.write_table(part.select(names), os.path.join(out, f"part-{k}.parquet"), row_group_size=100,
                   compression=["snappy", "none", "gzip"][k])
# a nested column, which a table declares as an array, never as text (PQ20)
pq.write_table(pa.table({"id": pa.array([1, 2], pa.int64()), "l": pa.array([[1, 2], [3]])}), os.path.join(out, "nested.parquet"))
# the same rows with the offset index and pages of 2 kB, for fetches by TID
# that read a column's page alone (external_arrow)
pq.write_table(t, os.path.join(out, "paged.parquet"), row_group_size=400, compression="snappy",
               write_page_index=True, data_page_size=2048)
# text for the databases not UTF-8 (the encodings test): empty strings
# among ASCII's, Latin-1's letters and NULLs
texts = ["", "abc", "é", "", "üü x", None, "", "z"] * 125
pq.write_table(pa.table({"id": pa.array(range(len(texts)), pa.int64()), "t": pa.array(texts, pa.string())}),
               os.path.join(out, "texts.parquet"), row_group_size=300)
# pg_vector_executor.md §3.37's PQ9 (the cpu test): text a batch is checked
# whole at once -- a character cut between two values, each alone not UTF-8
# though the two together are; a NUL inside a value -- every other value whole
cut = pa.array([b"ab", b"x\xc3", b"\xbcy", b"z"], pa.binary()).view(pa.string())
pq.write_table(pa.table({"id": pa.array([1, 2, 3, 4], pa.int64()), "t": cut}), os.path.join(out, "cut_utf8.parquet"))
nul_text = pa.array([b"ok", b"a\x00b", b"fine"], pa.binary()).view(pa.string())
pq.write_table(pa.table({"id": pa.array([1, 2, 3], pa.int64()), "t": nul_text}), os.path.join(out, "nul_utf8.parquet"))

# pg_vector_executor.md §3.39's PQ15-PQ18 (the geo test): GeoParquet's
# files of WKB -- GeoParquet 1.x's metadata alone ("V1"), Parquet's GEOMETRY
# with it ("BOTH") or without it, GeoParquet 2.0's, GEOGRAPHY -- their CRSs,
# values PostGIS refuses, and binaries of other types' binary forms
import json, struct

class WKB(pa.ExtensionType):
    """geoarrow.wkb, which Arrow's writer makes Parquet's GEOMETRY or GEOGRAPHY"""
    def __init__(self, meta=b"{}"):
        self._meta = meta
        super().__init__(pa.binary(), "geoarrow.wkb")
    def __arrow_ext_serialize__(self):
        return self._meta
    @classmethod
    def __arrow_ext_deserialize__(cls, storage_type, serialized):
        return WKB(serialized)

def wkb(kind, pts, srid=None, be=False):
    o = ">" if be else "<"
    head = struct.pack(o + "BI", 0 if be else 1, kind | (0x20000000 if srid else 0))
    if srid:
        head += struct.pack(o + "I", srid)
    if kind == 1:
        return head + struct.pack(o + "dd", *pts[0])
    return head + struct.pack(o + "I", len(pts)) + b"".join(struct.pack(o + "dd", x, y) for x, y in pts)

def line(i, x0, y0):
    return wkb(2, [(x0 + i / 100, y0 + i / 100), (x0 + i / 100 + 0.005, y0 + i / 100 + 0.005)])

geo_dir = os.path.join(out, "geo")
os.makedirs(geo_dir, exist_ok=True)
# 100 lines, two row groups far apart: rows 0-49 in [10, 11) x [50, 51), rows 50-99 in [20, 21) x [60, 61); every 10th NULL
lines = [None if i % 10 == 7 else line(i % 50, 10.0 if i < 50 else 20.0, 50.0 if i < 50 else 60.0) for i in range(100)]
geo_ids = pa.array(range(100), pa.int64())
bbox = [10.0, 50.0, 20.0 + 49 / 100 + 0.005, 60.0 + 49 / 100 + 0.005]

def geo_meta(version="1.0.0", **col):
    c = {"encoding": "WKB", "geometry_types": ["LineString"], "bbox": bbox}
    c.update(col)
    return {"version": version, "primary_column": "g", "columns": {"g": c}}

def write_geo(name, values, meta=None, ext=None, row_group_size=50, ids=None):
    g = pa.array(values, pa.binary())
    if ext is not None:
        g = pa.ExtensionArray.from_storage(WKB(ext), g)
    tbl = pa.table({"id": ids if ids is not None else pa.array(range(len(values)), pa.int64()), "g": g})
    # the footer's key "geo" alone, as GeoParquet's writers leave it -- no Arrow schema beside it
    w = pq.ParquetWriter(os.path.join(geo_dir, name), tbl.schema, store_schema=False)
    w.write_table(tbl, row_group_size=row_group_size)
    if meta is not None:
        w.add_key_value_metadata({"geo": json.dumps(meta)})
    w.close()

write_geo("v1.parquet", lines, meta=geo_meta(), ids=geo_ids)
write_geo("both.parquet", lines, meta=geo_meta(), ext=b"{}", ids=geo_ids)
write_geo("v2.parquet", lines, meta=geo_meta("2.0.0"), ext=b"{}", ids=geo_ids)
write_geo("none.parquet", lines, ext=b"{}", ids=geo_ids)
# CRSs: Parquet's logical type's, GeoParquet's PROJJSON and null, spherical edges
write_geo("type_3857.parquet", lines[:3], ext=json.dumps({"crs": "EPSG:3857"}).encode())
write_geo("type_srid.parquet", lines[:3], ext=json.dumps({"crs": "srid:3857"}).encode())
projjson_3857 = {"type": "ProjectedCRS", "name": "WGS 84 / Pseudo-Mercator", "id": {"authority": "EPSG", "code": 3857}}
write_geo("v1_projjson.parquet", lines[:3], meta=geo_meta(crs=projjson_3857))
write_geo("v1_crs_null.parquet", lines[:3], meta=geo_meta(crs=None))
write_geo("v1_spherical.parquet", lines[:3], meta=geo_meta(edges="spherical"))
write_geo("geography.parquet", lines[:3], ext=json.dumps({"edges": "spherical"}).encode())
# values: an empty line, a point, big-endian WKB, EWKB of SRIDs 4326 and 3857 -- and, each in a file of its own,
# what PostGIS's receive function refuses
cases = [line(1, 1.0, 2.0), None, wkb(2, []), wkb(1, [(3.0, 4.0)]), wkb(2, [(5.0, 6.0), (7.0, 8.0)], be=True),
         wkb(2, [(1.0, 1.0), (2.0, 2.0)], srid=4326), wkb(2, [(1.0, 1.0), (2.0, 2.0)], srid=3857)]
write_geo("cases.parquet", cases, meta=geo_meta(geometry_types=["Point", "LineString"], bbox=[1.0, 1.0, 7.0, 8.0]))
write_geo("one_point.parquet", [wkb(2, [(1.0, 1.0)])], meta=geo_meta())
write_geo("ewkb_3857.parquet", [wkb(2, [(1.0, 1.0), (2.0, 2.0)], srid=3857)], meta=geo_meta())
write_geo("truncated.parquet", [line(1, 1.0, 2.0)[:20]], meta=geo_meta())
# other types' binary forms (COPY BINARY's): a point's two float8s and an int8, big-endian; an int8 of 10 bytes
pq.write_table(pa.table({"id": pa.array([1, 2], pa.int64()),
                         "p": pa.array([struct.pack(">dd", 1.5, -2.25), None], pa.binary()),
                         "n": pa.array([struct.pack(">q", -42), struct.pack(">q", 1 << 40)], pa.binary())}),
               os.path.join(geo_dir, "recv.parquet"))
pq.write_table(pa.table({"n": pa.array([b"0123456789"], pa.binary())}), os.path.join(geo_dir, "recv_long.parquet"))

# pg_vector_executor.md §3.40's PQ19 and PQ20 (the nested test): lists of
# every flat element type, NULL lists and elements, empty ones, a large
# list; maps of strings to strings and of other types, NULL and empty maps,
# NULL values, a key twice; and shapes no column reads -- a list of lists, of
# structs -- in two row groups
nested_dir = os.path.join(out, "nested")
os.makedirs(nested_dir, exist_ok=True)
base_d = datetime.date(2024, 1, 1)
base_t = datetime.datetime(2024, 1, 1, 12, 0, 0)
lists = pa.table({
    "id": pa.array(range(6), pa.int64()),
    "l64": pa.array([[1, 2, 3], None, [], [None, 5], [1 << 40], [7] * 4], pa.list_(pa.int64())),
    "l32": pa.array([[10, 20], [None], None, [], [2147483647], [-1]], pa.list_(pa.int32())),
    "l16": pa.array([[1], [2, 3], [], None, [32767], [-32768]], pa.list_(pa.int16())),
    "l8": pa.array([[78, 87, 82], [87], [], None, [None], [82, 82]], pa.list_(pa.int8())),
    "ls": pa.array([["a", "ü", ""], None, [], [None, "x"], ["long text " * 3], ["outer", "inner"]], pa.list_(pa.string())),
    "lf": pa.array([[1.5, -2.25], [float("nan")], [], None, [float("inf")], [0.0]], pa.list_(pa.float32())),
    "ld": pa.array([[1.0 / 3], None, [], [None], [1e300], [-0.5]], pa.list_(pa.float64())),
    "lb": pa.array([[True, False], [None], [], None, [True], [False]], pa.list_(pa.bool_())),
    "ldate": pa.array([[base_d], [base_d + datetime.timedelta(days=1), None], [], None, [base_d], [base_d]], pa.list_(pa.date32())),
    "lts": pa.array([[base_t], [], None, [base_t + datetime.timedelta(seconds=1)], [base_t], [base_t]], pa.list_(pa.timestamp("us"))),
    "ldec": pa.array([[decimal.Decimal("1.25")], [], None, [decimal.Decimal("-3.50"), None], [decimal.Decimal("0.01")], []], pa.list_(pa.decimal128(9, 2))),
    "lbin": pa.array([[b"\x00\x01"], [b""], None, [], [None], [b"z"]], pa.list_(pa.binary())),
    "large": pa.array([[1, 2], [], None, [3], [4], [5, 6, 7]], pa.large_list(pa.int64())),
})
pq.write_table(lists, os.path.join(nested_dir, "lists.parquet"), row_group_size=3)
maps = pa.table({
    "id": pa.array(range(6), pa.int64()),
    "tags": pa.array([[("highway", "residential"), ("name", "ซอย 5")], None, [], [("building", None)],
                      [("k", "first"), ("k", "second")], [("a", "1"), ("b", "2"), ("c", "3")]],
                     pa.map_(pa.string(), pa.string())),
    "counts": pa.array([[("a", 1), ("b", None)], [], None, [("big", 1 << 50)], [("z", -1)], [("k", 2), ("k", 3)]],
                       pa.map_(pa.string(), pa.int64())),
    "scores": pa.array([[(1, 0.5), (2, float("nan"))], [(3, float("inf"))], None, [], [(4, -1.25)], [(5, 1e-3)]],
                       pa.map_(pa.int64(), pa.float64())),
    "flags": pa.array([[("on", True), ("off", False)], None, [], [("x", None)], [("y", True)], [("z", False)]],
                      pa.map_(pa.string(), pa.bool_())),
    "dates": pa.array([[("d", base_d)], None, [], [("e", None)], [("f", base_d)], [("g", base_d)]],
                      pa.map_(pa.string(), pa.date32())),
})
pq.write_table(maps, os.path.join(nested_dir, "maps.parquet"), row_group_size=3)
pq.write_table(pa.table({
    "id": pa.array([1, 2], pa.int64()),
    "ll": pa.array([[[1], [2, 3]], None], pa.list_(pa.list_(pa.int64()))),
    "lst": pa.array([[{"a": 1}], []], pa.list_(pa.struct([("a", pa.int64())]))),
    "ml": pa.array([[("k", [1, 2])], None], pa.map_(pa.string(), pa.list_(pa.int64()))),
}), os.path.join(nested_dir, "shapes.parquet"))
print("written", t.num_rows, "rows,", t.num_columns, "columns")
