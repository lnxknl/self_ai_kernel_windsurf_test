/*
 * Kernel Buffer (kbuf) Simulation
 * 
 * This program simulates the kernel buffer management system used by io_uring
 * in user space. It provides buffer pooling, selection, and management features
 * similar to the kernel implementation.
 *
 * Author: Cascade AI
 * Date: 2024-12-29
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Configuration Constants */
#define PAGE_SIZE               4096
#define MAX_BUFFER_ID          (1 << 16)  /* 16-bit buffer ID */
#define MAX_GROUP_ID           1024
#define BUFFER_ALLOC_BATCH     64
#define DEFAULT_BUFFER_SIZE    4096

/* Buffer Flags */
enum {
    BUF_FLAG_USED     = 1 << 0,  /* Buffer is in use */
    BUF_FLAG_MAPPED   = 1 << 1,  /* Buffer is memory mapped */
    BUF_FLAG_RING     = 1 << 2,  /* Buffer is part of a ring */
    BUF_FLAG_CACHE    = 1 << 3,  /* Buffer is cached */
};

/* Request Flags */
enum {
    REQ_F_BUFFER_SELECT = 1 << 0,  /* Buffer has been selected */
    REQ_F_BUFFER_RING   = 1 << 1,  /* Using buffer ring */
    REQ_F_PARTIAL_IO    = 1 << 2,  /* Partial I/O done */
};

/* Forward Declarations */
struct buffer_list;
struct buffer_ring;
struct buffer_group;
struct buffer_cache;
struct request;

/* Basic Buffer Structure */
struct buffer {
    struct buffer *next;      /* Next buffer in list */
    void *addr;              /* Buffer address */
    size_t len;              /* Buffer length */
    unsigned int bid;        /* Buffer ID */
    unsigned int bgid;       /* Buffer group ID */
    unsigned int flags;      /* Buffer flags */
    atomic_int refs;         /* Reference count */
};

/* Buffer List Structure */
struct buffer_list {
    struct buffer *head;
    struct buffer *tail;
    pthread_mutex_t lock;
    unsigned int count;
};

/* Buffer Ring Structure */
struct buffer_ring {
    void *ring_ptr;          /* Ring buffer memory */
    size_t ring_size;        /* Size of ring buffer */
    unsigned int head;       /* Producer index */
    unsigned int tail;       /* Consumer index */
    unsigned int mask;       /* Ring size mask */
    bool is_mapped;          /* Ring is memory mapped */
};

/* Buffer Group Structure */
struct buffer_group {
    unsigned int bgid;       /* Group ID */
    struct buffer_list free_list;    /* Free buffers */
    struct buffer_list used_list;    /* Used buffers */
    struct buffer_ring *ring;        /* Optional buffer ring */
    atomic_int ref_count;            /* Reference count */
    size_t buffer_size;              /* Size of each buffer */
    unsigned int flags;              /* Group flags */
};

/* Buffer Cache Structure */
struct buffer_cache {
    struct buffer_list cached;       /* Cached buffers */
    unsigned int max_cached;         /* Maximum cached buffers */
    pthread_mutex_t lock;
};

/* Request Structure */
struct request {
    struct buffer *buf;              /* Selected buffer */
    unsigned int flags;              /* Request flags */
    unsigned int buf_index;          /* Buffer index */
    struct buffer_group *bg;         /* Buffer group */
};

/* Context Structure */
struct context {
    struct buffer_group *groups[MAX_GROUP_ID];
    struct buffer_cache cache;
    pthread_mutex_t lock;
    unsigned int flags;
};

/* Helper Functions */

static void buffer_list_init(struct buffer_list *list) {
    list->head = list->tail = NULL;
    list->count = 0;
    pthread_mutex_init(&list->lock, NULL);
}

static void buffer_list_add(struct buffer_list *list, struct buffer *buf) {
    pthread_mutex_lock(&list->lock);
    
    if (!list->head) {
        list->head = list->tail = buf;
    } else {
        list->tail->next = buf;
        list->tail = buf;
    }
    buf->next = NULL;
    list->count++;
    
    pthread_mutex_unlock(&list->lock);
}

static struct buffer *buffer_list_remove(struct buffer_list *list) {
    struct buffer *buf = NULL;
    
    pthread_mutex_lock(&list->lock);
    
    if (list->head) {
        buf = list->head;
        list->head = buf->next;
        if (!list->head)
            list->tail = NULL;
        buf->next = NULL;
        list->count--;
    }
    
    pthread_mutex_unlock(&list->lock);
    return buf;
}

