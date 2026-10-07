# vexec_flight

Arrow Flight SQL for PostgreSQL 19, served only while
[vexec](../../README.md)'s vector executor is active
(`pg_vector_executor.md` §3.15, phase V10).  An extension of its own, with its
own dependencies; it needs nothing of the Cloudberry port's tree, and runs on
vanilla PostgreSQL 19 and on the port's coordinator.

- **Only with the vector executor.**  The acceptor starts only when
  `vexec_flight.listen_addresses` names an address, and exits with a WARNING
  when vexec is not loaded.  A statement runs only while `vexec.mode` is
  `auto` or `force` in the session, and fails with `FAILED_PRECONDITION`
  otherwise.  Every result goes to the client through vexec's egress API
  (`vexec_egress.h`), as Arrow IPC messages: the extension has no row path of
  its own.
- **PostgreSQL's process model.**  An acceptor without threads listens on the
  Flight port and asks the postmaster for a session worker for each
  connection, which takes the socket and owns it, as a backend does: one
  dynamic background worker, one thread, HTTP/2 and gRPC on nghttp2 in the
  session itself, its TLS an OpenSSL context of its own.  A message's body
  goes from the batch's buffers to the socket as it lies.
- **Logins** are PostgreSQL's: `pg_hba.conf` with the client's address and
  TLS, the role's password, `rolcanlogin`, `rolvaliduntil`, the role's and
  the database's connection limits.  Flight's basic authentication carries
  the credentials, and a `database` header the database.
- **Statements** go through portals, as a backend's do: the planner's hooks,
  permissions, row-level security, transactions, prepared statements and
  `pg_stat_statements` behave as over PostgreSQL's protocol.  Sessions are in
  `pg_stat_activity`, and `pg_cancel_backend()`, `pg_terminate_backend()` and
  Flight's `CancelFlightInfo` end their statements.

## Flight SQL

| Flight SQL | Here |
|---|---|
| `CommandStatementQuery`, `CommandPreparedStatementQuery` | parsed and analysed at `GetFlightInfo`, planned and run at `DoGet`; a statement that returns no rows runs at `GetFlightInfo`, whose FlightInfo then has no endpoint and its row count as `total_records` |
| `CommandStatementUpdate`, `CommandPreparedStatementUpdate` | their row counts, a prepared one's once for each parameter set, in one transaction; a prepared `INSERT ... VALUES` of parameters alone, into a table without triggers or rules, once for all its sets, as an ingest's (below) |
| `DoPut` parameters | a set a row, read by vexec's IPC reader, as values of the parameters' types |
| `CommandStatementIngest` | the client's batches into a table, as `INSERT INTO t SELECT ... FROM vexec.ingest_stream(handle)`: the stream read off the socket as the statement runs, through vexec's egress API (minor 1); each batch a batch of vexec's `VecIngest`, its buffers taken as they lie where a column's Arrow type is its column's, written by `VecInsert` through the table's sink or `table_multi_insert()` (§3.16, VI).  The table made, appended to or replaced as the command's options say, in its transaction or the client's.  A client that ends its call before its stream's end loads nothing |
| `CreatePreparedStatement`, `ClosePreparedStatement` | a saved plan source, as `Parse` makes one |
| `BeginTransaction`, `EndTransaction`, `BeginSavepoint`, `EndSavepoint` | `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `RELEASE`, `ROLLBACK TO` |
| `CommandGetCatalogs`, `...DbSchemas`, `...Tables`, `...TableTypes`, `...PrimaryKeys`, `...ExportedKeys`, `...ImportedKeys`, `...CrossReference` | SQL over `pg_catalog`, with the caller's privileges |
| `CommandGetSqlInfo`, `CommandGetXdbcTypeInfo` | arrays of the endpoint's own |
| `SetSessionOptions`, `GetSessionOptions`, `CloseSession` | `SET`, the settings, the session's end |
| `CancelFlightInfo`, `CancelQuery` | the statement's session signalled, as `pg_cancel_backend()` does |
| `CommandStatementSubstraitPlan`, `DoExchange` | not served |

## Settings

| Setting | Default | Context | What |
|---|---|---|---|
| `vexec_flight.listen_addresses` | empty | postmaster | where the acceptor listens; empty: no acceptor |
| `vexec_flight.port` | 32010 | postmaster | the Flight port |
| `vexec_flight.ssl_cert_file`, `vexec_flight.ssl_key_file` | the server's `ssl_cert_file`, `ssl_key_file` | sighup | TLS; without them, plaintext, which `hostssl` lines reject |
| `vexec_flight.max_sessions` | 16 | postmaster | sessions at once, each a background worker counted in `max_worker_processes` |
| `vexec_flight.idle_session_timeout` | 10 min | sighup | a session idle this long ends |

The port serves TLS and plaintext both, told apart by the client's first
byte, as `hostssl` and `hostnossl` lines need.

## Building and testing

```sh
make PG_CONFIG=/usr/local/pgsql/bin/pg_config          # vexec installed there
make PG_CONFIG=... install
test/run.sh images [vanilla|port]                      # vexec_flight/dev:<leg>
test/run.sh test [vanilla|port]                        # the clients' tests
test/run.sh bench [vanilla|port]                       # TPC-H Q1, Flight SQL against libpq's binary COPY
test/run.sh ingest [vanilla|port]                      # lineitem into tables: Flight's ingest and prepared
                                                       # INSERT, against COPY of CSV and adbc's binary COPY
test/run.sh serve [vanilla|port]                       # the tests' server, left running for docker exec
```

It needs nghttp2, protobuf-c and its compiler, protobuf's well-known
`.proto` files, and OpenSSL; the tests need pyarrow, the ADBC drivers for
Flight SQL and PostgreSQL, and the Flight SQL JDBC driver
(`docker/Dockerfile`, which pins each).  The legs build vexec from the tree
this module is in, `../..`, or from `VEXEC_SRC`.

## Licences

vexec_flight's files are Apache-2.0.  Arrow's `Flight.proto` and
`FlightSql.proto` (`proto/`, with their notice) and the Arrow C Data
Interface's definitions (`src/arrow_abi.h`) are Apache Arrow's, Apache-2.0.
