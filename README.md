# pg_vexec

pg_vexec is a vectorized query planner and executor for PostgreSQL 19. It is
built as extensions on the hooks PostgreSQL already has, with no fork and no
core patches. Instead of pulling rows through the plan one at a time, it
processes columnar batches stored either in PostgreSQL's own format or as
Apache Arrow. Vector operators aren't patched into a finished plan. They
compete on cost with row-based operators inside the planner, both
PostgreSQL's planner and ORCA, the Greenplum/Cloudberry optimizer, which also
runs on vanilla PostgreSQL. PostgreSQL's semantics are kept: anything a
vector kernel can't compute falls back to PostgreSQL's own expression
evaluation, and one setting brings back vanilla behaviour. Companion
extensions add an Arrow Flight SQL endpoint that returns query results and
accepts data as Arrow alongside regular PostgreSQL clients, plus kernel packs
that let pgvector and PostGIS functions run over batches. On the Apache
Cloudberry MPP port, vexec also reads and writes the columnar PAX and
ao_column tables without turning them into rows, and passes batches between
segments as Arrow IPC frames.


The plan's phases land here in order, each on a branch of its own:

- **VB, ClickBench's baseline before V0** (`vb`; the plan's §5 and §6.9):
  ClickBench run on the servers as they are, without `vexec`, and kept for
  every later phase to compare with.
