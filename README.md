# pg_accel

The home of `vexec`, the vectorized planner and executor for PostgreSQL 19
that `pg_vector_executor.md` plans: one extension on the hooks PostgreSQL
already has, which also runs inside the Cloudberry port's servers
(`github/cloudberry`, branch `extension_postgresql_19`).

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

## Layout

| Path | What it is |
|---|---|
| `include/vexec_source.h` | the batch-source contract (§3.5.1): storage modules register batch readers by a rendezvous variable; installed with `vexec` |
| `modules/vexec/` | the module, built by PGXS against vanilla PostgreSQL 19 or the port's server |
| `modules/vexec/batch/` | the logical batch, its layouts, the PostgreSQL and Arrow formats, the conversions, rows in and out, scaled numerics, export through Arrow's C Data Interface |
| `modules/vexec/plan/` | the vectorized planner: the oracle, the cost model, the path hooks, the node builders, ORCA's front end through gp_orca's API, the reasons, EXPLAIN's option, the plan check |
| `modules/vexec/exec/` | the vector nodes, `VecScan`, `VecResult`, `VecAgg` and `VecHashJoin`, with their row and batch interfaces, and the aggregates' transitions |
| `modules/vexec/expr/` | the expression compiler and evaluator: kernels bound by function OID, the fallback, PostgreSQL's evaluator for what may raise |
| `modules/vexec/source/` | `vexec`'s side of the source registry |
| `modules/vexec/pgxs/include/` | copies of the port's headers the PGXS build compiles against, kept equal to the originals |
| `modules/vexec/sql`, `expected` | `vexec`'s own regression suite, and the layouts' semantics corpus |
| `modules/vexec_test/` | a module of the tests alone: round trips through every layout, and the export check with nanoarrow, vendored there only |
| `test/vexec/` | `vexec`'s legs: `run.sh` on the host, the scripts each leg runs in a container, the differential runner, the checks |
| `test/tpc/` | the port's tpc suite, with `TPC_STORAGE` (heap, `ao_column`, PAX porc and porc_vec) and `vexec` preloaded |
| `docker/vexec.yml`, `docker/Dockerfile.vexec` | `vexec`'s images and containers: a server image with a C toolchain, vanilla or the port's |
| `docker/compose.yml`, `docker/Dockerfile.clickbench` | ClickBench's images and containers (VB) |
| `test/clickbench/` | ClickBench's suite and its baseline (VB) |

## vexec

```sh
test/vexec/run.sh images           # the dev images: vanilla, and the port's (from VB's pg_accel/cb-ext)
test/vexec/run.sh checks           # the header copies, the notices, the tree check
test/vexec/run.sh suite            # vexec's own suite, on the vanilla leg and the port
test/vexec/run.sh states           # §1.2's states: installed and not preloaded, objects without the library, library removed
test/vexec/run.sh pgregress        # PostgreSQL's own suite, unchanged, with vexec installed, preloaded off, and in explain mode
test/vexec/run.sh differential     # the differential runner: off, force postgres, force arrow, force with random layouts
test/vexec/run.sh cluster          # a coordinator and four segments of the port, vexec on every node
test/vexec/run.sh tpc              # the tpc suite in each storage, vexec preloaded
test/vexec/run.sh fullrun          # the port's full run with vexec on every node, against the run without it
test/vexec/run.sh v0               # V0's checks: checks, suite, states, pgregress, differential, cluster
test/vexec/run.sh portbuild        # the port's modules from V1's worktree (VEXEC_PORT_SRC), staged for the port's legs
test/vexec/run.sh sources          # every storage's batch source on one node, in seven sessions, ORCA's among them
test/vexec/run.sh portsuites       # the port's singlenode and greenplum suites, force against off
```

From V1 the port's legs install the port's modules of V1's worktree over
the image's own, from the stage `portbuild` makes
(`~/.cache/pg_accel/vexec/portbuild/stage`); `VEXEC_PORT_STAGE=none` runs
them on the image's own. `TPC_VEXEC_MODE` and `TPC_VEXEC_FORMAT` set the tpc
leg's `vexec.mode` and format.

Every leg runs in a container; nothing is installed on the host. The legs
build `vexec` from the tree by PGXS into a copy, with warnings as errors.
Their results go to a run of the cache, `~/.cache/pg_accel/vexec/runs`.
`VEXEC_CPUS=1` keeps a leg to one CPU beside a timed run; `tpc` and
`fullrun` want the whole machine. `fullrun`'s container joins the network
`pg_accel_default`, which VB's containers make (`docker/compose.yml`);
the other legs' servers listen on sockets only, with no network.

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
(`~/.cache/pg_accel/clickbench`), outside the tree: ClickBench is
CC BY-NC-SA 4.0, and nothing of it is copied here. The queries and the
table's definition are read from a ClickBench checkout pinned by the queries'
hash (`oss_databases/ClickBench` at `dfe44c96`).

## Licences

pg_accel's files are Apache-2.0. Three others' are here with their own
notices: Arrow's C Data Interface definitions (`modules/vexec/batch/arrow_abi.h`,
copied as Arrow asks), the port's headers (`modules/vexec/pgxs/include/`, its
`test/tpc/` suite), and nanoarrow (`modules/vexec_test/nanoarrow/`, for the
tests only), all Apache-2.0.
