# 🗄️ SegVault — Relational DBMS in pure C

> A fully hand-crafted relational database management system written in C,  
> complete with a native GUI for both **Windows** (Win32) and **Linux** (Xlib/X11),  
> a hand-written SQL parser, a B+Tree storage engine, Write-Ahead Logging, and zero dependencies.
---

## 📋 Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [GUI Layout](#gui-layout)
- [Source Structure](#source-structure)
- [Documentation & References](#documentation--references)
  - [GUI — Win32 (Windows)](#1--gui--win32-windows)
  - [GUI — Xlib (Linux/X11)](#2--gui--xlibx11-linux)
  - [C Standard Library](#3--c-standard-library)
  - [File I/O — POSIX](#4--file-io--posix-storage-engine)
  - [SQL Parser — Flex + Bison](#5--sql-parser--flex--bison)
  - [Data Structures](#6--data-structures-in-c)
  - [WAL & Recovery (ARIES)](#7--write-ahead-log--recovery-aries)
  - [Reference Source Code](#8--reference-source-code)
  - [Cross-compilation Linux → Windows](#9--cross-compilation-linux--windows)
- [Building](#building)
- [License](#license)

---

## Overview

**SegVault** is a didactic yet fully functional relational DBMS built entirely in C — no external libraries, no frameworks, no shortcuts.

| Feature | Details |
|---|---|
| **Language** | C (C11) |
| **Version** | 0.1.0 |
| **GUI** | Win32 / GDI (Windows) · Xlib/X11 (Linux) |
| **Storage** | Custom B+Tree with page-based I/O (4 KB pages) |
| **SQL** | Hand-written recursive descent parser + lexer |
| **Transactions** | Write-Ahead Log (ARIES-style recovery) |
| **Cross-compilation** | MinGW-w64 — build `.exe` from Linux |

---

## Supported SQL

Tutte le query si scrivono nell'editor SQL e si eseguono cliccando **Run Query** (pulsante verde in alto a sinistra).

### Data Definition Language (DDL)

```sql
-- Database
CREATE DATABASE nome_db;
DROP DATABASE nome_db;
USE nome_db;
SHOW DATABASES;
SHOW TABLES;
DESCRIBE nome_tabella;

-- Table
CREATE TABLE utenti (
    id     INT,
    nome   VARCHAR(100),
    eta    INT,
    salario FLOAT,
    attivo BOOLEAN
);
DROP TABLE nome_tabella;

-- Index
CREATE INDEX idx_nome ON utenti (nome);
```

### Data Manipulation Language (DML)

```sql
-- Insert
INSERT INTO utenti VALUES (1, 'Alice', 30, 45000.0, 1);
INSERT INTO utenti (id, nome) VALUES (2, 'Bob');

-- Select
SELECT * FROM utenti;
SELECT nome, eta FROM utenti;
SELECT * FROM utenti WHERE id = 1;
SELECT * FROM utenti WHERE nome = 'Alice';

-- Update
UPDATE utenti SET eta = 31 WHERE id = 1;

-- Delete
DELETE FROM utenti WHERE id = 2;
```

### Transactions

```sql
BEGIN;
INSERT INTO utenti VALUES (3, 'Charlie', 25, 50000.0, 1);
COMMIT;

BEGIN;
UPDATE utenti SET eta = 26 WHERE id = 3;
ROLLBACK;
```

### Tipi di dato supportati

| Tipo | Descrizione |
|---|---|
| `INT` / `INTEGER` | 4 byte, signed |
| `BIGINT` | 8 byte, signed |
| `FLOAT` / `DOUBLE` | 8 byte floating point |
| `VARCHAR(n)` | Stringa a lunghezza variabile |
| `CHAR(n)` | Stringa a lunghezza fissa |
| `TEXT` | Stringa lunga (max 65535 byte) |
| `BOOL` / `BOOLEAN` | 1 byte (0/1) |
| `DATE` | 4 byte (giorni dal 1970) |
| `DATETIME` | 8 byte (ms dal 1970) |
| `BLOB` | Dati binari |
| `DECIMAL` | Numero decimale a precisione fissa |

### Column constraints (parsed, not enforced)

`NOT NULL`, `NULL`, `PRIMARY KEY`, `AUTO_INCREMENT`, `DEFAULT valore`

### Non ancora implementato

`ALTER TABLE`, `JOIN`, `ORDER BY`, `GROUP BY`, `HAVING`, `LIMIT`, subquery, viste, trigger, stored procedure, EXPLAIN, SAVEPOINT.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        GUI Layer                            │
│  window.c / window.h  —  layout principale dell'app         │
│  Widget system: button · label · textbox · table_view · …   │
│                                                             │
│    platform/win32.c (Windows)  │  platform/xlib.c (Linux)   │
│    GDI — double buffer (HDC)   │  Xlib — Pixmap backbuffer  │
└──────────────────────┬──────────────────────────────────────┘
                       │  platform.h  (interfaccia unificata)
                       │  db_api.h    (unico punto di contatto GUI ↔ DB)
┌──────────────────────▼──────────────────────────────────────┐
│                    SQL Parser Layer                          │
│        Hand-written lexer (src/query/lexer.c)                │
│        Recursive descent parser (src/query/parser.c)         │
└──────────────────────┬───────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│              Query Executor                                 │
│        src/query/executor.c — dispatches AST → results      │
└──────────────────────┬──────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│                  Storage Engine                             │
│    B+Tree Index  │  Buffer Pool LRU (1024 pagine)           │
│    Heap File     │  Page size: 4 KB                         │
│    Limiti: 64 DB · 256 tabelle · 64 colonne · 16 indici     │
└──────────────────────┬──────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│           Write-Ahead Log (WAL)                             │
│         BEGIN · COMMIT · ROLLBACK                           │
└──────────────────────┬──────────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────────┐
│           POSIX File I/O  (pread / pwrite)                  │
└─────────────────────────────────────────────────────────────┘
```

---

## GUI Layout

Il layout principale è definito in `window.h` / `window.c`:

```
┌──────────────────────────────────────────────────────┐
│  TOOLBAR: [Run] [Commit] [Rollback] [DB selector]    │
├────────────┬─────────────────────────────────────────┤
│            │  EDITOR SQL (textbox multilinea)        │
│  SIDEBAR   ├─────────────────────────────────────────┤
│  (tree)    │  STATUS BAR (righe affected, tempo ms)  │
│            ├─────────────────────────────────────────┤
│            │  RESULTS (table_view)                   │
└────────────┴─────────────────────────────────────────┘
```

Il sistema grafico è astratto da `platform.h` — tutti i widget usano **solo** quell'interfaccia e non toccano mai `<windows.h>` o `<X11/Xlib.h>` direttamente. La GUI comunica con il DB engine **esclusivamente** tramite `db_api.h`.

### Widget system

Ogni widget estende `Widget` (da `widget.h`) mettendolo come **primo campo** della propria struct — pattern che emula l'ereditarietà in C:

| Widget | File | Descrizione |
|---|---|---|
| `Widget` | `widget.h` / `widget.c` | Struct base con vtable (`draw`, `handle_event`, `destroy`) |
| `Button` | `button.h` / `button.c` | Bottone con label, hover, pressed, disabled |
| `Label` | `label.h` / `label.c` | Testo non interattivo, allineamento L/C/R |
| `Textbox` | `textbox.h` / `textbox.c` | Input SQL multilinea |
| `TableView` | `table_view.h` / `table_view.c` | Griglia risultati query |
| `TreeView` | `tree_view.h` / `tree_view.c` | Sidebar oggetti DB |
| `Scrollbar` | `scrollbar.h` / `scrollbar.c` | Scrollbar verticale/orizzontale |
| `Panel` | `panel.h` / `panel.c` | Contenitore con 128 figli, bordo, padding |
| `Splitter` | `splitter.h` / `splitter.c` | Divisore ridimensionabile orizzontale/verticale |

### Colori di sistema (`platform.h`)

| Costante | Hex | Uso |
|---|---|---|
| `COLOR_BG` | `#0F1116` | Sfondo principale |
| `COLOR_PANEL` | `#161922` | Pannelli |
| `COLOR_ACCENT` | `#6E56FF` | Viola elettrico — pulsanti principali |
| `COLOR_TEXT` | `#E1E4F0` | Testo principale |
| `COLOR_TEXT_DIM` | `#585E78` | Testo secondario / placeholder |
| `COLOR_BORDER` | `#262A3A` | Bordi |
| `COLOR_HOVER` | `#202432` | Hover |
| `COLOR_SELECT` | `#3C2E8C` | Selezione |

---

## Source Structure

```
SegVaultDB/
│
├── src/
│   │
│   ├── platform/              # Astrazione OS (Win32 vs Xlib)
│   │   ├── platform.h         # Interfaccia comune: UNICA per entrambi gli OS
│   │   ├── win32.c            # Implementazione Windows
│   │   └── xlib.c             # Implementazione Linux
│   │
│   ├── widgets/               # Componenti grafici (usano solo platform.h)
│   │   ├── widget.h           # Struttura base con vtable
│   │   ├── widget.c
│   │   ├── window.h
│   │   ├── window.c           # Finestra principale + layout app
│   │   ├── button.h
│   │   ├── button.c           # Bottoni cliccabili
│   │   ├── textbox.h
│   │   ├── textbox.c          # Editor SQL multilinea
│   │   ├── label.h
│   │   ├── label.c            # Testo non interattivo
│   │   ├── table_view.h
│   │   ├── table_view.c       # Griglia risultati query
│   │   ├── tree_view.h
│   │   ├── tree_view.c        # Albero DB/tabelle nel pannello sx
│   │   ├── scrollbar.h
│   │   ├── scrollbar.c        # Scrollbar per tabelle e alberi
│   │   ├── panel.h
│   │   ├── panel.c            # Contenitore con bordo/padding
│   │   ├── splitter.h
│   │   └── splitter.c         # Divisore ridimensionabile
│   │
│   ├── storage/               # Layer fisico: pagine su disco
│   │   ├── page.h / page.c
│   │   └── buffer_pool.h / buffer_pool.c
│   │
│   ├── catalog/               # Metadati: tabelle, colonne, tipi, viste
│   │   └── schema.h / schema.c
│   │
│   ├── table/                 # Heap file + tuple
│   │   ├── heap.h / heap.c
│   │   └── tuple.h / tuple.c
│   │
│   ├── index/                 # B+Tree
│   │   └── btree.h / btree.c
│   │
│   ├── query/                 # SQL: lexer → parser → executor
│   │   ├── lexer.h / lexer.c
│   │   ├── parser.h / parser.c
│   │   └── executor.h / executor.c
│   │
│   ├── tx/                    # Transazioni + WAL
│   │   ├── transaction.h / transaction.c
│   │   └── wal.h / wal.c
│   │
│   ├── bridge/                # IL PONTE TRA GUI E DB
│   │   └── db_api.h / db_api.c
│   │
│   └── main.c                 # Entry point
│
├── include/
│   └── common.h               # Tipi globali, macro
│
└── Makefile.
```

### Costanti globali (`common.h`)

| Costante | Valore | Significato |
|---|---|---|
| `SV_PAGE_SIZE` | `4096` | Dimensione pagina su disco (4 KB) |
| `SV_BUFFER_POOL_CAP` | `1024` | Max pagine in cache RAM |
| `SV_MAX_DATABASES` | `64` | Max database aperti |
| `SV_MAX_TABLES` | `256` | Max tabelle per DB |
| `SV_MAX_COLUMNS` | `64` | Max colonne per tabella |
| `SV_MAX_INDEXES` | `16` | Max indici per tabella |
| `SV_MAX_SQL_LEN` | `65536` | Max lunghezza query SQL |

---

## Documentation & References

### 1 · GUI — Win32 (Windows)

Official docs: [learn.microsoft.com/en-us/windows/win32/](https://learn.microsoft.com/en-us/windows/win32/)

| Topic | Link |
|---|---|
| Creating a window | [Window Management](https://learn.microsoft.com/en-us/windows/win32/winmsg/windows) |
| Event handling (WndProc) | [Window Procedures](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-procedures) |
| Drawing with GDI | [Windows GDI](https://learn.microsoft.com/en-us/windows/win32/gdi/windows-gdi) |
| Text & fonts | [Fonts and Text](https://learn.microsoft.com/en-us/windows/win32/gdi/fonts-and-text) |
| Keyboard input | [Keyboard Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/keyboard-input) |
| Mouse input | [Mouse Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/mouse-input) |
| Dialog boxes | [Dialog Boxes](https://learn.microsoft.com/en-us/windows/win32/dlgbox/dialog-boxes) |

---

### 2 · GUI — Xlib/X11 (Linux)

Official docs: [tronche.com/gui/x/xlib/](https://tronche.com/gui/x/xlib/)

| Topic | Link |
|---|---|
| Open X server connection | [Opening a Display](https://tronche.com/gui/x/xlib/display/opening.html) |
| Create a window | [XCreateWindow](https://tronche.com/gui/x/xlib/window/XCreateWindow.html) |
| Select events | [XSelectInput](https://tronche.com/gui/x/xlib/event-handling/XSelectInput.html) |
| Read events | [Event Queue](https://tronche.com/gui/x/xlib/event-handling/manipulating-event-queue/) |
| Draw rectangles | [XFillRectangle](https://tronche.com/gui/x/xlib/graphics/drawing/XFillRectangle.html) |
| Draw text | [XDrawString](https://tronche.com/gui/x/xlib/graphics/drawing/XDrawString.html) |
| Graphic Context (GC) | [GC Reference](https://tronche.com/gui/x/xlib/GC/) |
| Pixmap (backbuffer) | [Pixmap](https://tronche.com/gui/x/xlib/pixmap-and-cursor/pixmap.html) |
| Keyboard / KeySym | [Keyboard Utilities](https://tronche.com/gui/x/xlib/utilities/keyboard/) |
| Window close (WM_DELETE) | [ICCCM §4](https://tronche.com/gui/x/icccm/sec-4.html) |

---

### 3 · C Standard Library

Reference: [en.cppreference.com/w/c](https://en.cppreference.com/w/c)

| Header | Purpose | Link |
|---|---|---|
| `<stdint.h>` | `uint32_t`, `uint64_t`, … | [Integer types](https://en.cppreference.com/w/c/types/integer) |
| `<stdbool.h>` | `bool`, `true`, `false` | [Boolean type](https://en.cppreference.com/w/c/types/boolean) |
| `<stdlib.h>` | `malloc`, `free`, `calloc` | [Memory](https://en.cppreference.com/w/c/memory) |
| `<string.h>` | `memcpy`, `memset`, `strlen` | [Byte strings](https://en.cppreference.com/w/c/string/byte) |
| `<stdio.h>` | `printf`, `fopen`, `fread` | [I/O](https://en.cppreference.com/w/c/io) |
| `<unistd.h>` | `pread`, `pwrite`, `open` | [POSIX](https://pubs.opengroup.org/onlinepubs/9699919799/) |
| `<fcntl.h>` | `O_RDWR`, `O_CREAT` | [fcntl.h](https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/fcntl.h.html) |

---

### 4 · File I/O — POSIX (Storage Engine)

POSIX reference: [pubs.opengroup.org/onlinepubs/9699919799/](https://pubs.opengroup.org/onlinepubs/9699919799/)

> `pread` e `pwrite` leggono/scrivono a un offset preciso **senza spostare il cursore del file** — ideale per le pagine da 4 KB del database.

| Function | Purpose | Link |
|---|---|---|
| `open()` | Open/create the DB file | [open()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/open.html) |
| `pread()` | Read a page at offset | [pread()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pread.html) |
| `pwrite()` | Write a page at offset | [pwrite()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pwrite.html) |
| `fsync()` | Flush to disk (durability) | [fsync()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/fsync.html) |
| `lseek()` | Seek within file | [lseek()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/lseek.html) |
| `close()` | Close the file descriptor | [close()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/close.html) |

---

### 5 · SQL Parser — Hand-written Recursive Descent

SegVault implements its own SQL parser from scratch — no Flex or Bison needed.

| Resource | Link |
|---|---|
| Hand-written lexer guide | [craftinginterpreters.com — Scanning](https://craftinginterpreters.com/ch04-scanning.html) |
| Recursive descent parsing | [craftinginterpreters.com — Parsing](https://craftinginterpreters.com/ch06-parsing.html) |
| SQL-92 grammar reference | [SQL 1992 spec (CMU)](http://www.contrib.andrew.cmu.edu/~shadow/sql/sql1992.txt) |
| SQLite SQL dialect (practical) | [sqlite.org/lang.html](https://www.sqlite.org/lang.html) |

---

### 6 · Data Structures in C

All core engine structures are implemented from scratch.

| Structure | Reference |
|---|---|
| **B+Tree** | [Wikipedia — B+Tree](https://en.wikipedia.org/wiki/B%2B_tree) · [USC Indexing paper (PDF)](https://infolab.usc.edu/csci585/Spring2010/den_ar/indexing.pdf) |
| **Buffer Pool / LRU Cache** | [Cache Replacement Policies — LRU](https://en.wikipedia.org/wiki/Cache_replacement_policies#LRU) |
| **Hash Map** (catalog) | [Hash Table](https://en.wikipedia.org/wiki/Hash_table) |
| **Linked List** | [Linked List](https://en.wikipedia.org/wiki/Linked_list) |

---

### 7 · Write-Ahead Log (WAL)

| Resource | Link |
|---|---|
| ARIES original paper (1992) | [cs.stanford.edu — ARIES PDF](https://cs.stanford.edu/people/chrismre/cs345/rl/aries.pdf) |
| SQLite WAL explained | [sqlite.org/wal.html](https://www.sqlite.org/wal.html) |
| PostgreSQL WAL internals | [postgresql.org/docs/current/wal-internals.html](https://www.postgresql.org/docs/current/wal-internals.html) |

---

### 8 · Reference Source Code

Reading real implementations is the fastest way to understand how everything fits together.

| Project | Why it's useful | Link |
|---|---|---|
| **SQLite amalgamation** | Entire DB in one commented `.c` file | [sqlite.org/amalgamation.html](https://www.sqlite.org/amalgamation.html) |
| **LittleD** | Minimal relational DB in pure C | [github.com/graemedouglas/LittleD](https://github.com/graemedouglas/LittleD) |
| **MiniDB** | Small didactic DB | [github.com/willcrichton/minidb](https://github.com/willcrichton/minidb) |
| **CMU Tiny DB** | Guided university project | [15445.courses.cs.cmu.edu](https://15445.courses.cs.cmu.edu/fall2023/project0/) |

---

### 9 · Cross-compilation Linux → Windows

Build `.exe` files from Arch Linux without touching Windows.

| Tool | Documentation |
|---|---|
| **MinGW-w64** | [mingw-w64.org](https://www.mingw-w64.org/) |
| **CMake cross-compilation** | [cmake.org — Toolchains](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html) |

---

## Building

```bash
# Full build (real DB engine) — produce ./segvault
make

# GUI-only build (uses db_api_test.c stub, no engine) — same binary name
make gui

# Windows — produce segvault.exe  (-lgdi32 -luser32 -lkernel32)
make   # eseguito da un ambiente Windows o MinGW
```

Il Makefile rileva automaticamente la piattaforma tramite `$(OS)` e seleziona il sorgente corretto (`src/platform/xlib.c` o `src/platform/win32.c`).
`make` (default) produce il binario completo con motore DB reale; `make gui` compila solo la GUI con uno stub di test (`db_api_test.c`).

---

## License

This project is for educational purposes.  
Feel free to study, fork, and extend it.

---

<p align="center">
  Built with no dependencies, no shortcuts.
</p>
