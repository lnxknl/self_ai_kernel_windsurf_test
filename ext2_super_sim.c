/*
 * EXT2 Superblock Management Simulation
 * 
 * This program simulates the superblock management of the ext2 filesystem,
 * including initialization, mounting, and superblock operations.
 * It provides a comprehensive simulation of how ext2 manages its superblock
 * and filesystem metadata.
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
#define EXT2_SUPER_MAGIC    0xEF53
#define EXT2_BLOCK_SIZE     4096
#define EXT2_MIN_BLOCK_SIZE 1024
#define EXT2_MAX_BLOCK_SIZE 4096
#define EXT2_BLOCKS_PER_GROUP 8192
#define EXT2_INODES_PER_GROUP 2048
#define EXT2_MAX_GROUPS     128
#define EXT2_DESC_PER_BLOCK 32

/* Feature Flags */
#define EXT2_FEATURE_COMPAT_DIR_PREALLOC   0x0001
#define EXT2_FEATURE_COMPAT_IMAGIC_INODES  0x0002
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL    0x0004
#define EXT2_FEATURE_COMPAT_EXT_ATTR       0x0008
#define EXT2_FEATURE_COMPAT_RESIZE_INO     0x0010
#define EXT2_FEATURE_COMPAT_DIR_INDEX      0x0020

/* Filesystem States */
#define EXT2_VALID_FS      0x0001
#define EXT2_ERROR_FS      0x0002
#define EXT2_ORPHAN_FS     0x0004

/* Error Behaviors */
#define EXT2_ERRORS_CONTINUE   1
#define EXT2_ERRORS_RO        2
#define EXT2_ERRORS_PANIC     3

/* Revision Levels */
#define EXT2_GOOD_OLD_REV     0
#define EXT2_DYNAMIC_REV      1

/* Superblock Structure */
struct ext2_super_block {
    uint32_t s_inodes_count;      /* Inodes count */
    uint32_t s_blocks_count;      /* Blocks count */
    uint32_t s_r_blocks_count;    /* Reserved blocks count */
    uint32_t s_free_blocks_count; /* Free blocks count */
    uint32_t s_free_inodes_count; /* Free inodes count */
    uint32_t s_first_data_block;  /* First Data Block */
    uint32_t s_log_block_size;    /* Block size */
    uint32_t s_log_frag_size;     /* Fragment size */
    uint32_t s_blocks_per_group;  /* # Blocks per group */
    uint32_t s_frags_per_group;   /* # Fragments per group */
    uint32_t s_inodes_per_group;  /* # Inodes per group */
    uint32_t s_mtime;            /* Mount time */
    uint32_t s_wtime;            /* Write time */
    uint16_t s_mnt_count;        /* Mount count */
    uint16_t s_max_mnt_count;    /* Maximal mount count */
    uint16_t s_magic;            /* Magic signature */
    uint16_t s_state;            /* File system state */
    uint16_t s_errors;           /* Behaviour when detecting errors */
    uint16_t s_minor_rev_level;  /* Minor revision level */
    uint32_t s_lastcheck;        /* Time of last check */
    uint32_t s_checkinterval;    /* Max. time between checks */
    uint32_t s_creator_os;       /* OS */
    uint32_t s_rev_level;        /* Revision level */
    uint16_t s_def_resuid;       /* Default uid for reserved blocks */
    uint16_t s_def_resgid;       /* Default gid for reserved blocks */
    uint32_t s_first_ino;        /* First non-reserved inode */
    uint16_t s_inode_size;       /* Size of inode structure */
    uint16_t s_block_group_nr;   /* Block group # of this superblock */
    uint32_t s_feature_compat;   /* Compatible feature set */
    uint32_t s_feature_incompat; /* Incompatible feature set */
    uint32_t s_feature_ro_compat;/* Read-only compatible feature set */
    uint8_t  s_uuid[16];         /* 128-bit uuid for volume */
    char     s_volume_name[16];  /* Volume name */
    char     s_last_mounted[64]; /* Directory where last mounted */
    uint32_t s_algorithm_usage_bitmap; /* For compression */
    uint8_t  s_prealloc_blocks;  /* Nr of blocks to try to preallocate*/
    uint8_t  s_prealloc_dir_blocks;  /* Nr to preallocate for dirs */
    uint16_t s_padding1;
    uint32_t s_reserved[204];    /* Padding to the end of the block */
};

