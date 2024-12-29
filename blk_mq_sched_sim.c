/*
 * Block Multi-Queue I/O Scheduler Simulation
 * 
 * This program simulates the block multi-queue I/O scheduler subsystem,
 * including hardware and software queue management, request dispatching,
 * and scheduling algorithms.
 *
 * Author: Cascade AI
 * Date: 2024-12-29
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <sys/types.h>

/* Configuration Constants */
#define BLK_MQ_MAX_QUEUES    16
#define BLK_MQ_QUEUE_DEPTH   128
#define BLK_MQ_TAG_DEPTH     256
#define BLK_MQ_MAX_SEGMENTS  128
#define BLK_SECTOR_SIZE      512
#define BLK_SEGMENT_SIZE     4096
#define BLK_MAX_SECTORS      256
#define BLK_MAX_HW_SECTORS   255

/* Request Types */
#define REQ_OP_READ          0
#define REQ_OP_WRITE         1
#define REQ_OP_FLUSH         2
#define REQ_OP_DISCARD       3
#define REQ_OP_SECURE_ERASE  4
#define REQ_OP_WRITE_SAME    5
#define REQ_OP_WRITE_ZEROES  6

/* Request Flags */
#define REQ_SYNC            (1 << 0)
#define REQ_META            (1 << 1)
#define REQ_PRIO           (1 << 2)
#define REQ_NOMERGE        (1 << 3)
#define REQ_IDLE           (1 << 4)
#define REQ_FUA            (1 << 5)
#define REQ_RAHEAD         (1 << 6)
#define REQ_BACKGROUND     (1 << 7)

/* Error Codes */
#define BLK_STS_OK          0
#define BLK_STS_AGAIN       1
#define BLK_STS_RESOURCE    2
#define BLK_STS_IOERR      3
#define BLK_STS_TIMEOUT    4

/* Scheduler Types */
#define SCHED_NONE          0
#define SCHED_NOOP         1
#define SCHED_DEADLINE     2
#define SCHED_CFQ          3

/* Bio Structure */
struct bio {
    uint64_t bi_sector;     /* Device sector */
    uint32_t bi_size;       /* Remaining I/O size */
    uint32_t bi_max_vecs;   /* Max bio segments */
    uint16_t bi_vcnt;       /* Current bio segments */
    uint16_t bi_idx;        /* Current index into bio */
    uint16_t bi_flags;      /* Bio status flags */
    uint16_t bi_status;     /* Bio completion status */
    struct bio_vec *bi_io_vec; /* Bio segment array */
    struct bio *bi_next;    /* Next bio in chain */
};

/* Bio Vector */
struct bio_vec {
    void    *bv_page;      /* Page buffer */
    uint32_t bv_len;       /* Length of segment */
    uint32_t bv_offset;    /* Offset within page */
};

/* Request Structure */
struct request {
    uint64_t req_sector;    /* Starting sector */
    uint32_t req_nr_sectors;/* Number of sectors */
    uint32_t req_flags;     /* Request flags */
    uint16_t req_op;        /* Request operation */
    uint16_t req_status;    /* Request status */
    uint16_t req_cpu;       /* CPU that submitted request */
    uint16_t req_queue_num; /* Hardware queue number */
    uint32_t req_tag;       /* Request tag */
    struct bio *req_bio;    /* Associated bio */
    void    *req_private;   /* Private data */
    struct request *req_next; /* Next request */
    struct request *req_prev; /* Previous request */
};

/* Hardware Queue */
struct blk_mq_hw_ctx {
    uint32_t queue_num;     /* Hardware queue number */
    uint32_t nr_active;     /* Number of active requests */
    uint32_t tags_depth;    /* Tag map depth */
    uint32_t *tag_map;      /* Tag allocation map */
    struct request **rq_map; /* Request map */
    pthread_mutex_t lock;    /* Queue lock */
    pthread_cond_t wait;     /* Wait condition */
    bool     stopped;        /* Queue stopped flag */
};

