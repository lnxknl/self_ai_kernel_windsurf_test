/*
 * EXT2 Block Allocation Simulation
 * 
 * This program simulates the block allocation mechanism of the ext2 filesystem.
 * It provides block allocation, deallocation, and bitmap management features
 * similar to the actual ext2 implementation.
 *
 * Author: Cascade AI
 * Date: 2024-12-29
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>
#include <time.h>

/* Configuration Constants */
#define BLOCK_SIZE          4096
#define BLOCKS_PER_GROUP    8192
#define INODES_PER_GROUP    2048
#define MAX_GROUPS          128
#define BITMAP_SIZE         (BLOCKS_PER_GROUP / 8)
#define DESC_PER_BLOCK      32
#define RESERVE_BLOCKS      5

/* Block Types */
enum {
    BLOCK_TYPE_FREE     = 0,
    BLOCK_TYPE_DATA     = 1,
    BLOCK_TYPE_BITMAP   = 2,
    BLOCK_TYPE_INODE    = 3,
    BLOCK_TYPE_SUPER    = 4,
    BLOCK_TYPE_GDT      = 5,
};

/* Block Group Descriptor */
struct group_desc {
    uint32_t block_bitmap;      /* Block number of block bitmap */
    uint32_t inode_bitmap;      /* Block number of inode bitmap */
    uint32_t inode_table;       /* Block number of first inode table block */
    uint16_t free_blocks_count; /* Number of free blocks */
    uint16_t free_inodes_count; /* Number of free inodes */
    uint16_t used_dirs_count;   /* Number of directories */
    uint16_t flags;             /* Flags */
    uint32_t reserved[3];       /* Reserved for future use */
};

/* Superblock Structure */
struct superblock {
    uint32_t inodes_count;      /* Total number of inodes */
    uint32_t blocks_count;      /* Total number of blocks */
    uint32_t r_blocks_count;    /* Number of reserved blocks */
    uint32_t free_blocks_count; /* Number of free blocks */
    uint32_t free_inodes_count; /* Number of free inodes */
    uint32_t first_data_block;  /* First data block number */
    uint32_t block_size;        /* Block size */
    uint32_t blocks_per_group;  /* Number of blocks per group */
    uint32_t inodes_per_group;  /* Number of inodes per group */
    uint32_t mtime;            /* Mount time */
    uint32_t wtime;            /* Write time */
    uint16_t mnt_count;        /* Mount count */
    uint16_t max_mnt_count;    /* Maximum mount count */
    uint16_t magic;            /* Magic signature */
    uint16_t state;            /* File system state */
    uint32_t lastcheck;        /* Last check time */
    uint32_t checkinterval;    /* Check interval */
};

/* Reservation Window */
struct reserve_window {
    uint32_t start;            /* First block */
    uint32_t end;             /* Last block */
    struct reserve_window *next;
    struct reserve_window *prev;
};

/* Block Group Info */
struct group_info {
    struct group_desc desc;     /* Group descriptor */
    unsigned char *block_bitmap;/* Block bitmap */
    pthread_mutex_t lock;       /* Group lock */
    struct reserve_window *rsv_list; /* Reservation list */
};

/* Filesystem Context */
struct fs_context {
    struct superblock sb;       /* Superblock */
    struct group_info *groups;  /* Block groups */
    pthread_mutex_t sb_lock;    /* Superblock lock */
    void *blocks;              /* Block data */
};

/* Helper Functions */

static inline uint32_t get_group_first_block(struct fs_context *ctx, unsigned int group) {
    return group * ctx->sb.blocks_per_group + ctx->sb.first_data_block;
}

static inline unsigned int get_block_group(struct fs_context *ctx, uint32_t block) {
    return (block - ctx->sb.first_data_block) / ctx->sb.blocks_per_group;
}

static void set_bit(unsigned char *bitmap, unsigned int bit) {
    bitmap[bit >> 3] |= 1 << (bit & 7);
}

static void clear_bit(unsigned char *bitmap, unsigned int bit) {
    bitmap[bit >> 3] &= ~(1 << (bit & 7));
}

static int test_bit(const unsigned char *bitmap, unsigned int bit) {
    return (bitmap[bit >> 3] >> (bit & 7)) & 1;
}

/* Bitmap Management Functions */

static int count_free_bits(const unsigned char *bitmap, unsigned int size) {
    int count = 0;
    unsigned int i;
    
    for (i = 0; i < size; i++) {
        unsigned char byte = bitmap[i];
        while (byte) {
            if (byte & 1)
                count++;
            byte >>= 1;
        }
    }
    
    return (size * 8) - count;
}

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

