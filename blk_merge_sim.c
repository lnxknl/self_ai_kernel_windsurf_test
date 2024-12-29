/*
 * Block I/O Request Merging Simulation
 * 
 * This program simulates the block I/O request merging subsystem,
 * including request merging, elevator operations, and queue management.
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
#define BLK_MAX_QUEUE_SIZE   128
#define BLK_MAX_SEGMENTS     128
#define BLK_SECTOR_SIZE      512
#define BLK_SEGMENT_SIZE     4096
#define BLK_MAX_SECTORS      256
#define BLK_MAX_MERGE_LEN    128
#define BLK_MAX_DEVICES      8

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

/* Merge Types */
#define ELEVATOR_NO_MERGE   0
#define ELEVATOR_BACK_MERGE 1
#define ELEVATOR_FRONT_MERGE 2

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
    struct bio *req_bio;    /* Associated bio */
    void    *req_private;   /* Private data */
    struct request *req_next; /* Next request */
    struct request *req_prev; /* Previous request */
};

/* Request Queue */
struct request_queue {
    struct request *queue_head;  /* Head of request queue */
    struct request *queue_tail;  /* Tail of request queue */
    uint32_t nr_requests;       /* Number of requests */
    uint32_t max_sectors;       /* Max sectors per request */
    uint32_t max_segments;      /* Max segments per request */
    uint32_t max_size;         /* Max queue size */
    pthread_mutex_t lock;       /* Queue lock */
    pthread_cond_t wait;        /* Wait condition */
    bool     merging_enabled;   /* Merging enabled flag */
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
static int blk_try_merge(struct request *req, struct request *next);
static int blk_attempt_merge(struct request_queue *q,
                           struct request *req,
                           struct request *next);
static void blk_merge_requests(struct request *req,
                             struct request *next);
static int blk_queue_merge_check(struct request_queue *q);

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

/* Request Operations */

static struct request *blk_alloc_request(struct request_queue *q) {
    struct request *req;
    
    if (!q)
        return NULL;
    
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

/* Queue Operations */

static struct request_queue *blk_alloc_queue(void) {
    struct request_queue *q;
    
    q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->wait, NULL);
    
    q->max_sectors = BLK_MAX_SECTORS;
    q->max_segments = BLK_MAX_SEGMENTS;
    q->max_size = BLK_MAX_QUEUE_SIZE;
    q->merging_enabled = true;
    
    return q;
}

static void blk_free_queue(struct request_queue *q) {
    struct request *req, *next;
    
    if (!q)
        return;
    
    pthread_mutex_lock(&q->lock);
    req = q->queue_head;
    while (req) {
        next = req->req_next;
        blk_free_request(req);
        req = next;
    }
    pthread_mutex_unlock(&q->lock);
    
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->wait);
    free(q);
}

/* Merge Operations */

static bool blk_can_merge_requests(struct request *req1,
                                 struct request *req2) {
    /* Check if requests can be merged */
    if (req1->req_op != req2->req_op)
        return false;
    
    if ((req1->req_flags & REQ_NOMERGE) ||
        (req2->req_flags & REQ_NOMERGE))
        return false;
    
    if (req1->req_op != REQ_OP_READ &&
        req1->req_op != REQ_OP_WRITE)
        return false;
    
    return true;
}

static int blk_try_merge(struct request *req,
                        struct request *next) {
    uint64_t req_end = req->req_sector + req->req_nr_sectors;
    
    if (!blk_can_merge_requests(req, next))
        return ELEVATOR_NO_MERGE;
    
    if (req_end == next->req_sector)
        return ELEVATOR_BACK_MERGE;
    
    if (req->req_sector == next->req_sector + next->req_nr_sectors)
        return ELEVATOR_FRONT_MERGE;
    
    return ELEVATOR_NO_MERGE;
}

