#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "fops_standalone.h"

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct block_device *blkdev_init(size_t size, size_t block_size) {
    struct block_device *bdev;

    if (size == 0 || block_size == 0 || (size % block_size) != 0) {
        errno = EINVAL;
        return NULL;
    }

    bdev = calloc(1, sizeof(*bdev));
    if (!bdev)
        return NULL;

    bdev->data = calloc(1, size);
    if (!bdev->data) {
        free(bdev);
        return NULL;
    }

    bdev->size = size;
    bdev->block_size = block_size;
    bdev->is_open = false;
    bdev->read_count = 0;
    bdev->write_count = 0;

    return bdev;
}

void blkdev_cleanup(struct block_device *bdev) {
    if (bdev) {
        free(bdev->data);
        free(bdev);
    }
}

int blkdev_open(struct block_device *bdev) {
    if (!bdev || bdev->is_open) {
        errno = EINVAL;
        return -1;
    }

    bdev->is_open = true;
    return 0;
}

int blkdev_release(struct block_device *bdev) {
    if (!bdev || !bdev->is_open) {
        errno = EINVAL;
        return -1;
    }

    bdev->is_open = false;
    return 0;
}

ssize_t blkdev_read(struct block_device *bdev, struct file_pos *pos,
                    void *buf, size_t count) {
    if (!bdev || !bdev->is_open || !pos || !buf) {
        errno = EINVAL;
        return -1;
    }

    if (pos->offset < 0 || (size_t)pos->offset >= bdev->size) {
        return 0;  /* EOF */
    }

    /* Adjust count if it would go past end of device */
    if ((size_t)pos->offset + count > bdev->size)
        count = bdev->size - pos->offset;

    /* Ensure reads are block-aligned */
    if (pos->offset % bdev->block_size != 0 || count % bdev->block_size != 0) {
        errno = EINVAL;
        return -1;
    }

    memcpy(buf, bdev->data + pos->offset, count);
    pos->offset += count;
    bdev->read_count++;

    return count;
}

ssize_t blkdev_write(struct block_device *bdev, struct file_pos *pos,
                     const void *buf, size_t count) {
    if (!bdev || !bdev->is_open || !pos || !buf) {
        errno = EINVAL;
        return -1;
    }

    if (pos->offset < 0 || (size_t)pos->offset >= bdev->size) {
        errno = ENOSPC;
        return -1;
    }

    /* Adjust count if it would go past end of device */
    if ((size_t)pos->offset + count > bdev->size)
        count = bdev->size - pos->offset;

    /* Ensure writes are block-aligned */
    if (pos->offset % bdev->block_size != 0 || count % bdev->block_size != 0) {
        errno = EINVAL;
        return -1;
    }

    memcpy(bdev->data + pos->offset, buf, count);
    pos->offset += count;
    bdev->write_count++;

    return count;
}

int64_t blkdev_llseek(struct block_device *bdev, struct file_pos *pos,
                      int64_t offset, int whence) {
    int64_t new_pos;

    if (!bdev || !bdev->is_open || !pos) {
        errno = EINVAL;
        return -1;
    }

    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = pos->offset + offset;
        break;
    case SEEK_END:
        new_pos = bdev->size + offset;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    if (new_pos < 0 || (size_t)new_pos > bdev->size) {
        errno = EINVAL;
        return -1;
    }

    pos->offset = new_pos;
    return new_pos;
}

void blkdev_get_stats(struct block_device *bdev, uint64_t *reads, uint64_t *writes) {
    if (bdev && reads && writes) {
        *reads = bdev->read_count;
        *writes = bdev->write_count;
    }
}

/* Example usage */
int main(void) {
    struct block_device *bdev;
    struct file_pos pos = {0};
    char write_buf[512] = "Hello, Block Device!";
    char read_buf[512] = {0};
    uint64_t reads, writes;

    /* Initialize a 1MB block device with 512-byte blocks */
    bdev = blkdev_init(1024 * 1024, 512);
    if (!bdev) {
        perror("Failed to initialize block device");
        return 1;
    }

    /* Open the device */
    if (blkdev_open(bdev) < 0) {
        perror("Failed to open block device");
        blkdev_cleanup(bdev);
        return 1;
    }

    /* Write some data */
    if (blkdev_write(bdev, &pos, write_buf, 512) < 0) {
        perror("Write failed");
        goto cleanup;
    }

    /* Seek back to start */
    if (blkdev_llseek(bdev, &pos, 0, SEEK_SET) < 0) {
        perror("Seek failed");
        goto cleanup;
    }

    /* Read the data back */
    if (blkdev_read(bdev, &pos, read_buf, 512) < 0) {
        perror("Read failed");
        goto cleanup;
    }

    /* Get and print statistics */
    blkdev_get_stats(bdev, &reads, &writes);
    printf("Read data: %s\n", read_buf);
    printf("Statistics: %lu reads, %lu writes\n", reads, writes);

cleanup:
    blkdev_release(bdev);
    blkdev_cleanup(bdev);
    return 0;
}
