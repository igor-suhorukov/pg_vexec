# vexec_pgvector

[vexec](../pg_accel)'s kernel pack for [pgvector](https://github.com/pgvector/pgvector)
(`pg_vector_executor.md` §3.17, phase VK).  An extension of its own: it
declares, in vexec's registry of kernel packs, what vexec cannot know of
pgvector's distances, so that a qual or a target over them runs a batch's
rows at a time in vexec's vector nodes, where it ran row by row.  It brings
no kernel of its own: vexec calls pgvector's own functions through fmgr,
with one prepared call frame, so every result is pgvector's.

- **What it declares.**  Every distance of `vector` and `halfvec` --
  `l2_distance`, `inner_product`, `cosine_distance`, `l1_distance`,
  `vector_l2_squared_distance`, `vector_negative_inner_product`,
  `vector_spherical_distance`, and `halfvec`'s seven -- raises in one place
  only: when its arguments' dimensions differ.  Each is declared with a
  check, that the dimensions agree.  vexec calls the function on the rows the
  check passes; any other row goes to PostgreSQL's evaluator in row order,
  where pgvector raises its own error, where PostgreSQL would.  `<->`,
  `<=>`, `<#>` and `<+>` are these functions.
- **For which pgvector.**  0.8.6 and 0.8.7, whose code was read.  Another
  version binds nothing, and the calls run as without the pack: the same
  answers, row by row.  So does a server without the pack.
- **No kernel ABI.**  The pack's check is a C function of fmgr's version-1
  convention; it includes only PostgreSQL's headers and vexec's
  `vexec_kernels.h`, and needs nothing of pgvector's to build: it reads a
  value's dimensions from the layout pgvector's headers document.
- **Preloaded**, on every server -- a coordinator, each segment, one node --
  in any order with vexec: `shared_preload_libraries = 'vexec_pgvector,vexec'`.
  `CREATE EXTENSION vexec_pgvector` adds only what a person and the tests
  ask of it: `vexec_pgvector.declarations()` and the check,
  `vexec_pgvector.dims_check()`.  vexec's `vexec.declared_calls()` lists
  what binds in a database, and `EXPLAIN VERBOSE` names each declared call:
  `Declared Calls: l2_distance(vector,vector) [vexec_pgvector: check]`.

## Building and testing

```sh
make PG_CONFIG=/usr/local/pgsql/bin/pg_config          # vexec installed there
make PG_CONFIG=... install
test/run.sh test       # the pack's suites on one node, with the pack and without it
test/run.sh corpus     # pgvector's own regression tests through vexec's differential runner
test/run.sh cluster    # a coordinator and its segments of the Cloudberry port
```

The legs run in containers of pg_accel's port image, which carries pgvector
0.8.6 as the port pins it; `VEXEC_SRC` names pg_accel's tree
(`../pg_accel-vk`) and `VEXEC_PORT_STAGE` the port's modules.  The suites:

- `declarations` -- pgvector's own functions against the check, called
  directly, without vexec, over random vectors of 1 to 16,000 dimensions and
  edges: zero vectors, negative zeros, the largest and smallest floats,
  values stored out of line.  None raises where the check passes; each
  raises pgvector's error where it fails.
- `answers` -- queries over every distance, in quals, targets, sort keys and
  aggregates, give PostgreSQL's answers -- rows and errors -- in vexec's
  force mode in both batch formats; the same expected output with the pack
  and without it.
- `plans` -- the qual eager, the declared calls named, a nearest-neighbour
  search's VecSort fed the scan's batches.
- `versions` -- `ALTER EXTENSION vector UPDATE` to a version the pack does
  not name, in another session, unbinds the declarations in this one.

## Licence

Apache-2.0.  pgvector is under the PostgreSQL licence; nothing of its code
is copied here.