/* Group Descriptor */
struct ext2_group_desc {
    uint32_t bg_block_bitmap;      /* Blocks bitmap block */
    uint32_t bg_inode_bitmap;      /* Inodes bitmap block */
    uint32_t bg_inode_table;       /* Inodes table block */
    uint16_t bg_free_blocks_count; /* Free blocks count */
    uint16_t bg_free_inodes_count; /* Free inodes count */
    uint16_t bg_used_dirs_count;   /* Directories count */
    uint16_t bg_flags;             /* EXT2_BG_flags */
    uint32_t bg_reserved[3];       /* Reserved for future use */
};

/* Mount Options */
struct ext2_mount_options {
    unsigned long s_mount_opt;
    uid_t s_resuid;
    gid_t s_resgid;
    unsigned long s_sb_block;
    unsigned long s_mount_time;
};

/* Filesystem Info */
struct ext2_fs_info {
    struct ext2_super_block *s_es; /* Superblock */
    struct ext2_group_desc *s_group_desc; /* Group descriptors */
    unsigned long s_blocks_per_group;
    unsigned long s_frags_per_group;
    unsigned long s_inodes_per_group;
    unsigned long s_desc_per_block;
    unsigned long s_groups_count;
    unsigned long s_overhead_last; /* Last calculated overhead */
    struct ext2_mount_options mount_opt;
    unsigned long s_mount_state;
    pthread_mutex_t s_lock;
};

/* Function Prototypes */
static int ext2_check_descriptors(struct ext2_fs_info *info);
static int ext2_setup_super(struct ext2_fs_info *info, int readonly);
static void ext2_sync_super(struct ext2_fs_info *info, int wait);
static int ext2_remount(struct ext2_fs_info *info, int *flags, char *data);
static void ext2_write_super(struct ext2_fs_info *info);
static int ext2_fill_super(struct ext2_fs_info *info, void *data, int silent);

/* Utility Functions */

static uint32_t get_block_size(struct ext2_super_block *es) {
    return EXT2_MIN_BLOCK_SIZE << es->s_log_block_size;
}

static uint32_t get_desc_count(struct ext2_fs_info *info) {
    return (info->s_blocks_per_group + EXT2_DESC_PER_BLOCK - 1) / 
           EXT2_DESC_PER_BLOCK;
}

static void update_super_time(struct ext2_super_block *es) {
    es->s_wtime = time(NULL);
    es->s_mtime = es->s_wtime;
}

/* Superblock Operations */

static int ext2_check_descriptors(struct ext2_fs_info *info) {
    struct ext2_super_block *es = info->s_es;
    struct ext2_group_desc *gdp = info->s_group_desc;
    unsigned long block = 0;
    int i;

    for (i = 0; i < info->s_groups_count; i++) {
        if (gdp[i].bg_block_bitmap < es->s_first_data_block ||
            gdp[i].bg_block_bitmap >= es->s_blocks_count) {
            printf("Block bitmap for group %d not in group (block %lu)!\n",
                   i, (unsigned long)gdp[i].bg_block_bitmap);
            return 0;
        }
        if (gdp[i].bg_inode_bitmap < es->s_first_data_block ||
            gdp[i].bg_inode_bitmap >= es->s_blocks_count) {
            printf("Inode bitmap for group %d not in group (block %lu)!\n",
                   i, (unsigned long)gdp[i].bg_inode_bitmap);
            return 0;
        }
        if (gdp[i].bg_inode_table < es->s_first_data_block ||
            gdp[i].bg_inode_table >= es->s_blocks_count) {
            printf("Inode table for group %d not in group (block %lu)!\n",
                   i, (unsigned long)gdp[i].bg_inode_table);
            return 0;
        }
        block += info->s_blocks_per_group;
    }
    return 1;
}

