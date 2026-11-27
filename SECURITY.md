# Security model

SegVault is a local development stack: a database engine, an HTTP / MySQL server and a web server for
PHP sites. This page states what each protection covers, so that nobody relies on one that is not there.

## Who can do what

| Setup | Who gets in | With which rights |
|---|---|---|
| no `--token`, no `--users`, bound to `127.0.0.1` (default) | every process on this computer | everything (root) |
| `--token T` | whoever sends `Authorization: Bearer T` (HTTP) or logs in as `root` with password `T` (MySQL) | everything (root) |
| `--users` | users created with `CREATE USER`, logging in with HTTP Basic or the MySQL protocol | the privileges granted with `GRANT`; administrators (`WITH GRANT OPTION`) everything |
| saved endpoint with `access = 'public'` | anyone, from any web site | the privileges of the user who saved it (see below) |

* **No users means everyone is root.** A missing `users.dat` is "no users"; a damaged or truncated one makes
  the engine refuse to start (since 1.3), it never silently falls back to "no users".
* Binding to anything other than `127.0.0.1` requires `--token`, `--users` or an explicit `--no-auth`.
* Privileges are checked on every table a statement reads or writes, in the executor where the table is
  opened: CTEs, subqueries anywhere (`SET`, `RETURNING`, window clauses, `ON DUPLICATE KEY UPDATE`), multi-table
  `UPDATE` / `DELETE`, `MERGE`, `LATERAL`, triggers and procedures (with the rights of the user who runs them).
  Views run with the rights checked when the view was created (`CREATE VIEW` executes its body).
  `DESCRIBE`, `SHOW CREATE`, `SHOW TABLES` and `information_schema` list only tables the user has a privilege on.
* **Saved endpoints** (`api_endpoints`) run with the privileges of their *definer*, a column the engine fills
  with the session user on every write. `public` is honoured only for endpoints saved by an administrator.
  Only administrators can change the structure of `api_endpoints`.
* **The admin API answers only on the server's own address** (since 1.5). Pages of hosted sites
  (`NAME.localhost`, `*.test`, `--site` names) cannot use `/api/v1/query` and the other admin endpoints in the
  open setup, whatever their `Origin`: on a site's address they need `--token` or `--users`. Before 1.5 any
  JavaScript served by SegVault (a plugin, an npm bundle, an XSS) could run SQL as root.
* **New names cannot break out of quoting**: database, table, column and user names with a backtick or a control
  character are refused (since 1.5); the desktop app quotes every name it writes into SQL.

## Files

* On POSIX the data directory is created `0700` and every engine file (`segvault.db`, `segvault.wal`,
  `segvault.cat`, `users.dat`, the audit log) `0600`. Windows uses the default ACL of the folder.
* `users.dat` stores a salted, iterated SHA-256 of each password and, for the MySQL protocol,
  `SHA1(SHA1(password))` (what `mysql_native_password` needs). Anyone who can read the file can try to crack
  the second form much faster: keep the file private.
* The web server never serves SegVault's own files (`segvault.*`, `users.dat`, `sequences.txt`), in any folder.
* SQL `BACKUP TO` refuses folders inside the data folder and, while the web server runs, inside its `www`
  folder or a site's folder (symlinks resolved), so a backup cannot land where it would be served. Backups made
  from the desktop app choose their folder freely.
* Logins are throttled per client address (1 024 tracked addresses; loopback exempt).
* PHP runs as `php-cgi` processes listening on Unix sockets inside a private `0700` folder (POSIX); on Windows
  they use a port of `127.0.0.1`.

## Logs

Every log line (stderr, the request log, the audit file) escapes control bytes and hides the string after
`IDENTIFIED BY` / `PASSWORD`. The audit file still contains the SQL text and bound values of every
modifying statement: treat it as confidential.

## Reporting a vulnerability

Open a private security advisory on the GitHub repository (Security → Report a vulnerability), or an issue
without exploit details asking for a contact. Please include the version (`/api/v1/health`) and the steps.
