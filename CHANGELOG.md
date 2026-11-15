# Changelog

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
