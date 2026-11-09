/*
 * 模块内部共享定义：数据文件格式、表结构、扫描回调。
 * 不对外暴露，调用者只使用 include/table.h。
 */
#ifndef TL_TABLE_INTERNAL_H
#define TL_TABLE_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "table.h"
#include "index.h"

/* 数据文件前 512 字节是表头；行长 = ncols * 8 */
#define TL_HEADER_SIZE      512
#define TL_IDX_HEADER_SIZE  512

#define TL_MAGIC       0x544C5442u   /* "TLTB" */
#define TL_IDX_MAGIC   0x54584C54u   /* "TLXT" */
#define TL_VERSION     1u

/*
 * 表头。整个结构固定 512 字节，写在一块磁盘扇区内。
 * 注意：这里依赖本机字节序与编译器对齐；索引文件同理，
 * 属于“同一台机器/同一 ABI 内使用”的简化设计，跨架构不可移植。
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t ncols;
    uint32_t reserved;
    int64_t  max_rows;
    int64_t  nrows;        /* 已提交的行数 */
    uint64_t uuid;         /* 标识“这张表”，用于校验索引文件归属 */
    uint8_t  pad[TL_HEADER_SIZE - 40];
} tl_header;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t col;
    uint32_t ncols;
    uint64_t uuid;
    int64_t  indexed_rows; /* 该索引覆盖到第几行（不含） */
    int64_t  nentries;     /* 叶子中的记录数 */
    uint8_t  pad[TL_IDX_HEADER_SIZE - 40];
} tl_idx_header;

_Static_assert(sizeof(tl_header) == TL_HEADER_SIZE, "header size");
_Static_assert(sizeof(tl_idx_header) == TL_IDX_HEADER_SIZE, "index header size");

struct tl_index;

struct tl_table {
    int fd;
    char *path;
    uint32_t ncols;
    int64_t max_rows;
    int64_t nrows;          /* 内存中的权威行数 */
    uint64_t uuid;
    pthread_rwlock_t lock;  /* 一把读写锁保护表、索引、索引文件三者的修改 */
    tl_index *idx[TL_MAX_COLS];
};

static inline int64_t tl_row_size(const tl_table *t)
{
    return (int64_t)t->ncols * (int64_t)sizeof(int64_t);
}

/*
 * 按列扫描 [start, end) 行，对每行调用 cb(rowid, value, ud)。
 * cb 返回：0 继续，1 提前正常结束，-1 出错。
 * 返回 0 表示成功（含提前结束），-1 表示 I/O 或回调错误。
 */
int tl_scan_column(tl_table *t, int col, int64_t start, int64_t end,
                   int (*cb)(int64_t rowid, int64_t value, void *ud), void *ud);

#endif /* TL_TABLE_INTERNAL_H */
