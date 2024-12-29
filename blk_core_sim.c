/*
 * Block I/O Core Simulation
 * 
 * This program simulates the core block I/O subsystem, including request queue
 * management, I/O scheduling, and block device operations.
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
#define BLK_MAX_QUEUE_SIZE  128
#define BLK_MAX_SEGMENTS    128
#define BLK_SECTOR_SIZE     512
#define BLK_SEGMENT_SIZE    4096
#define BLK_MAX_SECTORS     256
#define BLK_MAX_DEVICES     8
#define BLK_MAX_HW_SECTORS  255
#define BLK_BATCH_REQUESTS  16

/* Request Types */
#define REQ_OP_READ         0
#define REQ_OP_WRITE        1
#define REQ_OP_FLUSH        2
#define REQ_OP_DISCARD      3
#define REQ_OP_SECURE_ERASE 4
#define REQ_OP_ZONE_RESET   5
#define REQ_OP_WRITE_SAME   6
#define REQ_OP_ZONE_APPEND  7

/* Request Flags */
#define REQ_SYNC           (1 << 0)
#define REQ_META           (1 << 1)
#define REQ_PRIO          (1 << 2)
#define REQ_NOMERGE       (1 << 3)
#define REQ_IDLE          (1 << 4)
#define REQ_FUA           (1 << 5)
#define REQ_RAHEAD        (1 << 6)
#define REQ_BACKGROUND    (1 << 7)

/* Error Codes */
#define BLK_STS_OK          0
#define BLK_STS_TIMEOUT     1
#define BLK_STS_RESOURCE    2
#define BLK_STS_IOERR      3
#define BLK_STS_MEDIUM      4
#define BLK_STS_NOTSUPP     5

/* Bio Structure (Block I/O) */
struct bio {
    uint64_t bi_sector;     /* Device sector */
    uint32_t bi_size;       /* Remaining I/O size */
    uint32_t bi_max_vecs;   /* Max bio segments */
    uint16_t bi_vcnt;       /* Current bio segments */
    uint16_t bi_idx;        /* Current index into bio */
    uint16_t bi_flags;      /* Bio status flags */
    uint16_t bi_status;     /* Bio completion status */
    void    *bi_private;    /* Private data */
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
    struct bio *req_bio;    /* Associated bio */
    void    *req_private;   /* Private data */
    struct request *req_next; /* Next request */
};

/* Request Queue */
struct request_queue {
    struct request *queue_head;  /* Head of request queue */
    struct request *queue_tail;  /* Tail of request queue */
    uint32_t queue_count;       /* Number of requests */
    uint32_t queue_max_sectors; /* Max sectors per request */
    uint32_t queue_max_segments;/* Max segments per request */
    uint32_t queue_max_size;    /* Max queue size */
    pthread_mutex_t queue_lock;  /* Queue lock */
    pthread_cond_t queue_wait;   /* Wait queue */
    bool queue_running;          /* Queue running flag */
};

/* Block Device Structure */
struct block_device {
    char     name[32];          /* Device name */
    uint64_t capacity;          /* Device capacity in sectors */
    uint32_t max_sectors;       /* Max sectors per request */
    uint32_t max_segments;      /* Max segments per request */
    void    *private_data;      /* Private device data */
    struct request_queue *queue;/* Request queue */
    pthread_mutex_t lock;       /* Device lock */
};

/* Function Prototypes */
static struct bio *bio_alloc(uint32_t max_vecs);
static void bio_free(struct bio *bio);
static struct request *blk_alloc_request(struct request_queue *q);
static void blk_free_request(struct request *req);
static int blk_queue_enter(struct request_queue *q);
static void blk_queue_exit(struct request_queue *q);
static void blk_run_queue(struct request_queue *q);

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

static int bio_add_page(struct bio *bio, void *page,
                       uint32_t len, uint32_t offset) {
    struct bio_vec *bv;
    
    if (bio->bi_vcnt >= bio->bi_max_vecs)
        return 0;
    
    bv = &bio->bi_io_vec[bio->bi_vcnt];
    bv->bv_page = page;
    bv->bv_len = len;
    bv->bv_offset = offset;
    bio->bi_vcnt++;
    bio->bi_size += len;
    
    return len;
}

/* Request Queue Operations */

static struct request_queue *blk_alloc_queue(void) {
    struct request_queue *q;
    
    q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    
    pthread_mutex_init(&q->queue_lock, NULL);
    pthread_cond_init(&q->queue_wait, NULL);
    
    q->queue_max_sectors = BLK_MAX_SECTORS;
    q->queue_max_segments = BLK_MAX_SEGMENTS;
    q->queue_max_size = BLK_MAX_QUEUE_SIZE;
    
    return q;
}

static void blk_free_queue(struct request_queue *q) {
    struct request *req, *next;
    
    if (!q)
        return;
    
    /* Free all pending requests */
    req = q->queue_head;
    while (req) {
        next = req->req_next;
        blk_free_request(req);
        req = next;
    }
    
    pthread_mutex_destroy(&q->queue_lock);
    pthread_cond_destroy(&q->queue_wait);
    free(q);
}

static struct request *blk_alloc_request(struct request_queue *q) {
    struct request *req;
    
    req = calloc(1, sizeof(*req));
    if (!req)
        return NULL;
    
    return req;
}

static void blk_free_request(struct request *req) {
    if (!req)
        return;
    
    if (req->req_bio)
        bio_free(req->req_bio);
    
    free(req);
}

