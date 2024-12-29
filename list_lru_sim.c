// SPDX-License-Identifier: GPL-2.0-only
/*
 * List LRU Simulation
 * Based on Linux kernel's list_lru.c implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#define CONFIG_NUMA 1
#define MAX_NUMNODES 8
#define GFP_KERNEL 0
#define GFP_ATOMIC 1

/* Basic list implementation */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *new,
                            struct list_head *prev,
                            struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

static inline void list_del_init(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}

static inline void list_move(struct list_head *list, struct list_head *head)
{
    __list_del(list->prev, list->next);
    list_add(list, head);
}

static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

/* Memory cgroup simulation */
struct mem_cgroup {
    int id;
    unsigned long usage;
    struct list_head list;
    pthread_mutex_t mutex;
};

#define MAX_MEMCG 256
static struct mem_cgroup *memcg_array[MAX_MEMCG];
static int memcg_count = 0;
static pthread_mutex_t memcg_mutex = PTHREAD_MUTEX_INITIALIZER;

/* LRU structures */
struct list_lru_node {
    struct list_lru_one {
        struct list_head list;
        unsigned long nr_items;
    } lru;
    pthread_spinlock_t lock;
    unsigned long nr_items;
};

struct list_lru_memcg {
    struct list_lru_one *node;
};

struct list_lru {
    struct list_lru_node *node;
    bool memcg_aware;
    struct list_head list;    /* Used by memcg list */
    int shrinker_id;
    void *xa;                /* Extended array for memcg support */
    bool xa_initialized;
};

/* Callback type for list_lru_walk operations */
typedef bool (*list_lru_walk_cb)(struct list_head *item, void *cb_arg);

/* Extended array simulation */
#define XA_CHUNK_SIZE 16
struct xa_chunk {
    void *slots[XA_CHUNK_SIZE];
    struct xa_chunk *next;
};

struct xarray {
    struct xa_chunk *head;
    pthread_mutex_t lock;
};

static struct xarray *xa_create(void)
{
    struct xarray *xa = malloc(sizeof(*xa));
    if (!xa)
        return NULL;
    
    xa->head = NULL;
    pthread_mutex_init(&xa->lock, NULL);
    return xa;
}

