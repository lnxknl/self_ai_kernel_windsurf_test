#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
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

// XArray Constants
#define XARRAY_CHUNK_SHIFT 6
#define XARRAY_CHUNK_SIZE (1UL << XARRAY_CHUNK_SHIFT)
#define XARRAY_CHUNK_MASK (XARRAY_CHUNK_SIZE - 1)
#define XARRAY_MAX_HEIGHT 8

// Node Types
typedef enum {
    NODE_TYPE_INTERNAL,
    NODE_TYPE_LEAF,
    NODE_TYPE_VALUE
} node_type_t;

// XArray Node
typedef struct xarray_node {
    node_type_t type;
    struct xarray_node *slots[XARRAY_CHUNK_SIZE];
    void *value;
    unsigned long index;
    struct xarray_node *parent;
} xarray_node_t;

// XArray Statistics
typedef struct {
    unsigned long total_nodes;
    unsigned long leaf_nodes;
    unsigned long internal_nodes;
    unsigned long stored_values;
    size_t memory_used;
} xarray_stats_t;

// XArray Configuration
typedef struct {
    bool thread_safe;
    size_t initial_size;
    bool track_stats;
    bool verify_ops;
} xarray_config_t;

// XArray Structure
typedef struct {
    xarray_node_t *root;
    unsigned int height;
    xarray_config_t config;
    xarray_stats_t stats;
    pthread_mutex_t lock;
} xarray_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_node_type_string(node_type_t type);

xarray_t* create_xarray(xarray_config_t config);
void destroy_xarray(xarray_t *xa);

xarray_node_t* create_node(node_type_t type, unsigned long index);
void destroy_node(xarray_node_t *node);

bool xa_store(xarray_t *xa, unsigned long index, void *value);
void* xa_load(xarray_t *xa, unsigned long index);
void* xa_erase(xarray_t *xa, unsigned long index);

void xa_for_each(xarray_t *xa, void (*callback)(unsigned long index, void *value));
void print_xarray_stats(xarray_t *xa);
void demonstrate_xarray(void);

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
        case NODE_TYPE_INTERNAL: return "INTERNAL";
        case NODE_TYPE_LEAF:     return "LEAF";
        case NODE_TYPE_VALUE:    return "VALUE";
        default: return "UNKNOWN";
    }
}

// Create XArray Node
xarray_node_t* create_node(node_type_t type, unsigned long index) {
    xarray_node_t *node = malloc(sizeof(xarray_node_t));
    if (!node) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate XArray node");
        return NULL;
    }

    node->type = type;
    memset(node->slots, 0, sizeof(node->slots));
    node->value = NULL;
    node->index = index;
    node->parent = NULL;

    LOG(LOG_LEVEL_DEBUG, "Created %s node at index %lu",
        get_node_type_string(type), index);

    return node;
}

// Create XArray
xarray_t* create_xarray(xarray_config_t config) {
    xarray_t *xa = malloc(sizeof(xarray_t));
    if (!xa) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate XArray");
        return NULL;
    }

    xa->root = create_node(NODE_TYPE_LEAF, 0);
    if (!xa->root) {
        free(xa);
        return NULL;
    }

    xa->height = 0;
    xa->config = config;
    memset(&xa->stats, 0, sizeof(xarray_stats_t));
    
    if (config.thread_safe) {
        pthread_mutex_init(&xa->lock, NULL);
    }

    xa->stats.total_nodes = 1;
    xa->stats.leaf_nodes = 1;
    xa->stats.memory_used = sizeof(xarray_t) + sizeof(xarray_node_t);

    LOG(LOG_LEVEL_DEBUG, "Created XArray");
    return xa;
}

// Get Node Path for Index
static void get_node_path(
    unsigned long index,
    unsigned int height,
    unsigned long *slots
) {
    unsigned long shift = height * XARRAY_CHUNK_SHIFT;
    for (unsigned int i = 0; i <= height; i++) {
        slots[i] = (index >> shift) & XARRAY_CHUNK_MASK;
        shift -= XARRAY_CHUNK_SHIFT;
    }
}

