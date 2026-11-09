# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

`tablelite` — a C11 assignment implementation of a persistent table:
100 `int64_t` columns, up to 1,000,000 rows, append-only, range queries,
per-column B+ tree indexes, thread-safe via one rwlock. Target platform is Linux;
`src/plat_win32.c` is a debugging-only shim that emulates `pread/pwrite` on MinGW.

`include/table.h` is the only public header. `学习指南.md` is the design walkthrough;
`README.md` is the quick start.

## Commands

```bash
make                 # build libtablelite.a, tests, bench, demo
make test            # 9-case functional/concurrency suite
make demo            # minimal API usage example
make bench           # 1M rows; creates ~763MB bench_table.dat, deletes it at the end
make clean
```

- Windows: use `mingw32-make` (the Makefile checks `OS=Windows_NT` and adds the shim).
- `CC` is pinned to `gcc` in the Makefile; override with `make CC=clang`.
- Tests are a single binary, not per-case targets. To isolate a case, comment out
  other `test_*()` calls in `tests/test_table.c:main`, or run the binary and read the
  `[n]` section output. Test fixtures are created in the current directory and cleaned up.
- `./tests/bench <rows>` accepts a smaller row count for quick runs (e.g. `200000`).
- There is no lint config; the build uses `-Wall -Wextra -Wpedantic` and must stay warning-free.

## Architecture

Layering (top to bottom):

- `src/table.c` — public API, row addressing, block scan, one `pthread_rwlock_t` per table.
- `src/index.c` — per-column index lifecycle: build, atomic save, load, catch-up.
- `src/btree.c` — in-memory B+ tree; the index file is a preorder serialization of it.
- `src/util.c` — `io_pread`/`io_pwrite` (EINTR/short-I/O loops), `fsync`, `ftruncate`,
  atomic rename, clock/hostname. Never call raw `pread/pwrite` elsewhere.

### Storage layout

- One data file: 512-byte header (`tl_header` in `src/table_internal.h`) + fixed-size rows.
- `offset = 512 + rowid * ncols * 8`. `nrows` in the header is the commit point.
- `tl_append` under the write lock: pwrite row → pwrite header(nrows+1) → `nrows++` →
  insert into every loaded index. This order is crash-consistency-critical; do not reorder.
  Header is written per append; data is `fsync`ed only on checkpoint/close.
- `tl_open` self-heals: clamps `nrows` down to what the file can hold, truncates a tail
  beyond `nrows` (uncommitted append). Existing tables ignore the `ncols`/`max_rows` args.

### Index lifecycle and consistency

- Index path: `<table_path>.idx.<col>`. Header (`tl_idx_header`) stores magic, version,
  col, ncols, table UUID, `indexed_rows`, `nentries`.
- The in-memory tree is authoritative and always covers `[0, nrows)`; the disk file may
  lag. On open, a file with `indexed_rows < nrows` is loaded and the tail is scanned and
  inserted ("catch-up"). This relies on append-only data — adding delete/update breaks it.
- Save is atomic: write `.tmp` → `fsync` → `io_rename_replace` → `fsync` directory.
- Load validates magic/version/col/ncols/uuid/entry counts/node and depth caps. Any
  mismatch means the file is ignored and searches fall back to a full scan.
- `tl_build_index` holds the write lock for the entire scan + build + save (blocks appends).
- Corrupt/foreign index files must never fail `tl_open`; they are silently skipped.

### B+ tree specifics (`src/btree.c`)

- Order 64; max 63 keys/children per node. Keys are composite `(value, rowid)` so
  duplicates sort deterministically and are globally unique.
- Internal separators equal the minimum key of the right child; `child[i] < key[i] <= child[i+1]`.
- Leaves are chained via `next`; range queries descend with `(lo, INT64_MIN)` then walk the chain.
- Node arrays are `MAX_KEYS + 1` / `MAX_CHILDREN + 1` on purpose: insertion transiently
  overflows before splitting. Shrinking these arrays causes out-of-bounds writes.
- OOM calls `abort()` in `bt_alloc` — deliberate simplification, not an error path.

### Concurrency

- One rwlock per table: queries take the read lock, `append`/`build/drop index`/
  `checkpoint`/`close` take the write lock. Public APIs lock; internal `*_locked`
  helpers assume the lock is held. There is one lock, so no lock-ordering concerns.
- pthread locks only protect threads in one process. Multi-process access is unsupported.

## Invariants to preserve

1. Row IDs are dense: `[0, nrows)`, no holes.
2. In-memory indexes cover every row; disk indexes may lag and are repaired on open.
3. All mutations happen under the table write lock, including header writes and index inserts.
4. Append-only storage is what makes catch-up safe.

## Platform notes

- Linux is the submission target; `_WIN32` branches must never be required for correctness.
- The shim needs `O_BINARY` on `open()`, emulates positioned I/O with `_lseeki64` +
  `_read/_write` and a global mutex, and uses `_filelengthi64` because MinGW's `stat`
  misreports large files. `io_rename_replace` uses `MoveFileExA` because MSVCRT `rename`
  fails when the destination exists.
- File format assumes host endianness and struct layout; `_Static_assert`s pin header
  sizes to 512. Bump `TL_VERSION`/`TL_IDX_MAGIC` on any format change.
