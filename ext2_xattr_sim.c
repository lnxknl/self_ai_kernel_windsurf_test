/*
 * EXT2 Extended Attribute Management Simulation
 * 
 * This program simulates the extended attribute (xattr) management of the ext2 filesystem,
 * including xattr block allocation, storage, and manipulation.
 * It provides a comprehensive simulation of how ext2 manages extended attributes.
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
#define EXT2_XATTR_MAGIC     0xEA020000
#define EXT2_BLOCK_SIZE      4096
#define EXT2_NAME_LEN        255
#define EXT2_XATTR_PAD       4
#define EXT2_XATTR_ROUND     (EXT2_XATTR_PAD - 1)
#define EXT2_XATTR_LEN_MAX   (EXT2_BLOCK_SIZE - sizeof(struct ext2_xattr_header))
#define EXT2_XATTR_BLOCK_NR  3
#define EXT2_XATTR_HASH_BITS 10
#define EXT2_XATTR_HASH_SIZE (1 << EXT2_XATTR_HASH_BITS)

/* Error Codes */
#define ERR_NO_SPACE    -1
#define ERR_INVALID     -2
#define ERR_CORRUPT     -3
#define ERR_NO_MEMORY   -4
#define ERR_EXISTS      -5
#define ERR_NOT_FOUND   -6

/* Extended Attribute Name Index */
#define EXT2_XATTR_INDEX_USER              1
#define EXT2_XATTR_INDEX_POSIX_ACL_ACCESS  2
#define EXT2_XATTR_INDEX_POSIX_ACL_DEFAULT 3
#define EXT2_XATTR_INDEX_TRUSTED          4
#define EXT2_XATTR_INDEX_LUSTRE           5
#define EXT2_XATTR_INDEX_SECURITY         6

/* Extended Attribute Header */
struct ext2_xattr_header {
    uint32_t h_magic;    /* Magic number for identification */
    uint32_t h_refcount; /* Reference count */
    uint32_t h_blocks;   /* Number of disk blocks used */
    uint32_t h_hash;     /* Hash value of all attributes */
    uint32_t h_checksum; /* Checksum of the block */
    uint32_t h_reserved[3]; /* Zero right now */
};

/* Extended Attribute Entry */
struct ext2_xattr_entry {
    uint8_t  e_name_len;    /* Length of name */
    uint8_t  e_name_index;  /* Attribute name index */
    uint16_t e_value_offs;  /* Offset in disk block of value */
    uint32_t e_value_inum;  /* Inode where value is stored */
    uint32_t e_value_size;  /* Size of attribute value */
    uint32_t e_hash;        /* Hash value of name and value */
    char     e_name[0];     /* Attribute name */
};

/* Extended Attribute Block */
struct ext2_xattr_block {
    struct ext2_xattr_header header;
    char data[EXT2_BLOCK_SIZE - sizeof(struct ext2_xattr_header)];
};

/* Extended Attribute Cache Entry */
struct ext2_xattr_cache_entry {
    uint32_t ce_block;      /* Block number */
    uint32_t ce_refcount;   /* Reference count */
    uint32_t ce_access_count; /* Access count for LRU */
    bool     ce_dirty;      /* Modified flag */
    pthread_mutex_t ce_mutex; /* Entry lock */
    struct ext2_xattr_block *ce_block; /* Block data */
    struct ext2_xattr_cache_entry *ce_next; /* LRU list */
};

/* Extended Attribute Cache */
struct ext2_xattr_cache {
    unsigned int c_max_entries;  /* Maximum number of entries */
    unsigned int c_num_entries;  /* Current number of entries */
    pthread_mutex_t c_mutex;     /* Cache lock */
    struct ext2_xattr_cache_entry *c_lru_list; /* LRU list head */
    struct ext2_xattr_cache_entry *c_entries[EXT2_XATTR_HASH_SIZE];
};

/* Inode Structure (simplified) */
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

/* Function Prototypes */
static uint32_t ext2_xattr_hash_entry(struct ext2_xattr_entry *entry);
static struct ext2_xattr_cache *ext2_xattr_cache_create(unsigned int size);
static void ext2_xattr_cache_destroy(struct ext2_xattr_cache *cache);
static int ext2_xattr_cache_add(struct ext2_xattr_cache *cache,
                               struct ext2_xattr_block *block,
                               uint32_t block_nr);
