// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kernel Samepage Merging Simulation
 * Based on Linux kernel's KSM implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Configuration parameters */
#define KSM_MAX_PAGES 1000000
#define KSM_SCAN_RATIO 10
#define KSM_SLEEP_MS 20
#define KSM_PAGE_SIZE 4096
#define KSM_MAX_SCAN_PAGES 100
#define NUMA_NODES 4

/* RB-tree implementation */
struct rb_node {
    unsigned long rb_parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
};

struct rb_root {
    struct rb_node *rb_node;
};

#define RB_RED   0
#define RB_BLACK 1

#define rb_parent(r) ((struct rb_node *)((r)->rb_parent_color & ~3))
#define rb_color(r)  ((r)->rb_parent_color & 1)
#define rb_is_red(r)    (!rb_color(r))
#define rb_is_black(r)  rb_color(r)

/* Memory page representation */
struct page {
    unsigned long flags;
    void *virtual;
    unsigned long pfn;
    int count;
    struct list_head lru;
    union {
        struct {
            struct rb_node node;
            unsigned long private;
        };
        struct {
            struct list_head list;
            void *mapping;
        };
    };
};

/* List implementation */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

/* KSM structures */
struct ksm_stable_node {
    union {
        struct rb_node node;
        struct {
            struct list_head *head;
            struct {
                struct list_head list;
                struct list_head hlist;
            };
        };
    };
    unsigned long checksum;
    unsigned long pfn;
    int rmap_hlist_len;
    int nid;
};

struct ksm_rmap_item {
    struct list_head rmap_list;
    struct list_head anon_vma;
    int nid;
    unsigned long address;
    unsigned long oldchecksum;
    struct rb_node node;
    struct ksm_stable_node *head;
    struct list_head hlist;
    unsigned char age;
    unsigned char remaining_skips;
};

struct ksm_scan {
    struct list_head *mm_slot;
    unsigned long address;
    struct ksm_rmap_item **rmap_list;
    unsigned long seqnr;
};

/* Global KSM state */
static struct rb_root stable_tree[NUMA_NODES];
static struct rb_root unstable_tree[NUMA_NODES];
static struct ksm_scan ksm_scan;
static unsigned long ksm_pages_shared;
static unsigned long ksm_pages_sharing;
static unsigned long ksm_pages_unshared;
static unsigned long ksm_rmap_items;
static unsigned long ksm_stable_nodes;
static bool ksm_merge_across_nodes = true;
static pthread_mutex_t ksm_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ksm_thread_cond = PTHREAD_COND_INITIALIZER;
static bool ksm_run = false;

/* RB-tree functions */
static void rb_link_node(struct rb_node *node, struct rb_node *parent,
                        struct rb_node **rb_link)
{
    node->rb_parent_color = (unsigned long)parent;
    node->rb_left = node->rb_right = NULL;
    *rb_link = node;
}

static void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *parent, *gparent;

    while ((parent = rb_parent(node)) && rb_is_red(parent)) {
        gparent = rb_parent(parent);
        if (parent == gparent->rb_left) {
            struct rb_node *uncle = gparent->rb_right;
            if (uncle && rb_is_red(uncle)) {
                rb_set_black(uncle);
                rb_set_black(parent);
                rb_set_red(gparent);
                node = gparent;
                continue;
            }
            if (parent->rb_right == node) {
                struct rb_node *tmp;
                __rb_rotate_left(parent, root);
                tmp = parent;
                parent = node;
                node = tmp;
            }
            rb_set_black(parent);
            rb_set_red(gparent);
            __rb_rotate_right(gparent, root);
        } else {
            struct rb_node *uncle = gparent->rb_left;
            if (uncle && rb_is_red(uncle)) {
                rb_set_black(uncle);
                rb_set_black(parent);
                rb_set_red(gparent);
                node = gparent;
                continue;
            }
            if (parent->rb_left == node) {
                struct rb_node *tmp;
                __rb_rotate_right(parent, root);
                tmp = parent;
                parent = node;
                node = tmp;
            }
            rb_set_black(parent);
            rb_set_red(gparent);
            __rb_rotate_left(gparent, root);
        }
    }
    rb_set_black(root->rb_node);
}

/* List functions */
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

