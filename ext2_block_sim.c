/*
 * EXT2 Block Management Simulation
 * 
 * This program provides a detailed simulation of the ext2 filesystem's block
 * management system, including block allocation, deallocation, reservation
 * windows, and block group management. It simulates the behavior of the actual
 * ext2 filesystem while being independently compilable and runnable.
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

/* Configuration and Constants */
#define EXT2_BLOCK_SIZE       4096
#define EXT2_BLOCKS_PER_GROUP 8192
#define EXT2_INODES_PER_GROUP 2048
#define EXT2_MAX_GROUPS       128
#define EXT2_BITMAP_SIZE      (EXT2_BLOCKS_PER_GROUP / 8)
#define EXT2_DESC_PER_BLOCK   32
#define EXT2_RESERVE_WINDOW_BLOCKS 1024
#define EXT2_MAX_RESERVED_BLOCKS   256

/* Block States */
#define BLOCK_STATE_FREE      0
#define BLOCK_STATE_ALLOCATED 1
#define BLOCK_STATE_RESERVED  2
#define BLOCK_STATE_METADATA  3

/* Error Codes */
#define ERR_NO_SPACE    -1
#define ERR_INVALID     -2
#define ERR_RESERVED    -3
#define ERR_NO_MEMORY   -4

/* Statistics Tracking */
struct block_stats {
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t reserved_blocks;
    uint32_t metadata_blocks;
    uint32_t allocations;
    uint32_t deallocations;
    uint32_t reservation_hits;
    uint32_t reservation_misses;
    uint32_t fragmentation_score;
};

/* Block Group Descriptor */
struct ext2_group_desc {
    uint32_t bg_block_bitmap;      /* Block number of block bitmap */
    uint32_t bg_inode_bitmap;      /* Block number of inode bitmap */
    uint32_t bg_inode_table;       /* Block number of first inode table block */
    uint16_t bg_free_blocks_count; /* Number of free blocks */
    uint16_t bg_free_inodes_count; /* Number of free inodes */
    uint16_t bg_used_dirs_count;   /* Number of directories */
    uint16_t bg_flags;             /* Flags */
    uint32_t bg_reserved[3];       /* Reserved for future use */
    struct block_stats stats;      /* Per-group statistics */
};

/* Reservation Window */
struct ext2_reserve_window {
    uint32_t start_block;          /* First block in window */
    uint32_t end_block;            /* Last block in window */
    uint32_t owner;               /* Window owner ID */
    uint32_t allocated_blocks;    /* Number of allocated blocks */
    time_t creation_time;         /* Window creation timestamp */
    struct ext2_reserve_window *next;
    struct ext2_reserve_window *prev;
};

/* Block Group Management */
struct ext2_group_info {
    struct ext2_group_desc desc;   /* Group descriptor */
    unsigned char *block_bitmap;   /* Block bitmap */
    struct ext2_reserve_window *reservations; /* Reservation list */
    pthread_mutex_t lock;          /* Group lock */
    uint32_t last_allocated;       /* Last allocated block */
    uint32_t free_clusters;        /* Number of free block clusters */
};

/* Filesystem Context */
struct ext2_fs_context {
    uint32_t block_count;          /* Total number of blocks */
    uint32_t groups_count;         /* Number of block groups */
    uint32_t blocks_per_group;     /* Blocks per group */
    uint32_t inodes_per_group;     /* Inodes per group */
    struct ext2_group_info *groups; /* Array of block groups */
    struct block_stats total_stats; /* Filesystem-wide statistics */
    pthread_mutex_t fs_lock;       /* Filesystem lock */
    void *block_data;             /* Simulated block data */
};

/* Bitmap Operations */

static inline void set_bit(unsigned char *bitmap, unsigned int bit) {
    bitmap[bit >> 3] |= 1 << (bit & 7);
}

static inline void clear_bit(unsigned char *bitmap, unsigned int bit) {
    bitmap[bit >> 3] &= ~(1 << (bit & 7));
}

static inline int test_bit(const unsigned char *bitmap, unsigned int bit) {
    return (bitmap[bit >> 3] >> (bit & 7)) & 1;
}