static struct ext2_xattr_block *ext2_xattr_cache_find(
    struct ext2_xattr_cache *cache, uint32_t block_nr);
static void ext2_xattr_cache_remove(struct ext2_xattr_cache *cache,
                                  uint32_t block_nr);

/* Utility Functions */

static uint32_t ext2_xattr_hash(const char *name, size_t len) {
    uint32_t hash = 0;
    size_t i;

    for (i = 0; i < len; i++)
        hash = (hash << 5) + hash + name[i];

    return hash & (EXT2_XATTR_HASH_SIZE - 1);
}

static uint32_t ext2_xattr_hash_entry(struct ext2_xattr_entry *entry) {
    uint32_t hash;
    uint32_t value_hash = 0;
    char *value = NULL;

    /* Hash the name */
    hash = ext2_xattr_hash(entry->e_name, entry->e_name_len);

    /* Hash the value if present */
    if (entry->e_value_size > 0 && value) {
        value_hash = ext2_xattr_hash(value, entry->e_value_size);
        hash = (hash << 16) + value_hash;
    }

    return hash;
}

static size_t ext2_xattr_entry_size(size_t name_len) {
    return ((sizeof(struct ext2_xattr_entry) + name_len + 
             EXT2_XATTR_ROUND) & ~EXT2_XATTR_ROUND);
}

static struct ext2_xattr_entry *ext2_xattr_next_entry(
    struct ext2_xattr_entry *entry) {
    return (struct ext2_xattr_entry *)((char *)entry +
           ext2_xattr_entry_size(entry->e_name_len));
}

/* Cache Management */

static struct ext2_xattr_cache *ext2_xattr_cache_create(unsigned int size) {
    struct ext2_xattr_cache *cache;
    
    cache = calloc(1, sizeof(*cache));
    if (!cache)
        return NULL;
    
    cache->c_max_entries = size;
    pthread_mutex_init(&cache->c_mutex, NULL);
    
    return cache;
}

static void ext2_xattr_cache_destroy(struct ext2_xattr_cache *cache) {
    struct ext2_xattr_cache_entry *entry, *next;
    
    if (!cache)
        return;
    
    pthread_mutex_lock(&cache->c_mutex);
    
    /* Free all entries */
    entry = cache->c_lru_list;
    while (entry) {
        next = entry->ce_next;
        pthread_mutex_destroy(&entry->ce_mutex);
        free(entry->ce_block);
        free(entry);
        entry = next;
    }
    
    pthread_mutex_unlock(&cache->c_mutex);
    pthread_mutex_destroy(&cache->c_mutex);
    free(cache);
}

static int ext2_xattr_cache_add(struct ext2_xattr_cache *cache,
                               struct ext2_xattr_block *block,
                               uint32_t block_nr) {
    struct ext2_xattr_cache_entry *entry;
    uint32_t hash = block_nr % EXT2_XATTR_HASH_SIZE;
    
    if (!cache || !block)
        return ERR_INVALID;
    
    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return ERR_NO_MEMORY;
    
    entry->ce_block = malloc(sizeof(*block));
    if (!entry->ce_block) {
        free(entry);
        return ERR_NO_MEMORY;
    }
    
    pthread_mutex_init(&entry->ce_mutex, NULL);
    memcpy(entry->ce_block, block, sizeof(*block));
    entry->ce_block = block_nr;
    entry->ce_refcount = 1;
    entry->ce_access_count = 1;
    
    pthread_mutex_lock(&cache->c_mutex);
    
    /* Add to hash table */
    entry->ce_next = cache->c_entries[hash];
    cache->c_entries[hash] = entry;
    cache->c_num_entries++;
    
    /* Add to LRU list */
    entry->ce_next = cache->c_lru_list;
    cache->c_lru_list = entry;
    
    pthread_mutex_unlock(&cache->c_mutex);
    
    return 0;
}