// Ensure Path to Index Exists
static bool ensure_path(xarray_t *xa, unsigned long index) {
    unsigned long slots[XARRAY_MAX_HEIGHT + 1];
    unsigned int required_height = 0;
    
    // Calculate required height
    unsigned long tmp = index;
    while (tmp >= XARRAY_CHUNK_SIZE) {
        tmp >>= XARRAY_CHUNK_SHIFT;
        required_height++;
    }

    // Grow tree if needed
    while (xa->height < required_height) {
        xarray_node_t *new_root = create_node(NODE_TYPE_INTERNAL, 0);
        if (!new_root) return false;
        
        new_root->slots[0] = xa->root;
        xa->root->parent = new_root;
        xa->root = new_root;
        xa->height++;
        
        xa->stats.total_nodes++;
        xa->stats.internal_nodes++;
        xa->stats.memory_used += sizeof(xarray_node_t);
    }

    // Get path to index
    get_node_path(index, xa->height, slots);

    // Create path
    xarray_node_t *node = xa->root;
    for (unsigned int i = 0; i < xa->height; i++) {
        unsigned long slot = slots[i];
        
        if (!node->slots[slot]) {
            node->slots[slot] = create_node(
                i == xa->height - 1 ? NODE_TYPE_LEAF : NODE_TYPE_INTERNAL,
                index & ~((1UL << ((xa->height - i) * XARRAY_CHUNK_SHIFT)) - 1)
            );
            
            if (!node->slots[slot]) return false;
            
            node->slots[slot]->parent = node;
            xa->stats.total_nodes++;
            
            if (node->slots[slot]->type == NODE_TYPE_LEAF)
                xa->stats.leaf_nodes++;
            else
                xa->stats.internal_nodes++;
                
            xa->stats.memory_used += sizeof(xarray_node_t);
        }
        
        node = node->slots[slot];
    }

    return true;
}

// Store Value in XArray
bool xa_store(xarray_t *xa, unsigned long index, void *value) {
    if (!xa) return false;

    if (xa->config.thread_safe)
        pthread_mutex_lock(&xa->lock);

    bool success = false;
    if (ensure_path(xa, index)) {
        unsigned long slots[XARRAY_MAX_HEIGHT + 1];
        get_node_path(index, xa->height, slots);
        
        xarray_node_t *node = xa->root;
        for (unsigned int i = 0; i <= xa->height; i++) {
            node = node->slots[slots[i]];
        }
        
        if (!node->value && value)
            xa->stats.stored_values++;
        else if (node->value && !value)
            xa->stats.stored_values--;
            
        node->value = value;
        success = true;
    }

    if (xa->config.thread_safe)
        pthread_mutex_unlock(&xa->lock);

    LOG(LOG_LEVEL_DEBUG, "%s value at index %lu",
        value ? "Stored" : "Cleared", index);

    return success;
}

// Load Value from XArray
void* xa_load(xarray_t *xa, unsigned long index) {
    if (!xa) return NULL;

    if (xa->config.thread_safe)
        pthread_mutex_lock(&xa->lock);

    void *value = NULL;
    unsigned long slots[XARRAY_MAX_HEIGHT + 1];
    get_node_path(index, xa->height, slots);
    
    xarray_node_t *node = xa->root;
    for (unsigned int i = 0; i <= xa->height; i++) {
        if (!node || !node->slots[slots[i]]) {
            if (xa->config.thread_safe)
                pthread_mutex_unlock(&xa->lock);
            return NULL;
        }
        node = node->slots[slots[i]];
    }
    
    value = node->value;

    if (xa->config.thread_safe)
        pthread_mutex_unlock(&xa->lock);

    LOG(LOG_LEVEL_DEBUG, "Loaded value at index %lu", index);
    return value;
}