/* Find first zero bit from a given position */
static int find_next_zero_bit(const unsigned char *bitmap,
                             unsigned int size,
                             unsigned int offset) {
    unsigned int pos, bit;
    unsigned char byte;

    pos = offset >> 3;
    bit = offset & 7;

    for (; pos < size; pos++) {
        byte = bitmap[pos];
        if (byte != 0xFF) {
            for (; bit < 8; bit++) {
                if (!(byte & (1 << bit)))
                    return (pos << 3) + bit;
            }
        }
        bit = 0;
    }

    return size << 3;
}

/* Find first set bit from a given position */
static int find_next_set_bit(const unsigned char *bitmap,
                            unsigned int size,
                            unsigned int offset) {
    unsigned int pos, bit;
    unsigned char byte;

    pos = offset >> 3;
    bit = offset & 7;

    for (; pos < size; pos++) {
        byte = bitmap[pos];
        if (byte != 0) {
            for (; bit < 8; bit++) {
                if (byte & (1 << bit))
                    return (pos << 3) + bit;
            }
        }
        bit = 0;
    }

    return size << 3;
}

/* Count number of set bits in a range */
static int count_bits(const unsigned char *bitmap,
                     unsigned int start,
                     unsigned int end) {
    int count = 0;
    unsigned int i;

    for (i = start; i < end; i++) {
        if (test_bit(bitmap, i))
            count++;
    }

    return count;
}

/* Reservation Window Management */

static struct ext2_reserve_window *create_reservation(uint32_t start,
                                                    uint32_t end,
                                                    uint32_t owner) {
    struct ext2_reserve_window *rsv;

    rsv = calloc(1, sizeof(*rsv));
    if (!rsv)
        return NULL;

    rsv->start_block = start;
    rsv->end_block = end;
    rsv->owner = owner;
    rsv->allocated_blocks = 0;
    rsv->creation_time = time(NULL);
    rsv->next = rsv->prev = NULL;

    return rsv;
}

static void insert_reservation(struct ext2_group_info *group,
                             struct ext2_reserve_window *rsv) {
    rsv->next = group->reservations;
    if (group->reservations)
        group->reservations->prev = rsv;
    group->reservations = rsv;
}

static void remove_reservation(struct ext2_group_info *group,
                             struct ext2_reserve_window *rsv) {
    if (rsv->prev)
        rsv->prev->next = rsv->next;
    else
        group->reservations = rsv->next;

    if (rsv->next)
        rsv->next->prev = rsv->prev;

    free(rsv);
}

/* Check if a block range overlaps with any reservation */
static bool check_reservation_conflict(struct ext2_group_info *group,
                                     uint32_t start,
                                     uint32_t end) {
    struct ext2_reserve_window *rsv;

    for (rsv = group->reservations; rsv; rsv = rsv->next) {
        if (start < rsv->end_block && end > rsv->start_block)
            return true;
    }

    return false;
}

/* Block Allocation Strategy */

static int find_free_blocks(struct ext2_group_info *group,
                           uint32_t goal,
                           unsigned int count,
                           uint32_t *start) {
    unsigned int i, found = 0;
    uint32_t current = goal;

    while (found < count && current < EXT2_BLOCKS_PER_GROUP) {
        /* Skip allocated blocks */
        current = find_next_zero_bit(group->block_bitmap,
                                   EXT2_BLOCKS_PER_GROUP,
                                   current);
        if (current >= EXT2_BLOCKS_PER_GROUP)
            break;

        /* Count consecutive free blocks */
        for (i = 0; i < count - found && current + i < EXT2_BLOCKS_PER_GROUP; i++) {
            if (test_bit(group->block_bitmap, current + i))
                break;
        }

        if (i > 0) {
            if (found == 0)
                *start = current;
            found += i;
            current += i;
        } else {
            current++;
        }
    }

    return found;
}

/* Group Selection Strategy */