/* Buffer Management Functions */

static struct buffer *buffer_alloc(size_t size) {
    struct buffer *buf;
    
    buf = calloc(1, sizeof(*buf));
    if (!buf)
        return NULL;
    
    buf->addr = aligned_alloc(PAGE_SIZE, size);
    if (!buf->addr) {
        free(buf);
        return NULL;
    }
    
    buf->len = size;
    atomic_init(&buf->refs, 1);
    
    return buf;
}

static void buffer_free(struct buffer *buf) {
    if (!buf)
        return;
    
    if (buf->flags & BUF_FLAG_MAPPED)
        munmap(buf->addr, buf->len);
    else
        free(buf->addr);
    
    free(buf);
}

static struct buffer_group *buffer_group_create(unsigned int bgid, size_t buffer_size) {
    struct buffer_group *bg;
    
    bg = calloc(1, sizeof(*bg));
    if (!bg)
        return NULL;
    
    bg->bgid = bgid;
    bg->buffer_size = buffer_size;
    atomic_init(&bg->ref_count, 1);
    
    buffer_list_init(&bg->free_list);
    buffer_list_init(&bg->used_list);
    
    return bg;
}

static void buffer_group_destroy(struct buffer_group *bg) {
    struct buffer *buf;
    
    if (!bg)
        return;
    
    /* Free all buffers in free list */
    while ((buf = buffer_list_remove(&bg->free_list)))
        buffer_free(buf);
    
    /* Free all buffers in used list */
    while ((buf = buffer_list_remove(&bg->used_list)))
        buffer_free(buf);
    
    /* Free ring if present */
    if (bg->ring) {
        if (bg->ring->is_mapped)
            munmap(bg->ring->ring_ptr, bg->ring->ring_size);
        else
            free(bg->ring->ring_ptr);
        free(bg->ring);
    }
    
    pthread_mutex_destroy(&bg->free_list.lock);
    pthread_mutex_destroy(&bg->used_list.lock);
    free(bg);
}

/* Context Management */

struct context *context_create(void) {
    struct context *ctx;
    
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    
    pthread_mutex_init(&ctx->lock, NULL);
    pthread_mutex_init(&ctx->cache.lock, NULL);
    buffer_list_init(&ctx->cache.cached);
    ctx->cache.max_cached = BUFFER_ALLOC_BATCH;
    
    return ctx;
}

