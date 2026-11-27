# Changelog

## Unreleased

- `GROUP BY 1` grouped by the constant 1 (one row for the whole table); like MySQL it now means the first
  expression of the `SELECT` list, also with `ROLLUP` and in prepared statements.
- `DATE` / `DATETIME` columns compared with strings as MySQL does: `created = '2024-02-01'` now finds midnight,
  `day = '2024-01-15 00:00:00'` finds the date, and `created > '2024-02-01'` no longer includes midnight (they were
  compared as plain text). Also with indexes, `IN` and `BETWEEN`.
- `INSERT … VALUES (DEFAULT, …)` and `UPDATE … SET col = DEFAULT` work (they were "Unknown column 'DEFAULT'").
- `CHAR(n)` drops trailing spaces as MySQL does (`'ab  '` was stored and compared with its spaces). Values stored
  by older builds keep their spaces until they are written again (`UPDATE t SET c = c` rewrites them).
- Decimal literals are shown as MySQL shows them: `SELECT .5` = `0.5`, `SELECT 5.` = `5`.

## 1.5.1 — 2026-11-27

- Windows: the 1.5.0 packages could not be built (the per-thread `REGEXP` cache used POSIX thread keys, which the
  Windows build does not link); it now uses Fiber Local Storage there. No other change: use 1.5.1 instead of
  1.5.0, whose release has no packages. `test_win` exercises the cache under wine.

## 1.5.0 — 2026-11-27

**Security and fixes (code review 2026-11-26, see `CR-*` in TODO.md)**
- Permissions on names with spaces survived a restart wrongly: `GRANT SELECT ON shop.\`t 127\`` came back as
  `ALL PRIVILEGES ON shop.t`, and grants on such names were lost. `users.dat` now writes encoded names
  (`grantx` lines); ambiguous old lines are skipped, never reinterpreted. Builds before 1.5.0 do not read the new
  lines: going back to an older build, users lose their grants (accounts and passwords stay).
- The admin API (`/api/v1/query`, …) no longer trusts pages of hosted sites (`*.localhost`, `*.test`, configured
  sites): on a site's address it needs `--token` or `--users`. The server address (localhost, `--static`) is
  unchanged. SegVault data files (`segvault.*`, `users.dat`) are never served by the web server.
- SQL `BACKUP TO` refuses folders inside the data folder (also written as `data/.` or through symlinks) and,
  while the web server runs, inside its `www` folder or a site's folder. Backups from the app are unchanged.
