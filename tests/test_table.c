/*
 * 功能与并发测试。所有数据都由 rowid/列号确定性生成，
 * 因此可以随时用“暴力扫描”作为标准答案来核对索引查询结果。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "table.h"
#include "util.h"

#define NCOLS   100
#define MAXROWS 1000000

static int failures = 0;

#define CHECK(cond) do {                                              \
    if (!(cond)) {                                                    \
        printf("  [FAIL] line %d: %s\n", __LINE__, #cond);            \
        failures++;                                                   \
    }                                                                 \
} while (0)

/* ---------------- 数据生成 ---------------- */

static int64_t value_of(int64_t rid, int col)
{
    return (int64_t)(tl_hash64((uint64_t)rid * 131 + (uint64_t)col) % 2001) - 1000;
}

static void make_row(int64_t rid, int64_t *row)
{
    int c;
    for (c = 0; c < NCOLS; c++)
        row[c] = value_of(rid, c);
}

/* ---------------- 辅助 ---------------- */

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[65536];
    size_t n;

    if (!in)
        return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
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

/*
 * 精确核对：要求 max_out 足够大，两条路径都能返回全部匹配行。
 * 返回结果按 rowid 升序，逐项比较。
 */
static void check_exact(tl_table *t, int col, int64_t lo, int64_t hi)
{
    int max_out = 1000000;
    int64_t *got = (int64_t *)malloc((size_t)max_out * sizeof(int64_t));
    int64_t *exp = (int64_t *)malloc((size_t)max_out * sizeof(int64_t));
    int64_t *row = (int64_t *)malloc(NCOLS * sizeof(int64_t));
    int64_t n = tl_row_count(t);
    int cnt = 0, more = 0, ecnt = 0, emore = 0, r;
    int first_diff = -1;

    CHECK(tl_search(t, col, lo, hi, got, max_out, &cnt, &more) == TL_OK);

    for (r = 0; r < n; r++) {
        if (tl_get_row(t, r, row) != TL_OK) {
            failures++;
            break;
        }
        if (row[col] >= lo && row[col] <= hi) {
            if (ecnt == max_out) {
                emore = 1;
                break;
            }
            exp[ecnt++] = r;
        }
    }

    CHECK(cnt == ecnt);
    CHECK(more == emore);
    for (r = 0; r < cnt && r < ecnt; r++) {
        if (got[r] != exp[r]) {
            first_diff = r;
            break;
        }
    }
    if (first_diff >= 0) {
        printf("  [FAIL] col=%d range=[%lld,%lld] first diff at %d: got %lld exp %lld\n",
               col, (long long)lo, (long long)hi, first_diff,
               (long long)got[first_diff], (long long)exp[first_diff]);
        failures++;
    }

    free(got);
    free(exp);
    free(row);
}

/* 截断场景：只检查“数量、截断标志、每条结果确实满足条件” */
static void check_capped(tl_table *t, int col, int64_t lo, int64_t hi, int cap)
{
    int64_t out[64];
    int64_t row[NCOLS];
    int cnt = 0, more = 0, i;
    int64_t total = 0, r, n = tl_row_count(t);

    CHECK(cap <= 64);
    CHECK(tl_search(t, col, lo, hi, out, cap, &cnt, &more) == TL_OK);
    CHECK(cnt <= cap);

    for (r = 0; r < n; r++) {
        if (tl_get_row(t, r, row) != TL_OK) {
            failures++;
            return;
        }
        if (row[col] >= lo && row[col] <= hi)
            total++;
    }

    if (total > (int64_t)cap) {
        CHECK(cnt == cap);
        CHECK(more == 1);
    } else {
        CHECK(cnt == (int)total);
        CHECK(more == 0);
    }

    for (i = 0; i < cnt; i++) {
        int64_t v;
        if (out[i] < 0 || out[i] >= n ||
            tl_get_row(t, out[i], row) != TL_OK) {
            failures++;
            continue;
        }
        v = row[col];
        CHECK(v >= lo && v <= hi);
    }
}

/* ---------------- 测试用例 ---------------- */

