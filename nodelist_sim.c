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
    NODE_TYPE_INODE,
    NODE_TYPE_DIRENT,
    NODE_TYPE_DATA,
    NODE_TYPE_XATTR,
    NODE_TYPE_PADDING,
    NODE_TYPE_SUMMARY
} node_type_t;

// Node States
typedef enum {
    NODE_STATE_FREE,
    NODE_STATE_ACTIVE,
    NODE_STATE_OBSOLETE,
    NODE_STATE_DIRTY,
    NODE_STATE_PENDING_WRITE
} node_state_t;

// Compression Types
typedef enum {
    COMPRESSION_NONE,
    COMPRESSION_ZLIB,
    COMPRESSION_LZO,
    COMPRESSION_LZMA,
    COMPRESSION_RTIME
} compression_type_t;

// Node Metadata
typedef struct jffs2_node {
    uint64_t node_id;
    node_type_t type;
    node_state_t state;
    
    size_t data_size;
    void *data;
    
    uint64_t inode_number;
    uint64_t version;
    
    compression_type_t compression;
    bool is_checked;
    bool is_dirty;
    
    time_t creation_time;
    time_t last_modified;
} jffs2_node_t;

// Node List Management Statistics
typedef struct {
    unsigned long total_nodes_created;
    unsigned long total_nodes_deleted;
    unsigned long total_nodes_marked_obsolete;
    unsigned long compression_attempts;
    unsigned long compression_successes;
} node_list_stats_t;

// Node Management System
typedef struct {
    jffs2_node_t **nodes;
    size_t total_nodes;
    size_t max_nodes;
    
    node_list_stats_t stats;
    pthread_mutex_t system_lock;
} nodelist_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_node_type_string(node_type_t type);
const char* get_node_state_string(node_state_t state);
const char* get_compression_type_string(compression_type_t type);

nodelist_system_t* create_nodelist_system(size_t max_nodes);
void destroy_nodelist_system(nodelist_system_t *system);

jffs2_node_t* create_node(
    nodelist_system_t *system,
    node_type_t type,
    size_t data_size
);

bool delete_node(
    nodelist_system_t *system, 
    jffs2_node_t *node
);

bool mark_node_obsolete(
    nodelist_system_t *system, 
    jffs2_node_t *node
);

jffs2_node_t* find_node_by_inode(
    nodelist_system_t *system,
    uint64_t inode_number
);

bool compress_node(
    nodelist_system_t *system,
    jffs2_node_t *node,
    compression_type_t compression_type
);

void print_node_list_stats(nodelist_system_t *system);
void demonstrate_nodelist_system();

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
        case NODE_TYPE_INODE:    return "INODE";
        case NODE_TYPE_DIRENT:   return "DIRENT";
        case NODE_TYPE_DATA:     return "DATA";
        case NODE_TYPE_XATTR:    return "XATTR";
        case NODE_TYPE_PADDING:  return "PADDING";
        case NODE_TYPE_SUMMARY:  return "SUMMARY";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Node State String
