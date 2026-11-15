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

## Files

* On POSIX the data directory is created `0700` and every engine file (`segvault.db`, `segvault.wal`,
  `segvault.cat`, `users.dat`, the audit log) `0600`. Windows uses the default ACL of the folder.
* `users.dat` stores a salted, iterated SHA-256 of each password and, for the MySQL protocol,
  `SHA1(SHA1(password))` (what `mysql_native_password` needs). Anyone who can read the file can try to crack
  the second form much faster: keep the file private.
* PHP runs as `php-cgi` processes listening on Unix sockets inside a private `0700` folder (POSIX); on Windows
  they use a port of `127.0.0.1`.

## Logs

Every log line (stderr, the request log, the audit file) escapes control bytes and hides the string after
`IDENTIFIED BY` / `PASSWORD`. The audit file still contains the SQL text and bound values of every
modifying statement: treat it as confidential.

## Reporting a vulnerability

Open a private security advisory on the GitHub repository (Security → Report a vulnerability), or an issue
without exploit details asking for a contact. Please include the version (`/api/v1/health`) and the steps.