/* KSM core functions */
static unsigned long calc_checksum(void *data, size_t size)
{
    unsigned long checksum = 0;
    unsigned char *ptr = data;
    size_t i;

    for (i = 0; i < size; i++)
        checksum += ptr[i];
    return checksum;
}

static struct page *alloc_page(void)
{
    struct page *page = calloc(1, sizeof(*page));
    if (page) {
        page->virtual = mmap(NULL, KSM_PAGE_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page->virtual == MAP_FAILED) {
            free(page);
            return NULL;
        }
        INIT_LIST_HEAD(&page->lru);
    }
    return page;
}

static void free_page(struct page *page)
{
    if (page) {
        if (page->virtual)
            munmap(page->virtual, KSM_PAGE_SIZE);
        free(page);
    }
}

static struct ksm_stable_node *alloc_stable_node(void)
{
    struct ksm_stable_node *node = calloc(1, sizeof(*node));
    if (node) {
        INIT_LIST_HEAD(&node->list);
        INIT_LIST_HEAD(&node->hlist);
        ksm_stable_nodes++;
    }
    return node;
}

static void free_stable_node(struct ksm_stable_node *node)
{
    if (node) {
        ksm_stable_nodes--;
        free(node);
    }
}

static struct ksm_rmap_item *alloc_rmap_item(void)
{
    struct ksm_rmap_item *item = calloc(1, sizeof(*item));
    if (item) {
        INIT_LIST_HEAD(&item->rmap_list);
        INIT_LIST_HEAD(&item->anon_vma);
        INIT_LIST_HEAD(&item->hlist);
        ksm_rmap_items++;
    }
    return item;
}

static void free_rmap_item(struct ksm_rmap_item *item)
{
    if (item) {
        ksm_rmap_items--;
        free(item);
    }
}

static int try_to_merge_with_ksm_page(struct ksm_rmap_item *rmap_item,
                                     struct page *page, struct page *kpage)
{
    if (!kpage || !page)
        return -EINVAL;

    if (memcmp(page->virtual, kpage->virtual, KSM_PAGE_SIZE) != 0)
        return -EINVAL;

    /* Merge successful */
    ksm_pages_sharing++;
    return 0;
}

static struct page *stable_tree_search(struct page *page, int nid)
{
    struct rb_node *node = stable_tree[nid].rb_node;
    unsigned long checksum;

    checksum = calc_checksum(page->virtual, KSM_PAGE_SIZE);
    
    while (node) {
        struct ksm_stable_node *stable_node;
        int result;

        stable_node = rb_entry(node, struct ksm_stable_node, node);
        result = checksum - stable_node->checksum;

        if (result < 0)
            node = node->rb_left;
        else if (result > 0)
            node = node->rb_right;
        else {
            struct page *kpage = alloc_page();
            if (!kpage)
                return NULL;
            memcpy(kpage->virtual, page->virtual, KSM_PAGE_SIZE);
            return kpage;
        }
    }
    return NULL;
}

static struct ksm_stable_node *stable_tree_insert(struct page *kpage, int nid)
{
    struct rb_node **new = &stable_tree[nid].rb_node;
    struct rb_node *parent = NULL;
    struct ksm_stable_node *stable_node;
    unsigned long checksum;

    checksum = calc_checksum(kpage->virtual, KSM_PAGE_SIZE);
    
    while (*new) {
        struct ksm_stable_node *this;
        int result;

        this = rb_entry(*new, struct ksm_stable_node, node);
        result = checksum - this->checksum;

        parent = *new;
        if (result < 0)
            new = &(*new)->rb_left;
        else if (result > 0)
            new = &(*new)->rb_right;
        else
            return this;
    }

    stable_node = alloc_stable_node();
    if (!stable_node)
        return NULL;

    stable_node->checksum = checksum;
    stable_node->pfn = kpage->pfn;
    stable_node->nid = nid;

    rb_link_node(&stable_node->node, parent, new);
    rb_insert_color(&stable_node->node, &stable_tree[nid]);

    return stable_node;
}

static struct ksm_rmap_item *unstable_tree_search_insert(struct ksm_rmap_item *rmap_item,
                                                        struct page *page, int nid)
{
    struct rb_node **new = &unstable_tree[nid].rb_node;
    struct rb_node *parent = NULL;
    unsigned long checksum;

    checksum = calc_checksum(page->virtual, KSM_PAGE_SIZE);
    rmap_item->oldchecksum = checksum;
    