const char* get_node_state_string(node_state_t state) {
    switch(state) {
        case NODE_STATE_FREE:            return "FREE";
        case NODE_STATE_ACTIVE:          return "ACTIVE";
        case NODE_STATE_OBSOLETE:        return "OBSOLETE";
        case NODE_STATE_DIRTY:           return "DIRTY";
        case NODE_STATE_PENDING_WRITE:   return "PENDING_WRITE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Compression Type String
const char* get_compression_type_string(compression_type_t type) {
    switch(type) {
        case COMPRESSION_NONE:   return "NONE";
        case COMPRESSION_ZLIB:   return "ZLIB";
        case COMPRESSION_LZO:    return "LZO";
        case COMPRESSION_LZMA:   return "LZMA";
        case COMPRESSION_RTIME:  return "RTIME";
        default: return "UNKNOWN";
    }
}

// Create Node List Management System
nodelist_system_t* create_nodelist_system(size_t max_nodes) {
    nodelist_system_t *system = malloc(sizeof(nodelist_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate node list system");
        return NULL;
    }

    system->nodes = malloc(sizeof(jffs2_node_t*) * max_nodes);
    if (!system->nodes) {
        free(system);
        return NULL;
    }

    system->total_nodes = 0;
    system->max_nodes = max_nodes;

    // Reset statistics
    memset(&system->stats, 0, sizeof(node_list_stats_t));

    // Initialize system lock
    pthread_mutex_init(&system->system_lock, NULL);

    LOG(LOG_LEVEL_DEBUG, "Created Node List System with max %zu nodes", max_nodes);

    return system;
}

// Create Node
jffs2_node_t* create_node(
    nodelist_system_t *system,
    node_type_t type,
    size_t data_size
) {
    if (!system || system->total_nodes >= system->max_nodes) {
        LOG(LOG_LEVEL_ERROR, "Cannot create node: system limit reached");
        return NULL;
    }

    jffs2_node_t *node = malloc(sizeof(jffs2_node_t));
    if (!node) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate node");
        return NULL;
    }

    // Allocate node data
    node->data = malloc(data_size);
    if (!node->data) {
        free(node);
        return NULL;
    }

    // Initialize node metadata
    node->node_id = system->total_nodes + 1;
    node->type = type;
    node->state = NODE_STATE_ACTIVE;
    node->data_size = data_size;
    
    node->inode_number = rand();
    node->version = 1;
    
    node->compression = COMPRESSION_NONE;
    node->is_checked = false;
    node->is_dirty = false;
    
    node->creation_time = time(NULL);
    node->last_modified = node->creation_time;

    pthread_mutex_lock(&system->system_lock);
    system->nodes[system->total_nodes] = node;
    system->total_nodes++;
    system->stats.total_nodes_created++;
    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Created %s node, ID %lu, Size %zu", 
        get_node_type_string(type), node->node_id, data_size);

    return node;
}

// Delete Node
bool delete_node(
    nodelist_system_t *system, 
    jffs2_node_t *node
) {
    if (!system || !node) return false;

    pthread_mutex_lock(&system->system_lock);

    // Find and remove the node
    for (size_t i = 0; i < system->total_nodes; i++) {
        if (system->nodes[i] == node) {
            // Free node data
            free(node->data);

            // Shift remaining nodes
            for (size_t j = i; j < system->total_nodes - 1; j++) {
                system->nodes[j] = system->nodes[j + 1];
            }
            system->total_nodes--;

            // Update statistics
            system->stats.total_nodes_deleted++;

            pthread_mutex_unlock(&system->system_lock);

            LOG(LOG_LEVEL_DEBUG, "Deleted node %lu", node->node_id);
            free(node);
            return true;
        }
    }

    pthread_mutex_unlock(&system->system_lock);
    return false;
}

// Mark Node Obsolete
bool mark_node_obsolete(
    nodelist_system_t *system, 
    jffs2_node_t *node
) {
    if (!system || !node) return false;

    pthread_mutex_lock(&system->system_lock);

    node->state = NODE_STATE_OBSOLETE;
    system->stats.total_nodes_marked_obsolete++;

    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Marked node %lu as obsolete", node->node_id);

    return true;
}

// Find Node by Inode Number
jffs2_node_t* find_node_by_inode(
    nodelist_system_t *system,
    uint64_t inode_number
) {
    if (!system) return NULL;

    pthread_mutex_lock(&system->system_lock);

    for (size_t i = 0; i < system->total_nodes; i++) {
        if (system->nodes[i]->inode_number == inode_number) {
            pthread_mutex_unlock(&system->system_lock);
            return system->nodes[i];
        }
    }

    pthread_mutex_unlock(&system->system_lock);
    return NULL;
}

// Compress Node
bool compress_node(
    nodelist_system_t *system,
    jffs2_node_t *node,
    compression_type_t compression_type
) {
    if (!system || !node) return false;

    pthread_mutex_lock(&system->system_lock);

    system->stats.compression_attempts++;

    // Simulate compression (simplified)
    if (node->data_size > 0 && compression_type != COMPRESSION_NONE) {
        // Reduce data size to simulate compression
        size_t compressed_size = node->data_size / 2;
        void *compressed_data = realloc(node->data, compressed_size);
        
        if (compressed_data) {
            node->data = compressed_data;
            node->data_size = compressed_size;
            node->compression = compression_type;
            node->is_dirty = true;

            system->stats.compression_successes++;

            pthread_mutex_unlock(&system->system_lock);

            LOG(LOG_LEVEL_DEBUG, "Compressed node %lu using %s", 
                node->node_id, get_compression_type_string(compression_type));

            return true;
        }
    }

    pthread_mutex_unlock(&system->system_lock);
    return false;
}

// Print Node List Statistics
void print_node_list_stats(nodelist_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->system_lock);

    printf("\nNode List Management Statistics:\n");
    printf("--------------------------------\n");
    printf("Total Nodes Created:        %lu\n", system->stats.total_nodes_created);
    printf("Total Nodes Deleted:        %lu\n", system->stats.total_nodes_deleted);
    printf("Total Nodes Marked Obsolete:%lu\n", system->stats.total_nodes_marked_obsolete);
    printf("Compression Attempts:       %lu\n", system->stats.compression_attempts);
    printf("Compression Successes:      %lu\n", system->stats.compression_successes);
    printf("Current Active Nodes:       %zu\n", system->total_nodes);

    pthread_mutex_unlock(&system->system_lock);
}

