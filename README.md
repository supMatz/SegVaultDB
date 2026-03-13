# 🗄️ CRelDB — Relational DBMS in Pure C

> A fully hand-crafted relational database management system written in C,  
> complete with a native GUI for both **Windows** (Win32) and **Linux** (Xlib/X11),  
> a SQL parser, a B+Tree storage engine, Write-Ahead Logging, and zero dependencies.

---

## 📋 Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
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

**CRelDB** is a didactic yet fully functional relational DBMS built entirely in C — no external libraries, no frameworks, no shortcuts.

| Feature | Details |
|---|---|
| **Language** | C (C11) |
| **GUI** | Win32 API (Windows) · Xlib/X11 (Linux) |
| **Storage** | Custom B+Tree with page-based I/O |
| **SQL** | Flex + Bison parser (or hand-written recursive descent) |
| **Transactions** | Write-Ahead Log (ARIES-style recovery) |
| **Cross-compilation** | MinGW-w64 — build `.exe` from Linux |

---

## Architecture

```
┌─────────────────────────────────────────────────┐
│                  GUI Layer                       │
│         Win32 (Windows) │ Xlib (Linux)           │
└─────────────────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────┐
│               SQL Parser Layer                   │
│           Flex (lexer) + Bison (parser)          │
│       or hand-written recursive descent          │
└─────────────────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────┐
│             Query Executor / Planner             │
└─────────────────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────┐
│               Storage Engine                     │
│   B+Tree Index │ Buffer Pool (LRU) │ Catalog     │
└─────────────────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────┐
│         Write-Ahead Log (WAL / ARIES)            │
└─────────────────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────┐
│          POSIX File I/O  (pread / pwrite)        │
└─────────────────────────────────────────────────┘
```

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

> `pread` and `pwrite` read/write at a precise offset **without moving the file cursor** — ideal for page-based database storage.

| Function | Purpose | Link |
|---|---|---|
| `open()` | Open/create the DB file | [open()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/open.html) |
| `pread()` | Read a page at offset | [pread()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pread.html) |
| `pwrite()` | Write a page at offset | [pwrite()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/pwrite.html) |
| `fsync()` | Flush to disk (durability) | [fsync()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/fsync.html) |
| `lseek()` | Seek within file | [lseek()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/lseek.html) |
| `close()` | Close the file descriptor | [close()](https://pubs.opengroup.org/onlinepubs/9699919799/functions/close.html) |

---

### 5 · SQL Parser — Flex + Bison

The same toolchain used by **PostgreSQL** and **MySQL**.

| Tool | Documentation |
|---|---|
| Flex (lexer) | [westes.github.io/flex/manual/](https://westes.github.io/flex/manual/) |
| Bison (parser) | [gnu.org/software/bison/manual/](https://www.gnu.org/software/bison/manual/bison.html) |
| SQL-92 grammar reference | [SQL 1992 spec (CMU)](http://www.contrib.andrew.cmu.edu/~shadow/sql/sql1992.txt) |
| SQLite SQL dialect (practical) | [sqlite.org/lang.html](https://www.sqlite.org/lang.html) |

**Writing the parser by hand?** The best guide for hand-written recursive descent in C:  
📖 [craftinginterpreters.com](https://craftinginterpreters.com/) — Chapters 4 (Scanning) and 6 (Parsing) — **free online**

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

### 7 · Write-Ahead Log & Recovery (ARIES)

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
# Linux (Xlib)
gcc -o creldb main.c gui_xlib.c engine.c parser.c -lX11

# Cross-compile for Windows (from Linux, using MinGW-w64)
x86_64-w64-mingw32-gcc -o creldb.exe main.c gui_win32.c engine.c parser.c -lgdi32 -luser32
```

---

## License

This project is for educational purposes.  
Feel free to study, fork, and extend it.

---

<p align="center">
  Built with ❤️ in pure C — no dependencies, no shortcuts.
</p>
```