static int find_best_group(struct ext2_fs_context *ctx,
                          uint32_t goal,
                          unsigned int count) {
    int group = -1;
    unsigned int max_free = 0;
    int i, goal_group;

    goal_group = goal / EXT2_BLOCKS_PER_GROUP;

    /* First try the goal group */
    if (goal_group < ctx->groups_count &&
        ctx->groups[goal_group].desc.bg_free_blocks_count >= count) {
        return goal_group;
    }

    /* Then try to find the group with most free blocks */
    for (i = 0; i < ctx->groups_count; i++) {
        unsigned int free = ctx->groups[i].desc.bg_free_blocks_count;
        if (free > max_free && free >= count) {
            max_free = free;
            group = i;
        }
    }

    return group;
}

/* Block Management Functions */

static int allocate_block_range(struct ext2_fs_context *ctx,
                              uint32_t goal,
                              unsigned int count,
                              uint32_t owner,
                              uint32_t *blocks) {
    int group;
    uint32_t start = 0;
    int allocated;
    struct ext2_group_info *gi;

    /* Find suitable group */
    group = find_best_group(ctx, goal, count);
    if (group < 0)
        return ERR_NO_SPACE;

    gi = &ctx->groups[group];

    pthread_mutex_lock(&gi->lock);

    /* Find free blocks in the group */
    allocated = find_free_blocks(gi, goal % EXT2_BLOCKS_PER_GROUP,
                               count, &start);
    if (allocated <= 0) {
        pthread_mutex_unlock(&gi->lock);
        return ERR_NO_SPACE;
    }

    /* Check reservations */
    if (check_reservation_conflict(gi, start, start + allocated)) {
        pthread_mutex_unlock(&gi->lock);
        return ERR_RESERVED;
    }

    /* Mark blocks as allocated */
    unsigned int i;
    for (i = 0; i < allocated; i++) {
        set_bit(gi->block_bitmap, start + i);
        blocks[i] = group * EXT2_BLOCKS_PER_GROUP + start + i;
    }

    /* Update statistics */
    gi->desc.bg_free_blocks_count -= allocated;
    gi->desc.stats.allocations++;
    gi->last_allocated = start + allocated - 1;

    pthread_mutex_unlock(&gi->lock);

    /* Update filesystem statistics */
    pthread_mutex_lock(&ctx->fs_lock);
    ctx->total_stats.free_blocks -= allocated;
    ctx->total_stats.allocations++;
    pthread_mutex_unlock(&ctx->fs_lock);

    return allocated;
}

static int free_block_range(struct ext2_fs_context *ctx,
                           uint32_t start_block,
                           unsigned int count) {
    unsigned int i;
    int group = start_block / EXT2_BLOCKS_PER_GROUP;
    uint32_t start = start_block % EXT2_BLOCKS_PER_GROUP;
    struct ext2_group_info *gi;

    if (group >= ctx->groups_count)
        return ERR_INVALID;

    gi = &ctx->groups[group];

    pthread_mutex_lock(&gi->lock);

    /* Mark blocks as free */
    for (i = 0; i < count && start + i < EXT2_BLOCKS_PER_GROUP; i++) {
        if (!test_bit(gi->block_bitmap, start + i))
            continue;
        clear_bit(gi->block_bitmap, start + i);
        gi->desc.bg_free_blocks_count++;
    }

    /* Update statistics */
    gi->desc.stats.deallocations++;

    pthread_mutex_unlock(&gi->lock);

    /* Update filesystem statistics */
    pthread_mutex_lock(&ctx->fs_lock);
    ctx->total_stats.free_blocks += i;
    ctx->total_stats.deallocations++;
    pthread_mutex_unlock(&ctx->fs_lock);

    return i;
}

/* Filesystem Context Management */

static struct ext2_fs_context *create_fs_context(void) {
    struct ext2_fs_context *ctx;
    unsigned int i;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->block_count = EXT2_BLOCKS_PER_GROUP * EXT2_MAX_GROUPS;
    ctx->groups_count = EXT2_MAX_GROUPS;
    ctx->blocks_per_group = EXT2_BLOCKS_PER_GROUP;
    ctx->inodes_per_group = EXT2_INODES_PER_GROUP;

    /* Allocate groups */
    ctx->groups = calloc(ctx->groups_count, sizeof(struct ext2_group_info));
    if (!ctx->groups)
        goto err_groups;

