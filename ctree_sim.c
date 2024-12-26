#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

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

// Node Types
typedef enum {
    NODE_TYPE_ROOT,
    NODE_TYPE_INTERNAL,
    NODE_TYPE_LEAF,
    NODE_TYPE_TRANSIENT
} node_type_t;

// Operation Types
typedef enum {
    OP_INSERT,
    OP_DELETE,
    OP_UPDATE,
    OP_SEARCH,
    OP_TRAVERSE
} operation_type_t;

// Concurrency Modes
typedef enum {
    CONCURRENCY_NONE,
    CONCURRENCY_READERS_WRITER,
    CONCURRENCY_LOCK_FREE,
    CONCURRENCY_OPTIMISTIC
} concurrency_mode_t;

// Key-Value Pair
typedef struct {
    char *key;
    void *value;
    size_t value_size;
} kvp_t;

// Concurrent Tree Node
typedef struct ctree_node {
    node_type_t type;
    kvp_t *entries;
    size_t entry_count;
    size_t max_entries;
    
    struct ctree_node **children;
    size_t child_count;
    
    pthread_rwlock_t node_lock;
    bool is_dirty;
    uint64_t version;
} ctree_node_t;

// Concurrent Tree Statistics
typedef struct {
    unsigned long total_insertions;
    unsigned long total_deletions;
    unsigned long total_updates;
    unsigned long total_searches;
    unsigned long lock_contentions;
    unsigned long version_conflicts;
} ctree_stats_t;

// Concurrent Tree Configuration
typedef struct {
    size_t max_node_entries;
    concurrency_mode_t concurrency_mode;
    bool enable_versioning;
    bool enable_logging;
} ctree_config_t;

// Concurrent Tree Structure
typedef struct {
    ctree_node_t *root;
    ctree_config_t config;
    ctree_stats_t stats;
    pthread_mutex_t tree_lock;
} ctree_t;

// Operation Context
typedef struct {
    ctree_t *tree;
    operation_type_t op_type;
    kvp_t *data;
    void *result;
    bool success;
} op_context_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_node_type_string(node_type_t type);
const char* get_operation_type_string(operation_type_t type);
const char* get_concurrency_mode_string(concurrency_mode_t mode);

ctree_t* create_ctree(ctree_config_t *config);
void destroy_ctree(ctree_t *tree);

ctree_node_t* create_ctree_node(
    ctree_t *tree, 
    node_type_t type
);

bool insert_entry(
    ctree_t *tree, 
    ctree_node_t *node, 
    kvp_t *entry
);

bool delete_entry(
    ctree_t *tree, 
    ctree_node_t *node, 
    const char *key
);

kvp_t* search_entry(
    ctree_t *tree, 
    ctree_node_t *node, 
    const char *key
);

void traverse_tree(
    ctree_t *tree, 
    ctree_node_t *node, 
    void (*callback)(kvp_t *entry, void *context),
    void *context
);

void print_tree_stats(ctree_t *tree);

// Concurrency and Versioning Functions
void acquire_node_lock(ctree_t *tree, ctree_node_t *node, bool is_write);
void release_node_lock(ctree_t *tree, ctree_node_t *node);
bool validate_version(ctree_node_t *node, uint64_t expected_version);

// Demonstration and Test Functions
void *concurrent_insert_thread(void *arg);
void *concurrent_search_thread(void *arg);
void demonstrate_ctree_system();

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

