#include "btree.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 阶数 64：内部节点最多 64 个孩子（63 个分隔键），叶子最多 63 条记录。
 * 100 万条记录时树高约为 4，范围查询只需 4 次节点内查找。
 */
#define BT_ORDER       64
#define BT_MAX_KEYS    (BT_ORDER - 1)
#define BT_MAX_CHILDREN BT_ORDER
#define BT_MAX_DEPTH   32

typedef struct bt_node {
    int leaf;                           /* 1 = 叶子 */
    int n;                              /* 键的数量 */
    /*
     * 数组各多留一个槽位：插入瞬间节点可能暂时有 MAX_KEYS+1 个键，
     * 随后立刻被分裂。不留这个“溢出槽”就是经典的越界写 bug。
     */
    int64_t key[BT_MAX_KEYS + 1];       /* value */
    int64_t rid[BT_MAX_KEYS + 1];       /* rowid */
    struct bt_node *child[BT_MAX_CHILDREN + 1];
    struct bt_node *next;               /* 仅叶子使用：指向下一个叶子 */
} bt_node;

/*
 * OOM 直接终止：B+ 树的分裂过程会同时改写多个节点，
 * 做到“分配失败可回滚”会让代码复杂度翻倍，而本项目的内存需求很小
 * （100 万条索引约 30MB），这里选择让程序显式失败而不是带着半截树继续跑。
 */
static void *bt_alloc(size_t n)
{
    void *p = calloc(1, n);
    if (!p) {
        fprintf(stderr, "tablelite: 内存耗尽\n");
        abort();
    }
    return p;
}

static int key_cmp(int64_t k1, int64_t r1, int64_t k2, int64_t r2)
{
    if (k1 < k2) return -1;
    if (k1 > k2) return 1;
    if (r1 < r2) return -1;
    if (r1 > r2) return 1;
    return 0;
}

