#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "table.h"
#include "table_internal.h"
#include "index.h"
#include "util.h"

/* Windows 上 open() 默认文本模式，会把 0x0A/0x1A 当特殊字符处理，必须显式二进制 */
#ifndef O_BINARY
#define O_BINARY 0
#endif

/* ---------------- 内部工具 ---------------- */

static int write_header(tl_table *t, int64_t nrows)
{
    tl_header h;

    memset(&h, 0, sizeof(h));
    h.magic = TL_MAGIC;
    h.version = TL_VERSION;
    h.ncols = t->ncols;
    h.max_rows = t->max_rows;
    h.nrows = nrows;
    h.uuid = t->uuid;

    return io_pwrite(t->fd, &h, sizeof(h), 0) == (ssize_t)sizeof(h) ? 0 : -1;
}

int tl_scan_column(tl_table *t, int col, int64_t start, int64_t end,
                   int (*cb)(int64_t, int64_t, void *), void *ud)
{
    enum { BATCH_ROWS = 64 };       /* 64 行 * 800B ≈ 50KB，对页缓存友好 */
    int64_t rowsz, r;
    size_t bufsz;
    char *buf;
    int rc = 0;

    if (!t || col < 0 || col >= (int)t->ncols || !cb)
        return -1;
    if (start < 0)
        start = 0;
    if (end > t->nrows)
        end = t->nrows;

    rowsz = tl_row_size(t);
    bufsz = (size_t)BATCH_ROWS * (size_t)rowsz;
    buf = (char *)malloc(bufsz);
    if (!buf)
        return -1;

    r = start;
    while (r < end && rc == 0) {
        int64_t batch = end - r;
        size_t bytes;
        int64_t i;
        ssize_t got;

        if (batch > BATCH_ROWS)
            batch = BATCH_ROWS;
        bytes = (size_t)batch * (size_t)rowsz;

        got = io_pread(t->fd, buf, bytes,
                       TL_HEADER_SIZE + r * rowsz);
        if (got != (ssize_t)bytes) {
            rc = -1;
            break;
        }

        for (i = 0; i < batch; i++) {
            int64_t v;
            int cbrc;

            memcpy(&v, buf + (size_t)i * (size_t)rowsz + (size_t)col * 8,
                   sizeof(v));
            cbrc = cb(r + i, v, ud);
            if (cbrc < 0) {
                rc = -1;
                break;
            }
            if (cbrc > 0) {         /* 调用者要求提前结束 */
                break;
            }
        }
        if (rc != 0)
            break;
        if (i < batch)              /* 提前结束 */
            break;
        r += batch;
    }

    free(buf);
    return rc;
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static int checkpoint_locked(tl_table *t)
{
    int rc = 0;
    uint32_t c;

    for (c = 0; c < t->ncols; c++) {
        if (t->idx[c] && t->idx[c]->dirty) {
            if (tl_index_save(t, t->idx[c]) != 0)
                rc = -1;
        }
    }
    if (write_header(t, t->nrows) != 0)
        rc = -1;
    if (io_sync(t->fd) != 0)
        rc = -1;
    return rc;
}

/* ---------------- 生命周期 ---------------- */

tl_table *tl_open(const char *path, int ncols, int64_t max_rows, int *err)
{
    tl_table *t = NULL;
    tl_header h;
    int fd = -1;
    int e = TL_OK;
    int64_t rowsz;
    int64_t fsize;

    if (err)
        *err = TL_OK;
    if (!path || path[0] == '\0') {
        if (err)
            *err = TL_EINVAL;
        return NULL;
    }

    fd = open(path, O_RDWR | O_CREAT | O_BINARY, 0644);
    if (fd < 0) {
        if (err)
            *err = TL_EIO;
        return NULL;
    }
    fsize = io_file_size(fd);
    if (fsize < 0) {
        e = TL_EIO;
        goto fail;
    }

    memset(&h, 0, sizeof(h));

    if (fsize == 0) {
        /* 新表：参数在此生效 */
        if (ncols < 1 || ncols > TL_MAX_COLS ||
            max_rows < 1 || max_rows > 100000000LL) {
            e = TL_EINVAL;
            goto fail;
        }
        h.magic = TL_MAGIC;
        h.version = TL_VERSION;
        h.ncols = (uint32_t)ncols;
        h.max_rows = max_rows;
        h.nrows = 0;
        h.uuid = tl_new_uuid();

        if (io_pwrite(fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h)) {
            e = TL_EIO;
            goto fail;
        }
        if (io_sync(fd) != 0) {
            e = TL_EIO;
            goto fail;
        }
    } else {
        /* 已有表：以文件头为准 */
        if (fsize < TL_HEADER_SIZE) {
            e = TL_ECORRUPT;
            goto fail;
        }
        if (io_pread(fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h)) {
            e = TL_EIO;
            goto fail;
        }
        if (h.magic != TL_MAGIC || h.version != TL_VERSION ||
            h.ncols < 1 || h.ncols > TL_MAX_COLS ||
            h.max_rows < 1 || h.nrows < 0 || h.nrows > h.max_rows) {
            e = TL_ECORRUPT;
            goto fail;
        }

        rowsz = (int64_t)h.ncols * 8;
        {
            int64_t avail = (fsize - TL_HEADER_SIZE) / rowsz;
            int64_t expected;

            /*
             * 崩溃可能留下“数据已写、表头未提交”的尾巴：
             * 以表头为准，把多余字节截掉；若文件反而短了，则只能按实际可用行数收敛。
             */
            if (h.nrows > avail)
                h.nrows = avail;

            expected = TL_HEADER_SIZE + h.nrows * rowsz;
            if (fsize > expected) {
                if (io_truncate(fd, expected) != 0) {
                    e = TL_EIO;
                    goto fail;
                }
            }
        }
    }

    t = (tl_table *)calloc(1, sizeof(*t));
    if (!t) {
        e = TL_EIO;
        goto fail;
    }
    t->fd = fd;
    t->ncols = h.ncols;
    t->max_rows = h.max_rows;
    t->nrows = h.nrows;
    t->uuid = h.uuid;
    t->path = strdup(path);
    if (!t->path) {
        e = TL_EIO;
        goto fail;
    }
    if (pthread_rwlock_init(&t->lock, NULL) != 0) {
        e = TL_EIO;
        goto fail;
    }

    /* 发现索引文件就加载；损坏/不属于本表的文件会被忽略并退回扫描 */
    {
        uint32_t c;
        for (c = 0; c < t->ncols; c++) {
            tl_index *ix = tl_index_load(t, (int)c);
            if (ix)
                t->idx[c] = ix;
        }
    }

    if (err)
        *err = TL_OK;
    return t;

fail:
    if (fd >= 0)
        close(fd);
    if (t) {
        free(t->path);
        free(t);
    }
    if (err)
        *err = e;
    return NULL;
}

void tl_close(tl_table *t)
{
    uint32_t c;

    if (!t)
        return;

    pthread_rwlock_wrlock(&t->lock);
    checkpoint_locked(t);
    pthread_rwlock_unlock(&t->lock);

    for (c = 0; c < t->ncols; c++)
        tl_index_free(t->idx[c]);
    close(t->fd);
    pthread_rwlock_destroy(&t->lock);
    free(t->path);
    free(t);
}

/* ---------------- 读写 ---------------- */

int64_t tl_row_count(tl_table *t)
{
    int64_t n;

    if (!t)
        return TL_EINVAL;
    pthread_rwlock_rdlock(&t->lock);
    n = t->nrows;
    pthread_rwlock_unlock(&t->lock);
    return n;
}

int64_t tl_append(tl_table *t, const int64_t *row)
{
    int64_t rid;
    int64_t rowsz;
    uint32_t c;

    if (!t || !row)
        return TL_EINVAL;

    pthread_rwlock_wrlock(&t->lock);

    if (t->nrows >= t->max_rows) {
        pthread_rwlock_unlock(&t->lock);
        return TL_ENOSPC;
    }

    rid = t->nrows;
    rowsz = tl_row_size(t);

    /*
     * 顺序很重要：先写数据，再提交表头。
     * 若在两步之间崩溃，表头里的行数仍然是旧值，这一行不可见，
     * 下次打开时会被截掉；反过来则会读到未写数据的“幽灵行”。
     */
    if (io_pwrite(t->fd, row, (size_t)rowsz,
                  TL_HEADER_SIZE + rid * rowsz) != (ssize_t)rowsz) {
        pthread_rwlock_unlock(&t->lock);
        return TL_EIO;
    }
    if (write_header(t, rid + 1) != 0) {
        pthread_rwlock_unlock(&t->lock);
        return TL_EIO;
    }
    t->nrows = rid + 1;

    /* 同步维护所有已加载的索引（内存中），索引文件等 checkpoint 时再写 */
    for (c = 0; c < t->ncols; c++) {
        tl_index *ix = t->idx[c];
        if (ix) {
            bt_insert(&ix->root, row[c], rid);
            ix->nentries++;
            ix->indexed_rows = t->nrows;
            ix->dirty = 1;
        }
    }

    pthread_rwlock_unlock(&t->lock);
    return rid;
}

int tl_get_row(tl_table *t, int64_t rowid, int64_t *row)
{
    int64_t rowsz;
    ssize_t got;

    if (!t || !row || rowid < 0)
        return TL_EINVAL;

    pthread_rwlock_rdlock(&t->lock);
    if (rowid >= t->nrows) {
        pthread_rwlock_unlock(&t->lock);
        return TL_EINVAL;
    }
    rowsz = tl_row_size(t);
    got = io_pread(t->fd, row, (size_t)rowsz,
                   TL_HEADER_SIZE + rowid * rowsz);
    pthread_rwlock_unlock(&t->lock);

    return got == (ssize_t)rowsz ? TL_OK : TL_EIO;
}

/* ---------------- 查询 ---------------- */

typedef struct {
    int64_t lo, hi;
    int64_t *out;
    int max;
    int cnt;
    int more;
} search_ctx;

static int search_cb(int64_t rowid, int64_t value, void *ud)
{
    search_ctx *s = (search_ctx *)ud;

    if (value < s->lo || value > s->hi)
        return 0;
    if (s->cnt == s->max) {
        s->more = 1;
        return 1;                   /* 已满，且确认还有下一个匹配 */
    }
    s->out[s->cnt++] = rowid;
    return 0;
}

int tl_search(tl_table *t, int col, int64_t lo, int64_t hi,
              int64_t *out_rowids, int max_out,
              int *out_count, int *out_has_more)
{
    int cnt = 0, more = 0;

    if (!t || !out_rowids || !out_count || !out_has_more ||
        col < 0 || col >= (int)t->ncols || max_out <= 0)
        return TL_EINVAL;

    pthread_rwlock_rdlock(&t->lock);

    if (lo <= hi) {
        tl_index *ix = t->idx[col];
        if (ix) {
            bt_range(ix->root, lo, hi, out_rowids, max_out, &cnt, &more);
        } else {
            search_ctx s;
            s.lo = lo;
            s.hi = hi;
            s.out = out_rowids;
            s.max = max_out;
            s.cnt = 0;
            s.more = 0;
            if (tl_scan_column(t, col, 0, t->nrows, search_cb, &s) != 0) {
                pthread_rwlock_unlock(&t->lock);
                return TL_EIO;
            }
            cnt = s.cnt;
            more = s.more;
        }
    }

    pthread_rwlock_unlock(&t->lock);

    /* 索引路径按 (value,rowid) 排序，扫描路径按 rowid 排序；统一成 rowid 升序 */
    if (cnt > 1)
        qsort(out_rowids, (size_t)cnt, sizeof(int64_t), cmp_i64);

    *out_count = cnt;
    *out_has_more = more;
    return TL_OK;
}

/* ---------------- 索引管理 ---------------- */

int tl_build_index(tl_table *t, int col)
{
    tl_index *ix;

    if (!t || col < 0 || col >= (int)t->ncols)
        return TL_EINVAL;

    pthread_rwlock_wrlock(&t->lock);

    /*
     * 在写锁内构建，保证扫描到的行集与安装索引时的行集一致：
     * 不会出现“一边建索引一边追加”的漏行。代价是建索引期间写操作被阻塞。
     */
    ix = tl_index_build(t, col);
    if (!ix) {
        pthread_rwlock_unlock(&t->lock);
        return TL_EIO;
    }
    if (tl_index_save(t, ix) != 0) {
        tl_index_free(ix);
        pthread_rwlock_unlock(&t->lock);
        return TL_EIO;
    }
    if (t->idx[col])
        tl_index_free(t->idx[col]);
    t->idx[col] = ix;

    pthread_rwlock_unlock(&t->lock);
    return TL_OK;
}

int tl_drop_index(tl_table *t, int col)
{
    char path[4096];

    if (!t || col < 0 || col >= (int)t->ncols)
        return TL_EINVAL;

    pthread_rwlock_wrlock(&t->lock);
    if (!t->idx[col]) {
        pthread_rwlock_unlock(&t->lock);
        return TL_ENOENT;
    }
    tl_index_free(t->idx[col]);
    t->idx[col] = NULL;
    if (tl_index_path(t, col, path, sizeof(path)) == 0)
        unlink(path);
    pthread_rwlock_unlock(&t->lock);
    return TL_OK;
}

int tl_has_index(tl_table *t, int col)
{
    int r;

    if (!t || col < 0 || col >= (int)t->ncols)
        return 0;
    pthread_rwlock_rdlock(&t->lock);
    r = t->idx[col] != NULL;
    pthread_rwlock_unlock(&t->lock);
    return r;
}

int tl_checkpoint(tl_table *t)
{
    int rc;

    if (!t)
        return TL_EINVAL;
    pthread_rwlock_wrlock(&t->lock);
    rc = checkpoint_locked(t);
    pthread_rwlock_unlock(&t->lock);
    return rc == 0 ? TL_OK : TL_EIO;
}

const char *tl_strerror(int err)
{
    switch (err) {
    case TL_OK:        return "ok";
    case TL_EINVAL:    return "invalid argument";
    case TL_EIO:       return "I/O error";
    case TL_ENOSPC:    return "table is full";
    case TL_ECORRUPT:  return "corrupt file";
    case TL_ENOENT:    return "not found";
    default:           return "unknown error";
    }
}
