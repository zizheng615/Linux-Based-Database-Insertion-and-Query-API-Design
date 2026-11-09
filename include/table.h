/*
 * tablelite —— 基于文件的定长表存储 + B+ 树索引
 *
 * 这是作业唯一需要包含的公共头文件。调用者只依赖这里声明的接口，
 * 不依赖内部结构，便于替换存储格式或索引结构。
 *
 * 设计约定：
 *   - 一行固定 100 列（可配置，最多 TL_MAX_COLS），每列 int64_t；
 *   - 表以“追加”方式增长，不支持删除/修改（作业不要求）；
 *   - 所有接口都是线程安全的，可在多线程下并发调用。
 */
#ifndef TABLELITE_TABLE_H
#define TABLELITE_TABLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一行的最大列数，与作业要求一致 */
#define TL_MAX_COLS 100

/* 返回值约定：0 或 >=0 表示成功，以下负值表示错误 */
#define TL_OK        0
#define TL_EINVAL  (-1)   /* 参数非法 */
#define TL_EIO     (-2)   /* 读写文件失败 */
#define TL_ENOSPC  (-3)   /* 表已满（达到 max_rows） */
#define TL_ECORRUPT (-4)  /* 文件格式损坏 */
#define TL_ENOENT  (-5)   /* 索引不存在 */

typedef struct tl_table tl_table;

/*
 * 打开表；文件不存在则创建。
 *   path     数据文件路径，例如 "data/table.dat"
 *   ncols    列数（仅在新建时生效，1..TL_MAX_COLS）
 *   max_rows 最大行数（仅在新建时生效，例如 1000000）
 *   err      可选，失败时写入错误码（可为 NULL）
 * 返回 NULL 表示失败；成功返回表句柄。
 *
 * 打开已有表时，ncols/max_rows 以文件头中的记录为准，传入值被忽略。
 * 打开过程中会自动发现并加载该表已有的索引文件。
 */
tl_table *tl_open(const char *path, int ncols, int64_t max_rows, int *err);

/* 关闭表：把尚未落盘的索引写回磁盘，释放全部资源。 */
void tl_close(tl_table *t);

/* 当前行数（只读，O(1)）。 */
int64_t tl_row_count(tl_table *t);

/*
 * 在表尾追加一行。
 *   row 指向 ncols 个 int64_t，调用者拥有该内存。
 * 成功返回新行的行号（从 0 开始），失败返回负的错误码。
 * 追加时如果该列已有索引，会同步维护索引（内存中）。
 */
int64_t tl_append(tl_table *t, const int64_t *row);

/* 按行号读取一行，row 需容纳 ncols 个 int64_t。成功返回 0。 */
int tl_get_row(tl_table *t, int64_t rowid, int64_t *row);

/*
 * 为第 col 列建立索引，并写入索引文件 "<table>.idx.<col>"。
 * 已有索引会被重建。成功返回 0。
 */
int tl_build_index(tl_table *t, int col);

/* 删除第 col 列的索引（同时删除索引文件）。成功返回 0。 */
int tl_drop_index(tl_table *t, int col);

/* 第 col 列当前是否有可用索引：1 有，0 无。 */
int tl_has_index(tl_table *t, int col);

/*
 * 范围查询：返回第 col 列取值在 [lo, hi] 内的行号。
 *   out_rowids   输出数组，至少能容纳 max_out 个 int64_t
 *   max_out      最多返回多少个行号（例如 10）；必须 > 0
 *   out_count    实际返回的行数
 *   out_has_more 是否还有更多匹配行（1 表示结果被截断）
 * 返回的行号按升序排列。
 *
 * 若该列已有索引则走索引；否则退化为全表扫描。
 * 成功返回 0。
 */
int tl_search(tl_table *t, int col, int64_t lo, int64_t hi,
              int64_t *out_rowids, int max_out,
              int *out_count, int *out_has_more);

/*
 * 把内存中已修改的索引刷到磁盘，并同步表头。
 * 正常关闭时会自动调用；中途也可以主动调用。
 */
int tl_checkpoint(tl_table *t);

/* 错误码转字符串，便于打印。 */
const char *tl_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* TABLELITE_TABLE_H */
