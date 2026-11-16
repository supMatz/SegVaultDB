# Changelog

## Unreleased

**Storage engine**
- Indexes on disk: at clean shutdown and on `CHECKPOINT`, the in-memory index of each indexed column becomes a
  read-only B+tree in the data file (catalog format v7). After a restart a primary-key or unique lookup reads
  2-3 pages instead of scanning the whole table to rebuild the index (20 000-row table: 3 pages instead of
  ~1 600). The first write to a table drops its trees (logged before the row, so a crash never uses a stale
  tree); `ALTER` / `TRUNCATE` / `VACUUM FULL` make them unusable; damaged index pages fall back to the table.
  `EXPLAIN` shows "B+tree on disk".

**Performance**
- Table scans copy only the text columns the query still uses after its read-time filters, and a
  single-table `LIMIT` without `ORDER BY` / grouping stops reading at `OFFSET + LIMIT` rows
  (`SELECT * … LIMIT 10` on a 57 MB table: 56 → 8 ms; a filtered `COUNT(*)`: 47 → 20 ms).

**Security**
- The local certificate authority carries critical X.509 name constraints: it can only sign `localhost`,
  `*.localhost`, `*.test`, `127.0.0.0/8` and `::1`. Certificates are minted only for local site names; the
  256-entry leaf cache replaces its oldest entry instead of refusing new names. A CA made before is
  recognised at start (one-line warning) and `--renew-ca` replaces it (the old one is kept as `ca.crt.old`).

## 1.4.2 — 2026-11-16

- Windows: the 1.4.1 fix was not enough — with `_FORTIFY_SOURCE` the MinGW of Ubuntu still routes `snprintf`
  to the `msvcrt` implementation. `%zu` is gone from the code (sizes are printed as `%llu`), so
  `CONTENT_LENGTH` for PHP is right whatever C runtime the executable uses.

## 1.4.1 — 2026-11-16

- Windows: the executables built by the release workflow (MinGW of Ubuntu, `msvcrt` printf) did not
  understand `%zu`: the `CONTENT_LENGTH` given to PHP could be wrong, so POST bodies to PHP scripts were lost.
  Windows builds now use MinGW's C99 printf (`__USE_MINGW_ANSI_STDIO`).
- macOS: the server builds again (`prctl` is Linux-only).
- CI: Windows ACL test accepts the built-in administrator account of the runners; no compiler warnings left.

## 1.4.0 — 2026-11-16

Second round of the code audit (details and tests for each item in `TODO.md`).

**Storage engine**
- Transaction log v2: every record has a CRC-32C and an LSN; a torn or damaged log is recognised, never
  replayed. Checkpoints also run with transactions open (the log no longer grows without bound).
- Data file format 2: every page carries its LSN and a CRC-32C; before a page is overwritten its image goes
  to the log, so a torn page write is repaired. A 1.3 database is converted when it is opened (a copy is
  kept until the conversion ends).
- `ROLLBACK` / `ROLLBACK TO` work on the pages in memory (no flush, no `fsync`).
- `UPDATE` rewrites a row in place when it fits; space of replaced large rows and dropped tables is reused at
  once; `VACUUM FULL` compacts tables and shortens the file.
- Catalog without fixed limits (format v6, explicit encoding): up to 1 024 columns per table, any number of
  tables and databases; indexes and constraints allocated on demand (448 bytes per table instead of 30 KB).
- Schema changes are records in the log instead of a full catalog rewrite with two `fsync`s: 1 000
  `CREATE TABLE` in 0.3 s (4.4 s before on an NVMe disk). `TRUNCATE` gives the table a new empty page chain
  (before, its deletions were not logged and came back after a crash).
- Page cache split into partitions with disk reads outside the latch; evicted page buffers are reused.

**Concurrency**
- Lock waits are FIFO queues with condition variables (no polling, no writer starvation); deadlock detection
  over the whole wait-for graph. A plain `SELECT` inside a transaction releases its table locks at the end of
  the statement (READ COMMITTED): `BEGIN; SELECT …; UPDATE …` in two sessions no longer deadlocks.

**Security fixes**
- Local CA and site keys are private from the moment they are created (they were world-readable until the
  key had been written); the `certs` folder is created private.
- Reverse proxy: a malformed status line from the backend caused a heap buffer overflow.
- `.htaccess`: `%N` captures beyond 4 KB of condition text read stack memory into URLs; rewritten paths with
  `..` made rules outside the site root apply.
- `REGEXP` with deeply nested parentheses, `LIKE` and `JSON_SEARCH` with many `%`, could crash the server or
  never return: now bounded / linear.
- `--install-service --token`: the token is passed in the service environment, not on its command line.
- Server threads get an explicit 8 MB stack (deeply nested views could overflow a 1 MB Windows stack).

**GUI**
- Engine calls run on a worker thread (the window stays responsive during long statements; lock waits can be
  cancelled); input methods (XIM) and per-monitor UI scale on Linux.

**Authentication**
- Passwords: PBKDF2-HMAC-SHA256 with the iteration count stored in the hash (100 000); older hashes are
  redone on the next successful login.
