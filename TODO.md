# SegVaultDB — Engineering Roadmap

## Stato attuale — versione 1.5.1, aggiornato al 2026-11-27

Versione nel codice: **1.5.1** (`include/common.h`); ultima release pubblicata: **v1.5.1** (2026-11-27; la v1.5.0 dello
stesso giorno non ha pacchetti: il build Windows falliva). Le novità dalla 1.4.2 sono nel `CHANGELOG.md` sotto 1.5.0. `make test` (27 suite) e `make test-asan` verdi.

**Code review 2026-11-26 (voci `CR-*`, in fondo al file):** corrette tutte le voci che erano difetti, compresa
CR-B6 (`BACKUP TO` non scrive più nelle cartelle servite né in quella dei dati) e CR-B9 (`INSERT()`, JSON
normalizzato, regex in cache); fatti anche CR-B2 (un solo buffer di testo, `include/sv_strbuf.h`) e CR-M9 (cartella
privata per ogni suite, `tests/harness.h`) e CR-B10, i difetti trovati il 2026-11-27 con prove contro MySQL (commit
implicito prima del DDL, sottoquery correlate che davano "" invece di NULL, `COLLATE` / `BINARY`, tipi persi da
`SHOW CREATE TABLE` e dai dump, limiti di `TINYINT` / `INT UNSIGNED`, `CAST AS UNSIGNED`, `COUNT(DISTINCT a, b)`,
`ORD`). Restano i refactoring strutturali, che non correggono difetti visibili:

| Voce | Cosa resta |
|---|---|
| CR-M2 `[~]` | tabella `FuncDef` con puntatori a funzione al posto delle catene `FIS(...)` (oggi costano poco) |
| CR-M5 `[ ]` | il motore è un singleton di processo (circa 100 variabili globali, nessun handle nell'API) |
| CR-M6 `[ ]` | dividere `executor.c` (13 700 righe) e `window.c` / `AppWindow`; poi i test della logica di stato della finestra |
| CR-M11 `[ ]` | confine GUI ↔ logica: un terzo di `src/widgets/` non è interfaccia; la GUI comanda il motore costruendo SQL |

**Roadmap P0–P4 (audit dalla 1.2.0 in poi):** P0 tutto chiuso. Aperte:
P1.14 `[~]` (streaming di `ORDER BY` / `GROUP BY` / join), P1.15 `[~]` (B+tree mantenuto dalle scritture),
P1.17–P1.20 (statement serializzati nel server, lock per tabella, livello di isolamento dichiarato, group commit),
P2.16 (64 connessioni in tutto), P3.9 `[~]` (audit riga per riga di `parser.c`, `executor.c`, widget, `win32.c`),
P3.11 / P3.12 `[~]` (ultimi limiti fissi), P3.13 (client diversi da PHP), P3.14 (benchmark contro MariaDB),
P4.1–P4.23 (parità di prodotto con XAMPP / UniServerZ / Laragon: eventi, binlog, più versioni di PHP, Xdebug,
macOS, firma degli eseguibili, aggiornamenti, licenza, sicurezza predefinita, …).

## Code review 2026-11-26 — riepilogo

Revisione tecnica completa del working tree (`php-fastcgi` @ `3b66b31` + modifiche non committate): **24 voci nuove —
1 critica, 5 alte, 11 medie, 7 basse** — nella sezione "Code review 2026-11-26 — audit tecnico completo" in fondo al
file (ID `CR-*`, checkbox, `path:line`, perché è un problema, correzione). 5 voci sono confermate da una prova
eseguita (programmi su `libsegvault.a`, ThreadSanitizer): CR-C1, CR-A1, CR-A4, CR-A5, CR-M1. Le altre seguono dal
codice citato o da misure sul codice (warning del compilatore, conteggi); l'unico sospetto non provato (codice solo
Windows) porta l'etichetta **[da verificare]**. Base: `make test` e `make test-asan` verdi su una copia dell'albero.

**Stato 2026-11-26 (notte):** corrette 25 voci, tutte e 5 le priorità comprese — CR-C1, CR-A1, CR-A2, CR-A4,
CR-A5, CR-M1, CR-M3, CR-M4, CR-M7, CR-M8 (da provare su Windows), CR-M10, CR-B1, CR-B3, CR-B4, CR-B5, CR-B7, CR-A3, e i nuovi CR-A6 (DoS con REGEXP), CR-M12
(lettere accentate), CR-M13 (`LAST_INSERT_ID`), CR-M14 (divisione e `AVG` come MySQL), CR-M15 (`ROUND` / `FORMAT`),
CR-B8 (`FROM DUAL`) — più
due in parte: CR-M2 (smistamento delle funzioni 4-6 volte più veloce) e CR-B6 (resta la scelta sul percorso di
`BACKUP TO`). Restano aperte: CR-M5, CR-M6, CR-M9, CR-M11, CR-B2 (refactoring strutturali) e la scelta sulla
collation predefinita (CR-M12). Fuzzer (14 target, 60 s ciascuno) e confronto con SQLite: nessun errore. `make test` verde.

**Stato 2026-11-27:** corrette anche CR-B6 e CR-B9 (più `JSON_ARRAY_INSERT` / `JSON_MERGE_PRESERVE` trovate strada
facendo), CR-B2 (buffer unico, con due difetti trovati nelle vecchie copie) e CR-M9 (punti 1-3). Restano CR-M2 (in
parte), CR-M5, CR-M6 e CR-M11.

**Le 5 priorità da affrontare per prime**

1. **CR-C1 — escalation di privilegi al riavvio.** `users.dat` salva i GRANT come parole separate da spazi, ma i nomi
   di database e tabelle possono contenere spazi: ``GRANT SELECT ON shop.`t 255` `` torna dopo il riavvio come
   `GRANT ALL PRIVILEGES ON shop.t` (provato), e ogni permesso su un nome con spazi sparisce. Codificare i nomi e
   validare le righe al caricamento.
2. **CR-A2 (con CR-B6) — l'API SQL di amministrazione risponde a ogni sito ospitato.** Il controllo `Origin == Host`
   fa passare il JavaScript di qualunque pagina servita da SegVault (plugin, bundle npm, XSS), che nella configurazione
   predefinita esegue SQL come root su tutti i database (e può mettere `users.dat` nella radice di un sito con
   `BACKUP TO`). Servire l'API solo sull'host di amministrazione e con un segreto.
3. **CR-A5 e CR-A1 — due race nel motore confermate da TSan.** `db_current_database()` restituisce un puntatore
   dentro il catalogo dopo aver rilasciato il lock (use-after-free se un altro thread fa DDL: la GUI ospita il
   server nello stesso processo); le letture parallele aggiungono senza lock elementi alla lista globale delle mappe
   dell'heap.
4. **CR-A4 — `LIMIT` / `OFFSET` oltre 2³¹.** `LIMIT 7, 18446744073709551615`, l'idioma che il manuale di MySQL
   indica per "fino alla fine", restituisce 0 righe; `VARCHAR(4294967297)` diventa `varchar(1)`. Correzione piccola
   (`strtoull` con controllo dell'overflow), impatto alto: risultati sbagliati senza errori.
5. **CR-A3 — GUI ferma durante backup, restore e import.** Lettura e traduzione di un dump da centinaia di MB, e
   il backup con il lock esclusivo del motore, girano sul thread dell'interfaccia, senza avanzamento né annullamento:
   è il percorso più usato nelle classi.

**Valutazione complessiva.** Progetto molto ambizioso: un motore SQL con WAL, B+tree e versioni confermate, HTTP/1.1
e HTTP/2, TLS, protocollo MySQL, FastCGI e una GUI con widget propri, tutto in C. Per un sistema di questa ampiezza
le pratiche di verifica sono sopra la media: 26 suite con circa 4 000 controlli, ASan/UBSan/TSan, fuzzing notturno,
fault injection della durabilità, build Windows provata con wine, nessun warning con `-Wall -Wextra`. Anche la
sicurezza è curata in molti punti: PBKDF2 e confronti a tempo costante, difesa dal DNS rebinding, file privati
scritti in modo atomico, limiti di profondità e di dimensione. Quasi tutti i problemi trovati nascono da tre
debolezze strutturali:
(1) lo stato globale del motore, con le regole di locking scritte solo nei commenti (CR-M5, da cui CR-A1 e CR-A5);
(2) formati e interfacce a testo senza escape (CR-C1, CR-B5, e la GUI che comanda il motore costruendo SQL, CR-M11);
(3) file e funzioni troppo grandi per una revisione seria (CR-M6: `executor.c` da 13 660 righe, `window.c` da 6 255
con una `AppWindow` di 116 campi, 17 funzioni oltre le 200 righe).
Voto indicativo per area:

| Area | Voto | Note |
|---|---|---|
| Motore | 7/10 | durabilità solida, concorrenza fragile |
| Server | 7/10 | |
| GUI | 5/10 | lavoro pesante sul thread dell'interfaccia, god object |
| Test | 8/10 | |
| Architettura e manutenibilità | 5/10 | |
| Sicurezza | 6/10 | finché CR-C1 e CR-A2 restano aperte |

---

*Storico.* Audit baseline: **v1.2.0** (`124ec1a`, branch `php-fastcgi`), audited 2026-11-14. The P0–P3 items below
came from that audit (P4 from the 1.5.0 review); their original `file:line` anchors refer to that commit, the
"Fixed" notes to the code of the time. For the current state see "Stato attuale" at the top. The previous roadmap (XAMPP / UniServerZ parity) is
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

**Status 2026-11-24 — architecture review of v1.5.0 (new section P4, competitive matrix and v1.6 / v1.7 / v2.0
roadmap at the top of P4):** new items — **P0: 0 · P1: 4** (P1.17–P1.20) **· P2: 1** (P2.16) **· P3: 2** (P3.13, P3.14)
**· P4: 23** (P4.1–P4.23); "Review 2026 Update" notes added inside the open items P1.14, P1.15, P3.9 and P3.12
(×3: leftover caps, 32-argument function calls that break WordPress `post__in` ordering, 63-byte identifiers).
Baseline for the review: `make clean all test dist`, `make test-asan` and the MinGW build + wine tests all green on
the 1.5.0 tree; probes in `/tmp/sv_audit_probe/` (outside the repository).

**Status 2026-11-24:** P3.11 closed (all nine caps, plus `.htaccess` fail-open paths, atomic GUI config saves, a
private `server.token`, HTTP/2 header drop) and two security findings fixed: **stored identifier injection** (GUI
built GRANT / DROP / dump SQL from unquoted names; engine now refuses backticks and control characters in new
names) and a **login-throttle bypass** (64-slot LRU eviction). P3.12 #1 done (300-column `UPDATE` / upsert).
`make test`: everything green except `test_durability.c:1325` (P1.15 acceptance check, fails identically on
HEAD `3b66b31`: writes after a restart with on-disk trees still scan the table — expected until P1.15 is done).

**Status 2026-11-17:** new audit of hard-coded limits after the 1024-column change: P3.11 (caps that silently
drop data, rules or rows — includes a fail-open `.htaccess` deny and `server.ini` sites being erased) and P3.12
(engine limits below MySQL: 64-column `UPDATE`, 8 `JOIN`s, 16 indexes). GUI 64-column cap fixed.

**Status 2026-11-16 (after 1.4.2):** closed P3.10 (CA name constraints); P1.14 partly done (column pruning,
early stop at `LIMIT`); P3.9 advanced (executor write-path probes, `win32.c` launch / clipboard paths); still
closed P1.6 (read-only B+trees written at shutdown / `CHECKPOINT`; maintained trees → new P1.15); open P1.14
(single-table `SELECT` streams; `ORDER BY` / `GROUP BY` / joins still materialise), P1.15, P3.9
(`parser.c`, most of `executor.c`, `src/widgets/*`, rest of `win32.c`).

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

- [x] **P1.1 `[ENGINE]` WAL format v2: record checksum + LSN, page LSN + page checksum, compensation records, fuzzy checkpoints**
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
  - **Also (found by P3.6):** `ROLLBACK` = `wal_flush` (fsync) + `bp_flush_all` (every dirty page) + undo on the
    file + `ABORT` + `wal_flush` (fsync): ~5 ms on disk, 186 rollbacks/s against SQLite's 90 000/s. With page
    LSNs the undo can work on buffer-pool pages and the `ABORT` needs no `fsync` (recovery undoes a
    transaction without `COMMIT` anyway). Target: `rollback` in `make bench` within 10× of SQLite.
  - **Done (log v2, 2026-11-15):** new log format (`src/tx/wal.h`): a 48-byte file header (`SVWAL2`, version,
    page size, base LSN, start of the contiguous records, CRC-32C) and records `len | crc32c | lsn | type | tx |
    page | slot | dlen | olen | data | old` (CRC-32C over everything after the CRC, `src/storage/crc32c.h`).
    The LSN is the record's logical position and never goes back (a checkpoint keeps counting), so BEGIN /
    savepoint / CLR references stay valid across checkpoints. Reading tells the two failures apart: a bad record
    with nothing valid after it is an interrupted write (dropped, and recovery cuts it off before appending its
    CLRs — before, CLRs could land after a half-written record); a bad record followed by valid ones, or a bad
    header, is a damaged log: recovery stops with "the transaction log is damaged at byte N … nothing was
    applied", the log is left as it was. v1 logs (23-byte header, no CRC) are still recovered. **Fuzzy
    checkpoint:** with transactions open, the checkpoint (pages + catalog on disk) rewrites the log keeping
    only the records of the open transactions ("carried", with their old LSNs, indexed on open), so the log
    shrinks however long a transaction stays open; `CHECKPOINT` is allowed inside a transaction; automatic
    checkpoints with open transactions only when the log has doubled since the last one. Tests in
    `tests/test_durability.c`: with a transaction open across 60 000 inserts the log stays < 20 MB and the
    transaction is undone correctly by `ROLLBACK` and, after a crash, by recovery from the carried records; a
    flipped byte in the header or in the middle of the log is refused and the log left unchanged, one in the
    last record drops only that insert; **power cut at every write** of 10 transactions interleaved over 3
    sessions: every committed transaction whole, the one committing whole or absent, the others absent.
    `db_set_checkpoint_bytes()` sets the threshold.
  - **Done (pages, 2026-11-16):** data-file **format 2**: page header 24 bytes with `lsn` (8) and `crc` (CRC-32C of
    the page, set by `page_write`, checked on every read — `page_validate`; hardware `crc32` instruction when the
    CPU has SSE 4.2); page 0 is a file header (`SVDATA2`, format, page size, the LSN a new log starts from).
    **Migration on open** (`src/storage/page_migrate.c`): a 1.3 file is copied to `segvault.db.v1`, converted in
    place (rows stay in the same slots — data moves 12 bytes, pages are compacted when needed and, only if a page is
    completely full and no log has to be recovered, the largest row moves to a new page of the chain), overflow
    chains are rewritten with the new chunk size and the stub replaced in place, the old chain pages go to the free
    list; an interrupted conversion restarts from the copy, which is removed at the end. Every logged change stamps
    the page with its record's LSN (`bp_note_lsn`; CLRs too), and **redo applies a record only if the page LSN is
    older** (plus the old slot checks). **Torn pages:** before a page that existed at the last checkpoint is
    written to the data file, its full image goes into the log and the log is synced (one `fsync` for a whole
    flush); pages created since the last full flush need no image (the log has every record since they were empty);
    recovery puts an image back when the page on disk fails its CRC or is older, and a page rebuilt from empty
    always takes its next image. Overflow pages (immutable) get no image. **ROLLBACK and ROLLBACK TO work on the
    buffer-pool pages** (`wal_undo_tx_cache`): CLRs in the log buffer, no page flush, no `fsync`, and the `ABORT`
    record need not be durable (a crash before it leaves a transaction without `COMMIT`, which recovery undoes,
    redoing the CLRs); only the touched tables lose their in-memory indexes. The log reader uses a 4 MB read window
    (a checkpoint with open transactions scanned the log record by record: `update_heavy` had dropped to 23 k/s, now
    47 k/s); recovery cuts and reports an incomplete last record. Tests (`tests/test_durability.c`): a real 1.3.0
    database (`tests/fixtures/format1-v1.3.0.tar.gz`: 2 572 rows, 5 rows of 20-100 KB, pages filled to the byte)
    opens with identical results, accepts writes, survives a restart, and also upgrades after an interrupted
    conversion; a flipped byte in a data page is reported as damaged; half-zeroed pages after a crash are repaired —
    new pages from the log, old pages from their images; crash at every page and log write of ROLLBACK and of
    recovery. `bench`: see the `rollback` row of the README table (`make bench-readme`).

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

- [x] **P1.5 `[ENGINE]` Space is never reclaimed: dead slots, dropped tables and old overflow chains** — [reproduced]
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
  - **Done (with the page LSN of P1.1, 2026-11-16):** **UPDATE in place**: the new row takes the same slot when it fits
    in the page (counting what compaction recovers) — `LOG_UPDATE` now means "same slot", redo replaces the slot
    content under the page-LSN rule, undo uses a new `CLR_REPLACE`; otherwise delete + insert as before. **Space
    freed at once**: an overflow chain replaced or deleted (also the chain of a large row inserted by a transaction
    that rolls back), and every page of a dropped table with the chains of its rows, go back to the free list as soon
    as the change that abandoned them is durable in the log (`ready` LSN ≤ `synced_lsn`: until then a crash could
    bring back the row that uses them); with ≥ 64 KB waiting and `synchronous = NORMAL` the log is synced at the
    end of the statement so the next one reuses them. A freed page is written `FREE` with the current LSN and a
    reused page keeps it, so redo skips every record of its previous life. A heap page is never written before the
    overflow chains it points to (`ov_unsynced`: one data `fsync` in between). **`VACUUM FULL`**: every table
    rewritten into compact pages taking the lowest free pages first (two passes), old pages freed, free pages at
    the end of the file cut off. Measured (`tests/test_overflow.c`): 2 000 rows + ten full-table UPDATEs → file
    +0 %; 20 × (DROP, CREATE, 500 inserts) → +0 % after the first round (before: 2.09 MB); 1 000 updates of a 100 KB
    row → ≤ +300 KB (one copy of the row in flight; before: +5 MB per 50); VACUUM FULL after deleting 90 % of a
    table shrinks the file and keeps every row. **Not done:** reusing dead slot entries for new inserts (4 bytes per
    deleted row stay in the page directory: a reused slot would need per-slot commit state for the snapshot reads
    of other transactions), the free list still lives in `segvault.free` (rebuilt by VACUUM if lost), no
    size-class bins (the free-space map scan with its hint stays).

- [x] **P1.6 `[ENGINE]` Indexes exist only in memory; the on-disk B+tree is dead code**
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
  - **Plan (2026-11-16, not started — needs a session of its own: an index that disagrees with its table is
    silent data loss).** Two steps, the first already meeting the cold-start goal without touching the write
    path:
    1. *Checkpointed index snapshots.* At each checkpoint (and clean shutdown) every `MemIndex` that is
       complete is written bottom-up as a read-only B+tree (no splits: keys are already sorted) — leaf pages
       `[n][key len + key bytes (the existing `KEY_BUF` text, collation-folded)][RID list]`, internal pages
       `[n][separator key][child page]`, page flag `PAGE_FLAG_INDEX`, page LSN = checkpoint LSN, pages from the
       free list; the root goes into `Index.root_page_id` with the catalog save of the checkpoint, the old
       tree's pages are freed after that save. On the first write to a table after a checkpoint, a
       `LOG_CATALOG` record setting its roots to 0 is logged **before** the row records (so recovery also
       sees the tree as stale), then the in-memory index is built as today. A lookup on a table whose root is
       set reads root → leaf through the buffer pool (≤ 4 pages for 1 GB with 4 KB pages and short keys) and
       never builds the `MemIndex`; `EXPLAIN` says "B+tree (on disk)". Crash cases to test: crash while the
       tree is written (catalog still has the old root: new pages are orphans, freed by `VACUUM`), crash after
       the "stale" record (fallback to the scan), torn index page (CRC: treated as stale).
    2. *Maintained B+tree.* Inserts / deletes applied to the tree inside the transaction, logged as page
       images (`LOG_PAGE_IMAGE` already exists) with the transaction's id so undo restores the before-image,
       splits and merges as a sequence of images in one atomic group; then the `MemIndex` becomes an optional
       cache. Remove `src/index/btree.c` once step 1 lands (or reuse its tests).
  - **Step 1 done (2026-11-16):** `src/index/bptree.c` — read-only B+tree built bottom-up from sorted keys
    (slotted pages, `PAGE_FLAG_INDEX` / `PAGE_FLAG_INNER`, leaf entries `key + RID list`, internal entries
    `first key + child`), lookup and page walk; the old `src/index/btree.c` (unused `uint64_t` tree) is
    removed and `tests/test_btree.c` now tests the new one (20 000 keys: 123 pages, ≤ 2 page reads per
    lookup, absent keys, empty tree, too-long key refused with every page returned, non-index and damaged
    pages reported as errors). At clean shutdown and on `CHECKPOINT` with no open transaction, every
    in-memory index on one column of a table with ≥ 64 rows (no long keys, ≤ 600 rows per key) is written as
    a tree; the catalog (format **v7**, `LOG_CATALOG` records v2; v6 files and v1 records still read) keeps
    per table up to 16 trees with the table root, column type and collation they were built for, so a
    rewritten table (`ALTER`, `TRUNCATE`) or a changed column never uses an old tree. The first logged row
    change of a table drops its trees and logs the catalog change **before** the row record (recovery replays
    it first); `VACUUM FULL` drops every tree; pages of dropped trees are freed after the next catalog save,
    and `VACUUM` frees index pages that no tree in the catalog reaches (orphans of an interrupted shutdown,
    dropped tables). A primary-key or unique lookup uses the tree when no in-memory index exists yet and
    opens the table without its free-space map (which read the whole chain); `EXPLAIN` says
    "B+tree on disk"; a tree that cannot be read (damaged page) falls back to the in-memory index. Tests
    (`tests/test_durability.c` §10, a 20 000-row table of ~1 600 pages): after a restart a primary-key lookup
    reads **3 pages** and builds no in-memory index; text keys; absent keys; a write sees the new value and
    the next shutdown writes a new tree; a crash right after a delete and an insert (trees on disk, change
    only in the log) never uses the old tree; `ALTER TABLE` lookups read the rewritten table; damaged index
    pages fall back; `DROP TABLE` + `VACUUM` frees pages. **Not done (step 2):** trees are not maintained
    by writes (a modified table goes back to building its in-memory index on the next lookup until the next
    clean shutdown / `CHECKPOINT`); range scans and composite keys still use the in-memory index; trees are
    not written by the automatic checkpoints (cost proportional to the table on write-heavy workloads).
    Measured on a 130 000-row table: first lookup after opening 87 ms → 0.03 ms; the shutdown that writes the
    tree takes 21 ms once (0.4 ms afterwards). `bench/suite.c`: `cold_start` now runs the same first query as
    the SQLite variant (`SELECT COUNT(*)`) after one untimed reopen, new `cold_lookup` scenario (first
    primary-key lookup after reopening: 60 000/s, SQLite 92 000/s); both baselines updated for these two
    scenarios only (the scenario changed, so the 1.3 ratio no longer applies). Power cut at every write of a
    `CHECKPOINT` that rewrites a tree (51 points, `tests/test_durability.c` §10): lookups always right after
    the restore, `VACUUM` works. **The three "Done when" criteria hold** (≤ 4 page reads after a cold start,
    `EXPLAIN` shows the index, crash tests over a tree write in progress — bulk-built trees have no splits);
    the maintained tree (step 2) is **P1.15**.

- [~] **P1.15 `[ENGINE]` On-disk B+tree maintained by writes (P1.6 step 2)** — [code] · **Size:** XL · **Deps:** P1.6
  - **Where:** `src/index/bptree.c` (read-only trees), `src/query/executor.c` (`MemIndex`, `trees_on_write`).
  - **Why:** after the first write a table goes back to its in-memory index (built by a full scan, memory outside
    the buffer pool, lost on every rollback of a write to the table) until the next clean shutdown or
    `CHECKPOINT`; uniqueness checks of bulk loads still need the whole index in RAM; range scans and composite
    keys never use the disk.
  - **Do:** insert / delete in the tree inside the transaction; every changed page logged as a page image with
    the transaction id (`LOG_PAGE_IMAGE` exists; undo restores the before-image), splits and merges as one
    atomic group of images; range scans through leaf sibling links; composite keys with the same text
    encoding; `MemIndex` kept only as an optional cache.
  - **Done when:** a table written after a restart keeps answering point and range lookups with ≤ 4 page reads
    and no full scan; power cut at every write of a split; RSS of a 10 M-row table with two indexes does not
    grow with the index size.
  - **Progress 2026-11-24 — the remaining full scan is the free-space map, not the indexes.** Measured per
    statement after a restart (20 000 rows, ~1 600 pages): the first `INSERT` reads **1 608 pages**, everything
    after it 0–3. `heap_open` → `meta_build` walks the whole page chain to learn each page's free space and the
    tail (`src/table/heap.c`); for a big imported table that stalls the first insert for seconds. A persisted
    tail pointer is not safe as is: page headers carry no owner, so a stale tail (catalog older than the last
    append, page freed and reused) could not be detected and inserts would land in another table's chain. Options:
    (a) owner table id in the page header (data format 3) + tail in the catalog (v8), verified on use;
    (b) persist the FSM itself at checkpoint as a compact run-length list. Also done here: on the first write the
    table's on-disk trees are adopted as lazy in-memory indexes before being dropped (`trees_adopt`), so the other
    indexes of the table keep answering from the tree plus an in-memory overlay instead of being rebuilt by a
    scan. ~~`tests/test_durability.c:1325` still fails for the FSM reason above.~~
  - **Fixed the same day (narrow form of option (a), no page-format change):** the safety argument needs only
    the root, not an owner id. A heap chain is never shortened in the middle: it is freed whole, together with
    its root (`free_heap`: DROP, and TRUNCATE / ALTER rewrite / VACUUM FULL, which all give the table a new
    root). So "last known page + the root it belongs to" is always a page of that chain while the table still
    has that root, at worst not the last one (`heap_insert` already follows `next` to the real end). Catalog
    v8 stores `tail_page` / `tail_root` per table (`cat_save` takes them from the free-space map or the unused
    hint; deltas v3, v2 / v1 records still read); at startup `tails_from_catalog` registers only hints whose
    root is still the table's; `free_heap` forgets the hint and the map of a freed chain; the hinted page must
    still be a chained data page. `heap_open` then builds a **partial** map from the tail (`meta_from_tail`),
    and every `INSERT` visits 16 more pages from the head of the chain (`meta_walk`) until the map is
    complete, after which space freed by `DELETE`s is reused again. Result: first `INSERT` after a restart reads
    **22 pages instead of 1 608**; `tests/test_durability.c:1325` passes (30 pages for the whole sequence). New
    tests (`tests/test_durability.c`, "P1.15: coda della catena"): first insert after restart < 60 pages on a
    half-deleted table, deleted space reused (size within +25%), TRUNCATE + restart (stale root ignored),
    390 appends then exit without shutdown + recovery (stale tail followed), counts stable across restarts.
    Catalog v8 is not readable by builds with catalog v7 (unreleased 1.5 development builds and 1.4.x).
    Still open in P1.15: the tree itself maintained by writes (overlay is in memory), composite / range scans
    from the disk tree.
  - **Review 2026 Update (v1.5.0):** first write after a restart no longer scans (FSM tail, catalog v8). Still open: the in-memory overlay of a table written since the last checkpoint grows with the changed keys (RSS), and after an unclean shutdown the trees are not trusted, so the first query on a multi-GB table rebuilds its indexes by a full scan — the "first page is slow after a crash" effect on big imported databases.

- [x] **P1.7 `[CONCURRENCY]` Table locks for plain reads cause upgrade deadlocks; waits are polled; writers can starve** — [code]
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
  - **Fixed:** a plain `SELECT` (also `SHOW` / `EXPLAIN`, not `FOR UPDATE` / `FOR SHARE` / `LOCK IN SHARE
    MODE`) inside a transaction releases at the end of the statement the table `S` locks it took
    (`lk_modes` before, `lk_release_reads` after; locks held before the statement, `X` locks and the global
    `*` lock stay): reads are READ COMMITTED (the committed version through the existing snapshot when
    another transaction writes the table), documented in `locks.h`. The lock manager (`src/bridge/locks.c`,
    rewritten) queues waiters FIFO with a ticket: a new request that is compatible with the holders still
    waits behind an earlier conflicting waiter (a lock the owner already holds, and an `S → X` conversion,
    never queue); the wait set is the whole request (no 32-entry truncation); deadlock detection walks the
    whole wait-for graph — conflicting holders and conflicting waiters ahead in the queue — with a visited set
    instead of a depth-limited DFS. Waits sleep on a condition variable (pthread with `CLOCK_MONOTONIC`, Windows
    `SleepConditionVariableSRW`) woken by every release; a 200 ms cap only re-checks the timeout and the cancel
    flag. The engine's wait hook is now a pair (`db_set_lock_wait_hooks(leave, enter)`): the server gives up its
    turn for the whole sleep instead of a 2 ms nap per retry; both 20 ms "enter a session" polling loops in the
    server sleep on `db_session_wait` (woken when a session leaves the engine). Tests in
    `tests/test_sessions.c`: two sessions `BEGIN; SELECT …; UPDATE …` both commit (B waits for A, no 1213); a
    writer behind 8 readers looping `BEGIN; SELECT … FOR SHARE; 15 ms; COMMIT` gets its lock in a few ms (limit
    2 s); a waiter that times out after 1 s uses < 60 ms of CPU. **Not done (longer term in the item):**
    row-level write locks; a per-transaction (REPEATABLE READ) snapshot — needs multi-version rows.

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

- [x] **P1.9 `[ENGINE]` Exact numerics: `DECIMAL` is computed as `double`, `BIGINT UNSIGNED` stops at 2⁶³−1** — [reproduced]
  - **Where:** `src/catalog/schema.c:9-19` (`DECIMAL` stored as text), `src/table/tuple.c:200-216`; arithmetic and
    aggregates in `src/query/executor.c`.
  - **Why:** `SUM` over `DECIMAL(20,2)` prints `1.23456789012346e+16`; `SUM(0.10, 0.20) = 0.30` is false;
    `18446744073709551615` is rejected for `BIGINT UNSIGNED`.
  - **Do:** fixed-point decimal type (sign + up to 65 digits, base 10⁹ limbs) for literals, columns, `+ - * / %`,
    `SUM` / `AVG` / `MIN` / `MAX`, comparisons and casts, with MySQL's scale rules; `uint64` path for unsigned
    columns; never print a `DECIMAL` in exponent form.
  - **Done when:** the three cases above match MySQL 8 output; a property test compares 10 000 random decimal
    expressions against Python's `decimal`.
  - **Done:** `src/query/decimal.c` — fixed-point decimal, base-10⁹ limbs (up to 81 digits), add / sub / mul /
    div (scale of the dividend + 4) / mod / round half away from zero / truncate / compare, never an exponent.
    The executor's value gains `is_dec` (exact text + a `double` for float contexts; every place that builds
    a value by hand sets it): `DECIMAL` columns, literals with a decimal point but no exponent and integer
    literals beyond 64 bits (the parser keeps their text) are decimals; `+ - * / %` with decimal and integer
    operands stay exact (with a `double` operand they are `double`, as in MySQL); comparisons, `GROUP BY` /
    `DISTINCT` keys (1.50 = 1.5) and hashes are exact; `SUM` / `AVG` (also as window functions) keep an exact
    accumulator (`AVG` scale + 4); `ROUND` / `TRUNCATE` of a decimal are exact (`ROUND(150.000, 2)` = 150.00,
    an integer stays an integer, D at most 30); exact results are typed `DECIMAL`, so the MySQL protocol sends
    `NEWDECIMAL` text instead of a `double`. `BIGINT UNSIGNED`: values up to 2⁶⁴ − 1 are accepted, kept in the 64
    bits and read back without sign (`ev_from_col`), negatives refused also on the fast integer insert path.
    The three cases: `SUM` over `DECIMAL(20,2)` = `12345678901234567.90`, `0.10 + 0.20 = 0.30` → 1,
    `18446744073709551615` stored and returned. Property test `tests/test_decimal.c` + `tests/check_decimal.py`
    (in `make test`, and 3000 under ASan in `make test-asan`): 10 000 random `+ - * /`, `ROUND`, `<` and
    `SUM` over a `DECIMAL(30,8)` column, all equal to Python's `decimal` with MySQL's rules. Test expectations
    changed to MySQL's (`9.0`, `18.20`, `8100.00`, `2.500000000000000000000000000000`, `DECIMAL` type of `a * 1.5`).
    **Deliberate difference:** integer / integer and `AVG` of integers stay `double` (`7 / 2` = 3.5; MySQL
    prints 3.5000), so existing results do not all change; `DECIMAL` without a scale keeps the digits it was
    given (MySQL would round to `DECIMAL(10,0)`). **Not done:** an index lookup by a literal above 2⁶³ on a
    `BIGINT UNSIGNED` column finds nothing (the index keys use the signed bits).

