# tablelite —— 带 B+ 树索引的文件表存储

**[English](README.en.md)** | 中文

用 C11 实现一张可持久化的表：

- 100 个 `int64_t` 属性，最大 1,000,000 行；
- 追加一行（append-only）；
- 按任意属性做范围查询，可限制返回条数；
- 按属性建立 B+ 树索引，索引写入独立文件，重启自动加载；
- 所有 API 线程安全（读写锁）。

配套文档：**[学习指南.md](学习指南.md)** —— 从系统调用到 B+ 树分裂的详细讲解。

## 快速开始

```bash
make            # 编译
make demo       # 最小示例
make test       # 功能 + 并发测试
make bench      # 100 万行性能对比（会生成约 763MB 数据文件，结束后删除）
```

Linux 下需要 gcc/clang + make + pthread：

```bash
make CC=clang test
```

## 目录结构

```
include/table.h        公共 API（唯一需要 include 的头文件）
src/table.c            表：打开/追加/查询/锁
src/index.c            索引：构建/加载/原子落盘/补齐
src/btree.c            内存 B+ 树：插入/分裂/范围查询/序列化
src/util.c             pread/pwrite 封装、fsync、时钟等
src/table_internal.h   文件格式与内部结构
tests/test_table.c     9 组测试（含并发、重启、极值边界）
tests/bench.c          性能对比
examples/demo.c        最小示例
```

## 文件格式

```
table.dat
+----------------+--------+--------+-----+--------+
| 512B 表头       | 行 0   | 行 1   | ... | 行 n-1 |
+----------------+--------+--------+-----+--------+
                 每条 100*8 = 800 字节

table.dat.idx.<col>
+----------------+----------------------------+
| 512B 索引头     | B+ 树先序序列化              |
+----------------+----------------------------+
```

行地址：`offset = 512 + rowid * 800`，一次 `pread` 直接定位。

## 平台说明

正式目标平台是 Linux。`src/plat_win32.c` 与 `src/compat_win32.h` 只是为了让 Windows + MinGW
也能本地编译调试（模拟 `pread/pwrite`），Linux 构建不会包含它们。
