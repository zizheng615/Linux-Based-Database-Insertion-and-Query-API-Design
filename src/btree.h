/*
 * 内存 B+ 树：索引在运行时完全驻留内存，索引文件是它的序列化快照。
 *
 * 键是 (value, rowid) 复合键：
 *   - 只按 value 比较会让重复值无法区分/排序稳定；
 *   - 加上 rowid 后所有键全局唯一，且同一 value 下的行按 rowid 有序。
 * 对外的范围查询只看 value，重复值由叶子链表自然覆盖。
 */
#ifndef TL_BTREE_H
#define TL_BTREE_H

#include <stdint.h>
#include <stdio.h>

typedef struct bt_node bt_node;

/* 创建空树（返回根节点，初始为叶子）。 */
bt_node *bt_create(void);

/* 释放整棵树。 */
void bt_destroy(bt_node *root);

/* 插入一个 (key, rid)。重复的 (key, rid) 会被忽略。 */
void bt_insert(bt_node **root, int64_t key, int64_t rid);

/*
 * 范围查询：收集 value 在 [lo, hi] 内的 rowid，最多 max_out 个。
 * out_count 返回实际个数，out_has_more 返回是否还有更多匹配。
 * 结果顺序为 (value, rowid) 升序。
 */
void bt_range(bt_node *root, int64_t lo, int64_t hi,
              int64_t *out, int max_out, int *out_count, int *out_has_more);

/* 叶子链上的条目总数（用于校验索引文件）。 */
int64_t bt_entry_count(const bt_node *root);

/* 先序序列化到 fp；成功返回 0。 */
int bt_save(const bt_node *root, FILE *fp);

/*
 * 从 fp 反序列化。
 * max_nodes 是节点数上限，防止损坏文件导致巨量内存分配；
 * ok 置 0 表示失败，此时返回 NULL。
 */
bt_node *bt_load(FILE *fp, int64_t max_nodes, int *ok);

#endif /* TL_BTREE_H */