- [x] **P1.10 `[PERF]` Buffer pool: fixed 4 MB, one latch held across disk I/O, three syscalls per new page**
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
    Then (2026-11-15): the pool is split into up to 16 partitions by page id (`BpPart`: own latch, frames, hash,
    CLOCK hand, statistics; `bp_stats` sums them), a missing page is read from disk **outside** the latch (frame
    marked `loading`, pinned; others asking for that page wait for it alone), dirty write-backs go through one
    `wmx` (the WAL's `before_write` is not reentrant), the flush sorts with an explicit `DirtyRef` array (no
    global comparator context). Measured: 8 readers doing full scans of a 65 MB table with a 16 MB cache went
    from 1.8× to 4.1× over one reader.
  - **Done (2026-11-16):** evicted page buffers are kept per partition and reused for the next read (`spare`):
    a `malloc` / `free` of 4 KB per page read made glibc hand memory back and fault it in again. Measured on a
    57 MB table with a 16 MB cache, engine just opened (`/tmp`, 16 CPUs): 8 readers do 6.1-7.3× the work of
    one; with the table in cache 7×. **Tried and dropped:** a shared-latch fetch path with atomic pins — with
    a cache smaller than the table almost every access is a miss, and the read-then-write latch upgrade cut
    the 8-reader speed-up to 2.7×; with a hit-only workload it changed nothing. Test in `tests/test_parallel.c`
    (engine reopened, `min(8, CPUs)` readers, ≥ 2.5× with ≥ 8 CPUs outside CI: today 3-3.9× there, because the
    single reader is faster once the allocator is warm). **What limits scaling now is not the page cache:** every
    scan copies each qualifying row into an arena (P1.14), so 8 threads fault in and unmap tens of MB each and
    meet on the process memory map. **Not done:** 64-page file extension (tried caching the file end: overflow
    pages extend the file too, so it reused pages — reverted); bulk-insert throughput not compared with 1.2.

- [x] **P1.11 `[ENGINE]` Catalog: 1.71 GB fixed struct, raw struct dumps on disk, full rewrite + 2 `fsync`s per DDL**
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
  - **Partly done (2026-11-16):** no more fixed arrays: databases, tables (`TableSchema**`, one allocation per table,
    so a pointer stays valid when others are added or dropped), views, routines and triggers are growable lists;
    table columns live outside the struct (`schema_new` / `schema_dup` / `schema_assign`; executor work schemas
    keep them in the same block); `SV_MAX_COLUMNS` 1024, no table / database / routine / trigger limit; the
    parser's column lists (CREATE TABLE, SELECT list, INSERT column list) and the planner's output columns and
    USING map are dynamic too (`SelectPlan` no longer carries 1024-entry arrays). Catalog **format v6**: every
    field written explicitly (fixed-width little-endian integers, length-prefixed strings), independent of
    struct layout; v1-v5 files are still read and rewritten as v6.
  - **Done (2026-11-16, second part):** DDL goes through the WAL. `catalog_delta` compares every database list,
    per-database object list (tables, views, routines, triggers) and table with a 64-bit fingerprint of its last
    logged form and returns only what changed, each object whole; the statement writes it as one `LOG_CATALOG`
    record (tx 0) and syncs the log like an autocommit write (`synchronous`: FULL at once, NORMAL within 100 ms,
    the record is already in the file so a process crash does not lose it). Recovery replays the records in order
    on the catalog read from the file and saves it before removing the log; a checkpoint saves the whole catalog
    and drops them. A dropped table's pages are freed once the record is durable. Found and fixed on the way:
    the first page of a new table (and of an `ALTER` rewrite) was never logged — the old full flush hid it — now a
    `LOG_LINK` with previous page 0; redo of a `LOG_LINK` on an older page left on disk (freed and reused) now
    starts it empty instead of keeping the rows of its previous life; `TRUNCATE` deleted every row without a log
    record (rows came back after a crash) — it now gives the table a new empty root page and frees the old chain
    like `DROP TABLE`; `INSERT` still had 64-column arrays on the stack (a 300-column insert overflowed them:
    found by the new test under ASan); keys and constraints are refused on columns beyond the 256th (their
    positions are bytes in the catalog) instead of being truncated. Indexes and constraints of a catalog table
    are growable allocations too (`schema_add_index` / `schema_add_constraint`): `TableSchema` is 448 bytes
    instead of 29.7 KB. Tests (`tests/test_durability.c`): power cut at every write of a 14-statement DDL + DML
    workload (CREATE / ALTER / CREATE INDEX / VIEW / RENAME / CREATE and DROP DATABASE / qualified CREATE /
    DROP / TRUNCATE) — the schema after recovery is always the one after the confirmed statements, plus a second
    restart; a crash in NORMAL mode with 22 DDL statements only in the log; `CREATE TABLE` with 300 columns and
    a row in it; 2 000 tables in one database; 1 000 `CREATE TABLE` in ~290 ms (limit 2 s; before: 1.6 s on
    tmpfs, 4.4 s on an NVMe btrfs disk); engine start with 2 001 tables +0.7 MB resident (limit 20 MB).
    **Not done (deliberately):** the catalog is not stored in system tables inside the page file; DDL is
    still not transactional (an implicit commit, as in MySQL).

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

- [x] **P1.13 `[ENGINE]` Optimizer estimates are fixed constants; there are no column statistics** — [code]
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
  - **Done:** `ANALYZE TABLE` collects per column the distinct count, NULL fraction and, for numeric columns,
    min / max and a 16-bucket equi-depth histogram (`src/query/stats.c`, collected with internal queries in
    `analyze_columns`), kept in memory and in `DATA/segvault.stats` (text, written atomically, a damaged file
    means "no statistics"; the catalog itself is P1.11). Estimates (`cond_selectivity`) use them: `col = v` from
    the histogram (a frequent value spans whole buckets, so skew shows), ranges by interpolation in the
    buckets, `IS NULL` from the NULL fraction, equi-joins `|R|·|S| / max(distinct)`; without statistics the
    System R defaults as before. Index or scan: an equality lookup on a non-unique index counts the key's rows
    in the in-memory index first and scans instead when they are over a quarter of the table (the same rule
    as ranges), and `EXPLAIN` shows that choice and the real count. Join order: an inner-only join of 3-8
    base tables (no `USING` / `NATURAL` / `LATERAL` / derived tables / CTEs) moves its `ON` conditions into
    `WHERE` and runs in a greedy cost order (smallest filtered table first, then the equi-joined table giving
    the smallest intermediate result; ties by name), so every way of writing it gives the same plan; `SELECT *`
    is expanded first in the written order, so result columns and their names do not change. `num_rows` is
    exact after `ROLLBACK` / `ROLLBACK TO` (each undone insert / delete counts back; before, rolled-back rows
    stayed counted until the next `ANALYZE`, also across restarts). `EXPLAIN [ANALYZE] FORMAT=JSON` returns the
    plan as one JSON row. Found on the way: `EXPLAIN` applied the selectivity of a range or key condition
    twice when the index had already counted it. Tests in `tests/test_sql.c`: skewed table → scan for the
    common value, index for a rare one; histogram estimate 714 vs 715 real; a 3-table join written three
    ways → the same plan; `EXPLAIN ANALYZE` estimates within 2× at every step (125 / 100, 2500 / 2000);
    `SELECT *` columns after the reorder; exact row count after a rolled-back insert + delete; JSON plan.
    **Not done:** statistics are not refreshed automatically (only by `ANALYZE`); string columns get distinct
    count and NULL fraction but no histogram; joins with outer joins keep the written order.

- [x] **P1.16 `[PERF]` Profiling round 2026-11-24: rollback, parse-node size, join reordering** — [reproduced]
  - **ROLLBACK rebuilt every in-memory index of the tables it touched.** `invalidate_after_cache_undo` called
    `executor_invalidate_table`; the next statement re-read the whole table to rebuild the primary-key index
    (gprof: 60% in `sm_slot` / `mi_add_key` for `BEGIN; INSERT 10 rows; ROLLBACK`). Bench `rollback`
    **1.3 k → 48 k rollbacks/s** (SQLite 89 k; was 0.014×, now 0.55×). Fix: index journal in the executor
    (`ij_note` / `executor_index_undo` / `executor_index_journal_drop`): every key added / removed inside a
    transaction is recorded with db_api's change sequence and undone in reverse; an index built after the
    point rolled back to (it contains undone rows with no journal) is discarded; OOM → old behaviour for that
    transaction. The transaction's starting sequence is saved per session (`DbSession.tx_begin_seq`).
    Tests: `tests/test_sql.c` (INSERT / DELETE / key-changing UPDATE rolled back, `ROLLBACK TO`, a statement
    failing half way, index first built inside the transaction), `tests/test_sessions.c` (two sessions; the test
    fails if the begin sequence is not per session).
  - **`ASTNode` 66 KB → 3.3 KB** (see P3.12): inline 64 KB view / routine text, 36 KB `create_table`, 19 KB
    `merge`. Bench vs baseline: `point_lookup` +42%, `point_lookup_prepared` +11–17%, `readers_1/4` +37–40%.
  - **Heap overflow in `join_reorder` (P1.13) — memory safety, found with ASan.** Reordering a `SELECT *` over
    joined tables expanded the star into up to 64 column nodes and `memcpy`d them into `select_stmt.columns`,
    whose capacity is the written list (8 for `*`): any reorderable `SELECT *` over more than 8 columns in total
    wrote past the heap block. Now a new list of the exact size replaces the old one (also lifts the 64-column
    cut-off, and the reorder now considers up to 16 tables with 64-bit masks: `long` is 32 bits on Windows).
    Test in `tests/test_sql.c` (3 tables × 7 columns after `ANALYZE`).
  - **Measured, not changed:** `range_scan` 0.3× SQLite — removing the per-row re-check of exact integer bounds
    gave nothing (gprof's per-call overhead had inflated `eval_expr`); the cost is fetching each row by RID and
    materialising every cell as a string in `QueryResult` (P1.14 territory). `COUNT(*)` after a restart scans
    the table (1.7 ms for 100 k rows): `num_rows` cannot answer it (it counts other sessions' uncommitted rows
    and is not logged for recovery). Server idle footprint: 10 MB RSS, 1 thread, `/health` in 0.4 ms.

- [~] **P1.14 `[PERF]` The executor materialises every row it reads; results are fully built before they are sent**
  - **Where:** `src/query/executor.c` `plan_load` (~`:7024-7070`): the scan path copies every row that passes the
    `WHERE` into the source's arena (`source_push`), all of them, before joins, aggregation or `LIMIT` run;
    `query_exec` → `QueryResult` with every cell as a string (`db_api.h`), then the MySQL / HTTP layers encode it.
  - **Why (measured 2026-11-16):** `SELECT COUNT(*), SUM(v) FROM big WHERE pad <> 'x'` over a 57 MB table copies
    ~57 MB per execution: a freshly started process spends half of the scan in page faults of that arena, 8
    parallel scans meet on the process memory map (≈ 3.8× instead of ~7× with the pages cached), memory grows
    with the table (a `SELECT` over a 2 GB table needs > 2 GB), `LIMIT 10` on a big table still reads and copies
    every row, and P2.8's "stream a 2 GB result with flat memory" cannot be done. `make bench` (2026-11-16):
    `scan_aggregate` is 15 % slower relative to SQLite than in 1.3 (39 vs 46 queries/s); the same number was
    measured at the start of the 1.4 audit session (`e9c4ee6`), so it comes from the earlier 1.4 work (page
    format 2, decimals, statistics) — cause not isolated yet; check it first with this item.
  - **Do:** pull-based operators (open / next / close) for scan, filter, project, nested-loop and hash join,
    aggregate, sort, limit; copy only the columns a later operator uses (column pruning); aggregates and
    `COUNT(*)` consume rows straight from the page view; `LIMIT` without `ORDER BY` stops the scan; a cursor
    API in `db_api.h` (`db_query_open` / `db_query_next` / `db_query_close`) used by the MySQL protocol, the
    HTTP API and the CSV / SQL export; keep `QueryResult` as a convenience wrapper over the cursor.
  - **Done when:** that `COUNT(*)` allocates O(1) memory; `SELECT * FROM t LIMIT 10` on a 1 GB table reads
    one page chain prefix; `SELECT *` on a 2 GB table streams through PDO with flat RSS; 8 readers ≥ 6× on
    the P1.10 test with a warm allocator.
  - **Partly done (2026-11-16):** (1) *column pruning* — the binder counts the references to every column of a
    source (`Source.needc`, also for `*` expansion, `USING` / `NATURAL`, correlated references from
    subqueries, index probe and range columns); the scan subtracts the references inside the read-time filters,
    which are already evaluated on the page view, and copies into the arena only the text columns still used
    later (the others become NULL in the copied row). (2) *early stop* — a single-table query with
    `LIMIT` and no `ORDER BY`, grouping, `DISTINCT`, window functions, `SQL_CALC_FOUND_ROWS`, post-read
    conditions or correlation stops reading at `OFFSET + LIMIT` rows. Measured on the 57 MB table:
    `SELECT COUNT(*) … WHERE pad <> 'x'` 47 → 20 ms, `SELECT * … LIMIT 10` 56 → 8 ms,
    `… WHERE v = 5 LIMIT 3` 11 → 0.02 ms; P1.10 scans: one reader 246 → 145 ms, 8 readers 316 → 250 ms for
    32 scans. Tests: a block in `tests/test_sql.c` (text columns used only in `WHERE`, in `ORDER BY`, `GROUP BY` /
    `HAVING`, correlated subqueries, `USING` / `NATURAL`, `*`, window partitions, `EXISTS`, `LIMIT` / `OFFSET`);
    differential fuzzing against SQLite (280 000 queries, 0 different).
  - **Streaming results (2026-11-16):** a top-level `SELECT` on one table without `ORDER BY`, grouping,
    `DISTINCT`, window functions, `LIMIT` or correlation runs in blocks of 2 048 rows read (`Source.bscan`
    keeps the heap scan open between blocks, `scan_rows` is the scan loop shared with the normal path); every
    block goes through the same pipeline and is handed to a sink (`db_execute_stream`, `DbRowSink`), then
    freed. The MySQL text protocol uses it: column definitions with the first block, one packet per row,
    packets beyond 4 MB moved to an unlinked temporary file (`mw_spill`, sent by `my_send` before the rest);
    an error after rows becomes an `ERR` packet (allowed by the protocol). Measured in a new process on a
    129 MB table: peak memory **+23 MB** streamed instead of +425 MB. Tests: `tests/test_durability.c` §11
    (fresh process, 64 MB table: streamed peak below 32 MB and below half of the materialised one),
    `tests/test_server.c` (131 072 rows of 1 KB through the MySQL protocol, all received).
    `SELECT COUNT(*) FROM t [WHERE …]` (one table, no grouping, no index probe) counts during the scan on the
    page view without copying rows: +17 MB peak on the 129 MB table (the page cache), test in §11.
    `scan_aggregate` is back within 15 % of the 1.3 ratio (`make bench`: 39.9 vs SQLite 29.3).
    **Still open:** queries with `ORDER BY` / `GROUP BY` / joins / other aggregates still materialise
    (pull-based operators and an external sort are needed); the prepared-statement (binary) protocol, cursors
    and the HTTP API do not stream yet.
  - **Review 2026 Update (v1.5.0):** `make bench` 2026-11-24: `range_scan` 0.29–0.33× SQLite. A trial that skipped the per-row re-check of exact integer range bounds gained nothing; the cost is the RID-by-RID heap fetch and turning every cell into a string in `QueryResult`. The binary protocol (`COM_STMT_EXECUTE`, cursors) and the HTTP API still materialise. Schedule after P1.17: streaming per connection pays off once connections run concurrently.

- [ ] **P1.17 `[ENGINE]` One statement at a time server-wide: every MySQL-protocol command, and even the login, waits for any other running statement** — `[reproduced]` · **Size:** `XL` · **Deps:** `P1.18` (only for concurrent writers on one table)
  - **Where:** `src/server/server.c:4427-4430` (`COM_QUERY`, `COM_STMT_PREPARE`, `COM_STMT_EXECUTE`, `COM_INIT_DB` all go through `my_enter`), `:3812-3822` (`my_enter` → `exec_begin`), `:758-764` (`exec_begin` locks `exec_mx`, declared at `:168` as "una richiesta alla volta sul motore"), `:4302-4304` (login takes `exec_mx` around `db_session_new`); `src/bridge/db_api.c:81-82` (`db_lock` = exclusive `g_db_rw` for every statement), `:2784-2795` (`db_session_enter`: one active session `g_active`, ~20 globals swapped by `session_save` / `session_load`). The only parallel read path, `db_execute_readonly` (`db_api.c:2907`), is used by the HTTP API alone (`server.c:961`).
  - **Why:** reproduced on a 16-core machine with PHP 8.5.13 `mysqli` against `segvault-server --mysql-port 13306` (probe `/tmp/sv_audit_probe/q.php`): four concurrent CPU-bound `SELECT`s of 303 ms each completed at 0.30 / 0.61 / 0.92 / 1.22 s — fully serialized, and their *connect* alone waited 303–611 ms; four concurrent `SELECT SLEEP(1)` took 4 s; a `SELECT 1` sent 0.3 s into a 0.92 s `INSERT … SELECT` waited 621 ms to log in. A PHP site served by an 8-worker php-cgi pool therefore gets single-core database throughput, and one slow report query stalls every other page, new logins and the desktop editor (same engine lock, `db_api.c:81-82`). MariaDB/MySQL serve each connection in its own thread with row-level locking ("InnoDB employs Row-Level Locking", mariadb.com/kb/en/innodb-lock-modes/). The README comparison row "Letture che non aspettano le scritture (MVCC) ✅" (`README.md:153`) does not hold over the MySQL protocol.
  - **Do:** (1) route read-only statements of a session with no open transaction — and, after P1.19, every consistent read — through the shared-lock path (`db_execute_readonly`, extended to prepared statements and cursors) without `exec_mx`; (2) replace the single swapped active session (`g_active` + `session_save/load`) with an explicit `DbSession*` carried in `ExecCtx`, so several sessions are inside the engine at once; (3) split `g_db_rw` into finer latches: catalog rwlock (DDL exclusive), per-table latch for heap / index structure changes, the existing buffer-pool partitions (P1.10), a WAL append mutex; (4) login never waits for the engine (`db_session_new` only allocates). Rejected: a process per connection (buffer pool and lock manager in shared memory, cross-process latches — much harder on Windows); one engine thread fed by a queue (still serial CPU).
  - **Done when:** the probe (4 parallel 300 ms `SELECT`s) finishes in < 0.45 s on ≥ 4 cores and `SELECT 1` during a long write answers in < 5 ms; a new `tests/test_server.c` case with 8 concurrent MySQL connections; `make bench` gains wire scenarios (P3.14) with `mysql_readers_8` ≥ 5× `mysql_readers_1`; `make test-tsan` clean.

- [ ] **P1.18 `[ENGINE]` Write locks are per table: two transactions changing different rows of the same table serialize (1205 after the lock timeout)** — `[reproduced]` · **Size:** `XL` · **Deps:** `P1.17`
  - **Where:** `src/bridge/locks.c:65-70` (`LkEntry` keyed by database + table name), `:266` (`lk_acquire`), `src/bridge/db_api.c:1525-1536` (each statement acquires table locks, held to COMMIT); listed as a limit in `README.md:229-230`; P1.7 closed with "Longer term: row-level write locks".
  - **Why:** probe `/tmp/sv_audit_probe/eng.c` (two in-process sessions, `db_set_lock_wait_timeout(1000)`): A `BEGIN; UPDATE t SET v = 1 WHERE id = 1`, then B `UPDATE t SET v = 2 WHERE id = 2` waited 1 000 ms and failed with "Lock wait timeout exceeded … (a table is in use by another session's open transaction)". Web workloads write different rows of the same hot table from many requests (PHP sessions in the database, carts, WooCommerce orders, Laravel's `jobs` table polled by queue workers). InnoDB "locks only the index record found" for a unique search (dev.mysql.com/doc/refman/8.4/en/innodb-transaction-isolation-levels.html).
  - **Do:** row locks in the lock manager: table-level intention modes (IS / IX / S / X) plus a per-table hash of row locks keyed by primary-key value (by RID for tables without a key — `UPDATE` can move a row, `LOG_UPDATE_MOVE`); `UPDATE` / `DELETE` / `SELECT … FOR UPDATE` lock the rows they touch after the `WHERE` probe; escalate to a table X lock above ~5 000 row locks per statement; deadlock detection reuses the wait-for graph (`locks.c:215`). No gap locks under READ COMMITTED (as InnoDB); a unique check against an uncommitted insert waits on that row's lock. Rejected: page locks (every insert hits the tail page); commit-time write-write conflict detection only (PHP code does not retry transactions).
  - **Done when:** `tests/test_sessions.c`: A and B update different rows of one table, both commit, B waits < 10 ms; same row → B waits until A commits; crossed rows → 1213; a duplicate key against an uncommitted insert waits for its outcome; bench `writers_4` on one table ≥ 3× `writers_1` once P1.17 lands.

- [ ] **P1.19 `[ENGINE]` Isolation level is misreported (`@@transaction_isolation` = SERIALIZABLE) and `SET … ISOLATION LEVEL` is silently ignored; transactions only get READ COMMITTED** — `[reproduced]` · **Size:** `L` · **Deps:** `P1.18` (locking reads)
  - **Where:** `src/query/executor.c:2779-2780` and `src/server/mysql_wire.c:379` (two hand-kept copies of the variable table, both `"transaction_isolation", "SERIALIZABLE"`); `src/query/parser.c:5014-5016` (every `SET` is accepted: "Si accettano tutte"); READ COMMITTED since P1.7 (plain `SELECT` releases its S locks at statement end; committed-version snapshot per statement).
  - **Why:** probe `eng.c`: `SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ` → OK; `SELECT @@transaction_isolation` → `SERIALIZABLE`; inside one transaction A reads `v = 0`, B commits `v = 9`, A's second read returns `9` (non-repeatable read). MySQL and MariaDB default to REPEATABLE READ ("The default isolation level for InnoDB is REPEATABLE READ", dev.mysql.com/doc/refman/8.4/en/innodb-transaction-isolation-levels.html; same on mariadb.com/kb/en/set-transaction/). Code written for MySQL (read-check-write in a transaction, reports reading several tables as one snapshot) silently gets weaker guarantees, and anything that inspects the variable is told the strongest level. The two variable tables already disagree (`innodb_version` exists only in `mysql_wire.c:383`; probe: `SELECT @@innodb_version` fails in-process).
  - **Do:** (1) now: one shared system-variable table; report `READ-COMMITTED`; `SET [SESSION | GLOBAL] TRANSACTION ISOLATION LEVEL` / `transaction_isolation = …` either takes effect or leaves a warning (`SHOW WARNINGS`); (2) REPEATABLE READ as snapshot isolation: take the committed-version snapshot at the transaction's first read and keep it to COMMIT (today `try_snapshot` is per statement); writes and `FOR UPDATE` read the latest committed row under row locks (InnoDB semantics); make it the default like MySQL once benchmarked. Rejected: true SERIALIZABLE with predicate / gap locks (cost; rarely used by MySQL applications).
  - **Done when:** the probe returns `0` twice under REPEATABLE READ and `0` then `9` under READ COMMITTED; `@@transaction_isolation` follows `SET`; cases in `tests/test_sessions.c` and, over the wire, `tests/test_mysql.c`.

- [ ] **P1.20 `[PERF]` No group commit: a `synchronous = FULL` COMMIT fsyncs the log while holding the engine's exclusive lock** — `[code]` · **Size:** `M` · **Deps:** `P1.17`
  - **Where:** `src/bridge/db_api.c:2596-2600` (`db_commit` holds `db_lock`, exclusive `g_db_rw`), `:2066-2070` (COMMIT record + `wal_flush`), `src/tx/wal.c:162-174` (`wal_flush` → one `sv_fsync` per commit).
  - **Why:** each committing session holds the whole engine for one fsync (bench `commit_full`: 1.2 k commits/s on NVMe, `make bench` 2026-11-24); on a laptop HDD, network share or encrypted volume (5–10 ms per fsync) that is 100–200 commits/s for the *whole server*, with every reader stalled meanwhile. Group commit (several transactions' records flushed by one fsync) is the standard remedy [not-verified: no MariaDB group-commit page fetched in this review].
  - **Do:** after appending the COMMIT record, release the engine lock and wait on "flushed LSN ≥ my LSN"; the first waiter fsyncs once for everything appended so far and wakes the others (leader / follower). The transaction's locks are released only after its LSN is durable (strict 2PL kept). Rejected: weakening FULL (`synchronous = NORMAL` already offers that trade-off).
  - **Done when:** a new bench scenario `commit_full_8` (8 sessions, FULL) reaches ≥ 4× `commit_full`; the crash-point tests of `tests/test_durability.c` pass with the shared flush.

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
    Tests in `tests/test_server.c` (MySQL protocol). Then: a `SET` that names an unknown system variable is
    answered with an OK carrying one warning, and `SHOW WARNINGS` returns `Warning | 1193 | Unknown system
    variable '…'` until the next statement (known families — `character_set_*`, `collation_*`, `sql_*`, `net_*`,
    `innodb_*`, `transaction_*`, `NAMES`, … — stay silent). Found on the way: `SET @@session.x = …` was taken for
    a user variable (the `@@` check looked at one `@` at a time) and sent to the engine; `SET [GLOBAL]
    synchronous` / `cache_size` and `SELECT @@synchronous` / `@@cache_size` / `@@foreign_key_checks` were
    answered by the protocol layer without effect or with an empty value — they now reach the engine. Tests in
    `tests/test_server.c`.

- [x] **P2.2 `[NET]` Server-side prepared statements re-parse on every execute and run reads at `PREPARE` time** — [code]
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
    `SETVAL` / user functions inert, no locks). Then: the lexer has a `?` token (`TOK_PARAM`) that the parser
    accepts only between `parser_params_begin` / `_end`, which list every node made for a `?` (copies made while
    parsing included, freed ones removed); `DbPrepared` (`db_prepared_new` / `_exec`) keeps tokens + tree of a
    single `SELECT` / `INSERT` / `UPDATE` / `DELETE` and at each execute writes the typed values into those
    nodes (`EXPR_INT` / `FLOAT` / `STRING` / `NULL`, strings unescaped by the lexer exactly as in the text);
    `COM_STMT_EXECUTE` builds it at the first execute and uses it (the text with the values is still made for
    the log and the audit). The tree is re-parsed when `g_schema_gen` changed — bumped by every statement that
    is not a row read / write and by `ROLLBACK` — or after an engine restart. Everything else falls back to the
    text with the values: other statement kinds, a `?` that did not become a node, and a `?` that the executor
    re-parses from a stored text fragment (multi-table `UPDATE` / `DELETE` conditions: recognised by its error,
    the statement is then marked text-only). Engine counters `db_prepared_counts`. Tests in
    `tests/test_server.c`: three executes = one parse + three tree runs; typed values with quotes, `\\`,
    comment markers, `?`, `NULL`, negatives, `INT64_MAX`; `DROP` + `CREATE` with other columns between two
    executes → one re-parse, right value; prepared `NEXTVAL` advances once; a multi-table `UPDATE` with `?`
    falls back and works; 3000 prepared lookups checked. **Measured:** in the engine a point lookup goes from
    5.11 to 2.53 µs (2.02×, in-process probe); over the MySQL socket 40.1 → 37.7 µs (1.06×), because one
    loopback round trip through the thread-per-connection server costs ~35 µs — that cost is P2.5. Found on
    the way: the test client sent header and payload in two `send` calls without `TCP_NODELAY`, so every
    query of `tests/test_server.c` waited 40 ms for the delayed ACK. **Not done:** SQL-level `PREPARE` /
    `EXECUTE` and the HTTP API's `params` still splice values into the text (rare in practice; their parse is
    a small part of the request).

- [x] **P2.3 `[NET]` HTTP request limits truncate silently** — [code]
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
    Then: the header limit is configurable (`--max-header-size KB`, default 64 KB, minimum 4 KB; a header block
    that arrives in one piece but is over the limit also gets 431 — before, only one that kept growing did).
    Test in `tests/test_web.c` (9 KB of headers with an 8 KB limit → 431). Fixed copies stay: each field now
    has room for its whole limit, so nothing is cut short any more and a pointer + length rewrite would change
    nothing a client can see.

- [x] **P2.4 `[NET]` No overall request deadline; bodies are buffered whole, allocated before they arrive** — [code]
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
    longer to the declared `Content-Length` up front. Then: a body that arrives slower than 1 KB/s after the first
    10 s closes the connection (`MIN_BODY_RATE`); non-local addresses get at most 32 connections at once
    (`--max-per-address`, loopback and the Unix socket are exempt); `--max-body` (default now 128 MB) also sets
    PHP's `post_max_size` / `upload_max_filesize` through `-d`, so there is one upload limit; a body over 1 MB
    is written to an already-unlinked temporary file as it arrives (`spool_body`, `mkostemp` + `O_CLOEXEC`, on
    Windows `FILE_FLAG_DELETE_ON_CLOSE`) and FastCGI, CGI, the proxy and the API read it from a read-only
    mapping, so the server's own memory stays flat. Tests: `tests/test_web.c` (slow body closed; a 5 MB body to
    a CGI program arrives whole and a pipelined request after it is answered), `tests/test_php.c` (a 100 MB
    multipart upload reaches the real `php-cgi` with the default configuration while the process's anonymous
    memory grows by less than 40 MB). **Not done:** chunked request bodies are still collected in memory (up to
    `--max-body`); the per-address cap is not tested (the test client is always on loopback).
    Found on the way: the fake `php-cgi` of `tests/test_php.c` recognised only `-b ADDR` as its whole command
    line; once the server added `-d` options, every child re-ran the whole suite and started more children (a
    fork bomb that exhausted the memory of the machine). It now finds `-b` as the second-to-last argument and
    refuses to recurse (`SV_TEST_PHP_CHILD`).