/* Block Group Management */

static int init_group_desc(struct fs_context *ctx, unsigned int group) {
    struct group_info *gi = &ctx->groups[group];
    uint32_t first_block = get_group_first_block(ctx, group);
    
    /* Initialize descriptor */
    gi->desc.block_bitmap = first_block + 1;
    gi->desc.inode_bitmap = first_block + 2;
    gi->desc.inode_table = first_block + 3;
    gi->desc.free_blocks_count = ctx->sb.blocks_per_group;
    gi->desc.free_inodes_count = ctx->sb.inodes_per_group;
    gi->desc.used_dirs_count = 0;
    
    /* Allocate and initialize bitmap */
    gi->block_bitmap = calloc(BITMAP_SIZE, 1);
    if (!gi->block_bitmap)
        return -ENOMEM;
    
    /* Mark system blocks as used */
    set_bit(gi->block_bitmap, 0); /* Superblock */
    set_bit(gi->block_bitmap, 1); /* Block bitmap */
    set_bit(gi->block_bitmap, 2); /* Inode bitmap */
    
    /* Mark inode table blocks */
    unsigned int i;
    for (i = 0; i < (ctx->sb.inodes_per_group * sizeof(struct group_desc)) / BLOCK_SIZE; i++)
        set_bit(gi->block_bitmap, 3 + i);
    
    pthread_mutex_init(&gi->lock, NULL);
    gi->rsv_list = NULL;
    
    return 0;
}

static void cleanup_group(struct group_info *gi) {
    struct reserve_window *rsv, *next;
    
    free(gi->block_bitmap);
    
    /* Free reservation list */
    rsv = gi->rsv_list;
    while (rsv) {
        next = rsv->next;
        free(rsv);
        rsv = next;
    }
    
    pthread_mutex_destroy(&gi->lock);
}

/* Filesystem Context Management */

struct fs_context *fs_context_create(void) {
    struct fs_context *ctx;
    unsigned int i;
    
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    
    /* Initialize superblock */
    ctx->sb.blocks_count = BLOCKS_PER_GROUP * MAX_GROUPS;
    ctx->sb.inodes_count = INODES_PER_GROUP * MAX_GROUPS;
    ctx->sb.r_blocks_count = RESERVE_BLOCKS;
    ctx->sb.free_blocks_count = ctx->sb.blocks_count - RESERVE_BLOCKS;
    ctx->sb.free_inodes_count = ctx->sb.inodes_count;
    ctx->sb.first_data_block = 0;
    ctx->sb.block_size = BLOCK_SIZE;
    ctx->sb.blocks_per_group = BLOCKS_PER_GROUP;
    ctx->sb.inodes_per_group = INODES_PER_GROUP;
    ctx->sb.magic = 0xEF53;
    
    /* Allocate groups */
    ctx->groups = calloc(MAX_GROUPS, sizeof(struct group_info));
    if (!ctx->groups)
        goto err_groups;
    
    /* Initialize groups */
    for (i = 0; i < MAX_GROUPS; i++) {
        if (init_group_desc(ctx, i) < 0)
            goto err_init;
    }
    
    pthread_mutex_init(&ctx->sb_lock, NULL);
    
    /* Allocate blocks */
    ctx->blocks = calloc(ctx->sb.blocks_count, BLOCK_SIZE);
    if (!ctx->blocks)
        goto err_blocks;
    
    return ctx;

err_blocks:
    pthread_mutex_destroy(&ctx->sb_lock);
err_init:
    for (i = 0; i < MAX_GROUPS; i++)
        cleanup_group(&ctx->groups[i]);
    free(ctx->groups);
err_groups:
    free(ctx);
    return NULL;
}

void fs_context_destroy(struct fs_context *ctx) {
    unsigned int i;
    
    if (!ctx)
        return;
    
    free(ctx->blocks);
    
    for (i = 0; i < MAX_GROUPS; i++)
        cleanup_group(&ctx->groups[i]);
    
    free(ctx->groups);
    pthread_mutex_destroy(&ctx->sb_lock);
    free(ctx);
}

/* Block Allocation Functions */