static int blk_queue_enter(struct request_queue *q) {
    int ret = 0;
    
    pthread_mutex_lock(&q->queue_lock);
    
    while (q->queue_count >= q->queue_max_size && q->queue_running) {
        ret = pthread_cond_wait(&q->queue_wait, &q->queue_lock);
        if (ret)
            break;
    }
    
    if (!ret && q->queue_running)
        q->queue_count++;
    else
        ret = -EBUSY;
    
    pthread_mutex_unlock(&q->queue_lock);
    return ret;
}

static void blk_queue_exit(struct request_queue *q) {
    pthread_mutex_lock(&q->queue_lock);
    q->queue_count--;
    pthread_cond_signal(&q->queue_wait);
    pthread_mutex_unlock(&q->queue_lock);
}

static void blk_add_request(struct request_queue *q, struct request *req) {
    pthread_mutex_lock(&q->queue_lock);
    
    if (!q->queue_head)
        q->queue_head = req;
    else
        q->queue_tail->req_next = req;
    
    q->queue_tail = req;
    req->req_next = NULL;
    
    pthread_mutex_unlock(&q->queue_lock);
}

static struct request *blk_fetch_request(struct request_queue *q) {
    struct request *req = NULL;
    
    pthread_mutex_lock(&q->queue_lock);
    
    if (q->queue_head) {
        req = q->queue_head;
        q->queue_head = req->req_next;
        if (!q->queue_head)
            q->queue_tail = NULL;
        req->req_next = NULL;
    }
    
    pthread_mutex_unlock(&q->queue_lock);
    return req;
}

/* Block Device Operations */

static struct block_device *blk_alloc_device(void) {
    struct block_device *bdev;
    
    bdev = calloc(1, sizeof(*bdev));
    if (!bdev)
        return NULL;
    
    bdev->queue = blk_alloc_queue();
    if (!bdev->queue) {
        free(bdev);
        return NULL;
    }
    
    pthread_mutex_init(&bdev->lock, NULL);
    return bdev;
}

static void blk_free_device(struct block_device *bdev) {
    if (!bdev)
        return;
    
    if (bdev->queue)
        blk_free_queue(bdev->queue);
    
    pthread_mutex_destroy(&bdev->lock);
    free(bdev);
}

/* Request Processing */

static void blk_process_request(struct request *req) {
    struct bio *bio = req->req_bio;
    uint32_t i;
    
    printf("Processing request: op=%u, sector=%lu, count=%u\n",
           req->req_op, req->req_sector, req->req_nr_sectors);
    
    if (bio) {
        printf("  Bio segments:\n");
        for (i = 0; i < bio->bi_vcnt; i++) {
            struct bio_vec *bv = &bio->bi_io_vec[i];
            printf("    segment[%u]: len=%u, offset=%u\n",
                   i, bv->bv_len, bv->bv_offset);
        }
    }
    
    /* Simulate I/O processing time */
    usleep(1000);
    
    req->req_status = BLK_STS_OK;
}

static void *blk_queue_thread(void *data) {
    struct request_queue *q = data;
    struct request *req;
    
    while (q->queue_running) {
        req = blk_fetch_request(q);
        if (req) {
            blk_process_request(req);
            blk_free_request(req);
            blk_queue_exit(q);
        } else {
            usleep(1000);
        }
    }
    
    return NULL;
}

/* Example Usage and Testing */

static void run_block_test(void) {
    struct block_device *bdev;
    struct request *req;
    struct bio *bio;
    pthread_t thread;
    char *buffer;
    int i, ret;
    
    printf("Block I/O Core Simulation\n");
    printf("=========================\n\n");
    
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
    
    /* Start queue thread */
    bdev->queue->queue_running = true;
    pthread_create(&thread, NULL, blk_queue_thread, bdev->queue);
    
    printf("Submitting I/O requests:\n");
    
    /* Allocate test buffer */
    buffer = malloc(BLK_SEGMENT_SIZE);
    if (!buffer) {
        printf("Failed to allocate buffer\n");
        goto out;
    }
    
    /* Submit some test requests */
    for (i = 0; i < 5; i++) {
        /* Allocate request */
        req = blk_alloc_request(bdev->queue);
        if (!req) {
            printf("Failed to allocate request\n");
            continue;
        }
        
        /* Allocate bio */
        bio = bio_alloc(4);
        if (!bio) {
            printf("Failed to allocate bio\n");
            blk_free_request(req);
            continue;
        }
        
        /* Set up request */
        req->req_op = (i % 2) ? REQ_OP_WRITE : REQ_OP_READ;
        req->req_sector = i * BLK_MAX_SECTORS;
        req->req_nr_sectors = BLK_MAX_SECTORS;
        req->req_bio = bio;
        
        /* Add some segments to bio */
        bio_add_page(bio, buffer, BLK_SEGMENT_SIZE, 0);
        bio_add_page(bio, buffer, BLK_SEGMENT_SIZE, 0);
        
        /* Submit request */
        ret = blk_queue_enter(bdev->queue);
        if (ret) {
            printf("Queue full, request %d dropped\n", i);
            blk_free_request(req);
            continue;
        }
        
        blk_add_request(bdev->queue, req);
        printf("Submitted request %d\n", i);
    }
    
    /* Wait for requests to complete */
    sleep(1);
    
    free(buffer);
    
out:
    /* Stop queue thread */
    bdev->queue->queue_running = false;
    pthread_join(thread, NULL);
    
    /* Cleanup */
    blk_free_device(bdev);
}

int main(void) {
    run_block_test();
    return 0;
}