static void test_basic(void)
{
    const char *path = "tlt_basic.dat";
    tl_table *t;
    int64_t row[NCOLS], back[NCOLS];
    int err = 0, r, c;
    int64_t i;

    printf("[1] 基本读写\n");
    cleanup(path);
    t = tl_open(path, NCOLS, MAXROWS, &err);
    CHECK(t != NULL);
    if (!t)
        return;

    for (i = 0; i < 1000; i++) {
        make_row(i, row);
        CHECK(tl_append(t, row) == i);
    }
    CHECK(tl_row_count(t) == 1000);

    for (i = 0; i < 1000; i += 37) {
        CHECK(tl_get_row(t, i, back) == TL_OK);
        for (c = 0; c < NCOLS; c++)
            CHECK(back[c] == value_of(i, c));
    }
    CHECK(tl_get_row(t, 1000, back) == TL_EINVAL);
    CHECK(tl_get_row(t, -1, back) == TL_EINVAL);

    /* 越界列 / 非法查询参数 */
    CHECK(tl_build_index(t, NCOLS) == TL_EINVAL);
    CHECK(tl_search(t, 0, 0, 1, back, 0, &r, &c) == TL_EINVAL);

    tl_close(t);
}

static void test_search_without_index(void)
{
    const char *path = "tlt_basic.dat";
    tl_table *t;
    int err = 0;

    printf("[2] 无索引查询（全表扫描）\n");
    t = tl_open(path, NCOLS, MAXROWS, &err);
    CHECK(t != NULL);
    if (!t)
        return;
    CHECK(tl_has_index(t, 42) == 0);

    check_exact(t, 42, -1000, 1000);
    check_exact(t, 42, 100, 200);
    check_exact(t, 0, 0, 0);
    check_exact(t, 99, 5000, 6000);       /* 无匹配 */
    check_exact(t, 7, 100, 99);           /* lo > hi */

    check_capped(t, 42, -1000, 1000, 10);
    check_capped(t, 42, 100, 200, 7);

    tl_close(t);
}

static void test_index_and_reopen(void)
{
    const char *path = "tlt_basic.dat";
    const char *idx = "tlt_basic.dat.idx.42";
    const char *bak = "tlt_basic.dat.idx.42.bak";
    tl_table *t;
    int err = 0;
    int64_t row[NCOLS], back[NCOLS];
    int64_t i;
    double t0, t1;

    printf("[3] 建索引、持久化、重启加载\n");
    t = tl_open(path, NCOLS, MAXROWS, &err);
    CHECK(t != NULL);
    if (!t)
        return;

    t0 = tl_now_sec();
    CHECK(tl_build_index(t, 42) == TL_OK);
    t1 = tl_now_sec();
    printf("  建索引耗时 %.4f s\n", t1 - t0);
    CHECK(tl_has_index(t, 42) == 1);
    CHECK(access(idx, F_OK) == 0);
    CHECK(copy_file(idx, bak) == 0);

    check_exact(t, 42, 100, 200);
    check_exact(t, 42, -1000, 1000);
    check_capped(t, 42, -1000, 1000, 10);

    tl_close(t);

    /* 重新打开：索引应自动加载 */
    t = tl_open(path, NCOLS, MAXROWS, &err);
    CHECK(t != NULL);
    if (!t)
        return;
    CHECK(tl_has_index(t, 42) == 1);

    /* 追加 500 行，内存索引应同步增长 */
    for (i = 1000; i < 1500; i++) {
        make_row(i, row);
        CHECK(tl_append(t, row) == i);
    }
    check_exact(t, 42, 100, 200);
    check_exact(t, 42, -1000, 1000);
    tl_close(t);

    /*
     * 模拟“索引文件落后于表”：把建索引时的旧快照覆盖回去，
     * 再打开。加载逻辑应把 [1000,1500) 的尾部行补进索引。
     */
    CHECK(copy_file(bak, idx) == 0);
    t = tl_open(path, NCOLS, MAXROWS, &err);
    CHECK(t != NULL);
    if (!t)
        return;
    CHECK(tl_has_index(t, 42) == 1);
    check_exact(t, 42, 100, 200);
    check_exact(t, 42, -1000, 1000);

    /* 索引内容是否正确：抽查最后一条追加的行 */
    CHECK(tl_get_row(t, 1499, back) == TL_OK);
    CHECK(back[42] == value_of(1499, 42));
    tl_close(t);

    unlink(bak);
}

static void test_duplicates(void)
{
    const char *path = "tlt_dup.dat";
    tl_table *t;
    int64_t row[NCOLS];
    int err = 0, cnt, more, i;
    int64_t out[1000];

    printf("[4] 重复键与截断标志\n");
    cleanup(path);
    t = tl_open(path, NCOLS, MAXROWS, &err);
    CHECK(t != NULL);
    if (!t)
        return;

    for (i = 0; i < 500; i++) {
        make_row(i, row);
        row[0] = 7;                       /* 第 0 列全部相同 */
        CHECK(tl_append(t, row) == i);
    }
    CHECK(tl_build_index(t, 0) == TL_OK);

    CHECK(tl_search(t, 0, 0, 10, out, 1000, &cnt, &more) == TL_OK);
    CHECK(cnt == 500);
    CHECK(more == 0);

    CHECK(tl_search(t, 0, 0, 10, out, 10, &cnt, &more) == TL_OK);
    CHECK(cnt == 10);
    CHECK(more == 1);

    CHECK(tl_search(t, 0, 8, 9, out, 1000, &cnt, &more) == TL_OK);
    CHECK(cnt == 0 && more == 0);

    tl_close(t);
    cleanup(path);
}

