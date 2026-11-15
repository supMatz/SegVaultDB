# SegVaultDB — Engineering Roadmap

Audit baseline: **v1.2.0** (`124ec1a`, branch `php-fastcgi`), audited 2026-11-14.
All `file:line` anchors refer to that commit. The previous roadmap (XAMPP / UniServerZ parity) is
complete and summarised at the bottom; its original text is `git show v1.2.0:TODO.md`.

How the findings were obtained:

- `make all` (0 warnings), `make test` (22 suites), `make test-asan` and `make test-tsan` on a scratch copy of
  the tree: all green. Every defect below therefore lies outside what the current suites exercise.
- Line-by-line read of `src/storage`, `src/tx`, `src/table`, `src/index`, `src/catalog`, `src/bridge`,
  `src/server/{server,main,mysql_wire,fastcgi,h2}.c`, `src/query/{lexer.c,parser.h}`; targeted reads of
  `src/query/executor.c`, `src/server/cgi.c`, `src/platform/xlib.c`.
- In-process probes linked against `libsegvault.a`, and a ThreadSanitizer build of the engine.
- **Not** read line by line (grep and spot checks only — see P3.9): `src/query/parser.c`, most of the 12 k lines
  of `src/query/executor.c`, `src/query/{regex,sqljson}.c`, `src/server/{tls,mail,rewrite,proxy,json,service}.c`,
  `src/widgets/*` apart from the engine call sites in `window.c`, `src/platform/win32.c`.

Status tags: **[reproduced]** = observed on a running build; **[code]** = follows from the cited code,
not executed; **[verify-win]** = Windows-only, needs a run on Windows.

`[ ]` to do · `[~]` in progress · `[x]` done.

**Status after v1.3.0 (2026-11-15):** fixed `[x]` P0.1-P0.12 (Windows parts verified under wine), P1.2, P1.3, P1.4, P1.8, P1.12, P2.1, P2.9, P2.11, P2.13, P3.1, P3.4, P3.7;
partly done `[~]` P1.5, P1.10, P2.2, P2.3, P2.4, P2.6, P2.7, P2.8,
P2.10, P2.12, P3.2, P3.3, P3.5. Every item has a **Fixed / Done** note with what changed and the test that covers it.
Still to do: P1.1, P1.6, P1.7, P1.9, P1.11, P1.13 (WAL v2, on-disk B+tree, locks, decimals, catalog in
system tables, statistics), P2.5, P2.14, P2.15, P3.6, P3.8, P3.9 and the open parts of the `[~]` items. New suites: `tests/test_durability.c`
(fault injection), `tests/test_h2.c`.

---

## P0 — Critical Stability, Memory Safety & Data Integrity

- [x] **P0.1 `[SECURITY]` Privilege checks miss whole statement branches (any user can read / change other tables)** — [reproduced]
  - **Where:** `src/bridge/auth.c:594-738` (`auth_check`, `walk_node`, `walk_select`, `walk_expr`, `need`, `is_cte`);
    `src/query/executor.c:9335,9459` (`auth_hook` is called only when `sc->proc`).
  - **Why:** authorization is a second, hand-written walk over a *subset* of the AST, so every construct it does
    not visit is unchecked. With `GRANT SELECT,INSERT,UPDATE,DELETE ON d.mine TO bob` (nothing on `d.secret`)
    all of these succeed as `bob`:
    `WITH secret AS (SELECT * FROM secret) SELECT * FROM secret` (`is_cte` skips the name, `auth.c:604-610,635-637`);
    `SET @x = (SELECT pw FROM secret LIMIT 1)` (`NODE_SET` needs nothing, `auth.c:686`);
    `INSERT … RETURNING (SELECT …)` (`ASTNode.returning` not walked);
    `UPDATE mine m JOIN secret s … SET m.x = s.pw` and `DELETE mine, secret FROM mine JOIN secret …`
    (`multi_from` / `targets` are text, `auth.c:662-670`) — the second one deleted `secret`'s rows;
    `INSERT … ON DUPLICATE KEY UPDATE x = (SELECT …)` (`dup_exprs`); window `OVER (ORDER BY (SELECT …))` (`win`);
    `CREATE TABLE copy AS SELECT * FROM secret` (`as_select`, `auth.c:710`);
    `CREATE TRIGGER … ON mine … UPDATE secret …` (needs only `ALTER` on `mine`; trigger bodies never reach `auth_hook`).
    `DESCRIBE secret`, `SHOW CREATE TABLE secret` and `information_schema.columns` also expose tables the user has
    no privilege on (`auth.c:694-699`). False denials exist too: `JSON_TABLE(...) jt` and aliased multi-table
    `DELETE m, s FROM …` are refused because the alias is treated as a table.
  - **Do:** move enforcement to the one place tables are resolved for reading or writing in the executor
    (add `user` + required privilege to `ExecCtx` and check in the table-open helper; views keep definer semantics;
    trigger and function bodies run with the definer recorded at `CREATE` time). Keep `auth_check` only for
    statement-level rules (DDL, admin-only statements). Filter `DESCRIBE` / `SHOW CREATE` / `information_schema`
    per table with `auth_privs`.
  - **Done when:** `tests/test_auth.c` contains every statement above (plus `MERGE`, `LATERAL`, `VALUES`,
    `GENERATE_SERIES`, CTE in subquery, nested view) and each is denied for `bob`, while the two false denials pass.
  - **Fixed:** table privileges are checked by the executor where tables are opened (`executor_priv_ok`:
    `source_open` for every read, `exec_insert` / `exec_insert_mysql` (REPLACE → +DELETE, ON DUPLICATE → +UPDATE),
    `exec_update`, `exec_delete`, multi-table targets, `CREATE TABLE … LIKE`); `ExecCtx.user` / `priv_off`.
    Views: SELECT on the view, body runs with the rights checked at `CREATE VIEW` (which executes the body).
    Trigger bodies run with the invoker's rights (same rule as procedures). `DESCRIBE`, `SHOW CREATE`,
    `SHOW TABLES`, `SHOW TRIGGERS`, `information_schema` list only tables with some privilege. The AST walker in
    `auth_check` is gone (DML is statement-level only); `RETURNING` and `MERGE` statements are atomic.
    Tests: 21 new checks in `tests/test_auth.c`.

