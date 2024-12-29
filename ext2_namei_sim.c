/*
 * EXT2 Name Lookup and Directory Entry Management Simulation
 * 
 * This program simulates the name lookup and directory entry management
 * of the ext2 filesystem, including directory traversal, entry creation,
 * and deletion operations.
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
#define EXT2_NAME_LEN        255
#define EXT2_MAX_DEPTH       10
#define EXT2_BLOCK_SIZE      4096
#define EXT2_DIR_PAD         4
#define EXT2_DIR_ROUND       (EXT2_DIR_PAD - 1)
#define EXT2_ROOT_INO        2
#define EXT2_GOOD_OLD_FIRST_INO 11
#define EXT2_LINK_MAX        32000
#define EXT2_LINKS_MAX       65000

/* File Types */
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

/* Error Codes */
#define ERR_NO_SPACE     -1
#define ERR_INVALID      -2
#define ERR_CORRUPT      -3
#define ERR_NO_MEMORY    -4
#define ERR_EXISTS       -5
#define ERR_NOT_FOUND    -6
#define ERR_NOT_DIR      -7
#define ERR_IS_DIR       -8
#define ERR_TOO_MANY_LINKS -9
#define ERR_NAME_TOO_LONG  -10

/* Directory Entry Structure */
struct ext2_dir_entry {
    uint32_t inode;        /* Inode number */
    uint16_t rec_len;      /* Directory entry length */
    uint8_t  name_len;     /* Name length */
    uint8_t  file_type;    /* File type */
    char     name[EXT2_NAME_LEN]; /* File name */
};

/* Directory Block */
struct ext2_dir_block {
    char data[EXT2_BLOCK_SIZE];
    uint32_t free_space;
    struct ext2_dir_block *next;
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
};

/* Directory Context */
struct ext2_dir_context {
    uint32_t inode;
    struct ext2_dir_block *first_block;
    uint32_t block_count;
    uint32_t entry_count;
    pthread_mutex_t lock;
};

/* Name Hash Entry */
struct ext2_name_hash_entry {
    char *name;
    uint32_t inode;
    uint8_t file_type;
    struct ext2_name_hash_entry *next;
};

/* Name Hash Table */
struct ext2_name_hash {
    struct ext2_name_hash_entry *buckets[256];
    pthread_mutex_t lock;
};

/* Function Prototypes */
static uint32_t ext2_name_hash(const char *name, size_t len);
static struct ext2_dir_entry *ext2_find_entry(struct ext2_dir_context *dir,
                                            const char *name,
                                            size_t namelen);
static int ext2_add_entry(struct ext2_dir_context *dir,
                         const char *name,
                         uint32_t inode,
                         uint8_t file_type);
static int ext2_delete_entry(struct ext2_dir_context *dir,
                           const char *name);
static struct ext2_name_hash *ext2_create_name_hash(void);
static void ext2_destroy_name_hash(struct ext2_name_hash *hash);

/* Utility Functions */

static uint32_t ext2_name_hash(const char *name, size_t len) {
    uint32_t hash = 0;
    size_t i;

    for (i = 0; i < len; i++)
        hash = (hash << 5) + hash + name[i];

    return hash & 0xFF;
}

static uint16_t ext2_dir_rec_len(uint8_t name_len) {
    return (sizeof(struct ext2_dir_entry) - EXT2_NAME_LEN + name_len + 
            EXT2_DIR_ROUND) & ~EXT2_DIR_ROUND;
}

static struct ext2_dir_entry *ext2_next_entry(struct ext2_dir_entry *entry) {
    return (struct ext2_dir_entry *)((char *)entry + entry->rec_len);
}

static int ext2_check_dir_entry(struct ext2_dir_context *dir,
                               struct ext2_dir_entry *de,
                               struct ext2_dir_block *block,
                               unsigned int offset) {
    const char *error_msg = NULL;
    const int rlen = ext2_dir_rec_len(de->name_len);

    if (de->rec_len < rlen)
        error_msg = "rec_len is smaller than minimal";
    else if (de->rec_len % 4 != 0)
        error_msg = "rec_len % 4 != 0";
    else if (de->rec_len < rlen)
        error_msg = "rec_len is too small for name_len";
    else if ((char *)de + de->rec_len > block->data + EXT2_BLOCK_SIZE)
        error_msg = "directory entry across block boundary";
    else if (de->inode > dir->entry_count)
        error_msg = "inode out of bounds";

    if (error_msg) {
        printf("bad directory entry: %s\n"
               "inode=%u, rec_len=%u, name_len=%u\n",
               error_msg, de->inode, de->rec_len, de->name_len);
        return ERR_CORRUPT;
    }
    return 0;
}

