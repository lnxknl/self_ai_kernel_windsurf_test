/*
 * EXT2 Directory and Inode Management Simulation
 * 
 * This program simulates the directory and inode management of the ext2 filesystem,
 * including directory creation, inode allocation, and directory entry management.
 * It provides a comprehensive simulation of how ext2 manages its directory structure.
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
#define EXT2_BLOCK_SIZE          4096
#define EXT2_INODES_PER_GROUP    8192
#define EXT2_BLOCKS_PER_GROUP    32768
#define EXT2_MAX_GROUPS          128
#define EXT2_INODE_SIZE          256
#define EXT2_FIRST_INO           11
#define EXT2_ROOT_INO            2
#define EXT2_NAME_LEN            255
#define EXT2_MAX_DEPTH           10
#define EXT2_DIR_PAD            4
#define EXT2_DIR_ROUND          (EXT2_DIR_PAD - 1)

/* Inode Modes */
#define EXT2_S_IFREG  0x8000  /* Regular file */
#define EXT2_S_IFDIR  0x4000  /* Directory */
#define EXT2_S_IFLNK  0xA000  /* Symbolic link */

/* Error Codes */
#define ERR_NO_SPACE    -1
#define ERR_INVALID     -2
#define ERR_CORRUPT     -3
#define ERR_NO_MEMORY   -4
#define ERR_EXISTS      -5
#define ERR_NOT_FOUND   -6

/* Directory Entry Structure */
struct ext2_dir_entry {
    uint32_t inode;        /* Inode number */
    uint16_t rec_len;      /* Directory entry length */
    uint8_t  name_len;     /* Name length */
    uint8_t  file_type;    /* File type */
    char     name[EXT2_NAME_LEN]; /* File name */
};

/* Inode Structure */
struct ext2_inode {
    uint16_t i_mode;        /* File mode */
    uint16_t i_uid;         /* Owner Uid */
    uint32_t i_size;        /* Size in bytes */
    uint32_t i_atime;       /* Access time */
    uint32_t i_ctime;       /* Creation time */
    uint32_t i_mtime;       /* Modification time */
    uint32_t i_dtime;       /* Deletion Time */
    uint16_t i_gid;         /* Group Id */
    uint16_t i_links_count; /* Links count */
    uint32_t i_blocks;      /* Blocks count */
    uint32_t i_flags;       /* File flags */
    uint32_t i_block[15];   /* Pointers to blocks */
    uint32_t i_generation;  /* File version (for NFS) */
    uint32_t i_file_acl;    /* File ACL */
    uint32_t i_dir_acl;     /* Directory ACL */
    uint32_t i_faddr;       /* Fragment address */
    uint8_t  i_frag;        /* Fragment number */
    uint8_t  i_fsize;       /* Fragment size */
    uint16_t i_pad1;        /* Padding */
    uint32_t i_reserved[2]; /* Reserved */
};

/* Directory Block */
struct ext2_dir_block {
    char data[EXT2_BLOCK_SIZE];
    uint32_t free_space;
    struct ext2_dir_block *next;
};

/* Directory Context */
struct ext2_dir_context {
    uint32_t inode;
    struct ext2_dir_block *first_block;
    uint32_t block_count;
    uint32_t entry_count;
    pthread_mutex_t lock;
};

/* Group Descriptor */
struct ext2_group_desc {
    uint32_t bg_block_bitmap;      /* Block bitmap block */
    uint32_t bg_inode_bitmap;      /* Inode bitmap block */
    uint32_t bg_inode_table;       /* Inode table block */
    uint16_t bg_free_blocks_count; /* Free blocks count */
    uint16_t bg_free_inodes_count; /* Free inodes count */
    uint16_t bg_used_dirs_count;   /* Directories count */
    uint16_t bg_flags;             /* Flags */
    uint32_t bg_reserved[3];       /* Reserved for future use */
};

/* Group Info */
struct ext2_group_info {
    struct ext2_group_desc desc;    /* Group descriptor */
    unsigned char *inode_bitmap;    /* Inode bitmap */
    struct ext2_inode *inode_table; /* Inode table */
    pthread_mutex_t lock;           /* Group lock */
    uint32_t last_allocated_inode;  /* Last allocated inode */
};

/* Filesystem Context */
struct ext2_fs_context {
    uint32_t inode_count;           /* Total number of inodes */
    uint32_t groups_count;          /* Number of groups */
    uint32_t inodes_per_group;      /* Inodes per group */
    struct ext2_group_info *groups; /* Array of groups */
    struct ext2_dir_context *dirs;  /* Array of directories */
    uint32_t dir_count;             /* Number of directories */
    pthread_mutex_t fs_lock;        /* Filesystem lock */
    uint32_t s_free_inodes_count;   /* Free inodes count */
    uint32_t s_dirs_count;          /* Directory count */
};

/* Directory Entry Management */