static void test_corrupt_index(void)
{
    const char *path = "tlt_basic.dat";
    const char *idx = "tlt_basic.dat.idx.42";
    tl_table *t;
    FILE *fp;
    int err = 0;

    printf("[5] 索引文件损坏时退回全表扫描\n");
    fp = fopen(idx, "wb");
    CHECK(fp != NULL);
    if (fp) {
        fputs("garbage", fp);
        fclose(fp);
    }

    t = tl_open(path, NCOLS, MAXROWS, &err);
    CHECK(t != NULL);
    if (!t)
        return;
    CHECK(tl_has_index(t, 42) == 0);
    check_exact(t, 42, 100, 200);
    tl_close(t);
}

/* ---------- 并发测试 ---------- */

typedef struct {
    tl_table *t;
    int w;
    int n;
    int errors;
} writer_arg;

static void *writer_thread(void *p)
{
    writer_arg *a = (writer_arg *)p;
    int64_t row[NCOLS];
    int i, c;

    for (i = 0; i < a->n; i++) {
        for (c = 0; c < NCOLS; c++)
            row[c] = value_of(a->w * 100000 + i, c);
        row[0] = (int64_t)a->w * 1000000 + i;   /* 便于校验归属 */
        if (tl_append(a->t, row) < 0)
            a->errors++;
    }
    return NULL;
}

typedef struct {
    tl_table *t;
    int iters;
    int errors;
    uint64_t seed;
} searcher_arg;

static void *searcher_thread(void *p)
{
    searcher_arg *a = (searcher_arg *)p;
    int64_t out[64];
    int64_t row[NCOLS];
    int i;

    for (i = 0; i < a->iters; i++) {
        int64_t base = (int64_t)(tl_hash64(a->seed++) % 4000000);
        int64_t lo = base, hi = base + 500;
        int cnt = 0, more = 0, k;

        if (tl_search(a->t, 0, lo, hi, out, 64, &cnt, &more) != TL_OK) {
            a->errors++;
            continue;
        }
        if (cnt > 64) {
            a->errors++;
            continue;
        }
        for (k = 0; k < cnt; k++) {
            if (out[k] < 0 || out[k] >= tl_row_count(a->t) ||
                tl_get_row(a->t, out[k], row) != TL_OK ||
                row[0] < lo || row[0] > hi) {
                a->errors++;
                break;
            }
        }
    }
    return NULL;
}

static void test_concurrency(void)
{
    const char *path = "tlt_mt.dat";
    enum { NW = 4, NS = 3, PER = 2000 };
    tl_table *t;
    pthread_t wth[NW], sth[NS];
    writer_arg wa[NW];
    searcher_arg sa[NS];
    int err = 0, i, bad = 0;
    int64_t row[NCOLS];
    unsigned char *seen;
    double t0, t1;

    printf("[6] 多线程并发追加 + 查询（已有索引）\n");
    cleanup(path);
    t = tl_open(path, NCOLS, MAXROWS, &err);
    CHECK(t != NULL);
    if (!t)
        return;

    /* 先对空表建索引，之后所有 append 都会走“增量维护索引”的路径 */
    CHECK(tl_build_index(t, 0) == TL_OK);

    t0 = tl_now_sec();
    for (i = 0; i < NS; i++) {
        sa[i].t = t;
        sa[i].iters = 4000;
        sa[i].errors = 0;
        sa[i].seed = (uint64_t)(i + 1) * 7919;
        pthread_create(&sth[i], NULL, searcher_thread, &sa[i]);
    }
    for (i = 0; i < NW; i++) {
        wa[i].t = t;
        wa[i].w = i;
        wa[i].n = PER;
        wa[i].errors = 0;
        pthread_create(&wth[i], NULL, writer_thread, &wa[i]);
    }
    for (i = 0; i < NW; i++)
        pthread_join(wth[i], NULL);
    for (i = 0; i < NS; i++)
        pthread_join(sth[i], NULL);
    t1 = tl_now_sec();

    for (i = 0; i < NW; i++)
        bad += wa[i].errors;
    for (i = 0; i < NS; i++)
        bad += sa[i].errors;
    CHECK(bad == 0);
    CHECK(tl_row_count(t) == NW * PER);
    printf("  %d 次追加 + %d 次查询耗时 %.3f s\n",
           NW * PER, NS * 4000, t1 - t0);

    /* 每一行都应存在且恰好出现一次，第 0 列落在对应线程的取值区间 */
    seen = (unsigned char *)calloc((size_t)(NW * PER), 1);
    CHECK(seen != NULL);
    for (i = 0; i < (int)tl_row_count(t); i++) {
        int w, k;
        CHECK(tl_get_row(t, i, row) == TL_OK);
        w = (int)(row[0] / 1000000);
        k = (int)(row[0] % 1000000);
        CHECK(w >= 0 && w < NW && k >= 0 && k < PER);
        if (w >= 0 && w < NW && k >= 0 && k < PER) {
            seen[w * PER + k] = 1;
        }
    }
    for (i = 0; i < NW * PER; i++)
        if (!seen[i])
            bad++;
    CHECK(bad == 0);
    free(seen);

    /* 并发结束后，索引结果必须与暴力扫描完全一致 */
    check_exact(t, 0, 0, 4000000);
    check_exact(t, 0, 1234500, 1234600);
    check_capped(t, 0, 0, 4000000, 20);

    tl_close(t);
    cleanup(path);
}