static int ext2_setup_super(struct ext2_fs_info *info, int readonly) {
    struct ext2_super_block *es = info->s_es;
    int res = 0;

    if (es->s_magic != EXT2_SUPER_MAGIC) {
        printf("Magic mismatch, very wrong magic number\n");
        return -EINVAL;
    }

    /* Set defaults */
    if (!es->s_max_mnt_count)
        es->s_max_mnt_count = 20;
    
    es->s_state &= ~EXT2_VALID_FS;
    if (!readonly) {
        es->s_mnt_count++;
        es->s_state = EXT2_VALID_FS;
        es->s_mtime = time(NULL);
    }
    
    if (es->s_rev_level == EXT2_GOOD_OLD_REV) {
        es->s_first_ino = 11;
        es->s_inode_size = 128;
    }
    
    info->s_mount_state = es->s_state;
    info->mount_opt.s_mount_time = es->s_mtime;
    
    if (!readonly) {
        ext2_write_super(info);
    }
    
    return res;
}

static void ext2_sync_super(struct ext2_fs_info *info, int wait) {
    struct ext2_super_block *es = info->s_es;
    
    pthread_mutex_lock(&info->s_lock);
    es->s_wtime = time(NULL);
    if (wait) {
        /* In a real implementation, we would write to disk here */
        printf("Syncing superblock to disk...\n");
    }
    pthread_mutex_unlock(&info->s_lock);
}

static void ext2_write_super(struct ext2_fs_info *info) {
    struct ext2_super_block *es = info->s_es;
    
    if (!(info->s_mount_state & EXT2_VALID_FS))
        return;
    
    pthread_mutex_lock(&info->s_lock);
    es->s_wtime = time(NULL);
    /* In a real implementation, we would write to disk here */
    printf("Writing superblock to disk...\n");
    pthread_mutex_unlock(&info->s_lock);
}

static int ext2_remount(struct ext2_fs_info *info, int *flags, char *data) {
    struct ext2_super_block *es = info->s_es;
    unsigned long old_mount_opt = info->mount_opt.s_mount_opt;
    
    printf("Remounting filesystem...\n");
    
    if (!(*flags & MS_RDONLY) && (es->s_state & EXT2_ERROR_FS)) {
        printf("Filesystem has errors, remounting read-only\n");
        *flags |= MS_RDONLY;
    }
    
    if (*flags & MS_RDONLY) {
        /* Read-only remount */
        if (!(old_mount_opt & MS_RDONLY)) {
            ext2_sync_super(info, 1);
            es->s_state = info->s_mount_state;
        }
    } else {
        /* Read-write remount */
        es->s_state = info->s_mount_state;
        es->s_mtime = time(NULL);
        ext2_write_super(info);
    }
    
    return 0;
}

static int ext2_fill_super(struct ext2_fs_info *info, void *data, int silent) {
    struct ext2_super_block *es;
    unsigned long block_size;
    unsigned long logic_sb_block;
    int ret = -EINVAL;
    
    /* Allocate and initialize superblock */
    es = calloc(1, sizeof(*es));
    if (!es)
        return -ENOMEM;
    
    info->s_es = es;
    pthread_mutex_init(&info->s_lock, NULL);
    
    /* Set up defaults */
    es->s_magic = EXT2_SUPER_MAGIC;
    es->s_state = EXT2_VALID_FS;
    es->s_blocks_count = EXT2_BLOCKS_PER_GROUP * EXT2_MAX_GROUPS;
    es->s_inodes_count = EXT2_INODES_PER_GROUP * EXT2_MAX_GROUPS;
    es->s_r_blocks_count = es->s_blocks_count / 20; /* 5% reserved */
    es->s_first_data_block = 0;
    es->s_log_block_size = 2; /* 4KB blocks */
    es->s_blocks_per_group = EXT2_BLOCKS_PER_GROUP;
    es->s_inodes_per_group = EXT2_INODES_PER_GROUP;
    es->s_first_ino = 11;
    es->s_inode_size = 128;
    es->s_rev_level = EXT2_GOOD_OLD_REV;
    
    /* Calculate derived values */
    block_size = get_block_size(es);
    info->s_blocks_per_group = es->s_blocks_per_group;
    info->s_inodes_per_group = es->s_inodes_per_group;
    info->s_groups_count = (es->s_blocks_count + 
                           info->s_blocks_per_group - 1) /
                          info->s_blocks_per_group;
    
    /* Allocate group descriptors */
    info->s_group_desc = calloc(info->s_groups_count,
                               sizeof(struct ext2_group_desc));
    if (!info->s_group_desc) {
        ret = -ENOMEM;
        goto failed_mount;
    }
    
    /* Initialize group descriptors */
    unsigned long i;
    for (i = 0; i < info->s_groups_count; i++) {
        struct ext2_group_desc *gdp = &info->s_group_desc[i];
        unsigned long blk = es->s_first_data_block +
                          i * info->s_blocks_per_group;
        
        gdp->bg_block_bitmap = blk + 1;
        gdp->bg_inode_bitmap = blk + 2;
        gdp->bg_inode_table = blk + 3;
        gdp->bg_free_blocks_count = info->s_blocks_per_group;
        gdp->bg_free_inodes_count = info->s_inodes_per_group;
        gdp->bg_used_dirs_count = 0;
    }
    
    if (!ext2_check_descriptors(info)) {
        printf("Group descriptors corrupted!\n");
        ret = -EFAULT;
        goto failed_mount;
    }
    
    /* Set up mount options */
    info->mount_opt.s_mount_opt = 0;
    info->mount_opt.s_resuid = 0;
    info->mount_opt.s_resgid = 0;
    
    ret = ext2_setup_super(info, 0);
    if (ret)
        goto failed_mount;
    
    printf("Filesystem mounted successfully!\n");
    return 0;

failed_mount:
    free(info->s_group_desc);
    free(es);
    pthread_mutex_destroy(&info->s_lock);
    return ret;
}

