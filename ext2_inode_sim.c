#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ext2_inode_sim.h"

/* Create a new inode */
struct ext2_inode_sim *ext2_create_inode(void) {
    struct ext2_inode_sim *inode = calloc(1, sizeof(struct ext2_inode_sim));
    if (!inode) {
        return NULL;
    }

    /* Initialize with default values */
    time_t current_time = time(NULL);
    inode->i_atime = current_time;
    inode->i_ctime = current_time;
    inode->i_mtime = current_time;
    inode->i_links_count = 1;
    
    return inode;
}

/* Free an inode */
void ext2_free_inode(struct ext2_inode_sim *inode) {
    if (inode) {
        free(inode);
    }
}

/* Test if an inode is a fast symlink */
bool ext2_inode_is_fast_symlink(struct ext2_inode_sim *inode) {
    if (!inode) {
        return false;
    }

    int ea_blocks = inode->i_file_acl ? (EXT2_BLOCK_SIZE >> 9) : 0;
    return (S_ISLNK(inode->i_mode) && inode->i_blocks - ea_blocks == 0);
}

/* Write inode data to a file (simulation) */
int ext2_write_inode(struct ext2_inode_sim *inode, const char *path) {
    if (!inode || !path) {
        return -1;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }

    size_t written = fwrite(inode, sizeof(struct ext2_inode_sim), 1, fp);
    fclose(fp);

    return (written == 1) ? 0 : -1;
}

/* Read inode data from a file (simulation) */
int ext2_read_inode(struct ext2_inode_sim *inode, const char *path) {
    if (!inode || !path) {
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }

    size_t read = fread(inode, sizeof(struct ext2_inode_sim), 1, fp);
    fclose(fp);

    return (read == 1) ? 0 : -1;
}

/* Set inode flags */
void ext2_set_inode_flags(struct ext2_inode_sim *inode, uint32_t flags) {
    if (!inode) {
        return;
    }
    inode->i_flags = flags;
}

/* Print inode information */
void ext2_print_inode(struct ext2_inode_sim *inode) {
    if (!inode) {
        printf("NULL inode\n");
        return;
    }

    printf("Inode Information:\n");
    printf("Mode: %o\n", inode->i_mode);
    printf("Size: %u bytes\n", inode->i_size);
    printf("UID/GID: %u/%u\n", inode->i_uid, inode->i_gid);
    printf("Links: %u\n", inode->i_links_count);
    printf("Blocks: %u\n", inode->i_blocks);
    printf("Flags: 0x%x\n", inode->i_flags);
    printf("Access Time: %u\n", inode->i_atime);
    printf("Creation Time: %u\n", inode->i_ctime);
    printf("Modification Time: %u\n", inode->i_mtime);
    
    if (ext2_inode_is_fast_symlink(inode)) {
        printf("Fast Symlink Target: %s\n", inode->i_symlink);
    }
}

/* Example usage */
int main(void) {
    /* Create a new inode */
    struct ext2_inode_sim *inode = ext2_create_inode();
    if (!inode) {
        printf("Failed to create inode\n");
        return 1;
    }

    /* Set up a symlink inode */
    inode->i_mode = S_IFLNK | 0777;
    inode->i_size = 10;
    strcpy(inode->i_symlink, "target.txt");
    ext2_set_inode_flags(inode, EXT2_SYMLINK_FL);

    /* Print initial inode info */
    printf("Initial inode state:\n");
    ext2_print_inode(inode);

    /* Write to file */
    const char *test_file = "test_inode.dat";
    if (ext2_write_inode(inode, test_file) != 0) {
        printf("Failed to write inode\n");
        ext2_free_inode(inode);
        return 1;
    }

    /* Read back from file */
    struct ext2_inode_sim *read_inode = ext2_create_inode();
    if (!read_inode) {
        printf("Failed to create read inode\n");
        ext2_free_inode(inode);
        return 1;
    }

    if (ext2_read_inode(read_inode, test_file) != 0) {
        printf("Failed to read inode\n");
        ext2_free_inode(inode);
        ext2_free_inode(read_inode);
        return 1;
    }

    /* Print read inode info */
    printf("\nRead inode state:\n");
    ext2_print_inode(read_inode);

    /* Test fast symlink detection */
    printf("\nIs fast symlink: %s\n", 
           ext2_inode_is_fast_symlink(read_inode) ? "yes" : "no");

    /* Cleanup */
    ext2_free_inode(inode);
    ext2_free_inode(read_inode);
    remove(test_file);

    return 0;
}