- [x] **P2.5 `[NET]` Connection architecture: one thread per connection, TLS and HTTP/2 through loopback sockets**
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
  - **Fixed:** the TLS bridge and every HTTP/2 stream reach the HTTP server **in process**: the server registers
    a connector for its port (`sv_inner_register`, `src/server/h2.h`) that makes a `socketpair` and hands the
    server end to `serve_connection` — no TCP connect / accept, no connection slot, no loopback port. A TLS
    HTTP/1.1 connection (long-lived keep-alive) keeps its own thread; HTTP/2 streams (one request each) go to a
    fixed pool of 24 workers with an 8 192-entry queue (queue full → that stream gets 503). `select` is gone
    from every per-connection loop (h2 event loop, TLS bridge, TLS writes, reverse proxy, WebSocket pipe, SMTP):
    `sv_poll` (poll / WSAPoll) has no `FD_SETSIZE` limit. Measured with 50 curl clients × 32 concurrent HTTP/2
    streams × 400 requests (curl stands in for h2load, not installed): 20 000 × 200, 0.43 s, at most 75 threads;
    the v1.3.0 server on the same load answered 1 538 × 503, 17 985 failed and then aborted (fortify
    `FD_SET` overflow past descriptor 1024). The same load is now a check in `tests/test_tls.c` (all 200, < 100
    threads, `sv_process_threads`). **Not done:** plain HTTP/1.1 and MySQL connections still have one thread
    each (the accept loop itself uses `select` on its ≤ 7 listening sockets, which is safe); epoll / IOCP
    readiness for idle keep-alive connections; Windows has no `socketpair`, so there the bridge still uses a
    loopback TCP connection (through the same pool-less path as before).

- [x] **P2.6 `[SECURITY]` Credentials: slow hash under the engine lock, fast unsalted copy on disk, world-readable files**
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
    `tests/test_durability.c`). Then: hashes are PBKDF2-HMAC-SHA256 with the iteration count stored in the hash
    (`$p2$100000$<hex>`; `auth_pbkdf2_sha256` checked against the RFC 7914 vectors); an old-format hash is
    still accepted and redone on the next successful login (`auth_password_verified`, under the lock, only if
    salt and hash have not changed meanwhile). MySQL protocol: `caching_sha2_password` — fast path from
    `SHA256(SHA256(pw))` kept **only in memory** (filled by any successful password check, emptied by a
    restart, like MySQL), full path with the password in clear text only over TLS or the Unix socket,
    otherwise encrypted with the server's RSA key (generated in memory at first use through the OpenSSL that
    TLS already loads; OAEP); `mysql_native_password` is used only for users created `IDENTIFIED WITH
    mysql_native_password` (the unsalted `SHA1(SHA1(pw))` is no longer written for everyone; existing users
    keep theirs until their password is changed); `COM_CHANGE_USER` goes through the same code. Back-off:
    after 5 wrong passwords in a row for one user from one address each new attempt waits 1, 2, 4 … s, up to
    5 minutes, refused without computing the hash (HTTP 429 + `Retry-After`, MySQL 1045 with the wait); a
    success clears it. Windows: the data folder gets a protected ACL (current user + SYSTEM, inherited by new
    files) and existing engine files and every `sv_fopen_private` file get the same (`src/platform/win_acl.c`).
    Tests: `tests/test_auth.c` (PBKDF2 vectors, file format, old hash accepted and upgraded, `IDENTIFIED WITH`
    plugins), `tests/test_server.c` (after a restart: sha2 over TCP asks for full auth, full auth in clear text
    over the Unix socket, then fast auth over TCP, wrong password, native client switched to sha2, native user
    with no switch, root token both ways, **real PHP `mysqli` logging in through the RSA key over TCP**,
    back-off), `tests/test_win.c` (ACL as SDDL on native Windows; wine keeps no ACLs, so it is skipped there).
    Found on the way: after a full authentication the server also sent "fast auth OK" (0x03), which PHP
    rejects — only seen with a real client. **Not verified:** the Windows ACL on real Windows (wine rebuilds
    ACLs from Unix permissions; the check runs in the `windows-native` CI job).

- [x] **P2.7 `[SECURITY]` Secrets and control characters reach the logs**
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
    Then: what is a read is decided by the parser (`db_statement_is_read`: every statement a `SELECT` without
    `INTO`, `SHOW` or `DESCRIBE`, no `@` assignment, no `NEXTVAL` / `SETVAL`, no user function call), so
    `SELECT f()` with a writing `f` and `SELECT … INTO @v` are audited, and failed reads too (denied attempts);
    the audit file is `fsync`ed at most one second after its last line, from the accept loop and only when
    `exec_mx` is free (`audit_sync_tick`). Tests in `tests/test_server.c`.

- [x] **P2.8 `[NET]` MySQL protocol completeness**
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
    configuration. Tests in `tests/test_server.c`. Then: column definitions carry `NOT_NULL`, `PRI_KEY`,
    `UNIQUE_KEY`, `MULTIPLE_KEY`, `UNSIGNED` and `AUTO_INCREMENT` from the catalog (`db_column_flags`, for
    columns the result maps to a base table; computed columns get none), in text and binary results and in
    `COM_STMT_PREPARE`; `COM_STMT_EXECUTE` with a cursor type answers with the columns and an EOF carrying
    `SERVER_STATUS_CURSOR_EXISTS`, and `COM_STMT_FETCH` sends the next N rows in binary form, then
    `LAST_ROW_SENT` (error 1421 without an open cursor; `COM_STMT_RESET`, a new `EXECUTE` and `CLOSE` drop it).
    Tests in `tests/test_server.c` (flags of a 5-column result; a cursor read 2 rows, then the rest).
    **Moved to P1.14:** streaming result sets with flat memory — the cursor serves a materialised `QueryResult`;
    streaming needs the executor to hand rows out as it produces them. Everything else in this item is done.

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