static void xa_destroy(struct xarray *xa)
{
    struct xa_chunk *chunk = xa->head;
    while (chunk) {
        struct xa_chunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    pthread_mutex_destroy(&xa->lock);
    free(xa);
}

static int xa_store(struct xarray *xa, unsigned long index, void *entry)
{
    struct xa_chunk *chunk;
    unsigned int chunk_index = index / XA_CHUNK_SIZE;
    unsigned int slot_index = index % XA_CHUNK_SIZE;
    
    pthread_mutex_lock(&xa->lock);
    
    /* Create first chunk if needed */
    if (!xa->head) {
        xa->head = calloc(1, sizeof(struct xa_chunk));
        if (!xa->head) {
            pthread_mutex_unlock(&xa->lock);
            return -ENOMEM;
        }
    }
    
    /* Find or create needed chunk */
    chunk = xa->head;
    while (chunk_index > 0) {
        if (!chunk->next) {
            chunk->next = calloc(1, sizeof(struct xa_chunk));
            if (!chunk->next) {
                pthread_mutex_unlock(&xa->lock);
                return -ENOMEM;
            }
        }
        chunk = chunk->next;
        chunk_index--;
    }
    
    /* Store entry */
    chunk->slots[slot_index] = entry;
    pthread_mutex_unlock(&xa->lock);
    return 0;
}

static void *xa_load(struct xarray *xa, unsigned long index)
{
    struct xa_chunk *chunk;
    unsigned int chunk_index = index / XA_CHUNK_SIZE;
    unsigned int slot_index = index % XA_CHUNK_SIZE;
    void *entry = NULL;
    
    pthread_mutex_lock(&xa->lock);
    
    chunk = xa->head;
    while (chunk && chunk_index > 0) {
        chunk = chunk->next;
        chunk_index--;
    }
    
    if (chunk)
        entry = chunk->slots[slot_index];
    
    pthread_mutex_unlock(&xa->lock);
    return entry;
}

/* Memory cgroup functions */
static struct mem_cgroup *memcg_create(void)
{
    struct mem_cgroup *memcg;
    
    pthread_mutex_lock(&memcg_mutex);
    
    if (memcg_count >= MAX_MEMCG) {
        pthread_mutex_unlock(&memcg_mutex);
        return NULL;
    }
    
    memcg = malloc(sizeof(*memcg));
    if (!memcg) {
        pthread_mutex_unlock(&memcg_mutex);
        return NULL;
    }
    
    memcg->id = memcg_count++;
    memcg->usage = 0;
    INIT_LIST_HEAD(&memcg->list);
    pthread_mutex_init(&memcg->mutex, NULL);
    
    memcg_array[memcg->id] = memcg;
    
    pthread_mutex_unlock(&memcg_mutex);
    return memcg;
}

static void memcg_destroy(struct mem_cgroup *memcg)
{
    if (!memcg)
        return;
    
    pthread_mutex_lock(&memcg_mutex);
    
    memcg_array[memcg->id] = NULL;
    pthread_mutex_destroy(&memcg->mutex);
    free(memcg);
    
    pthread_mutex_unlock(&memcg_mutex);
}

static inline int memcg_kmem_id(struct mem_cgroup *memcg)
{
    return memcg ? memcg->id : -1;
}

/* List LRU functions */
static struct list_lru_one *list_lru_from_memcg_idx(struct list_lru *lru,
                                                   int nid, int idx)
{
    if (lru->memcg_aware && idx >= 0) {
        struct list_lru_memcg *mlru = xa_load(lru->xa, idx);
        return mlru ? &mlru->node[nid] : NULL;
    }
    return &lru->node[nid].lru;
}

bool list_lru_add(struct list_lru *lru, struct list_head *item,
                 int nid, struct mem_cgroup *memcg)
{
    struct list_lru_node *nlru = &lru->node[nid];
    struct list_lru_one *l;
    bool ret = false;
    
    pthread_spin_lock(&nlru->lock);
    if (list_empty(item)) {
        l = list_lru_from_memcg_idx(lru, nid, memcg_kmem_id(memcg));
        if (l) {
            list_add_tail(item, &l->list);
            l->nr_items++;
            nlru->nr_items++;
            ret = true;
        }
    }
    pthread_spin_unlock(&nlru->lock);
    return ret;
}

bool list_lru_del(struct list_lru *lru, struct list_head *item,
                 int nid, struct mem_cgroup *memcg)
{
    struct list_lru_node *nlru = &lru->node[nid];
    struct list_lru_one *l;
    bool ret = false;
    
    pthread_spin_lock(&nlru->lock);
    if (!list_empty(item)) {
        l = list_lru_from_memcg_idx(lru, nid, memcg_kmem_id(memcg));
        if (l) {
            list_del_init(item);
            l->nr_items--;
            nlru->nr_items--;
            ret = true;
        }
    }
    pthread_spin_unlock(&nlru->lock);
    return ret;
}

void list_lru_isolate(struct list_lru_one *list, struct list_head *item)
{
    list_del_init(item);
    list->nr_items--;
}

void list_lru_isolate_move(struct list_lru_one *list, struct list_head *item,
                          struct list_head *head)
{
    list_move(item, head);
    list->nr_items--;
}

unsigned long list_lru_count_one(struct list_lru *lru,
                               int nid, struct mem_cgroup *memcg)
{
    struct list_lru_one *l;
    unsigned long count;
    
    l = list_lru_from_memcg_idx(lru, nid, memcg_kmem_id(memcg));
    count = l ? l->nr_items : 0;
    
    return count;
}

unsigned long list_lru_count_node(struct list_lru *lru, int nid)
{
    struct list_lru_node *nlru = &lru->node[nid];
    return nlru->nr_items;
}

static unsigned long __list_lru_walk_one(struct list_lru *lru, int nid,
                                       int memcg_idx,
                                       list_lru_walk_cb isolate,
                                       void *cb_arg,
                                       unsigned long *nr_to_walk)
{
    struct list_lru_node *nlru = &lru->node[nid];
    struct list_lru_one *l;
    struct list_head *item, *n;
    unsigned long isolated = 0;
    
    pthread_spin_lock(&nlru->lock);
    l = list_lru_from_memcg_idx(lru, nid, memcg_idx);
    if (!l)
        goto out;
    
    list_for_each_safe(item, n, &l->list) {
        if (isolated >= *nr_to_walk)
            break;
        
        if (isolate(item, cb_arg)) {
            list_lru_isolate(l, item);
            isolated++;
            nlru->nr_items--;
        }
    }
    
out:
    pthread_spin_unlock(&nlru->lock);
    *nr_to_walk -= isolated;
    return isolated;
}

unsigned long list_lru_walk_one(struct list_lru *lru, int nid,
                              struct mem_cgroup *memcg,
                              list_lru_walk_cb isolate,
                              void *cb_arg,
                              unsigned long *nr_to_walk)
{
    return __list_lru_walk_one(lru, nid, memcg_kmem_id(memcg),
                              isolate, cb_arg, nr_to_walk);
}

unsigned long list_lru_walk_node(struct list_lru *lru, int nid,
                               list_lru_walk_cb isolate,
                               void *cb_arg,
                               unsigned long *nr_to_walk)
{
    unsigned long isolated = 0;
    unsigned long walk;
    
    /* First walk memcg unaware entries if any */
    walk = *nr_to_walk;
    isolated = __list_lru_walk_one(lru, nid, -1, isolate, cb_arg, &walk);
    
    if (walk > 0 && lru->memcg_aware) {
        int i;
        for (i = 0; i < MAX_MEMCG && walk > 0; i++) {
            if (!memcg_array[i])
                continue;
            isolated += __list_lru_walk_one(lru, nid, i,
                                          isolate, cb_arg, &walk);
        }
    }
    
    *nr_to_walk = walk;
    return isolated;
}

static void init_one_lru(struct list_lru_one *l)
{
    INIT_LIST_HEAD(&l->list);
    l->nr_items = 0;
}

static int init_lru_node(struct list_lru *lru, int nid)
{
    struct list_lru_node *nlru = &lru->node[nid];
    int ret;

    ret = pthread_spin_init(&nlru->lock, PTHREAD_PROCESS_PRIVATE);
    if (ret)
        return ret;

    init_one_lru(&nlru->lru);
    nlru->nr_items = 0;
    return 0;
}

int list_lru_init(struct list_lru *lru)
{
    int i, ret = 0;

    lru->node = calloc(MAX_NUMNODES, sizeof(*lru->node));
    if (!lru->node)
        return -ENOMEM;

    for (i = 0; i < MAX_NUMNODES; i++) {
        ret = init_lru_node(lru, i);
        if (ret)
            goto fail;
    }

    if (lru->memcg_aware) {
        lru->xa = xa_create();
        if (!lru->xa) {
            ret = -ENOMEM;
            goto fail;
        }
        lru->xa_initialized = true;
    }

    return 0;

fail:
    list_lru_destroy(lru);
    return ret;
}

void list_lru_destroy(struct list_lru *lru)
{
    int i;

    if (lru->xa_initialized) {
        xa_destroy(lru->xa);
        lru->xa_initialized = false;
    }

    if (lru->node) {
        for (i = 0; i < MAX_NUMNODES; i++)
            pthread_spin_destroy(&lru->node[i].lock);
        free(lru->node);
        lru->node = NULL;
    }
}

/* Example usage and test functions */
struct test_item {
    struct list_head list;
    int value;
};

static bool test_isolate(struct list_head *item, void *arg)
{
    struct test_item *test = (struct test_item *)item;
    int *target = (int *)arg;
    
    return test->value == *target;
}

void list_lru_test(void)
{
    struct list_lru lru = {};
    struct test_item items[10];
    struct mem_cgroup *memcg;
    int i, target;
    unsigned long nr_to_walk;
    
    /* Initialize LRU */
    lru.memcg_aware = true;
    if (list_lru_init(&lru) != 0) {
        printf("Failed to initialize LRU\n");
        return;
    }
    
    /* Create memory cgroup */
    memcg = memcg_create();
    if (!memcg) {
        printf("Failed to create memcg\n");
        goto out_destroy_lru;
    }
    
    /* Initialize and add test items */
    for (i = 0; i < 10; i++) {
        INIT_LIST_HEAD(&items[i].list);
        items[i].value = i;
        if (!list_lru_add(&lru, &items[i].list, 0, memcg))
            printf("Failed to add item %d\n", i);
    }
    
    /* Test counting */
    printf("Node 0 count: %lu\n", list_lru_count_node(&lru, 0));
    printf("Memcg count: %lu\n", list_lru_count_one(&lru, 0, memcg));
    
    /* Test walking and isolating */
    target = 5;
    nr_to_walk = 1;
    list_lru_walk_one(&lru, 0, memcg, test_isolate, &target, &nr_to_walk);
    
    /* Test removal */
    for (i = 0; i < 10; i++) {
        if (!list_lru_del(&lru, &items[i].list, 0, memcg))
            printf("Failed to remove item %d\n", i);
    }
    
    /* Cleanup */
    memcg_destroy(memcg);
out_destroy_lru:
    list_lru_destroy(&lru);
}

int main(void)
{
    list_lru_test();
    return 0;
}