// Utility Function: Get Node Type String
const char* get_node_type_string(node_type_t type) {
    switch(type) {
        case NODE_TYPE_ROOT:       return "ROOT";
        case NODE_TYPE_INTERNAL:   return "INTERNAL";
        case NODE_TYPE_LEAF:       return "LEAF";
        case NODE_TYPE_TRANSIENT:  return "TRANSIENT";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Operation Type String
const char* get_operation_type_string(operation_type_t type) {
    switch(type) {
        case OP_INSERT:   return "INSERT";
        case OP_DELETE:   return "DELETE";
        case OP_UPDATE:   return "UPDATE";
        case OP_SEARCH:   return "SEARCH";
        case OP_TRAVERSE: return "TRAVERSE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Concurrency Mode String
const char* get_concurrency_mode_string(concurrency_mode_t mode) {
    switch(mode) {
        case CONCURRENCY_NONE:             return "NONE";
        case CONCURRENCY_READERS_WRITER:   return "READERS_WRITER";
        case CONCURRENCY_LOCK_FREE:        return "LOCK_FREE";
        case CONCURRENCY_OPTIMISTIC:       return "OPTIMISTIC";
        default: return "UNKNOWN";
    }
}

// Create Concurrent Tree
ctree_t* create_ctree(ctree_config_t *config) {
    ctree_t *tree = malloc(sizeof(ctree_t));
    if (!tree) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate concurrent tree");
        return NULL;
    }

    // Set configuration
    if (config) {
        tree->config = *config;
    } else {
        // Default configuration
        tree->config.max_node_entries = 16;
        tree->config.concurrency_mode = CONCURRENCY_READERS_WRITER;
        tree->config.enable_versioning = true;
        tree->config.enable_logging = true;
    }

    // Initialize root node
    tree->root = create_ctree_node(tree, NODE_TYPE_ROOT);
    if (!tree->root) {
        free(tree);
        return NULL;
    }

    // Initialize tree lock
    pthread_mutex_init(&tree->tree_lock, NULL);

    // Reset statistics
    memset(&tree->stats, 0, sizeof(ctree_stats_t));

    LOG(LOG_LEVEL_DEBUG, "Created Concurrent Tree with mode %s", 
        get_concurrency_mode_string(tree->config.concurrency_mode));

    return tree;
}

// Create Concurrent Tree Node
ctree_node_t* create_ctree_node(
    ctree_t *tree, 
    node_type_t type
) {
    ctree_node_t *node = malloc(sizeof(ctree_node_t));
    if (!node) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate tree node");
        return NULL;
    }

    node->type = type;
    node->entry_count = 0;
    node->max_entries = tree->config.max_node_entries;
    
    // Allocate entries array
    node->entries = malloc(sizeof(kvp_t) * node->max_entries);
    if (!node->entries) {
        free(node);
        return NULL;
    }

    // Allocate children array
    node->children = malloc(sizeof(ctree_node_t*) * (node->max_entries + 1));
    if (!node->children) {
        free(node->entries);
        free(node);
        return NULL;
    }

    // Initialize node lock
    pthread_rwlock_init(&node->node_lock, NULL);

    node->is_dirty = false;
    node->version = 0;

    return node;
}

// Insert Entry
bool insert_entry(
    ctree_t *tree, 
    ctree_node_t *node, 
    kvp_t *entry
) {
    if (!tree || !node || !entry) return false;

    // Acquire write lock
    acquire_node_lock(tree, node, true);

    // Check if node is full
    if (node->entry_count >= node->max_entries) {
        LOG(LOG_LEVEL_WARN, "Node is full, cannot insert");
        release_node_lock(tree, node);
        return false;
    }

    // Check for existing key
    for (size_t i = 0; i < node->entry_count; i++) {
        if (strcmp(node->entries[i].key, entry->key) == 0) {
            // Update existing entry
            free(node->entries[i].value);
            node->entries[i].value = malloc(entry->value_size);
            memcpy(node->entries[i].value, entry->value, entry->value_size);
            node->entries[i].value_size = entry->value_size;
            
            node->is_dirty = true;
            node->version++;

            tree->stats.total_updates++;
            
            release_node_lock(tree, node);
            return true;
        }
    }

    // Insert new entry
    node->entries[node->entry_count] = *entry;
    node->entry_count++;
    node->is_dirty = true;
    node->version++;

    tree->stats.total_insertions++;

    release_node_lock(tree, node);

    LOG(LOG_LEVEL_DEBUG, "Inserted entry with key %s", entry->key);
    return true;
}

// Delete Entry
bool delete_entry(
    ctree_t *tree, 
    ctree_node_t *node, 
    const char *key
) {
    if (!tree || !node || !key) return false;

    // Acquire write lock
    acquire_node_lock(tree, node, true);

    // Find and delete entry
    for (size_t i = 0; i < node->entry_count; i++) {
        if (strcmp(node->entries[i].key, key) == 0) {
            // Free memory for key and value
            free(node->entries[i].key);
            free(node->entries[i].value);

            // Shift remaining entries
            for (size_t j = i; j < node->entry_count - 1; j++) {
                node->entries[j] = node->entries[j + 1];
            }

            node->entry_count--;
            node->is_dirty = true;
            node->version++;

            tree->stats.total_deletions++;

            release_node_lock(tree, node);

            LOG(LOG_LEVEL_DEBUG, "Deleted entry with key %s", key);
            return true;
        }
    }

    release_node_lock(tree, node);
    return false;
}

// Search Entry
kvp_t* search_entry(
    ctree_t *tree, 
    ctree_node_t *node, 
    const char *key
) {
    if (!tree || !node || !key) return NULL;

    // Acquire read lock
    acquire_node_lock(tree, node, false);

    // Search for entry
    for (size_t i = 0; i < node->entry_count; i++) {
        if (strcmp(node->entries[i].key, key) == 0) {
            tree->stats.total_searches++;
            release_node_lock(tree, node);
            return &node->entries[i];
        }
    }

    release_node_lock(tree, node);
    return NULL;
}

// Traverse Tree
void traverse_tree(
    ctree_t *tree, 
    ctree_node_t *node, 
    void (*callback)(kvp_t *entry, void *context),
    void *context
) {
    if (!tree || !node || !callback) return;

    // Acquire read lock
    acquire_node_lock(tree, node, false);

    // Traverse entries
    for (size_t i = 0; i < node->entry_count; i++) {
        callback(&node->entries[i], context);
    }

    release_node_lock(tree, node);
}

// Acquire Node Lock
void acquire_node_lock(ctree_t *tree, ctree_node_t *node, bool is_write) {
    if (tree->config.concurrency_mode == CONCURRENCY_READERS_WRITER) {
        if (is_write) {
            pthread_rwlock_wrlock(&node->node_lock);
        } else {
            pthread_rwlock_rdlock(&node->node_lock);
        }
    } else if (tree->config.concurrency_mode == CONCURRENCY_NONE) {
        pthread_mutex_lock(&tree->tree_lock);
    }
}

// Release Node Lock
void release_node_lock(ctree_t *tree, ctree_node_t *node) {
    if (tree->config.concurrency_mode == CONCURRENCY_READERS_WRITER) {
        pthread_rwlock_unlock(&node->node_lock);
    } else if (tree->config.concurrency_mode == CONCURRENCY_NONE) {
        pthread_mutex_unlock(&tree->tree_lock);
    }
}

// Validate Version
bool validate_version(ctree_node_t *node, uint64_t expected_version) {
    return node->version == expected_version;
}

// Print Tree Statistics
void print_tree_stats(ctree_t *tree) {
    if (!tree) return;

    printf("\nConcurrent Tree Statistics:\n");
    printf("-------------------------\n");
    printf("Total Insertions:     %lu\n", tree->stats.total_insertions);
    printf("Total Deletions:      %lu\n", tree->stats.total_deletions);
    printf("Total Updates:        %lu\n", tree->stats.total_updates);
    printf("Total Searches:       %lu\n", tree->stats.total_searches);
    printf("Lock Contentions:     %lu\n", tree->stats.lock_contentions);
    printf("Version Conflicts:    %lu\n", tree->stats.version_conflicts);
}

// Destroy Concurrent Tree
void destroy_ctree(ctree_t *tree) {
    if (!tree) return;

    // Recursively free nodes (simplified for demonstration)
    free(tree->root->entries);
    free(tree->root->children);
    pthread_rwlock_destroy(&tree->root->node_lock);
    free(tree->root);

    pthread_mutex_destroy(&tree->tree_lock);
    free(tree);
}

// Concurrent Insert Thread
void *concurrent_insert_thread(void *arg) {
    op_context_t *context = (op_context_t*)arg;
    
    context->success = insert_entry(
        context->tree, 
        context->tree->root, 
        context->data
    );

    return NULL;
}

// Concurrent Search Thread
void *concurrent_search_thread(void *arg) {
    op_context_t *context = (op_context_t*)arg;
    
    context->result = search_entry(
        context->tree, 
        context->tree->root, 
        context->data->key
    );

    context->success = (context->result != NULL);

    return NULL;
}

// Demonstrate Concurrent Tree
void demonstrate_ctree_system() {
    // Create Concurrent Tree Configuration
    ctree_config_t config = {
        .max_node_entries = 16,
        .concurrency_mode = CONCURRENCY_READERS_WRITER,
        .enable_versioning = true,
        .enable_logging = true
    };

    // Create Concurrent Tree
    ctree_t *ctree = create_ctree(&config);

    // Prepare Concurrent Threads
    pthread_t insert_threads[4];
    op_context_t insert_contexts[4];

    // Prepare Sample Entries
    kvp_t entries[] = {
        {strdup("key1"), strdup("value1"), 6},
        {strdup("key2"), strdup("value2"), 6},
        {strdup("key3"), strdup("value3"), 6},
        {strdup("key4"), strdup("value4"), 6}
    };

    // Concurrent Insertion
    for (int i = 0; i < 4; i++) {
        insert_contexts[i].tree = ctree;
        insert_contexts[i].op_type = OP_INSERT;
        insert_contexts[i].data = &entries[i];
        
        pthread_create(
            &insert_threads[i], 
            NULL, 
            concurrent_insert_thread, 
            &insert_contexts[i]
        );
    }

    // Wait for Insertion Threads
    for (int i = 0; i < 4; i++) {
        pthread_join(insert_threads[i], NULL);
    }

    // Concurrent Search
    pthread_t search_threads[4];
    op_context_t search_contexts[4];

    for (int i = 0; i < 4; i++) {
        search_contexts[i].tree = ctree;
        search_contexts[i].op_type = OP_SEARCH;
        search_contexts[i].data = &entries[i];
        
        pthread_create(
            &search_threads[i], 
            NULL, 
            concurrent_search_thread, 
            &search_contexts[i]
        );
    }

    // Wait for Search Threads
    for (int i = 0; i < 4; i++) {
        pthread_join(search_threads[i], NULL);
        
        if (search_contexts[i].success) {
            kvp_t *result = (kvp_t*)search_contexts[i].result;
            LOG(LOG_LEVEL_INFO, "Found: Key=%s, Value=%s", 
                result->key, (char*)result->value);
        }
    }

    // Print Statistics
    print_tree_stats(ctree);

    // Cleanup
    for (int i = 0; i < 4; i++) {
        free(entries[i].key);
        free(entries[i].value);
    }

    destroy_ctree(ctree);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_ctree_system();

    return 0;
}
