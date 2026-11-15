# Contributing

## Build

```bash
make -j"$(nproc)"          # GUI (segvault) + server (segvault-server), Linux / X11
make server                # only segvault-server
make windows               # MinGW-w64 cross build: segvault.exe, segvault-server.exe
```

C11, no external dependencies except X11 / Xft for the GUI and, optionally, OpenSSL (loaded at run time)
for HTTPS. The build must stay free of warnings (`-Wall -Wextra`, `-D_FORTIFY_SOURCE=2`).

## Tests

```bash
make test                  # every suite (about 2 minutes on 16 cores with -j16)
make test-asan             # the suites under AddressSanitizer + UndefinedBehaviorSanitizer
make test-tsan             # parallel reads and PHP under ThreadSanitizer
make test-win              # Windows-only code: MinGW build run under wine
make bench                 # bench/bench.c: inserts, scans, joins, rollback cost
```

* `tests/test_durability.c` is built with `-DSV_FAULT_INJECTION`: every `write` / `pwrite` / `fsync` /
  `pread` of the engine goes through shims that can fail or stop the process at the N-th call. Use it for
  anything that touches the WAL, the buffer pool, the catalog or recovery: a change there is not done until
  it survives a crash at every write.
* A bug fix comes with a test that fails without the fix.
* Engine changes that affect files on disk must keep reading the files of the previous release (see
  `docs/FORMATS.md`).

## Style

* Comments and messages in the code are in Italian, user-facing error messages in English (as MySQL).
* Prefer small, self-contained changes; keep functions near the code they serve.
* `TODO.md` is the engineering roadmap: when you fix an item, mark it and add a short **Fixed** note
  saying what changed and which test covers it.

## Commits

Short English summary line (`WAL: compensation records for ROLLBACK`), details in the body if needed.