    while (*new) {
        struct ksm_rmap_item *this;
        int result;

        this = rb_entry(*new, struct ksm_rmap_item, node);
        result = checksum - this->oldchecksum;

        parent = *new;
        if (result < 0)
            new = &(*new)->rb_left;
        else if (result > 0)
            new = &(*new)->rb_right;
        else
            return this;
    }

    rb_link_node(&rmap_item->node, parent, new);
    rb_insert_color(&rmap_item->node, &unstable_tree[nid]);

    return NULL;
}

static void *ksm_scan_thread(void *arg)
{
    while (1) {
        pthread_mutex_lock(&ksm_thread_mutex);
        while (!ksm_run)
            pthread_cond_wait(&ksm_thread_cond, &ksm_thread_mutex);
        pthread_mutex_unlock(&ksm_thread_mutex);

        if (!ksm_run)
            break;

        /* Scan pages */
        int i;
        for (i = 0; i < KSM_MAX_SCAN_PAGES; i++) {
            struct page *page = alloc_page();
            if (!page)
                continue;

            /* Generate some random content */
            memset(page->virtual, rand() % 256, KSM_PAGE_SIZE);

            /* Try to merge */
            int nid = rand() % NUMA_NODES;
            struct page *kpage = stable_tree_search(page, nid);
            if (kpage) {
                struct ksm_rmap_item *rmap_item = alloc_rmap_item();
                if (rmap_item) {
                    if (try_to_merge_with_ksm_page(rmap_item, page, kpage) == 0) {
                        printf("Page merged successfully\n");
                    }
                    free_rmap_item(rmap_item);
                }
                free_page(kpage);
            } else {
                struct ksm_rmap_item *rmap_item = alloc_rmap_item();
                if (rmap_item) {
                    struct ksm_rmap_item *tree_rmap_item;
                    tree_rmap_item = unstable_tree_search_insert(rmap_item, page, nid);
                    if (tree_rmap_item) {
                        printf("Found duplicate in unstable tree\n");
                        /* Move to stable tree */
                        struct ksm_stable_node *stable_node;
                        stable_node = stable_tree_insert(page, nid);
                        if (stable_node) {
                            ksm_pages_shared++;
                            printf("Pages moved to stable tree\n");
                        }
                    }
                    free_rmap_item(rmap_item);
                }
            }

            free_page(page);
        }

        /* Sleep between scans */
        usleep(KSM_SLEEP_MS * 1000);
    }

    return NULL;
}

/* KSM control interface */
void ksm_init(void)
{
    int i;
    for (i = 0; i < NUMA_NODES; i++) {
        stable_tree[i].rb_node = NULL;
        unstable_tree[i].rb_node = NULL;
    }

    ksm_pages_shared = 0;
    ksm_pages_sharing = 0;
    ksm_pages_unshared = 0;
    ksm_rmap_items = 0;
    ksm_stable_nodes = 0;

    srand(time(NULL));
}

void ksm_run_thread(void)
{
    pthread_t thread;
    pthread_mutex_lock(&ksm_thread_mutex);
    ksm_run = true;
    pthread_create(&thread, NULL, ksm_scan_thread, NULL);
    pthread_detach(thread);
    pthread_mutex_unlock(&ksm_thread_mutex);
    pthread_cond_signal(&ksm_thread_cond);
}

void ksm_stop_thread(void)
{
    pthread_mutex_lock(&ksm_thread_mutex);
    ksm_run = false;
    pthread_mutex_unlock(&ksm_thread_mutex);
    pthread_cond_signal(&ksm_thread_cond);
}

void ksm_show_stats(void)
{
    printf("KSM Statistics:\n");
    printf("Pages Shared: %lu\n", ksm_pages_shared);
    printf("Pages Sharing: %lu\n", ksm_pages_sharing);
    printf("Pages Unshared: %lu\n", ksm_pages_unshared);
    printf("Rmap Items: %lu\n", ksm_rmap_items);
    printf("Stable Nodes: %lu\n", ksm_stable_nodes);
}

/* Test program */
int main(void)
{
    printf("Starting KSM simulation...\n");

    ksm_init();
    ksm_run_thread();

    /* Let KSM run for a while */
    sleep(10);

    ksm_show_stats();
    ksm_stop_thread();

    printf("KSM simulation completed\n");
    return 0;
}