static void test_capacity(void)
{
    const char *path = "tlt_cap.dat";
    tl_table *t;
    int64_t row[NCOLS];
    int err = 0, i;

    printf("[7] 容量上限\n");
    cleanup(path);
    t = tl_open(path, NCOLS, 10, &err);
    CHECK(t != NULL);
    if (!t)
        return;
    for (i = 0; i < 10; i++) {
        make_row(i, row);
        CHECK(tl_append(t, row) == i);
    }
    CHECK(tl_append(t, row) == TL_ENOSPC);
    CHECK(tl_row_count(t) == 10);
    tl_close(t);

    t = tl_open(path, 1, 1, &err);         /* 已有表：忽略传入的列数/上限 */
    CHECK(t != NULL);
    if (t) {
        CHECK(tl_row_count(t) == 10);
        CHECK(tl_append(t, row) == TL_ENOSPC);
        tl_close(t);
    }
    cleanup(path);
}

static void test_extremes(void)
{
    const char *path = "tlt_ext.dat";
    tl_table *t;
    int64_t row[NCOLS];
    int64_t vals[4];
    int err = 0, i;

    printf("[9] int64 极值边界\n");
    cleanup(path);
    t = tl_open(path, NCOLS, MAXROWS, &err);
    CHECK(t != NULL);
    if (!t)
        return;

    vals[0] = INT64_MIN;
    vals[1] = INT64_MAX;
    vals[2] = 0;
    vals[3] = -1;
    for (i = 0; i < 200; i++) {
        make_row(i, row);
        row[0] = vals[i % 4];
        CHECK(tl_append(t, row) == i);
    }
    CHECK(tl_build_index(t, 0) == TL_OK);

    check_exact(t, 0, INT64_MIN, INT64_MAX);
    check_exact(t, 0, INT64_MIN, INT64_MIN);
    check_exact(t, 0, INT64_MAX, INT64_MAX);
    check_exact(t, 0, -1, -1);
    check_capped(t, 0, INT64_MIN, INT64_MAX, 10);

    tl_close(t);
    cleanup(path);
}

static void test_drop_index(void)
{
    const char *path = "tlt_basic.dat";
    tl_table *t;
    int err = 0;

    printf("[8] 删除索引后退回扫描\n");
    t = tl_open(path, NCOLS, MAXROWS, &err);
    CHECK(t != NULL);
    if (!t)
        return;
    /* 前面的损坏测试已使 42 列索引不可用，这里先重建再删除 */
    CHECK(tl_build_index(t, 42) == TL_OK);
    CHECK(tl_has_index(t, 42) == 1);
    CHECK(tl_drop_index(t, 42) == TL_OK);
    CHECK(tl_has_index(t, 42) == 0);
    CHECK(tl_drop_index(t, 42) == TL_ENOENT);
    check_exact(t, 42, 100, 200);
    tl_close(t);
}

int main(void)
{
    printf("==== tablelite 测试开始 ====\n");

    test_basic();
    test_search_without_index();
    test_index_and_reopen();
    test_duplicates();
    test_corrupt_index();
    test_concurrency();
    test_capacity();
    test_drop_index();
    test_extremes();

    printf("==== %s：失败 %d 项 ====\n",
           failures == 0 ? "全部通过" : "存在失败", failures);

    cleanup("tlt_basic.dat");
    cleanup("tlt_mt.dat");
    cleanup("tlt_dup.dat");
    cleanup("tlt_cap.dat");
    return failures == 0 ? 0 : 1;
}