- [x] **P2.10 `[SECURITY]` Child processes: PHP FastCGI port open to every local user; handles leak into children**
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
    `_O_NOINHERIT`. Then: on POSIX every engine file is opened with `O_CLOEXEC` (`sv_open`), every `fopen` with
    a literal mode gets glibc's `"e"` (`SV_FE`, 41 sites), every socket is created with `SOCK_CLOEXEC` and
    accepted sockets get `FD_CLOEXEC` — so a child started by any path, not only the three that close
    descriptors, inherits nothing; on Windows CGI programs are started with `STARTUPINFOEX` +
    `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (only their stdin / stdout / stderr; FastCGI and the service already
    pass `bInheritHandles = FALSE`); `php-cgi` is no longer searched as `php/php-cgi` relative to the current
    folder, nor in relative or empty `PATH` entries. Test in `tests/test_web.c`: a CGI script lists
    `/proc/$$/fd` and sees exactly `0 1 2`. **Not verified:** the Windows handle list (cross-compiled only).

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

- [x] **P2.12 `[API]` HTTP API: unbounded results, 32-bit ids, no machine-readable metrics**
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
    `"last_insert_id":3000000000` (`tests/test_server.c`). Then: `"max_rows": N` on `/query` (and `?max_rows=` on
    saved endpoints) returns the first N rows with `"next"` + `"rows_left"`; the rest stays in a server-side
    cursor (owner = the request's user, 5 minutes idle, 32 per server, oldest dropped when full), read with
    `{"cursor": "…", "max_rows": N}` without touching the engine — one result computed once, so pages are
    consistent; `"big_ints": "string"` returns integers beyond ±(2⁵³ − 1) as strings; `GET /api/v1/metrics` in
    Prometheus text format 0.0.4: requests by service × status class, latency histogram per service,
    connections, rows sent, buffer-pool frames / used / requests / hits / misses / evictions, pages written,
    data-file and WAL size, WAL records, checkpoints, lock waits and deadlocks (new counters in `locks.c`),
    open transactions; the engine numbers also appear in the MySQL `SHOW STATUS` under the MySQL / MariaDB
    names. Tests in `tests/test_server.c`: 25 rows read in pages of 10/10/5 through the cursor, unknown
    cursor → 404, `rows_left`, no cursor without `max_rows`, big integers both ways, every line of `/metrics`
    checked against the text-format grammar with cumulative buckets and `+Inf` = `_count`, `SHOW STATUS`.
    **Not done:** `promtool check metrics` itself was not run (not installed here; installing it was not
    allowed); results are still materialised before the first page (the cursor bounds the response, not
    the server's memory — that needs the streaming executor noted under P2.8).

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

- [x] **P2.14 `[GUI]` Engine calls on the UI thread** — [code]
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
  - **Fixed:** every statement the window runs from an event handler (table preview, foreign-key navigation,
    grid edits and re-fetch, Designer apply, context-menu actions, import steps, dump, row counts, search and the
    database report in `db_tools.c` through `dbt_set_exec`) goes through `ui_exec`: the statement runs in a worker
    thread; after 150 ms a "Waiting for the database… Ns" box with Cancel appears and the window keeps
    repainting (only the box over the last frame — the handler's state is half-built, so nothing else is drawn),
    a resize is applied afterwards, closing the window cancels and quits after. Cancel / Esc sets the worker's
    thread cancel flag, which now also stops a **lock wait** (`acquire_locks` → "Query cancelled while waiting
    for a lock held by another session"; the same flag serves `KILL QUERY` on the server, which therefore
    also interrupts a waiting statement now). Found on the way: the prepared-statement path did not check
    the `LockSet` allocation. Test `tests/test_sessions.c`: another session holds a write lock, a statement
    with the thread flag set after 200 ms returns "cancelled" in ~200 ms instead of 30 s, the table is usable
    afterwards. **Not done:** a single queue/event model for all engine calls — catalog reads for the tree
    (`db_list_*`) still run on the UI thread (they take the engine lock briefly, never a table lock).

- [x] **P2.15 `[GUI]` Text input and display scaling** — [code]
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
  - **Fixed:** X11 opens an input method (`XOpenIM` / `XCreateIC`, `XFilterEvent`, focus forwarded with
    `XSetICFocus`) in the user's `LC_CTYPE` only for the call, then restores the locale (numbers keep printing
    with a dot); keys go through `Xutf8LookupString`, so dead keys, Compose and IMEs give UTF-8 text, decoded into
    code points (invalid sequences dropped — never Latin-1 bytes) and queued so a multi-character commit (CJK)
    arrives as consecutive `EVT_CHAR`s; without an IM the old `XLookupString` path stays. Scaling: one factor in
    the platform layer (`platform_set_ui_scale`, `platform_ui_scale`): widgets keep drawing in logical 96-dpi
    pixels, every primitive, clip, text size and bitmap is multiplied, text measurements / mouse / resize are
    divided back, so fonts are rendered at the real size (sharp) and every metric in the widgets scales without
    touching them. Automatic value: X11 `Xft.dpi` / 96 (or `SV_UI_SCALE`), Windows the monitor DPI with
    per-monitor-v2 awareness (`SetProcessDpiAwarenessContext`, resolved at run time, falling back to
    `SetProcessDpiAwareness` / `SetProcessDPIAware`) and `WM_DPICHANGED` when the window moves to another
    monitor; Settings → Appearance → "Display scale" (Automatic, 100-300 %) overrides it. Test
    `tests/test_platform.c` (in `make test`, skipped without an X display): synthetic key events for `è`, `ñ`,
    dead double-acute + `o` → `ő`, dead acute + `e` → `é`, a queued CJK commit, invalid UTF-8; at 200 %
    logical metrics stay within 1-2 px of 100 %, the physical font is twice as wide, a logical 10×10 rectangle
    covers 20×20 physical pixels, `platform_read_pixels` and mouse coordinates are logical.
    **Not verified:** the Windows side only cross-compiles (no high-DPI Windows run); a real IME (fcitx / ibus)
    was not exercised, only Xlib's built-in compose.

- [ ] **P2.16 `[NET]` 64 connections in total for HTTP + MySQL + SMTP: 64 idle MySQL clients take every website down** — `[reproduced]` · **Size:** `M` · **Deps:** `none` (the event-loop part fits P1.17)
  - **Where:** `src/server/server.c:5049` (`max_clients` defaults to 64, 256 with HTTPS), `:5076` (one `conns[]` table for every listener), `:4489-4512` (one thread per connection, 8 MB stack reservation each), `src/server/mysql_wire.c:383` (`@@max_connections` = that same budget).
  - **Why:** probe `/tmp/sv_audit_probe/conns.php` against `segvault-server --mysql-port 13306`: the 65th MySQL connection fails with `1040 Too many connections`, and while 64 MySQL connections are open `GET /api/v1/health` answers `{"ok":false,"error":{"message":"too many connections"}}` — every site on the stack is down too. 64 is easy to reach locally: php-cgi pool + Laravel Horizon / queue workers + `PDO::ATTR_PERSISTENT` + an IDE with several tabs + browser keep-alive. MariaDB's default `max_connections` is 151 [not-verified: value read from a summarised page, mariadb.com/kb/en/server-system-variables/#max_connections].
  - **Do:** separate budgets per listener (HTTP, MySQL, SMTP), MySQL default 151 like MariaDB; when the HTTP table is full, close the least recently used idle keep-alive connection instead of refusing the new request; `--max-connections` and a Settings field; idle connections should cost a small struct, not a thread with an 8 MB reservation — an epoll / IOCP loop parks idle sockets and hands ready requests to a worker pool (same shape as the HTTP/2 inner workers, P2.5). Rejected: only raising the constant (thread stacks and Windows commit charge grow with it).
  - **Done when:** the probe opens 200 MySQL connections while HTTP still answers 200 OK; a `tests/test_server.c` case; limits documented in `docs/API.md`.

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

- [x] **P3.2 `[TEST]` Sanitizer and platform coverage in CI**
  - **Where:** `Makefile:187-191` (`test-tsan` covers `test_parallel` and `test_php` only), `:267-321`
    (`test-asan` omits `test_mysql`, `test_web`, `test_mail`, `test_tls`); `.github/workflows/ci.yml` (one Ubuntu
    job; the MinGW build is compiled but never run).
  - **Do:** ASan + UBSan for every suite; TSan for `test_sessions`, `test_server`, `test_web` and the new
    parallel cases (P0.3); a `windows-latest` job that runs the suites natively; `macos-latest` for
    `segvault-server`; `-fanalyzer` and `cppcheck` as non-blocking jobs first; cache build objects.
  - **Done when:** CI has the five jobs and a deliberately injected race / overflow fails each relevant one.
  - **Done:** `.github/workflows/ci.yml` gains a `sanitizers` job (`make test-tsan`, the durability suite and
    `test_h2` under ASan + UBSan), the Windows-only code run under wine (`make test-win`), a `-DDEBUG` compile
    check and a non-blocking `cppcheck` job. Then: `make test-asan` covers every suite (added `test_mysql`,
    `test_web`, `test_mail`, `test_tls`, `test_h2`, `test_durability` with fault injection) and runs in the
    main CI job; `make test-tsan` adds `test_sessions`, `test_server` and `test_web` (0 TSan reports; one
    `test_web` check compared the process name and failed only because of the `_tsan` suffix); CI gains a
    `windows-native` job (MSYS2 UCRT64: `make test-win WIN_CC=gcc WIN_RUN=` runs the Windows-only suite
    natively, then the native build) and a `macos` job (`segvault-server` + engine suites; the ELF-only
    `-z relro` flags are skipped on Darwin). Object caching is not needed any more: the engine is compiled once
    into `libsegvault.a` (P3.4). **Not verified:** the two new jobs have not run on GitHub's runners yet
    (no push), so they are `continue-on-error` until they have.

- [x] **P3.3 `[TEST]` Fuzzing of every parser that reads untrusted bytes**
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
    Runs: millions of inputs per target, 5-minute runs of `exec` and `sql` clean. Then, new targets: `http`
    (the real server started in-process with API, static files and a `.htaccess`; each input is a connection
    closed for writing, so `parse_head`, `read_chunked`, routing and the rewrite run as for a client), `h2`
    (frames + HPACK through an `H2Io` on a socketpair, streams going to that server), `smtp` (a whole session,
    then the mail panel's message / HTML / raw / part pages: the MIME reader), `htaccess` (mutated rules on
    fixed requests through `rw_apply`), `regex`, `sqljson`, and `diff`: random SQL over the common subset
    (arithmetic, `CASE`, `COALESCE`, `NULLIF`, comparisons, `BETWEEN`, `IN` with `NULL`, `EXISTS`, `GROUP BY`
    aggregates) run on SegVault and on SQLite, results compared as multisets. Every target then ran **10
    minutes under ASan + UBSan**: two real bugs found and fixed — `RewriteRule … [S=-9]` (a negative skip moved
    the rule index before the array: heap read; now clamped to ≥ 0, test in `tests/test_php.c`) and
    `INTERVAL '1844674407370955161' SECOND` (signed overflow in `mydt_add`, undefined behaviour; now `NULL`, test
    in `tests/test_sql.c`); after the fixes all 13 targets ran 10 minutes clean (`diff`: 475 730 queries
    compared, 0 differences). New `.github/workflows/fuzz.yml`: every night each target for 10 minutes with
    a new seed, crash input / differences kept as artifacts. **Not done:** libFuzzer / coverage guidance (no
    clang here: the mutator is blind, so the corpus is the built-in seeds, not a cached corpus).

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

- [x] **P3.5 `[REPO]` Repository hygiene**
  - **Where:** `test_sessions_tsan` (2.1 MB ELF tracked at the root since `45ae544`, not in `.gitignore`, not in
    `make clean`); `bin/linux-x86_64/*`, `bin/windows-x86_64/*` (five executables re-committed per release;
    `.git` is already 65 MB); `test_engine.c` at the root while every other test is in `tests/`.
  - **Do:** `git rm` the stray binary and ignore `*_tsan`; publish executables only as release assets with
    `SHA256SUMS` (and a signature); keep `bin/README.md` pointing at the releases page; move `test_engine.c`
    to `tests/`.
  - **Done when:** a fresh clone contains no executable; the release workflow attaches assets and checksums.
  - **Done:** `test_sessions_tsan` deleted from the tree (the deletion still has to be committed), `*_tsan` in
    `.gitignore` and in `make clean`. Then: the five executables under `bin/linux-x86_64` and
    `bin/windows-x86_64` are no longer tracked (`git rm --cached`; they are attached to every release, which
    already had them as assets); `make dist` writes executables, packages and `SHA256SUMS` to `dist/`
    (ignored) instead of `bin/`; new `.github/workflows/release.yml` builds, tests and attaches `dist/*` to the
    release when a `v*` tag is pushed; README and `bin/README.md` link to
    `releases/latest/download/<file>` and explain `sha256sum -c`; `test_engine.c` moved to
    `tests/test_engine.c`. Old history still carries the binaries (rewriting history is not worth breaking
    every clone). **Not done:** a signature next to `SHA256SUMS` (needs a signing key the project does not have).

- [x] **P3.6 `[BENCH]` Reproducible benchmarks with a baseline**
  - **Where:** `bench/bench.c` (91 lines: one schema, in-process only), `Makefile:354-358`.
  - **Do:** scenarios — bulk load, point lookup, range scan, join, update-heavy, commit rate by `synchronous`
    mode, rollback cost (P1.2), cold start, 1-16 concurrent readers and writers, PDO over the MySQL protocol —
    each also run against SQLite (in-process) and MariaDB (protocol) with the same SQL; JSON output; a CI job
    that fails on a > 15 % regression against the stored baseline.
  - **Done when:** `make bench` prints a comparison table and the README's performance section is generated from it.
  - **Done:** `bench/suite.c` runs the same scenarios with the same SQL on SegVault (through `db_execute` /
    `db_prepared_exec`) and on SQLite (in-process, WAL, `synchronous` set per connection): bulk load, point
    lookup (text and prepared), range scan, join + `GROUP BY`, full-scan aggregate, update-heavy, commit rate
    with `synchronous` FULL / NORMAL / OFF, `ROLLBACK` cost, cold start, 1 / 4 / 16 concurrent readers and
    1 / 4 writers, and PHP PDO over the MySQL protocol against a `segvault-server` it starts; every value is
    "higher = better" in JSON. Data goes to `BENCH_DIR` (default `build-bench/`), never `/tmp`: on tmpfs
    `fsync` is free and the first run showed 226 000 "durable" commits/s. `bench/compare.py` (standard
    library only) keeps the best of 3 runs, prints the table, rewrites the README section between markers
    (`make bench-readme`), stores baselines (`make bench-baseline`: `bench/baseline.json`, `baseline-quick.json`)
    and fails on a regression: the gate compares SegVault / SQLite measured in the same run with the same
    ratio in the baseline, so a slower machine does not count as a regression (absolute-only scenarios such as
    PDO only warn). `make bench` = full run + check; `make bench-check` = quick run for CI (new `bench` job,
    uploads the JSON). Two quick checks in a row on the same PC passed (no false alarm). **Found:** a
    `ROLLBACK` costs ~5 ms on disk (186/s against SQLite's 90 000/s): two `fsync`s of the log plus a flush of
    every dirty page, because the undo works on the data file — added to P1.1, which gives the undo page LSNs
    to work in the buffer pool; a cold start costs ~14 ms (the 1.7 GB catalog struct, P1.11). **Not done:**
    MariaDB is not installed here (the column is ready: `mariadb` values are read and shown); the CI gate is
    `continue-on-error` until a baseline recorded on GitHub's runners is stored (no push yet).

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

- [x] **P3.8 `[DX]` Allocation-failure policy**
  - **Where:** unchecked results, e.g. `src/tx/wal.c:9`, `src/tx/transaction.c:5`, `src/table/tuple.c:8`,
    `src/bridge/db_api.c:1311-1387,1447-1461,1506-1518`, `src/server/server.c:793`.
  - **Do:** choose one rule — "engine allocations go through `sv_xalloc`, which reports and fails the statement
    via a `longjmp`-free error path" — and apply it; add an allocation-failure sweep to the harness (fail the
    N-th `malloc` for N = 1…k on the SQL test corpus).
  - **Done when:** the sweep completes with no crash and no leak under ASan.
  - **Fixed (rule chosen: out of memory = controlled stop):** `include/sv_alloc.h` — every engine allocation
    (397 call sites in `src/catalog`, `storage`, `table`, `index`, `query`, `tx`, `bridge`, plus `SV_ALLOC` /
    `SV_ALLOC_N` everywhere) goes through `sv_malloc` / `sv_calloc` / `sv_realloc` / `sv_strdup` /
    `sv_strndup`, which print "out of memory" and `abort()` instead of returning `NULL`; committed data is
    already in the log, so the next start recovers it. Sizes that come from the data keep a real error path
    with `sv_try_*`: the log reads of recovery and rollback (`wal.c`, 11 sites), the WAL buffer, the
    1.7 GB catalog (`catalog_create` / `catalog_load` → "out of memory" error at start). A `longjmp`-free
    error path for all 400 sites was rejected: unexecuted error branches are where corruption hides.
    Sweep in `tests/test_durability.c` (`-DSV_FAULT_INJECTION` → `sv_fi_alloc_fail`): a workload of start,
    30 inserts, update, `COMMIT`, open `DELETE` is run with the N-th allocation failing for ~400 N spread over
    its ~10 000 allocations (`SV_OOM_ALL=1`: every N; 1 658 points checked once, all clean); each child must end
    normally or by `SIGABRT` from the handler, never another signal, and after the restart the 600 committed
    rows are intact, the open `DELETE` is gone and the `UPDATE` is whole. Under ASan the suite runs in
    `make test-asan`. Rule documented in `CONTRIBUTING.md`.

- [~] **P3.9 `[AUDIT]` Finish the code audit of the modules that were only spot-checked**
  - **Pass 2026-11-24 (P3.11 / P3.12 work, ASan):** found and fixed the `join_reorder` heap overflow, the stored
    identifier injection in the GUI, the login-throttle bypass, the `.htaccess` fail-open paths and the HTTP/2
    header drop (see P3.11, P1.16). Grep of every unbounded `strcpy` / `strcat` / `sprintf` in `src/`: all
    bounded by an earlier length check or an allocation sized from the inputs, except `service.c`
    (`build_args`: an option with a value adds two entries but the loop reserved one — with exactly
    `max - 2` entries the terminating NULL went one past the array; fixed). Still to read line by line: the
    rest of `parser.c` / `executor.c`, `regex.c`, `sqljson.c`, widgets.
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
  - **Pass of 2026-11-16 (written as found):**
    - `tls.c` (read in full): **fixed** — the CA private key (`certs/ca.key`, trusted by the browser after
      `--trust-ca`) and `sites.key` were created by OpenSSL with the default umask and narrowed with `chmod`
      only after the key was written, and got no ACL on Windows; now the file is created private first
      (`sv_fopen_private`: 0600 / owner-only ACL), then written; a `certs` folder created by the server is
      0700 / owner-only. Test: `tests/test_tls.c` checks both keys. **Open (P3.10):** the local CA has no
      X.509 name constraints, so whoever reads `ca.key` can mint a certificate for any domain that the
      machine's browsers accept; leaf certificates are minted for any SNI name a client sends (256-entry cache:
      with `--bind` on a network address a remote client can fill it and new site names fall back to the
      `localhost` certificate).
    - `proxy.c` (read in full): **fixed** — a heap buffer overflow: a backend status line shorter than 8 bytes
      (`HTTP/1.\r\n`) made the precision of `"%.*s"` negative, so `sprintf` copied the rest of the 64 KB header
      buffer, body bytes included, into `out` (`hlen + 128` bytes). Confirmed under ASan; the status line must now
      be `HTTP/1.x NNN` (else 502) and the copy uses `snprintf`. Test: `/short-status` in `tests/test_web.c`.
      Checked and fine: chunked parser (15 hex digits, states), 1xx skipping, hop-by-hop headers, header size
      limit, WebSocket bridging. Noted, not changed: a response with both `Content-Length` and
      `Transfer-Encoding: chunked` is forwarded with both headers (the proxy itself follows chunked).
    - `rewrite.c` (read in full): **fixed** — (1) an out-of-bounds read: the text of the last matching
      `RewriteCond` was copied into a 4 KB buffer but its `%N` captures were computed on the whole text, so a
      capture beyond byte 4096 (e.g. `%{HTTP_HOST}%{REQUEST_URI}` or a long query string) copied stack memory
      into the rewritten URL or the `Location` of a redirect; the copy is now dynamic. (2) a rewritten path
      containing `..` (from a capture) was used for the next rewrite passes: `.htaccess` files and `-f` / `-d`
      checks outside the site root were read and applied (a test shows a parent folder's rule redirecting the
      request); now such a path ends rewriting at once and is refused by the usual checks (404). Tests in
      `tests/test_php.c` (both fail without the fix). Checked and fine: `S=` bounds, `<Files>` glob, nested
      blocks, flag parsing, 414 on overlong results, `MAX_PASSES`. **Noted (low):** `%{ENV:NAME}` exposes any
      environment variable of the server process to whoever writes a `.htaccess` (on a shared classroom
      server: any student site) — limit it to variables set by the site (`E=` flag, not implemented) or a
      fixed allow-list.
    - `json.c` (read in full): no defect found (depth limit 64, surrogate pairs, output never longer than the
      input, numbers by the JSON grammar).
    - `service.c` (read in full): **fixed** — `--install-service --token T` wrote the token into the systemd
      `ExecStart` line (unit file 0644) / the plist arguments, so it was on the service's command line, which
      every local account can read in `/proc/PID/cmdline`; now the token goes into the service environment
      (`Environment="SEGVAULT_TOKEN=…"` / plist `EnvironmentVariables`) and the unit / plist is 0600 (also
      when it already existed); plist strings are XML-escaped (a path with `&` broke the file). Checked by hand
      with `SEGVAULT_SERVICE_DRYRUN=1` + `systemd-analyze verify`. **Not changed:** the Windows Run key keeps
      `--token` on the command line (per-user key; other users need administrator rights to read a process's
      command line); `mkdir -p` / `launchctl` go through `system()` with paths from `$HOME` /
      `$XDG_CONFIG_HOME` in single quotes (only the user's own environment can break them).
    - `mail.c` (read in full): no defect found (line and message size limits, attachments served without a
      file name in the header, message HTML in a sandboxed iframe with a CSP).
    - `regex.c` (read in full): **fixed** — the pattern parser recursed once per `(` with no limit: one statement
      `SELECT 'a' REGEXP '((((…a))))'` with 100 000 parentheses crashed the whole server (stack overflow; any SQL
      user, also `.htaccess` patterns); nesting is now limited to 250. Test in `tests/test_sql.c`. Matching was
      already bounded (2·10⁶ steps, depth 4 000).
    - `sqljson.c` (read in full): **fixed** — the `LIKE` matcher of `JSON_SEARCH` recursed once per character
      (stack overflow with a 90 000-character value and pattern) and was exponential with several `%`
      (`'%a%a…%b'` on a short string never returned); now iterative with backtracking to the last `%` only.
      Tests in `tests/test_sql.c`. Every other recursive helper receives documents already checked by
      `sj_valid` (depth ≤ 128).
    - `executor.c` `like_match_esc` (the SQL `LIKE`): **fixed** — same two problems as `JSON_SEARCH`: one
      recursion per `%` (stack overflow with 60 000 `%`) and exponential time (`'%a%a…%b'`); now iterative.
      Tests in `tests/test_sql.c` (escape, `_`, case, empty strings).
    - Depth probes (one forked process per case, stacks of 1, 2, 4 and 8 MB): the parser refuses expressions
      deeper than 128 levels and over-nested derived tables; procedures, functions and triggers stop with
      "nested too deeply"; recursive CTEs stop at 1 000 steps / 10⁶ rows; views at 16 levels; `REPEAT` / `LPAD`
      stop at 16 MB. All hold with a 2 MB stack, but **with 1 MB a chain of nested views crashed the process**.
      **Fixed:** the server's connection threads and the HTTP/2 worker pool get an explicit 8 MB stack
      reservation (`SV_THREAD_STACK`; Windows took the executable's default of 1-2 MB, Linux `ulimit -s`).
      **Noted (usability):** a flat `a + b + c + …` of more than 128 terms is refused as "too complex" (the
      AST is a left-deep tree evaluated recursively) — generated SQL summing many columns can hit it.
    - Executor write paths (probes with a 1 MB stack): an `ON DELETE / ON UPDATE CASCADE` chain of 20 000
      self-referencing rows is deleted in 24 ms without recursion problems; two triggers inserting into each
      other's table stop with "nested too deeply"; recursive CTEs, procedures and functions are bounded.
    - `win32.c` (process launching, clipboard and text conversion paths read; the rest of the file not yet):
      URLs and files are opened only for `https://` / local server addresses and existing regular files.
      **Fixed:** a window title over 255 UTF-16 units left the converted buffer without a terminator; the
      text-drawing buffer did not check `realloc`; a clipboard conversion returning 0 led to reading an
      uninitialised byte. Linux `xlib.c` launch paths (`xdg-open`, zenity / kdialog via `execv`, descriptors
      closed) are fine.
    - `executor.c` sequences (`sequences.txt`): **fixed** — the file was rewritten with `remove` + `rename` and no
      `fsync` (a power cut in between lost it: every sequence restarted from its start, giving values already
      used), write errors were ignored, and a name with a space (`` `my seq` ``) was not read back. Now: temporary
      file written, checked and synced, atomic replace; names %-encoded; `NEXTVAL` saves a reserve of 32 values
      ahead (one `fsync` per 32 calls, like PostgreSQL's sequence cache) so a value is never given twice after a
      crash, and fails with an error if the state cannot be saved; a clean shutdown saves the exact values.
      Tests in `tests/test_durability.c` §10b (100 `NEXTVAL` ≤ 8 `fsync`s, exact continuation after a clean
      shutdown, no repeat after a crash, name with a space).
    - `executor.c` long waits: **fixed** — `SLEEP(n)` slept up to 60 s per call without looking at `KILL QUERY` or
      `--timeout` (`SELECT SLEEP(60) FROM t` held the engine for hours); it now sleeps in 50 ms slices and stops
      (error "Query cancelled") when the statement is cancelled. Test: the `KILL QUERY` block of
      `tests/test_server.c` kills a `SLEEP(30)` within seconds. Infinite `WHILE` loops in procedures and
      functions were already cancellable (probed). **Noted:** `GET_LOCK` / `RELEASE_LOCK` / `IS_FREE_LOCK` are
      no-ops that always succeed (MySQL named locks are not implemented: applications that use them for
      mutual exclusion get none).
    - `src/widgets/csv_import.c` (read in full; it parses files that come from outside): no memory defect —
      numeric values are checked before being written unquoted, text values escape `'` and `\`. The column
      limit was still 64 ("the engine's limit"): raised to 1 024 like the engine (P1.11). New fuzz target
      `csv` (also in `.github/workflows/fuzz.yml`): 5.3 M inputs in 90 s, no sanitizer error.
      `sql_text.c` statement splitting falls back to the heap past 64 statements (fine).
    - The fuzz harness (`tests/fuzz/fuzz.c`) had stopped working after P1.11 (a static `TableSchema` with no
      column storage: every target died at start-up under UBSan); fixed with `schema_new`.
  - **Review 2026 Update (v1.5.0 spot audit):**
    - *GUI thread still takes the engine lock directly* — `[code]`: `update_session` runs after every statement and calls `update_completions` (`src/widgets/window.c:151-161`, `107-128`), which lists every database, table and column through `db_list_*` on the UI thread; so do `refresh_tree` (`:760-779`), the Designer sync in the draw path (`:5632` → `src/widgets/schema_view.c:233,253`, `sm_load`) and the users panel (`src/widgets/users_view.c:98,104`). Each call takes the exclusive `g_db_rw` (`src/bridge/db_api.c:2632-2671`). The work itself is cheap (probe `/tmp/sv_audit_probe/comp.c`: 2 000 tables / 20 000 columns rebuilt in 9.5 ms), but while the server runs a long statement (P1.17) the window freezes until it ends. Fix with P1.17: catalog reads under a shared catalog latch, or through `ui_exec` like the editor's queries.
    - *Recursion and size guards hold* — `[reproduced]`, probe `/tmp/sv_audit_probe/deep.c`: deleting the root of a 20 000-row self-referencing `ON DELETE CASCADE` chain (21 ms, no stack growth), a trigger inserting into its own table ("nested too deeply"), an unbounded `WITH RECURSIVE` (stops at 1 000 steps / 1 000 000 rows), 100 000 nested parentheses and 20 000 nested scalar subqueries ("more than 128 nested levels"), 5 000 nested derived tables, 200 000 nested JSON arrays — all refused cleanly, no crash. Note: MySQL caps cascades at 15 levels [not-verified]; SegVault is more permissive.
    - *Windows platform layer* — `[code]`: per-monitor DPI v2 with fallbacks and `WM_DPICHANGED` handling (`src/platform/win32.c:82-112`, `334`) — fine.
    - *Nightly fuzzing is failing* — `[reproduced in CI]`: the scheduled `fuzz` workflow of 2026-11-24 (run 45394474620, target `exec`, seed = run number 8) failed with LeakSanitizer "66 728 byte(s) leaked in 3 allocation(s)" from `parse_select_core` (an `ASTNode` of a nested `SELECT` arm, `src/query/parser.c` via `parse_query`, `db_execute_impl`). The saved `fuzz-crash.bin` (a mangled `MERGE` / upsert) is only the last input — it does not leak on the current tree (replayed under ASan: lexer error, no leak) — so the leaking input is one of the earlier ones in that run. A leak per malformed statement grows a long-running server's memory under hostile or buggy clients. Re-run of the same target and seed on the 1.5.0 tree: see the note below.

- [x] **P3.10 `[SECURITY]` Constrain the local certificate authority** — [code]
  - **Where:** `src/server/tls.c` `make_cert` (CA: `basicConstraints CA:TRUE, pathlen:0`, no `nameConstraints`),
    `ctx_for` (a leaf for any SNI name that passes `good_name`, cached up to `LEAF_CACHE` = 256).
  - **Why:** the CA is installed as a trusted root by `--trust-ca`; it can sign for `bank.example` as well as
    `shop.localhost`. A stolen `ca.key` (backup, synced folder, another local account before the P3.9 fix) is a
    universal MITM certificate for that user's browsers. Remote clients (server bound to a LAN address) can
    exhaust the leaf cache.
  - **Do:** new CAs carry a critical `nameConstraints` extension (permitted `localhost`, `.localhost`, `.test`,
    the configured site names, `127.0.0.0/8`, `::1`, private LAN ranges when `--bind` is not loopback); mint
    leaves only for names that map to a configured site or vhost folder; replace the oldest cache entry instead
    of refusing; offer `--renew-ca` that regenerates and re-trusts. Keep reading an old unconstrained CA but
    print a one-line suggestion to renew it.
  - **Done when:** a certificate for `example.com` signed by a new CA is rejected by `openssl verify` with the
    CA as the only root, `shop.localhost` still verifies, and 1 000 random SNI names do not stop a new site name
    from getting its certificate.
  - **Done (2026-11-16):** new CAs get a critical `nameConstraints` extension (permitted `DNS:localhost`, which
    covers `*.localhost`, `DNS:test`, `127.0.0.0/8`, `::1/128`) — the server serves only those names, so no
    configured-site list is needed. `ctx_for` mints a leaf only for `*.localhost` / `*.test` (other SNI names
    get the `localhost` certificate); a full cache replaces its oldest entry, and the context is handed out
    with its own reference (`SSL_CTX_up_ref`, dropped after `SSL_set_SSL_CTX`) so an eviction cannot free a
    context another thread is about to use. A CA without the extension is still used (it is already
    trusted) but reported at start; `segvault-server --renew-ca` moves it to `ca.crt.old` / `ca.key.old` and
    makes a constrained one (restored if the new one cannot be made). Tests in `tests/test_tls.c`: the CA
    carries the constraints; a certificate for `example.com` signed with the CA key by the `openssl` CLI fails
    `openssl verify`, `shop.localhost` and `api.blog.test` verify; after 300 SNI names `fresh.localhost` and
    an evicted name still get their own certificate, `example.com` gets `localhost`; an old-style CA is
    recognised and `tls_renew_ca` replaces it keeping the old files. **Not verified:** which browsers enforce
    name constraints on a user-added root (OpenSSL does; the `--trust-ca` stores were not exercised).

- [~] **P3.11 `[ARCH]` Hard-coded caps that silently drop data, rules or rows** — audit 2026-11-17 — all findings fixed 2026-11-24, plus two security findings (identifier injection, login throttle bypass)
  - **Root cause:** fixed-size arrays sized for "nobody will ever have more than N" with a `&& n < N` guard in the
    loop that fills them. The guard keeps memory safe but discards everything past N **without telling anyone**.
    Raising an engine limit (P1.11: 1024 columns, dynamic table / database lists) does not raise these copies,
    so every such cap is a latent bug that appears exactly when a real (migrated XAMPP) project is opened.
  - **Rule for new code:** a cap is either (a) dynamic, (b) equal to the engine limit it mirrors (use the
    `SV_MAX_*` constant, never a literal), or (c) enforced with a visible error / "N more" note. Never a silent
    `&& n < 64` in a filling loop.
  - **Findings** (each [code] unless marked):
    1. **`.htaccess` deny rules dropped — security, fail-open.** `src/server/rewrite.c:49` `deny_files[16]`,
       `:190` `if (deny && h->ndeny < 16)`. From the 17th `<Files>` / `<FilesMatch>` block with
       `Require all denied` / `Deny from all` on, the rule is ignored and the file is **served**. Big
       WordPress / plugin `.htaccess` files reach this. Do: dynamic list; on allocation failure deny the whole
       directory (fail closed). [reproduced] — **Fixed 2026-11-17:** `deny_files` grows (doubling from 8); if
       it cannot grow, the whole directory is denied. Test `tests/test_php.c`: 20 `<Files>` deny blocks, the
       20th file gets 403 (was 200), a file outside the blocks still 200.
       **Same file — fixed 2026-11-24:** `Cond pending[32]` dropped `RewriteCond` lines past 32, so the rule fired
       with fewer conditions than written. Now dynamic. Reading the file further found four more fail-open paths,
       all fixed: (a) a `RewriteCond` with a pattern our regex engine rejects was dropped and its rule fired
       **unconditionally** for that condition; (b) `malloc` failure for `r.conds` kept the rule with no conditions;
       (c) an `.htaccess` that exists but cannot be opened (`EACCES`, or `EMFILE` on a busy server) was treated
       as absent, so its `Require all denied` was ignored; (d) directories past `MAX_LEVELS` (32) were dropped.
       Now: a rule whose pattern or conditions cannot all be loaded stays in the list as a **disabled
       placeholder** (never fires, keeps its slot so `S=n` and `[C]` still count it); (c), (d) and an OOM while
       growing the rule list deny the directory (403, as Apache answers 403/500). Tests in `tests/test_php.c`:
       40 conditions with the 40th false (was 403, now 200) and true (403); broken condition (rule not fired);
       `S=1` over a disabled rule; `chmod 000 .htaccess` → 403.
    2. **Named sites deleted from `server.ini` — config loss.** `src/widgets/server_view.c:210`
       `site_name[32]`, `:306` reads at most 32 `site.NAME=` lines, `:322` `web_save` rewrites the file with only
       those, `:404` `SvSite sites[32]`. The CLI accepts 64 (`src/server/main.c:196,254`). Changing the web
       folder or PHP path in the GUI erases sites 33+. Do: one shared limit (or dynamic), and rewrite the file by
       replacing only the keys the panel owns, keeping unknown lines.
       **Fixed 2026-11-24:** the panel keeps every `site.NAME=` line (dynamic `SvSite*`, passed straight to the
       server, which validates names and copies them); `web_save` rewrites only `web_folder=` / `php_cgi=` and
       copies every other line verbatim, through tmp + `fsync` + rename (new `sv_fclose_replace` in
       `src/platform_compat.h`); a `server.ini` over 1 MB or unreadable is never rewritten.
       **Found while there, fixed:** the GUI wrote `server.token` (the HTTP API token) with plain `fopen`, i.e.
       0644 / inherited ACL — now `sv_fopen_private` + atomic replace. `settings.ini`, saved queries
       (`remove` + `rename` left a moment with no file), the session files, the ER positions and **the user's
       own `.sql` files saved from the editor** were truncated in place, so a crash or full disk mid-save left
       an empty / cut file; all now written to a sibling file and swapped in (editor save keeps the file mode
       and writes symlinks in place).
    3. **ER diagram: tables piled at (0,0)** — [reproduced]. `src/widgets/schema_model.c:236`
       `int order[512]; float key[512]` in `sm_auto_layout`: tables past 512 in one level are never placed.
       600 tables without foreign keys (typical MyISAM schema from XAMPP, WordPress multisite): 88 tables stay
       stacked at the origin, also after "Arrange". **Fixed 2026-11-17:** `order` / `key` allocated for
       `m->nt` tables. Test `tests/test_schema_model.c`: 600 tables, all placed (was 88 unplaced).
    4. **Users panel hides table grants.** `src/widgets/users_view.c:297,329,502` show and hit-test only the
       first 4 table-level grants, no "N more" line (the database list has one); the others cannot be seen or
       revoked from the GUI. Fetch is capped at `UV_MAX_TG` = 64 (`:14`) as well.
       **Fixed 2026-11-24:** users, databases and table grants are dynamic (`UV_MAX_USERS` 128 and `UV_MAX_DBS`
       64 were silent caps too, and the user list did not scroll: users past the panel height were invisible);
       the table-grant section scrolls with a "1-4 of N" header; the user list scrolls and follows the
       selection. **Security finding while there — stored identifier injection, fixed:** the panel built
       `GRANT … ON %s.* TO u` / `REVOKE ALL ON %s.%s` from database / table names **without quoting** and ran
       them with `exec_statements` (several statements, as the GUI administrator). The engine accepts
       ``CREATE DATABASE `x.* TO bob; DROP DATABASE victim; GRANT SELECT ON y` ``, so any user with `CREATE`
       (a web app's MySQL account) could make the admin's next click on that row run `DROP DATABASE`
       [reproduced: the name is stored and the generated text parses as three statements]. Same class
       elsewhere: `DROP DATABASE %s` (confirm dialog), the editor snippets `SELECT * FROM %s` / `SHOW CREATE
       TABLE %s` (run by the user's next Run-all), the GUI dump (`CREATE DATABASE %s; USE %s;` and
       `INSERT INTO %s` — a re-imported dump ran the name), `db_get_objects_sql` (views / triggers / routines in
       dumps), `SHOW GRANTS` output (replayed by user-copy scripts), CSV import and SQL export (also broke on
       keyword names such as `order`). And the backtick-quoted sites were not safe either: a **string literal is
       accepted as a name** (`CREATE DATABASE 'q`x'`, `CREATE INDEX 'i`j'`, CTAS aliases), so a name could hold
       a backtick and close the quotes; backtick names could hold newlines (breaking `-- comment` lines in
       dumps). Fix, two layers: (1) engine — `sv_name_ok` (`include/common.h`): a new database / table /
       column / index / constraint / view / sequence / trigger / routine name must be non-empty, without
       backtick or control characters, checked in the executor DDL paths (create, CTAS, ADD / RENAME COLUMN,
       RENAME TO, ADD CONSTRAINT) and again in `catalog_add_db/table/view/index` (`SV_BAD_NAME`); existing
       objects stay addressable. (2) GUI — every generated statement goes through `sql_ident`
       (`src/widgets/sql_quote.h`), which refuses names it cannot quote; dumps refuse to write a partial file
       (a table that cannot be read or an OOM used to produce a "successful" truncated dump). `SHOW GRANTS`
       quotes names like MySQL. The MySQL wire `USE` (handshake / `COM_CHANGE_USER` / `COM_INIT_DB`) used
       `%.63s`: a longer name was cut and the session landed in another database — now whole or rejected
       (`my_use_db`). The grid-edit `USE \`%.60s\`` had the same truncation, so an edit could hit the
       same-named table of another database; now it aborts if the `USE` fails. Tests: `tests/test_sql.c` (name
       rules on every DDL path; `x; DROP …` stays a legal quoted name), `tests/test_auth.c` (quoted `SHOW
       GRANTS`), `tests/test_engine.c` (CSV with `order,select,date` headers). Not unit-tested: the GUI call
       sites (no harness for `window.c` / `users_view.c`); reviewed line by line.
    5. **ER diagram database menu: first 64 databases only.** `src/widgets/schema_view.c:232`
       (`v->dbs[64]`); the engine allows 100 000. **Fixed 2026-11-24:** dynamic list; the menu shows as many
       rows as fit under the button, scrolls with the wheel ("1-20 of 300 · scroll"), opens on the current DB.
    6. **`SHOW PROCESSLIST` over the MySQL protocol: 256 rows max.** `src/server/server.c:3851`
       `Row rows[256]` (plus ~430 KB of `buf` on the stack); the HTTPS default for `max_clients` is 256
       (`server.c:5003`), so a busy server truncates the list an admin uses to find what to `KILL`.
       **Fixed 2026-11-24:** rows and text buffers on the heap, one per `max_clients`.
    7. **Recursive CTE: only the first 64 `UNION` arms.** `src/query/executor.c:5921` `ASTNode* arms[64]`;
       arms past 64 are ignored (rare, but a wrong result, not an error). **Fixed 2026-11-24:** arms counted and
       allocated; test in `tests/test_sql.c` (70 anchors + a recursive arm: 139 rows).
    8. **Mail catcher: 64 attachments per message.** `src/server/mail.c:308,374` — the rest disappear.
       **Fixed 2026-11-24:** dynamic list; `tests/test_mail.c` sends 70 and downloads the 70th.
    9. **Grid editing: read-only above 64 columns.** `src/widgets/grid_edit.h:31-34`, `window.h:216-221`,
       `window.c:1947,1980,2071` (`types[64]`, `vals[64]`). Not silent ("Read-only: too many columns"), and every
       access is guarded by `grid_edit_cell_ok`, but a 100-column table from XAMPP cannot be edited in the grid.
       The `window.c` stack arrays read past 64 if that guard is ever relaxed: make them `num_cols`-sized
       together with `GridEdit`. **Fixed 2026-11-24:** `GE_MAX_COLS` = `SV_MAX_COLUMNS` for `GridEdit`, the
       draft-row arrays and the `window.c` buffers (one constant, rule (b)); `result_set_row` refuses wider
       results; the window-teardown loop freed only 64 draft values (leak). Test: a 100-column table edited at
       column 100 in `tests/test_grid_edit.c`.
    10. **Login throttling bypass — security, found by the "done when" grep.** `src/server/server.c`
       `login_fail[64]`, LRU by (user, address): alternating password guesses for `root` with 64 invented user
       names evicted `root`'s entry, so its back-off never started (unlimited guesses at full speed)
       [reproduced: the new test logs in with 200 on the old code]. **Fixed 2026-11-24:** 1024 slots
       (`LOGIN_SLOTS`); a new entry never evicts one that is still making someone wait while a non-blocking one
       exists; a second counter per address alone (`LOGIN_FREE_FAILS_ADDR` = 20) for non-loopback clients
       (loopback is exempt: every local app shares 127.0.0.1 and one misconfigured app must not lock the others
       out); a successful login clears only its own pair. Test: `tests/test_server.c` (blocked user stays 429
       after 80 failures under other names).
  - **Already fixed (2026-11-17):** result grid, Designer and ER model were capped at 64 columns
    (`TABLE_VIEW_MAX_COLS`, `DESIGN_MAX_COLS`, `SM_MAX_COLS`) and showed only the first 64 of a wide table
    without a word; at the cap `design_add_column` did nothing, so naming the "new" column renamed column 64
    (`RENAME COLUMN c63 TO c100`, seen in the test). Now: grid widths allocated per result, ER columns
    allocated per table, Designer = `SV_MAX_COLUMNS`. Tests: 100-column tables in `tests/test_table_design.c`
    (load all, add the 101st) and `tests/test_schema_model.c`.
  - **Done when:** every finding above is dynamic or reports the cap; a grep for `&& [a-z_]+ < [0-9]+` in filling
    loops under `src/widgets`, `src/server`, `src/bridge` finds only bounded, documented cases.
  - **Grep 2026-11-24, remaining hits (all bounded on purpose):** chart series 64 / 400 sampled rows and export
    column-width sampling (1000 / 2000 rows: widths only, every row is written); primary-key columns 8
    (`db_tools.c`, `schema_model.c`: = `SV_MAX_KEY_COLS`, see P3.12); PHP pool refs 8, FastCGI argv 76,
    MIME nesting 12; settings rows 32 per section (UI);
    `EXECUTE … USING` 64 variables (P3.12 #5, loud). One more silent cap found by the grep and fixed: HTTP/2
    `hpack_decode` (`src/server/h2.c`) dropped every header after the 512th of a block (the request went on
    without them); now the block is refused (`GOAWAY`), like HTTP/1's 431, and a failed `strdup` no longer
    leaves a NULL value.

- [~] **P3.12 `[ARCH]` Engine limits still sized for the pre-1.5 world (loud, but below MySQL)** — audit 2026-11-17 (1 fixed 2026-11-24)
  - **Why it matters:** these fail with an error, so no data is lost, but they turn a working XAMPP project
    (dump import, ORM query, WordPress plugin) into "Too many …" on the first run. Several are the same
    literal 64 that P1.11 removed for columns.
  - **Findings** (limit here → MySQL / MariaDB):
    1. `UPDATE … SET` and `ON DUPLICATE KEY UPDATE` / `ON CONFLICT DO UPDATE`: **64 columns**
       (`SV_MAX_SETS`, `src/query/parser.h:227`; checks `parser.c:2393,2426,2668`) → no limit. A table can now
       have 1024 columns but cannot be fully updated or upserted in one statement (Laravel `upsert`, Doctrine
       full-entity `UPDATE`). **Fixed 2026-11-24:** `set_cols / set_exprs / set_tables / set_texts` and
       `dup_cols / dup_exprs` are heap lists grown by `upd_set_room` / `ins_dup_room` (the fixed arrays also made
       every `ASTNode` ~12 KB larger, since they sit in the node union); `SV_MAX_SETS` = 4 × `SV_MAX_COLUMNS`
       only stops absurd text and is named in the message. Executor: `exec_update` allocates the column-index
       array, the upsert's temporary UPDATE node borrows the insert's names, multi-table UPDATE gives each target
       a slice of one buffer. Tests (`tests/test_sql.c`): a 300-column table updated, upserted with `ON DUPLICATE
       KEY UPDATE` and `ON CONFLICT DO UPDATE`, and updated through a self-`JOIN`, each in one statement.
    2. `JOIN`s per `SELECT`: **8** (`parser.h:274` `joins[8]`, `parser.c:1901`) → 61. WordPress `meta_query`
       with several clauses self-joins `wp_postmeta` once per clause; Magento EAV queries exceed 8 joins.
       **Fixed 2026-11-24:** 61 tables as in MySQL (`SV_MAX_JOINS` 60). `joins` is a heap list; the plan's
       ~22 per-source arrays (`Source` alone is 1.3 KB) are carved from one block sized to the query's real
       table count (`plan_alloc_src`) instead of a fixed `MAX_SOURCES` in every plan — a 2-table query
       allocates less than before, not more; source masks widened to `uint64_t`. Tests: a 12-way self-join, a
       61-table `LEFT JOIN` chain, and the 62nd table refused with the limit in the message.
    3. Indexes per table: **16** (`SV_MAX_INDEXES`, `include/common.h:44`, `schema.c:1224`;
       `SV_MAX_KEY_DEFS` `parser.h:156`) → 64. Constraints per table: **16** (`SV_MAX_CONSTRAINTS`,
       `SV_MAX_CONS_DEFS`). Columns per key: **8** (`SV_MAX_KEY_COLS`, `src/catalog/schema.h:69`) → 16.
       Dumps of schemas with many indexes / foreign keys fail at `CREATE TABLE`. `SV_MAX_KEY_COLS` is part of
       the catalog format (`schema.c:539,589` write/read fixed 8 slots): raising it needs a catalog version.
       **Fixed 2026-11-24 (indexes and constraints):** 64 indexes and 64 constraints per table (formats v6/v7
       already store the counts, so no format change; the v4/v5 loaders use frozen `SV_V5_*` = 16 so old
       catalogs still migrate). Work tables (`schema_new`: derived tables, CTEs, `information_schema`, routine
       variables) no longer carry 16 + 16 index / constraint slots (~30 KB zeroed each time); only `CREATE
       TABLE` reserves constraint slots (`schema_new_aux`), `schema_dup` exactly the source's. The ENUM check
       constraint was silently skipped when the constraint list was full — now an error. `CREATE TABLE`
       constraint / key lists are heap (`ast_cons_add`, `ast_key_add`). Tests: a mysqldump-style table with 20
       `KEY`s and 20 `FOREIGN KEY`s (enforced, and kept across two restarts and a `CHECKPOINT`); 65 keys
       refused. **Key columns 8 → 16 done 2026-11-24:** `SV_MAX_KEY_COLS` 16 (`Index.cols`, `Constraint.cols/ref_cols`,
       parser key / FK lists, `KEY_BUF` 4096 so a 16-part key never falls back to "not indexed", `MemIndex.col`,
       FK info in `db_api.h`, GUI grid-edit / ER / db-tools buffers); catalog v8 writes only `ncols` key
       columns, v6 / v7 keep reading 8 fixed slots, v4 / v5 use frozen `IndexV5` / `ConstraintV5` (layout
       still checked by the `TableSchemaV5` static assert). Tests: 12-column PK, enforced 16-column UNIQUE,
       12-column FK, 17 refused. **To do:** a restart test for a 16-column key (v8 variable encoding).
    4. `ORDER BY`: **8** expressions (`SV_MAX_ORDER`, `parser.c:2242`); `GROUP BY` / `ROLLUP`: **16**
       (`SV_MAX_GROUP`, `parser.c:2059,2073`; `:2059` also tests the literal `16` twice) → no limit.
       **Fixed 2026-11-24:** heap lists (cap 4096, named in the message); `ROLLUP` / `CUBE` / `GROUPING SETS`
       keep 32-bit set masks and say "up to 32" instead of shifting past bit 31 (`WITH ROLLUP` over 64
       expressions would also have overrun the 64-entry set array). The compound-query `corder` is allocated
       per plan instead of a fixed array. Tests: 22-key `ORDER BY`, 41-expression `GROUP BY`, the ROLLUP error,
       10-key `ORDER BY` on a `UNION`.
    5. `MERGE`: 8 `WHEN` clauses, 32 columns / values in the `INSERT` branch (`parser.c:3407,3423,3448`)
       (**fixed 2026-11-24:** heap lists, 256 clauses / 1024 columns; the `INSERT` column list now uses its own
       count instead of the value count);
       stored routines: 32 parameters, `CALL` 32 arguments (`parser.c:3255,3526`); window functions per query 16
       (`executor.c:5707`); `EXECUTE … USING` 64 variables (`src/bridge/db_api.c:1608`).
    6. Misleading messages: table-function alias column list `alias(c1, …)` allows 8 names
       (`parser.c:1849`, `parser.h:193`) and the `MERGE` insert column list 32 (`parser.c:3441`), but both
       report "Expected a column name" instead of the limit.
    7. Silent optimisation cut-offs (correct results, slower plans): join reordering skips `SELECT *` that
       expands past 64 columns (`executor.c:6941`) and queries with 128+ conjuncts (`executor.c:6845`).
  - **Side effect worth more than the limits — `ASTNode` 66 KB → 3.3 KB (fixed 2026-11-24):** every parse
    node (each `SELECT`, subquery, expression statement, routine statement, prepared statement) is one union, and
    `create_view.definition` / `create_trigger.body` were inline `char[SV_MAX_SQL_LEN]` (64 KB), `create_table`
    held 16 `ConstraintDef` + 16 `TableKeyDef` (36 KB) and `merge` 8 × 2.3 KB. Every node was `calloc`ed and
    zeroed at 66 KB; a routine body of 100 statements held 6.6 MB of AST. All those are heap now (view / routine
    text sized to its length).
  - **Do:** make the parser lists dynamic (as P1.11 did for column lists), tie every remaining cap to one named
    constant with the MySQL value where cheap (JOIN 61, indexes 64, key columns 16 with a catalog bump), and
    put the limit in every "Too many" message. List the remaining limits in `docs/` next to the P3.7 limits.
  - **Done when:** a 300-column table can be updated / upserted in one statement, a 12-way self-join and a
    table with 20 indexes and 20 foreign keys import from a mysqldump, and `tests/test_sql.c` covers each new
    limit and its message.
  - **Review 2026 Update (v1.5.0):** items 1-5 done in 1.5.0 (61 tables, 64 indexes / constraints, 16 key columns, dynamic ORDER / GROUP / SET / MERGE lists). Remaining caps, all loud: `MATCH` 16 columns (`src/query/parser.c:1291`), `SELECT … INTO` 16 variables (`:1989`), table-function columns 16 (`:2384`), routine parameters / `CALL` arguments 32 (`:3361`, `:3666`), `CHECK / ANALYZE / OPTIMIZE TABLE` 16 tables (`:4801`), `DROP TABLE a, b, …` 16 tables (`:4984` — a multisite cleanup script dropping 20 tables fails), `JOIN … USING` 8 columns and trigger `UPDATE OF` 8 columns (`src/query/parser.h:204,513`); join reordering considers ≤ 16 tables and ≤ 128 conjuncts (`src/query/executor.c:6995,7017`).
  - **Review 2026 Update (v1.5.0) — function calls take at most 32 arguments, which breaks WordPress `post__in` ordering** — `[reproduced]`: `MAX_FUNC_ARGS 32` (`src/query/parser.c:705`, checked at `:725`; argument arrays allocated at that size at `:722,753,1065,1114,1210,1288,1379`). Probe `/tmp/sv_audit_probe/fargs.c`: `SELECT ID FROM wp_posts WHERE ID IN (…40 ids…) ORDER BY FIELD(wp_posts.ID, …40 ids…)` → "Too many function arguments"; also `JSON_OBJECT` with 20 key / value pairs, `COALESCE` with 41 arguments, `GREATEST` with 40. WordPress core builds that exact clause with one argument per requested post and no batching (`WP_Query::parse_orderby`: `"FIELD({$wpdb->posts}.ID," . implode( ',', array_map( 'absint', $this->query_vars['post__in'] ) ) . ')'`, same for `post_parent__in` / `post_name__in`; github.com/WordPress/wordpress-develop, `src/wp-includes/class-wp-query.php`), so any hand-picked list of more than 31 posts or products fails. **Do:** grow the argument array by doubling (as the other lists in 1.5.0) with a limit far above MySQL practice (e.g. 65 535, the prepared-statement placeholder limit), keep `FIELD` / `ELT` / `JSON_OBJECT` / `COALESCE` / `CONCAT` / `GREATEST` / `LEAST` working at that size; **Done when:** `tests/test_mysql.c` runs the WordPress clause with 500 ids and `JSON_OBJECT` with 100 pairs. Long `IN (…)` lists (10 000 values) and `OR` chains (1 000 terms) already work (probe `inlist.c`).
  - **Review 2026 Update (v1.5.0) — identifiers stop at 63 bytes (MySQL: 64 characters; aliases 256)** — `[reproduced]`: names live in `char[SV_MAX_NAME_LEN]` with `SV_MAX_NAME_LEN 64` counting the NUL (`include/common.h:61`), checked by `copy_text` (`src/query/parser.c:128`). Probe `/tmp/sv_audit_probe/ident64.c`: a 64-character table name and a 64-character column name → "Name or value too long (max 63 characters)"; a 100-character column alias (quoted or not) → same error. MySQL counts identifier length in characters (64) and allows 256-character column aliases, so dumps with maximal names (generated index / constraint names cut to exactly 64) and ORMs emitting long aliases fail here, and non-ASCII names (`città_…`) hit the byte limit even sooner. Long names are rejected, never truncated (no aliasing — checked). **Do:** decouple the on-disk legacy layouts (`SV_V5_*` style constant for v4 / v5 structs) and raise the in-memory name buffers to 64 characters × 4 bytes + NUL (catalog v8 already length-prefixes strings); column aliases in result metadata as heap strings up to 256 characters. **Done when:** `tests/test_mysql.c` creates 64-character ASCII and 64-character accented table / column / index names and selects a 256-character alias.

- [ ] **P3.13 `[COMPAT]` Client compatibility is only tested with PHP: no `mysql` CLI / `mysqldump`, JDBC, Node `mysql2`, Python drivers, phpMyAdmin or Adminer in CI** — `[code]` · **Size:** `M` · **Deps:** `none`
  - **Where:** `tests/test_server.c:1384-1400` (PHP `mysqli`, the only third-party client in the suites) and the in-repo C client of `tests/test_server.c`; `.github/workflows/ci.yml` installs only `build-essential php-cgi`.
  - **Why:** `README.md:130` advertises the MySQL protocol for "PHP, Node, Python, Java, client mysql", but none of those is exercised; this review could not run them either (no `mysql`, `mysqldump`, `mariadb`, Java, Node `mysql2`, `pymysql` or `mysql.connector` on the review machine) [not-verified]. Each driver has its own handshake and protocol paths (RSA for `caching_sha2_password`, `CLIENT_DEPRECATE_EOF`, binary protocol, `COM_STMT_FETCH` cursors, `CLIENT_LOCAL_FILES`, which the server does not advertise: `src/server/mysql_wire.h:23-35`).
  - **Do:** a `compat` CI job: `mysql-client` (CLI + `mysqldump` → import round trip of the test database), Node 22 + `mysql2` (prepared + pooled), Python `PyMySQL`, `mysql-connector-python` and an SQLAlchemy ORM smoke test, Java single-file programs with MariaDB Connector/J and MySQL Connector/J, PHP `PDO_MYSQL` with emulated and native prepares, and a crawl of Adminer 4.8 and phpMyAdmin 5.2 pages that fails on any SQL error text. Rejected: one Docker image per tool (slow; no Windows story).
  - **Done when:** the job is green on every push and generates the driver matrix in `docs/API.md`.

- [ ] **P3.14 `[PERF]` No benchmark against MariaDB over the wire: in-process numbers hide the protocol path (P1.17 is invisible to `make bench`)** — `[code]` · **Size:** `M` · **Deps:** `P3.13`
  - **Where:** `bench/suite.c` (SegVault in-process vs SQLite; the only wire scenario `pdo_point_lookup` is absolute and not gated, `bench/compare.py`), `bench/baseline.json`.
  - **Why:** the product competes with XAMPP's MariaDB 10.11.19 and Laragon's MySQL, but no benchmark runs either; `readers_16` uses in-process sessions, so the server-wide serialization of P1.17 (4 × 300 ms queries → 1.2 s over the wire) never shows up.
  - **Do:** `make bench-wire`: start `segvault-server` and MariaDB 10.11 (same minor as XAMPP 8.5.13) from a pinned tarball; one C driver speaking the protocol directly (reuse the `tests/test_server.c` client) with N connections; scenarios: PK lookup at 1 / 8 / 32 connections, 1 000-row range, autocommit INSERT (SegVault `synchronous = FULL` vs `innodb_flush_log_at_trx_commit = 1`), 8 connections updating different rows of one table, a 12-query WordPress-like page, import of a 100 MB dump; p50 / p99 latency and throughput; the same ±15 % gate as `bench/compare.py`. Rejected: sysbench (needs libmysqlclient + Lua, Linux only).
  - **Done when:** `make bench-wire` writes the README table; a weekly CI job keeps a baseline.

---

## P4 — Product parity and beyond (XAMPP / UniServerZ / Laragon)

Review 2026-11-24 of **v1.5.0** (tree at the 1.5.0 version bump, `make clean all test dist`, `make test-asan` and the
Windows build all green on that tree). Competitor facts come from the vendors' pages read on 2026-11-24:
XAMPP 8.3.37 / 8.4.28 / 8.5.13 (apachefriends.org/blog/new_xampp_20261120.html, apachefriends.org/download.html,
apachefriends.org/faq_windows.html); Uniform Server ZeroXV 15.0.2 (sourceforge.net/projects/miniserver/ and its
ZeroXV Modules listing — uniformserver.com answered HTTP 403); Laragon 8.x (github.com/leokhoa/laragon/releases/tag/8.0.0,
laragon.org/, laragon.org/docs/quick-app, /multi-version, /mailpit, /auto-ssl, /quick-share, laragon.org/pricing).
Probes live in `/tmp/sv_audit_probe/` (outside the repository).

### Competitive matrix — SegVaultDB v1.5.0 vs. XAMPP (8.x) vs. UniServerZ (v15) vs. Laragon (v8)

Cells: `✓` = yes, `Partial`, `✗` = no; "n/d" = not documented on the vendor pages read (treat as `✗` / unknown, `[not-verified]`).

| Capability | **SegVaultDB 1.5.0** | XAMPP 8.3.37 / 8.4.28 / 8.5.13 | UniServerZ 15.0.2 | Laragon 8.x |
|---|---|---|---|---|
| **DB — engine** | own C engine, MySQL dialect + wire protocol | MariaDB 10.11.19 | MySQL 8.4.1 or MariaDB 10.11.8 module | MySQL (Quick-add lists 8.4 / 9.1), PostgreSQL optional |
| DB — concurrent queries over the MySQL protocol | ✗ one statement at a time server-wide (P1.17) | ✓ thread per connection | ✓ | ✓ |
| DB — row-level locking | ✗ table locks (P1.18) | ✓ InnoDB | ✓ InnoDB | ✓ InnoDB |
| DB — isolation | Partial: READ COMMITTED only, reports SERIALIZABLE (P1.19) | ✓ REPEATABLE READ default | ✓ | ✓ |
| DB — FK cascades / triggers / routines / CTE / window / JSON | ✓ (probed: cascades, trigger recursion guard, CTE guard) | ✓ | ✓ | ✓ |
| DB — FULLTEXT | Partial: per-row scan, relevance always 1 (P4.3) | ✓ ranked, boolean mode | ✓ | ✓ |
| DB — spatial | ✗ type accepted, no `ST_*` (P4.2) | ✓ geometry types / constructors | ✓ | ✓ |
| DB — EVENT scheduler | ✗ (P4.1) | ✓ | ✓ | ✓ |
| DB — binlog / point-in-time recovery | ✗ (P4.4) | ✓ | ✓ | ✓ |
| DB — MySQL-size limits | Partial: 61 joins / 64 indexes / 16 key parts ✓; 32 function args, 63-byte names (P3.12) | ✓ | ✓ | ✓ |
| DB — default connection budget | 64 shared with HTTP + SMTP (P2.16) | 151 [not-verified] | 151 [not-verified] | 151 [not-verified] |
| DB — admin UI | ✓ native editor, designer, ER, charts, users | phpMyAdmin 5.2.3 | phpMyAdmin / Adminer | n/d on pages read |
| DB — backup / restore | ✓ `BACKUP TO`, scheduled, GUI buttons | n/d | ✓ MySQLAutoBackup | ✓ MySQL auto backup |
| DB — large dump import / export | Partial: 256 MB cap, whole file in RAM (P4.6) | ✓ server-side tools | ✓ | ✓ |
| **Web — PHP versions** | 1 per server; `get-php.cmd` takes latest 8.5 / 8.4 / 8.3, unverified (P4.7) | 1 per installer | 7.0–8.3, global switch | global switch, newest auto-added |
| Web — per-site PHP version | ✗ (P4.7) | ✗ | ✗ | n/d |
| Web — Xdebug toggle | ✗ (P4.8) | n/d | n/d | n/d |
| Web — automatic virtual hosts | ✓ `NAME.localhost` / `NAME.test`, no hosts file | ✗ manual `httpd-vhosts` | n/d | ✓ `*.test` |
| Web — trusted HTTPS per site | ✓ local CA with name constraints, one-click trust | n/d (port 443 only) | n/d | Partial: self-signed, manual "Add to Trust Store" |
| Web — HTTP/2, reverse proxy for Vite / Next (WebSocket) | ✓ / ✓ | n/d / ✗ manual | n/d / ✗ | n/d / n/d |
| Web — Node / Python / Go runtimes and workers | ✗ proxy only, no process manager (P4.10) | ✗ | ✗ | ✓ multi-version |
| Web — Redis-compatible cache | ✗ (P4.11) | ✗ | ✗ | n/d |
| Web — mail catcher | ✓ SMTP + web UI + JSON API; no live updates / release (P4.14) | Partial: Mercury MTA (Windows) | n/d | ✓ Mailpit |
| Web — public sharing tunnel | ✗ (P4.12) | ✗ | ✗ | ✓ ngrok "Quick share" |
| **DX — footprint / install** | ✓ single exe (server 1.4 MB, GUI 3.4 MB on Windows), PHP separate | installer / zip, full Apache + MariaDB + Perl + Tomcat bundle | 52.7 MB self-extractor, portable | installer, portable folder |
| DX — one-click apps | ✗ (P4.9) | ✗ | ✗ | ✓ WordPress / Laravel / Symfony / Drupal |
| DX — migration from another stack | ✗ (P4.13) | — | — | — |
| DX — updates | ✗ (P4.19) | n/d | n/d | Partial: paid tiers only |
| DX — GUI language | English only (P4.15) | n/d | n/d | n/d |
| DX — accessibility / HiDPI | ✗ screen readers (P4.16) / ✓ per-monitor DPI v2 + UI scale | n/d / n/d | n/d / n/d | n/d / ✓ "full High DPI" |
| **Security — network exposure** | ✓ 127.0.0.1; LAN refused without token; DNS-rebinding and Origin checks | n/d | n/d | n/d |
| Security — DB login by default | ✗ open mode: any local user is root (P4.21) | ✗ security console to set passwords | n/d (users report no root password) | ✗ empty root password |
| Security — signed binaries / SBOM | ✗ (P4.18) | n/d (FAQ on antivirus false positives) | ✗ SHA1 sums only | ✓ code-signed since 8.0 |
| Security — telemetry | ✓ none in code; not yet verifiable (P4.23) | n/d | n/d | n/d |
| **Ops — crash / power-loss safety** | ✓ WAL v2, page CRC, double write, fault-injection suite | ✓ InnoDB | ✓ InnoDB | ✓ InnoDB |
| Ops — platforms | Windows, Linux | Windows, Linux x64, macOS arm64 | Windows 64-bit | Windows |
| Ops — licence / price | ✗ no licence file (P4.20) | free | free | free non-commercial; commercial $49 / year or $149 lifetime |

### Execution roadmap — v1.6 → v1.7 → v2.0

**v1.6 — Stability & core engine.** P1.19 (report the real isolation level first: a one-day fix that stops tools being misled), P3.12 rest (32 function arguments — WordPress `post__in`; 64-character names), P2.16, **P1.17**, **P1.18**, P1.20, P1.15, P1.14, P3.9, P3.13, P3.14, P4.5, plus two non-engine prerequisites: P4.20 (licence) and P4.18 (signing).
*Why this order:* every ecosystem feature of v1.7 multiplies concurrent database clients (per-site PHP pools, workers, queue runners, a Redis-backed app still hitting MySQL), so the server-wide serialization (P1.17) is the root bottleneck; row locks (P1.18) only pay once statements run concurrently, and group commit (P1.20) only once commits can overlap. P2.16 is cheap and removes the "64 idle connections take every site down" failure before connection counts grow. P3.13 / P3.14 come with the engine work because they are how the engine change is proven (driver matrix, MariaDB wire benchmark). The licence and signing are prerequisites with no code dependency: distribution channels and SmartScreen reputation take time to build, and the v2.0 updater and macOS notarization reuse the signing pipeline.

**v1.7 — Ecosystem & framework parity.** P4.21 (secure default credentials) → P4.7 (side-by-side and per-site PHP) → P4.8 (Xdebug) → P4.9 (Quick App), P4.10 (process manager) → P4.12 (sharing), P4.11 (Redis), P4.14 (mail), P4.6 (streaming import / export) → P4.13 (migration wizard), P4.22 (ports), and the SQL parity items P4.1 (events, needs P1.17's multi-session engine for its scheduler thread), P4.3 (full-text index, persisted like the P1.15 trees), P4.2 (spatial).
*Why this order:* Quick App must write real credentials into `wp-config.php` / `.env`, so the default-auth change comes first; Xdebug and Quick App build on the per-version PHP pools; sharing reuses the process manager; the migration wizard needs both the streaming importer and the wire client of P3.13.

**v2.0 — Commercial polish & scale.** P4.19 (auto-update, on the v1.6 signing pipeline), P4.17 (macOS: server first, then the Cocoa GUI), P4.15 (Italian / English) → P4.16 (accessibility, labels from the string table), P4.4 phases 2–3 (archived WAL for point-in-time restore, then a row-event stream), P4.23 (verifiable zero telemetry, together with the update check it must cover).
*Why this order:* these widen the market (Mac users, public-sector buyers, teams) but depend on a stable engine and the v1.7 feature set; localisation must precede accessibility because screen-reader labels come from the same string table.

### Items

- [ ] **P4.1 `[COMPAT]` No EVENT scheduler (`CREATE EVENT`, `SHOW EVENTS`, `@@event_scheduler`)** — `[reproduced]` · **Size:** `M` · **Deps:** `P1.17`
  - **Where:** missing — `src/query/parser.c:5100` (`SHOW` rejects `EVENTS`), the `CREATE` dispatch in `parser.c` has no `EVENT` branch; `information_schema.EVENTS` exists only as an empty 3-column stub (`src/query/executor.c:6374`); planned in `README.md:817,1671`.
  - **Why:** probe `eng.c`: `CREATE EVENT e1 ON SCHEDULE EVERY 1 MINUTE DO …` → "Unexpected token: e1"; `SHOW EVENTS` → parse error; `SELECT @@event_scheduler` → unknown variable. MariaDB and MySQL schedule SQL with `CREATE EVENT` and the `event_scheduler` variable (mariadb.com/kb/en/events/); dumps made with `mysqldump --events` and apps that purge old rows with events (WordPress plugins, Matomo [not-verified]) fail on import or silently stop cleaning up.
  - **Do:** catalog object (name, `AT` / `EVERY` + `STARTS` / `ENDS`, `ON COMPLETION [NOT] PRESERVE`, `ENABLE` / `DISABLE`, definer, body stored like a routine); one scheduler thread in the engine sleeping until the next due time, running each body as its definer in its own session (needs P1.17 to not block clients); `event_scheduler` ON / OFF; `ALTER` / `DROP EVENT`, `SHOW EVENTS`, `SHOW CREATE EVENT`, full `information_schema.EVENTS`; dump / import (`/*!50106 CREATE EVENT … */`). Rejected: delegating to OS cron / Task Scheduler (not portable, no SQL semantics, breaks the single-binary model).
  - **Done when:** `tests/test_sql.c` with an injectable clock: an `EVERY 1 SECOND` event inserts rows, `AT` + `ON COMPLETION NOT PRESERVE` drops itself, events survive a restart; a mysqldump with `--events` imports (`tests/test_mysql_import.c`).

- [ ] **P4.2 `[COMPAT]` Spatial types are accepted but have no functions or indexes (`POINT` column OK, `ST_*` unknown, `SPATIAL KEY` dropped)** — `[reproduced]` · **Size:** `L` · **Deps:** `none`
  - **Where:** `src/query/parser.c:4270` (`SPATIAL` keys discarded: "SPATIAL: ignorato"); no geometry type or `ST_*` function in `src/query/executor.c`.
  - **Why:** probe `eng.c`: `CREATE TABLE g (id INT PRIMARY KEY, p POINT NOT NULL, SPATIAL KEY (p))` → OK, then `SELECT ST_AsText(ST_GeomFromText('POINT(1 2)'))` → "Unknown function 'ST_ASTEXT'", `ST_Distance_Sphere(POINT(…), POINT(…))` → unknown function. MariaDB provides geometry types and constructors such as `POINT`, `ST_BUFFER`, `ST_AsGeoJSON` (mariadb.com/docs/server/reference/sql-statements/geometry-constructors/geometry-constructors.md); `ST_Distance_Sphere` and SPATIAL indexes on MariaDB [not-verified: page not fetched]. Store-locator / delivery-zone plugins and Laravel spatial packages need at least points, WKT / WKB I/O, distance and bounding-box search.
  - **Do:** a `GEOMETRY` family stored as MySQL internal WKB (4-byte SRID + WKB) so the wire protocol returns the same bytes as MySQL; functions in two steps: (a) `ST_GeomFromText` / `ST_AsText` / `ST_AsGeoJSON` / `ST_X` / `ST_Y` / `ST_SRID` / `ST_Distance` / `ST_Distance_Sphere` / `MBRContains` / `ST_Within` for points and polygons; (b) an R-tree (or quadtree over the existing B+tree code) for `SPATIAL` indexes. Rejected: linking GEOS (C++ runtime, breaks the zero-dependency single binary).
  - **Done when:** `tests/test_sql.c` covers WKT round trips, `ST_Distance_Sphere` against known city distances (±0.1 %), and a 100 000-point bounding-box query that reads < 1 % of the rows with an `EXPLAIN` showing the spatial index.

- [ ] **P4.3 `[COMPAT]` `FULLTEXT` is a per-row filter: full table scan and a relevance that is always 1** — `[reproduced]` · **Size:** `L` · **Deps:** `P1.15` (index persistence)
  - **Where:** `src/query/executor.c:4779-4800` (`MATCH_AGAINST` evaluated per row), `:3211` (`ft_score`: term matching without document frequencies); `FULLTEXT KEY` parsed but builds no inverted index.
  - **Why:** probe `eng.c` on 3 documents: `MATCH(body) AGAINST('database')` returned score `1` for both matching rows (no ranking), `EXPLAIN` shows "Table scan" + "Filter WHERE MATCH_AGAINST(…)"; boolean mode (`+database -embedded`) works. MariaDB "calculates a relevance for each result" in natural-language and boolean modes (mariadb.com/kb/en/full-text-index-overview/). WordPress search plugins and Laravel Scout's database engine sort by relevance; on a 100 000-post table every search is a full scan of the text.
  - **Do:** an inverted index per `FULLTEXT KEY` (token → posting list of RIDs with term frequency) maintained by `mi_on_insert` / `mi_on_delete` like the other in-memory indexes and written at checkpoint like the B+trees; BM25-style scoring with MySQL's stopword list and `ft_min_word_len` semantics; `IN BOOLEAN MODE` operators over postings; planner uses the index when `MATCH` is in `WHERE`. Rejected: n-gram trigram LIKE acceleration (does not give MySQL's relevance order).
  - **Done when:** relevance order on a fixed corpus matches MariaDB's order for the top 10 of 20 test queries (corpus + expected lists in `tests/fixtures/`); a 100 000-row search reads < 2 % of the pages (`EXPLAIN ANALYZE`).

- [ ] **P4.4 `[COMPAT]` No binary log: no replication source, no point-in-time recovery, `@@log_bin` / `SHOW MASTER STATUS` fail** — `[reproduced]` · **Size:** `XL` · **Deps:** `P1.17`
  - **Where:** missing — `src/query/parser.c:5100` rejects `SHOW BINARY LOGS` / `SHOW MASTER STATUS`; `@@log_bin`, `@@server_id`, `@@binlog_format` unknown (`executor.c:2772-2790`); the WAL (`src/tx/wal.c`) is physical (page / row images by RID), not a logical change stream.
  - **Why:** probe `eng.c`: all three statements error. MariaDB's binary log "contains a record of all changes to the databases" and is used "for replication and point-in-time recovery" (mariadb.com/kb/en/binary-log/). For a local stack the realistic needs are (a) point-in-time restore after a bad migration ("undo the last 10 minutes"), (b) CDC feeds for local dev of Debezium-style consumers, (c) tools that probe `@@log_bin` before dumping [not-verified which ones abort].
  - **Do:** phase 1 — answer the variables (`log_bin = OFF`, `server_id = 1`) and `SHOW MASTER STATUS` empty, so tools proceed; phase 2 — keep archived WAL segments after checkpoints (opt-in, size-capped) + `RESTORE … TO '2026-11-24 10:00'` replaying from the last `BACKUP TO`; phase 3 — a logical row-event stream (ROW format) over the MySQL replication protocol (`COM_BINLOG_DUMP`). Rejected: statement-based binlog (non-deterministic functions, diverges).
  - **Done when:** phase 1: the probe statements answer like a MySQL with `log_bin=OFF`; phase 2: `tests/test_backup.c` restores to a timestamp between two transactions exactly.

- [ ] **P4.5 `[COMPAT]` Admin tools see gaps: `information_schema` does not list itself, 9 of its tables are missing, `SHOW STATUS LIKE` fails, `@@sql_mode` is empty** — `[reproduced]` · **Size:** `M` · **Deps:** `none`
  - **Where:** `src/query/executor.c:6340-6380` (virtual `information_schema` tables), `:2772-2790` (system variables), `src/query/parser.c:5100` (`SHOW` forms).
  - **Why:** probe `eng.c`: `SELECT table_name FROM information_schema.tables WHERE table_schema = 'information_schema'` → 0 rows (DBeaver / HeidiSQL / Workbench browse that schema [not-verified: tools not installed here]); missing `COLLATION_CHARACTER_SET_APPLICABILITY`, `SCHEMA_PRIVILEGES`, `TABLE_PRIVILEGES`, `COLUMN_PRIVILEGES`, `GLOBAL_STATUS`, `FILES`, `INNODB_TRX`, `KEYWORDS`, `ST_GEOMETRY_COLUMNS`; `SHOW STATUS LIKE 'Threads_connected'` → "Unexpected token: LIKE"; `@@sql_mode` = '' (MySQL 8 default is strict, so apps tested on SegVault can break on MySQL with strict-mode errors and vice versa); `@@max_connections` = 64. phpMyAdmin / Adminer were not run in this review (no PHP copy of them available offline) [not-verified].
  - **Do:** generate `information_schema` rows for its own tables; add the missing tables (privileges from `src/bridge/auth.c`, `GLOBAL_STATUS` from `db_engine_stats`, `KEYWORDS` from the lexer table); `SHOW [GLOBAL | SESSION] STATUS [LIKE …]`; an honest `sql_mode` (e.g. `STRICT_TRANS_TABLES,NO_ZERO_DATE,…` matching the actual checks) and per-session `SET sql_mode` that toggles the strict / lenient behaviours already implemented; a CI job that runs Adminer and phpMyAdmin's own test pages against `segvault-server` (P3.13).
  - **Done when:** `tests/test_mysql.c` asserts each listed statement; Adminer 4.8 and phpMyAdmin 5.2 open every page of a sample database without a SQL error (scripted with PHP's built-in client in CI).

- [ ] **P4.6 `[UX]` SQL dump import and export are whole-file in memory: import capped at 256 MB with ≥ 2× the file in RAM, export holds the entire dump; no CLI / `SOURCE` / `LOAD DATA`** — `[code]` · **Size:** `M` · **Deps:** `none`
  - **Where:** `src/widgets/window.c:4291-4303` (`read_whole_file`: grows a buffer to the whole file and gives up at 256 MB), `:4355-4379` (`run_file`: whole text → `mysql_translate` makes a second full copy → `run_text`); export: `dump_database` (`:4383`) appends every table's `CREATE` and every row as `INSERT` text to one in-memory `SBuf` (`:4391`, rows read with `SELECT * FROM t` materialised per table, `:4410`) and writes the file only at the end; `src/server/main.c` has no import option; the parser has no `LOAD DATA` (grep `LOAD DATA` in `src/query/parser.c`, `src/bridge/db_api.c`: none) and the wire does not advertise `CLIENT_LOCAL_FILES` (`src/server/mysql_wire.h:23-35`).
  - **Why:** WordPress / WooCommerce / Magento dumps of 0.5–5 GB are routine when moving a site to a local stack; above 256 MB the GUI reports "Cannot read … : <strerror(errno)>" with an unrelated errno, below it peak memory is file × 2 plus results. XAMPP users import with `mysql < dump.sql` or phpMyAdmin (streaming on the server side) — SegVault has neither a client nor `LOAD DATA`.
  - **Do:** a streaming importer: 1 MB reads, a statement splitter state machine (strings, comments, `/*! */`, `DELIMITER`) shared with `mysql_translate` per statement; worker thread with progress (bytes / total, statements, rows / s), cancel, stop-or-skip on error, resume from byte offset; `.sql.gz` via a small inflate (no zlib dependency) ; `segvault-server --import FILE [--database DB]`; `LOAD DATA [LOCAL] INFILE` with `CLIENT_LOCAL_FILES` on the wire. Rejected: memory-mapping the file (still 2× after translation; 32-bit address limits irrelevant but Windows mapping of > 4 GB files on FAT/USB sticks fails).
  - **Done when:** a generated 2 GB dump imports with RSS < 150 MB and a progress bar, and the GUI dump of a 2 GB database writes the file with RSS < 150 MB (rows streamed per block, P1.14 single-table streaming already exists); `tests/test_mysql_import.c` checks that the streaming splitter fed one byte at a time yields the same statements as the whole-file splitter.

- [ ] **P4.7 `[PHP]` One PHP for every site: no side-by-side versions, no per-site version, and `get-php.cmd` installs an unverified "latest"** — `[code]` · **Size:** `L` · **Deps:** `P2.16`
  - **Where:** `src/server/server.h:48-49` (one `php_cgi` or one `php_fcgi` per server), `:17-23` (`SvSite` = name + target only), `src/server/fastcgi.c:452` (silent cap of 36 `-d` overrides per pool), `:714-716` (auto-detection knows `C:\xampp\php` and `C:\UniServerZ\core\php83`, not Laragon), `packaging/windows/get-php.cmd:10-13` (first of 8.5 / 8.4 / 8.3 that downloads, `…-latest.zip`, no SHA-256 check); documented limit `README.md:229-230`.
  - **Why:** XAMPP ships one PHP per installer (8.3.37, 8.4.28, 8.5.13 are separate downloads, apachefriends.org/download.html); UniServerZ switches 7.0–8.3 globally (SourceForge feature list); Laragon switches globally from "Menu > PHP > Version" and auto-adds the newest PHP (laragon.org/docs/multi-version; release 8.0.0). None documents a per-project version — real projects need it (a legacy WordPress on 8.2/8.3 next to a Laravel 12 app on 8.5; PHP 8.2 security support ends 31 Dec 2026 [not-verified: third-party guide forgekit.tools]).
  - **Do:** `php/<version>/` directories; one FastCGI pool per installed version, started lazily by the first request that needs it; per-site choice (`site.NAME.php = 8.3` in `server.ini`, a `.php-version` file in the site folder, a dropdown in the GUI); a runtime manager that downloads official builds (windows.php.net zips verified against the SHA-256 in `releases.json`; on Linux, distro `php-cgi` binaries found on `PATH` per version) and refuses unverified files; per-version `php.ini` generated with the usual extensions. Rejected: a single global switch (restart, breaks parallel projects).
  - **Done when:** two sites on different PHP versions answer concurrently (`tests/test_php.c` with two fake php-cgi binaries reporting their version); a tampered download is rejected.

- [ ] **P4.8 `[PHP]` No Xdebug integration: no toggle, no IDE configuration** — `[code]` · **Size:** `S` · **Deps:** `P4.7`
  - **Where:** missing (no `xdebug` anywhere in `src/`); pools already pass `-d` options (`src/server/fastcgi.c:452-463`).
  - **Why:** step debugging is the most requested PHP dev feature; XAMPP, UniServerZ and Laragon document no one-click toggle on the pages read (manual `php.ini` editing) [competitor: not documented].
  - **Do:** detect the extension next to each PHP version (`php_xdebug*.dll` / `xdebug.so`), per-site toggle that restarts only that site's pool with `-d zend_extension=xdebug -d xdebug.mode=debug -d xdebug.start_with_request=trigger -d xdebug.client_port=9003`; "Copy VS Code launch.json / PhpStorm path mapping" for the site; optional download of the matching DLL (PHP version / NTS / x64) with hash check. Rejected: Xdebug always on (2–3× slower requests [not-verified]).
  - **Done when:** turning it on for one site leaves other sites' pools untouched (fake php-cgi records its `-d` flags in `tests/test_php.c`); documented in `docs/API.md`.

- [ ] **P4.9 `[UX]` No "Quick App": WordPress / Laravel / Symfony / Drupal must be installed by hand** — `[competitor: laragon.org/docs/quick-app]` · **Size:** `M` · **Deps:** `P4.7`, `P4.21`
  - **Where:** missing (no scaffolding code in `src/widgets/` or `src/server/`).
  - **Why:** Laragon's Quick app creates WordPress, Laravel (composer and zip), Symfony, Drupal or a blank project, the database and a pretty URL in one click, with user-defined recipes in `usr/sites.conf` (laragon.org/docs/quick-app); XAMPP and UniServerZ document nothing comparable. SegVault already has the pieces (folder sites on `NAME.localhost`, HTTPS, PHP pools, SQL engine) but not the workflow.
  - **Do:** recipes in a JSON file (download URL + checksum source, post-steps): WordPress (`latest.tar.gz` + checksum from the official API, `wp-config.php` with a dedicated DB user and random salts), Laravel / Symfony (`composer create-project` when Composer is present, else a verified `composer.phar`), Drupal, blank; create database + user (strong random password), write `.env`, open `https://NAME.localhost`; progress and log in the GUI. Rejected: bundling the applications (package size, stale versions).
  - **Done when:** a scripted run creates a WordPress site whose install page answers 200 and whose `wp-config.php` connects; the Laravel recipe reaches the welcome page when Composer is available.

