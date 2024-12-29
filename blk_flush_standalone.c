#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blk_flush_standalone.h"

#define FLUSH_PENDING_TIMEOUT 5 /* 5 seconds */

/* Helper function to create a new request */
static struct request *alloc_request(enum req_opf op, unsigned int flags,
                                   uint64_t sector, unsigned int nr_sectors,
                                   void *data) {
    struct request *rq = calloc(1, sizeof(*rq));
    if (!rq)
        return NULL;

    rq->op = op;
    rq->flags = flags;
    rq->sector = sector;
    rq->nr_sectors = nr_sectors;
    rq->data = data;
    rq->submit_time = time(NULL);
    rq->next = NULL;

    return rq;
}

/* Initialize block device */
struct block_device *blk_init_device(bool has_cache, bool has_fua) {
    struct block_device *bdev = calloc(1, sizeof(*bdev));
    if (!bdev)
        return NULL;

    bdev->data = calloc(MAX_BLOCKS, BLOCK_SIZE);
    if (!bdev->data) {
        free(bdev);
        return NULL;
    }

    if (has_cache) {
        bdev->cache = calloc(CACHE_SIZE, BLOCK_SIZE);
        if (!bdev->cache) {
            free(bdev->data);
            free(bdev);
            return NULL;
        }
    }

    bdev->has_cache = has_cache;
    bdev->has_fua = has_fua;
    bdev->cache_size = CACHE_SIZE;
    bdev->size = MAX_BLOCKS;

    return bdev;
}

/* Free block device */
void blk_free_device(struct block_device *bdev) {
    if (!bdev)
        return;
    free(bdev->data);
    free(bdev->cache);
    free(bdev);
}

/* Allocate flush queue */
struct flush_queue *blk_alloc_flush_queue(void) {
    struct flush_queue *fq = calloc(1, sizeof(*fq));
    if (!fq)
        return NULL;

    fq->last_flush = time(NULL);
    return fq;
}

/* Free flush queue */
void blk_free_flush_queue(struct flush_queue *fq) {
    if (!fq)
        return;

    /* Free all pending requests */
    struct request *rq = fq->pending_flush;
    while (rq) {
        struct request *next = rq->next;
        free(rq);
        rq = next;
    }

    rq = fq->data_reqs;
    while (rq) {
        struct request *next = rq->next;
        free(rq);
        rq = next;
    }

    free(fq);
}

/* Process flush operation */
static void process_flush(struct block_device *bdev) {
    if (bdev->has_cache) {
        /* Simulate flushing cache to disk */
        memcpy(bdev->data, bdev->cache, bdev->cache_size * BLOCK_SIZE);
        memset(bdev->cache, 0, bdev->cache_size * BLOCK_SIZE);
        printf("Cache flushed to disk\n");
    }
}

/* Process write operation */
static void process_write(struct block_device *bdev, struct request *rq) {
    uint8_t *src = rq->data;
    uint64_t offset = rq->sector * BLOCK_SIZE;
    size_t size = rq->nr_sectors * BLOCK_SIZE;

    if (bdev->has_cache && !(rq->flags & REQ_FUA)) {
        /* Write to cache */
        memcpy(bdev->cache + (offset % (bdev->cache_size * BLOCK_SIZE)),
               src, size);
        printf("Data written to cache at sector %lu\n", rq->sector);
    } else {
        /* Write directly to disk */
        memcpy(bdev->data + offset, src, size);
        printf("Data written directly to disk at sector %lu\n", rq->sector);
    }
}

/* Process read operation */
static void process_read(struct block_device *bdev, struct request *rq) {
    uint8_t *dst = rq->data;
    uint64_t offset = rq->sector * BLOCK_SIZE;
    size_t size = rq->nr_sectors * BLOCK_SIZE;

    if (bdev->has_cache) {
        /* Check cache first */
        memcpy(dst, bdev->cache + (offset % (bdev->cache_size * BLOCK_SIZE)),
               size);
        printf("Data read from cache at sector %lu\n", rq->sector);
    } else {
        /* Read from disk */
        memcpy(dst, bdev->data + offset, size);
        printf("Data read from disk at sector %lu\n", rq->sector);
    }
}