// Erase Value from XArray
void* xa_erase(xarray_t *xa, unsigned long index) {
    void *old_value = xa_load(xa, index);
    if (old_value) {
        xa_store(xa, index, NULL);
    }
    return old_value;
}

// Iterate Over XArray Values
static void xa_for_each_node(
    xarray_node_t *node,
    unsigned int height,
    void (*callback)(unsigned long index, void *value)
) {
    if (!node) return;

    if (height == 0) {
        if (node->value) {
            callback(node->index, node->value);
        }
    } else {
        for (unsigned int i = 0; i < XARRAY_CHUNK_SIZE; i++) {
            if (node->slots[i]) {
                xa_for_each_node(node->slots[i], height - 1, callback);
            }
        }
    }
}

void xa_for_each(xarray_t *xa, void (*callback)(unsigned long index, void *value)) {
    if (!xa || !callback) return;

    if (xa->config.thread_safe)
        pthread_mutex_lock(&xa->lock);

    xa_for_each_node(xa->root, xa->height, callback);

    if (xa->config.thread_safe)
        pthread_mutex_unlock(&xa->lock);
}

// Print XArray Statistics
void print_xarray_stats(xarray_t *xa) {
    if (!xa) return;

    if (xa->config.thread_safe)
        pthread_mutex_lock(&xa->lock);

    printf("\nXArray Statistics:\n");
    printf("----------------\n");
    printf("Total Nodes:     %lu\n", xa->stats.total_nodes);
    printf("Leaf Nodes:      %lu\n", xa->stats.leaf_nodes);
    printf("Internal Nodes:  %lu\n", xa->stats.internal_nodes);
    printf("Stored Values:   %lu\n", xa->stats.stored_values);
    printf("Memory Used:     %zu bytes\n", xa->stats.memory_used);
    printf("Tree Height:     %u\n", xa->height);

    if (xa->config.thread_safe)
        pthread_mutex_unlock(&xa->lock);
}

// Destroy XArray Node
void destroy_node(xarray_node_t *node) {
    if (!node) return;

    for (unsigned int i = 0; i < XARRAY_CHUNK_SIZE; i++) {
        if (node->slots[i]) {
            destroy_node(node->slots[i]);
        }
    }

    free(node);
}

// Destroy XArray
void destroy_xarray(xarray_t *xa) {
    if (!xa) return;

    if (xa->config.thread_safe)
        pthread_mutex_lock(&xa->lock);

    destroy_node(xa->root);

    if (xa->config.thread_safe) {
        pthread_mutex_unlock(&xa->lock);
        pthread_mutex_destroy(&xa->lock);
    }

    free(xa);
    LOG(LOG_LEVEL_DEBUG, "Destroyed XArray");
}

// Value Callback for Demonstration
static void print_value(unsigned long index, void *value) {
    printf("Index %lu: %p\n", index, value);
}

// Demonstrate XArray
void demonstrate_xarray(void) {
    // Create XArray configuration
    xarray_config_t config = {
        .thread_safe = true,
        .initial_size = XARRAY_CHUNK_SIZE,
        .track_stats = true,
        .verify_ops = true
    };

    // Create XArray
    xarray_t *xa = create_xarray(config);
    if (!xa) return;

    // Store some values
    int values[10];
    for (int i = 0; i < 10; i++) {
        values[i] = i * 100;
        xa_store(xa, i * XARRAY_CHUNK_SIZE, &values[i]);
    }

    // Print statistics
    print_xarray_stats(xa);

    // Iterate over values
    printf("\nStored Values:\n");
    xa_for_each(xa, print_value);

    // Erase some values
    for (int i = 0; i < 5; i++) {
        xa_erase(xa, i * XARRAY_CHUNK_SIZE);
    }

    // Print final statistics
    print_xarray_stats(xa);

    // Cleanup
    destroy_xarray(xa);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_xarray();

    return 0;
}