/* 找到第一个 >= (key,rid) 的位置（叶子插入点） */
static int leaf_pos(const bt_node *nd, int64_t key, int64_t rid)
{
    int lo = 0, hi = nd->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (key_cmp(nd->key[mid], nd->rid[mid], key, rid) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* 找到第一个大于 (key,rid) 的分隔键，即应下降到的孩子下标 */
static int child_pos(const bt_node *nd, int64_t key, int64_t rid)
{
    int lo = 0, hi = nd->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (key_cmp(nd->key[mid], nd->rid[mid], key, rid) <= 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

bt_node *bt_create(void)
{
    bt_node *nd = bt_alloc(sizeof(*nd));
    nd->leaf = 1;
    nd->n = 0;
    nd->next = NULL;
    return nd;
}

void bt_destroy(bt_node *root)
{
    int i;
    if (!root)
        return;
    if (!root->leaf) {
        for (i = 0; i <= root->n; i++)
            bt_destroy(root->child[i]);
    }
    free(root);
}

/* 分裂结果：若 split=1，则 (key,rid) 与 right 需要插入父节点 */
typedef struct {
    int split;
    int64_t key;
    int64_t rid;
    bt_node *right;
} bt_split;

static void insert_rec(bt_node *nd, int64_t key, int64_t rid, bt_split *sp)
{
    int pos;

    sp->split = 0;

    if (nd->leaf) {
        pos = leaf_pos(nd, key, rid);
        if (pos < nd->n &&
            key_cmp(nd->key[pos], nd->rid[pos], key, rid) == 0)
            return;                     /* 完全重复，忽略 */

        memmove(&nd->key[pos + 1], &nd->key[pos],
                (size_t)(nd->n - pos) * sizeof(int64_t));
        memmove(&nd->rid[pos + 1], &nd->rid[pos],
                (size_t)(nd->n - pos) * sizeof(int64_t));
        nd->key[pos] = key;
        nd->rid[pos] = rid;
        nd->n++;

        if (nd->n <= BT_MAX_KEYS)
            return;

        /* 叶子分裂：右半部分搬到新节点，并把新节点的首键提升给父节点 */
        {
            int mid = nd->n / 2;
            int right_n = nd->n - mid;
            bt_node *right = bt_create();

            memcpy(right->key, &nd->key[mid], (size_t)right_n * sizeof(int64_t));
            memcpy(right->rid, &nd->rid[mid], (size_t)right_n * sizeof(int64_t));
            right->n = right_n;
            nd->n = mid;

            right->next = nd->next;     /* 维护叶子链表，范围查询依赖它 */
            nd->next = right;

            sp->split = 1;
            sp->key = right->key[0];
            sp->rid = right->rid[0];
            sp->right = right;
        }
        return;
    }

    pos = child_pos(nd, key, rid);
    insert_rec(nd->child[pos], key, rid, sp);
    if (!sp->split)
        return;

    /* 把子节点分裂产生的分隔键插到本节点 */
    memmove(&nd->key[pos + 1], &nd->key[pos],
            (size_t)(nd->n - pos) * sizeof(int64_t));
    memmove(&nd->rid[pos + 1], &nd->rid[pos],
            (size_t)(nd->n - pos) * sizeof(int64_t));
    memmove(&nd->child[pos + 2], &nd->child[pos + 1],
            (size_t)(nd->n - pos) * sizeof(bt_node *));
    nd->key[pos] = sp->key;
    nd->rid[pos] = sp->rid;
    nd->child[pos + 1] = sp->right;
    nd->n++;
    sp->split = 0;

    if (nd->n <= BT_MAX_KEYS)
        return;

    /* 内部节点分裂：中间键上移，不留在本层 */
    {
        int mid = nd->n / 2;
        int right_n = nd->n - mid - 1;
        bt_node *right = bt_create();

        right->leaf = 0;
        memcpy(right->key, &nd->key[mid + 1],
               (size_t)right_n * sizeof(int64_t));
        memcpy(right->rid, &nd->rid[mid + 1],
               (size_t)right_n * sizeof(int64_t));
        memcpy(right->child, &nd->child[mid + 1],
               (size_t)(right_n + 1) * sizeof(bt_node *));
        right->n = right_n;

        sp->split = 1;
        sp->key = nd->key[mid];
        sp->rid = nd->rid[mid];
        sp->right = right;

        nd->n = mid;
    }
}

void bt_insert(bt_node **root, int64_t key, int64_t rid)
{
    bt_split sp;

    insert_rec(*root, key, rid, &sp);
    if (sp.split) {
        /* 根节点分裂：长高一层 */
        bt_node *nr = bt_create();
        nr->leaf = 0;
        nr->n = 1;
        nr->key[0] = sp.key;
        nr->rid[0] = sp.rid;
        nr->child[0] = *root;
        nr->child[1] = sp.right;
        *root = nr;
    }
}

void bt_range(bt_node *root, int64_t lo, int64_t hi,
              int64_t *out, int max_out, int *out_count, int *out_has_more)
{
    bt_node *leaf;
    int n = 0, more = 0;

    *out_count = 0;
    *out_has_more = 0;
    if (!root || lo > hi || max_out <= 0)
        return;

    /* 1. 从根一路下降到“第一个可能包含 >= lo 的叶子” */
    leaf = root;
    while (!leaf->leaf)
        leaf = leaf->child[child_pos(leaf, lo, INT64_MIN)];

    /* 2. 沿叶子链表向右扫描；key 有序，越界即可停 */
    for (; leaf && !more; leaf = leaf->next) {
        int i;
        for (i = 0; i < leaf->n; i++) {
            if (leaf->key[i] > hi)
                goto done;
            if (leaf->key[i] < lo)
                continue;
            if (n == max_out) {
                more = 1;               /* 发现第 max_out+1 个匹配 */
                goto done;
            }
            out[n++] = leaf->rid[i];
        }
    }
done:
    *out_count = n;
    *out_has_more = more;
}

int64_t bt_entry_count(const bt_node *root)
{
    const bt_node *leaf;
    int64_t total = 0;

    if (!root)
        return 0;
    leaf = root;
    while (!leaf->leaf)
        leaf = leaf->child[0];
    for (; leaf; leaf = leaf->next)
        total += leaf->n;
    return total;
}

int bt_save(const bt_node *nd, FILE *fp)
{
    uint8_t leaf = (uint8_t)nd->leaf;
    uint32_t n = (uint32_t)nd->n;
    int i;

    if (fwrite(&leaf, 1, 1, fp) != 1)
        return -1;
    if (fwrite(&n, sizeof(n), 1, fp) != 1)
        return -1;
    if (n > 0) {
        if (fwrite(nd->key, sizeof(int64_t), n, fp) != n)
            return -1;
        if (fwrite(nd->rid, sizeof(int64_t), n, fp) != n)
            return -1;
    }
    if (!nd->leaf) {
        for (i = 0; i <= nd->n; i++)
            if (bt_save(nd->child[i], fp) != 0)
                return -1;
    }
    return 0;
}

typedef struct {
    bt_node *last_leaf;   /* 用于重建叶子链表 */
    int64_t nodes;
    int ok;
} bt_load_ctx;

static bt_node *load_rec(FILE *fp, int depth, int64_t max_nodes, bt_load_ctx *ctx)
{
    uint8_t leaf;
    uint32_t n;
    bt_node *nd;
    int i;

    if (!ctx->ok)
        return NULL;
    if (depth > BT_MAX_DEPTH || ++ctx->nodes > max_nodes) {
        ctx->ok = 0;
        return NULL;
    }

    if (fread(&leaf, 1, 1, fp) != 1 ||
        fread(&n, sizeof(n), 1, fp) != 1) {
        ctx->ok = 0;
        return NULL;
    }
    if (leaf > 1 || n > BT_MAX_KEYS || (!leaf && n == 0)) {
        ctx->ok = 0;
        return NULL;
    }

    nd = bt_alloc(sizeof(*nd));
    nd->leaf = leaf ? 1 : 0;
    nd->n = (int)n;

    if (n > 0) {
        if (fread(nd->key, sizeof(int64_t), n, fp) != n ||
            fread(nd->rid, sizeof(int64_t), n, fp) != n) {
            free(nd);
            ctx->ok = 0;
            return NULL;
        }
    }

    if (nd->leaf) {
        if (ctx->last_leaf)
            ctx->last_leaf->next = nd;
        ctx->last_leaf = nd;
        return nd;
    }

    for (i = 0; i <= nd->n; i++) {
        nd->child[i] = load_rec(fp, depth + 1, max_nodes, ctx);
        if (!ctx->ok) {
            bt_destroy(nd);
            return NULL;
        }
    }
    return nd;
}

bt_node *bt_load(FILE *fp, int64_t max_nodes, int *ok)
{
    bt_load_ctx ctx;
    bt_node *root;

    ctx.last_leaf = NULL;
    ctx.nodes = 0;
    ctx.ok = 1;

    root = load_rec(fp, 0, max_nodes, &ctx);
    *ok = ctx.ok;
    return ctx.ok ? root : NULL;
}