// Destroy Node List Management System
void destroy_nodelist_system(nodelist_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->system_lock);

    // Free all nodes
    for (size_t i = 0; i < system->total_nodes; i++) {
        free(system->nodes[i]->data);
        free(system->nodes[i]);
    }

    free(system->nodes);

    pthread_mutex_unlock(&system->system_lock);
    pthread_mutex_destroy(&system->system_lock);

    free(system);
}

// Demonstrate Node List Management System
void demonstrate_nodelist_system() {
    // Create Node List System
    nodelist_system_t *nodelist_system = create_nodelist_system(100);

    // Create Sample Nodes
    jffs2_node_t *nodes[5];
    for (int i = 0; i < 5; i++) {
        nodes[i] = create_node(
            nodelist_system, 
            (node_type_t)(i % 3),  // Cycle through node types
            1024 * (i + 1)  // Varying node sizes
        );
    }

    // Simulate Node Operations
    // Compress Nodes
    for (int i = 0; i < 3; i++) {
        if (nodes[i]) {
            compress_node(
                nodelist_system, 
                nodes[i], 
                (compression_type_t)((i % 4) + 1)  // Cycle through compression types
            );
        }
    }

    // Find Node by Inode Number
    for (int i = 0; i < 5; i++) {
        if (nodes[i]) {
            jffs2_node_t *found_node = find_node_by_inode(
                nodelist_system, 
                nodes[i]->inode_number
            );
            
            if (found_node) {
                LOG(LOG_LEVEL_INFO, "Found node: ID=%lu, Type=%s", 
                    found_node->node_id, 
                    get_node_type_string(found_node->type));
            }
        }
    }

    // Mark Some Nodes Obsolete
    for (int i = 0; i < 2; i++) {
        if (nodes[i]) {
            mark_node_obsolete(nodelist_system, nodes[i]);
        }
    }

    // Delete Nodes
    for (int i = 2; i < 5; i++) {
        if (nodes[i]) {
            delete_node(nodelist_system, nodes[i]);
        }
    }

    // Print Statistics
    print_node_list_stats(nodelist_system);

    // Cleanup
    destroy_nodelist_system(nodelist_system);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_nodelist_system();

    return 0;
}