- `LIMIT 7, 18446744073709551615` (MySQL's "to the end" idiom) returned no rows; values above 2³¹ in
  `LIMIT` / `OFFSET` / `FETCH` were truncated to 32 bits. `VARCHAR(4294967297)` is now an error.
- Two data races found with ThreadSanitizer: parallel readers right after a restart built table maps without a
  lock, and `db_current_database()` returned a pointer into the catalog after releasing the engine lock.
- HTTP API sessions with an idle transaction are now closed after 30 s even when no other API request arrives
  (their locks blocked MySQL clients and DDL until then).
- MySQL protocol: data sent with `COM_STMT_SEND_LONG_DATA` is capped at `max_body` per statement (error 1153).
- Windows: `php-cgi`, CGI interpreters and OpenSSL found in shared places (`C:\php`, `C:\xampp`, `PATH`) are used
  only if no other user owns the file or its folders; OpenSSL is loaded by full path only.
- Desktop app: backup, restore, reading and translating import files run off the window thread; a read error
  no longer imports a truncated file. Editor undo no longer crashes when memory runs out.
- Engine stack: nested queries need 512 KB of thread stack instead of more than 1 MB.
- Dead code removed and `-Wunused-function` enabled; CI actions pinned to commit SHAs.
- A `REGEXP` with exponential backtracking (`'(a*)*b'`) took 20 ms per row with the engine locked (38 s on 2 000
  rows) and then answered "no match"; it now fails the statement at the first row ("Regular expression too
  expensive"), like MySQL's `ER_REGEXP_TIME_OUT`, and honours `KILL QUERY`.
- `UPPER` / `LOWER`, `LIKE` and `_ci` collations handle accented, Greek and Cyrillic letters (`UPPER('città')` =
  `CITTÀ`, `'CITTÀ' = 'città'` in a `_ci` table); `_` in `LIKE` matches one character, not one byte. On-disk
  indexes of `_ci` columns written by older builds are rebuilt. `information_schema` reports `utf8mb4_bin` for
  tables without a collation (their comparisons were already case-sensitive).
- `LAST_INSERT_ID()` after a multi-row `INSERT` returns the first generated id, as MySQL (it returned the last).
- Integer division and `AVG` of integers return a DECIMAL with 4 decimals, as MySQL (`7 / 2` = `3.5000`).
- `SELECT … FROM DUAL` is accepted.
- `ROUND(-0.5)` returned `1` (sign lost below one); `FORMAT` rounded like `printf` (`FORMAT(2.5, 0)` = `2`), now
  half away from zero like MySQL. `_ci` comparisons also ignore accents (`'Èra' = 'era'`), and `COUNT(DISTINCT)`
  on a `_ci` column counts `'a'` and `'A'` once.
- SQL functions evaluate 4–6× faster per row (name dispatch no longer runs hundreds of string comparisons).
- A `REGEXP` with a constant pattern is compiled once instead of on every row (about 2× faster on 200 000 rows).
- New MySQL functions: text `INSERT(str, pos, len, new)`, `JSON_ARRAY_INSERT`, `JSON_MERGE_PRESERVE` / `JSON_MERGE`.
- A correlated scalar subquery with no rows returned an empty string instead of NULL (wrong `COALESCE` / `IS NULL`).
- `BINARY x` and `x COLLATE name` work in expressions (`ORDER BY name COLLATE utf8mb4_bin`, `LIKE BINARY`), and a
  column's own `COLLATE` is honoured (it was ignored: only the table's counted).
- `SHOW CREATE TABLE`, `CREATE TABLE … LIKE` and the app's dump keep `TINYINT` / `SMALLINT` / `MEDIUMINT`,
  `UNSIGNED` and collations (they became `INT` / `BIGINT`, so a dump and restore lost the limits).
- `INSERT … VALUES` enforces the `TINYINT` / `SMALLINT` / `MEDIUMINT` ranges and `INT UNSIGNED`'s 2³² − 1 (it skipped
  them; `UPDATE` and `INSERT … SELECT` already checked).
- DDL inside a transaction commits it first, as in MySQL: `BEGIN; INSERT …; CREATE TABLE …; ROLLBACK` kept the new
  table but undid the `INSERT`. `BEGIN` inside a transaction commits it and opens a new one (it was an error).
- `CAST(-1 AS UNSIGNED)` = `18446744073709551615` as in MySQL (it returned 0); `COUNT(DISTINCT a, b)` is accepted.
- JSON results use MySQL's normalized text (`{"a": 1, "b": [1, 2]}`) for `JSON_SET` / `INSERT` / `REPLACE` /
  `REMOVE` / `ARRAY_APPEND`, `JSON_EXTRACT` / `->`, `JSON_MERGE_PATCH` and `CAST(… AS JSON)`, which now rejects
  text that is not JSON.
- Code quality: one growable text buffer (`include/sv_strbuf.h`) replaces 13 hand-written copies (two of them
  had bugs: one kept appending after a failed allocation, the SQL formatter started from an unchecked `malloc`);
  test suites share `tests/harness.h` and each runs in a private `mkdtemp` folder (no fixed `/tmp` paths, nothing
  left behind); new suite `test_strbuf`, a small-stack test for nested subqueries, fuzz dictionary extended.

**Storage engine**
- Indexes on disk: at clean shutdown and on `CHECKPOINT`, the in-memory index of each indexed column becomes a
  read-only B+tree in the data file (catalog format v7). After a restart a primary-key or unique lookup reads
  2-3 pages instead of scanning the whole table to rebuild the index (20 000-row table: 3 pages instead of
  ~1 600). The first write to a table drops its trees (logged before the row, so a crash never uses a stale
  tree); `ALTER` / `TRUNCATE` / `VACUUM FULL` make them unusable; damaged index pages fall back to the table.
  `EXPLAIN` shows "B+tree on disk". Databases opened by this version (catalog format v7) cannot be opened by
  1.4.2 or older.