/* Software Queue */
struct blk_mq_ctx {
    uint32_t cpu;           /* CPU number */
    uint32_t index;         /* Queue index */
    uint32_t nr_queued;     /* Number of queued requests */
    struct request *rq_list; /* Request list */
    pthread_mutex_t lock;    /* Queue lock */
};

/* Scheduler Data */
struct blk_mq_sched {
    uint32_t sched_type;    /* Scheduler type */
    uint32_t nr_queues;     /* Number of queues */
    struct blk_mq_hw_ctx **hw_queues; /* Hardware queues */
    struct blk_mq_ctx **sw_queues;    /* Software queues */
    pthread_mutex_t lock;    /* Scheduler lock */
};

/* Block Device Structure */
struct block_device {
    char     name[32];      /* Device name */
    uint64_t capacity;      /* Device capacity in sectors */
    uint32_t max_sectors;   /* Max sectors per request */
    uint32_t max_segments;  /* Max segments per request */
    void    *private_data;  /* Private device data */
    struct blk_mq_sched *sched; /* Scheduler */
    pthread_mutex_t lock;    /* Device lock */
};

/* Function Prototypes */
static struct bio *bio_alloc(uint32_t max_vecs);
static void bio_free(struct bio *bio);
static struct request *blk_mq_alloc_request(struct blk_mq_sched *sched);
static void blk_mq_free_request(struct request *req);
static struct blk_mq_hw_ctx *blk_mq_alloc_hw_queue(uint32_t queue_num);
static void blk_mq_free_hw_queue(struct blk_mq_hw_ctx *hctx);
static struct blk_mq_ctx *blk_mq_alloc_sw_queue(uint32_t cpu);
static void blk_mq_free_sw_queue(struct blk_mq_ctx *ctx);

/* Bio Operations */

static struct bio *bio_alloc(uint32_t max_vecs) {
    struct bio *bio;
    
    bio = calloc(1, sizeof(*bio));
    if (!bio)
        return NULL;
    
    bio->bi_io_vec = calloc(max_vecs, sizeof(struct bio_vec));
    if (!bio->bi_io_vec) {
        free(bio);
        return NULL;
    }
    
    bio->bi_max_vecs = max_vecs;
    return bio;
}

static void bio_free(struct bio *bio) {
    if (!bio)
        return;
    
    free(bio->bi_io_vec);
    free(bio);
}

/* Request Operations */

static struct request *blk_mq_alloc_request(struct blk_mq_sched *sched) {
    struct request *req;
    
    if (!sched)
        return NULL;
    
    req = calloc(1, sizeof(*req));
    if (!req)
        return NULL;
    
    return req;
}

static void blk_mq_free_request(struct request *req) {
    if (!req)
        return;
    
    if (req->req_bio)
        bio_free(req->req_bio);
    
    free(req);
}

/* Hardware Queue Operations */

static struct blk_mq_hw_ctx *blk_mq_alloc_hw_queue(uint32_t queue_num) {
    struct blk_mq_hw_ctx *hctx;
    
    hctx = calloc(1, sizeof(*hctx));
    if (!hctx)
        return NULL;
    
    hctx->queue_num = queue_num;
    hctx->tags_depth = BLK_MQ_TAG_DEPTH;
    
    hctx->tag_map = calloc(BLK_MQ_TAG_DEPTH, sizeof(uint32_t));
    if (!hctx->tag_map) {
        free(hctx);
        return NULL;
    }
    
    hctx->rq_map = calloc(BLK_MQ_TAG_DEPTH, sizeof(struct request *));
    if (!hctx->rq_map) {
        free(hctx->tag_map);
        free(hctx);
        return NULL;
    }
    
    pthread_mutex_init(&hctx->lock, NULL);
    pthread_cond_init(&hctx->wait, NULL);
    
    return hctx;
}

static void blk_mq_free_hw_queue(struct blk_mq_hw_ctx *hctx) {
    if (!hctx)
        return;
    
    pthread_mutex_destroy(&hctx->lock);
    pthread_cond_destroy(&hctx->wait);
    free(hctx->rq_map);
    free(hctx->tag_map);
    free(hctx);
}

/* Software Queue Operations */