/* Name Hash Table Operations */

static struct ext2_name_hash *ext2_create_name_hash(void) {
    struct ext2_name_hash *hash;
    
    hash = calloc(1, sizeof(*hash));
    if (!hash)
        return NULL;
    
    pthread_mutex_init(&hash->lock, NULL);
    return hash;
}

static void ext2_destroy_name_hash(struct ext2_name_hash *hash) {
    struct ext2_name_hash_entry *entry, *next;
    int i;
    
    if (!hash)
        return;
    
    pthread_mutex_lock(&hash->lock);
    
    for (i = 0; i < 256; i++) {
        entry = hash->buckets[i];
        while (entry) {
            next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    
    pthread_mutex_unlock(&hash->lock);
    pthread_mutex_destroy(&hash->lock);
    free(hash);
}

static int ext2_add_to_hash(struct ext2_name_hash *hash,
                           const char *name,
                           uint32_t inode,
                           uint8_t file_type) {
    struct ext2_name_hash_entry *entry;
    uint32_t hash_val;
    size_t name_len;
    
    if (!hash || !name)
        return ERR_INVALID;
    
    name_len = strlen(name);
    if (name_len > EXT2_NAME_LEN)
        return ERR_NAME_TOO_LONG;
    
    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return ERR_NO_MEMORY;
    
    entry->name = strdup(name);
    if (!entry->name) {
        free(entry);
        return ERR_NO_MEMORY;
    }
    
    entry->inode = inode;
    entry->file_type = file_type;
    
    hash_val = ext2_name_hash(name, name_len);
    
    pthread_mutex_lock(&hash->lock);
    entry->next = hash->buckets[hash_val];
    hash->buckets[hash_val] = entry;
    pthread_mutex_unlock(&hash->lock);
    
    return 0;
}

static struct ext2_name_hash_entry *ext2_find_in_hash(
    struct ext2_name_hash *hash,
    const char *name) {
    struct ext2_name_hash_entry *entry;
    uint32_t hash_val;
    
    if (!hash || !name)
        return NULL;
    
    hash_val = ext2_name_hash(name, strlen(name));
    
    pthread_mutex_lock(&hash->lock);
    entry = hash->buckets[hash_val];
    while (entry) {
        if (strcmp(entry->name, name) == 0)
            break;
        entry = entry->next;
    }
    pthread_mutex_unlock(&hash->lock);
    
    return entry;
}

/* Directory Operations */

static struct ext2_dir_entry *ext2_find_entry(struct ext2_dir_context *dir,
                                            const char *name,
                                            size_t namelen) {
    struct ext2_dir_block *block;
    struct ext2_dir_entry *de;
    unsigned int offset;
    int err;
    
    if (!dir || !name || namelen > EXT2_NAME_LEN)
        return NULL;
    
    pthread_mutex_lock(&dir->lock);
    
    for (block = dir->first_block; block; block = block->next) {
        offset = 0;
        while (offset < EXT2_BLOCK_SIZE) {
            de = (struct ext2_dir_entry *)(block->data + offset);
            
            err = ext2_check_dir_entry(dir, de, block, offset);
            if (err) {
                pthread_mutex_unlock(&dir->lock);
                return NULL;
            }
            
            if (de->name_len == namelen &&
                memcmp(de->name, name, namelen) == 0) {
                pthread_mutex_unlock(&dir->lock);
                return de;
            }
            
            offset += de->rec_len;
        }
    }
    
    pthread_mutex_unlock(&dir->lock);
    return NULL;
}

static int ext2_add_entry(struct ext2_dir_context *dir,
                         const char *name,
                         uint32_t inode,
                         uint8_t file_type) {
    struct ext2_dir_block *block, *new_block;
    struct ext2_dir_entry *de;
    size_t namelen;
    uint16_t rec_len;
    
    if (!dir || !name)
        return ERR_INVALID;
    
    namelen = strlen(name);
    if (namelen > EXT2_NAME_LEN)
        return ERR_NAME_TOO_LONG;
    
    rec_len = ext2_dir_rec_len(namelen);
    
    pthread_mutex_lock(&dir->lock);
    
    /* Try to find space in existing blocks */
    for (block = dir->first_block; block; block = block->next) {
        if (block->free_space >= rec_len) {
            de = (struct ext2_dir_entry *)(block->data + 
                                         EXT2_BLOCK_SIZE - 
                                         block->free_space);
            de->inode = inode;
            de->rec_len = rec_len;
            de->name_len = namelen;
            de->file_type = file_type;
            memcpy(de->name, name, namelen);
            
            block->free_space -= rec_len;
            dir->entry_count++;
            
            pthread_mutex_unlock(&dir->lock);
            return 0;
        }
    }
    
    /* Need to allocate new block */
    new_block = calloc(1, sizeof(*new_block));
    if (!new_block) {
        pthread_mutex_unlock(&dir->lock);
        return ERR_NO_MEMORY;
    }
    
    new_block->free_space = EXT2_BLOCK_SIZE;
    
    /* Add entry to new block */
    de = (struct ext2_dir_entry *)new_block->data;
    de->inode = inode;
    de->rec_len = rec_len;
    de->name_len = namelen;
    de->file_type = file_type;
    memcpy(de->name, name, namelen);
    
    new_block->free_space -= rec_len;
    
    /* Add block to chain */
    block = dir->first_block;
    while (block->next)
        block = block->next;
    block->next = new_block;
    
    dir->block_count++;
    dir->entry_count++;
    
    pthread_mutex_unlock(&dir->lock);
    return 0;
}

static int ext2_delete_entry(struct ext2_dir_context *dir,
                           const char *name) {
    struct ext2_dir_block *block, *prev_block;
    struct ext2_dir_entry *de, *prev_de;
    size_t namelen;
    unsigned int offset;
    
    if (!dir || !name)
        return ERR_INVALID;
    
    namelen = strlen(name);
    if (namelen > EXT2_NAME_LEN)
        return ERR_NAME_TOO_LONG;
    
    pthread_mutex_lock(&dir->lock);
    
    prev_block = NULL;
    for (block = dir->first_block; block; block = block->next) {
        offset = 0;
        prev_de = NULL;
        
        while (offset < EXT2_BLOCK_SIZE) {
            de = (struct ext2_dir_entry *)(block->data + offset);
            
            if (de->name_len == namelen &&
                memcmp(de->name, name, namelen) == 0) {
                /* Found the entry to delete */
                if (prev_de)
                    prev_de->rec_len += de->rec_len;
                else
                    block->free_space += de->rec_len;
                
                /* If block is now empty, remove it */
                if (block->free_space == EXT2_BLOCK_SIZE &&
                    block != dir->first_block) {
                    prev_block->next = block->next;
                    free(block);
                    dir->block_count--;
                } else {
                    /* Clear the deleted entry */
                    memset(de, 0, de->rec_len);
                }
                
                dir->entry_count--;
                pthread_mutex_unlock(&dir->lock);
                return 0;
            }
            
            prev_de = de;
            offset += de->rec_len;
        }
        prev_block = block;
    }
    
    pthread_mutex_unlock(&dir->lock);
    return ERR_NOT_FOUND;
}

/* Example Usage and Testing */

static void print_dir_entries(struct ext2_dir_context *dir) {
    struct ext2_dir_block *block;
    struct ext2_dir_entry *de;
    unsigned int offset;
    int block_num = 0;
    
    printf("Directory inode %u contents:\n", dir->inode);
    printf("Total entries: %u\n", dir->entry_count);
    printf("Total blocks: %u\n\n", dir->block_count);
    
    pthread_mutex_lock(&dir->lock);
    
    for (block = dir->first_block; block; block = block->next) {
        printf("Block %d (free space: %u):\n", block_num++, block->free_space);
        
        offset = 0;
        while (offset < EXT2_BLOCK_SIZE - block->free_space) {
            de = (struct ext2_dir_entry *)(block->data + offset);
            
            if (de->inode) {
                char name[EXT2_NAME_LEN + 1];
                memcpy(name, de->name, de->name_len);
                name[de->name_len] = '\0';
                
                printf("  %-20s inode: %-10u rec_len: %-4u type: %u\n",
                       name, de->inode, de->rec_len, de->file_type);
            }
            
            offset += de->rec_len;
        }
        printf("\n");
    }
    
    pthread_mutex_unlock(&dir->lock);
}

static void run_directory_test(void) {
    struct ext2_dir_context *dir;
    struct ext2_name_hash *hash;
    int ret;
    
    printf("EXT2 Directory Entry Management Simulation\n");
    printf("========================================\n\n");
    
    /* Create directory context */
    dir = calloc(1, sizeof(*dir));
    if (!dir) {
        printf("Failed to allocate directory context\n");
        return;
    }
    
    dir->inode = EXT2_ROOT_INO;
    pthread_mutex_init(&dir->lock, NULL);
    
    /* Create first block */
    dir->first_block = calloc(1, sizeof(struct ext2_dir_block));
    if (!dir->first_block) {
        printf("Failed to allocate directory block\n");
        free(dir);
        return;
    }
    
    dir->first_block->free_space = EXT2_BLOCK_SIZE;
    dir->block_count = 1;
    
    /* Create name hash table */
    hash = ext2_create_name_hash();
    if (!hash) {
        printf("Failed to create name hash table\n");
        free(dir->first_block);
        free(dir);
        return;
    }
    
    printf("Adding directory entries:\n");
    
    /* Add some test entries */
    ret = ext2_add_entry(dir, ".", EXT2_ROOT_INO, EXT2_FT_DIR);
    printf("Adding '.': %d\n", ret);
    
    ret = ext2_add_entry(dir, "..", EXT2_ROOT_INO, EXT2_FT_DIR);
    printf("Adding '..': %d\n", ret);
    
    ret = ext2_add_entry(dir, "file1.txt", 100, EXT2_FT_REG_FILE);
    printf("Adding 'file1.txt': %d\n", ret);
    
    ret = ext2_add_entry(dir, "file2.txt", 101, EXT2_FT_REG_FILE);
    printf("Adding 'file2.txt': %d\n", ret);
    
    ret = ext2_add_entry(dir, "subdir", 102, EXT2_FT_DIR);
    printf("Adding 'subdir': %d\n", ret);
    
    /* Add entries to hash table */
    ext2_add_to_hash(hash, "file1.txt", 100, EXT2_FT_REG_FILE);
    ext2_add_to_hash(hash, "file2.txt", 101, EXT2_FT_REG_FILE);
    ext2_add_to_hash(hash, "subdir", 102, EXT2_FT_DIR);
    
    printf("\nDirectory contents:\n");
    print_dir_entries(dir);
    
    printf("Looking up entries:\n");
    struct ext2_dir_entry *de;
    
    de = ext2_find_entry(dir, "file1.txt", strlen("file1.txt"));
    printf("Looking up 'file1.txt': %s\n",
           de ? "found" : "not found");
    
    de = ext2_find_entry(dir, "nonexistent", strlen("nonexistent"));
    printf("Looking up 'nonexistent': %s\n",
           de ? "found" : "not found");
    
    printf("\nDeleting entries:\n");
    
    ret = ext2_delete_entry(dir, "file1.txt");
    printf("Deleting 'file1.txt': %d\n", ret);
    
    ret = ext2_delete_entry(dir, "nonexistent");
    printf("Deleting 'nonexistent': %d\n", ret);
    
    printf("\nFinal directory contents:\n");
    print_dir_entries(dir);
    
    /* Cleanup */
    ext2_destroy_name_hash(hash);
    struct ext2_dir_block *block = dir->first_block;
    while (block) {
        struct ext2_dir_block *next = block->next;
        free(block);
        block = next;
    }
    pthread_mutex_destroy(&dir->lock);
    free(dir);
}

int main(void) {
    run_directory_test();
    return 0;
}
