/*
 * 性能对比：无索引全表扫描 vs B+ 树索引。
 * 用法: ./tests/bench [行数]    默认 1000000 行（作业要求的规模）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "table.h"
#include "util.h"

#define NCOLS 100

#ifndef O_BINARY
#define O_BINARY 0
#endif

static int64_t path_size(const char *path)
{
    int fd = open(path, O_RDONLY | O_BINARY);
    int64_t sz;

    if (fd < 0)
        return -1;
    sz = io_file_size(fd);
    close(fd);
    return sz;
}

static int64_t value_of(int64_t rid, int col)
{
    return (int64_t)(tl_hash64((uint64_t)rid * 131 + (uint64_t)col) % 1000000);
}

static void cleanup(const char *path)
{
    char buf[4096];
    int c;

    unlink(path);
    for (c = 0; c < NCOLS; c++) {
        snprintf(buf, sizeof(buf), "%s.idx.%d", path, c);
        unlink(buf);
        snprintf(buf, sizeof(buf), "%s.idx.%d.tmp", path, c);
        unlink(buf);
    }
}

static double time_search(tl_table *t, int col, int64_t lo, int64_t hi, int runs)
{
    int64_t out[64];
    int cnt, more, i;
    double t0 = tl_now_sec();

    for (i = 0; i < runs; i++) {
        if (tl_search(t, col, lo, hi, out, 64, &cnt, &more) != TL_OK) {
            printf("search failed\n");
            exit(1);
        }
    }
    return (tl_now_sec() - t0) / (double)runs;
}

/* 每次查询落在不同的范围，避免“同一路径反复命中缓存”的乐观偏差 */
static double time_search_random(tl_table *t, int col, int runs)
{
    int64_t out[64];
    int cnt, more, i;
    double t0 = tl_now_sec();

    for (i = 0; i < runs; i++) {
        int64_t base = (int64_t)(tl_hash64((uint64_t)i + 12345) % 1000000);
        if (tl_search(t, col, base, base + 10, out, 64, &cnt, &more) != TL_OK) {
            printf("search failed\n");
            exit(1);
        }
    }
    return (tl_now_sec() - t0) / (double)runs;
}

int main(int argc, char **argv)
{
    int64_t n = (argc > 1) ? atoll(argv[1]) : 1000000;
    const char *path = "bench_table.dat";
    const char *idxpath = "bench_table.dat.idx.7";
    tl_table *t;
    int err = 0;
    int64_t *row, i;
    int c;
    double t0, t1, scan_s, idx_s, build_s, open_s;
    int64_t out_scan[64], out_idx[64];
    int cnt_scan = 0, cnt_idx = 0, more = 0, same = 1;
    int64_t file_sz, idx_sz;

    printf("==== tablelite 性能测试 ====\n");
    printf("主机名: %s\n", tl_hostname());
    printf("行数: %lld, 列数: %d, 单行字节: %d\n",
           (long long)n, NCOLS, NCOLS * 8);

    cleanup(path);
    t = tl_open(path, NCOLS, 1000000, &err);
    if (!t) {
        printf("open failed: %s\n", tl_strerror(err));
        return 1;
    }

    row = (int64_t *)malloc(NCOLS * sizeof(int64_t));
    if (!row)
        return 1;

    /* 1. 顺序追加 */
    t0 = tl_now_sec();
    for (i = 0; i < n; i++) {
        for (c = 0; c < NCOLS; c++)
            row[c] = value_of(i, c);
        if (tl_append(t, row) < 0) {
            printf("append failed at %lld\n", (long long)i);
            return 1;
        }
    }
    t1 = tl_now_sec();
    printf("\n[追加] %lld 行耗时 %.3f s (%.0f 行/秒)\n",
           (long long)n, t1 - t0, (double)n / (t1 - t0));
    file_sz = path_size(path);
    printf("       数据文件 %.1f MB\n",
           file_sz > 0 ? (double)file_sz / (1024.0 * 1024.0) : 0.0);

    /* 2. 无索引：全表扫描一个很窄的范围 */
    tl_search(t, 7, 500000, 500010, out_scan, 64, &cnt_scan, &more);
    scan_s = time_search(t, 7, 500000, 500010, 3);
    printf("\n[查询-无索引] 范围 [500000,500010]，命中 %d 行，平均每次 %.1f ms\n",
           cnt_scan, scan_s * 1000.0);

    /* 3. 建索引 */
    t0 = tl_now_sec();
    if (tl_build_index(t, 7) != TL_OK) {
        printf("build index failed\n");
        return 1;
    }
    build_s = tl_now_sec() - t0;
    idx_sz = path_size(idxpath);
    printf("[建索引] 属性 7 耗时 %.3f s，索引文件 %.1f MB\n",
           build_s, idx_sz > 0 ? (double)idx_sz / (1024.0 * 1024.0) : 0.0);

    /* 4. 有索引：同样范围（多次执行以摊薄计时误差） */
    idx_s = time_search(t, 7, 500000, 500010, 20000);
    tl_search(t, 7, 500000, 500010, out_idx, 64, &cnt_idx, &more);
    printf("[查询-有索引] 平均每次 %.1f us，加速比 %.0f 倍\n",
           idx_s * 1e6, scan_s / idx_s);
    {
        double rnd_s = time_search_random(t, 7, 20000);
        printf("[查询-有索引/随机范围] 平均每次 %.1f us\n", rnd_s * 1e6);
    }

    if (cnt_scan != cnt_idx)
        same = 0;
    for (i = 0; i < cnt_scan && i < cnt_idx; i++)
        if (out_scan[i] != out_idx[i])
            same = 0;
    printf("[正确性] 两种路径结果%s\n", same ? "一致" : "不一致（错误！）");

    /* 5. 关闭后重开：验证索引从文件恢复 */
    tl_close(t);
    t0 = tl_now_sec();
    t = tl_open(path, NCOLS, 1000000, &err);
    open_s = tl_now_sec() - t0;
    if (!t) {
        printf("reopen failed: %s\n", tl_strerror(err));
        return 1;
    }
    printf("\n[重启] 打开耗时 %.3f s (含加载索引)，有索引=%d\n",
           open_s, tl_has_index(t, 7));
    idx_s = time_search(t, 7, 500000, 500010, 20000);
    printf("[重启后查询] 平均每次 %.1f us\n", idx_s * 1e6);

    tl_close(t);
    free(row);
    cleanup(path);

    printf("\n==== 测试结束 ====\n");
    return same ? 0 : 1;
}
