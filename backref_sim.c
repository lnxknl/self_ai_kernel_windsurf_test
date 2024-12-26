#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

// Logging Macros
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

#define LOG(level, ...) do { \
    if (level >= current_log_level) { \
        printf("[%s] ", get_log_level_string(level)); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while(0)

// Global Log Level
static int current_log_level = LOG_LEVEL_INFO;

// Block Types
typedef enum {
    BLOCK_TYPE_DATA,
    BLOCK_TYPE_METADATA,
    BLOCK_TYPE_EXTENT,
    BLOCK_TYPE_TREE,
    BLOCK_TYPE_FREESPACE
} block_type_t;

// Reference Types
typedef enum {
    REF_TYPE_DIRECT,
    REF_TYPE_SHARED,
    REF_TYPE_COPY_ON_WRITE,
    REF_TYPE_INLINE,
    REF_TYPE_SNAPSHOT
} ref_type_t;

// Block Reference Structure
typedef struct block_ref {
    uint64_t block_id;
    uint64_t parent_id;
    block_type_t block_type;
    ref_type_t ref_type;
    size_t ref_count;
    bool is_pinned;
} block_ref_t;

// Reference Tree Node
typedef struct ref_tree_node {
    block_ref_t ref;
    struct ref_tree_node *left;
    struct ref_tree_node *right;
    int height;
} ref_tree_node_t;

// Backreference Statistics
typedef struct {
    unsigned long total_refs_created;
    unsigned long total_refs_deleted;
    unsigned long shared_refs;
    unsigned long cow_refs;
    unsigned long pin_operations;
    unsigned long unpin_operations;
} backref_stats_t;

// Backreference Management System
typedef struct {
    ref_tree_node_t *ref_tree;
    size_t max_refs;
    size_t current_refs;
    backref_stats_t stats;
} backref_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_block_type_string(block_type_t type);
const char* get_ref_type_string(ref_type_t type);

backref_system_t* create_backref_system(size_t max_refs);
void destroy_backref_system(backref_system_t *system);

int get_node_height(ref_tree_node_t *node);
int get_balance_factor(ref_tree_node_t *node);

ref_tree_node_t* rotate_right(ref_tree_node_t *y);
ref_tree_node_t* rotate_left(ref_tree_node_t *x);

ref_tree_node_t* insert_ref_node(
    ref_tree_node_t *root, 
    block_ref_t *ref
);

ref_tree_node_t* delete_ref_node(
    ref_tree_node_t *root, 
    uint64_t block_id
);

block_ref_t* find_ref_node(
    ref_tree_node_t *root, 
    uint64_t block_id
);

bool pin_block_reference(
    backref_system_t *system, 
    uint64_t block_id
);

bool unpin_block_reference(
    backref_system_t *system, 
    uint64_t block_id
);

void increase_ref_count(
    backref_system_t *system, 
    uint64_t block_id
);

void decrease_ref_count(
    backref_system_t *system, 
    uint64_t block_id
);

void print_backref_stats(backref_system_t *system);
void demonstrate_backref_system();

// Utility Function: Get Log Level String
const char* get_log_level_string(int level) {
    switch(level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Block Type String
const char* get_block_type_string(block_type_t type) {
    switch(type) {
        case BLOCK_TYPE_DATA:      return "DATA";
        case BLOCK_TYPE_METADATA:  return "METADATA";
        case BLOCK_TYPE_EXTENT:    return "EXTENT";
        case BLOCK_TYPE_TREE:      return "TREE";
        case BLOCK_TYPE_FREESPACE: return "FREESPACE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Reference Type String
const char* get_ref_type_string(ref_type_t type) {
    switch(type) {
        case REF_TYPE_DIRECT:          return "DIRECT";
        case REF_TYPE_SHARED:          return "SHARED";
        case REF_TYPE_COPY_ON_WRITE:   return "COPY_ON_WRITE";
        case REF_TYPE_INLINE:          return "INLINE";
        case REF_TYPE_SNAPSHOT:        return "SNAPSHOT";
        default: return "UNKNOWN";
    }
}

// Create Backreference System
backref_system_t* create_backref_system(size_t max_refs) {
    backref_system_t *system = malloc(sizeof(backref_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate backref system");
        return NULL;
    }

    system->ref_tree = NULL;
    system->max_refs = max_refs;
    system->current_refs = 0;

    // Reset statistics
    memset(&system->stats, 0, sizeof(backref_stats_t));

    return system;
}

// Get Node Height
int get_node_height(ref_tree_node_t *node) {
    return node ? node->height : 0;
}

// Get Balance Factor
int get_balance_factor(ref_tree_node_t *node) {
    return node ? get_node_height(node->left) - get_node_height(node->right) : 0;
}

// Rotate Right
ref_tree_node_t* rotate_right(ref_tree_node_t *y) {
    ref_tree_node_t *x = y->left;
    ref_tree_node_t *T2 = x->right;

    x->right = y;
    y->left = T2;

    y->height = 1 + (get_node_height(y->left) > get_node_height(y->right) ? 
                     get_node_height(y->left) : get_node_height(y->right));
    x->height = 1 + (get_node_height(x->left) > get_node_height(x->right) ? 
                     get_node_height(x->left) : get_node_height(x->right));

    return x;
}

// Rotate Left
ref_tree_node_t* rotate_left(ref_tree_node_t *x) {
    ref_tree_node_t *y = x->right;
    ref_tree_node_t *T2 = y->left;

    y->left = x;
    x->right = T2;

    x->height = 1 + (get_node_height(x->left) > get_node_height(x->right) ? 
                     get_node_height(x->left) : get_node_height(x->right));
    y->height = 1 + (get_node_height(y->left) > get_node_height(y->right) ? 
                     get_node_height(y->left) : get_node_height(y->right));

    return y;
}

// Insert Reference Node
ref_tree_node_t* insert_ref_node(
    ref_tree_node_t *root, 
    block_ref_t *ref
) {
    // Standard BST insertion
    if (!root) {
        ref_tree_node_t *new_node = malloc(sizeof(ref_tree_node_t));
        if (!new_node) {
            LOG(LOG_LEVEL_ERROR, "Failed to allocate ref tree node");
            return NULL;
        }
        new_node->ref = *ref;
        new_node->left = NULL;
        new_node->right = NULL;
        new_node->height = 1;
        return new_node;
    }

    if (ref->block_id < root->ref.block_id) {
        root->left = insert_ref_node(root->left, ref);
    } else if (ref->block_id > root->ref.block_id) {
        root->right = insert_ref_node(root->right, ref);
    } else {
        // Duplicate block ID, update existing node
        root->ref = *ref;
        return root;
    }

    // Update height
    root->height = 1 + (get_node_height(root->left) > get_node_height(root->right) ? 
                        get_node_height(root->left) : get_node_height(root->right));

    // Balance the tree
    int balance = get_balance_factor(root);

    // Left Left Case
    if (balance > 1 && ref->block_id < root->left->ref.block_id) {
        return rotate_right(root);
    }

    // Right Right Case
    if (balance < -1 && ref->block_id > root->right->ref.block_id) {
        return rotate_left(root);
    }

    // Left Right Case
    if (balance > 1 && ref->block_id > root->left->ref.block_id) {
        root->left = rotate_left(root->left);
        return rotate_right(root);
    }

    // Right Left Case
    if (balance < -1 && ref->block_id < root->right->ref.block_id) {
        root->right = rotate_right(root->right);
        return rotate_left(root);
    }

    return root;
}

// Find Reference Node
block_ref_t* find_ref_node(
    ref_tree_node_t *root, 
    uint64_t block_id
) {
    if (!root) return NULL;

    if (block_id == root->ref.block_id) {
        return &root->ref;
    }

    if (block_id < root->ref.block_id) {
        return find_ref_node(root->left, block_id);
    }

    return find_ref_node(root->right, block_id);
}

// Pin Block Reference
bool pin_block_reference(
    backref_system_t *system, 
    uint64_t block_id
) {
    if (!system) return false;

    block_ref_t *ref = find_ref_node(system->ref_tree, block_id);
    if (!ref) {
        LOG(LOG_LEVEL_WARN, "Cannot pin block: Block %lu not found", block_id);
        return false;
    }

    ref->is_pinned = true;
    system->stats.pin_operations++;

    LOG(LOG_LEVEL_DEBUG, "Pinned block %lu", block_id);
    return true;
}

// Unpin Block Reference
bool unpin_block_reference(
    backref_system_t *system, 
    uint64_t block_id
) {
    if (!system) return false;

    block_ref_t *ref = find_ref_node(system->ref_tree, block_id);
    if (!ref) {
        LOG(LOG_LEVEL_WARN, "Cannot unpin block: Block %lu not found", block_id);
        return false;
    }

    ref->is_pinned = false;
    system->stats.unpin_operations++;

    LOG(LOG_LEVEL_DEBUG, "Unpinned block %lu", block_id);
    return true;
}

// Increase Reference Count
void increase_ref_count(
    backref_system_t *system, 
    uint64_t block_id
) {
    if (!system) return;

    block_ref_t *ref = find_ref_node(system->ref_tree, block_id);
    if (!ref) {
        LOG(LOG_LEVEL_WARN, "Cannot increase ref count: Block %lu not found", block_id);
        return;
    }

    ref->ref_count++;

    // Update system statistics
    if (ref->ref_type == REF_TYPE_SHARED) {
        system->stats.shared_refs++;
    } else if (ref->ref_type == REF_TYPE_COPY_ON_WRITE) {
        system->stats.cow_refs++;
    }

    LOG(LOG_LEVEL_DEBUG, "Increased ref count for block %lu to %zu", 
        block_id, ref->ref_count);
}

// Decrease Reference Count
void decrease_ref_count(
    backref_system_t *system, 
    uint64_t block_id
) {
    if (!system) return;

    block_ref_t *ref = find_ref_node(system->ref_tree, block_id);
    if (!ref) {
        LOG(LOG_LEVEL_WARN, "Cannot decrease ref count: Block %lu not found", block_id);
        return;
    }

    if (ref->ref_count > 0) {
        ref->ref_count--;
    }

    LOG(LOG_LEVEL_DEBUG, "Decreased ref count for block %lu to %zu", 
        block_id, ref->ref_count);
}

// Print Backreference Statistics
void print_backref_stats(backref_system_t *system) {
    if (!system) return;

    printf("\nBackreference System Statistics:\n");
    printf("--------------------------------\n");
    printf("Total References Created:  %lu\n", system->stats.total_refs_created);
    printf("Total References Deleted:  %lu\n", system->stats.total_refs_deleted);
    printf("Shared References:         %lu\n", system->stats.shared_refs);
    printf("Copy-on-Write References:  %lu\n", system->stats.cow_refs);
    printf("Pin Operations:            %lu\n", system->stats.pin_operations);
    printf("Unpin Operations:          %lu\n", system->stats.unpin_operations);
}

// Destroy Reference Tree
void destroy_ref_tree(ref_tree_node_t *node) {
    if (!node) return;

    destroy_ref_tree(node->left);
    destroy_ref_tree(node->right);
    free(node);
}

// Destroy Backreference System
void destroy_backref_system(backref_system_t *system) {
    if (!system) return;

    // Destroy reference tree
    destroy_ref_tree(system->ref_tree);

    free(system);
}

// Demonstrate Backreference System
void demonstrate_backref_system() {
    // Create Backreference System
    backref_system_t *backref_system = create_backref_system(1024);

    // Create Sample Block References
    block_ref_t refs[] = {
        {
            .block_id = 1000,
            .parent_id = 0,
            .block_type = BLOCK_TYPE_DATA,
            .ref_type = REF_TYPE_DIRECT,
            .ref_count = 1,
            .is_pinned = false
        },
        {
            .block_id = 2000,
            .parent_id = 1000,
            .block_type = BLOCK_TYPE_METADATA,
            .ref_type = REF_TYPE_SHARED,
            .ref_count = 2,
            .is_pinned = false
        },
        {
            .block_id = 3000,
            .parent_id = 2000,
            .block_type = BLOCK_TYPE_EXTENT,
            .ref_type = REF_TYPE_COPY_ON_WRITE,
            .ref_count = 1,
            .is_pinned = false
        }
    };

    // Insert References
    for (size_t i = 0; i < sizeof(refs)/sizeof(refs[0]); i++) {
        backref_system->ref_tree = insert_ref_node(
            backref_system->ref_tree, 
            &refs[i]
        );
        backref_system->stats.total_refs_created++;
    }

    // Demonstrate Reference Operations
    increase_ref_count(backref_system, 2000);
    decrease_ref_count(backref_system, 2000);

    pin_block_reference(backref_system, 3000);
    unpin_block_reference(backref_system, 3000);

    // Find and Print a Reference
    block_ref_t *found_ref = find_ref_node(
        backref_system->ref_tree, 
        2000
    );

    if (found_ref) {
        LOG(LOG_LEVEL_INFO, "Found Reference: Block %lu, Type %s, Ref Count %zu", 
            found_ref->block_id, 
            get_block_type_string(found_ref->block_type),
            found_ref->ref_count);
    }

    // Print Statistics
    print_backref_stats(backref_system);

    // Cleanup
    destroy_backref_system(backref_system);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_backref_system();

    return 0;
}