- [ ] **P4.10 `[OPS]` Dev servers and workers are not managed: `--site app=5173` proxies to a process the user must start** — `[code]` · **Size:** `M` · **Deps:** `none`
  - **Where:** `src/server/server.c` named sites with a proxy target (`Site.phost` / `pport`, set up around `:5046-5070`); `README.md:133` (`--site app=5173` → Vite / Next.js); no process supervision besides php-cgi (`src/server/fastcgi.c`).
  - **Why:** Laravel + Vite projects need `npm run dev`, `php artisan queue:work`, `schedule:work` running next to the site; Laragon manages Node.js, Python, Ruby and Go versions (laragon.org/docs/multi-version). Today SegVault users keep extra terminals open and the proxy shows 502 when they forget.
  - **Do:** per-site `procs` (command, cwd, env, restart policy, expected port) started / stopped with the site, logs into the existing per-service log view, crash-restart with backoff, children killed with SegVault (Windows Job Objects, POSIX process groups); port auto-assignment + proxy registration. Rejected: bundling Node / Python runtimes (size; users already have nvm / pyenv).
  - **Done when:** a test supervises a fake dev server: proxy routes to it, a crash is restarted, stop kills the whole process tree (Linux and wine).

- [ ] **P4.11 `[COMPAT]` No Redis-compatible cache / queue** — `[code]` · **Size:** `L` · **Deps:** `none`
  - **Where:** missing (no RESP server in `src/server/`).
  - **Why:** Laravel projects commonly configure Redis for cache, sessions and queues (Horizon requires it), WordPress uses Redis object-cache plugins; XAMPP and UniServerZ ship no Redis; Laragon's Redis is not mentioned on the pages read [not-verified].
  - **Do:** a built-in RESP2 server on 127.0.0.1:6379 (strings with `EX` / `PX`, hashes, lists with `BLPOP` / `BRPOP`, sets, sorted sets basics, `EXPIRE` / `TTL`, `SELECT`, `FLUSHDB`, pub/sub, `INFO`, `MULTI` / `EXEC`), in memory with optional snapshot at shutdown, shown as a service in the panel. Rejected: bundling Redis / Valkey binaries (no official Windows builds; licence changes of recent Redis releases [not-verified]).
  - **Done when:** phpredis and Predis smoke tests plus Laravel cache / queue tests pass against it in CI.

