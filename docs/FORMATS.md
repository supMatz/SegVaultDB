# On-disk formats, guarantees and limits

All integers are little-endian. A data directory contains:

| File | Content |
|---|---|
| `segvault.db` | pages of 4096 bytes; page 0 is reserved |
| `segvault.wal` | the transaction log (absent after a clean shutdown) |
| `segvault.cat` | the catalog (databases, tables, views, routines, triggers) |
| `users.dat` | users and grants (text) |
| `sequences.txt`, `segvault.free` | sequence values, free pages left by `VACUUM` |
| `mysql.sock` | the MySQL protocol's Unix socket while the server runs with `--mysql-port` |

## Pages (`segvault.db`)

```
0   uint32 page_id      must equal the page number (checked on every read)
4   uint16 free_space
6   uint16 num_slots    at most (4096 - 12) / 4 = 1021
8   uint16 flags        0x04 chained heap page, 0x08 overflow page, 0x10 free (VACUUM); bits 8-15: next page bits 16-23
10  uint16 next_page_id next page of the table (low 16 bits; 0 = end)
12  slot directory      num_slots × { uint16 offset, uint16 length (0 = deleted) }, growing upwards
    ...                 tuples, growing downwards from the end of the page
```

A page whose header or slot directory is inconsistent is refused (`page_validate`): the statement that
needed it fails with "Page N of the data file is damaged", it never returns fewer rows.

**Rows** are serialised as a NULL bitmap (`(columns + 7) / 8` bytes), then the non-NULL values in column
order: `INT` 4 bytes, `BIGINT` / `FLOAT` 8, `BOOL` 1, text types a `uint16` length (or `0xFFFF` + `uint32`) and
the bytes. A row over 2048 bytes is stored in a chain of **overflow pages** (`uint16` length + data per page)
and the slot holds a 24-byte stub: magic `FF 00 "SVOVF1"`, total length, first page, page count, FNV-1a. A row
whose own bytes have the shape of a stub is always stored through a chain, so a stub is never forged.

**Binary values**: inside the engine byte 0x00 is written as `C0 80` and byte 0xC0 as `C0 81` (0xC0 never
occurs in valid UTF-8). The MySQL protocol, the JSON API, exports and `LENGTH` / `HEX` / ... use the real bytes.

## Transaction log (`segvault.wal`)

A sequence of records `type:1 tx:8 page:4 slot:2 data_len:4 old_len:4 data old`. A record cut short at the
end of the file (crash during a write) ends the log.

| Type | Meaning |
|---|---|
| 1 BEGIN, 2 COMMIT, 3 ABORT | transaction boundaries; ABORT is written only after the undo is on disk |
| 4 INSERT, 5 DELETE, 6 UPDATE | row images (`data` new, `old` old) |
| 7 SAVEPOINT, 8 ROLLBACK_TO | `ROLLBACK_TO` carries the savepoint's log offset: later records are already undone |
| 9 UPDATE_MOVE | a row that moved: `data` = new page:4, new slot:2, new row; `old` = old row |
| 10 CLR | compensation: one undo step (`data` = compensated record offset:8, action:1, row); 1 delete slot, 2 put the row back into its own slot |
| 11 LINK | a new heap page linked after `data` = previous page:4 (recovery recreates both) |

Records of transaction 0 are autocommit statements. Recovery: pass 1 reads the headers and keeps, per
transaction still open, the offsets of its records; pass 2 redoes every page record and CLR in log order (each
is idempotent on the page state); pass 3 undoes, newest first, the records of transactions without COMMIT or
ABORT that no CLR compensates, logging CLRs itself; then the data file is `fsync`ed, the log removed and the
directory `fsync`ed. Any read, memory or write error stops the start with the log left untouched.

## Catalog (`segvault.cat`)

`"SVCAT2\0\0"`, `uint32` version (5), `int32` databases, `int32` current database, `uint32` next page, then
per database its name and the raw `TableSchema` / `ViewSchema` / `RoutineSchema` / `TriggerSchema` structures
(`src/catalog/schema.h`). Since 1.3 the file ends with `"CRC1"` + CRC-32 of everything before it; older files
without the trailer are accepted. A catalog that is truncated, damaged or of a newer version stops the start:
it is never replaced by an empty one. It is rewritten through `segvault.cat.tmp` and an atomic replacement.

## `users.dat`

Text: `user NAME SALT HASH ADMIN NATIVE` and `grant USER DB TABLE PRIVS` lines, then (since 1.3)
`end N` with the number of users: a file whose count does not match stops the start.

## Durability

`SET GLOBAL synchronous = FULL | NORMAL | OFF` (see the README): `FULL` makes every committed write durable
before the answer, `NORMAL` (default) every `COMMIT` and autocommit writes within 100 ms, `OFF` leaves it to
checkpoints. A failed write or `fsync` is never reported as success: the engine stops accepting statements
until it is restarted.

## Isolation

Writes take table locks held until `COMMIT` (deadlocks: error 1213, waits over 30 s: error 1205). Reads of a
table being written by another open transaction see its last committed version. `transaction_isolation`
reports `SERIALIZABLE` for table-level writes; plain reads are committed-version reads, not repeatable reads.

## Limits

| Limit | Value |
|---|---|
| databases | 64 |
| tables per database | 256 |
| columns per table | 64 |
| indexes / constraints per table | 16 / 16 |
| columns of a key | 8 |
| views, routines, triggers per database | 64 each |
| name length | 63 bytes |
| SQL statement, view / routine body | 64 KB |
| one text value | 16 MB; a serialised row 64 MB |
| page | 4096 bytes; up to 2²⁴ pages (64 GB) per data file |
| HTTP request target / header field | 8 KB / see `server.c` (`Authorization` 4 KB), else 414 / 431 |
| page cache | 1/8 of the RAM, 4-512 MB (`--cache-size`, `SET GLOBAL cache_size`) |
