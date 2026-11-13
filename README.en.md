# tablelite — A File-Backed Table with B+ Tree Indexes

A persistent table implemented in C11:

- 100 `int64_t` attributes, up to 1,000,000 rows;
- Append-only writes (one row at a time);
- Range queries on any attribute, with an optional limit on the number of rows returned;
- Per-attribute B+ tree indexes, each persisted to its own file and loaded automatically on restart;
- Every API is thread-safe (guarded by a read-write lock).

Companion document: **[学习指南.md](学习指南.md)** — a detailed walkthrough from system calls to B+ tree node splits.

## Quick Start

```bash
make            # Build
make demo       # Minimal example
make test       # Functional + concurrency tests
make bench      # 1M-row performance comparison (creates a ~763 MB data file and deletes it afterwards)
```

On Linux you need gcc/clang + make + pthread:

```bash
make CC=clang test
```

## Directory Layout

```
include/table.h        Public API (the only header you need to include)
src/table.c            Table: open / append / query / locking
src/index.c            Indexes: build / load / atomic persistence / catch-up
src/btree.c            In-memory B+ tree: insert / split / range query / serialization
src/util.c             pread/pwrite wrappers, fsync, clock, etc.
src/table_internal.h   On-disk format and internal structures
tests/test_table.c     9 test groups (concurrency, restart, and extreme boundary cases)
tests/bench.c          Performance comparison
examples/demo.c        Minimal example
```

## File Format

```
table.dat
+----------------+--------+--------+-----+--------+
| 512B header    | row 0  | row 1  | ... | row n-1|
+----------------+--------+--------+-----+--------+
                 each row is 100*8 = 800 bytes

table.dat.idx.<col>
+----------------+----------------------------+
| 512B idx hdr   | B+ tree preorder dump      |
+----------------+----------------------------+
```

Row addressing: `offset = 512 + rowid * 800`, so a single `pread` locates a row directly.

## Platform Notes

The official target platform is Linux. `src/plat_win32.c` and `src/compat_win32.h` exist only so the
project can be built and debugged locally on Windows + MinGW (they emulate `pread/pwrite`);
they are not included in the Linux build.