static struct blk_mq_ctx *blk_mq_alloc_sw_queue(uint32_t cpu) {
    struct blk_mq_ctx *ctx;
    
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    
    ctx->cpu = cpu;
    pthread_mutex_init(&ctx->lock, NULL);
    
    return ctx;
}

static void blk_mq_free_sw_queue(struct blk_mq_ctx *ctx) {
    struct request *req, *next;
    
    if (!ctx)
        return;
    
    req = ctx->rq_list;
    while (req) {
        next = req->req_next;
        blk_mq_free_request(req);
        req = next;
    }
    
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

/* Scheduler Operations */

static struct blk_mq_sched *blk_mq_alloc_scheduler(uint32_t nr_queues) {
    struct blk_mq_sched *sched;
    uint32_t i;
    
    sched = calloc(1, sizeof(*sched));
    if (!sched)
        return NULL;
    
    sched->nr_queues = nr_queues;
    sched->sched_type = SCHED_NOOP;
    
    sched->hw_queues = calloc(nr_queues, sizeof(struct blk_mq_hw_ctx *));
    if (!sched->hw_queues) {
        free(sched);
        return NULL;
    }
    
    sched->sw_queues = calloc(nr_queues, sizeof(struct blk_mq_ctx *));
    if (!sched->sw_queues) {
        free(sched->hw_queues);
        free(sched);
        return NULL;
    }
    
    for (i = 0; i < nr_queues; i++) {
        sched->hw_queues[i] = blk_mq_alloc_hw_queue(i);
        if (!sched->hw_queues[i])
            goto cleanup;
        
        sched->sw_queues[i] = blk_mq_alloc_sw_queue(i);
        if (!sched->sw_queues[i])
            goto cleanup;
    }
    
    pthread_mutex_init(&sched->lock, NULL);
    return sched;
    
cleanup:
    while (i--) {
        if (sched->hw_queues[i])
            blk_mq_free_hw_queue(sched->hw_queues[i]);
        if (sched->sw_queues[i])
            blk_mq_free_sw_queue(sched->sw_queues[i]);
    }
    free(sched->sw_queues);
    free(sched->hw_queues);
    free(sched);
    return NULL;
}

static void blk_mq_free_scheduler(struct blk_mq_sched *sched) {
    uint32_t i;
    
    if (!sched)
        return;
    
    for (i = 0; i < sched->nr_queues; i++) {
        if (sched->hw_queues[i])
            blk_mq_free_hw_queue(sched->hw_queues[i]);
        if (sched->sw_queues[i])
            blk_mq_free_sw_queue(sched->sw_queues[i]);
    }
    
    pthread_mutex_destroy(&sched->lock);
    free(sched->sw_queues);
    free(sched->hw_queues);
    free(sched);
}

/* Block Device Operations */

static struct block_device *blk_alloc_device(void) {
    struct block_device *bdev;
    
    bdev = calloc(1, sizeof(*bdev));
    if (!bdev)
        return NULL;
    
    bdev->sched = blk_mq_alloc_scheduler(BLK_MQ_MAX_QUEUES);
    if (!bdev->sched) {
        free(bdev);
        return NULL;
    }
    
    pthread_mutex_init(&bdev->lock, NULL);
    return bdev;
}

static void blk_free_device(struct block_device *bdev) {
    if (!bdev)
        return;
    
    if (bdev->sched)
        blk_mq_free_scheduler(bdev->sched);
    
    pthread_mutex_destroy(&bdev->lock);
    free(bdev);
}

/* Example Usage and Testing */

static void *queue_thread(void *data) {
    struct blk_mq_hw_ctx *hctx = data;
    struct request *req;
    uint32_t tag;
    
    while (!hctx->stopped) {
        pthread_mutex_lock(&hctx->lock);
        
        while (hctx->nr_active == 0 && !hctx->stopped)
            pthread_cond_wait(&hctx->wait, &hctx->lock);
        
        if (hctx->stopped) {
            pthread_mutex_unlock(&hctx->lock);
            break;
        }
        
        /* Process requests */
        for (tag = 0; tag < hctx->tags_depth; tag++) {
            if (hctx->tag_map[tag]) {
                req = hctx->rq_map[tag];
                if (req) {
                    printf("Processing request on queue %u: op=%u, sector=%lu\n",
                           hctx->queue_num, req->req_op, req->req_sector);
                    
                    /* Simulate I/O processing time */
                    usleep(1000);
                    
                    hctx->tag_map[tag] = 0;
                    hctx->rq_map[tag] = NULL;
                    hctx->nr_active--;
                    
                    blk_mq_free_request(req);
                }
            }
        }
        
        pthread_mutex_unlock(&hctx->lock);
        usleep(1000);
    }
    
    return NULL;
}

static void run_scheduler_test(void) {
    struct block_device *bdev;
    struct request *req;
    pthread_t *threads;
    uint32_t i;
    
    printf("Block Multi-Queue I/O Scheduler Simulation\n");
    printf("=========================================\n\n");
    
    /* Create block device */
    bdev = blk_alloc_device();
    if (!bdev) {
        printf("Failed to allocate block device\n");
        return;
    }
    
    strncpy(bdev->name, "simdev0", sizeof(bdev->name) - 1);
    bdev->capacity = 1024 * 1024; /* 1M sectors */
    bdev->max_sectors = BLK_MAX_SECTORS;
    bdev->max_segments = BLK_MAX_SEGMENTS;
    
    /* Start queue threads */
    threads = calloc(BLK_MQ_MAX_QUEUES, sizeof(pthread_t));
    if (!threads) {
        printf("Failed to allocate thread array\n");
        goto out;
    }
    
    for (i = 0; i < BLK_MQ_MAX_QUEUES; i++) {
        pthread_create(&threads[i], NULL, queue_thread,
                      bdev->sched->hw_queues[i]);
    }
    
    printf("Submitting requests to multiple queues:\n");
    
    /* Submit test requests */
    for (i = 0; i < 50; i++) {
        req = blk_mq_alloc_request(bdev->sched);
        if (!req) {
            printf("Failed to allocate request\n");
            continue;
        }
        
        /* Set up request */
        req->req_op = (i % 2) ? REQ_OP_WRITE : REQ_OP_READ;
        req->req_sector = i * BLK_MAX_SECTORS;
        req->req_nr_sectors = BLK_MAX_SECTORS;
        req->req_cpu = i % BLK_MQ_MAX_QUEUES;
        req->req_queue_num = i % BLK_MQ_MAX_QUEUES;
        
        /* Add to hardware queue */
        struct blk_mq_hw_ctx *hctx = bdev->sched->hw_queues[req->req_queue_num];
        
        pthread_mutex_lock(&hctx->lock);
        uint32_t tag;
        for (tag = 0; tag < hctx->tags_depth; tag++) {
            if (!hctx->tag_map[tag]) {
                hctx->tag_map[tag] = 1;
                hctx->rq_map[tag] = req;
                req->req_tag = tag;
                hctx->nr_active++;
                pthread_cond_signal(&hctx->wait);
                break;
            }
        }
        pthread_mutex_unlock(&hctx->lock);
        
        if (tag == hctx->tags_depth) {
            printf("No free tags for request %d\n", i);
            blk_mq_free_request(req);
            continue;
        }
        
        printf("Submitted request %d to queue %u\n", i, req->req_queue_num);
    }
    
    /* Wait for requests to complete */
    sleep(1);
    
    /* Stop queue threads */
    for (i = 0; i < BLK_MQ_MAX_QUEUES; i++) {
        struct blk_mq_hw_ctx *hctx = bdev->sched->hw_queues[i];
        pthread_mutex_lock(&hctx->lock);
        hctx->stopped = true;
        pthread_cond_signal(&hctx->wait);
        pthread_mutex_unlock(&hctx->lock);
    }
    
    for (i = 0; i < BLK_MQ_MAX_QUEUES; i++) {
        pthread_join(threads[i], NULL);
    }
    
    free(threads);
    
out:
    blk_free_device(bdev);
}

int main(void) {
    run_scheduler_test();
    return 0;
}