- [ ] **P4.12 `[NET]` No public sharing (tunnel) for webhooks and phone testing** — `[competitor: laragon.org/docs/quick-share]` · **Size:** `S` · **Deps:** `P4.10`
  - **Where:** missing.
  - **Why:** Laragon's Quick share exposes a project through ngrok ("Menu > www > Share", own authtoken, region choice, QR code; laragon.org/docs/quick-share). Payment / OAuth webhooks and mobile testing need a public URL.
  - **Do:** manage `cloudflared` quick tunnels (no account) or ngrok (authtoken) as supervised processes (P4.10), rewrite the `Host` header to `NAME.localhost`, force a token on the API while shared, optional basic auth, a visible "shared" banner and one-click stop. Rejected: running our own relay service (infrastructure and abuse handling).
  - **Done when:** "Share site" yields a public URL that serves the site while `/api/v1/*` answers 401 without the token.

- [ ] **P4.13 `[UX]` No migration from XAMPP / Laragon / UniServerZ (sites, virtual hosts, PHP settings, databases)** — `[code]` · **Size:** `L` · **Deps:** `P4.6`, `P3.13`
  - **Where:** missing; only PHP auto-detection looks at `C:\xampp\php` and `C:\UniServerZ\core\php83` (`src/server/fastcgi.c:714`).
  - **Why:** the target users already have projects in `htdocs` / `www`, virtual hosts in `httpd-vhosts.conf`, tuned `php.ini`, and data in InnoDB files that SegVault cannot read.
  - **Do:** detect installations; import site folders (reference, not copy), virtual hosts → `site.NAME` entries, key `php.ini` settings, and databases by starting their `mysqld` / `mariadbd` if needed and pulling a logical dump over the MySQL protocol with a built-in client (reuse the `tests/test_server.c` client code) streamed into the importer of P4.6; a report of unsupported objects (events, spatial columns, full-text indexes, `VIRTUAL` columns). Rejected: reading InnoDB files directly.
  - **Done when:** against a MariaDB 10.11 with sakila and a WordPress database, the wizard migrates both with equal row counts and the WordPress site loads.

- [ ] **P4.14 `[UX]` Mail catcher below Mailpit: no live updates, no release to a real SMTP server, no checks** — `[code]` · **Size:** `S` · **Deps:** `none`
  - **Where:** `src/server/mail.c` (SMTP capture, web page with search at `:701`, JSON API); no SSE / WebSocket, relay or retention code in the file (grep).
  - **Why:** Laragon bundles Mailpit, wired into PHP and the dashboard (laragon.org/docs/mailpit; release 8.0.0); XAMPP ships Mercury (a real MTA, Windows only, apachefriends.org/download.html).
  - **Do:** a Server-Sent Events stream for new messages (the server already streams SSE for PHP); "release" of a message to a configured SMTP relay (STARTTLS); HTML and link checks done locally; retention by count / size with pruning; raw / headers / HTML / text tabs. Rejected: embedding Mailpit (Go binary, second UI).
  - **Done when:** `tests/test_mail.c`: an SSE event arrives within 100 ms of the end of `DATA`; release delivers to a local sink.

- [ ] **P4.15 `[UX]` The desktop app is English-only (no Italian), while docs and release notes are Italian** — `[code]` · **Size:** `M` · **Deps:** `none`
  - **Where:** user-visible strings are literals in `src/widgets/*.c` (e.g. `src/widgets/server_view.c` "Start server", "Copy address", "Open SQL editor"); no language setting in `src/widgets/settings.c` (grep `language` / `locale`: none).
  - **Why:** the README and every release note are Italian and the users include Italian schools, but menus, dialogs and errors are English. Laragon, XAMPP and UniServerZ localisation was not verified [not-verified].
  - **Do:** a string table with IDs (`tr(S_SERVER_START)`), Italian and English catalogs compiled in (no gettext dependency, single binary), language from the OS locale with an override in Settings; CI check that draw / status calls receive `tr()` results, plus a pseudo-localised build (+40 % length) to catch truncation in the hand-made layouts.
  - **Done when:** every visible string comes from the table; the Italian build passes a screenshot walk-through of all panels without truncation.