/* Example Usage and Testing */

static void print_super_info(struct ext2_fs_info *info) {
    struct ext2_super_block *es = info->s_es;
    
    printf("\nSuperblock Information:\n");
    printf("Magic: 0x%04x\n", es->s_magic);
    printf("Blocks count: %u\n", es->s_blocks_count);
    printf("Free blocks: %u\n", es->s_free_blocks_count);
    printf("Inodes count: %u\n", es->s_inodes_count);
    printf("Free inodes: %u\n", es->s_free_inodes_count);
    printf("Block size: %lu\n", get_block_size(es));
    printf("State: 0x%04x\n", es->s_state);
    printf("Mount count: %u\n", es->s_mnt_count);
    printf("Last mount time: %u\n", es->s_mtime);
    printf("Last write time: %u\n", es->s_wtime);
    printf("\n");
}

static void print_group_info(struct ext2_fs_info *info) {
    unsigned int i;
    
    printf("Block Group Information:\n");
    for (i = 0; i < info->s_groups_count && i < 5; i++) {
        struct ext2_group_desc *gdp = &info->s_group_desc[i];
        printf("Group %u:\n", i);
        printf("  Block bitmap: %u\n", gdp->bg_block_bitmap);
        printf("  Inode bitmap: %u\n", gdp->bg_inode_bitmap);
        printf("  Inode table: %u\n", gdp->bg_inode_table);
        printf("  Free blocks: %u\n", gdp->bg_free_blocks_count);
        printf("  Free inodes: %u\n", gdp->bg_free_inodes_count);
        printf("  Used directories: %u\n", gdp->bg_used_dirs_count);
        printf("\n");
    }
}

static void run_super_test(void) {
    struct ext2_fs_info *info;
    int ret;
    
    printf("EXT2 Superblock Management Simulation\n");
    printf("=====================================\n\n");
    
    /* Create filesystem info */
    info = calloc(1, sizeof(*info));
    if (!info) {
        printf("Failed to allocate memory\n");
        return;
    }
    
    /* Initialize superblock */
    ret = ext2_fill_super(info, NULL, 0);
    if (ret) {
        printf("Failed to initialize superblock: %d\n", ret);
        free(info);
        return;
    }
    
    /* Print initial state */
    printf("Initial filesystem state:\n");
    print_super_info(info);
    print_group_info(info);
    
    /* Simulate some operations */
    printf("Simulating filesystem operations:\n");
    
    /* Remount read-only */
    int flags = MS_RDONLY;
    ext2_remount(info, &flags, NULL);
    
    /* Sync superblock */
    ext2_sync_super(info, 1);
    
    /* Remount read-write */
    flags = 0;
    ext2_remount(info, &flags, NULL);
    
    /* Write superblock */
    ext2_write_super(info);
    
    /* Print final state */
    printf("\nFinal filesystem state:\n");
    print_super_info(info);
    
    /* Cleanup */
    free(info->s_group_desc);
    free(info->s_es);
    pthread_mutex_destroy(&info->s_lock);
    free(info);
}

int main(void) {
    run_super_test();
    return 0;
}