- MySQL protocol: `caching_sha2_password` (MySQL 8 / 9 clients), with full authentication over TLS, the Unix
  socket or the server's RSA key; `mysql_native_password` only for users created `IDENTIFIED WITH
  mysql_native_password`.
- Wrong passwords: after 5 in a row for one user from one address, each new attempt waits 1, 2, 4 … s
  (up to 5 minutes).
- Windows: data folder and engine files get an owner-only ACL.

**Performance**
- MySQL prepared statements are parsed once: each `COM_STMT_EXECUTE` writes the values into the parsed tree
  (point lookups 2× faster in the engine); the tree is redone after a schema change.

**Exact numbers**
- `DECIMAL` values, literals with a decimal point and integers beyond 64 bits are exact decimals (up to 81
  digits) with MySQL's scale rules: `0.10 + 0.20 = 0.30`, `SUM` / `AVG` of `DECIMAL(20,2)` without loss or
  exponent, `ROUND` / `TRUNCATE` exact; results are typed `DECIMAL` (MySQL `NEWDECIMAL`).
- `BIGINT UNSIGNED` up to 18446744073709551615.

**Optimizer**
- `ANALYZE TABLE` collects column statistics (distinct values, NULL fraction, 16-bucket histogram), kept in
  `segvault.stats`; estimates use them. Inner joins of 3+ tables run in the cheapest order whatever the
  written order; an index lookup on a value that covers over a quarter of the table becomes a scan;
  `num_rows` stays exact after `ROLLBACK`; `EXPLAIN FORMAT=JSON`.

**HTTP API**
- `max_rows` + cursor pagination on `/query`; `big_ints: "string"`; `GET /api/v1/metrics` (Prometheus);
  engine counters (buffer pool, log, lock waits, deadlocks, open transactions) also in MySQL `SHOW STATUS`.

**Web server**
- Request bodies over 1 MB are written to an unlinked temporary file as they arrive (server memory stays
  flat during large uploads); `--max-body` (default 128 MB) also sets PHP's `post_max_size` /
  `upload_max_filesize`; bodies slower than 1 KB/s are cut; at most 32 connections per remote address
  (`--max-per-address`); `--max-header-size`.

## 1.3.0 — 2026-11-15

Fixes from the code audit of 1.2.0 (details and tests for each item in `TODO.md`).

**Security**
- Privileges are checked on every table a statement reads or writes (CTEs, subqueries in `SET` / `RETURNING`
  / window clauses / `ON DUPLICATE KEY UPDATE`, multi-table `UPDATE` / `DELETE`, `MERGE`, `LATERAL`, trigger
  bodies); `DESCRIBE`, `SHOW CREATE`, `SHOW TABLES`, `information_schema` hide tables without privileges.
- Saved endpoints run with the rights of the user who saved them (`api_endpoints.definer`); `public` only for
  administrators.
- Engine files `0600`, data directory `0700`; PHP workers on private Unix sockets; passwords hashed outside the
  engine lock and logins re-checked after `ALTER USER`; passwords and control bytes removed from logs.
- Windows path aliases (`a.php.`, `A~1.PHP`) and `.phtml` / `.inc` sources are never served.
- HTTP/2: malformed header blocks refused, rapid-reset and PING floods answered with `GOAWAY`.

**Durability and data integrity**
- Write and `fsync` errors are reported (never a false "OK"); the engine stops writing until restarted.
- `ROLLBACK` and crash recovery are restartable at any point (compensation records); rows return to their
  own slot, so rolling back large deletes on full pages no longer loses rows.
- Recovery streams the log (memory bounded by open transactions) and refuses to start on any error.
- Damaged pages, catalogs and users files are refused instead of yielding fewer rows or an empty database.
- Binary values keep every byte, `0x00` included (`X'…'`, `\0`, MySQL and JSON parameters, results, exports).
- `SET synchronous = FULL | NORMAL | OFF`; autocommit writes are synced within 100 ms by default.
- Shutdown with transactions open in other sessions keeps the log; nothing uncommitted becomes permanent.
- Windows: 64-bit file offsets (data files over 2 GiB) and atomic file replacement.

**Performance**
- `ROLLBACK` costs what the transaction changed, not the size of the database; savepoints need no `fsync`.
- Page cache sized from the RAM (`--cache-size`, `SET GLOBAL cache_size`); committed-version reads with many
  pending changes are linear; parallel `SELECT`s are free of data races.
- Space of dropped tables and replaced large rows is reclaimed by an automatic `VACUUM`.

**Protocols**
- MySQL: `PREPARE` no longer executes the statement (no side effects, no double run), `SET @var`, `FOREIGN_KEY_CHECKS` restored after dumps, packets over 16 MB, `SHOW PROCESSLIST`, `KILL`,
  `COM_CHANGE_USER`, `COM_STATISTICS`, `COM_SET_OPTION`, idle-transaction timeout, Unix socket.
- HTTP: IPv6 (`[::1]`), 8 KB request targets (414 / 431 instead of silent truncation), slow-header deadline,
  64-bit `last_insert_id`.

## 1.2.0 — 2026-11-14
Full stored-routine language, session variables and MySQL operators, `PREPARE` / `EXECUTE`, `LATERAL` / `APPLY`,
`JSON_TABLE`, `GENERATE_SERIES`, `ROLLUP` / `CUBE` / `GROUPING SETS`, cross-database queries, `RETURNING`,
`ON CONFLICT`, `MERGE`, generated columns, sequences, multi-operation `ALTER TABLE`, many new functions.

## 1.1.1 — 2026-11-14
Classic MySQL dumps (classicmodels, Northwind, world, Sakila) import; the current database is the same in the
tree, the Designer and the status bar; native *Browse* dialog for opening and saving files.

## 1.1.0 — 2026-11-13
Development stack: PHP (FastCGI), per-folder sites, reverse proxy, HTTPS + HTTP/2 with a local CA, mail
catcher, `.htaccess`, CGI, concurrent transactions with committed-version reads, MySQL compatibility tested
with WordPress, independent services, tray icon, portable packages.

## 1.0.2 – 1.0.4 — 2026-11-05 … 2026-11-12
First public releases: SQL engine with WAL and recovery, GUI SQL editor, HTTP JSON API, MySQL protocol.