- [ ] **P4.16 `[UX]` No accessibility layer: screen readers see an empty window** — `[code]` · **Size:** `L` · **Deps:** `P4.15`
  - **Where:** custom-drawn widgets (`src/widgets/*`, `src/platform/xlib.c`, `src/platform/win32.c`); no UI Automation / MSAA / AT-SPI code (grep `UIAutomation`, `IAccessible`, `at-spi`: none).
  - **Why:** schools and public bodies must buy accessible software (EU accessibility rules for public procurement [not-verified]); a blind student cannot use the SQL editor or read result grids. Native-control UIs (XAMPP's control panel) get basic accessibility from the OS [not-verified].
  - **Do:** a Windows UI Automation provider in the platform layer for the main widgets (editor as TextPattern, result grid as GridPattern / TablePattern, buttons as Invoke, tree as ExpandCollapse); AT-SPI2 on Linux later; a keyboard-only audit (focus order, visible focus ring) and a high-contrast theme. Labels come from the P4.15 string table.
  - **Done when:** NVDA reads editor text, grid cells and buttons; a keyboard-only walk-through script of every panel passes.

- [ ] **P4.17 `[PLATFORM]` No macOS build (XAMPP ships macOS arm64)** — `[code]` · **Size:** `XL` · **Deps:** `P4.18`
  - **Where:** the GUI platform layer exists only for X11 and Win32 (`Makefile:9-12`, `src/platform/{xlib,win32}.c`); the server code already has `__APPLE__` branches (`src/server/service.c:170,249`, `src/server/tls.c:231,834`) and the Makefile a Darwin linker branch (`Makefile:25-27`), never built in CI (`.github/workflows/ci.yml` uses Ubuntu only); `README.md:229-231` "Nessuna versione nativa per macOS".
  - **Why:** XAMPP 8.x supports macOS on Apple silicon (apachefriends.org/download.html); a large share of web developers use Macs; UniServerZ and Laragon are Windows-only.
  - **Do:** phase 1: build and run the engine / server suites on a `macos-14` runner (arm64), ship `segvault-server` for macOS; phase 2: a Cocoa platform layer (NSWindow, CoreText, CGContext) implementing `src/platform/platform.h`, app bundle, notarization (P4.18), `launchd` service. Rejected: requiring XQuartz.
  - **Done when:** macOS CI green for the engine / server suites; the GUI app runs on arm64 and passes `tests/test_platform.c` equivalents.

- [ ] **P4.18 `[OPS]` Release binaries are unsigned (SmartScreen / antivirus warnings), no SBOM or provenance** — `[code]` · **Size:** `S` · **Deps:** `none`
  - **Where:** `.github/workflows/release.yml:36-48` (build, `make dist`, upload; no signing step); `dist/SHA256SUMS` only.
  - **Why:** an unsigned `segvault.exe` downloaded from GitHub triggers "Windows protected your PC" and antivirus heuristics; Laragon 8 binaries and installer are code-signed (github.com/leokhoa/laragon/releases/tag/8.0.0); XAMPP's FAQ deals with antivirus false positives instead (apachefriends.org/faq_windows.html).
  - **Do:** Authenticode (OV certificate or Azure Trusted Signing) with `osslsigncode` in the Linux release job (MinGW build) and an RFC 3161 timestamp; GitHub artifact attestations (SLSA provenance) and an SPDX SBOM; macOS signing / notarization with P4.17. Rejected: self-signed certificates (no SmartScreen reputation).
  - **Done when:** `osslsigncode verify` passes on every `.exe` in the release job and the attestation verifies with `gh attestation verify`.

- [ ] **P4.19 `[OPS]` No update mechanism (engine, GUI or PHP runtimes)** — `[code]` · **Size:** `M` · **Deps:** `P4.18`, `P4.7`
  - **Where:** missing; the binaries contain no outbound URL at all (grep of `src/` finds only XML namespace URIs), which also backs the zero-telemetry claim.
  - **Why:** local stacks stay on old PHP / OpenSSL for years; Laragon offers auto update to licensed users only (laragon.org/pricing: free tier "No auto update"); XAMPP and UniServerZ document none.
  - **Do:** opt-in check at start (one GET of an Ed25519-signed manifest, public key in the binary, no identifiers sent), download + verify + atomic swap at next start with automatic rollback if the new binary fails its self-test; same channel for PHP security releases (P4.7); portable mode off by default. Rejected: silent background updates; an updater service running as admin.
  - **Done when:** tests against a local manifest server: valid update applies, tampered manifest / binary rejected, failed self-test rolls back.

- [ ] **P4.20 `[DOCS]` No licence file: the README says "educational project" while the engine reports `@@license = MIT`** — `[code]` · **Size:** `S` · **Deps:** `none`
  - **Where:** no `LICENSE` / `COPYING` in the repository root; `README.md:1680-1682` ("Progetto a scopo didattico: puoi studiarlo, farne un fork ed estenderlo"); `@@license = MIT` in `src/query/executor.c:2784` and `src/server/mysql_wire.c:380`; fonts under OFL (`assets/fonts/OFL-*.txt`); PHP downloaded by `packaging/windows/get-php.cmd` (PHP License).
  - **Why:** without a licence nobody has the legal right to redistribute or use the software commercially; package managers (winget, Chocolatey) and schools' procurement require one; the contradictory signals are worse than either choice.
  - **Do:** pick the model (permissive MIT / Apache-2.0, or AGPL-3.0 + commercial licence for a paid edition), add `LICENSE` and a `NOTICE` (fonts OFL, OpenSSL Apache-2.0 when bundled, PHP License for downloaded runtimes), make `@@license` and the About box match; CLA or DCO if dual licensing.
  - **Done when:** `LICENSE` + `NOTICE` exist, `SELECT @@license` and the README agree.

- [ ] **P4.21 `[SECURITY]` Open by default: any local process or OS user logs in as root (MySQL protocol and HTTP API) without a password** — `[code]` · **Size:** `M` · **Deps:** `none`
  - **Where:** `src/server/server.c:3692` (`open_mode`: no token and no `--users` → "qualunque nome entra come root", comment above); `src/widgets/server_view.c` (`token_required`: only LAN / CORS / explicit option turn the token on).
  - **Why:** on shared machines (school labs, terminal servers, multi-user Linux) every OS user can read and change every database and call the API; competitors are similar — Laragon's default root password is empty (laragon.org docs), XAMPP points users to a security console (apachefriends.org/faq_windows.html) — so a secure default is a differentiator, not just parity.
  - **Do:** generate a strong root password at first start (0600 file in the data folder, shown and copyable in the GUI, injected into Quick App configs, P4.9); keep open mode as an explicit choice; on Linux accept the owning OS user without a password over the Unix socket via peer credentials (`SO_PEERCRED`); per-OS-user default data folder.
  - **Done when:** a fresh data folder refuses anonymous root logins from another OS user; peer authentication covered in `tests/test_server.c`.

- [ ] **P4.22 `[UX]` Port conflicts are diagnosed but not resolved** — `[code]` · **Size:** `S` · **Deps:** `none`
  - **Where:** busy-port diagnosis (`README.md:156`: "Porta occupata: chi la usa"); `--port 0` = any free port (`src/server/main.c:130`); no automatic fallback in the GUI (`src/widgets/server_view.c`).
  - **Why:** port 80 / 443 / 3306 / 8080 taken by IIS, Skype, another MySQL service or Docker is the classic first-run failure of local stacks [not-verified as the #1 cause].
  - **Do:** on bind failure offer "use the next free port" (8485…, 3307…, 1026…), persist it, update dashboard and site URLs and the generated app configs; if the holder is an older SegVault, offer to stop it.
  - **Done when:** a test holds 8484 and 3306; the GUI flow picks 8485 / 3307, persists them, and sites answer.

- [ ] **P4.23 `[SECURITY]` Zero telemetry is true but not verifiable: no test or published statement proves it** — `[code]` · **Size:** `S` · **Deps:** `P4.19` (the update check must stay opt-in)
  - **Where:** `src/` contains no outbound URL except XML namespace URIs (`src/widgets/export_bin.c:143-149`, `src/server/service.c:181`); the only outbound connections are user-configured proxy targets (`src/server/proxy.c`) and `get-php.cmd` (`packaging/windows/get-php.cmd:12`); no privacy statement in `README.md`.
  - **Why:** "no telemetry" is a selling point for schools and companies (GDPR, data-protection officers) only if it can be checked; competitors publish no such guarantee on the pages read [competitor: not documented].
  - **Do:** a CI job that runs `segvault-server` and the GUI (Xvfb) through the test suites under `strace -f -e trace=connect,sendto` (Linux) and fails on any non-loopback address that the test did not configure; a short privacy statement (what is never sent; what the opt-in update check of P4.19 sends: nothing but the GET); `--offline` switch that disables every outbound feature at once.
  - **Done when:** the CI job is green and the README states the guarantee with a link to the job.

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

---

## Code review 2026-11-26 — audit tecnico completo (working tree `php-fastcgi` @ `3b66b31` + modifiche non committate)

Riepilogo, 5 priorità e valutazione complessiva: **in cima al file** (sezione "Code review 2026-11-26 — riepilogo").
Le voci già presenti nella roadmap (P1.14, P1.15, P1.17–P1.20, P2.16, P3.9, P3.12, P4.x) non sono ripetute: dove
un problema nuovo le tocca, la voce rimanda al loro numero.

Come è stata fatta: warning estesi di GCC 16 (`-Wnull-dereference -Wduplicated-* -Wlogical-op -Wstack-usage=65536
-Wunused-function -Wmissing-prototypes -Wformat-truncation=2 …`), `gcc -fanalyzer` su ogni file C di `src/`
(37 segnalazioni, verificate a mano una per una), test e sanitizer su una copia dello scratchpad (dentro
`systemd-run` con `MemoryMax` / `TasksMax`), lettura del codice. ID delle voci: `CR-C*` critico, `CR-A*` alto,
`CR-M*` medio, `CR-B*` basso. **[da verificare]** = sospetto non ancora confermato da una prova.

### Critico

- [x] **CR-C1 · I GRANT su nomi con spazi si corrompono al riavvio: un `GRANT SELECT` diventa `ALL PRIVILEGES` su
  un'altra tabella** — `src/bridge/auth.c:280` (scrittura: `"grant %s %s %s %u\n"` senza escape),
  `:338` (lettura: `sscanf(line, "grant %63s %63s %79s %u", …)`), `include/common.h:46-53` (`sv_name_ok` ammette lo
  spazio nei nomi di database e tabelle) · **confermato con una prova**
  - **Problema:** `users.dat` salva ogni permesso come parole separate da spazi, ma i nomi di database e di
    tabella possono contenere spazi (`` CREATE TABLE `t 255` `` è accettato). Al caricamento `%s` si ferma al
    primo spazio e il resto del nome diventa il campo successivo. Prova con `libsegvault.a`:
    ```
    GRANT SELECT ON shop.`t 255` TO bob;  GRANT SELECT ON shop.`order items` TO bob;  GRANT SELECT ON `my db`.* TO bob;
    SHOW GRANTS FOR bob   → i tre GRANT SELECT
    -- db_shutdown(); db_init()
    SHOW GRANTS FOR bob   → GRANT ALL PRIVILEGES ON `shop`.`t` TO bob        (gli altri due spariti)
    ```
    La riga `grant bob shop t 255 1` viene letta come tabella `t`, privilegi `255` (tutti i bit, `AUTH_ALL` = 0x7F).
  - **Perché è critico:** è un'escalation di privilegi persistente. Chi può creare una tabella con un nome scelto
    da lui (`` `orders 127` ``) e ottenere un permesso qualsiasi su di essa (anche solo `SELECT`, concesso da un
    amministratore ignaro) ha `ALL PRIVILEGES` sulla tabella vera `orders` dopo il primo riavvio del server.
    In più, ogni permesso su un database o una tabella con uno spazio nel nome sparisce in silenzio al riavvio
    (utenti che perdono l'accesso dopo un aggiornamento o un riavvio della macchina).
  - **Correzione:** (1) scrivere i nomi in una forma senza ambiguità (percent-encoding come per `sequences.txt`,
    oppure campi separati da TAB con escape) e alzare la versione del file; (2) al caricamento rifiutare le righe
    con campi in più, privilegi fuori da `AUTH_ALL`, o nomi che non rispettano `sv_name_ok`, e segnalare il file
    come danneggiato invece di applicarle; (3) test di regressione in `tests/test_auth.c` con nomi che contengono
    spazi, `%`, `*` e cifre finali, verificati dopo un riavvio.
    ```c
    static void put_name(FILE* f, const char* s) {          // spazi, '%' e controlli codificati: %20 %25 …
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            if (c <= ' ' || c == '%' || c == 0x7F) fprintf(f, "%%%02X", c); else fputc(c, f);
        }
    }
    // caricamento: sscanf con %n per verificare che la riga sia finita, poi decodifica e controllo dei bit
    if (sscanf(line, "grant %63s %190s %190s %u%n", a, b, c, &pr, &end) == 4 && line[end] == '\n'
        && (pr & ~AUTH_ALL) == 0 && name_decode(b) && name_decode(c)) { … }
    ```
  - **Corretto (2026-11-26):** `users.dat` scrive i permessi come righe `grantx` con i nomi codificati (`sv_name_encode`, `include/common.h`); le vecchie righe `grant` si leggono solo se hanno esattamente quattro campi e privilegi dentro `AUTH_ALL`, altrimenti vengono scartate con un avviso (mai reinterpretate). Test in `tests/test_auth.c`: GRANT su `` `t 127` ``, `` `50% off` `` e `` `my db`.* `` uguali dopo il riavvio, nessun permesso su `shop.t`, una riga ambigua della 1.5 ignorata.

### Alto

- [x] **CR-A1 · Race sulla cache delle mappe dell'heap tra letture parallele** — `src/table/heap.c:259-281`
  (`heap_open`), `:202-203` (`meta_from_tail`), `:244-245` (`meta_build`), `:41`, `:47` (`g_heaps`, `g_tails`) ·
  **confermato con ThreadSanitizer**
  - **Problema:** `db_execute_readonly` (`src/bridge/db_api.c:2908-2991`) esegue più SELECT insieme con il lock del
    motore preso **in lettura**. Ogni scansione passa da `heap_open` (`executor.c:5447`, `7677`, `9609`), che
    cerca la mappa dello spazio libero nella lista globale `g_heaps` e, se manca, la costruisce e la aggancia in
    testa (`m->next = g_heaps; g_heaps = m;`) senza alcun mutex. Due lettori sulla stessa tabella (o su tabelle
    diverse) scrivono e leggono la lista insieme. Prova (programma di prova fuori dal repository: 4 tabelle da 3 000
    righe, `db_shutdown` + `db_init`, 8 thread di `SELECT SUM(v)` con `db_execute_readonly`) → 6 data race
    di TSan: `heap_open heap.c:269/270/278` contro `meta_from_tail heap.c:202/203` (stesso lock `M0` in lettura).
    `make test-tsan` non lo vede perché `test_parallel` fa prima gli INSERT nello stesso processo, quindi le
    mappe esistono già quando partono i lettori.
  - **Perché è grave:** è un comportamento indefinito nel cuore del motore. In pratica: (1) mappe doppie per
    la stessa radice; `heap_cache_forget` (`heap.c:91-102`) ne toglie solo la prima, così dopo un TRUNCATE o un
    DROP resta una mappa vecchia che, se quella pagina torna radice di un'altra tabella, risulta valida per una
    catena diversa; (2) su CPU a ordinamento debole (Windows ARM, il futuro macOS arm64 di P4.17) un lettore può
    vedere `g_heaps = m` prima dei campi di `m`; (3) mappe perse (leak). Il nuovo codice P1.15 (`meta_from_tail`,
    non committato) allarga la finestra: ora la mappa si costruisce anche da una coda nota, quindi in più punti.
  - **Correzione:** le letture non hanno bisogno della mappa dello spazio libero, che serve solo agli INSERT.
    Separare le due cose: una `heap_open_scan()` per i lettori che non tocca `g_heaps` e segue solo la catena;
    la mappa si costruisce soltanto in `heap_insert`, che gira sempre con il lock esclusivo. In alternativa,
    un `SvRwLock g_heaps_lock` con ricerca in lettura e inserimento in scrittura, ripetendo la ricerca dopo
    aver preso il lock:
    ```c
    sv_rw_rd(&g_heaps_lock);  m = meta_find(bp, root);  sv_rw_rdunlock(&g_heaps_lock);
    if (!m) {
        HeapMeta* nm = meta_build_detached(bp, root, num_pages);   // costruita fuori dalla lista
        sv_rw_wr(&g_heaps_lock);
        m = meta_find(bp, root);                                   // un altro thread l'ha già messa?
        if (!m && nm) { nm->next = g_heaps; g_heaps = m = nm; nm = NULL; }
        sv_rw_wrunlock(&g_heaps_lock);
        if (nm) meta_free(nm);
    }
    ```
    Test: aggiungere a `test-tsan` il caso "riavvio, poi N lettori in parallelo prima di qualunque scrittura".
  - **Corretto (2026-11-26):** `g_heaps_lock` (`SvRwLock`) in `heap.c`: ricerca in lettura, costruzione con il lock esclusivo ripetendo la ricerca. La prova TSan (8 tipi di query a freddo) non segnala più nulla; nuovo caso "riavvio + 8 lettori a freddo" in `tests/test_parallel.c` (verde sotto TSan).

- [x] **CR-A2 · Ogni sito ospitato può usare l'API SQL di amministrazione: il controllo `Origin` considera "stessa
  origine" qualunque `Host` servito** — `src/server/server.c:2501-2512` (`origin_problem`), `:2752-2757`
  (`route`: l'API risponde su ogni host, anche con PHP), `:2801-2826` (nessuna autenticazione senza token)
  - **Problema:** `origin_problem` accetta una richiesta del browser se `Origin` è `http(s)://` + l'header `Host`
    della richiesta stessa. Ma lo stesso processo serve anche i siti e i vhost (`shop.localhost`, `*.test`,
    cartelle di `www/`), e `route` lascia a SegVault i percorsi `/api/v1/{query,sessions,databases,…}` su
    **ogni** host (`:2757` toglie al sito solo i percorsi che non sono dell'API). Quindi il JavaScript di una
    qualunque pagina locale (un plugin WordPress, un bundle npm di un progetto, una XSS in un'app in sviluppo,
    la pagina di uno studente sul server della classe) può fare
    `fetch('/api/v1/query', {method:'POST', body:'{"sql":"DROP DATABASE wordpress"}'})`: `Origin` e `Host`
    coincidono, il controllo passa, e senza `--token` / `--users` (la configurazione predefinita)
    l'istruzione gira come root su **tutti** i database.
  - **Perché è grave:** P4.21 copre i processi e gli utenti locali; qui l'attaccante è codice che gira nel browser
    dentro un'origine che il server stesso considera fidata. Resta aperto anche dopo P4.21 se la password di
    root protegge solo il protocollo MySQL. Un ambiente di sviluppo esegue per definizione codice di terzi.
  - **Correzione:** (1) servire l'API solo sull'host di amministrazione (`localhost` / `127.0.0.1` / un nome
    dedicato come `segvault.localhost`) e rispondere 404 sugli host dei siti; (2) non contare `Origin == Host` come
    fidato quando quell'host è un sito; (3) chiedere sempre un segreto che le pagine dei siti non possono
    conoscere: un token per istanza (file `0600`, già presente come `server.token`) in `Authorization`, oppure
    un cookie `HttpOnly; SameSite=Strict` emesso solo dalla dashboard sull'host di amministrazione.
    ```c
    static bool admin_host(const SvServer* s, const Request* rq) {
        return !rq->site[0] && is_loopback_host(rq->host);   // nessun sito / vhost risolto per questo Host
    }
    ...
    if (api && !admin_host(s, rq) && !bearer) { json_error(rs, 404, "not found"); return; }
    ```
    Test in `tests/test_web.c`: richiesta con `Host: shop.localhost` e `Origin: http://shop.localhost` a
    `/api/v1/query` → 404/401, la stessa da `Host: localhost` con il token → 200.
  - **Corretto (2026-11-26):** `origin_problem` non considera più fidata la stessa origine sugli host dei siti (`rq->site` valorizzato); sugli host dei siti l'API risponde 403 senza `--token` / `--users` (con credenziali funziona come prima). L'host principale (localhost, `--static`) è invariato. Test in `tests/test_web.c` (POST `/api/v1/query` da `blog.localhost` con e senza `Origin` → 403, da `localhost` → 200).

- [x] **CR-A3 · GUI: backup, restore, lettura e traduzione dei file di import girano sul thread dell'interfaccia** —
  `src/widgets/window.c:1439` (`db_backup` / `db_restore` chiamati dal gestore della barra), `:4291-4312`
  (`read_whole_file`, fino a 256 MB), `:4325-4352` (`import_csv`: lettura + `csv_to_sql` di tutto il file),
  `:4355-4372` (`run_file`: lettura + `mysql_translate` del dump intero), `:4383-…` (`dump_database`: formattazione
  di tutte le righe in `INSERT` sul thread della UI, dopo `ui_exec`)
  - **Problema:** solo l'esecuzione degli statement passa da un worker (`ui_exec`, `run_worker`). Il backup e il
    restore di tutto il database, la lettura di un file da 200 MB, la conversione CSV → SQL e la traduzione di un
    dump phpMyAdmin girano direttamente nel gestore dell'evento: la finestra non si ridisegna, non risponde e non
    ha un "Annulla" per secondi o minuti (su Windows dopo 5 s compare "Non risponde"). Il backup prende anche il
    lock esclusivo del motore per tutta la copia, quindi blocca insieme GUI e siti.
    (Diverso da P3.9 "GUI thread still takes the engine lock", che riguarda le letture del catalogo, e da P4.6, che
    riguarda la memoria.)
  - **Perché è un problema:** è il percorso tipico della classe che importa un dump (segnalazione del
    2026-11-14): sembra un crash, e chiudere la finestra a metà di un restore è il modo più rapido per
    ritrovarsi con dati incompleti.
  - **Correzione:** stesso schema di `ui_exec`, generalizzato: un `ui_job(fn, arg, label)` che esegue `fn` in un
    thread con avanzamento (`atomic` byte letti / righe scritte) e annullamento, e che a fine lavoro manda un
    evento alla finestra; backup, restore, import (lettura + traduzione) e dump diventano job. Per i file grandi,
    lettura a blocchi in streaming invece di `read_whole_file` (vedi P4.6).
    ```c
    typedef struct { void (*fn)(void* arg, UiProgress* p); void* arg; UiProgress prog; atomic_int done, cancel; } UiJob;
    static bool ui_job(AppWindow* app, UiJob* j, const char* label);   // modale come ui_exec, con barra e Annulla
    ```
  - **In parte (2026-11-26):** nuovo `ui_call(fn, arg)` in `window.c` (stesso ciclo modale di `ui_exec`): backup e restore, lettura dei file (`ui_read_file`), conversione CSV → SQL e traduzione dei dump MySQL girano in un thread mentre la finestra si ridisegna. **Resta:** `dump_database` formatta ancora le righe sul thread della UI (le SELECT passano già da `ui_exec`); l'avanzamento è il riquadro "Waiting…" generico, senza percentuale.
  - **Completato (2026-11-26):** anche il dump del database (`dump_job`) ora si costruisce e si scrive nel thread del lavoro; la finestra resta viva e Esc annulla le SELECT del dump.

- [x] **CR-A4 · `LIMIT` / `OFFSET` oltre 2³¹ danno risultati sbagliati in silenzio; `VARCHAR(4294967297)` diventa
  `varchar(1)`** — `src/query/parser.c:2317`, `:2324`, `:2335`, `:2341`, `:2346` (`atoi` in `int limit/offset`,
  `parser.h:268-269`), `:3978` / `:3982` (dimensione e scala della colonna con `atoi`) · **confermato con una prova**
  - **Problema:** i numeri vengono convertiti con `atoi` in un `int`: oltre `INT_MAX` il risultato è indefinito
    (glibc: `(int)strtol`, quindi troncato a 32 bit). Prova su una tabella con le righe 1…10:
    `LIMIT 7, 18446744073709551615` → **0 righe** (MySQL: 8, 9, 10); `LIMIT 18446744073709551615 OFFSET 7` → 0 righe;
    `LIMIT 2147483648` → 0 righe (MySQL: tutte); `LIMIT 4294967298` → 2 righe; `LIMIT 4294967300, 2` → righe 5 e 6
    (MySQL: nessuna). `CREATE TABLE s (v VARCHAR(4294967297))` va a buon fine e `information_schema` dice
    `varchar(1)`.
  - **Perché è grave:** `LIMIT offset, 18446744073709551615` è l'idioma che il manuale di MySQL indica per "dalla
    riga N alla fine" ed è quello che generano gli ORM per un `OFFSET` senza `LIMIT` (per esempio Doctrine
    DBAL): la pagina "tutti i risultati dopo i primi N" torna vuota senza alcun errore. La colonna da 1 carattere
    tronca o rifiuta dati che l'utente crede di poter salvare.
  - **Correzione:** un helper unico per i letterali interi del parser che usa `strtoull` e controlla l'overflow;
    `limit` / `offset` a 64 bit, con i valori oltre `INT64_MAX` portati a "illimitato"; dimensioni delle colonne
    oltre il massimo ammesso → errore come MySQL (1074 "Column length too big").
    ```c
    static bool tok_u64(const Token* t, uint64_t* out) {
        errno = 0; char* end;
        unsigned long long v = strtoull(t->text, &end, 10);
        if (errno == ERANGE || *end || t->text[0] == '-') return false;
        *out = v; return true;
    }
    ...
    uint64_t v; if (!tok_u64(&a, &v)) { parse_fail(ps, "LIMIT value out of range", a); return; }
    n->select_stmt.limit = v > INT64_MAX ? INT64_MAX : (int64_t)v;
    ```
    Test in `tests/test_sql.c` con i sette casi qui sopra (e lo stesso per `UPDATE … LIMIT` / `DELETE … LIMIT`).
  - **Corretto (2026-11-26):** `row_count()` in `parser.c` (`strtoull`, saturazione a `INT32_MAX`: "senza limite" per LIMIT, "oltre la fine" per OFFSET) per LIMIT / OFFSET / FETCH; dimensioni di colonna oltre `INT32_MAX` → "Column length too big". Test in `tests/test_sql.c` (7 casi).

- [x] **CR-A5 · `db_current_database()` restituisce un puntatore dentro il catalogo dopo aver rilasciato il lock:
  use-after-free con DDL concorrente** — `src/bridge/db_api.c:3221-3226` e `:2303-2307` (`return db->name`),
  `src/catalog/schema.c:247` (`grow` → `realloc` dell'array `databases`), `:1170` (`memmove` in `catalog_drop_db`);
  chiamanti: 16 in `src/widgets/window.c` (per esempio `:114`, `:145`, `:152-156` in `update_session`, `:4384`), 13 in
  `src/server/server.c`, 1 in `src/widgets/schema_view.c`; simile: `db_session_user()` `:3033` (buffer globale
  letto senza lock) · **confermato con ThreadSanitizer**
  - **Problema:** la funzione prende il lock del motore, prende `catalog_current_db(...)->name` e lo restituisce dopo
    `db_unlock()`. Il nome vive dentro `Catalog.databases`, un array di strutture che `CREATE DATABASE` rialloca e
    `DROP DATABASE` compatta. Se un altro thread esegue DDL (la GUI ospita nello stesso processo il server HTTP e
    MySQL, "modalità API server"), il chiamante legge memoria liberata o il nome di un altro database. Prova: un
    thread ripete `db_current_database()` + `strlen`, un altro alterna `DROP DATABASE` del primo database e
    `CREATE DATABASE` → TSan segnala 3 race: con `realloc` (`schema.c:205`, l'array vecchio viene liberato), con
    `memmove` (`schema.c:1170`), con `strncpy` (`schema.c:1158`).
  - **Perché è grave:** nella GUI il puntatore viene poi usato per decidere su quale database lavorare
    (`update_session` → `schema_view_show_db(app->schema, db, …)`, `dump_database` → `snprintf(dbname, …, db)`):
    nel caso migliore un crash, nel peggiore un'azione (dump, Designer, `USE`) sul database sbagliato.
  - **Correzione:** l'API non deve mai restituire memoria del motore oltre il lock. Copia nel buffer del chiamante:
    ```c
    // db_api.h
    bool db_current_database_copy(char* out, size_t n);   // false se nessun database
    // db_api.c
    bool db_current_database_copy(char* out, size_t n) {
        db_lock();
        const char* r = db_current_database_impl();
        bool ok = r && snprintf(out, n, "%s", r) < (int)n;
        db_unlock();
        if (!ok && n) out[0] = '\0';
        return ok;
    }
    ```
    e sostituire le 30 chiamate; lo stesso per `db_session_user()`. Test: la prova qui sopra in `make test-tsan`.
  - **Corretto (2026-11-26):** `db_current_database()` copia il nome sotto il lock in quattro buffer del thread a rotazione (stessa firma, nessun chiamante da cambiare); `db_session_user()` copia sotto il lock. Test TSan in `tests/test_sessions.c` (lettore + DDL concorrente).

- [x] **CR-A6 · Una `REGEXP` con backtracking esponenziale bloccava il server per minuti (e rispondeva "nessuna
  corrispondenza")** — `src/query/regex.c` (`MAX_STEPS` 2·10⁶ per ricerca, poi `false`), `src/query/executor.c`
  (`REGEXP_LIKE` / `REPLACE` / `SUBSTR` / `INSTR` / `COUNT`) · trovato da questa revisione (il fuzzer `regex` faceva solo
  4 600 input al minuto) e **confermato con una prova**
  - **Problema:** il limite di passi valeva per ogni singola ricerca: `SELECT COUNT(*) FROM t WHERE s REGEXP '(a*)*b'`
    su 2 000 righe da 30 caratteri impiegava **38 s** (20 ms × riga), con il lock del motore preso, quindi con tutto
    il server fermo (P1.17); su 100 000 righe circa mezz'ora. Ogni utente SQL poteva farlo. E il risultato era
    sbagliato senza avviso: al limite la ricerca diceva "nessuna corrispondenza".
  - **Corretto (2026-11-26):** al limite di passi lo statement fallisce con "Regular expression too expensive for this
    value (backtracking limit)" (come `ER_REGEXP_TIME_OUT` di MySQL) e le scansioni si fermano subito
    (`sv_scan_abort()` in `heap.c`, azzerato a ogni statement); il matcher controlla anche `KILL QUERY` / il timeout
    ogni 8 192 passi. Prova: 38 s → 21 ms. Test in `tests/test_sql.c` (1 000 righe, errore in meno di 2 s, le
    scansioni successive funzionano).

### Medio

- [x] **CR-M1 · Il motore ha bisogno di più di 1 MB di stack per 14 sottoquery annidate: crash prima del limite di
  profondità** — `src/query/executor.c:7215` (`char names[SV_MAX_COLUMNS][64]` = 64 KB in `plan_prepare_ex`,
  ricorsiva, frame 71 KB), `:13488-13527` (`exec_returning`: `char names[SV_MAX_COLUMNS][128]` + altri array =
  frame da **168 KB**), `:2214`, `:10335`, `:11568`, `:12923` (altri array da `SV_MAX_COLUMNS` sullo stack);
  `src/bridge/db_api.h` (nessun requisito di stack documentato) · **confermato con una prova**
  - **Problema:** con P1.11 `SV_MAX_COLUMNS` è passato da 64 a 1024 e gli array locali dimensionati su di lui sono
    cresciuti 16 volte. `-Wstack-usage=65536` ne segnala 15 sopra i 64 KB. Prova: `SELECT a FROM v1 WHERE a IN
    (SELECT … IN (…))` con N livelli, eseguita in un thread con stack di dimensione fissata: con 8 MB il motore
    rifiuta da solo oltre ~14 livelli ("nested too deeply"); con **1 MB, a 14 livelli il processo va in crash**
    (stack overflow, core dump); con 2 MB passa.
  - **Perché è un problema:** server (`SV_THREAD_STACK` 8 MB) e GUI (worker da 16 MB) sono al sicuro, ma
    `libsegvault.a` è presentata "come SQLite" da incorporare: chi chiama `db_execute` da un thread con lo stack
    predefinito (1 MB su Windows, 512 KB sui thread secondari di macOS, 128 KB con musl) va in crash con una
    query lecita. Lo stesso vale per il ripiego di `ui_exec` sul thread principale (`window.c:919-924`) e per il
    thread principale di Windows (1 MB, nessun `-Wl,--stack` nel Makefile).
  - **Correzione:** (1) togliere dallo stack gli array da `SV_MAX_COLUMNS` (allocazione dinamica dimensionata su
    `ts->num_columns`, o un'arena per statement); (2) aggiungere un controllo dello stack rimasto all'ingresso delle
    funzioni ricorsive (`plan_prepare_ex`, `eval_expr`, `query_exec_inner`): base dello stack salvata per thread
    in `db_execute`, errore "query too complex" sotto una soglia, invece del crash; (3) documentare in `db_api.h`
    lo stack minimo; (4) un test CI che rifà la prova su un thread da 1 MB.
    ```c
    static _Thread_local char* t_stack_base;              // impostata all'ingresso di db_execute
    static bool stack_low(void) { char here; return t_stack_base && (size_t)(t_stack_base - &here) > g_stack_budget; }
    ...
    if (stack_low()) { snprintf(err, errn, "Query is too complex for this thread's stack"); return NULL; }
    ```
  - **Corretto (2026-11-26):** i nomi di `NATURAL` / `USING` in `plan_prepare_ex` sono puntatori (8 KB invece di 64 KB); gli array per colonna di `exec_returning` stanno in un blocco nell'heap (il frame da 168 KB non c'è più). Con 1 MB e 512 KB di stack il motore arriva al suo limite e rifiuta la query pulito; con 256 KB no: requisito minimo di 512 KB scritto in `db_api.h`. Non aggiunto: il controllo dello stack rimasto a runtime.

- [~] **CR-M2 · Le funzioni SQL vengono cercate per nome a ogni riga: catene di 230 `strcmp`** —
  `src/query/executor.c:4766-4776` (`eval_func` → `eval_internal` `:2812` → `eval_sequence` → `eval_more` `:4424`
  → `eval_mysql` `:3644`)
  - **Problema:** l'AST tiene il nome della funzione come testo (`ASTExpr.func[32]`, `parser.h:106`); il binder lo
    cerca una volta in `find_func` (`:2513`) per controllare l'arità, ma la valutazione rifà la ricerca a ogni
    chiamata e per ogni riga, scorrendo funzioni di centinaia di righe fatte di `if (!strcmp(f, "…"))`:
    91 confronti in `eval_more`, 146 in `eval_mysql`, 223 nel file. Una funzione che sta in fondo alla catena
    (o una funzione definita dall'utente che non c'è) costa centinaia di `strcmp` per riga. Le stesse catene
    sono anche il punto più difficile da mantenere del file (`eval_mysql` = 688 righe, la funzione più lunga del
    progetto).
  - **Perché è un problema:** costo per riga nel percorso caldo di ogni SELECT con espressioni, e una struttura
    in cui aggiungere una funzione vuol dire indovinare in quale delle quattro catene metterla.
  - **Correzione:** risolvere la funzione una volta sola nel binder e salvare nell'AST un identificatore
    (`e->fid`, enum generato dalla tabella `FuncDef`) o direttamente un puntatore a funzione; valutare con uno
    `switch (e->fid)` oppure con `fd->eval(e, c, out)`. Le catene diventano una tabella:
    ```c
    typedef void (*FuncEval)(ASTExpr*, EvalCtx*, EvalVal*);
    typedef struct { const char* name; int min_args, max_args; FuncEval eval; } FuncDef;
    static const FuncDef FUNCS[] = { {"UPPER", 1, 1, fn_upper}, {"CONCAT", 1, -1, fn_concat}, … };
    // bind:  e->fdef = find_func(e->func);      eval:  e->fdef->eval(e, c, out);
    ```
  - **In parte (2026-11-26):** `FIS("NOME")` al posto di `!strcmp(f, "NOME")` in `eval_internal` / `eval_sequence` / `eval_more` / `eval_mysql` / `eval_func` / `eval_agg_extra` (266 confronti: prima la lunghezza costante e il primo carattere, poi `memcmp`) e l'appartenenza all'elenco `MINE` di `eval_mysql` memorizzata nel nodo (`ASTExpr.fmine`: prima 115 `strcmp` per riga per ogni funzione fuori elenco). Misura su 200 000 righe: `ABS(n)` 101 → 20 ms, `UPPER(s)` 99 → 25 ms, `JSON_VALID` 99 → 16 ms, `COALESCE` 85 → 18 ms. **Resta:** la tabella `FuncDef` con puntatori a funzione (le catene esistono ancora, ma costano poco). La regex non si ricompila più a ogni riga (CR-B9).

- [x] **CR-M3 · Protocollo MySQL: `COM_STMT_SEND_LONG_DATA` accumula senza limite** — `src/server/server.c:4237-4252`
  (`my_long_data`), `:4114` (`MAX_STMTS` 4096)
  - **Problema:** ogni pacchetto è limitato (`max_body`, 16 MB), ma i pezzi di un parametro si concatenano con
    `realloc(d->p, d->n + add + 1)` senza un totale massimo, finché non arriva un `EXECUTE`. Un client autenticato
    (in modalità aperta qualunque processo locale, P4.21) manda pacchetti da 16 MB all'infinito per un parametro, o
    per 65 535 parametri di 4 096 statement, e il server cresce fino a esaurire la RAM; con la politica "OOM =
    abort" del motore (`include/sv_alloc.h`) o con l'OOM killer di Linux cade tutto il processo (siti, MySQL, GUI).
  - **Correzione:** un tetto come `max_long_data_size` / `max_allowed_packet` di MySQL (per parametro e per
    connessione) e un errore `ER_NET_PACKET_TOO_LARGE` (1153) al superamento; liberare i dati dello statement
    quando lo si chiude o resetta (`COM_STMT_RESET`).
    ```c
    if (d->n + add > s->cfg.max_body) { d->too_long = true; return; }   // l'EXECUTE successivo risponde 1153
    ```
  - **Corretto (2026-11-26):** totale dei dati lunghi per statement limitato a `max_body` (come `max_allowed_packet`): oltre, i dati si scartano e il prossimo EXECUTE risponde 1153.

- [x] **CR-M4 · Editor SQL: ogni passo di annulla copia tutto il testo (fino a 4 GB) e i fallimenti di `malloc`
  diventano crash** — `src/widgets/textbox.c:171-205` (`snapshot_take`, `stack_push`, `undo_push`), `:207-212`
  (`restore_snapshot`), `:66-70` (`rebuild_lines`), `src/widgets/textbox.h:26` (`TEXTBOX_UNDO_CAP 256`),
  `src/widgets/window.c:450` (file fino a 16 MB nell'editor)
  - **Problema:** l'annulla salva un'istantanea completa del testo a ogni modifica non accorpata: con un dump da
    16 MB aperto nell'editor, 256 passi = **4 GB** di RAM, e ogni tasto premuto dopo una pausa di 1,5 s copia
    16 MB sul thread della UI. Inoltre `snapshot_take` lascia `s.text = NULL` se `malloc` fallisce e
    `restore_snapshot` fa poi `strlen(NULL)`; `rebuild_lines` e `restore_snapshot` usano `p = realloc(p, …)`
    senza controllo (crash e perdita del buffer vecchio).
  - **Correzione:** annulla per differenze (posizione, testo tolto, testo inserito: memoria proporzionale alle
    modifiche) oppure un budget in byte per la pila oltre al numero di passi; controllare ogni allocazione
    (un'istantanea mancata = passo non registrato, non un crash).
    ```c
    typedef struct { int pos; char* removed; int nrem; char* inserted; int nins; int cursor, anchor; } EditOp;
    ```
  - **Corretto (2026-11-26):** **Correzione della diagnosi:** l'editor tronca il testo a `TEXTBOX_MAX_LEN` = 64 KB, quindi il caso peggiore dell'annulla è 256 × 64 KB per pila (circa 32 MB in tutto), non 4 GB; i 16 MB di `window.c:450` sono il file delle query salvate. Corretti i crash: un'istantanea non riuscita non entra nella pila, `restore_snapshot` e `rebuild_lines` controllano `realloc`.

- [ ] **CR-M5 · Il motore è un singleton di processo: circa 100 variabili globali, nessun handle nell'API** —
  `src/bridge/db_api.c:59-107` (`g_db_rw`, `g_db`, sessioni, `g_open_tx` …: 37 globali `g_*`, 6 thread-local),
  `src/query/executor.c` (43 globali, 27 `_Thread_local`: `g_uvars` `:2665`, `g_cap`, `g_ij*` `:1855-1861`,
  `g_mem_indexes`, `g_describe`, `g_infoschema`…), `src/table/heap.c:41-49`, `src/tx/wal.c`, `src/bridge/auth.c`,
  `src/bridge/locks.c:80-88`; API: `db_init(const char* dir)` / `db_execute(const char* sql)` in `db_api.h`
  - **Problema:** tutto lo stato del motore vive in variabili statiche di file; l'API pubblica non ha un oggetto
    `Db*`. Le regole di concorrenza sono scritte solo nei commenti ("Protezione: … sotto db_lock", "chiamata con
    g_mi_lock in scrittura") e ogni modulo le applica a mano: CR-A1 nasce proprio da una lista globale
    (`g_heaps`) che nessun lock possedeva, toccata da un percorso (le letture parallele) arrivato dopo. Le 27
    variabili `_Thread_local` dell'executor sono parametri impliciti passati tra funzioni (`g_describe`,
    `g_see_db`, `g_cap`…), quindi il comportamento di una funzione dipende da chi l'ha chiamata prima sullo
    stesso thread.
  - **Perché è un problema:** `libsegvault.a` ("come SQLite") non può aprire due database nello stesso processo;
    i test devono girare in processi separati e con cartelle fisse in `/tmp` (`test_parallel.c:125`); ogni
    nuova funzione concorrente richiede di ricontrollare a mano l'intero insieme di globali.
  - **Correzione (graduale):** (1) raccogliere lo stato in strutture esplicite (`SvEngine { Catalog*, BufferPool*,
    WAL*, HeapCache, MemIndexCache, LockTable … }`) con il lock che protegge ogni campo scritto accanto;
    (2) un `SvEngine*` passato all'executor dentro `ExecCtx` invece dei `_Thread_local`; (3) API con handle
    (`SvDb* sv_open(path)`, `sv_exec(SvDb*, sql)`), lasciando `db_*` come wrapper sul database predefinito;
    (4) annotazioni di thread-safety (`__attribute__((capability))` con clang `-Wthread-safety`) sui wrapper di
    mutex di `platform_lock.h`, così il compilatore segnala un campo letto senza il suo lock.

- [ ] **CR-M6 · File e funzioni "god object": un sesto del codice in `executor.c`, la finestra in un'unica struttura
  da 116 campi** — `src/query/executor.c` (13 660 righe, 685 KB), `src/widgets/window.c` (6 255 righe) +
  `window.h:83-243` (`AppWindow`), `src/server/server.c` (5 490 righe), `src/query/parser.c` (5 464),
  `src/bridge/db_api.c` (3 462)
  - **Problema:** `executor.c` contiene valutazione delle espressioni, binder, planner e riordino dei join,
    DML, DDL, `ALTER TABLE`, trigger, interprete delle routine, `information_schema`, `SHOW`, JSON, sequenze e
    il diario degli indici; `server.c` contiene HTTP/1.1, API JSON, login e throttling, sessioni, dashboard,
    metriche, siti PHP e tutto il server MySQL; `window.c` gestisce editor, risultati, albero, transazioni,
    impostazioni, import/export, cronologia, ricerca e sostituzione, sincronizzazione del Designer. 17 funzioni
    superano le 200 righe (e le righe sono dense, spesso con più istruzioni ciascuna): `eval_mysql` 688
    (`executor.c:3644`), `parse_statement` 508 (`parser.c:4740`), `eval_more` 341, `backup_thread` 337
    (`server/main.c:102`), `arm_exec` 320, `ctx_run` 313 (`window.c:3518`), `parse_special_form` 303,
    `lexer_tokenize` 299, `schema_view_event` 296, `app_window_handle_event` 272, `infoschema_open` 265 (annidamento
    8). I 23 `-Wredundant-decls` (prototipi ripetuti a metà file, per esempio `executor.c:12906-12907`,
    `:13328`) sono il sintomo: il file è troppo lungo per essere ordinato.
  - **Perché è un problema:** revisione e test mirati impossibili (P3.9 è aperto da due settimane proprio perché
    questi file "non sono stati letti riga per riga"); ogni modifica ricompila 685 KB; conflitti di merge
    garantiti; nessun confine che impedisca, per esempio, a un pezzo dell'interprete delle routine di usare lo
    stato del planner.
  - **Correzione:** dividere per responsabilità, mantenendo `static` dentro ogni nuovo modulo: `query/eval*.c`
    (funzioni per famiglia, vedi CR-M2), `query/plan.c`, `query/join.c`, `query/dml.c`, `query/ddl.c`,
    `query/routine.c`, `query/infoschema.c`; `server/http.c`, `server/api.c`, `server/mysql_server.c`,
    `server/sessions.c`, `server/login_throttle.c`; per la GUI un "controller" per pannello (editor, risultati,
    albero, impostazioni) e un `AppState` senza widget dentro. Obiettivo misurabile: nessun file sopra le 3 000
    righe, nessuna funzione sopra le 150 (un controllo in CI con lo script che ha prodotto questi numeri).

- [x] **CR-M7 · `read_whole_file` scambia un errore di lettura per la fine del file: import troncati in silenzio** —
  `src/widgets/window.c:4304-4306`, messaggi a `:4330-4333` e `:4358-4360`
  - **Problema:** il ciclo termina su `fread(...) == 0` senza guardare `ferror(f)`: un errore di I/O a metà file
    (chiavetta estratta, share di rete, settore illeggibile) restituisce i byte letti fin lì come se fossero il
    file intero, e `run_file` / `import_csv` eseguono un import parziale con esito "OK". Quando invece si supera il
    limite di 256 MB, la funzione restituisce `NULL` senza impostare `errno` e il messaggio diventa
    "Cannot read …: Success".
  - **Correzione:**
    ```c
    if (got == 0) { if (ferror(f)) { free(buf); buf = NULL; errno = EIO; } break; }
    ...
    if (cap >= ((size_t)256 << 20)) { free(buf); buf = NULL; errno = EFBIG; break; }
    ```
  - **Corretto (2026-11-26):** `ferror` controllato (un errore di lettura non è più un file corto) ed `errno` impostato (`EIO`, `EFBIG`).

- [x] **CR-M8 · Windows: eseguibili e DLL cercati in cartelle che qualunque utente locale può creare** **[da verificare]**
  (solo Windows: letto, non eseguito)
  — `src/server/fastcgi.c:714` (`C:\php\php-cgi.exe`, `C:\xampp\php\php-cgi.exe`, `C:\UniServerZ\…`),
  `src/server/cgi.c:216` (`C:\Strawberry\perl\bin\`, `C:\xampp\perl\bin\`, `C:\Perl64\bin\`, `C:\Ruby33-x64\bin\`),
  `src/server/tls.c:31` + `:226-227` (`LoadLibraryA("libssl-3-x64.dll")` per nome semplice come ultimo tentativo)
  - **Problema:** su Windows gli utenti autenticati possono creare cartelle nella radice di `C:` (ACL predefinita).
    Se SegVault non trova PHP accanto a sé né nel `PATH`, prova `C:\php\php-cgi.exe` e simili: chi crea per primo
    quella cartella sceglie il programma che SegVault eseguirà con l'account di chi lo avvia. Lo stesso vale per gli
    interpreti CGI e, con `LoadLibraryA` per nome, per una `libssl-3-x64.dll` messa nella cartella corrente o in una
    voce del `PATH` scrivibile (ricerca standard delle DLL). P2.10 ha già tolto la cartella corrente dalla ricerca
    di `php-cgi`; questi percorsi "noti" riaprono lo stesso problema.
  - **Perché è un problema:** il pubblico dichiarato sono i laboratori scolastici (PC condivisi): uno studente prepara
    `C:\php\php-cgi.exe` e il codice gira con l'account del compagno o del docente che avvia SegVault dopo di lui.
  - **Correzione:** usare i percorsi "noti" solo se la cartella appartiene a un amministratore o a SYSTEM (non
    scrivibile dall'utente corrente: `GetNamedSecurityInfo` + controllo del proprietario), oppure eliminarli e
    chiedere il percorso nella GUI; caricare OpenSSL solo per percorso completo con
    `LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)` e chiamare
    `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` all'avvio di `segvault.exe` / `segvault-server.exe`.
  - **Corretto (2026-11-26):** `sv_win_trusted_path()` in `win_acl.c`: proprietario del file e di ogni cartella del percorso fra Amministratori, SYSTEM, TrustedInstaller o l'utente corrente. Applicato a `php-cgi` (PATH e percorsi noti), agli interpreti CGI (`SearchPathA` compreso) e a OpenSSL, che ora si carica solo per percorso completo (`LoadLibraryExA` con `LOAD_WITH_ALTERED_SEARCH_PATH`, cartelle del PATH controllate). Compilato con MinGW senza warning; **da provare su Windows vero**.

- [x] **CR-M9 · Test: tanti e utili per il motore, ma con percorsi fissi in `/tmp`, un'infrastruttura copiata 21 volte
  e nessun test della finestra principale** — `tests/*.c` (38 percorsi `"/tmp/…"` fissi in 23 file, per esempio
  `tests/test_parallel.c:125` `rm -rf /tmp/sv_par_test`; `mkdtemp` solo in `test_web.c:238`, `test_mysql.c:50`,
  `test_tls.c:83`), macro `CHECK` ridefinita in 21 file, nessun test collega `src/widgets/window.c`
  - **Situazione:** 26 suite e circa 4 000 controlli, tutti verdi su questo albero (`make test` e `make test-asan`
    rieseguiti per questa revisione), più fault injection per la durabilità, TSan, fuzzing e CI con MinGW + wine:
    sopra la media per un progetto di questa dimensione. I buchi sono nell'organizzazione:
    1. **percorsi fissi in `/tmp` con `rm -rf` iniziale:** due esecuzioni insieme (due checkout, `make -j`, CI
       condivisa) si cancellano i dati a vicenda; su una macchina multiutente un altro utente può creare prima
       `/tmp/sv_par_test` (o un link simbolico con quel nome) e far fallire o deviare il test;
    2. **infrastruttura duplicata:** ogni file ha i suoi `CHECK`, `run()`, la sua preparazione della cartella e il suo
       riepilogo; non c'è un modo comune per eseguire un solo caso o per avere un output leggibile da una macchina;
    3. **le classi di bug trovate qui non hanno test:** riavvio + lettori paralleli a freddo (CR-A1), API del motore
       usata mentre un altro thread fa DDL (CR-A5), permessi rivisti dopo un riavvio con nomi "strani" (CR-C1), limiti
       numerici del parser (CR-A4), stack piccolo (CR-M1);
    4. **la GUI è testata solo nei moduli estratti** (`grid_edit`, `table_design`, `schema_model`, `db_tools`,
       `platform`): `window.c`, cioè la logica di stato (transazione, database corrente, job), non ha test (vedi CR-M6).
  - **Correzione:** un `tests/harness.h` unico (`CHECK`, `CHECK_EQ_STR`, cartella temporanea con `mkdtemp`
    cancellata a fine test, filtro per nome del caso, riepilogo TAP); convertire i 23 file un po' alla volta;
    aggiungere i casi di regressione delle voci CR-*; estrarre da `window.c` un `AppState` senza widget (CR-M6) per
    testarne le transizioni senza X11.
    ```c
    // tests/harness.h
    static char g_tmp[256];
    static const char* test_tmpdir(void) {
        if (!g_tmp[0]) { snprintf(g_tmp, sizeof g_tmp, "%s/svtest-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
                         if (!mkdtemp(g_tmp)) abort(); }
        return g_tmp;
    }
    ```

  - **Corretto (2026-11-27), punti 1-3:** `tests/harness.h` comune (`CHECK`, `test_enter_tmpdir`, `test_src`,
    `test_summary`). Ogni suite entra all'avvio in una cartella privata creata con `mkdtemp` (0700, in `$TMPDIR` o
    `/tmp`) e cancellata all'uscita (`SV_TEST_KEEP=1` la conserva; i processi figli non cancellano nulla): i 38
    percorsi fissi `/tmp/sv_*` sono diventati relativi, quindi due esecuzioni insieme non si toccano e nessuno può
    preparare prima la cartella o un link con quel nome. La macro `CHECK` duplicata è sparita da 20 file (`test_sql`,
    `test_btree`, `test_engine`, `test_examples`, `test_decimal` hanno contatori propri e usano solo la cartella;
    `test_win` resta com'era, gira con MinGW). I file del repository (esempi, fixture 1.3.0) si leggono con
    `test_src`. Regressioni delle voci CR-*: C1 (`test_auth`), A1 (`test_parallel`, lettori a freddo), A4
    (`test_sql`), A5 (`test_sessions`) c'erano già; aggiunto CR-M1 in `test_parallel` (sottoquery annidate da 1 a
    30 livelli su un thread con 512 KB di stack; provato che con 128 KB il test va in crash). Dopo `make test` non
    resta nessuna cartella né nel repository né in `/tmp`.
  - **Resta (con CR-M6):** il punto 4, test della logica di `window.c`, richiede prima l'estrazione di un `AppState`
    senza widget.
- [x] **CR-M10 · Le transazioni abbandonate delle sessioni HTTP scadono solo quando arriva un'altra richiesta
  all'API: fino ad allora tengono i lock (anche quello globale `*`)** — `src/server/server.c:96-97`
  (`SESSION_IDLE_MS`, `TX_IDLE_MS` "una transazione ferma si annulla (non blocca gli altri)"), `:806-817`
  (`expire_sessions`), chiamata solo da `:833` (nuova sessione), `:990` e `:1009` (una query HTTP); il ciclo del
  server `:5243-5254` fa ogni 200 ms `db_sync_tick` e `audit_sync_tick`, ma non la scadenza delle sessioni
  - **Problema:** un client dell'API apre una sessione, fa `BEGIN` e un `UPDATE` (lock X sulla tabella, lock S
    globale `*` tenuto fino al COMMIT, `db_api.c:1479-1491`) e poi sparisce (crash, rete, script interrotto).
    Il commento promette l'annullamento dopo 30 s, ma la scadenza la fa solo la prossima richiesta all'API HTTP. Se
    il resto del traffico passa dal protocollo MySQL (il caso di un sito PHP), nessuno la fa partire: ogni scrittura
    su quella tabella fallisce con 1205 dopo l'attesa del lock, e ogni DDL (che chiede `*` in modo X) aspetta per
    sempre, finché qualcuno non chiama l'API o non si riavvia il server.
  - **Correzione:** chiamare `expire_sessions` dal ciclo periodico del server, con `exec_mx` preso come negli altri
    punti, e aggiungere un test che apre una transazione via API, aspetta `TX_IDLE_MS` e verifica che un client
    MySQL possa scrivere sulla tabella senza mandare richieste HTTP.
    ```c
    db_sync_tick();
    audit_sync_tick(s);
    if (now_ms() - s->last_expire_ms > 1000) {          // al massimo una volta al secondo
        s->last_expire_ms = now_ms();
        exec_begin(s); expire_sessions(s); exec_end(s);
    }
    ```
  - **Corretto (2026-11-26):** thread `reaper_main` in `server.c` che ogni secondo chiama `expire_sessions` (con `exec_mx`), avviato da `sv_server_start` e atteso da `sv_server_free`. Nessun test automatico (richiederebbe 30 s di attesa).

- [ ] **CR-M11 · Confine GUI ↔ logica: un terzo di `src/widgets/` non è interfaccia, e GUI e motore parlano solo
  costruendo testo SQL** — `src/widgets/{csv_import,mysql_import,result_export,export_bin,png_write,sql_text,
  table_design,db_tools,grid_edit}.c` (circa 2 700 righe senza alcun riferimento a `platform.h` / `widget.h`),
  `Makefile:63` e `:101` (`segvault-server` compila `src/widgets/png_write.c` e `src/widgets/sql_text.c`),
  `src/widgets/window.c:4383-…` (`dump_database`: generazione del dump dentro la finestra), `src/widgets/sql_quote.h`
  (nuovo, non tracciato: quoting degli identificatori lato GUI)
  - **Problema:** import CSV e MySQL, export, generazione di DDL (`table_design.c`) e di `UPDATE`
    (`grid_edit.c`), suddivisione degli script (`sql_text.c`) sono logica applicativa senza interfaccia, ma stanno
    nella cartella dei widget; il server li compila da lì. Il dump del database (ordine di tabelle, dati, chiavi
    esterne, viste, trigger) è scritto in `window.c` e quindi non è disponibile né all'API né alla CLI. Inoltre
    la GUI comanda il motore componendo testo SQL (`USE …`, `GRANT …`, `ALTER TABLE …`): ogni nome passa da
    `sql_ident`, e la injection di identificatori corretta il 2026-11-24 (P3.11) è nata esattamente da questo
    accoppiamento a stringhe.
  - **Perché è un problema:** la struttura delle cartelle non dice più chi dipende da chi; la logica utile al
    server resta legata alla GUI; il codice testabile senza display è mescolato a quello che non lo è (CR-M9).
  - **Correzione:** spostare questi moduli in `src/app/` (o `src/services/`) senza dipendenze da `platform` /
    `widgets`, con test propri; portare `dump_database` lì come `sv_dump_database(db, writer)` riusato da GUI, API
    (`GET /api/v1/databases/x/dump`) e CLI; per le operazioni amministrative più usate dalla GUI (USE, GRANT,
    REVOKE, DROP) aggiungere funzioni tipizzate in `db_api.h` (`db_use(const char* db)`, `db_grant(...)`) invece di costruire
    testo SQL (la vecchia `db_use` inutilizzata è stata rimossa con CR-B1).

- [x] **CR-M12 · `UPPER` / `LOWER`, `LIKE` e le collation `_ci` ignoravano le lettere non ASCII** —
  `src/query/executor.c` (`ci_cmp`, `ci_fold`, `UPPER`/`LOWER`, `like_match_esc`, hash di GROUP BY / JOIN, chiavi degli
  indici `_ci`) · **confermato con una prova**
  - **Problema:** tutto passava da `toupper` / `tolower`: `UPPER('Città')` = `CITTà`; in una tabella `utf8mb4_unicode_ci`
    `'CITTÀ' = 'città'` era falso (e un `UNIQUE` lasciava entrare entrambe); in `LIKE` il carattere `_` valeva un byte,
    quindi `'à' LIKE '_'` era falso. Per un prodotto usato in Italia è un errore visibile.
  - **Corretto (2026-11-26):** nuovo `src/query/u8case.h` (ASCII, Latino-1, Latino esteso A, greco, cirillico: mappature
    che non cambiano la lunghezza in byte) usato da tutti questi punti; `_` in `LIKE` consuma un carattere UTF-8. Come le
    collation `_ci` di MySQL (general / unicode / 0900_ai) i confronti ignorano anche gli accenti delle lettere latine
    (`'Èra' = 'era'`, tabelle generate da Unicode NFD più đ ħ ı ł ŧ ø). Corretto insieme `COUNT(DISTINCT k)` su una
    colonna `_ci`, che contava `'a'` e `'A'` come valori diversi (anche prima, con le sole lettere ASCII). I byte dei
    caratteri di 3-4 byte (`€`, emoji) non vengono mai toccati. Gli
    alberi su disco delle colonne `_ci` scritti dalle versioni precedenti (chiavi piegate solo in ASCII) hanno un
    segno diverso (`tree_ci` 1 contro 2) e si ricostruiscono invece di dare ricerche sbagliate. Test in
    `tests/test_mysql.c` (8 casi).
  - **Resta da decidere (prodotto):** senza `COLLATE … _ci` una tabella confronta i testi distinguendo le maiuscole,
    mentre MySQL usa per default una collation `_ci`. Ora `information_schema` dice il vero (`utf8mb4_bin`, prima
    `utf8mb4_general_ci`), ma cambiare il default cambierebbe il significato dei database esistenti (indici UNIQUE).

- [x] **CR-M13 · `LAST_INSERT_ID()` dopo un INSERT di più righe restituiva l'ultimo id, MySQL il primo** —
  `src/query/executor.c` (`exec_insert_in`: `last_generated`; percorso riga per riga del modo MySQL) · **confermato**
  - **Problema:** con `INSERT INTO t (v) VALUES ('x'), ('y')` MySQL dà 1 (il primo id generato dallo statement), e
    `mysqli_insert_id()` / PDO `lastInsertId()` lo usano per calcolare gli id delle righe inserite: SegVault dava 2,
    anche nel pacchetto OK del protocollo MySQL e in `last_insert_id` dell'API HTTP.
  - **Corretto (2026-11-26):** si registra il primo id generato; aggiornati i due test che fissavano il comportamento
    vecchio (`test_sql`, `test_server`).

- [x] **CR-M14 · Intero / intero e `AVG` di interi davano un double invece del DECIMAL di MySQL** —
  `src/query/executor.c` (`OP_DIV`, `AVG` aggregato e finestra)
  - **Problema:** `SELECT 7 / 2` → `3.5` (MySQL `3.5000`), `AVG(voto)` → `7.33333333333333` (MySQL `7.3333`), tipo di
    colonna FLOAT invece di DECIMAL: i driver ricevevano un double inesatto e le pagine mostravano numeri diversi da
    quelli di MySQL (gli esempi delle lezioni compresi).
  - **Corretto (2026-11-26):** divisione e media di interi con l'aritmetica decimale esistente (scala 0 + 4,
    `div_precision_increment`); aggiornati i test che fissavano il comportamento vecchio. Il fuzzer differenziale con
    SQLite resta a 0 differenze.

- [x] **CR-M15 · `ROUND(-0.5)` dava `1` e `FORMAT(0.5, 0)` dava `0`** — `src/query/decimal.c` (`dec_round`),
  `src/query/executor.c` (`FORMAT`) · **confermato con una prova**
  - **Problema:** in `dec_round` il segno si leggeva dopo la divisione, quando `-0.5` era già diventato una mantissa 0
    senza segno: `ROUND(-0.5)` = `1`, `ROUND(-0.05, 1)` = `0.1` (positivi!). `FORMAT` arrotondava un double con
    `printf`: `FORMAT(0.5, 0)` = `0`, `FORMAT(2.5, 0)` = `2`, `FORMAT(1.005, 2)` = `1.00`, `FORMAT(-0.5, 0)` = `-0`
    (MySQL: 1, 3, 1.01, -1). Importi e medie mostrati in una pagina risultavano sbagliati.
  - **Corretto (2026-11-26):** segno preso prima della divisione; `FORMAT` arrotonda con l'aritmetica decimale (a metà
    lontano dallo zero) e usa `printf` solo come ripiego. Test in `tests/test_sql.c`.

### Basso

- [x] **CR-B1 · Codice morto nascosto da `-Wno-unused-function`** — `Makefile:20` (`-Wno-unused-function`,
  `-Wno-unused-parameter`)
  - **Problema:** compilando con `-Wunused-function -Wmissing-prototypes` escono 11 funzioni `static` mai usate e
    4 funzioni esportate che nessuno chiama:
    `src/bridge/db_api.c:1868` `invalidate_after_undo`; `src/query/executor.c:2616` `ev_str`, `:5520` `str_hash`,
    `:6806` `probe_text`, `:9807` `pk_column`, `:9812` `rid_key`; `src/query/parser.c:138` `copy_qualified`,
    `:1626` `parse_condition`, `:2812` `parse_column_ref`; `src/server/service.c:114` `xml_put`;
    `src/widgets/schema_model.c:289` `dupf`; esportate senza prototipo e senza chiamanti:
    `src/bridge/db_api.c:2646` `db_list_procedures`, `:3214` `db_use`, `src/catalog/schema.c:908`
    `catalog_serialize`, `src/widgets/window.c:969` `app_window_ui_waits`; macro `sleep_ms` in `src/main.c:32`.
  - **Perché è un problema:** codice che si legge e si mantiene ma non gira (e non è testato); le funzioni
    esportate allargano la superficie di `libsegvault.a`. `parse_condition` e `parse_column_ref` lasciano
    pensare a un secondo percorso di parsing che non esiste più.
  - **Correzione:** eliminarle; togliere `-Wno-unused-function` dai `CFLAGS` (il controllo "nessun warning" della
    CI lo farà rispettare); se una funzione serve solo ai test, dichiararla in un header `*_internal.h`.
  - **Corretto (2026-11-26):** rimosse le 11 funzioni `static` e le 4 esportate inutilizzate (più `db_use_impl`, `db_list_procedures_impl`, la macro `sleep_ms` di `main.c`); `catalog_serialize` è `static`; `xml_put`, `unit_put` e `file_pick_worker` dentro l'`#ifdef` della loro piattaforma. `-Wno-unused-function` tolto dal Makefile: build Linux e MinGW senza warning.

- [x] **CR-B2 · Lo stesso buffer di testo dinamico è riscritto almeno 13 volte** — `src/bridge/db_api.c:3250`
  (`sb_cat`), `src/query/executor.c:3141-3142` (`TextBuf`), `:12154-12158` (`TBuf`/`tb_add`),
  `src/query/sqljson.c:236` (`SjBuf`), `src/server/mysql_wire.c:824-825` (`Sb`), `src/widgets/csv_import.c:13-15`,
  `result_export.c:11-13`, `mysql_import.c:9-11`, `table_view.c:785-787`, `window.c:1882-1884`,
  `schema_model.c:308-310`, `sql_text.c:225`, `table_design.c:75`, `grid_edit.c:11`, più `JsonBuf` di `server/json.c`
  - **Problema:** ogni copia ha la sua politica di crescita e di errore (`ok`, `oom`, `err`, oppure `free` del
    buffer e `NULL`, come `sb_cat`); alcune usano `realloc`, altre `sv_realloc` (che termina il processo).
  - **Perché è un problema:** un bug di overflow o di gestione dell'OOM va corretto 13 volte; le differenze di
    comportamento (troncare, fermarsi, terminare il processo) sono casuali e non scelte.
  - **Correzione:** un solo `include/sv_strbuf.h` (`sb_put`, `sb_puts`, `sb_printf`, `sb_take`, flag `oom`
    "appiccicoso") usato da motore, server e GUI; `JsonBuf` può basarsi su di esso.
  - **Corretto (2026-11-27):** `include/sv_strbuf.h` (`SvStrBuf`: `svsb_put` / `puts` / `putc` / `printf` / `take` /
    `free`, flag `oom` appiccicoso, limite `max` facoltativo con `toolong`, crescita senza overflow di `size_t`,
    `realloc` che non termina il processo). Sostituisce `TextBuf`, `TBuf` (executor), `SjBuf`, `sb_cat` (db_api),
    `Sb` (mysql_wire), i `Buf` di csv_import / result_export / mysql_import, `TBuf` di table_view, gli `SBuf` di
    window / schema_model / sql_text, `DBuf`, `GBuf`; `JsonBuf` (API pubblica) usa la stessa `svsb_grow`. I nomi tra
    backtick della GUI passano da un solo `sql_ident_sb` (sql_quote.h), che rifiuta i nomi non scrivibili invece di
    spezzarli. Trovati strada facendo: `sb_add` di window.c non si fermava dopo un errore di memoria (un pezzo
    successivo più corto riusciva e il testo restava bucato in mezzo); il formattatore SQL (`sql_text.c`) partiva da
    un `malloc` non controllato (`NULL` con capacità > 0, poi `memcpy` su NULL) e contava in `int`. Test: `tests/test_strbuf.c` (13).

- [x] **CR-B3 · Numero magico 1024 nella codifica delle colonne legate: corretto solo finché `SV_MAX_COLUMNS` è
  1024** — `src/query/executor.c:2227-2228`, `2411`, `2454-2471`, `5209`, `5218`, `5418-5419`, `5839`, `5917`,
  `6692`, `6726-6755`, `7239` (più di 20 punti con `s * 1024 + c + 1`, `/ 1024`, `% 1024`)
  - **Problema:** `ASTExpr.bound` codifica (sorgente, colonna) come `s * 1024 + c + 1` con il letterale 1024,
    uguale per coincidenza a `SV_MAX_COLUMNS` (`include/common.h:59`). Nessun `_Static_assert` li lega.
  - **Perché è un problema:** alzare `SV_MAX_COLUMNS` (MySQL ne ammette 4 096, P3.12 / P4 vanno in quella
    direzione) fa leggere a `WHERE c1025 = …` la colonna 0 della sorgente successiva, in silenzio.
  - **Correzione:** macro uniche e un'asserzione statica:
    ```c
    #define BOUND_STRIDE      SV_MAX_COLUMNS
    #define BOUND_MAKE(s, c)  ((s) * BOUND_STRIDE + (c) + 1)
    #define BOUND_SRC(b)      (((b) - 1) / BOUND_STRIDE)
    #define BOUND_COL(b)      (((b) - 1) % BOUND_STRIDE)
    _Static_assert((long long)MAX_SOURCES * BOUND_STRIDE < INT_MAX, "bound overflow");
    ```
  - **Corretto (2026-11-26):** macro `BOUND_STRIDE` / `BOUND_MAKE` / `BOUND_SRC` / `BOUND_COL` legate a `SV_MAX_COLUMNS`, con `_Static_assert` sull'intervallo di un `int`; 41 occorrenze sostituite.

- [x] **CR-B4 · Puntatore confrontato dopo `free`** — `src/query/executor.c:2669-2677` (`executor_uservars_free`)
  - **Problema:** `free(v); if (g_uvars == v) g_uvars = NULL;` — dopo `free` il valore di `v` è indeterminato per
    lo standard C (GCC: `-Wuse-after-free=3`). In pratica funziona, ma è comportamento indefinito che un
    ottimizzatore può sfruttare.
  - **Correzione:** spostare il confronto prima di `free(v)`.
  - **Corretto (2026-11-26):** confronto spostato prima di `free`.

- [x] **CR-B5 · Statistiche dell'ottimizzatore: stesso formato a parole separate da spazi di CR-C1** —
  `src/query/stats.c:72-75` (`stats_save`), `:96-118` (`stats_load`)
  - **Problema:** `table DB TABLE ROWS NCOLS` e `col NAME …` con `%63s`: un nome di database, tabella o colonna con
    uno spazio fa fallire il caricamento e `stats_clear()` butta le statistiche di **tutte** le tabelle (piani
    peggiori ovunque, senza messaggi); un nome costruito apposta (`` `t 1e30 3` ``) scrive statistiche finte per
    un'altra tabella (`t` con 10³⁰ righe), cioè piani pessimi scelti a comando.
  - **Correzione:** la stessa codifica dei nomi decisa per CR-C1 (una funzione condivisa `sv_name_encode` /
    `sv_name_decode` in `include/`), e una riga illeggibile scarta solo la sua tabella.
  - **Corretto (2026-11-26):** nomi codificati con `sv_name_encode`; al caricamento una tabella illeggibile viene scartata da sola invece di tutte. Verificato con una prova (`` `t 1e30 3` `` / `` `my col` `` dopo il riavvio).

- [x] **CR-B6 · `BACKUP TO '<percorso>'` scrive ovunque: nessun equivalente di `secure_file_priv`** —
  `src/bridge/db_api.c:1000-1002` (statement SQL), `:3381-3395` (`db_backup_impl`: crea anche le cartelle
  intermedie), `:3332` (`BK_FILES`: `segvault.db`, `segvault.cat`, `users.dat` con gli hash delle password…)
  - **Problema:** solo gli amministratori possono eseguirlo (`auth.c`, caso `default`), ma il percorso è libero.
    Nella configurazione predefinita "amministratore" vuol dire qualunque processo locale (P4.21) o qualunque pagina
    servita dal server (CR-A2): `BACKUP TO 'www/sito/x'` mette `users.dat` e l'intero database dentro la radice di
    un sito, e il server stesso li serve via HTTP (i file sono `0600`, ma il processo che li serve è lo stesso che
    li ha scritti; `handle_web` blocca solo i nomi che iniziano con il punto).
  - **Correzione:** come `secure_file_priv` di MySQL: una cartella dei backup configurata (predefinita
    `<dati>/backups`) e percorsi relativi a essa, rifiutando `..` e i percorsi assoluti fuori da lì; in più,
    `handle_web` dovrebbe rifiutarsi di servire i file del formato SegVault (`segvault.*`, `users.dat`).
  - **In parte (2026-11-26):** il server web non serve più i file del formato SegVault (`segvault.*`, `users.dat`, `sequences.txt`, anche con alias Windows come `users.dat.`); test in `tests/test_web.c`. **Resta da decidere:** limitare il percorso dello statement SQL `BACKUP TO` a una cartella dei backup cambierebbe il comportamento documentato nel README (`BACKUP TO '/percorso/backup'`).
  - **Corretto (2026-11-27), senza cambiare il comportamento documentato:** il server registra la sua cartella web e
    quelle dei siti (`db_backup_protect`, tolte quando il server si ferma) e lo statement SQL `BACKUP TO` rifiuta le
    destinazioni lì dentro e dentro la cartella dei dati, dopo aver risolto link simbolici e percorsi relativi
    (prima bastava `BACKUP TO 'data/.'` per aggirare il controllo con `strcmp`); `..` dopo una cartella che non esiste
    ancora è rifiutato. Il backup dalla GUI (`db_backup`) resta libero. Una cartella dei backup obbligatoria
    (come `secure_file_priv`) resta possibile in futuro, ma romperebbe chi usa `BACKUP TO '/percorso'`. Test in
    `tests/test_backup.c` (link simbolici, `data/.`, prefissi simili come `wwwx`) e `tests/test_web.c`.

- [x] **CR-B7 · CI: azioni GitHub fissate per tag e non per commit** — `.github/workflows/ci.yml:16`, `:52`, `:63`,
  `:86-87`, `:124`, `:129`; `fuzz.yml:23`, `:32`; `release.yml:25`
  - **Problema:** `actions/checkout@v4`, `actions/upload-artifact@v4`, `msys2/setup-msys2@v2` seguono il tag, che
    l'autore dell'azione (o chi ne compromette l'account) può spostare. Il workflow di release costruisce e
    pubblica gli eseguibili distribuiti (P4.18: non firmati), quindi un'azione compromessa finisce nei file che gli
    utenti scaricano.
  - **Correzione:** fissare ogni `uses:` allo SHA del commit (`actions/checkout@<sha> # v4.2.2`), permessi
    minimi anche in `ci.yml` e `fuzz.yml` (`permissions: contents: read`; `release.yml:14` li dichiara già) e
    Dependabot per gli aggiornamenti delle azioni.
  - **Corretto (2026-11-26):** ogni `uses:` fissato allo SHA (`actions/checkout` v4 `11d5960`, `upload-artifact` v4 `ea165f8`, `setup-msys2` v2 `3c51bf6`), `permissions: contents: read` in `ci.yml` e `fuzz.yml`, `.github/dependabot.yml` settimanale per le azioni.

- [x] **CR-B8 · `SELECT … FROM DUAL` non era riconosciuto** — `src/query/parser.c` (`parse_select_core`)
  - **Corretto (2026-11-26):** `FROM DUAL` (MySQL, Oracle) vale come nessuna tabella; test in `tests/test_sql.c`.

- [x] **CR-B9 · Piccole differenze da MySQL trovate dalle prove** — `src/query/executor.c`, `parser.c`
  - la funzione di testo `INSERT(str, pos, len, new)` non è riconosciuta (il parser vede la parola chiave `INSERT`);
  - `JSON_SET` / `JSON_REMOVE` restituiscono il documento con spaziature miste (`{"a":1, "b": 2}`) invece della forma
    normalizzata di MySQL (`{"a": 1, "b": 2}`): conta per chi confronta il testo JSON;
  - la regex di `REGEXP` viene compilata di nuovo a ogni riga anche quando il pattern è una costante.
  - **Corretto (2026-11-27):** `INSERT(str, pos, len, new)` come MySQL (anche con lettere accentate, e nelle viste);
    output JSON nella forma normalizzata di MySQL (`sj_normalize` in `sqljson.c`) per `JSON_SET` / `INSERT` /
    `REPLACE` / `REMOVE` / `ARRAY_APPEND`, `JSON_EXTRACT` e `->`, `JSON_MERGE_PATCH` e `CAST(… AS JSON)` (che ora
    rifiuta il testo non JSON); regex in una cache per thread di 8 posti (`sv_regex_cached`, nessuna scrittura
    nell'AST condiviso, liberata all'uscita del thread): la stessa `REGEXP` su 200 000 righe 290 → 145 ms.
    Trovate strada facendo e aggiunte: `JSON_ARRAY_INSERT`, `JSON_MERGE_PRESERVE` / `JSON_MERGE` (risultati uguali
    agli esempi del manuale MySQL). Test in `tests/test_sql.c` e `tests/test_mysql.c`.

- [~] **CR-B10 · Differenze da MySQL trovate con le prove del 2026-11-27** — sonde su `libsegvault.a` confrontate
  con i risultati documentati di MySQL 8 (funzioni di testo, date, numeri, bit, JSON, conversioni)
  - **Corretto:** `CAST(-1 AS UNSIGNED)` dava `0` (MySQL: `18446744073709551615`, modulo 2⁶⁴) e
    `CAST('18446744073709551615' AS UNSIGNED)` si fermava a 2⁶³ − 1; `CAST(1e30 AS SIGNED)` passava da un `double`
    fuori intervallo (comportamento indefinito in C), ora satura come MySQL. Test in `tests/test_sql.c`.
  - **Corretto:** `COUNT(DISTINCT a, b, …)` era un errore di sintassi ("Aggregate functions take exactly one
    argument"); ora conta le combinazioni distinte e salta le righe con un NULL, come MySQL (chiave composta con le
    lunghezze in testa: `('a1','b')` e `('a','1b')` restano diverse).
  - **Corretto:** `INSERT … VALUES` saltava i limiti di `TINYINT` / `SMALLINT` / `MEDIUMINT` (200 entrava in un
    `TINYINT`; `INSERT … SELECT` e `UPDATE` invece li controllavano) e `INT UNSIGNED`, conservato come `BIGINT`,
    accettava 4294967296. Ora il controllo sta in `row_fits`, comune a ogni scrittura.
  - **Corretto:** una sottoquery scalare **correlata** senza righe (`(SELECT MAX(y) FROM b WHERE b.aid = a.id)`)
    dava una stringa vuota invece di NULL, quindi `COALESCE(…, -1)` e `IS NULL` sbagliavano: la copia del testo
    del risultato trasformava il NULL in "".
  - **Corretto:** `BINARY x` era ignorato (`'ABC' LIKE BINARY 'abc'` = 1) e `x COLLATE nome` in un'espressione era un
    errore di sintassi (`ORDER BY nome COLLATE utf8mb4_bin`, `WHERE s = 'a' COLLATE utf8mb4_general_ci`): ora
    diventano `__COLLATE(x, nome)` e una collation esplicita prevale su quella della colonna in `=`, `<`, `LIKE`.
  - **Corretto:** la `COLLATE` scritta su una colonna (`s VARCHAR(5) COLLATE utf8mb4_general_ci`) era ignorata:
    contava solo quella della tabella. Nuovi bit `COLF_CI` / `COLF_BIN` nel catalogo (le versioni precedenti li
    ignorano).
  - **Corretto:** `SHOW CREATE TABLE`, `CREATE TABLE … LIKE` e il dump della GUI scrivevano `INT` per `TINYINT` /
    `SMALLINT` / `MEDIUMINT`, `BIGINT` per `INT UNSIGNED` e perdevano `UNSIGNED` e le collation: dopo un dump e un
    ripristino i limiti sparivano. Ora i tipi numerici escono come dichiarati e le collation di colonna e di tabella
    sono scritte.
  - **Corretto:** il commit implicito di MySQL prima di un DDL (descritto in P1.11 come già fatto) non c'era:
    `BEGIN; INSERT …; CREATE TABLE x …; ROLLBACK` lasciava la tabella nuova e annullava l'`INSERT`; un `BEGIN`
    dentro una transazione dava errore. Ora DDL (non `TEMPORARY`), `ANALYZE` / `OPTIMIZE` e `BEGIN` fanno prima il
    commit della transazione aperta (`implicit_commit_stmt` in `db_api.c`). Test in `tests/test_sql.c`.
  - **Corretto:** `ORD('à')` dava 195 (il primo byte), MySQL 50080 (i byte del carattere).
  - **Aperto (minori):** un `DOUBLE` che trabocca (`1e308 * 10`, `EXP(1000)`) dà NULL invece dell'errore "DOUBLE value
    is out of range"; `-(-9223372036854775808)` e `ABS(-9223372036854775808)` danno un double invece dell'errore
    BIGINT.
  - **Aperto:** `ADD COLUMN … AFTER x` / `FIRST` sono accettati ma la colonna va in fondo (serve separare l'ordine
    logico delle colonne da quello delle tuple).
  - **Aperto:** l'aritmetica non tiene il tipo unsigned: `CAST(2 AS UNSIGNED) - 3` dà `-1`, MySQL l'errore
    "BIGINT UNSIGNED value is out of range".

### Falsi positivi degli strumenti e sospetti scartati (controllati, nessuna azione)

Elencati perché chi riprende il lavoro non li ricontrolli da capo.

- **`gcc -fanalyzer` (37 segnalazioni), tutte false:** `src/table/tuple.c:126-156` (overflow dell'heap:
  `tuple_create(s->num_columns)` alloca proprio quei valori); `src/bridge/db_api.c:2398`, `:3259`;
  `src/server/mysql_wire.c:834-838` (il buffer resta in `b->p`); `src/server/server.c:5178` (il socket lo chiude
  `sv_server_free`); `src/tx/wal.c:245` (finestra già letta); `src/widgets/line_edit.c:75`,
  `result_export.c:22`, `table_view.c:796`, `textbox.c:192`, `csv_import.c:102`; `src/query/lexer.c:64`;
  `src/platform/fonts_embed.c:26-29` (differenza tra simboli del linker: l'idioma dei dati incorporati);
  `src/widgets/schema_view.c:995`. Nel motore i "NULL dopo l'allocazione" non sono raggiungibili: la politica
  dichiarata in `include/sv_alloc.h` è terminare il processo se manca la memoria.
- **Warning estesi:** `server.c:2370`, `:2425` (`-Wformat-overflow`: i buffer sono dimensionati dalla lunghezza);
  `h2.c:43`, `tls.c:643` (`-Wlogical-op`: `EAGAIN == EWOULDBLOCK` su Linux, idioma portabile); `server.c:4336`
  (`-Wjump-misses-init`: `max` non è usato dopo `done:`); i 146 `-Wformat-truncation` sono `snprintf` che troncano
  di proposito (dove conta, il valore di ritorno è controllato).
- **Sospetti scartati con una prova:** SIGPIPE nel processo della GUI con HTTPS (TLS usa BIO in memoria e
  `send(…, MSG_NOSIGNAL)`: 6 disconnessioni con RST durante un download da 40 MB, processo vivo); overflow
  aritmetici a 64 bit (errore "out of range", nessuna trap con `INT64_MIN DIV -1`); atomicità degli statement con
  violazioni di PRIMARY KEY / UNIQUE / FOREIGN KEY (corretta; UNIQUE controllato a fine statement come da standard
  SQL); il leak del parser segnalato dal fuzzing notturno (3 × 50 s del target `exec` con semi 8, 9, 10: nessun
  errore); altre race nelle letture parallele (8 tipi di query dopo un riavvio sotto TSan: solo CR-A1);
  `DROP DATABASE` mentre altre transazioni tengono lock con l'indice del database (impossibile: il lock globale `*`
  in modo X aspetta tutte le transazioni); lock di procedure, trigger, viste e chiavi esterne (espansi in modo
  conservativo da `ls_name`, `db_api.c`); `PREPARE` / `EXECUTE` (rientrano in `db_execute_impl`, quindi nel
  percorso normale dei lock); login MySQL e sessioni HTTP (token casuali da 16 byte, confronto a tempo
  costante, controllo del proprietario della sessione); hash delle password (PBKDF2-HMAC-SHA256 con confronto a
  tempo costante).
- **Limiti di questa revisione:** il codice solo Windows (`win32.c`, ACL, percorsi `C:\`) è stato letto ma non
  eseguito; la GUI non è stata aperta a schermo; `parser.c` ed `executor.c` sono stati campionati con strumenti e
  prove, non letti riga per riga (P3.9 resta aperto).