- [x] **P0.2 `[SECURITY]` Saved endpoints run as root, and any user who can write `api_endpoints` can create one** — [code]
  - **Where:** `src/server/server.c:1030-1060` (`endpoint_lookup` via `engine_enter`), `:1100-1120`
    (`handle_endpoint` → `run_script(..., user = NULL, ...)`), `:2343-2360,2383` (public endpoints skip token and Origin).
  - **Why:** the row's SQL is executed with an empty session user (= `AUTH_ALL`, `auth.c:277`). A non-admin with
    `ALL` on their own database (the normal per-app grant) can `CREATE TABLE api_endpoints` there, insert
    `('x', 'SELECT * FROM otherdb.secret', 'public', '')` or `CREATE USER evil …; GRANT ALL ON *.* …`, and call
    `/api/v1/endpoints/x?database=mine` — unauthenticated if `access = 'public'`.
  - **Do:** store the definer in the row (new `definer` column, filled from the session user, not from the payload)
    and execute with that user's privileges; refuse user-admin statements, DDL and cross-database names inside
    endpoints; with `--users`, require an administrator to make an endpoint `public`.
  - **Done when:** a test in `tests/test_server.c` creates the endpoint as a restricted user and gets
    `Access denied` for the foreign table, and a pre-1.3 table without `definer` still works for root-created rows.
  - **Fixed:** the engine writes `api_endpoints.definer` with the session user on every `INSERT` / `UPDATE`
    (after `BEFORE` triggers, `definer_force` in `executor.c`); `auth_check` lets only administrators `ALTER` /
    `RENAME` it, fill it with `CREATE … AS SELECT` or give it foreign keys. `db_endpoint_runas` picks the identity:
    the definer (must exist), `""` = root, no definer = root only if no non-admin can write the table, else 403.
    `public` is honoured only for admin / root definers. The endpoint runs through `run_script` with that user.
    Not done: refusing DDL / user-admin statements for root-defined endpoints (an administrator's choice).
    Tests: 7 new checks in `tests/test_server.c`; README and `docs/API.md` updated.

- [x] **P0.3 `[CONCURRENCY]` Data races and use-after-free in parallel `SELECT`s** — [reproduced, ThreadSanitizer: 34 reports]
  - **Where:** `src/query/executor.c:6992` (`g_wsort`, written at `:7210`, read in `wsort_cmp` `:6994-7003`);
    `:7666` (`g_cmp_desc`, written at `:7739`; its siblings at `:7663-7665` are `_Thread_local`);
    `:1430` (`g_ord_ix`) and `mi_ordered` `:1438-1456` / `mi_range` `:1482-1494` (lazy ordered-index build outside
    `g_mi_lock`, which only covers `mi_get_cols` `:1531-1538`). Entry point: `db_execute_readonly`, `src/bridge/db_api.c:1766-1848`.
  - **Why:** these are plain `static` comparator contexts and lazily built index arrays, touched by several reader
    threads holding the *shared* engine lock. Two window-function queries sort with each other's key arrays
    (TSan: `key_cmp` reading memory another thread `calloc`'d/freed, `win_free` `parser.c:203` vs `wsort_cmp`);
    two first range scans on the same index both `free(ix->ord)`. Result: wrong ordering, heap corruption, crash.
  - **Do:** replace `qsort` + globals with a context-carrying sort (`qsort_r` on glibc, `qsort_s` on MinGW, or a
    local merge sort taking `void* ctx`); build `ord`/`okey` under `g_mi_lock` in write mode and publish with a
    single pointer store; audit every remaining non-`_Thread_local` `static` in `executor.c`
    (`g_mem_indexes`, `g_uvars`, `g_seq`, `g_fk_checks`, `g_change_hook`) and document which lock protects it.
  - **Done when:** `tests/test_parallel.c` runs ≥6 threads mixing window functions, `UNION … ORDER BY … DESC`, and
    first-use range predicates on two indexes, and `make test-tsan` reports zero races (today's test only runs one
    equality-join `COUNT`, so it cannot see them).
  - **Fixed:** `g_wsort`, `g_cmp_desc`, `g_ord_ix` are `_Thread_local`; `mi_range` builds / reads the ordered
    view under `g_mi_lock`; `key_seed_init` publishes the seed once (atomic flag + lock); the remaining statics are
    documented with the lock that protects them. `tests/test_parallel.c` gained an 8-thread × 12-round mixed phase
    (window functions, `UNION … ORDER BY … DESC`, first-use range scans on two indexes, results compared with the
    single-thread ones): before the fix TSan reported 53 races + 2 use-after-free + SEGV, after it 0 (3 runs).

- [x] **P0.4 `[DURABILITY]` Write and `fsync` failures are ignored on every durability path** — [code]
  - **Where:** `src/bridge/db_api.c:1211-1223` (`db_commit_impl` ignores `wal_write` and `wal_flush`);
    `src/tx/wal.c:82-87` (`wal_flush` drops the `fsync` result), `:422-430` (`wal_truncate`);
    `src/storage/buffer_pool.c:52-64` (`frame_release` frees a dirty frame even when `page_write` failed),
    `:210-233` (`bp_flush_all_nl` always returns `SV_OK`, `fsync` unchecked at `:231`),
    `:158,170,184` (`page_write` unchecked in `bp_new_page_nl`); `src/bridge/db_api.c:171-176` (`do_checkpoint`
    truncates the log after an unchecked flush).
  - **Why:** on `ENOSPC` / `EIO`, `COMMIT` still answers OK; an evicted dirty page is silently dropped (later reads
    see the old page); a checkpoint empties the WAL although pages never reached the disk.
  - **Do:** make `wal_flush`, `bp_flush_all`, `bp_flush_page`, `frame_release` return errors and propagate them.
    A failed commit record → the transaction is rolled back and the statement returns an error. A failed page write
    → the frame stays dirty and pinned-out of eviction, `bp_fetch` returns NULL, the engine enters a read-only
    "I/O error" state (every write statement fails with one clear message) until restart. Never truncate the WAL
    unless flush + `fsync` succeeded. Treat a failed `fsync` as fatal for that file descriptor (do not retry).
  - **Done when:** the fault-injection harness of P3.1 fails the N-th `pwrite` / `fsync` and (a) `COMMIT` reports
    the error, (b) after restart every transaction that reported success is present and none that reported failure is.
  - **Fixed:** `wal_write` / `wal_write_buffer` / `wal_flush` / `wal_truncate` return errors and set the sticky
    `WAL.failed`; the buffer pool's `before_write` hook returns `bool`, `frame_release` keeps a dirty frame whose
    write failed (never dropped), `bp_flush_all` / `bp_flush_page` / `bp_new_page` report errors, `fsync` failure
    sets the sticky `BufferPool.io_error`. `COMMIT`: on failure the log is truncated back to before the commit
    record, the transaction ends as not committed and the error is returned. Checkpoint truncates only after a
    successful WAL + page flush. Once either flag is set every statement fails with one clear message until restart
    (`io_failed()` in `db_api.c`) and shutdown writes nothing. Test: `tests/test_durability.c` (new, built with
    `-DSV_FAULT_INJECTION`: write / pwrite / fsync shims in `platform_compat.h`), scenarios 1-2.

- [x] **P0.5 `[ACID]` `ROLLBACK` is not crash-safe: `ABORT` is logged before the undo, and recovery never undoes an aborted transaction** — [code]
  - **Where:** `src/bridge/db_api.c:1225-1245` (`db_rollback_impl`: `LOG_ABORT` + `wal_flush`, then `bp_flush_all`,
    then `wal_undo_tx`), `:1278-1304` (`db_rollback_to_impl`); `src/tx/wal.c:332-414` (`wal_undo_tx_from` writes
    through a second fd and closes it without `fsync`); `src/tx/wal.c:274-284,300-314` (recovery treats `LOG_ABORT`
    like `LOG_COMMIT`: the transaction is redone and not undone); `src/tx/wal.c:177-205` (`undo_entry` re-inserts
    the old image as a new slot, so repeating it duplicates rows).
  - **Why:** (1) a process kill in the middle of a long rollback leaves a half-undone transaction that recovery
    considers finished; (2) `bp_flush_all` makes the transaction's pages durable *before* the undo, the undo writes
    stay in the page cache, so a power loss after `ROLLBACK` returned brings the rolled-back rows back;
    (3) a crash during recovery's own undo duplicates rows on the next start.
  - **Do (minimal, before P1.1):** write `LOG_ABORT` only after the undo finished and was `fsync`ed; in recovery,
    undo every transaction without `LOG_COMMIT` (an `ABORT` record only marks "undo completed"); make
    `undo_entry` idempotent by checking the slot state the way `replay_entry` does (`wal.c:137-175`).
    **Do (final):** compensation log records, see P1.1.
  - **Done when:** harness tests "kill at the k-th undo write", "restore the data file from before the undo, keep
    the WAL" and "kill during recovery undo" all end with exactly the committed rows, for every k.
  - **Fixed:** compensation records (`LOG_CLR`, `wal.h`): every undo step is logged, the log is `fsync`ed, then
    the pages are written and the data file `fsync`ed, and only then `LOG_ABORT` (= "undo complete") is written.
    Undo now puts a row back **into its original slot** (`page_revive_slot` / `ov_slot_revive`), which is
    idempotent and always fits — the old "append a new slot" undo could not fit on full pages and silently lost
    rows (found by the new test: rolling back a 200-row `DELETE` on full pages). Recovery redoes CLRs, skips
    compensated records, undoes every transaction without `COMMIT` / `ABORT` with CLRs of its own, so it can be
    interrupted and restarted. Test: crash at every page write of a `ROLLBACK` (37 points) and of recovery
    (367 points) all end with exactly the committed rows.

- [x] **P0.6 `[ACID]` Crash recovery can discard committed work and the engine starts even when recovery failed** — [code]
  - **Where:** `src/tx/wal.c:207-326` (`wal_recovery`): whole log loaded into RAM (`:229-285`); `realloc` failure →
    `break` (`:241-242`) and the function still redoes, undoes and `remove(log_path)` (`:324`); no `fsync(db_fd)`
    before `:323-324`; `active_tx[1024]` silently full (`:276`); `LOG_INSERT` into a page that does not exist is
    skipped (`:145-149`). `src/bridge/db_api.c:119` ignores the return value; `:122-142` never checks
    `catalog_load`, `bp_create`, `txm_create`, `wal_create` for NULL before `initialized = true`.
  - **Why:** out of memory or a short read in the middle of the log deletes the remaining committed transactions;
    a second crash right after recovery loses the redone pages (log already removed); new heap pages are written
    but never `fsync`ed and their chain links are not logged (`src/table/heap.c:259-261`), so after a power loss a
    committed insert on a fresh page is dropped without a message; with a NULL WAL the engine runs without a log.
  - **Do:** two-pass streaming recovery (pass 1: transaction table with offsets only; pass 2: redo; pass 3: undo
    reading backwards) with O(open transactions) memory; any allocation or read error → return `SV_ERR` and keep
    the log; `fsync` the data file, then truncate the log, then `fsync` the directory; `db_init_impl` fails with
    a specific `db_init_error()` if recovery or any component fails; log page allocation and chain linking (or let
    redo recreate and re-link a missing page).
  - **Done when:** recovery of a 2 GB log peaks under 64 MB RSS; injecting `ENOMEM` into recovery leaves log and
    data untouched and `db_init` returns false; "crash immediately after recovery" loses nothing.
  - **Done:** recovery returns an error and keeps the log on any read / allocation / write failure (no more
    `break` + `remove`); transaction table is dynamic (no 1024 limit); data file `fsync`ed before the log is removed,
    then the directory `fsync`ed; `db_init` fails with `db_init_error()` = "crash recovery failed: …" and checks
    `catalog_load` / `bp_create` / `txm_create` / `wal_create`. New `LOG_LINK` record (heap page allocation and chain
    link, via `BufferPool.on_link`): redo recreates missing pages and re-links them; the file is extended to the
    highest page named by the log first, so overflow allocations during redo cannot take a missing page's id.
  - **Done (2nd pass):** recovery is streaming: pass 1 reads only record headers and keeps, per transaction still
    open, the offsets of its page records (freed at its `COMMIT` / `ABORT`; `ROLLBACK TO` truncates, CLRs mark);
    pass 2 redoes one record at a time; pass 3 re-reads just the records to undo, newest first. Tests in
    `tests/test_durability.c`: a 24+ MB log recovers with < 16 MB more RSS than a plain start; a read error
    during recovery makes `db_init` fail with the log untouched and the next start succeeds; a crash right after
    recovery loses nothing.

- [x] **P0.7 `[DATA]` A damaged or newer catalog is replaced by an empty one; file replacement is not atomic on Windows** — [code] / [verify-win]
  - **Where:** `src/catalog/schema.c:157-255` (`catalog_load`: `version <= CAT_VERSION` at `:170`, then
    "starting with an empty catalog" `:249-253`); `:188` (v5 tables read raw, counts and NUL terminators not
    validated); `:257-286` (`catalog_save`: no directory `fsync`; Windows `remove(path)` then `rename` `:281-284`);
    same pattern for users in `src/bridge/auth.c:175-194` (no `fsync` at all).
  - **Why:** a truncated file, or data opened with an older binary, yields an empty catalog that the next DDL or
    shutdown writes over the real one — every database disappears while its pages are still in `segvault.db`.
    On Windows a crash between `remove` and `rename` leaves no catalog. A lost `users.dat` turns logins off,
    because an empty user list means "everyone is root" (`auth.c:277`, `server.c:3035`).
  - **Do:** on any load failure refuse to start and keep the file (offer `--repair` that renames it to
    `segvault.cat.corrupt-<time>`); if only `segvault.cat.tmp` exists, load it; reject `version > CAT_VERSION` with
    "created by a newer SegVault"; validate every count and force NUL termination on load; replace files with
    `MoveFileExA(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows and `fsync` the directory on POSIX;
    add a CRC32 trailer to both files; when `--users` is set and `users.dat` is unreadable, refuse to start.
  - **Done when:** truncating `segvault.cat` at 10 random offsets and bumping its version field both make `db_init`
    fail with the file unchanged; a kill between `remove` and `rename` (hook in the test build) still starts.
  - **Fixed:** `catalog_load` returns NULL with `catalog_load_error()` on any failure (truncated, damaged,
    `version > CAT_VERSION` = "created by a newer SegVault"), never an empty catalog; `db_init` fails and the file
    is untouched. Every count, name and text is validated / NUL-terminated on load. New CRC-32 trailer (`CRC1` +
    crc, appended after the v5 fields, so v1.2 still reads the file; files without it are accepted as legacy).
    Only `segvault.cat.tmp` present → loaded and renamed. File replacement through `sv_replace_file`
    (`MoveFileExA(REPLACE_EXISTING | WRITE_THROUGH)` on Windows, `rename` + directory `fsync` on POSIX), also for
    `users.dat`, which now gets `fsync` and a final `end N` line: a truncated users file makes `db_init` fail
    instead of meaning "no users = everyone is root". Test: `tests/test_durability.c` scenario 7.
    **Not done:** `--repair`; the Windows path is cross-compiled only ([verify-win]).

- [x] **P0.8 `[DATA]` Values are C strings: bytes after a `NUL` are silently lost and binary literals are stored as text** — [reproduced]
  - **Where:** `src/table/tuple.c:45-110` (`tuple_serialize` uses `strlen`, `:58,92`), `tuple.h` (`Value.str_val`;
    `blob_val` / `blob_len` are never used); `src/bridge/db_api.h:31-36` (`Cell.value` is a `char*`);
    `src/query/lexer.c:261-294` (`0x…` / `X'…'` with non-printable bytes is kept as the text `0x…`),
    `:214-229` (`\0` and `\Z` are not escapes); `src/server/mysql_wire.c:614-627,696,752` (binary parameters with
    `NUL` are rejected with error 1210); `src/server/server.c:441` (same for JSON params).
  - **Why:** `SELECT LENGTH(UNHEX('410042'))` returns 1; `INSERT … VALUES (x'41004243')` stores the ten characters
    `0x41004243`; `CONCAT('a', CHAR(0), 'b')` is `ab`. PHP `serialize()` output with private / protected
    properties, binary UUIDs, images and `mysqldump --hex-blob` data are corrupted or refused.
  - **Do:** carry an explicit length everywhere: `Value` (`len` next to `str_val`), `EvalVal`, `Cell` (`len` field,
    `value` stays NUL-terminated for the GUI), row serialisation (already length-prefixed), `QueryResult`
    encoders (`mw_lenstr` with the real length, JSON as `\u0000` or base64 for `BLOB`), lexer (decode `0x…` /
    `X'…'` / `\0` / `\Z` to bytes), functions (`LENGTH`, `HEX`, `UNHEX`, `CONCAT`, `SUBSTRING`, `CHAR`, comparisons
    with `memcmp`). Accept `NUL` in bound parameters once the path is length-safe.
  - **Done when:** a round-trip test stores all 256 byte values in `BLOB`, `VARBINARY` and `TEXT` through
    `db_execute`, the HTTP API and the MySQL protocol (text and prepared) and reads back identical bytes;
    a `mysqldump --hex-blob` file imports byte-exact.
  - **Fixed (internal byte encoding instead of length-carrying values):** inside the engine byte 0x00 is stored
    as `C0 80` and byte 0xC0 as `C0 81` (`src/table/binesc.h`; 0xC0 never occurs in valid UTF-8, so text is
    unchanged and every C-string path — rows, indexes, WAL, re-lexed SQL — stays exact). Encoded where bytes
    enter: `X'…'` / `0x…` literals now decode to the real bytes (no more `0x…` text), `\0` and `\Z` escapes,
    MySQL binary parameters (no more error 1210), JSON parameters with `\u0000`, `CHAR`, `UNHEX`,
    `FROM_BASE64`. Decoded where bytes leave or are counted: MySQL text and binary rows (real length), HTTP
    JSON (`\u0000`), CSV / JSON exports, SQL exports and grid edits (`X'…'`, valid for MySQL too), `LENGTH`,
    `OCTET_LENGTH`, `BIT_LENGTH`, `HEX`, `TO_BASE64`, `MD5` / `SHA*` / `CRC32`; `db_value_bytes()` for C API
    users. Tests: all 256 byte values through `X''` and a prepared statement and back through the text protocol,
    NUL functions and escapes, HTTP `\u0000` round-trip (`tests/test_server.c`), binary export literal
    (`tests/test_grid_edit.c`). **Limits:** character functions (`SUBSTRING`, `UPPER`, comparisons) see the
    internal form; the GUI grid shows binary values undecoded.

- [x] **P0.9 `[ACID]` Stopping the server makes other sessions' uncommitted changes permanent** — [code]
  - **Where:** `src/bridge/db_api.c:1480-1502` (`db_shutdown_impl` rolls back only `g_db.exec_ctx.tx_id`, flushes
    all pages, then `remove(g_db.log_path)` at `:1495`); `src/server/server.c:4064-4074` (`sv_server_free` gives up
    after ~5 s with connection threads still running, `:4072`); `src/server/main.c:399-401`.
  - **Why:** a session whose thread is still inside a non-interruptible write when the stop arrives keeps its
    transaction open; shutdown writes its dirty pages and deletes the only record that could undo them.
  - **Do:** in `db_shutdown_impl`, when `g_open_tx > 0` after rolling back every session the engine knows
    (iterate the `TxManager` slots, not just the current context), keep the WAL and skip the final checkpoint so
    the next start runs recovery; make `sv_server_free` wait for the threads (or mark the engine "stopping" so
    new statements fail) instead of returning early.
  - **Done when:** `tests/test_sessions.c` opens a transaction in session A, calls `db_shutdown()` from the default
    session, re-opens the data directory and finds A's rows gone.
  - **Fixed (engine side):** shutdown flushes the WAL and pages, but removes the log only when no transaction
    of any session is open (`g_open_tx == 0`) and every write succeeded; otherwise the next start recovers. The
    other sessions' state does not survive a restart: `g_open_tx`, the uncommitted-change list and the lock table
    are reset (`lk_reset`), and a `DbSession` from before the restart loses its transaction (`DbSession.gen`) —
    without this a restarted engine read 50 duplicated rows through the committed-version snapshot. Test:
    `tests/test_durability.c` scenario 5. `sv_server_free` now waits up to 30 s for connection threads (with a
    message); if one is still inside a statement, `db_shutdown` waits for it, keeps the log and every later engine
    call fails cleanly ("DB non inizializzato").

- [x] **P0.10 `[DATA]` Read errors and damaged pages end a scan silently, and a corrupt page can write out of bounds** — [code]
  - **Where:** `src/table/heap.c:428-429` (`bp_fetch` NULL → `break`: the scan just ends), `:452` (unreadable
    overflow chain → row skipped), `:483` (row that does not deserialise → skipped);
    `src/storage/page.c:77-84` (`page_get_slot`: no `offset + length` bound), `:96-115` (`page_compact`:
    `write_pos -= entry->length` can wrap, then `memcpy` into `temp[4096]` on the stack), `:33-40,42-75`
    (`num_slots` trusted); `src/storage/buffer_pool.c:67-86` (all frames pinned → `bp_fetch` NULL).
  - **Why:** an `EIO`, a full pin table (1024 frames) or one bad page turns into a *successful* statement with
    fewer rows: wrong aggregates, partial `UPDATE` / `DELETE`, incomplete dumps and backups. A data file from an
    untrusted source can smash the stack through `page_compact`.
  - **Do:** `page_validate()` on every disk read (`num_slots <= SV_MAX_SLOTS`, each live slot inside `data[]`,
    `free_space` consistent; plus the checksum of P1.1) → `bp_fetch` fails with a typed error; give `HeapScan` an
    `error` field set on any failed fetch / chain / row and make the executor fail the statement with
    "table '%s' page %u is damaged or unreadable"; `CHECK TABLE` (`db_api.c:377-441`) reports the page ids.
  - **Done when:** flipping bytes in a table page makes `SELECT COUNT(*)` return an error, never a smaller number;
    a libFuzzer target over `page_validate` + `page_compact` + `tuple_deserialize` runs clean under ASan.
  - **Fixed:** `page_validate` (page id, slot count, every live slot inside `data[]`, plausible `free_space`)
    on every buffer-pool read — a damaged page is never used; `page_get_slot` bounds-checks, `page_compact` can
    no longer write outside its stack buffer. Heap scans record the first damage (unreadable / damaged page,
    broken overflow chain, undecodable row) in a thread-local (`heap_damage`); `executor_run` turns the
    statement into an error, `UPDATE` / `DELETE` stop before writing, an index build is discarded. Test:
    `tests/test_durability.c` scenario 6 (three kinds of corruption: `COUNT(*)`, `UPDATE`, `DELETE` all fail).
    **Not done:** `CHECK TABLE` listing the damaged page ids; the libFuzzer target (P3.3).

- [x] **P0.11 `[SECURITY]` PHP source and hidden files can be downloaded on Windows through path aliases** — [verify-win]
  - **Where:** `src/server/server.c:2083-2087` (hidden-file and `..` filter), `:2139-2144` (`.php` only when followed
    by end or `/`), `:2159-2223` (anything else is a static file).
  - **Why:** Win32 strips trailing dots and spaces and resolves 8.3 names, the URL checks do not:
    `GET /wp-config.php.` or `/wp-config.php%20` is not PHP for the router but opens `wp-config.php`;
    `/ENV~1` or `/HTACCE~1` contain no `/.` yet open `.env` / `.htaccess`.
  - **Do:** on `_WIN32`, reject path segments ending in `.` or space, or containing `~<digit>`, `<`, `>`, `|`, `"`,
    `*`, `?`; then compare `GetFinalPathNameByHandle` of the opened file with the requested path
    (case-insensitive) and refuse on mismatch. Add `.phtml`, `.phar`, `.php3-8`, `.inc` to the "never serve as
    text" list on all platforms.
  - **Done when:** a Windows CI job (see P3.2) requests each alias and gets 404 / 403, never the file body.
  - **Done (cross-compiled, not run on Windows):** `win_alias_segment` in `handle_web` refuses, on `_WIN32`,
    path segments ending in `.` or space, containing `~<digit>` or `< > | " * ?` (404). On every platform
    `.phtml`, `.phar`, `.php3`-`.php8`, `.phps`, `.inc` are never served as static files (403; test in
    `tests/test_web.c`). **Verified under wine:** `segvault-server.exe --static` answers 404 for
    `/wp-config.php.`, `/wp-config.php%20`, `/WP-CON~1.PHP`, `/ENV~1`, `/.env` and never returns the file body.
    **Not done:** the extra `GetFinalPathNameByHandle` comparison (it would also refuse legitimate junctions);
    a run on real Windows / a Windows CI job (P3.2).

- [x] **P0.12 `[DATA]` Windows: files larger than 2 GiB are addressed with 32-bit offsets** — [verify-win]
  - **Where:** `src/platform_compat.h:30` (`sv_lseek` → `_lseek(long)`), `:34` (`_chsize_s(fd, long)`), `:48-64`
    (`sv_pread` / `sv_pwrite`: seek + read + seek back, `off_t` is 32-bit on MinGW without `_FILE_OFFSET_BITS=64`);
    `src/storage/page.c:19,27`; `src/storage/buffer_pool.c:164`; `src/table/overflow.c:120`; `src/tx/wal.c:418-420`.
  - **Why:** past page 524 288 the offset overflows, the write fails and (P0.4) the failure is ignored. The
    seek/read/seek emulation is also not atomic if two threads ever share the descriptor.
  - **Do:** `int64_t` offsets everywhere; on Windows implement `sv_pread` / `sv_pwrite` with `ReadFile` /
    `WriteFile` + `OVERLAPPED` on `_get_osfhandle(fd)`, `sv_lseek` with `_lseeki64`, `sv_ftruncate` with
    `_chsize_s(fd, __int64)`; build with `-D_FILE_OFFSET_BITS=64`; add `_Static_assert(sizeof(off_t) == 8)`.
  - **Done when:** a sparse-file test writes and reads page 600 000 on Windows and Linux.
  - **Done (cross-compiled, not run on Windows):** `sv_lseek` returns `int64_t` (`_lseeki64`), `sv_ftruncate`
    takes `int64_t`, `sv_pread` / `sv_pwrite` use `ReadFile` / `WriteFile` with `OVERLAPPED` (64-bit offset,
    descriptor position untouched, no seek / read / seek); every engine offset is `int64_t` (`page.c`,
    `buffer_pool.c`, `overflow.c`, `wal.c`, savepoints); `-D_FILE_OFFSET_BITS=64` in `CFLAGS` and
    `_Static_assert(sizeof(off_t) == 8)` on POSIX. Linux test: page 600 000 written and read back
    (`tests/test_durability.c` scenario 8). New `make test-win` (`tests/test_win.c`, MinGW + wine): page
    600 000, file size past 2 GiB, descriptor position untouched by `sv_pread` / `sv_pwrite`, `MoveFileEx`
    replacement, engine with `ROLLBACK`, restart, users and binary values — 12/12 under wine. **Not done:** a
    run on real Windows.

---

## P1 — Core Engine Performance & ACID Architecture

- [ ] **P1.1 `[ENGINE]` WAL format v2: record checksum + LSN, page LSN + page checksum, compensation records, fuzzy checkpoints**
  - **Where:** `src/tx/wal.c:22-56` (23-byte header: no CRC, no LSN, no length trailer, no file header);
    `src/storage/page.h:25-32` (12-byte page header: no LSN, no checksum); `src/tx/wal.c:137-175` (redo idempotence
    relies on "slots are only appended"); `src/bridge/db_api.c:169-176,243-245` (checkpoint only when
    `g_open_tx == 0`, threshold 16 MB).
  - **Why:** a torn or stale log tail is indistinguishable from a valid record; a torn 4 KB page write cannot be
    detected or repaired; "append-only slots" is what forces every `UPDATE` to burn a slot (P1.5); one idle open
    transaction blocks checkpoints, so the log, recovery time and `ROLLBACK` cost grow without bound, and
    `VACUUM`, `BACKUP`, `OPTIMIZE` are refused (`db_api.c:453,523,2116`).
  - **Do:** file header (magic, format version, page size); record = `len | crc32c | lsn | prev_lsn_of_tx | type |
    tx | page | slot | payloads`; page header gains `lsn` (8 bytes) and `crc32c` (4 bytes) — bump the data-file
    format and migrate on open; redo applies a record only if `page.lsn < rec.lsn`; undo writes CLRs so recovery
    and rollback are restartable; full-page image for the first change of a page after a checkpoint (torn-page
    protection); fuzzy checkpoint = dirty-page table + active-transaction table, log truncated up to
    `min(oldest recLSN, oldest active tx first LSN)`; split the log into 16 MB segments so truncation is a file
    delete.
  - **Done when:** the P3.1 harness passes with crashes injected at every write of a 10-transaction interleaved
    workload; corrupting one byte anywhere in log or data file is reported, never applied; a transaction left open
    for an hour no longer stops the log from shrinking.

- [x] **P1.2 `[PERF]` `ROLLBACK` costs O(database), not O(transaction)** — [code]
  - **Where:** `src/bridge/db_api.c:1225-1245`, `:1278-1304`; `src/tx/wal.c:329-414` (reads the log from offset 0
    for a full rollback and loads the records into RAM); `src/storage/buffer_pool.c:241-244` (`bp_remove_all`);
    `src/query/executor.c:1316` (`heap_cache_clear`) and `executor_invalidate_indexes`.
  - **Why:** each rollback (also the internal one for a failed statement in `exec_atomic`, `db_api.c:260-282`, and
    for a deadlock victim, `:967-973`) does two `fsync`s, flushes every dirty page of every transaction, scans the
    whole log, empties the buffer pool and throws away every in-memory index and free-space map — the next
    statement on each table re-reads the whole table. Test suites that roll back after each test pay this per test.
  - **Do:** undo in memory through the buffer pool using the before-images the engine already keeps per
    transaction (`TxChange`, `db_api.c:596-642`): walk the transaction's changes backwards, apply
    `heap_delete` / `heap_insert` / index maintenance, log CLRs (P1.1). No flush, no log read, no invalidation.
  - **Done when:** `bench/bench.c` gains "1 000 × (BEGIN; INSERT 10 rows; ROLLBACK) on a 1 M-row database":
    time per rollback is independent of database size and no index rebuild is observed (`SHOW STATUS` counter).
  - **Fixed:** the undo reads the log from the transaction's `BEGIN` (`tx_start`, kept per session), not from
    offset 0; afterwards only the pages the undo rewrote leave the buffer pool and only the touched tables lose
    their in-memory indexes and free-space map (`executor_invalidate_table`); a transaction with DDL / `CALL`, or
    with an unrecorded change, still invalidates everything. The undo itself is still file-based with a page
    flush first (bounded by the 4 MB pool) and two `fsync`s — CLRs are logged (P0.5). `bench/bench.c`:
    1000 × (BEGIN; INSERT 10 rows; ROLLBACK) = 0.049 ms each on 22 k and on 220 k rows. Test in
    `tests/test_durability.c` (index of an untouched table survives 200 rollbacks; data and indexes right).

- [x] **P1.3 `[PERF]` Every `SAVEPOINT` does an `fsync`, and one is taken per statement** — [code]
  - **Where:** `src/bridge/db_api.c:1251-1276` (`wal_flush` at `:1268`, offset via `lseek` at `:1272`);
    `:260-282` (`exec_atomic` sets `__sv_statement` for every DML when the database has any trigger, for `CALL`,
    `INSERT IGNORE` / `REPLACE` / `ON DUPLICATE KEY UPDATE`, multi-table `UPDATE` / `DELETE`).
  - **Why:** inside a transaction, a loop of `INSERT … ON DUPLICATE KEY UPDATE` is capped at the device's `fsync`
    rate although nothing needs to be durable before `COMMIT`.
  - **Do:** a savepoint is `{ log position = file size + buffered bytes, change-list sequence }`; write the
    `LOG_SAVEPOINT` record into the buffer without flushing. With P1.2 the statement savepoint becomes a pure
    in-memory marker.
  - **Done when:** 10 000 upserts in one transaction issue exactly one `fsync` (counted by the harness shim).
  - **Fixed:** `SAVEPOINT` writes its record into the WAL buffer without `fsync`; its position is
    `wal_size()` (file + buffer). `ROLLBACK TO` flushes the log before reading it.

- [x] **P1.4 `[ENGINE]` Autocommit statements are never `fsync`ed; make durability an explicit, documented setting** — [reproduced]
  - **Where:** `src/bridge/db_api.c:1117,1163` (`wal_write_buffer` only); `src/query/executor.c` has no `wal_flush`;
    `src/bridge/db_api.c:1215` is the only commit-time `fsync`.
  - **Why:** 30 000 single-row autocommit inserts take 0.2 s — they survive a process crash, not a power loss,
    while explicit `COMMIT` is fully synchronous. WordPress-style applications never issue `COMMIT`.
  - **Do:** `SET [GLOBAL] synchronous = FULL | NORMAL | OFF` (and `--synchronous`): `FULL` syncs every commit
    including autocommit, `NORMAL` (default) syncs explicit commits and group-syncs autocommit every N ms,
    `OFF` leaves it to checkpoints. Implement group commit: committers enqueue their LSN, one leader `fsync`s,
    all waiters with `lsn <= flushed_lsn` return (the engine lock is released while waiting). Document the
    guarantee of each mode in the README.
  - **Done when:** `FULL` survives the power-loss simulation for autocommit; 16 threads of small committed
    transactions reach several times the single-thread rate.
  - **Fixed:** `SET [GLOBAL] synchronous = OFF | NORMAL | FULL` (also `@@synchronous`, `--synchronous` for the
    server, administrators only with `--users`). FULL: every autocommit write and every `COMMIT` is `fsync`ed before
    the answer; NORMAL (default): `COMMIT` at once, autocommit writes grouped — at most one `fsync` per 100 ms, and
    `db_sync_tick()` (called by the server's accept loop with a try-lock) syncs the last ones; OFF: no log `fsync`
    (checkpoints only). Guarantees documented in the README. Test: `tests/test_durability.c` counts `fsync`s per
    mode (0 / grouped / one per commit). **Not done:** leader / follower group commit with the engine lock
    released while waiting (needs P1.7's lock rework); the 16-thread throughput benchmark.

- [~] **P1.5 `[ENGINE]` Space is never reclaimed: dead slots, dropped tables and old overflow chains** — [reproduced]
  - **Where:** `src/storage/page.c:42-75` (`page_insert` always appends a slot; `SV_MAX_SLOTS` = 1021);
    `src/table/heap.c:381-387` (`heap_update` = delete + insert); `src/table/overflow.h:5-8` (chains immutable,
    never reused); `src/bridge/db_api.c:206-241` (`do_vacuum` frees only unreferenced overflow pages and scans the
    whole file); no page is freed on `DROP TABLE` / `TRUNCATE`.
  - **Why (measured):** 2 000 rows = 352 KB; ten full-table `UPDATE`s → 450 KB, unchanged by `VACUUM` + `OPTIMIZE`;
    20 × (`DROP TABLE`, `CREATE`, 500 inserts) → 2.09 MB (dropped pages are leaked for good); one 100 KB row
    updated 50 times → +5.12 MB until a manual `VACUUM`, which needs zero open transactions.
  - **Do:** with page LSNs (P1.1) reuse dead slots and update in place when the new row fits; add a persistent
    free-page list (a chain of free-list pages inside `segvault.db`, replacing `segvault.free`,
    `db_api.c:180-202`) fed by `DROP TABLE`, `TRUNCATE`, emptied heap pages and replaced overflow chains at commit;
    replace the linear free-space scan (`heap.c:215-220`) with size-class bins; background incremental vacuum
    driven by a dead-space counter; `VACUUM FULL` to compact and truncate the file.
  - **Done when:** the three measurements above return to within 10 % of the initial size without a manual
    `VACUUM`; `tests/test_overflow.c` asserts the file size after 1 000 large-row updates.
  - **Done:** `VACUUM` also frees heap pages no table chain reaches any more (dropped tables), not only unused
    overflow chains, and refuses to free anything if a chain is broken by a damaged page; a counter of abandoned
    pages (overflow chains of deleted / replaced large rows, pages of dropped tables) starts an automatic `VACUUM`
    above 4 MB when no transaction is open. Tests (`tests/test_overflow.c`): 200 updates of a 100 KB row grow the
    file by < 8 MB (20 MB before), 20 × (CREATE, 150 rows, DROP) by < 6 MB. **Still open (needs P1.1's page LSN
    for redo idempotence):** reusing dead slots / updating in place, a persistent free-page list, size-class
    bins, `VACUUM FULL`.

- [ ] **P1.6 `[ENGINE]` Indexes exist only in memory; the on-disk B+tree is dead code**
  - **Where:** `src/index/btree.c` (319 lines, `uint64_t` keys, unique only, no delete rebalancing, not WAL-logged)
    is referenced only by `tests/test_btree.c`; real indexes are `MemIndex` in `src/query/executor.c:1303-1560`,
    built by scanning the table on first use and dropped by every rollback / `REPAIR` (`db_api.c:1242,1302,475`).
  - **Why:** first query after start (and after each rollback) on every indexed table is a full scan; index memory
    grows with the data, outside the 4 MB buffer pool; uniqueness checks during bulk loads depend on that RAM.
  - **Do:** a page-based B+tree with variable-length, order-preserving encoded keys (composite, collation-aware
    for `_ci`), duplicates via `(key, RID)` suffix, leaf sibling links, WAL-logged splits/merges (P1.1), root page
    id in `Index.root_page_id` (already in the catalog, `schema.h:60`). Keep the hash index as an optional
    in-memory accelerator. Either delete `src/index/btree.c` or evolve it into this.
  - **Done when:** cold start on a 1 GB database answers a primary-key lookup with ≤ 4 page reads; `EXPLAIN` shows
    the index; crash tests cover a split in progress.

- [ ] **P1.7 `[CONCURRENCY]` Table locks for plain reads cause upgrade deadlocks; waits are polled; writers can starve** — [code]
  - **Where:** `src/bridge/db_api.c:841-878` (every `SELECT` takes `LK_S` on its tables, kept to `COMMIT`,
    `:976-978`); `:916-962` (`acquire_locks`: sleep 3 ms, drop and retake the engine lock);
    `src/bridge/locks.c:154-177` (no queue: whoever polls first wins), `:25-29,168-169` (`LkWait.req[32]` truncates
    the wait set used for deadlock detection), `:141-152` (DFS without a visited set);
    `src/server/server.c:824-836,3041-3046` (20 ms polling to enter a session), `:599-605` (`exec_mx`: one
    non-read-only request at a time).
  - **Why:** two sessions doing `BEGIN; SELECT … FROM t; UPDATE t …` always end with error 1213 for one of them;
    a steady stream of readers can keep a writer waiting until the 30 s timeout; N waiters wake 333 times per
    second each and take the global lock to re-check.
  - **Do:** plain `SELECT` inside a transaction reads a snapshot and takes no table lock (only `FOR UPDATE` /
    `FOR SHARE` lock) — needs a per-transaction snapshot instead of today's per-statement one
    (`try_snapshot`, `db_api.c:896-914`); FIFO wait queues with a condition variable per lock entry; deadlock
    detection over the full request set with a visited bitmap; replace both polling loops with the same queue.
    Longer term: row-level write locks.
  - **Done when:** the two-session read-then-write test commits both; a writer behind 8 overlapping readers
    acquires its lock in bounded time; idle waiters consume no CPU.

- [x] **P1.8 `[PERF]` Uncommitted-change tracking is quadratic and heavy** — [code]
  - **Where:** `src/bridge/db_api.c:596-642` (`TxChange` = 184 bytes per changed row, with database and table
    names copied as strings; one global array for all transactions), `:635-642` (`chg_drop` scans everything),
    `:666-736` (`snapshot_build` searches its working set linearly for every change).
  - **Why:** a 100 000-row transaction makes each concurrent `SELECT` on that table spend ~5·10⁹ comparisons
    building the snapshot; a 10 M-row import in one transaction needs ~1.8 GB just for this list.
  - **Do:** per-transaction lists keyed by table id (catalog index + root page), 24-byte entries, before-images in
    an arena; build the snapshot with a hash map `RID → entry`; cache the built snapshot per (transaction, table,
    change sequence).
  - **Done when:** snapshot build for 1 M changes takes < 200 ms and bookkeeping stays under 40 bytes per insert.
  - **Fixed:** `(database, table)` interned into a 16-bit id (`ctab_id`, reset when nothing is pending), so a
    `TxChange` is 56 bytes instead of 184 (40 would need packing RIDs; not worth it); `snapshot_build` uses an
    open-addressing hash `RID → entry` (linear instead of quadratic); the per-rollback table scan uses a bitmap.
    Test (`tests/test_durability.c`): a read of the committed version with 150 000 pending changes takes ~12 ms.
    Found and fixed on the way: rolling back a large transaction with `UPDATE`s could fail with "cannot put a
    row back" — every slot appended and then undone kept its 4 directory bytes; the undo (and the CLR redo) now
    pop the trailing slot it deletes (`page_pop_slot`).

- [ ] **P1.9 `[ENGINE]` Exact numerics: `DECIMAL` is computed as `double`, `BIGINT UNSIGNED` stops at 2⁶³−1** — [reproduced]
  - **Where:** `src/catalog/schema.c:9-19` (`DECIMAL` stored as text), `src/table/tuple.c:200-216`; arithmetic and
    aggregates in `src/query/executor.c`.
  - **Why:** `SUM` over `DECIMAL(20,2)` prints `1.23456789012346e+16`; `SUM(0.10, 0.20) = 0.30` is false;
    `18446744073709551615` is rejected for `BIGINT UNSIGNED`.
  - **Do:** fixed-point decimal type (sign + up to 65 digits, base 10⁹ limbs) for literals, columns, `+ - * / %`,
    `SUM` / `AVG` / `MIN` / `MAX`, comparisons and casts, with MySQL's scale rules; `uint64` path for unsigned
    columns; never print a `DECIMAL` in exponent form.
  - **Done when:** the three cases above match MySQL 8 output; a property test compares 10 000 random decimal
    expressions against Python's `decimal`.

- [~] **P1.10 `[PERF]` Buffer pool: fixed 4 MB, one latch held across disk I/O, three syscalls per new page**
  - **Where:** `include/common.h:41` (`SV_BUFFER_POOL_CAP 1024`), `src/bridge/db_api.c:128`;
    `src/storage/buffer_pool.c:258-269` (exclusive latch for every `bp_fetch` / `bp_unpin`), `:100-121` (disk read
    inside it), `:149-191` (`lseek` + `pwrite` per allocation), `src/table/heap.c:249-286` (two more page flushes
    per new heap page), `src/storage/buffer_pool.c:204-208` (`g_sort_bp` global comparator context).
  - **Why:** parallel readers serialise on every page access and stall behind one thread's disk read; any
    working set above 4 MB thrashes; bulk loads pay ~4 syscalls per page.
  - **Do:** `--cache-size` / `SET GLOBAL cache_size` (default: 1/8 of RAM, capped); bucket-level latches with a
    per-frame "loading" state so I/O happens outside the table latch; pin counts as atomics; extend the file in
    64-page steps (`posix_fallocate` / `SetEndOfFile`) and stop flushing chain links eagerly once allocation is
    logged (P1.1); sort dirty frames with an explicit context.
  - **Done when:** 8 reader threads on a cold 1 GB table scale ≥ 5× over one thread; bulk insert throughput at
    least doubles in `bench/bench.c`.
  - **Done:** the page cache is 1/8 of the RAM (4-512 MB) instead of a fixed 4 MB; `--cache-size MB`,
    `db_set_cache_size()`, `SET GLOBAL cache_size = MB` at run time (`bp_resize`, administrators only, not inside
    a transaction), `@@cache_size`; new heap pages and chain links are no longer flushed eagerly (two page writes
    per new page — `LOG_LINK` lets recovery recreate them). Test in `tests/test_durability.c`.
    **Still open:** bucket latches with I/O outside the latch, atomic pin counts, 64-page file extension, the
    8-reader scaling benchmark (bulk insert did not change on tmpfs: 122 ms for 110 k rows).

- [ ] **P1.11 `[ENGINE]` Catalog: 1.71 GB fixed struct, raw struct dumps on disk, full rewrite + 2 `fsync`s per DDL**
  - **Where:** `src/catalog/schema.h:88-148` (`sizeof(Catalog)` = 1 834 562 576 bytes, `Database` = 27.3 MB,
    `TableSchema` = 62.7 KB; limits 64 databases / 256 tables / 64 columns / 16 indexes / 16 constraints /
    64 views, routines, triggers; 64 KB per body); `src/catalog/schema.c:311-324` (`catalog_drop_db` `memmove`s
    27 MB per following database); `:257-286` (format = `write` of the C structs);
    `src/bridge/db_api.c:562-573` (every DDL: `bp_flush_all` + whole-catalog save).
  - **Why:** applications with more than 256 tables or 64 columns cannot be imported; the on-disk format depends
    on compiler padding and needs a legacy struct copy per version (v1…v5 today); importing a 200-table dump
    rewrites megabytes hundreds of times; on Windows the 1.7 GB allocation is charged to the commit limit.
  - **Do:** heap-allocated, growable catalog objects with hash lookup by name; store the catalog in system tables
    inside the page file (rows protected by the WAL, so DDL becomes transactional and costs one log record);
    explicit little-endian field encoding with per-record version tags; raise or remove the limits.
  - **Done when:** `CREATE TABLE` with 300 columns and a database with 2 000 tables work; 1 000 `CREATE TABLE`
    statements run in under a second; process RSS at start is below 20 MB.

- [x] **P1.12 `[SECURITY]` Overflow stubs are recognised by their content** — [code]
  - **Where:** `src/table/overflow.c:7,25-33` (`ov_is_stub`: any 24-byte slot starting with `FF 00 "SVOVF1"` and
    a valid FNV-1a), used by `src/table/heap.c:343,448,506`.
  - **Why:** a row whose serialised form is those 24 bytes (first eight columns `NULL`, the rest chosen) is read
    back as a pointer to an arbitrary overflow chain — including chains of tables the user cannot read.
  - **Do:** mark stubs out of band: bit 15 of `SlotEntry.length` (inline rows are ≤ 2 048 bytes, `overflow.h:20`);
    keep reading old files by accepting the magic only when the data-file format version is < the new one.
  - **Done when:** inserting the forged row returns it verbatim; a format-upgrade test opens a v1.2 file.
  - **Fixed (without a format change):** a row whose serialised form has the exact shape of a stub is always
    written through an overflow chain (`heap_insert`, `ov_slot_put`, `ov_slot_revive`), so a slot with the stub
    shape is always a real stub; v1.2 files open unchanged. Test in `tests/test_overflow.c` (fails without the fix).

- [ ] **P1.13 `[ENGINE]` Optimizer estimates are fixed constants; there are no column statistics** — [code]
  - **Where:** `src/query/executor.c:7869-7887` (`cond_selectivity`: `=` 0.1, range 1/3, `LIKE` 0.1, anything else
    0.5), `:8000` (non-unique index lookup = `num_rows * 0.1`), `:8024-8034` (join cardinality from those numbers);
    `src/catalog/schema.h:96` (`num_rows` "approximate", refreshed only by `ANALYZE` / `OPTIMIZE`,
    `src/bridge/db_api.c:469-474`). A hash join exists for equality conditions (`executor.c:4645,4756,4956`) and
    `EXPLAIN [ANALYZE]` already prints estimated vs. scanned rows.
  - **Why:** with constant selectivities the planner cannot prefer the selective predicate, the better of two
    indexes, or the smaller build side; estimates in `EXPLAIN` are uninformative on skewed data.
  - **Do:** `ANALYZE` collects per-column distinct count, null fraction, min / max and an equi-depth histogram
    (sampled), stored in the catalog (P1.11); `num_rows` maintained exactly at commit; selectivity from
    statistics; choose access path and join order by cost (first verify with `EXPLAIN` on a 3-table query
    whether today's order simply follows the `FROM` clause, and record it in a test); expose
    `EXPLAIN FORMAT=JSON` for the GUI plan view.
  - **Done when:** on a skewed table the plan switches between index and scan at the right threshold, a 3-table
    join picks the same order regardless of how the query is written, and `EXPLAIN ANALYZE` estimates stay
    within 2× of actual rows on the benchmark schema.

---

## P2 — Web Server, Protocol & Custom GUI Excellence

- [x] **P2.1 `[NET]` MySQL protocol: every `SET …` is acknowledged and dropped** — [code]
  - **Where:** `src/server/mysql_wire.c:355-363` (`mw_client_noise`: `SET` → OK unless `SET PASSWORD`), called
    before the engine in `src/server/server.c:3117`.
  - **Why:** `SET @v = …`, `SET FOREIGN_KEY_CHECKS = 0` (first line of every `mysqldump`), `SET sql_mode`,
    `SET time_zone`, `SET TRANSACTION ISOLATION LEVEL` never reach `NODE_SET`
    (`db_api.c:506-508`), which already implements the first two.
  - **Do:** only swallow an explicit allow-list (`NAMES`, `CHARACTER SET`, `character_set_*`, `collation_*`,
    `sql_mode`, `time_zone`, `net_*`, `SQL_*`), forward everything else; answer unknown system variables with a
    warning count instead of silence.
  - **Done when:** `tests/test_php.c` (real PDO and mysqli) reads back a user variable and loads a dump whose
    tables are created in foreign-key-violating order.
  - **Fixed:** `mw_client_noise` forwards to the engine every `SET` that names a user variable (`@x`, not
    `@@`) or `FOREIGN_KEY_CHECKS`; the rest stays an OK without effect. The engine now evaluates the assignments
    of a `SET` before applying `FOREIGN_KEY_CHECKS`, so mysqldump's
    `SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0` saves 1 and the closing
    `SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS` turns checks back on (before: they stayed off for the session).
    Tests in `tests/test_server.c` (MySQL protocol). **Not done:** warning count for unknown system variables.

- [~] **P2.2 `[NET]` Server-side prepared statements re-parse on every execute and run reads at `PREPARE` time** — [code]
  - **Where:** `src/server/server.c:3218-3253` (`my_prepare` executes read-only statements with `NULL` parameters
    to learn the columns), `:3255-3266`; `src/server/mysql_wire.c:728-773` (`mw_bind_execute` splices literals into
    the text); same model in `src/server/server.c:457-517` (`bind_params`) and `src/bridge/db_api.c:1030-1067`.
  - **Why:** a parameterless or weakly filtered `SELECT` runs twice; statements with side effects (`NEXTVAL`,
    user functions, `FOR UPDATE`, `SELECT … INTO @v`) take effect at prepare time; no plan is ever reused.
  - **Do:** parse once into an AST with parameter nodes (`EXPR_PARAM`), bind typed values at execute; add a
    describe-only path that resolves output columns from the plan without running it; cache (text → plan) per
    session, invalidated by a catalog version counter.
  - **Done when:** a prepared `SELECT COUNT(*) FROM big` executes once (engine counter); `SELECT NEXTVAL(s)`
    prepared and executed once advances the sequence by one; prepared point lookups get ≥ 2× throughput.
  - **Done:** `COM_STMT_PREPARE` no longer runs the `SELECT`: `db_describe()` resolves the columns in a
    describe-only executor mode (tables look empty, no row for `FROM`-less selects or table functions, `NEXTVAL` /
    `SETVAL` / user functions inert, no locks). **Still open:** parse once with parameter nodes, plan cache.

- [~] **P2.3 `[NET]` HTTP request limits truncate silently** — [code]
  - **Where:** `src/server/server.c:165-202` (`Request.query[2048]`, `auth[512]`, `content_type[128]`,
    `path[2048]`), `:2513` (`%4095s` target), `:2519` (`snprintf` into `query`), `:1583` (`QUERY_STRING` for PHP),
    `:2527-2532` (no check on header-name syntax; `Name : value` is ignored here but forwarded by the proxy,
    `:2726-2743`).
  - **Why:** a query string over 2 047 bytes reaches PHP cut short (WordPress `load-scripts.php`, OAuth
    callbacks, signed URLs); a bearer token over 511 bytes never matches; a URL over 4 095 bytes answers 400.
  - **Do:** keep target, query and header values as pointer + length into the request buffer (no fixed copies);
    answer 414 / 431 with explicit limits (8 KB request line, 64 KB headers, configurable); reject header names
    that are not RFC 9110 tokens with 400.
  - **Done when:** `tests/test_web.c` sends a 6 KB query string and PHP echoes `strlen($_SERVER['QUERY_STRING'])`
    correctly; oversize inputs produce 414 / 431; `Transfer-Encoding : chunked` is rejected.
  - **Done:** nothing is truncated any more: request target up to 8 KB (`uri` / `query` 8 KB, `path` 4 KB),
    longer → 414; `Authorization` (now 4 KB), `Content-Type`, `Host`, `Origin`, `Range`, session id that do not
    fit → 431; a header line without `:` or whose name is not an RFC 9110 token → 400. Tests in
    `tests/test_php.c` (6 KB query string reaches PHP intact, 9 KB → 414, `Transfer-Encoding : chunked` → 400).
    **Still open:** pointer + length into the request buffer instead of fixed copies; configurable limits.

- [~] **P2.4 `[NET]` No overall request deadline; bodies are buffered whole, allocated before they arrive** — [code]
  - **Where:** `src/server/server.c:2469-2479` (per-`recv` timeout only, `IDLE_TIMEOUT_MS` 15 s), `:2581-2604`
    (`fill` grows the buffer to `Content-Length` up front), `:2860` (whole body before routing), `:3829`
    (64 connection slots); `src/server/fastcgi.c:715-717` (zero-config PHP sets `post_max_size=256M` while
    `max_body` defaults to 16 MB, `server.c:3828`).
  - **Why:** 64 clients sending one header byte every 14 s occupy every slot; 64 requests declaring 16 MB reserve
    2 GB without sending a byte; uploads between 16 and 256 MB fail with 413 although PHP would accept them.
  - **Do:** absolute deadlines for header (10 s) and body (minimum rate); grow the body buffer as data arrives;
    stream request bodies to FastCGI / proxy instead of buffering; per-address connection cap; one setting for
    the upload limit that also sets PHP's `post_max_size` / `upload_max_filesize`.
  - **Done when:** a slow-header test is disconnected at the deadline; RSS stays flat while 64 idle connections
    declare large bodies; a 100 MB upload to PHP succeeds with the default configuration.
  - **Done:** request headers must be complete within 10 s of their first byte (`HEADER_DEADLINE_MS`, 408 and
    close; test in `tests/test_web.c`); `fill` grows the buffer with the data received (at most doubling), no
    longer to the declared `Content-Length` up front. **Still open:** minimum body rate, streaming bodies to
    FastCGI / proxy, per-address connection cap, one upload-limit setting shared with PHP.

- [ ] **P2.5 `[NET]` Connection architecture: one thread per connection, TLS and HTTP/2 through loopback sockets**
  - **Where:** `src/server/server.c:3445-3454` (detached thread per connection), `:3974-4053` (accept loop with
    `select` and a 200 ms tick), `:2907-2927` (TLS bridge preface `SVTLS <secret> ip port`);
    `src/server/h2.c:416-429,442-513` (every HTTP/2 stream opens a new loopback HTTP/1.1 connection),
    `:903` (`FD_SET` on stream sockets).
  - **Why:** an HTTPS request costs two threads and two sockets, an HTTP/2 page load up to 64 extra loopback
    connections and threads; 256 slots are exhausted by a handful of browsers; `FD_SET` with a descriptor ≥ 1024
    is undefined behaviour.
  - **Do:** make `route()` callable on an in-memory `Request` so TLS and HTTP/2 dispatch in-process; move socket
    I/O to `poll` (then `epoll` / IOCP) with a fixed worker pool; keep thread-per-request only for blocking
    engine and PHP calls.
  - **Done when:** `h2load -n 20000 -c 50 -m 32` against a static file completes without 503s and the process
    holds < 100 threads.

- [~] **P2.6 `[SECURITY]` Credentials: slow hash under the engine lock, fast unsalted copy on disk, world-readable files**
  - **Where:** `src/bridge/db_api.c:1886-1891` (`db_login` holds the exclusive engine lock while
    `password_hash` runs 20 000 SHA-256 rounds, `src/bridge/auth.c:122-140`); `auth.c:552-555` (also stores
    `SHA1(SHA1(password))` for `mysql_native_password`); `auth.c:175-194` (`fopen "w"` → 0644);
    `src/server/main.c:307` (data directory 0755); `src/platform_compat.h:82` + `db_api.c:106` (data file 0644);
    `src/server/server.c:2285-2323` (60 s login cache not cleared by `ALTER USER`); `auth.c:156` (salt falls back
    to a clock-seeded generator).
  - **Why:** each wrong password stalls the whole database for milliseconds — a login flood is a denial of
    service; the unsalted double SHA-1 makes the salted hash pointless for anyone who can read `users.dat`,
    which every local user can.
  - **Do:** copy salt and hash under the lock, hash outside it; PBKDF2-HMAC-SHA256 with a stored iteration
    count (migrate on next successful login); `caching_sha2_password` for the MySQL protocol and make the
    native hash opt-in per user; create the data directory 0700 and every file 0600 (owner-only ACL on
    Windows); clear the login cache on any user change; fail instead of using the fallback salt; exponential
    back-off per account and address.
  - **Done when:** 32 threads of failing logins leave `SELECT 1` latency unchanged; a fresh data directory has no
    group / other permission bits; a changed password is refused immediately.
  - **Done:** `db_login` copies salt + hash under the engine lock and hashes outside it (then re-checks the
    user under the lock); `random_salt` fails instead of using a clock-seeded value (`CREATE USER` errors);
    the HTTP login cache is keyed by `db_users_version()` so any user / grant change drops it (test: old password
    refused right after `ALTER USER`); data file, WAL, catalog, `users.dat`, free list, sequences and the audit log
    are created `0600` (existing data file `fchmod`ed), the data directory and backup folders `0700` (test:
    `tests/test_durability.c`). **Still open:** PBKDF2 with stored iterations, `caching_sha2_password` and an
    opt-in native hash, per-account back-off, owner-only ACL on Windows.

- [~] **P2.7 `[SECURITY]` Secrets and control characters reach the logs**
  - **Where:** `src/server/server.c:725-755` (`audit_stmt` writes the statement with bound values, e.g.
    `CREATE USER … IDENTIFIED BY '…'`), `:3126-3133` + `:2642-2646` (MySQL statements to the ring log and stderr),
    `:2887-2889` (decoded request path printed raw); `:729-733` (anything starting with `SELECT` / `WITH` is
    not audited, including `SELECT f()` with side effects and `SELECT … INTO @v`).
  - **Do:** one redaction pass shared by audit, ring log and stderr (`IDENTIFIED BY '***'`, `PASSWORD('***')`);
    escape bytes < 0x20 and 0x7f as `\xNN` in every log line; audit by statement kind from the parser, not by
    the first keyword; open the audit file 0600 and `fsync` it on a timer.
  - **Done when:** a test greps the audit file and stderr for a known password and a `%0a%1b[2J` path and finds
    neither.
  - **Done:** `one_line` (used for every log line: stderr, request ring log, audit) escapes bytes < 0x20 and 0x7f
    as `\xNN` and replaces the string after `IDENTIFIED … BY` / `PASSWORD` with `'***'`; request paths and proxy
    host / URI go through it on stderr; the audit file is created 0600. Test in `tests/test_server.c`.
    **Still open:** audit by statement kind from the parser, periodic `fsync` of the audit file.

- [~] **P2.8 `[NET]` MySQL protocol completeness**
  - **Where:** `src/server/mysql_wire.c:50-57` (`mw_end`: payloads over 16 MB − 1 are not split, the length is
    clamped → the stream desynchronises for a ≥ 16 MB value); `src/server/server.c:3410` (`COM_CHANGE_USER`,
    `COM_SET_OPTION`, `COM_STMT_FETCH`, `COM_STATISTICS`, `COM_PROCESS_KILL` → "Unknown command"); `:3292`
    (8 h socket timeout and no idle-in-transaction limit, while HTTP sessions have `TX_IDLE_MS`, `:82,651-662`);
    `src/server/mysql_wire.c:128-144` (no `NOT_NULL` / `PRI_KEY` / `AUTO_INCREMENT` / `UNSIGNED` column flags),
    `:265-279` (static variables: `have_ssl DISABLED` even with TLS on), `:304-310` (exponential `LIKE` matcher);
    `src/server/server.c:3064-3149` (result sets are fully materialised, then fully encoded).
  - **Do:** split packets at 0xFFFFFF; implement the five commands (`KILL` needs P2.9's cancel flag); idle
    transaction timeout that rolls back and closes; real column flags from the catalog; variables that reflect
    the running configuration; iterative `LIKE`; send rows as they are produced (needs a cursor API in
    `db_api.h` instead of `QueryResult` with every cell as a string, `db_api.h:66-93`).
  - **Done when:** a 20 MB `LONGBLOB` round-trips through PDO; JDBC with `useCursorFetch=true` and a pooled
    `changeUser` connection work; `SELECT *` on a 2 GB table streams with flat memory.
  - **Done:** `mw_end` splits payloads of 16 MB − 1 or more into 0xFFFFFF-byte packets plus the remainder (an
    empty one when the size is an exact multiple) with consecutive sequence numbers (round-trip test up to 32 MB
    in `tests/test_server.c`); the `LIKE` matcher of `SHOW VARIABLES` / `SHOW STATUS` is iterative (no
    exponential backtracking); `COM_STATISTICS`, `COM_SET_OPTION`, `COM_CHANGE_USER` (re-authentication with the
    connection's scramble, the session restarts, wrong password closes the connection like MySQL) and
    `COM_PROCESS_KILL` (P2.9) are implemented; a connection that stays silent for 10 minutes with a transaction
    open is closed and the transaction rolled back; `have_ssl` / `have_openssl` / `max_connections` reflect the
    configuration. Tests in `tests/test_server.c`. **Still open:** `COM_STMT_FETCH` (cursors), column flags
    from the catalog, streaming result sets (needs a cursor API instead of `QueryResult`).

- [x] **P2.9 `[NET]` Only `SELECT` scans can be interrupted**
  - **Where:** `src/server/server.c:3991-3998` (watchdog → `db_request_cancel`), `src/bridge/db_api.c:150-163`,
    `src/table/heap.c:402-415` (`g_sv_scan_cancel` is consulted only during read-only queries).
  - **Why:** a runaway `UPDATE`, `ALTER TABLE` or `INSERT … SELECT` holds `exec_mx` and the engine lock until it
    ends; `--timeout` does not apply; there is no `KILL`.
  - **Do:** per-session cancel flag checked in the write loops at row boundaries; a cancelled write statement
    rolls back to its statement savepoint (cheap after P1.2); `SHOW PROCESSLIST` with real entries and
    `KILL [QUERY] id`.
  - **Done when:** an `UPDATE` over 10 M rows stops within 100 ms of `KILL QUERY` and leaves no partial change.
  - **Fixed:** every MySQL connection has a cancel flag the engine's scans check (`db_set_thread_cancel`), and
    every statement (not only `SELECT`) is marked as running for `--timeout` / `db_request_cancel`; a cancelled scan
    fails the statement instead of returning fewer rows, so `UPDATE` / `DELETE` / `INSERT … SELECT` stop in their
    read phase before writing anything. `SHOW [FULL] PROCESSLIST`, `KILL [QUERY | CONNECTION] id` and
    `COM_PROCESS_KILL` are answered without entering the engine (the target may hold it); administrators can kill
    any connection, other users their own. Tests in `tests/test_server.c`: a long `SELECT` and a long `DELETE`
    are listed and killed from another connection; the `DELETE` changes nothing. **Limit:** the write phase itself
    (applying already-selected rows) is not interrupted.

- [~] **P2.10 `[SECURITY]` Child processes: PHP FastCGI port open to every local user; handles leak into children**
  - **Where:** `src/server/fastcgi.c:193-205,241-290` (`php-cgi -b 127.0.0.1:<free port>`; the port is chosen,
    closed, then given to the child); `src/server/cgi.c:288` (`CreateProcessA(..., bInheritHandles = TRUE)`);
    `src/platform/xlib.c:852-891` (`fork` + `xdg-open` without closing descriptors; no `O_CLOEXEC` anywhere in
    the tree); `src/server/fastcgi.c:657` (`php/php-cgi` relative to the current directory).
  - **Why:** any local account can connect to the FastCGI port and run any PHP file as the SegVault user;
    a browser started by "open in browser" inherits the data-file lock and the listening sockets, so the next
    start reports "data directory already in use" / "port in use" until that browser exits; CGI programs on
    Windows receive the database handle.
  - **Do:** create the listening socket in SegVault and pass it to `php-cgi` as descriptor 0 (FastCGI's own
    convention) — a Unix socket in a 0700 directory on POSIX; open every file and socket with `O_CLOEXEC` /
    `SOCK_CLOEXEC` / `WSA_FLAG_NO_HANDLE_INHERIT` and `_O_NOINHERIT`; whitelist the three pipe handles with
    `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`; drop the relative `php/` lookup.
  - **Done when:** `ls -l /proc/<child>/fd` of php-cgi, a CGI script and the browser helper show only 0-2 (and the
    FastCGI socket); no TCP listener exists for PHP.
  - **Done:** on POSIX the php-cgi workers listen on Unix sockets (`php-cgi -b PATH`) inside a private
    `mkdtemp` folder (0700), removed with the pool — no local TCP port any more (test in `tests/test_php.c`, also
    with the real `/usr/bin/php-cgi`); the GUI's `xdg-open` and file-chooser children close every descriptor
    above 2 before `exec` (CGI and FastCGI children already did); on Windows engine files are opened with
    `_O_NOINHERIT`. **Still open:** `O_CLOEXEC` / `SOCK_CLOEXEC` on every descriptor, the Windows
    `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` whitelist for CGI, the relative `php/php-cgi` lookup.

- [x] **P2.11 `[NET]` HTTP/2 input hardening**
  - **Where:** `src/server/h2.c:442-513` (`open_stream`: values are checked for CR / LF by `valid_field`
    `:436-439`, header *names* are copied into the HTTP/1.1 request unchecked, `:483`), `:772-776` (`RST_STREAM`
    frees the stream with no rate limit), `:780-783,703-720` (`PING` / `SETTINGS` answered without limit).
  - **Do:** reject names that are not lowercase tokens, pseudo-headers after regular ones, duplicated or unknown
    pseudo-headers, `connection`-specific fields and `te` other than `trailers` (RFC 9113 §8.2-8.3); count resets
    and control frames per connection and send `GOAWAY(ENHANCE_YOUR_CALM)` above a budget.
  - **Done when:** `h2spec` passes the generic and HPACK sections; a rapid-reset loop is disconnected.
  - **Fixed:** `open_stream` accepts only `:method` / `:path` / `:authority` / `:scheme`, once each and before
    regular fields; names must be lowercase RFC 9110 tokens (the HPACK decoder no longer lower-cases them, and an
    over-long name is invalid instead of truncated); `connection`, `keep-alive`, `proxy-connection`,
    `transfer-encoding`, `upgrade` and `te` ≠ `trailers` are refused (RST_STREAM PROTOCOL_ERROR). More than 200
    RST_STREAM / PING / SETTINGS per second → `GOAWAY(ENHANCE_YOUR_CALM)`. New `tests/test_h2.c` drives
    `h2_serve` over a socketpair with hand-written frames (9 checks). **Not done:** an `h2spec` run.

- [~] **P2.12 `[API]` HTTP API: unbounded results, 32-bit ids, no machine-readable metrics**
  - **Where:** `src/server/server.c:554-583` (`result_json` builds the whole response in memory),
    `:580` and `src/bridge/db_api.h:84-85` (`int rows_affected`, `int last_insert_id`), `:2406-2414` (`/status`
    is the only runtime information); `src/bridge/db_api.c:1850-1874` (`db_engine_stats` is not exposed).
  - **Do:** `max_rows` + cursor pagination (`next` token) for `/query`; 64-bit ids and counts end to end;
    option to return integers above 2⁵³ as strings; `GET /api/v1/metrics` in Prometheus text format (requests
    by service and status, latency histogram, buffer-pool hits / misses / evictions, WAL bytes, checkpoints,
    lock waits, deadlocks, open transactions, connections) and the same numbers in `SHOW STATUS`.
  - **Done when:** an `AUTO_INCREMENT` value of 3 000 000 000 is returned intact; `/metrics` parses with
    `promtool check metrics`.
  - **Done:** `QueryResult.rows_affected` / `last_insert_id` are `long long` end to end (engine, HTTP JSON,
    MySQL OK packet, audit, GUI); the `INT32_MAX` caps are gone. Test: `AUTO_INCREMENT = 3000000000` returns
    `"last_insert_id":3000000000` (`tests/test_server.c`). **Still open:** `max_rows` + cursor pagination,
    integers above 2⁵³ as strings, `/metrics`.

- [x] **P2.13 `[NET]` Listeners: IPv4 only, no Unix socket for MySQL**
  - **Where:** `src/server/server.c:3458-3477,3925-3951` (`AF_INET`, `inet_addr`).
  - **Why:** `localhost` resolves to `::1` first for many clients; PHP's `mysqli_connect('localhost')` uses a
    Unix socket on Linux and macOS, so the hostname every tutorial uses fails here.
  - **Do:** dual-stack listeners (`getaddrinfo`, `IPV6_V6ONLY` off or two sockets); a MySQL Unix socket in the
    data directory, exported to php-cgi through `mysqli.default_socket` / `pdo_mysql.default_socket`.
  - **Done when:** `curl -6 http://[::1]:8484/api/v1/health` and `new mysqli('localhost', …)` both work in the tests.
  - **Fixed:** the HTTP port is also served on IPv6 (`[::1]` when bound to 127.0.0.1, `[::]` for 0.0.0.0;
    `IPV6_V6ONLY`, skipped if the system has no IPv6), clients shown as `::1` count as local; with `--mysql-port`
    the MySQL protocol also listens on `DATA_DIR/mysql.sock` (owner-only, `--mysql-socket PATH` to change it) and
    php-cgi gets `mysqli.default_socket` / `pdo_mysql.default_socket`. Tests in `tests/test_server.c`: HTTP over
    `[::1]`, MySQL handshake over the socket. **Not verified:** a real `new mysqli('localhost')` (the PHP here
    has no mysqli extension); IPv6 for the MySQL and HTTPS ports.

- [ ] **P2.14 `[GUI]` Engine calls on the UI thread** — [code]
  - **Where:** `src/widgets/window.c:855,867,984,1828-1832,1867-1869,3087,3191,3337,3515,3646,4274` (direct
    `db_execute` from event handlers: table browsing, foreign-key navigation, grid edits, import steps). Only the
    editor's Run goes through a worker (`run_worker`, `:5580-5593`, started at `:5796`, cancellable at `:5687`).
  - **Why:** each of these waits for the engine's exclusive lock (`src/bridge/db_api.c:57`). With the server
    running in the same process ("editor and server together") a long PHP-driven statement, a lock wait
    (up to 30 s, `db_api.c:751`) or a large table freezes the window.
  - **Do:** one job queue for every engine call (the existing worker generalised), results delivered to the UI
    thread as events; a visible "waiting for the database" state with Cancel; never block in an event handler.
  - **Done when:** with a session holding a write lock on a table, opening that table in the explorer keeps the
    window repainting and can be cancelled.

- [ ] **P2.15 `[GUI]` Text input and display scaling** — [code]
  - **Where:** `src/platform/xlib.c:538-548` (`XLookupString`: no input method, no compose or dead keys, Latin-1
    bytes for non-ASCII keys); `src/platform/win32.c` (no DPI-awareness call or manifest; text is drawn at fixed
    pixel sizes, `:481-492`); `src/widgets/settings.h:34-36` (font sizes 11-16 are the only scaling control).
  - **Why:** accented and non-Latin text cannot be typed reliably on Linux (and arrives as non-UTF-8 bytes in
    the SQL buffer); on high-density screens Windows bitmap-stretches the window and X11 renders it tiny.
  - **Do:** `XOpenIM` / `XCreateIC` + `Xutf8LookupString` (and `XFilterEvent`) on X11; per-monitor-v2 DPI
    awareness and `WM_DPICHANGED` on Windows; a single UI scale factor (auto from `Xft.dpi` / monitor DPI,
    overridable in Settings) applied to every metric in `src/widgets/theme.h`.
  - **Done when:** typing `è`, `ñ`, `ő` (dead key) and pasting CJK text round-trips through the editor into a
    table on both platforms; at 200 % scaling the UI is sharp and proportioned.

---

## P3 — Ecosystem, Testing, Benchmarks & Open-Source Polish

- [x] **P3.1 `[TEST]` Crash and fault-injection harness**
  - **Where:** new `tests/harness/` + `Makefile`; engine I/O goes through `sv_pwrite` / `sv_write` / `sv_fsync` /
    `sv_ftruncate` (`src/platform_compat.h`) and `rename` (`schema.c:284`, `auth.c:189-191`).
  - **Do:** link the test binaries with `-Wl,--wrap=` shims that (a) count calls, (b) fail the N-th with a chosen
    `errno`, (c) `_exit` at the N-th, (d) record "durable" state only at `fsync` so a power loss can be replayed
    by discarding unsynced writes. Drive a model-checked workload (random interleaved transactions with
    commit / rollback / savepoints) and compare against an in-memory oracle after each simulated crash.
  - **Done when:** the harness reproduces P0.4-P0.6 on v1.2.0 and is part of `make test`.
  - **Done:** `tests/test_durability.c` (in `make test`), built with `-DSV_FAULT_INJECTION` (macro shims in
    `platform_compat.h` instead of `--wrap`): counts calls, fails the N-th `write` / `pwrite` / `fsync` / `pread`,
    `_exit`s at the N-th, and simulates a **power cut** — every write keeps its previous bytes until an `fsync`
    of that file makes it durable; at the cut the child saves the journal and dies, the parent undoes the
    unsynced writes. Scenarios: failed COMMIT `fsync`, full log, crash at every write of `ROLLBACK` (37 points)
    and of recovery (367), power cut at 61 points of a workload of transactions (each `INSERT` + `DELETE` +
    `UPDATE`, `synchronous = FULL`: after recovery exactly the committed ones, each whole), damaged pages,
    catalog and users files, recovery memory, rollback cost, `synchronous`. **Not done:** a randomly interleaved
    multi-session workload checked against an in-memory oracle.

- [~] **P3.2 `[TEST]` Sanitizer and platform coverage in CI**
  - **Where:** `Makefile:187-191` (`test-tsan` covers `test_parallel` and `test_php` only), `:267-321`
    (`test-asan` omits `test_mysql`, `test_web`, `test_mail`, `test_tls`); `.github/workflows/ci.yml` (one Ubuntu
    job; the MinGW build is compiled but never run).
  - **Do:** ASan + UBSan for every suite; TSan for `test_sessions`, `test_server`, `test_web` and the new
    parallel cases (P0.3); a `windows-latest` job that runs the suites natively; `macos-latest` for
    `segvault-server`; `-fanalyzer` and `cppcheck` as non-blocking jobs first; cache build objects.
  - **Done when:** CI has the five jobs and a deliberately injected race / overflow fails each relevant one.
  - **Done:** `.github/workflows/ci.yml` gains a `sanitizers` job (`make test-tsan`, the durability suite and
    `test_h2` under ASan + UBSan), the Windows-only code run under wine (`make test-win`), a `-DDEBUG` compile
    check and a non-blocking `cppcheck` job. **Still open:** native `windows-latest` / `macos-latest` jobs,
    every suite under ASan in CI, object caching.

- [~] **P3.3 `[TEST]` Fuzzing of every parser that reads untrusted bytes**
  - **Where / targets:** `lexer_tokenize` + `parser_parse` (`src/query`), `parse_head` and `read_chunked`
    (`src/server/server.c:2508-2573,2661-2697`), `mw_parse_login` and `mw_bind_execute`
    (`src/server/mysql_wire.c:241-260,728-773`), `hpack_decode` and the frame loop (`src/server/h2.c:236-299,722-800`),
    `json_parse` (`src/server/json.c`), `rw_apply` (`src/server/rewrite.c`), the SMTP / MIME reader
    (`src/server/mail.c`), `catalog_load`, `page_validate` (P0.10), `regex.c`, `sqljson.c`.
  - **Do:** libFuzzer entry points under `tests/fuzz/` with seed corpora taken from the test suites; an
    engine-level differential fuzzer (random SQL against SQLite for the common subset); nightly CI run with
    corpus caching.
  - **Done when:** each target runs 10 minutes clean under ASan + UBSan in CI.
  - **Done (without libFuzzer: no clang here):** `tests/fuzz/fuzz.c` (`make fuzz FUZZ_TARGET=… FUZZ_SECS=…`), a
    mutational fuzzer under ASan + UBSan with built-in seeds: `sql` (lexer + parser), `json`, `login`
    (`mw_parse_login`), `page` (`page_validate`, `page_compact`, `tuple_deserialize`), `catalog`
    (`catalog_load` of mutated files) and `exec` (mutated statements executed on a live database with tables,
    foreign keys, views, triggers). It found two real bugs, both fixed with regression tests in
    `tests/test_sql.c`: a heap over-read freeing an incomplete `SET` in a routine (`parse_trig_stmt`) and a
    stack overflow in `decimal_round` for `DECIMAL(10,200)` (the parser now refuses scales over 30, as MySQL).
    Runs: millions of inputs per target, 5-minute runs of `exec` and `sql` clean. **Still open:** HTTP
    (`parse_head`, `read_chunked` are static), HPACK, `.htaccess`, SMTP / MIME, regex and SQL/JSON targets; a
    differential run against SQLite; the nightly CI job.

- [x] **P3.4 `[DX]` Build system**
  - **Where:** `Makefile:20` (flags), `:147-263` (each test target recompiles the 600 KB `executor.c` and the whole
    engine), `:85` (`.PHONY` list misses the test binaries' dependencies on headers);
    `include/common.h:58-61` (`SV_CHECK_NULL` passes `msg` as a stray `fprintf` argument and tests nothing),
    `:66-69` (`SV_DEBUG` has a trailing `\` before `#else`: `-DDEBUG` does not compile), `:76` (`SV_ARRAY_LEN`
    ends with `;`).
  - **Do:** build the engine once into `build/<config>/libsegvault.a` (configs: release, asan, tsan) and link the
    tests against it; add `-Wshadow -Wformat=2 -Wstrict-prototypes -Wvla -fstack-protector-strong
    -D_FORTIFY_SOURCE=3 -fPIE -pie -Wl,-z,relro,-z,now` (and `-fcf-protection` where available); fix or delete
    the three macros; add a `DEBUG=1` build to CI.
  - **Done when:** a no-change `make test` takes seconds; `make DEBUG=1` builds.
  - **Done:** every regular test binary links `libsegvault.a` (engine compiled once, with header dependencies)
    instead of recompiling the engine: a clean `make all test` uses 3 min of CPU instead of 9 min 51 s (wall
    time is now dominated by running the suites); sanitizer builds and the fault-injection suite still compile
    the sources with their own flags. The three macros — `SV_CHECK_NULL(ptr, msg)` now tests the pointer, `SV_DEBUG` compiles with
    `-DDEBUG`, `SV_ARRAY_LEN` has no trailing `;`; `-D_FILE_OFFSET_BITS=64` added to `CFLAGS` (P0.12).
    Hardening: `-Wshadow -Wformat=2 -Wstrict-prototypes` for every build (the 9 shadowed variables fixed —
    one rename in `tuple.c` was caught by the suites before it shipped), `-fstack-protector-strong -fPIE` and
    `-pie -Wl,-z,relro,-z,now` for the Linux executables; the `-DDEBUG` compile check is in CI.
    **Not done:** `-Wvla` (two deliberate VLAs remain: function arguments in `executor.c`), per-config library
    builds (release / asan / tsan), `-fcf-protection`.

- [~] **P3.5 `[REPO]` Repository hygiene**
  - **Where:** `test_sessions_tsan` (2.1 MB ELF tracked at the root since `45ae544`, not in `.gitignore`, not in
    `make clean`); `bin/linux-x86_64/*`, `bin/windows-x86_64/*` (five executables re-committed per release;
    `.git` is already 65 MB); `test_engine.c` at the root while every other test is in `tests/`.
  - **Do:** `git rm` the stray binary and ignore `*_tsan`; publish executables only as release assets with
    `SHA256SUMS` (and a signature); keep `bin/README.md` pointing at the releases page; move `test_engine.c`
    to `tests/`.
  - **Done when:** a fresh clone contains no executable; the release workflow attaches assets and checksums.
  - **Done:** `test_sessions_tsan` deleted from the tree (the deletion still has to be committed), `*_tsan` in
    `.gitignore` and in `make clean`. **Still open:** release-only executables, moving `test_engine.c`.

- [ ] **P3.6 `[BENCH]` Reproducible benchmarks with a baseline**
  - **Where:** `bench/bench.c` (91 lines: one schema, in-process only), `Makefile:354-358`.
  - **Do:** scenarios — bulk load, point lookup, range scan, join, update-heavy, commit rate by `synchronous`
    mode, rollback cost (P1.2), cold start, 1-16 concurrent readers and writers, PDO over the MySQL protocol —
    each also run against SQLite (in-process) and MariaDB (protocol) with the same SQL; JSON output; a CI job
    that fails on a > 15 % regression against the stored baseline.
  - **Done when:** `make bench` prints a comparison table and the README's performance section is generated from it.

- [x] **P3.7 `[DOCS]` Document the real guarantees and limits**
  - **Where:** `README.md` (reliability and architecture sections), `docs/API.md`.
  - **Do:** state durability per mode (P1.4), isolation as implemented (table locks to commit, committed-version
    reads, what `transaction_isolation` reports in `mysql_wire.c:273`), every hard limit of
    `include/common.h:33-41` and `schema.h`, the security model ("no users = everyone is root", what `--token`
    and `--users` protect, the file permissions), the on-disk formats (page, row, WAL, catalog) with version
    history; add `SECURITY.md`, `CONTRIBUTING.md`, a `CHANGELOG.md`, and an English README next to the Italian one.
  - **Done when:** every number in the docs is produced by a test or a `_Static_assert`.
  - **Done:** `docs/FORMATS.md` (page, row, overflow stub, binary encoding, every WAL record type incl. CLR and
    LINK, recovery passes, catalog with CRC trailer, `users.dat`, durability modes, isolation as implemented, all
    hard limits), `SECURITY.md` (who gets in with which setup, "no users = root", files, logs, reporting),
    `CONTRIBUTING.md`, `CHANGELOG.md`, `README.en.md`; README durability table and buffer-pool line updated.
    **Not done:** generating every number from a test or `_Static_assert`.

- [ ] **P3.8 `[DX]` Allocation-failure policy**
  - **Where:** unchecked results, e.g. `src/tx/wal.c:9`, `src/tx/transaction.c:5`, `src/table/tuple.c:8`,
    `src/bridge/db_api.c:1311-1387,1447-1461,1506-1518`, `src/server/server.c:793`.
  - **Do:** choose one rule — "engine allocations go through `sv_xalloc`, which reports and fails the statement
    via a `longjmp`-free error path" — and apply it; add an allocation-failure sweep to the harness (fail the
    N-th `malloc` for N = 1…k on the SQL test corpus).
  - **Done when:** the sweep completes with no crash and no leak under ASan.

- [ ] **P3.9 `[AUDIT]` Finish the code audit of the modules that were only spot-checked**
  - **Where:** `src/query/parser.c` (5 122 lines) and `src/query/executor.c` (11 959 lines: constraint and
    foreign-key enforcement, `ALTER TABLE` rewrite, trigger / routine interpreter, `MemIndex` maintenance on
    write, `sequences.txt` persistence at `:2295-2297`); `src/server/tls.c`, `mail.c`, `rewrite.c`, `proxy.c`,
    `json.c`, `service.c`; `src/query/regex.c`, `sqljson.c`; `src/widgets/*` (20 k lines), `src/platform/win32.c`.
  - **Known so far from spot checks:** mail message ids are parsed with `strtol` and the HTML view is served
    with `Content-Security-Policy: sandbox` inside a sandboxed iframe (`mail.c:383,885-902`); the CA key is
    `chmod 0600` after writing (`tls.c:331-334`, leaving a short window with default permissions); the regex
    matcher has step and depth limits (`regex.c:275,335`); expression depth is bounded in the parser
    (`parser.c:475,612`).
  - **Do:** same method as this pass — read, write a probe or a sanitizer test for each suspicion, add items here
    with `file:line`. Priorities: (1) executor write paths against the crash harness (P3.1), (2) `tls.c`
    certificate and key handling, (3) `proxy.c` response parsing, (4) `rewrite.c` against hostile `.htaccess`.
  - **Done when:** the "Not read line by line" list at the top of this file is empty.

---

## Completed milestones (v1.0 → v1.2.0)

Condensed from the previous roadmap; every item below shipped and is covered by the test suites named.

- **PHP (FastCGI):** supervised `php-cgi` pool or external php-fpm, full CGI variables, front controller,
  chunked request bodies, streamed responses and Server-Sent Events, source never served when PHP is off
  (`test_php`, 115 checks).
- **Web server:** MIME types, `Range`, `ETag` / 304, gzip, `404.html`, directory listing, per-folder sites on
  `NAME.localhost` / `NAME.test`, reverse proxy with WebSocket, dashboard, `.htaccess` (`mod_rewrite`, access
  rules), HTTPS with a local CA and SNI, HTTP/2, TLS for the MySQL protocol (`test_web`, `test_tls`).
- **MySQL compatibility:** MySQL `CREATE TABLE` dialect, catalog v5, `information_schema`, `SHOW` forms, upserts,
  multi-table `UPDATE` / `DELETE`, regular expressions, 100+ functions, `_ci` collations, WordPress 7.1.4
  end to end (`test_mysql`, 139 checks); in 1.2: full stored-routine language, session variables, window and
  JSON functions, `RETURNING`, `MERGE`, `ROLLUP` / `CUBE`, `PREPARE` / `EXECUTE`, CTAS, multi-operation `ALTER`,
  `LATERAL`, `JSON_TABLE`, sequences, generated columns, cross-database queries (`test_sql`, 1 449 checks).
- **Concurrency:** per-statement table locks held to commit, deadlock detection (1213), lock wait timeout (1205),
  parallel read-only `SELECT`s, committed-version reads for tables being written (`test_sessions`, `test_parallel`).
- **Control panel:** services started and stopped individually, per-service log, tray icon, start at login,
  `--install-service`, busy-port diagnosis, portable packages, zero-config PHP.
- **Extras:** mail catcher (SMTP, MIME, web page, JSON API), CGI programs (Perl, Python, Ruby).