static uint16_t ext2_dir_rec_len(uint8_t name_len) {
    return (name_len + 8 + EXT2_DIR_ROUND) & ~EXT2_DIR_ROUND;
}

static struct ext2_dir_entry *ext2_next_entry(struct ext2_dir_entry *entry) {
    return (struct ext2_dir_entry *)((char *)entry + entry->rec_len);
}

static int ext2_dir_entry_valid(struct ext2_dir_entry *entry,
                               unsigned int block_size) {
    if (entry->rec_len < EXT2_DIR_ROUND)
        return 0;
    if (entry->rec_len % EXT2_DIR_PAD)
        return 0;
    if (entry->rec_len < ext2_dir_rec_len(entry->name_len))
        return 0;
    if ((char *)ext2_next_entry(entry) > ((char *)entry + block_size))
        return 0;
    return 1;
}

/* Directory Block Management */

static struct ext2_dir_block *create_dir_block(void) {
    struct ext2_dir_block *block;

    block = calloc(1, sizeof(*block));
    if (!block)
        return NULL;

    block->free_space = EXT2_BLOCK_SIZE;
    block->next = NULL;

    return block;
}

static void free_dir_block(struct ext2_dir_block *block) {
    struct ext2_dir_block *next;

    while (block) {
        next = block->next;
        free(block);
        block = next;
    }
}

/* Directory Context Management */

static struct ext2_dir_context *create_dir_context(uint32_t inode) {
    struct ext2_dir_context *ctx;
    struct ext2_dir_block *block;

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    block = create_dir_block();
    if (!block) {
        free(ctx);
        return NULL;
    }

    ctx->inode = inode;
    ctx->first_block = block;
    ctx->block_count = 1;
    ctx->entry_count = 0;
    pthread_mutex_init(&ctx->lock, NULL);

    return ctx;
}

