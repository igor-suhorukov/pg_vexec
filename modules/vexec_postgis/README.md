# vexec_postgis

[vexec](../pg_accel)'s kernel pack for [PostGIS](https://postgis.net)
(`pg_vector_executor.md` §3.17, phase VK).  An extension of its own: it
declares, in vexec's registry of kernel packs, what vexec cannot know of
PostGIS's geometry functions, so that a qual or a target over them runs a
batch's rows at a time in vexec's vector nodes, where it ran row by row.
Every result it does not take from a prefilter is PostGIS's own function's,
called by vexec through fmgr.

| Declaration | Functions | What vexec does |
|---|---|---|
| never raises | `&&` and the other eleven 2-D box operators (`geometry_overlaps`, `_same`, `_contains`, `_within`, `_left`, `_overleft`, `_below`, `_overbelow`, `_overright`, `_right`, `_overabove`, `_above`); `ST_SRID(geometry)` | calls PostGIS's function on every row |
| check: the value is a point | `ST_X`, `ST_Y` | calls PostGIS's function where the check passes; any other row goes to PostgreSQL's evaluator, where PostGIS raises its error |
| prefilter: the boxes' short-circuit | `ST_Intersects`, `ST_Touches`, `ST_Overlaps`, `ST_Crosses`, `ST_Disjoint`, `ST_Contains`, `ST_ContainsProperly`, `ST_Covers`, `ST_Within`, `ST_CoveredBy`, `ST_Equals` | takes the prefilter's answer where the two boxes decide, as PostGIS's own short-circuit would before GEOS; any other row -- other SRIDs, an empty geometry, boxes that do not decide -- goes to PostGIS's function, through PostgreSQL's evaluator |

- **For which PostGIS.**  3.7.0rc2, whose code was read.  Another version
  binds nothing, and the calls run as without the pack: the same answers,
  row by row.  So does a server without the pack.
- **A reader of its own** (`src/reader.c`), written from the format's
  description, PostGIS's `liblwgeom/gserialized.txt`: the header, the
  version 2 flags, the extended flags, the cached box, and the bodies of
  points, lines, polygons, circular strings and collections.  It decides
  nothing for a version, a flag or a type the description does not give --
  NURBS curves, triangles, TINs, polyhedral surfaces, curve polygons -- nor
  for a value it would read past its end, nor gives a box with a NaN in it.
  It includes no PostGIS header and copies no PostGIS code; PostGIS's code
  was read only to check behaviour.
- **No kernel ABI.**  The pack's checks and prefilters are C functions of
  fmgr's version-1 convention; a prefilter reports an undecided row by a soft
  error.  The pack includes only PostgreSQL's headers and vexec's
  `vexec_kernels.h`.
- **Preloaded**, on every server, in any order with vexec:
  `shared_preload_libraries = 'vexec_postgis,vexec'`.  `CREATE EXTENSION
  vexec_postgis` adds only what a person and the tests ask of it:
  `declarations()`, `prefilter()`, `is_point()` and `reader()`.

PostGIS 3.7.0rc2's `ST_Equals` compares its two boxes through
`gbox_same_2d_float()`, whose tests of the minimums compare one box's value
with itself, so only the maximums decide (`liblwgeom/gbox.c:195-203`).  The
prefilter decides exactly where PostGIS does, and no more.

## Building and testing

```sh
make PG_CONFIG=/usr/local/pgsql/bin/pg_config          # vexec installed there
make PG_CONFIG=... install
test/run.sh test       # the pack's suites on one node, with the pack and without it
test/run.sh corpus     # PostGIS's own core regression tests, off, force, and force with the pack
test/run.sh cluster    # a coordinator and its segments of the Cloudberry port
```

The legs run in containers of pg_accel's port image, which carries PostGIS
3.7.0rc2 and its build tree as the port pins them.  The suites:

- `reader` -- the reader against PostGIS over a corpus of random geometries
  and edges: for every value it knows, its type, its emptiness and its box
  are PostGIS's, and SRIDs' bytes are equal where SRIDs are.
- `declarations` -- PostGIS's own functions against the declarations,
  without vexec, over every pair of the corpus: each prefilter's answers are
  the predicate's own, on no pair it raises on; the box operators and
  `ST_SRID` raise nothing; `ST_X` and `ST_Y` raise nowhere the check passes,
  and everywhere else.
- `answers` -- queries over every declared function give PostgreSQL's
  answers -- rows and errors -- in vexec's force mode in both batch formats;
  the same expected output with the pack and without it.
- `plans`, `versions` -- the declared calls named in EXPLAIN, and
  `ALTER EXTENSION postgis UPDATE` to a version the pack does not name, in
  another session, unbinding them in this one.

`test/corpus-kept` holds the differences of PostGIS's own tests the corpus
keeps, reviewed: plans' node names, `Seq Scan` against `Vec Seq Scan`.

## Licence

Apache-2.0: the pack includes no PostGIS header and copies no PostGIS code.
PostGIS is under the GPL, version 2 or later.  The licence is the user's to
confirm (`pg_vector_executor.md` §8).