    /* Initialize groups */
    for (i = 0; i < ctx->groups_count; i++) {
        struct ext2_group_info *gi = &ctx->groups[i];

        gi->block_bitmap = calloc(EXT2_BITMAP_SIZE, 1);
        if (!gi->block_bitmap)
            goto err_init;

        pthread_mutex_init(&gi->lock, NULL);
        gi->desc.bg_free_blocks_count = EXT2_BLOCKS_PER_GROUP;
        gi->desc.bg_free_inodes_count = EXT2_INODES_PER_GROUP;
    }

    pthread_mutex_init(&ctx->fs_lock, NULL);

    /* Initialize statistics */
    ctx->total_stats.total_blocks = ctx->block_count;
    ctx->total_stats.free_blocks = ctx->block_count;

    return ctx;

err_init:
    for (i = 0; i < ctx->groups_count; i++) {
        free(ctx->groups[i].block_bitmap);
        pthread_mutex_destroy(&ctx->groups[i].lock);
    }
    free(ctx->groups);
err_groups:
    free(ctx);
    return NULL;
}

static void destroy_fs_context(struct ext2_fs_context *ctx) {
    unsigned int i;

    if (!ctx)
        return;

    for (i = 0; i < ctx->groups_count; i++) {
        struct ext2_group_info *gi = &ctx->groups[i];
        struct ext2_reserve_window *rsv, *next;

        /* Free reservations */
        rsv = gi->reservations;
        while (rsv) {
            next = rsv->next;
            free(rsv);
            rsv = next;
        }

        free(gi->block_bitmap);
        pthread_mutex_destroy(&gi->lock);
    }

    free(ctx->groups);
    pthread_mutex_destroy(&ctx->fs_lock);
    free(ctx);
}

/* Example Usage and Testing */

static void print_group_stats(struct ext2_fs_context *ctx, int group) {
    struct ext2_group_info *gi = &ctx->groups[group];
    struct block_stats *stats = &gi->desc.stats;

    printf("Group %d Statistics:\n", group);
    printf("  Free blocks: %u\n", gi->desc.bg_free_blocks_count);
    printf("  Allocations: %u\n", stats->allocations);
    printf("  Deallocations: %u\n", stats->deallocations);
    printf("  Reservation hits: %u\n", stats->reservation_hits);
    printf("  Reservation misses: %u\n", stats->reservation_misses);
    printf("\n");
}

static void run_allocation_test(struct ext2_fs_context *ctx) {
    uint32_t blocks[100];
    int i, ret;

    printf("Running block allocation test...\n\n");

    /* Single block allocations */
    printf("Single block allocations:\n");
    for (i = 0; i < 5; i++) {
        ret = allocate_block_range(ctx, 0, 1, 1, blocks);
        if (ret > 0)
            printf("Allocated block: %u\n", blocks[0]);
        else
            printf("Allocation failed: %d\n", ret);
    }
    printf("\n");

    /* Multi-block allocation */
    printf("Multi-block allocation:\n");
    ret = allocate_block_range(ctx, 1000, 10, 1, blocks);
    if (ret > 0) {
        printf("Allocated %d blocks:\n", ret);
        for (i = 0; i < ret; i++)
            printf("  Block %d: %u\n", i, blocks[i]);
    } else {
        printf("Allocation failed: %d\n", ret);
    }
    printf("\n");

    /* Free some blocks */
    printf("Freeing blocks:\n");
    for (i = 0; i < ret; i++) {
        int freed = free_block_range(ctx, blocks[i], 1);
        printf("Freed %d blocks starting at %u\n", freed, blocks[i]);
    }
    printf("\n");

    /* Print statistics */
    printf("Block Group Statistics:\n");
    for (i = 0; i < 3; i++)
        print_group_stats(ctx, i);
}

int main(void) {
    struct ext2_fs_context *ctx;

    ctx = create_fs_context();
    if (!ctx) {
        fprintf(stderr, "Failed to create filesystem context\n");
        return 1;
    }

    run_allocation_test(ctx);

    destroy_fs_context(ctx);
    return 0;
}
