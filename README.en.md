# SegVault — a relational DBMS and local development stack in plain C

SegVault is a complete **local development environment** with its own relational database, written by hand in
C11 with no dependencies: a web server with PHP, one site per folder on `NAME.localhost`, HTTPS + HTTP/2 with a
local CA, a reverse proxy for Vite / Next.js / Django, a mail catcher, the **MySQL network protocol** with
concurrent transactions and committed-version reads, an HTTP/JSON API, a SQL editor and visual tools — in one
1.8 MB package for **Windows** and **Linux**. It is meant as an alternative to XAMPP, UniServerZ and Laragon,
and as a lab for database courses (plans, indexes, buffer pool, log, transactions, triggers).

The main documentation is the Italian [README.md](README.md); this page is a summary.

## Download and start

Packages for Windows and Linux are attached to every [release](https://github.com/supMatz/SegVaultDB/releases).

```bash
make                        # ./segvault (desktop app) and ./segvault-server
./segvault                  # asks: SQL editor or API server (data in ./data)
./segvault-server --static www --php auto --mysql-port 3306
```

`segvault-server --help` lists every option: `--token`, `--users` (logins and `GRANT`), `--https`, `--site`,
`--cache-size`, `--synchronous FULL|NORMAL|OFF`, `--audit`, `--backup-every`, ...

## What it does

| Area | Details |
|---|---|
| Web | static files, PHP over FastCGI (zero configuration), per-folder sites, `.htaccess` (`mod_rewrite`), CGI, reverse proxy with WebSocket, HTTPS + HTTP/2, IPv6 |
| Database | SQL with subqueries, CTEs (recursive), window functions, `MERGE`, `RETURNING`, `LATERAL`, `JSON_TABLE`, views, constraints, triggers, stored procedures and functions, sequences, generated columns |
| Storage | 4 KB pages with slot directory and free-space map, overflow chains for large rows, in-memory hash / ordered indexes, CLOCK buffer pool |
| Transactions | write-ahead log with compensation records, restartable rollback and recovery, savepoints, table locks with deadlock detection, committed-version reads, configurable durability (`synchronous`) |
| Protocols | HTTP/JSON API with bound parameters and sessions, MySQL protocol (text and prepared statements, TLS, Unix socket) usable by PHP, Python, Java and the `mysql` client |
| Security | users and privileges checked on every table access, saved endpoints with their definer's rights, owner-only data files — see [SECURITY.md](SECURITY.md) |

## More

* [docs/API.md](docs/API.md) — the HTTP API (Italian)
* [docs/FORMATS.md](docs/FORMATS.md) — on-disk formats, guarantees and limits
* [CONTRIBUTING.md](CONTRIBUTING.md) — build, tests (`make test`, fault-injection durability suite), style
* [CHANGELOG.md](CHANGELOG.md) — release notes
* [TODO.md](TODO.md) — engineering roadmap