static struct ext2_xattr_block *ext2_xattr_cache_find(
    struct ext2_xattr_cache *cache, uint32_t block_nr) {
    struct ext2_xattr_cache_entry *entry;
    uint32_t hash = block_nr % EXT2_XATTR_HASH_SIZE;
    struct ext2_xattr_block *block = NULL;
    
    if (!cache)
        return NULL;
    
    pthread_mutex_lock(&cache->c_mutex);
    
    entry = cache->c_entries[hash];
    while (entry) {
        if (entry->ce_block == block_nr) {
            pthread_mutex_lock(&entry->ce_mutex);
            entry->ce_refcount++;
            entry->ce_access_count++;
            block = entry->ce_block;
            pthread_mutex_unlock(&entry->ce_mutex);
            break;
        }
        entry = entry->ce_next;
    }
    
    pthread_mutex_unlock(&cache->c_mutex);
    
    return block;
}

static void ext2_xattr_cache_remove(struct ext2_xattr_cache *cache,
                                  uint32_t block_nr) {
    struct ext2_xattr_cache_entry *entry, *prev = NULL;
    uint32_t hash = block_nr % EXT2_XATTR_HASH_SIZE;
    
    if (!cache)
        return;
    
    pthread_mutex_lock(&cache->c_mutex);
    
    entry = cache->c_entries[hash];
    while (entry) {
        if (entry->ce_block == block_nr) {
            if (prev)
                prev->ce_next = entry->ce_next;
            else
                cache->c_entries[hash] = entry->ce_next;
            
            /* Remove from LRU list */
            if (cache->c_lru_list == entry)
                cache->c_lru_list = entry->ce_next;
            
            pthread_mutex_destroy(&entry->ce_mutex);
            free(entry->ce_block);
            free(entry);
            cache->c_num_entries--;
            break;
        }
        prev = entry;
        entry = entry->ce_next;
    }
    
    pthread_mutex_unlock(&cache->c_mutex);
}

/* Extended Attribute Operations */

static int ext2_xattr_set(struct ext2_inode *inode,
                         int name_index,
                         const char *name,
                         const void *value,
                         size_t value_len,
                         int flags) {
    struct ext2_xattr_block *block;
    struct ext2_xattr_entry *entry, *last;
    size_t name_len, total_len;
    char *end;
    
    if (!inode || !name)
        return ERR_INVALID;
    
    name_len = strlen(name);
    if (name_len > EXT2_NAME_LEN)
        return ERR_INVALID;
    
    if (value_len > EXT2_XATTR_LEN_MAX)
        return ERR_INVALID;
    
    /* Allocate new block if needed */
    if (!inode->i_file_acl) {
        block = calloc(1, sizeof(*block));
        if (!block)
            return ERR_NO_MEMORY;
        
        block->header.h_magic = EXT2_XATTR_MAGIC;
        block->header.h_refcount = 1;
        block->header.h_blocks = 1;
    } else {
        /* Load existing block */
        block = malloc(sizeof(*block));
        if (!block)
            return ERR_NO_MEMORY;
        
        /* In a real implementation, we would read from disk here */
        memset(block, 0, sizeof(*block));
    }
    
    /* Find the place to add new entry */
    entry = (struct ext2_xattr_entry *)(block->data);
    end = block->data + EXT2_XATTR_LEN_MAX;
    last = entry;
    
    while ((char *)entry < end && entry->e_name_len) {
        if (entry->e_name_index == name_index &&
            entry->e_name_len == name_len &&
            memcmp(entry->e_name, name, name_len) == 0) {
            if (flags & XATTR_CREATE)
                return ERR_EXISTS;
            break;
        }
        last = entry;
        entry = ext2_xattr_next_entry(entry);
    }
    
    total_len = ext2_xattr_entry_size(name_len) + value_len;
    
    if ((char *)entry >= end || total_len > (end - (char *)entry))
        return ERR_NO_SPACE;
    
    /* Set up new entry */
    entry->e_name_len = name_len;
    entry->e_name_index = name_index;
    entry->e_value_size = value_len;
    memcpy(entry->e_name, name, name_len);
    
    if (value_len > 0)
        memcpy((char *)last + sizeof(*entry) + name_len, value, value_len);
    
    entry->e_hash = ext2_xattr_hash_entry(entry);
    
    /* Update header */
    block->header.h_hash = 0;  /* Recalculate hash */
    
    /* In a real implementation, we would write to disk here */
    printf("Setting xattr '%s' with value of size %zu\n", name, value_len);
    
    free(block);
    return 0;
}

