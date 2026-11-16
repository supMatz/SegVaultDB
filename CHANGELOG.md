# Changelog

## Unreleased

**Security**
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