static int try_to_allocate_block(struct fs_context *ctx,
                               unsigned int group,
                               uint32_t goal,
                               unsigned long *count) {
    struct group_info *gi = &ctx->groups[group];
    unsigned int start, end, bit;
    
    start = goal & (BLOCKS_PER_GROUP - 1);
    end = BLOCKS_PER_GROUP;
    
    pthread_mutex_lock(&gi->lock);
    
    /* Try to allocate starting from goal */
    bit = find_next_zero_bit(gi->block_bitmap, end, start);
    if (bit >= end) {
        /* Try from beginning if goal failed */
        bit = find_next_zero_bit(gi->block_bitmap, start, 0);
        if (bit >= start) {
            pthread_mutex_unlock(&gi->lock);
            return -ENOSPC;
        }
    }
    
    /* Mark block as used */
    set_bit(gi->block_bitmap, bit);
    gi->desc.free_blocks_count--;
    
    pthread_mutex_unlock(&gi->lock);
    
    pthread_mutex_lock(&ctx->sb_lock);
    ctx->sb.free_blocks_count--;
    pthread_mutex_unlock(&ctx->sb_lock);
    
    *count = 1;
    return bit;
}

uint32_t allocate_blocks(struct fs_context *ctx,
                        uint32_t goal,
                        unsigned long *count) {
    unsigned int group, ngroups;
    int ret;
    
    if (*count == 0)
        return 0;
    
    if (goal >= ctx->sb.blocks_count)
        goal = 0;
    
    group = get_block_group(ctx, goal);
    ngroups = ctx->sb.blocks_count / ctx->sb.blocks_per_group;
    
    /* Try goal group first */
    ret = try_to_allocate_block(ctx, group, goal, count);
    if (ret >= 0)
        return get_group_first_block(ctx, group) + ret;
    
    /* Try other groups */
    unsigned int i;
    for (i = 0; i < ngroups; i++) {
        group = (group + 1) % ngroups;
        ret = try_to_allocate_block(ctx, group, 0, count);
        if (ret >= 0)
            return get_group_first_block(ctx, group) + ret;
    }
    
    return 0;
}

void free_blocks(struct fs_context *ctx, uint32_t block, unsigned long count) {
    unsigned int group;
    struct group_info *gi;
    uint32_t bit;
    
    if (block >= ctx->sb.blocks_count || count == 0)
        return;
    
    group = get_block_group(ctx, block);
    gi = &ctx->groups[group];
    bit = block & (BLOCKS_PER_GROUP - 1);
    
    pthread_mutex_lock(&gi->lock);
    
    /* Mark blocks as free */
    while (count > 0 && bit < BLOCKS_PER_GROUP) {
        if (test_bit(gi->block_bitmap, bit)) {
            clear_bit(gi->block_bitmap, bit);
            gi->desc.free_blocks_count++;
            
            pthread_mutex_lock(&ctx->sb_lock);
            ctx->sb.free_blocks_count++;
            pthread_mutex_unlock(&ctx->sb_lock);
        }
        bit++;
        count--;
    }
    
    pthread_mutex_unlock(&gi->lock);
    
    /* Handle blocks in next group */
    if (count > 0) {
        block = get_group_first_block(ctx, group + 1);
        free_blocks(ctx, block, count);
    }
}

/* Example Usage */

void example_block_allocation(void) {
    struct fs_context *ctx;
    uint32_t block;
    unsigned long count;
    
    printf("EXT2 Block Allocation Simulation\n");
    printf("================================\n\n");
    
    /* Create filesystem context */
    ctx = fs_context_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create filesystem context\n");
        return;
    }
    
    /* Allocate some blocks */
    printf("Initial free blocks: %u\n", ctx->sb.free_blocks_count);
    
    count = 5;
    block = allocate_blocks(ctx, 0, &count);
    printf("Allocated %lu blocks starting at block %u\n", count, block);
    
    count = 3;
    block = allocate_blocks(ctx, 1000, &count);
    printf("Allocated %lu blocks starting at block %u\n", count, block);
    
    printf("Remaining free blocks: %u\n\n", ctx->sb.free_blocks_count);
    
    /* Free blocks */
    printf("Freeing blocks...\n");
    free_blocks(ctx, block, count);
    printf("Free blocks after freeing: %u\n", ctx->sb.free_blocks_count);
    
    /* Show group statistics */
    printf("\nBlock Group Statistics:\n");
    unsigned int i;
    for (i = 0; i < 5; i++) {
        printf("Group %u: %u free blocks\n", i,
               ctx->groups[i].desc.free_blocks_count);
    }
    
    fs_context_destroy(ctx);
}

int main(void) {
    example_block_allocation();
    return 0;
}