static int ext2_xattr_get(struct ext2_inode *inode,
                         int name_index,
                         const char *name,
                         void *buffer,
                         size_t buffer_size) {
    struct ext2_xattr_block *block;
    struct ext2_xattr_entry *entry;
    size_t name_len;
    char *end;
    
    if (!inode || !name)
        return ERR_INVALID;
    
    if (!inode->i_file_acl)
        return ERR_NOT_FOUND;
    
    name_len = strlen(name);
    if (name_len > EXT2_NAME_LEN)
        return ERR_INVALID;
    
    /* Load block */
    block = malloc(sizeof(*block));
    if (!block)
        return ERR_NO_MEMORY;
    
    /* In a real implementation, we would read from disk here */
    memset(block, 0, sizeof(*block));
    
    /* Find the entry */
    entry = (struct ext2_xattr_entry *)(block->data);
    end = block->data + EXT2_XATTR_LEN_MAX;
    
    while ((char *)entry < end && entry->e_name_len) {
        if (entry->e_name_index == name_index &&
            entry->e_name_len == name_len &&
            memcmp(entry->e_name, name, name_len) == 0) {
            size_t size = entry->e_value_size;
            
            if (buffer && size > buffer_size) {
                free(block);
                return ERR_NO_SPACE;
            }
            
            if (buffer) {
                memcpy(buffer,
                       (char *)entry + sizeof(*entry) + entry->e_name_len,
                       size);
            }
            
            free(block);
            return size;
        }
        entry = ext2_xattr_next_entry(entry);
    }
    
    free(block);
    return ERR_NOT_FOUND;
}

/* Example Usage and Testing */

static void run_xattr_test(void) {
    struct ext2_inode *inode;
    struct ext2_xattr_cache *cache;
    char value_buf[256];
    int ret;
    
    printf("EXT2 Extended Attribute Management Simulation\n");
    printf("===========================================\n\n");
    
    /* Create test inode */
    inode = calloc(1, sizeof(*inode));
    if (!inode) {
        printf("Failed to allocate inode\n");
        return;
    }
    
    /* Create xattr cache */
    cache = ext2_xattr_cache_create(16);
    if (!cache) {
        printf("Failed to create xattr cache\n");
        free(inode);
        return;
    }
    
    printf("Setting extended attributes:\n");
    
    /* Set some test attributes */
    ret = ext2_xattr_set(inode, EXT2_XATTR_INDEX_USER,
                        "user.test1", "value1", 6, 0);
    printf("Setting user.test1: %d\n", ret);
    
    ret = ext2_xattr_set(inode, EXT2_XATTR_INDEX_USER,
                        "user.test2", "value2", 6, 0);
    printf("Setting user.test2: %d\n", ret);
    
    ret = ext2_xattr_set(inode, EXT2_XATTR_INDEX_TRUSTED,
                        "trusted.test", "secret", 6, 0);
    printf("Setting trusted.test: %d\n", ret);
    
    printf("\nRetrieving extended attributes:\n");
    
    /* Get test attributes */
    ret = ext2_xattr_get(inode, EXT2_XATTR_INDEX_USER,
                        "user.test1", value_buf, sizeof(value_buf));
    printf("Getting user.test1: %d\n", ret);
    
    ret = ext2_xattr_get(inode, EXT2_XATTR_INDEX_USER,
                        "user.test2", value_buf, sizeof(value_buf));
    printf("Getting user.test2: %d\n", ret);
    
    ret = ext2_xattr_get(inode, EXT2_XATTR_INDEX_TRUSTED,
                        "trusted.test", value_buf, sizeof(value_buf));
    printf("Getting trusted.test: %d\n", ret);
    
    /* Try to get non-existent attribute */
    ret = ext2_xattr_get(inode, EXT2_XATTR_INDEX_USER,
                        "user.nonexistent", value_buf, sizeof(value_buf));
    printf("Getting non-existent attribute: %d\n", ret);
    
    /* Cleanup */
    ext2_xattr_cache_destroy(cache);
    free(inode);
}

int main(void) {
    run_xattr_test();
    return 0;
}
