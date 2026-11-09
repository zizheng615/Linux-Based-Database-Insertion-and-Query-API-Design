/*
 * 最小使用示例：建表 -> 追加 -> 查询 -> 建索引 -> 查询 -> 重启再查。
 * 编译: make demo     运行: ./examples/demo
 */
#include <stdio.h>
#include <unistd.h>

#include "table.h"

#define NCOLS 100

int main(void)
{
    const char *path = "demo.dat";
    tl_table *t;
    int64_t row[NCOLS];
    int64_t hits[10];
    int err = 0, cnt = 0, more = 0, i, c;

    unlink(path);
    unlink("demo.dat.idx.3");

    /* 打开（不存在则创建）：100 列，最多 100 万行 */
    t = tl_open(path, NCOLS, 1000000, &err);
    if (!t) {
        printf("open failed: %s\n", tl_strerror(err));
        return 1;
    }

    /* 追加 1000 行；第 3 列取 0..99 循环出现，便于制造重复值 */
    for (i = 0; i < 1000; i++) {
        for (c = 0; c < NCOLS; c++)
            row[c] = (int64_t)i * 1000 + c;
        row[3] = i % 100;
        if (tl_append(t, row) != i) {
            printf("append failed\n");
            return 1;
        }
    }
    printf("追加完成，行数 = %lld\n", (long long)tl_row_count(t));

    /* 没有索引：全表扫描，结果按 rowid 升序，最多返回 10 条 */
    tl_search(t, 3, 10, 20, hits, 10, &cnt, &more);
    printf("无索引查询 col3 in [10,20]：返回 %d 条，还有更多=%d\n", cnt, more);

    /* 为第 3 列建索引，并写入 demo.dat.idx.3 */
    if (tl_build_index(t, 3) != TL_OK) {
        printf("build index failed\n");
        return 1;
    }
    printf("建索引完成，has_index(3) = %d\n", tl_has_index(t, 3));

    tl_search(t, 3, 10, 20, hits, 10, &cnt, &more);
    printf("有索引查询 col3 in [10,20]：返回 %d 条，还有更多=%d，前 3 条 rowid = ",
           cnt, more);
    for (i = 0; i < cnt && i < 3; i++)
        printf("%lld ", (long long)hits[i]);
    printf("\n");

    /* 关闭再打开：索引文件应被自动发现并加载 */
    tl_close(t);
    t = tl_open(path, NCOLS, 1000000, &err);
    if (!t) {
        printf("reopen failed: %s\n", tl_strerror(err));
        return 1;
    }
    printf("重启后 has_index(3) = %d\n", tl_has_index(t, 3));
    printf("重启后行数 = %lld\n", (long long)tl_row_count(t));

    tl_close(t);
    unlink(path);
    unlink("demo.dat.idx.3");
    return 0;
}
