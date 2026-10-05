# pg_accel

The home of `vexec`, the vectorized planner and executor for PostgreSQL 19
that `pg_vector_executor.md` plans: one extension on the hooks PostgreSQL
already has, which also runs inside the Cloudberry port's servers
(`github/cloudberry`, branch `extension_postgresql_19`). Nothing of `vexec`
is built yet.

The plan's phases land here in order. The first is **VB, ClickBench's
baseline before V0** (the plan's §5 and §6.9): ClickBench run on the servers
as they are, without `vexec`, and kept for every later phase to compare with.

## Layout

| Path | What it is |
|---|---|
| `docker/compose.yml` | the images and containers: the port's builds, tagged by commit, and the vanilla leg with DuckDB |
| `docker/Dockerfile.clickbench` | vanilla PostgreSQL 19 (the port's two builds of `REL_19_STABLE`) with DuckDB |
| `test/clickbench/run.sh` | the suite on the host: images, data, runs, the baseline, comparisons |
| `test/clickbench/bench.sh` | one load and its queries, inside a container |
| `test/clickbench/clickbench.py` | subsets, DuckDB's reference answers, the answer rules, reports, `compare` |
| `test/clickbench/baseline/<host>/<date>/` | a baseline: metrics, plans and answers' hashes |

## ClickBench

```sh
test/clickbench/run.sh images      # the servers' images, from the port's Dockerfiles
test/clickbench/run.sh prepare     # hits.tsv.gz (16.3 GB), the 1M and 10M subsets, DuckDB's answers
test/clickbench/run.sh baseline    # check mode on 1M rows, time mode on 10M rows, saved and checked
test/clickbench/run.sh run time vanilla-heap port-aoco-s4   # any loads, into a run of the cache
test/clickbench/run.sh compare test/clickbench/baseline/<host>/<date> <run>
```

The data, DuckDB's answers and every answer PostgreSQL gave stay in the cache
(`~/.cache/pg_accel/clickbench`), outside the tree: ClickBench is
CC BY-NC-SA 4.0, and nothing of it is copied here. The queries and the
table's definition are read from a ClickBench checkout pinned by the queries'
hash (`oss_databases/ClickBench` at `dfe44c96`).