- The first write to a table after a restart no longer reads the whole table: the last page of each table's
  chain is kept in the catalog (format v8) and the free-space map is completed a few pages per `INSERT`
  (20 000-row table: 22 pages instead of 1 608). On that first write the other on-disk indexes of the table
  keep serving lookups (tree + in-memory changes) instead of being rebuilt by a scan. Databases opened by this
  version (catalog format v8) cannot be opened by older builds.

**Performance**
- `ROLLBACK` keeps the in-memory indexes: the keys a transaction changed are undone from a journal instead of
  rebuilding every index of the touched tables by reading them (`BEGIN; INSERT 10 rows; ROLLBACK` on a loaded
  table: 1 300 → 48 000 per second). Also for `ROLLBACK TO SAVEPOINT` and statements that fail half way.
- Parse trees are 20× smaller (66 KB → 3.3 KB per node: view / routine text and `CREATE TABLE` / `MERGE` lists
  moved out of the node): point lookups +42%, concurrent readers +37–40% against the 1.4 baseline.
- Large results stream: a `SELECT` on one table without `ORDER BY` / `GROUP BY` / `DISTINCT` / `LIMIT` is
  computed in blocks of 2 048 rows and sent to MySQL clients as it goes (beyond 4 MB the packets wait in a
  temporary file instead of memory): `SELECT *` of a 129 MB table needs +23 MB instead of +425 MB.
- Table scans copy only the text columns the query still uses after its read-time filters, and a
  single-table `LIMIT` without `ORDER BY` / grouping stops reading at `OFFSET + LIMIT` rows
  (`SELECT * … LIMIT 10` on a 57 MB table: 56 → 8 ms; a filtered `COUNT(*)`: 47 → 20 ms).

**Limits (closer to MySQL)**
- 61 tables per `SELECT` (was 9), 64 indexes and 64 constraints per table (was 16 + 16: mysqldump schemas with
  many keys failed at `CREATE TABLE`), `UPDATE … SET` / `ON DUPLICATE KEY UPDATE` / `ON CONFLICT` over any number
  of columns (was 64), `ORDER BY` / `GROUP BY` without the 8 / 16 cap, `MERGE` with up to 256 `WHEN` clauses and
  1 024 columns, 64 window functions per query. Every remaining cap names its limit in the error.

**Security**
- Names of new databases, tables, columns, indexes, constraints, views, routines, triggers and sequences can no
  longer contain a backtick or a control character (`CREATE DATABASE 'x`; DROP …'` was accepted). The GUI quotes
  every name in the SQL it generates (user permissions, drop / truncate, dumps, editor snippets, CSV import,
  SQL export) — before, a user with `CREATE` could name a database so that the administrator's next click in the
  Users panel ran `DROP DATABASE`. `SHOW GRANTS` quotes names like MySQL.
- Login throttling could be bypassed by alternating guesses with 64 invented user names (the blocked entry left
  the 64-slot table): 1 024 slots, blocking entries are never evicted first, and a per-address counter for
  remote clients.
- `.htaccess`: an unreadable file (permissions, out of file handles), very deep folders or an out-of-memory
  while loading now deny the folder instead of ignoring its rules; a rule whose `RewriteCond` cannot be loaded
  (pattern our engine rejects, more than 32 conditions) is disabled instead of running with fewer conditions.
- The GUI writes `server.token` readable only by the owner. Settings, saved queries, `server.ini`, ER layouts and
  the user's own `.sql` files saved from the editor are written atomically (a crash or full disk no longer
  leaves an empty file); the GUI no longer erases named sites past the 32nd from `server.ini`.
- A heap overflow in join reordering (`SELECT *` over joined tables with more than 8 columns, found with ASan)
  is fixed; HTTP/2 requests with more than 512 headers are refused instead of losing the extra headers.

- Keys (primary, unique, foreign, indexes) can have up to 16 columns, as in MySQL (was 8).
- GUI: the "Copy address" and "Open SQL editor" buttons of the server panel follow the theme (they stayed black
  in the light theme).

**Fixed limits that silently dropped data**
- Users panel (users, databases and table grants beyond the first 128 / 64 / 4), ER database menu (64), MySQL
  `SHOW PROCESSLIST` (256 rows), `WITH RECURSIVE` (64 parts), mail catcher (64 attachments), grid editing
  (read-only above 64 columns), SQL export header (columns dropped on wide results), dumps (a table that could
  not be read produced a "successful" partial dump).

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
