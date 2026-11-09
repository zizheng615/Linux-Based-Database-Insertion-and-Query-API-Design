#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "index.h"
#include "table_internal.h"
#include "util.h"

int tl_index_path(struct tl_table *t, int col, char *buf, size_t n)
{
    int w = snprintf(buf, n, "%s.idx.%d", t->path, col);
    return (w > 0 && (size_t)w < n) ? 0 : -1;
}

/* ---------- 建索引 ---------- */

typedef struct {
    bt_node *root;
} build_ctx;

static int build_cb(int64_t rowid, int64_t value, void *ud)
{
    build_ctx *b = (build_ctx *)ud;
    bt_insert(&b->root, value, rowid);
    return 0;
}

tl_index *tl_index_build(struct tl_table *t, int col)
{
    build_ctx b;
    tl_index *ix;

    b.root = bt_create();
    if (tl_scan_column(t, col, 0, t->nrows, build_cb, &b) != 0) {
        bt_destroy(b.root);
        return NULL;
    }

    ix = (tl_index *)calloc(1, sizeof(*ix));
    if (!ix) {
        bt_destroy(b.root);
        return NULL;
    }
    ix->col = (uint32_t)col;
    ix->root = b.root;
    ix->indexed_rows = t->nrows;
    ix->nentries = t->nrows;
    ix->dirty = 1;
    return ix;
}

/* ---------- 落盘 ---------- */

int tl_index_save(struct tl_table *t, tl_index *ix)
{
    char path[4096];
    char tmp[4200];
    tl_idx_header h;
    FILE *fp;
    int failed = 0;

    if (tl_index_path(t, (int)ix->col, path, sizeof(path)) != 0)
        return -1;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return -1;

    fp = fopen(tmp, "wb");
    if (!fp)
        return -1;

    memset(&h, 0, sizeof(h));
    h.magic = TL_IDX_MAGIC;
    h.version = TL_VERSION;
    h.col = ix->col;
    h.ncols = t->ncols;
    h.uuid = t->uuid;
    h.indexed_rows = ix->indexed_rows;
    h.nentries = ix->nentries;

    if (fwrite(&h, sizeof(h), 1, fp) != 1)
        failed = 1;
    if (!failed && bt_save(ix->root, fp) != 0)
        failed = 1;
    if (!failed && fflush(fp) != 0)
        failed = 1;
    if (!failed && fsync(fileno(fp)) != 0)
        failed = 1;
    if (fclose(fp) != 0)
        failed = 1;

    if (failed) {
        unlink(tmp);
        return -1;
    }

    /* rename 在同一文件系统内是原子的：读者要么看到旧索引，要么看到新索引 */
    if (io_rename_replace(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    io_sync_dir(path);
    ix->dirty = 0;
    return 0;
}

/* ---------- 加载 ---------- */

typedef struct {
    bt_node *root;
} catchup_ctx;

static int catchup_cb(int64_t rowid, int64_t value, void *ud)
{
    catchup_ctx *c = (catchup_ctx *)ud;
    bt_insert(&c->root, value, rowid);
    return 0;
}

tl_index *tl_index_load(struct tl_table *t, int col)
{
    char path[4096];
    tl_idx_header h;
    tl_index *ix;
    FILE *fp;
    int ok = 1;
    int64_t max_nodes;

    if (tl_index_path(t, col, path, sizeof(path)) != 0)
        return NULL;

    fp = fopen(path, "rb");
    if (!fp)
        return NULL;

    if (fread(&h, sizeof(h), 1, fp) != 1)
        goto bad;

    if (h.magic != TL_IDX_MAGIC || h.version != TL_VERSION ||
        h.col != (uint32_t)col || h.ncols != t->ncols ||
        h.uuid != t->uuid ||
        h.indexed_rows < 0 || h.indexed_rows > t->nrows ||
        h.nentries != h.indexed_rows)
        goto bad;

    /* 节点数上限：正常树的最小填充率是 50%，再加常数余量 */
    max_nodes = h.nentries / 16 + 1024;

    ix = (tl_index *)calloc(1, sizeof(*ix));
    if (!ix)
        goto bad;

    ix->col = (uint32_t)col;
    ix->root = bt_load(fp, max_nodes, &ok);
    if (!ok) {
        tl_index_free(ix);
        goto bad;
    }
    if (bt_entry_count(ix->root) != h.nentries) {
        tl_index_free(ix);
        goto bad;
    }

    /*
     * 表中可能在上次写索引之后又追加了行。索引文件头记录了它覆盖到哪一行，
     * 这里把“尾部新增的行”补插进树里，使内存索引重新与表对齐。
     */
    if (h.indexed_rows < t->nrows) {
        catchup_ctx c;
        c.root = ix->root;
        if (tl_scan_column(t, col, h.indexed_rows, t->nrows,
                           catchup_cb, &c) != 0) {
            tl_index_free(ix);
            goto bad;
        }
    }

    ix->indexed_rows = t->nrows;
    ix->nentries = t->nrows;
    ix->dirty = 0;
    fclose(fp);
    return ix;

bad:
    fclose(fp);
    return NULL;
}

void tl_index_free(tl_index *ix)
{
    if (!ix)
        return;
    bt_destroy(ix->root);
    free(ix);
}