static void destroy_dir_context(struct ext2_dir_context *ctx) {
    if (!ctx)
        return;

    free_dir_block(ctx->first_block);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

/* Directory Entry Operations */

static int add_dir_entry(struct ext2_dir_context *ctx,
                        uint32_t inode,
                        const char *name,
                        uint8_t file_type) {
    struct ext2_dir_block *block;
    struct ext2_dir_entry *entry;
    uint16_t rec_len;
    size_t name_len;

    name_len = strlen(name);
    if (name_len > EXT2_NAME_LEN)
        return ERR_INVALID;

    rec_len = ext2_dir_rec_len(name_len);

    pthread_mutex_lock(&ctx->lock);

    /* Find block with enough space */
    for (block = ctx->first_block; block; block = block->next) {
        if (block->free_space >= rec_len)
            break;
    }

    /* Create new block if needed */
    if (!block) {
        block = create_dir_block();
        if (!block) {
            pthread_mutex_unlock(&ctx->lock);
            return ERR_NO_MEMORY;
        }

        /* Add to end of chain */
        struct ext2_dir_block *last = ctx->first_block;
        while (last->next)
            last = last->next;
        last->next = block;
        ctx->block_count++;
    }

    /* Add entry to block */
    entry = (struct ext2_dir_entry *)(block->data + EXT2_BLOCK_SIZE - block->free_space);
    entry->inode = inode;
    entry->rec_len = rec_len;
    entry->name_len = name_len;
    entry->file_type = file_type;
    memcpy(entry->name, name, name_len);

    block->free_space -= rec_len;
    ctx->entry_count++;

    pthread_mutex_unlock(&ctx->lock);
    return 0;
}

static int remove_dir_entry(struct ext2_dir_context *ctx,
                           const char *name) {
    struct ext2_dir_block *block;
    struct ext2_dir_entry *entry, *prev;
    size_t name_len = strlen(name);

    pthread_mutex_lock(&ctx->lock);

    /* Search all blocks */
    for (block = ctx->first_block; block; block = block->next) {
        prev = NULL;
        entry = (struct ext2_dir_entry *)block->data;

        while ((char *)entry < block->data + EXT2_BLOCK_SIZE - block->free_space) {
            if (entry->name_len == name_len &&
                memcmp(entry->name, name, name_len) == 0) {
                /* Found entry to remove */
                uint16_t rec_len = entry->rec_len;

                if (prev) {
                    /* Merge with previous entry */
                    prev->rec_len += rec_len;
                } else {
                    /* First entry in block */
                    memmove(block->data,
                           (char *)entry + rec_len,
                           EXT2_BLOCK_SIZE - block->free_space - rec_len);
                }

                block->free_space += rec_len;
                ctx->entry_count--;

                pthread_mutex_unlock(&ctx->lock);
                return 0;
            }

            prev = entry;
            entry = ext2_next_entry(entry);
        }
    }

    pthread_mutex_unlock(&ctx->lock);
    return ERR_NOT_FOUND;
}

/* Directory Tree Operations */

static int create_directory(struct ext2_fs_context *fs_ctx,
                          uint32_t parent_ino,
                          const char *name) {
    struct ext2_dir_context *parent_dir = NULL;
    struct ext2_dir_context *new_dir;
    uint32_t i;
    int ret;

    /* Find parent directory */
    for (i = 0; i < fs_ctx->dir_count; i++) {
        if (fs_ctx->dirs[i]->inode == parent_ino) {
            parent_dir = fs_ctx->dirs[i];
            break;
        }
    }

    if (!parent_dir)
        return ERR_NOT_FOUND;

    /* Allocate new inode */
    uint32_t new_ino = fs_ctx->inode_count + 1;
    fs_ctx->inode_count++;

    /* Create directory context */
    new_dir = create_dir_context(new_ino);
    if (!new_dir)
        return ERR_NO_MEMORY;

    /* Add to filesystem context */
    struct ext2_dir_context **new_dirs = realloc(fs_ctx->dirs,
                                                (fs_ctx->dir_count + 1) *
                                                sizeof(*new_dirs));
    if (!new_dirs) {
        destroy_dir_context(new_dir);
        return ERR_NO_MEMORY;
    }

    fs_ctx->dirs = new_dirs;
    fs_ctx->dirs[fs_ctx->dir_count++] = new_dir;

    /* Add directory entries */
    ret = add_dir_entry(new_dir, new_ino, ".", EXT2_S_IFDIR);
    if (ret < 0)
        goto err;

    ret = add_dir_entry(new_dir, parent_ino, "..", EXT2_S_IFDIR);
    if (ret < 0)
        goto err;

    /* Add entry to parent */
    ret = add_dir_entry(parent_dir, new_ino, name, EXT2_S_IFDIR);
    if (ret < 0)
        goto err;

    return new_ino;

err:
    destroy_dir_context(new_dir);
    fs_ctx->dirs[--fs_ctx->dir_count] = NULL;
    return ret;
}

/* Example Usage and Testing */

static void print_dir_entries(struct ext2_dir_context *ctx) {
    struct ext2_dir_block *block;
    struct ext2_dir_entry *entry;
    int block_num = 0;

    printf("Directory inode %u contents:\n", ctx->inode);
    printf("Total entries: %u\n", ctx->entry_count);
    printf("Total blocks: %u\n\n", ctx->block_count);

    for (block = ctx->first_block; block; block = block->next) {
        printf("Block %d (free space: %u):\n", block_num++, block->free_space);
        entry = (struct ext2_dir_entry *)block->data;

        while ((char *)entry < block->data + EXT2_BLOCK_SIZE - block->free_space) {
            char name[EXT2_NAME_LEN + 1];
            memcpy(name, entry->name, entry->name_len);
            name[entry->name_len] = '\0';

            printf("  %-20s inode: %-10u rec_len: %-4u type: %u\n",
                   name, entry->inode, entry->rec_len, entry->file_type);

            entry = ext2_next_entry(entry);
        }
        printf("\n");
    }
}

static void run_directory_test(void) {
    struct ext2_fs_context fs_ctx = {0};
    struct ext2_dir_context *root_dir;
    uint32_t new_dirs[5];
    int i;

    /* Create root directory */
    root_dir = create_dir_context(EXT2_ROOT_INO);
    if (!root_dir) {
        fprintf(stderr, "Failed to create root directory\n");
        return;
    }

    fs_ctx.dirs = calloc(1, sizeof(*fs_ctx.dirs));
    if (!fs_ctx.dirs) {
        destroy_dir_context(root_dir);
        return;
    }

    fs_ctx.dirs[0] = root_dir;
    fs_ctx.dir_count = 1;
    fs_ctx.inode_count = EXT2_ROOT_INO;

    /* Add initial entries to root */
    add_dir_entry(root_dir, EXT2_ROOT_INO, ".", EXT2_S_IFDIR);
    add_dir_entry(root_dir, EXT2_ROOT_INO, "..", EXT2_S_IFDIR);

    /* Create some directories */
    printf("Creating directories:\n");
    for (i = 0; i < 5; i++) {
        char name[32];
        snprintf(name, sizeof(name), "dir%d", i + 1);
        
        new_dirs[i] = create_directory(&fs_ctx, EXT2_ROOT_INO, name);
        if (new_dirs[i] > 0)
            printf("Created directory '%s' with inode %u\n", name, new_dirs[i]);
        else
            printf("Failed to create directory '%s': %d\n", name, new_dirs[i]);
    }
    printf("\n");

    /* Print directory contents */
    print_dir_entries(root_dir);
    for (i = 1; i < fs_ctx.dir_count; i++)
        print_dir_entries(fs_ctx.dirs[i]);

    /* Cleanup */
    for (i = 0; i < fs_ctx.dir_count; i++)
        destroy_dir_context(fs_ctx.dirs[i]);
    free(fs_ctx.dirs);
}

int main(void) {
    printf("EXT2 Directory Management Simulation\n");
    printf("===================================\n\n");

    run_directory_test();

    return 0;
}