/* Submit a new request */
int blk_submit_request(struct block_device *bdev, struct flush_queue *fq,
                      enum req_opf op, unsigned int flags,
                      uint64_t sector, unsigned int nr_sectors,
                      void *data) {
    struct request *rq;

    if (!bdev || !fq)
        return -1;

    if (sector + nr_sectors > bdev->size)
        return -1;

    rq = alloc_request(op, flags, sector, nr_sectors, data);
    if (!rq)
        return -1;

    /* Handle flush request */
    if (op == REQ_OP_FLUSH || (flags & REQ_PREFLUSH)) {
        rq->next = fq->pending_flush;
        fq->pending_flush = rq;
        fq->nr_pending++;
        printf("Flush request queued\n");
    } else {
        /* Add to data requests queue */
        rq->next = fq->data_reqs;
        fq->data_reqs = rq;
        printf("%s request queued for sector %lu\n",
               op == REQ_OP_READ ? "Read" : "Write", sector);
    }

    return 0;
}

/* Process the flush queue */
void blk_process_flush_queue(struct block_device *bdev, struct flush_queue *fq) {
    time_t now;
    struct request *rq, *next;

    if (!bdev || !fq)
        return;

    now = time(NULL);

    /* Process pending flushes */
    if (fq->pending_flush &&
        (difftime(now, fq->last_flush) >= FLUSH_PENDING_TIMEOUT)) {
        process_flush(bdev);
        fq->last_flush = now;

        /* Free processed flush requests */
        rq = fq->pending_flush;
        while (rq) {
            next = rq->next;
            free(rq);
            rq = next;
            fq->nr_pending--;
        }
        fq->pending_flush = NULL;
    }

    /* Process data requests */
    rq = fq->data_reqs;
    fq->data_reqs = NULL;

    while (rq) {
        next = rq->next;

        switch (rq->op) {
        case REQ_OP_READ:
            process_read(bdev, rq);
            break;
        case REQ_OP_WRITE:
            process_write(bdev, rq);
            break;
        default:
            printf("Unknown request type\n");
            break;
        }

        free(rq);
        rq = next;
    }
}

/* Print device and queue status */
void blk_print_device_status(struct block_device *bdev, struct flush_queue *fq) {
    if (!bdev || !fq)
        return;

    printf("\nBlock Device Status:\n");
    printf("------------------\n");
    printf("Device size: %lu blocks\n", bdev->size);
    printf("Has cache: %s\n", bdev->has_cache ? "Yes" : "No");
    printf("Supports FUA: %s\n", bdev->has_fua ? "Yes" : "No");
    if (bdev->has_cache)
        printf("Cache size: %u blocks\n", bdev->cache_size);

    printf("\nFlush Queue Status:\n");
    printf("------------------\n");
    printf("Pending flush requests: %u\n", fq->nr_pending);
    printf("Time since last flush: %.0f seconds\n",
           difftime(time(NULL), fq->last_flush));
}

/* Example usage */
int main(void) {
    struct block_device *bdev;
    struct flush_queue *fq;
    uint8_t write_data[BLOCK_SIZE] = "Hello, Block Device!";
    uint8_t read_data[BLOCK_SIZE];

    /* Initialize device with cache but no FUA support */
    bdev = blk_init_device(true, false);
    if (!bdev) {
        printf("Failed to initialize block device\n");
        return 1;
    }

    fq = blk_alloc_flush_queue();
    if (!fq) {
        printf("Failed to allocate flush queue\n");
        blk_free_device(bdev);
        return 1;
    }

    /* Print initial status */
    blk_print_device_status(bdev, fq);

    /* Submit some test requests */
    printf("\nSubmitting test requests:\n");
    printf("------------------------\n");

    /* Write with pre-flush */
    blk_submit_request(bdev, fq, REQ_OP_WRITE,
                      REQ_PREFLUSH, 0, 1, write_data);

    /* Process the queue */
    blk_process_flush_queue(bdev, fq);

    /* Read back the data */
    blk_submit_request(bdev, fq, REQ_OP_READ,
                      0, 0, 1, read_data);

    /* Process the queue again */
    blk_process_flush_queue(bdev, fq);

    /* Print final status */
    blk_print_device_status(bdev, fq);

    /* Cleanup */
    blk_free_flush_queue(fq);
    blk_free_device(bdev);

    return 0;
}