static void blk_merge_requests(struct request *req,
                             struct request *next) {
    struct bio *bio;
    
    /* Merge bio lists */
    if (req->req_bio) {
        bio = req->req_bio;
        while (bio->bi_next)
            bio = bio->bi_next;
        bio->bi_next = next->req_bio;
    } else {
        req->req_bio = next->req_bio;
    }
    next->req_bio = NULL;
    
    /* Update sectors */
    req->req_nr_sectors += next->req_nr_sectors;
    
    /* Preserve flags */
    req->req_flags |= (next->req_flags & REQ_SYNC);
}

static int blk_attempt_merge(struct request_queue *q,
                           struct request *req,
                           struct request *next) {
    int ret;
    
    if (!q->merging_enabled)
        return ELEVATOR_NO_MERGE;
    
    ret = blk_try_merge(req, next);
    if (ret != ELEVATOR_NO_MERGE) {
        /* Remove next from queue */
        if (next->req_next)
            next->req_next->req_prev = next->req_prev;
        else
            q->queue_tail = next->req_prev;
        
        if (next->req_prev)
            next->req_prev->req_next = next->req_next;
        else
            q->queue_head = next->req_next;
        
        /* Merge requests */
        blk_merge_requests(req, next);
        
        /* Free merged request */
        blk_free_request(next);
        
        q->nr_requests--;
    }
    
    return ret;
}

static int blk_queue_merge_check(struct request_queue *q) {
    struct request *req, *next;
    int merges = 0;
    
    if (!q || !q->merging_enabled)
        return 0;
    
    pthread_mutex_lock(&q->lock);
    
    req = q->queue_head;
    while (req && req->req_next) {
        next = req->req_next;
        if (blk_attempt_merge(q, req, next) != ELEVATOR_NO_MERGE)
            merges++;
        else
            req = next;
    }
    
    pthread_mutex_unlock(&q->lock);
    return merges;
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

/* Example Usage and Testing */

static void run_merge_test(void) {
    struct block_device *bdev;
    struct request *req;
    char *buffer;
    int i, merges;
    
    printf("Block I/O Request Merging Simulation\n");
    printf("===================================\n\n");
    
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
    
    /* Allocate test buffer */
    buffer = malloc(BLK_SEGMENT_SIZE);
    if (!buffer) {
        printf("Failed to allocate buffer\n");
        goto out;
    }
    
    printf("Submitting requests for merge testing:\n");
    
    /* Submit sequential write requests */
    for (i = 0; i < 10; i++) {
        req = blk_alloc_request(bdev->queue);
        if (!req) {
            printf("Failed to allocate request\n");
            continue;
        }
        
        /* Set up request */
        req->req_op = REQ_OP_WRITE;
        req->req_sector = i * BLK_MAX_SECTORS;
        req->req_nr_sectors = BLK_MAX_SECTORS;
        
        /* Add to queue */
        pthread_mutex_lock(&bdev->queue->lock);
        if (!bdev->queue->queue_head)
            bdev->queue->queue_head = req;
        else {
            bdev->queue->queue_tail->req_next = req;
            req->req_prev = bdev->queue->queue_tail;
        }
        bdev->queue->queue_tail = req;
        bdev->queue->nr_requests++;
        pthread_mutex_unlock(&bdev->queue->lock);
        
        printf("Submitted request %d: sector=%lu, count=%u\n",
               i, req->req_sector, req->req_nr_sectors);
    }
    
    /* Try to merge requests */
    merges = blk_queue_merge_check(bdev->queue);
    printf("\nPerformed %d request merges\n", merges);
    
    /* Print final queue state */
    printf("\nFinal queue state:\n");
    pthread_mutex_lock(&bdev->queue->lock);
    req = bdev->queue->queue_head;
    i = 0;
    while (req) {
        printf("Request %d: sector=%lu, count=%u\n",
               i++, req->req_sector, req->req_nr_sectors);
        req = req->req_next;
    }
    pthread_mutex_unlock(&bdev->queue->lock);
    
    free(buffer);
    
out:
    blk_free_device(bdev);
}

int main(void) {
    run_merge_test();
    return 0;
}