- **V0, groundwork: nothing is vectorized** (`v0`; the plan's §5): the
  module and its settings, the vectorized planner's core in PostgreSQL's
  planner (the capability oracle, the vector cost model, the reasons and
  EXPLAIN's `vexec` option, the plan check), the batch layer with its two
  in-memory formats and Arrow export, the batch-source contract, and the
  test harness.
- **V1, vector scans, filters and projections** (`v1`; the plan's §5):
  `VecScan` and `VecResult` with both node interfaces, the expression
  compiler with its kernels and fallback, PostgreSQL's front end and
  ORCA's, and on a cluster the fragments' vector scans on every segment.
  Its changes to the port -- gp_orca's API, gp_core's settings, gp_ao's and
  PAX's batch readers -- are in a worktree of the port, under `pg19/` only.
- **V2, aggregation, and the first measurement** (`v2`; the plan's §5):
  `VecAgg`, plain and hashed, with its spill and PostgreSQL's states at
  stage boundaries; the aggregate kernels; PostgreSQL's grouping paths and
  ORCA's Agg of every split; aggregates answered from a source's statistics
  (the contract's `aggregate()`, PAX's in the port's worktree); and the tpc
  suite timed.
- **V3, hash joins** (`v3`; the plan's §5): `VecHashJoin` -- inner, left,
  semi, anti and right, with its spill -- in place of PostgreSQL's HashJoin
  and its Hash; PostgreSQL's join paths and ORCA's HashJoin; on a cluster,
  a side with a receiving Motion drained where PostgreSQL leaves it unread.
- **V4, heap pages, parallel paths, sorts** (`v4`; the plan's §5): heap's
  page reader; partial vector paths under PostgreSQL's Gather and Gather
  Merge, and vector nodes under M8's Gathers in a segment; `VecSort` with
  its top-N bound, from the ordered paths and ORCA's Sort; and VH's H1 for
  heap, H5's `VecRepartition`, H6's late columns and running bound, and
  H9's `VecBitmapHeapScan`.  Its changes to the port -- gp_orca's API's
  `describe_node`, M8's pass, the source contract's `set_keys` in PAX,
  gp_core's settings -- are in a worktree of the port, under `pg19/` only.
- **VK, kernel packs** (`vk`, made from `v4`; the plan's §3.17 and §5): the
  registry of the packs' declarations, `vexec/kernels_v1`, and its header,
  `vexec_kernels.h`; a call of another extension's function bound to a
  pack's declaration -- by the extension, its version, the function's name,
  signature and C symbol -- and run through fmgr a batch's rows at a time:
  the function itself where it never raises or where the pack's check
  passes, the pack's prefilter's answer where it decides; and the test pack
  `vexec_testpack`.  The packs themselves, `vexec_pgvector` and
  `vexec_postgis`, are extensions of their own, in `modules/`.  Nothing
  of the port changes.
- **V7_0, Arrow IPC messages** (`v7_0`, and `v10` after it; the plan's
  §5): the codec V7's frames and V10's egress share, which `vexec` writes
  and reads with its own code for the flatbuffer tables of Arrow's
  `Message.fbs` and `Schema.fbs` -- a message's body going out as pieces
  where its arrays lie, a message coming in checked as a client's input.
- **V10, the Flight SQL endpoint's half in `vexec`** (`v10`; the plan's §5
  and §3.15): the egress API, `vexec/egress_v1`, by which an extension of
  its own, `vexec_flight` (`modules/vexec_flight`), serves Arrow to clients
  only while the vector executor is active -- a statement's result as IPC
  messages, from a vector node's batches where one is at the top of the
  plan, and a client's parameter batches read as values.

## Layout

| Path | What it is |
|---|---|
| `include/vexec_source.h` | the batch-source contract (§3.5.1): storage modules register batch readers by a rendezvous variable; installed with `vexec` |
| `include/vexec_kernels.h` | the kernel packs' registry (§3.17): a pack declares which of an extension's functions never raise, checks the rows on which the others may, or prefilters them, by a rendezvous variable, with no kernel ABI -- only fmgr's functions; installed with `vexec` |
| `include/vexec_egress.h` | the egress API (§3.15): a statement's result as Arrow IPC messages, and a client's parameter batches as values, for an extension that serves Arrow to clients; installed with `vexec` |
| `modules/vexec/` | the module, built by PGXS against vanilla PostgreSQL 19 or the port's server |
| `modules/vexec/batch/` | the logical batch, its layouts, the PostgreSQL and Arrow formats, the conversions, rows in and out, scaled numerics, export through Arrow's C Data Interface |
| `modules/vexec/plan/` | the vectorized planner: the oracle, the cost model, the path hooks, the node builders, ORCA's front end through gp_orca's API, the reasons, EXPLAIN's option, the plan check |
| `modules/vexec/exec/` | the vector nodes, `VecScan` and `VecBitmapHeapScan`, `VecResult`, `VecAgg`, `VecHashJoin`, `VecSort` and `VecRepartition`, with their row and batch interfaces, and the aggregates' transitions |
| `modules/vexec/expr/` | the expression compiler and evaluator: kernels bound by function OID, the fallback, PostgreSQL's evaluator for what may raise; calls bound to kernel packs' declarations (`packs.c`), kept per backend and dropped by syscache callbacks |
| `modules/vexec/source/` | `vexec`'s side of the source registry, and heap's page reader |
| `modules/vexec/ipc/` | Arrow IPC messages, written and read by `vexec`'s own code (V7_0) |
| `modules/vexec/egress/` | the egress API: its routine, its receiver, the batches of a vector node at the top of a plan, a client's parameters (V10) |
| `modules/vexec/pgxs/include/` | copies of the port's headers the PGXS build compiles against, kept equal to the originals |
| `modules/vexec/sql`, `expected` | `vexec`'s own regression suite, and the layouts' semantics corpus |
| `modules/vexec_test/` | a module of the tests alone: round trips through every layout, the export check with nanoarrow, vendored there only, the IPC codec's checks, and a query through the egress's receiver |
| `modules/vexec_testpack/` | a kernel pack of the tests alone: an extension's functions and a pack's declarations of them in one library, preloaded before `vexec` by the suite (`vexec_packs`) |
| `modules/vexec_flight/` | Arrow Flight SQL while the vector executor is active (V10, VI): an extension of its own, with its own dependencies (nghttp2, protobuf-c, OpenSSL), dev image and legs (`test/run.sh`) |
| `modules/vexec_pgvector/`, `modules/vexec_postgis/` | the kernel packs for pgvector and PostGIS (VK): extensions of their own, each with its legs (`test/run.sh`) |
| `Makefile` | the four extensions built by PGXS from the top (below) |
| `test/vexec/` | `vexec`'s legs: `run.sh` on the host, the scripts each leg runs in a container, the differential runner, the checks |
| `test/tpc/` | the port's tpc suite, with `TPC_STORAGE` (heap, `ao_column`, PAX porc and porc_vec) and `vexec` preloaded |
| `docker/vexec.yml`, `docker/Dockerfile.vexec` | `vexec`'s images and containers: a server image with a C toolchain, vanilla or the port's |
| `docker/Dockerfile.arrow` | pyarrow on those images, against which the IPC codec and the egress are checked |
| `docker/compose.yml`, `docker/Dockerfile.clickbench` | ClickBench's images and containers (VB) |
| `test/clickbench/` | ClickBench's suite and its baseline (VB) |

## Building

The four extensions are built by PGXS from the top, one after another,
against an installed PostgreSQL 19 -- vanilla `REL_19_STABLE` or the port's
server:

```sh
make PG_CONFIG=/usr/local/pgsql/bin/pg_config      # vexec, vexec_pgvector, vexec_postgis, vexec_flight
make PG_CONFIG=... install
make PG_CONFIG=... EXTENSIONS="vexec vexec_pgvector vexec_postgis"   # without vexec_flight's dependencies
```

The packs and `vexec_flight` compile against `include/` here, not against
`vexec`'s installed headers, so that a commit builds as one.
`vexec_flight` needs nghttp2, protobuf-c and its compiler, protobuf's
well-known `.proto` files and OpenSSL.  Each extension is preloaded beside
`vexec`, in any order, and is built and tested on its own too, by its
directory's `Makefile` and `test/run.sh`.

## vexec

```sh
test/vexec/run.sh images           # the dev images: vanilla, and the port's (from VB's pg_vexec/cb-ext)
test/vexec/run.sh checks           # the header copies, the notices, the tree check
test/vexec/run.sh suite            # vexec's own suite, on the vanilla leg and the port
test/vexec/run.sh states           # §1.2's states: installed and not preloaded, objects without the library, library removed
test/vexec/run.sh pgregress        # PostgreSQL's own suite, unchanged, with vexec installed, preloaded off, and in explain mode
test/vexec/run.sh differential     # the differential runner: off, force postgres, force arrow, force with random layouts
test/vexec/run.sh cluster          # a coordinator and four segments of the port, vexec on every node
VEXEC_INTERCONNECT=shm test/vexec/run.sh cluster   # the same over the port's shm transport (V7)
test/vexec/run.sh shm              # the shm transport (V7): Motions, switches, remote path, hangs, kills, the port's ic test
test/vexec/run.sh tpc              # the tpc suite in each storage, vexec preloaded
test/vexec/run.sh fullrun          # the port's full run with vexec on every node, against the run without it
test/vexec/run.sh v0               # V0's checks: checks, suite, states, pgregress, differential, cluster
test/vexec/run.sh portbuild        # the port's modules from V1's worktree (VEXEC_PORT_SRC), staged for the port's legs
test/vexec/run.sh sources          # every storage's batch source on one node, in seven sessions, ORCA's among them
test/vexec/run.sh portsuites       # the port's singlenode and greenplum suites, force against off
```

From V1 the port's legs install the port's modules of V1's worktree over
the image's own, from the stage `portbuild` makes
(`~/.cache/pg_vexec/vexec/portbuild/stage`); `VEXEC_PORT_STAGE=none` runs
them on the image's own. `TPC_VEXEC_MODE` and `TPC_VEXEC_FORMAT` set the tpc
leg's `vexec.mode` and format, and `TPC_WORKERS` its parallel workers a
segment, each query run once with each of them ("0 4").

Every leg runs in a container; nothing is installed on the host. The legs
build `vexec` from the tree by PGXS into a copy, with warnings as errors.
Their results go to a run of the cache, `~/.cache/pg_vexec/vexec/runs`.
`VEXEC_CPUS=1` keeps a leg to one CPU beside a timed run; `tpc` and
`fullrun` want the whole machine. `fullrun`'s container joins the network
`pg_vexec_default`, which VB's containers make (`docker/compose.yml`);
the other legs' servers listen on sockets only, with no network.

The legs build the kernel packs with `vexec`, and `vexec_flight` on the
images that have its dependencies, so a change to `vexec`'s headers that
breaks a pack fails every leg, and one that breaks `vexec_flight` its legs.
The suite preloads `vexec_testpack` before `vexec`.  The packs' own legs are
their modules' (`test/run.sh test|corpus|cluster` in `modules/vexec_pgvector`
and `modules/vexec_postgis`), `vexec_flight`'s are its own
(`modules/vexec_flight/test/run.sh images|test|bench|ingest`), and the
differential runner takes pgvector's own tests as a corpus
(`VEXEC_CORPORA=pgvector`), its `postgres-packs` and `arrow-packs` sessions
preloading `VEXEC_PACKS`.

`vexec` is preloaded (`shared_preload_libraries = 'vexec'`), and with
`vexec.mode = off`, its default, every hook adds nothing.
`EXPLAIN (VEXEC)` prints the vector alternatives the planner considered,
and why each was not taken. From V1, `vexec.mode = force` builds a vector
scan wherever the oracle accepts one, and `auto` where its cost model
prices it below the row scan.

## ClickBench

```sh
test/clickbench/run.sh images      # the servers' images, from the port's Dockerfiles
test/clickbench/run.sh prepare     # hits.tsv.gz (16.3 GB), the 1M and 10M subsets, DuckDB's answers
test/clickbench/run.sh baseline    # check mode on 1M rows, time mode on 10M rows, saved and checked
test/clickbench/run.sh run time vanilla-heap port-aoco-s4   # any loads, into a run of the cache
test/clickbench/run.sh compare test/clickbench/baseline/<host>/<date> <run>
CB_VEXEC="force-postgres force-arrow" test/clickbench/run.sh run check vanilla-heap port-porc-s4
```

`CB_VEXEC` adds `vexec`'s sessions to each load's planners, in check mode
(VH with V1): `vexec` built in its own images and installed in each
container as it starts, with the port's modules of V1's worktree on the
port's route.

The data, DuckDB's answers and every answer PostgreSQL gave stay in the cache
(`~/.cache/pg_vexec/clickbench`), outside the tree: ClickBench is
CC BY-NC-SA 4.0, and nothing of it is copied here. The queries and the
table's definition are read from a ClickBench checkout pinned by the queries'
hash (`oss_databases/ClickBench` at `dfe44c96`).

## Licences

pg_vexec's files are Apache-2.0. Three others' are here with their own
notices: Arrow's C Data Interface definitions (`modules/vexec/batch/arrow_abi.h`,
`modules/vexec_flight/src/arrow_abi.h`, copied as Arrow asks) and Flight's and
Flight SQL's protocols (`modules/vexec_flight/proto/`), the port's headers
(`modules/vexec/pgxs/include/`, its `test/tpc/` suite), and nanoarrow
(`modules/vexec_test/nanoarrow/`, for the tests only), all Apache-2.0.
`modules/vexec_flight/src/hba.c` is Apache-2.0 AND PostgreSQL: it carries
copies of PostgreSQL's `hba.c` functions under PostgreSQL's notice.