void context_destroy(struct context *ctx) {
    int i;
    struct buffer *buf;
    
    if (!ctx)
        return;
    
    /* Destroy all buffer groups */
    for (i = 0; i < MAX_GROUP_ID; i++) {
        if (ctx->groups[i])
            buffer_group_destroy(ctx->groups[i]);
    }
    
    /* Free cached buffers */
    while ((buf = buffer_list_remove(&ctx->cache.cached)))
        buffer_free(buf);
    
    pthread_mutex_destroy(&ctx->cache.lock);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

/* Buffer Selection Functions */

static struct buffer *buffer_select_from_group(struct buffer_group *bg) {
    struct buffer *buf;
    
    buf = buffer_list_remove(&bg->free_list);
    if (buf) {
        buffer_list_add(&bg->used_list, buf);
        buf->flags |= BUF_FLAG_USED;
    }
    
    return buf;
}

static struct buffer *buffer_select_from_ring(struct buffer_group *bg) {
    struct buffer_ring *ring = bg->ring;
    struct buffer *buf = NULL;
    
    if (!ring || ring->head == ring->tail)
        return NULL;
    
    /* Get buffer from ring */
    unsigned int idx = ring->head & ring->mask;
    void *ring_buf = ring->ring_ptr + (idx * sizeof(struct buffer));
    
    /* Create new buffer structure */
    buf = buffer_alloc(bg->buffer_size);
    if (buf) {
        memcpy(buf->addr, ring_buf, bg->buffer_size);
        buf->flags |= BUF_FLAG_RING;
        ring->head++;
    }
    
    return buf;
}

/* Public API Functions */

int provide_buffers(struct context *ctx, unsigned int bgid, 
                   void *addr, size_t len, unsigned int nbufs) {
    struct buffer_group *bg;
    struct buffer *buf;
    int i;
    
    if (bgid >= MAX_GROUP_ID || !nbufs || !len)
        return -EINVAL;
    
    pthread_mutex_lock(&ctx->lock);
    
    /* Create or get buffer group */
    bg = ctx->groups[bgid];
    if (!bg) {
        bg = buffer_group_create(bgid, len);
        if (!bg) {
            pthread_mutex_unlock(&ctx->lock);
            return -ENOMEM;
        }
        ctx->groups[bgid] = bg;
    }
    
    /* Add buffers to group */
    for (i = 0; i < nbufs; i++) {
        buf = buffer_alloc(len);
        if (!buf)
            break;
        
        buf->bgid = bgid;
        buf->bid = i;
        
        if (addr) {
            memcpy(buf->addr, addr + (i * len), len);
            buf->flags |= BUF_FLAG_MAPPED;
        }
        
        buffer_list_add(&bg->free_list, buf);
    }
    
    pthread_mutex_unlock(&ctx->lock);
    return i;
}

int remove_buffers(struct context *ctx, unsigned int bgid, unsigned int nbufs) {
    struct buffer_group *bg;
    struct buffer *buf;
    int removed = 0;
    
    if (bgid >= MAX_GROUP_ID)
        return -EINVAL;
    
    pthread_mutex_lock(&ctx->lock);
    
    bg = ctx->groups[bgid];
    if (!bg) {
        pthread_mutex_unlock(&ctx->lock);
        return -ENOENT;
    }
    
    while (removed < nbufs && (buf = buffer_list_remove(&bg->free_list))) {
        buffer_free(buf);
        removed++;
    }
    
    if (bg->free_list.count == 0 && bg->used_list.count == 0) {
        ctx->groups[bgid] = NULL;
        buffer_group_destroy(bg);
    }
    
    pthread_mutex_unlock(&ctx->lock);
    return removed;
}

struct buffer *select_buffer(struct context *ctx, struct request *req) {
    struct buffer_group *bg;
    struct buffer *buf = NULL;
    
    if (!req || req->flags & REQ_F_BUFFER_SELECT)
        return NULL;
    
    pthread_mutex_lock(&ctx->lock);
    
    bg = ctx->groups[req->buf_index];
    if (bg) {
        if (bg->ring)
            buf = buffer_select_from_ring(bg);
        else
            buf = buffer_select_from_group(bg);
        
        if (buf) {
            req->flags |= REQ_F_BUFFER_SELECT;
            req->buf = buf;
            req->bg = bg;
        }
    }
    
    pthread_mutex_unlock(&ctx->lock);
    return buf;
}

void release_buffer(struct context *ctx, struct request *req) {
    struct buffer *buf;
    
    if (!req || !(req->flags & REQ_F_BUFFER_SELECT))
        return;
    
    buf = req->buf;
    if (!buf)
        return;
    
    pthread_mutex_lock(&ctx->lock);
    
    if (req->flags & REQ_F_BUFFER_RING) {
        /* Return to ring */
        buffer_free(buf);
    } else {
        /* Return to free list or cache */
        if (ctx->cache.cached.count < ctx->cache.max_cached) {
            buf->flags &= ~BUF_FLAG_USED;
            buffer_list_add(&ctx->cache.cached, buf);
        } else {
            buffer_list_remove(&req->bg->used_list);
            buffer_list_add(&req->bg->free_list, buf);
        }
    }
    
    req->flags &= ~REQ_F_BUFFER_SELECT;
    req->buf = NULL;
    
    pthread_mutex_unlock(&ctx->lock);
}

/* Example Usage */

void example_buffer_operations(void) {
    struct context *ctx;
    struct request req = {0};
    struct buffer *buf;
    int ret;
    
    printf("Buffer Management Simulation\n");
    printf("===========================\n\n");
    
    /* Create context */
    ctx = context_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return;
    }
    
    /* Provide some buffers */
    printf("Providing buffers to group 1...\n");
    ret = provide_buffers(ctx, 1, NULL, 4096, 10);
    printf("Provided %d buffers\n", ret);
    
    /* Select and use a buffer */
    req.buf_index = 1;
    printf("\nSelecting buffer from group 1...\n");
    buf = select_buffer(ctx, &req);
    if (buf) {
        printf("Selected buffer %p with ID %u\n", buf->addr, buf->bid);
        /* Simulate some work */
        memset(buf->addr, 0x42, buf->len);
        printf("Performed work with buffer\n");
    }
    
    /* Release buffer */
    printf("\nReleasing buffer...\n");
    release_buffer(ctx, &req);
    
    /* Remove buffers */
    printf("\nRemoving buffers from group 1...\n");
    ret = remove_buffers(ctx, 1, 10);
    printf("Removed %d buffers\n", ret);
    
    /* Cleanup */
    context_destroy(ctx);
}

int main(void) {
    example_buffer_operations();
    return 0;
}
