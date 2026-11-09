/* 单列索引：内存 B+ 树 + 磁盘快照。 */
#ifndef TL_INDEX_H
#define TL_INDEX_H

#include <stddef.h>
#include <stdint.h>

#include "btree.h"

struct tl_table;

typedef struct tl_index {
    uint32_t col;
    int64_t indexed_rows;   /* 已覆盖的行数，正常情况下等于表的 nrows */
    int64_t nentries;       /* 树中条目数 */
    int dirty;              /* 是否有未落盘的修改 */
    bt_node *root;
} tl_index;

/* 扫描全表构建索引（调用者需持有写锁）。失败返回 NULL。 */
tl_index *tl_index_build(struct tl_table *t, int col);

/* 原子写入索引文件：先写 .tmp，fsync，再 rename 覆盖。成功返回 0。 */
int tl_index_save(struct tl_table *t, tl_index *ix);

/*
 * 尝试加载第 col 列的索引文件。
 * 文件不存在或格式/归属校验失败时返回 NULL（调用者退回全表扫描）。
 * 加载后会把 [indexed_rows, nrows) 这段“索引尚不知道”的新行补进树里。
 */
tl_index *tl_index_load(struct tl_table *t, int col);

void tl_index_free(tl_index *ix);

/* 拼接索引文件路径 "<table>.idx.<col>"，成功返回 0。 */
int tl_index_path(struct tl_table *t, int col, char *buf, size_t n);

#endif /* TL_INDEX_H */
