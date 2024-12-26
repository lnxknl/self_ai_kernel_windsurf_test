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

// Block Types
typedef enum {
    BLOCK_TYPE_DATA,
    BLOCK_TYPE_METADATA,
    BLOCK_TYPE_EXTENT,
    BLOCK_TYPE_TREE,
    BLOCK_TYPE_INLINE
} block_type_t;

// Relocation Strategies
typedef enum {
    RELOC_STRATEGY_SEQUENTIAL,
    RELOC_STRATEGY_RANDOM,
    RELOC_STRATEGY_FRAGMENTATION_AWARE,
    RELOC_STRATEGY_PRIORITY_BASED,
    RELOC_STRATEGY_COPY_ON_WRITE
} reloc_strategy_t;

// Block State
typedef enum {
    BLOCK_STATE_FREE,
    BLOCK_STATE_ALLOCATED,
    BLOCK_STATE_DIRTY,
    BLOCK_STATE_RELOCATING,
    BLOCK_STATE_RESERVED
} block_state_t;

// Block Metadata
typedef struct block_metadata {
    uint64_t block_id;
    block_type_t type;
    block_state_t state;
    size_t size;
    void *data;
    
    uint64_t original_location;
    uint64_t new_location;
    
    bool is_pinned;
    bool is_compressed;
} block_metadata_t;

// Relocation Operation
typedef struct reloc_operation {
    block_metadata_t *block;
    reloc_strategy_t strategy;
    bool is_completed;
    time_t start_time;
    time_t end_time;
} reloc_operation_t;

// Block Management Statistics
typedef struct {
    unsigned long total_blocks_created;
    unsigned long total_blocks_relocated;
    unsigned long total_blocks_freed;
    unsigned long relocation_failures;
    unsigned long fragmentation_events;
} block_stats_t;

// Relocation Management System
typedef struct {
    block_metadata_t **blocks;
    size_t total_blocks;
    size_t max_blocks;
    
    reloc_operation_t **reloc_queue;
    size_t reloc_queue_size;
    size_t max_reloc_queue;
    
    block_stats_t stats;
    pthread_mutex_t system_lock;
} relocation_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_block_type_string(block_type_t type);
const char* get_block_state_string(block_state_t state);
const char* get_reloc_strategy_string(reloc_strategy_t strategy);

relocation_system_t* create_relocation_system(size_t max_blocks, size_t max_reloc_queue);
void destroy_relocation_system(relocation_system_t *system);

block_metadata_t* create_block(
    relocation_system_t *system,
    block_type_t type,
    size_t size
);

bool free_block(
    relocation_system_t *system, 
    block_metadata_t *block
);

reloc_operation_t* initiate_block_relocation(
    relocation_system_t *system,
    block_metadata_t *block,
    reloc_strategy_t strategy
);

bool complete_block_relocation(
    relocation_system_t *system,
    reloc_operation_t *reloc_op
);

void print_block_stats(relocation_system_t *system);
void demonstrate_relocation_system();

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
        case BLOCK_TYPE_INLINE:    return "INLINE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Block State String
const char* get_block_state_string(block_state_t state) {
    switch(state) {
        case BLOCK_STATE_FREE:         return "FREE";
        case BLOCK_STATE_ALLOCATED:    return "ALLOCATED";
        case BLOCK_STATE_DIRTY:        return "DIRTY";
        case BLOCK_STATE_RELOCATING:   return "RELOCATING";
        case BLOCK_STATE_RESERVED:     return "RESERVED";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Relocation Strategy String
const char* get_reloc_strategy_string(reloc_strategy_t strategy) {
    switch(strategy) {
        case RELOC_STRATEGY_SEQUENTIAL:            return "SEQUENTIAL";
        case RELOC_STRATEGY_RANDOM:                return "RANDOM";
        case RELOC_STRATEGY_FRAGMENTATION_AWARE:   return "FRAGMENTATION_AWARE";
        case RELOC_STRATEGY_PRIORITY_BASED:        return "PRIORITY_BASED";
        case RELOC_STRATEGY_COPY_ON_WRITE:         return "COPY_ON_WRITE";
        default: return "UNKNOWN";
    }
}

// Create Relocation System
relocation_system_t* create_relocation_system(
    size_t max_blocks, 
    size_t max_reloc_queue
) {
    relocation_system_t *system = malloc(sizeof(relocation_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate relocation system");
        return NULL;
    }

    system->blocks = malloc(sizeof(block_metadata_t*) * max_blocks);
    system->reloc_queue = malloc(sizeof(reloc_operation_t*) * max_reloc_queue);

    if (!system->blocks || !system->reloc_queue) {
        free(system->blocks);
        free(system->reloc_queue);
        free(system);
        return NULL;
    }

    system->total_blocks = 0;
    system->max_blocks = max_blocks;
    system->reloc_queue_size = 0;
    system->max_reloc_queue = max_reloc_queue;

    memset(&system->stats, 0, sizeof(block_stats_t));
    pthread_mutex_init(&system->system_lock, NULL);

    return system;
}

// Create Block
block_metadata_t* create_block(
    relocation_system_t *system,
    block_type_t type,
    size_t size
) {
    if (!system || system->total_blocks >= system->max_blocks) {
        LOG(LOG_LEVEL_ERROR, "Cannot create block: system limit reached");
        return NULL;
    }

    block_metadata_t *block = malloc(sizeof(block_metadata_t));
    if (!block) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate block metadata");
        return NULL;
    }

    block->block_id = system->total_blocks + 1;
    block->type = type;
    block->state = BLOCK_STATE_ALLOCATED;
    block->size = size;
    block->data = malloc(size);
    
    if (!block->data) {
        free(block);
        return NULL;
    }

    block->original_location = 0;
    block->new_location = 0;
    block->is_pinned = false;
    block->is_compressed = false;

    pthread_mutex_lock(&system->system_lock);
    system->blocks[system->total_blocks] = block;
    system->total_blocks++;
    system->stats.total_blocks_created++;
    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Created block %lu of type %s, size %zu", 
        block->block_id, get_block_type_string(type), size);

    return block;
}

// Free Block
bool free_block(
    relocation_system_t *system, 
    block_metadata_t *block
) {
    if (!system || !block) return false;

    pthread_mutex_lock(&system->system_lock);

    if (block->is_pinned) {
        LOG(LOG_LEVEL_WARN, "Cannot free pinned block %lu", block->block_id);
        pthread_mutex_unlock(&system->system_lock);
        return false;
    }

    // Remove from blocks array
    for (size_t i = 0; i < system->total_blocks; i++) {
        if (system->blocks[i] == block) {
            // Shift remaining blocks
            for (size_t j = i; j < system->total_blocks - 1; j++) {
                system->blocks[j] = system->blocks[j + 1];
            }
            system->total_blocks--;
            break;
        }
    }

    // Free block data
    free(block->data);
    free(block);

    system->stats.total_blocks_freed++;

    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Freed block %lu", block->block_id);

    return true;
}

// Initiate Block Relocation
reloc_operation_t* initiate_block_relocation(
    relocation_system_t *system,
    block_metadata_t *block,
    reloc_strategy_t strategy
) {
    if (!system || !block) return NULL;

    pthread_mutex_lock(&system->system_lock);

    // Check relocation queue capacity
    if (system->reloc_queue_size >= system->max_reloc_queue) {
        LOG(LOG_LEVEL_ERROR, "Relocation queue full");
        pthread_mutex_unlock(&system->system_lock);
        return NULL;
    }

    // Create relocation operation
    reloc_operation_t *reloc_op = malloc(sizeof(reloc_operation_t));
    if (!reloc_op) {
        pthread_mutex_unlock(&system->system_lock);
        return NULL;
    }

    reloc_op->block = block;
    reloc_op->strategy = strategy;
    reloc_op->is_completed = false;
    reloc_op->start_time = time(NULL);
    reloc_op->end_time = 0;

    // Update block state
    block->state = BLOCK_STATE_RELOCATING;

    // Add to relocation queue
    system->reloc_queue[system->reloc_queue_size] = reloc_op;
    system->reloc_queue_size++;

    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Initiated relocation for block %lu using %s strategy", 
        block->block_id, get_reloc_strategy_string(strategy));

    return reloc_op;
}

// Complete Block Relocation
bool complete_block_relocation(
    relocation_system_t *system,
    reloc_operation_t *reloc_op
) {
    if (!system || !reloc_op || reloc_op->is_completed) return false;

    pthread_mutex_lock(&system->system_lock);

    block_metadata_t *block = reloc_op->block;

    // Simulate relocation based on strategy
    switch (reloc_op->strategy) {
        case RELOC_STRATEGY_SEQUENTIAL:
            block->new_location = block->original_location + block->size;
            break;
        case RELOC_STRATEGY_RANDOM:
            block->new_location = rand() % (1024 * 1024 * 1024);
            break;
        case RELOC_STRATEGY_FRAGMENTATION_AWARE:
            // Simulate defragmentation
            block->new_location = block->original_location;
            system->stats.fragmentation_events++;
            break;
        default:
            block->new_location = block->original_location;
            break;
    }

    // Update block state
    block->state = BLOCK_STATE_ALLOCATED;
    block->original_location = block->new_location;

    // Remove from relocation queue
    for (size_t i = 0; i < system->reloc_queue_size; i++) {
        if (system->reloc_queue[i] == reloc_op) {
            for (size_t j = i; j < system->reloc_queue_size - 1; j++) {
                system->reloc_queue[j] = system->reloc_queue[j + 1];
            }
            system->reloc_queue_size--;
            break;
        }
    }

    reloc_op->is_completed = true;
    reloc_op->end_time = time(NULL);

    system->stats.total_blocks_relocated++;

    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Completed relocation for block %lu", block->block_id);

    return true;
}

// Print Block Statistics
void print_block_stats(relocation_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->system_lock);

    printf("\nBlock Relocation System Statistics:\n");
    printf("-----------------------------------\n");
    printf("Total Blocks Created:     %lu\n", system->stats.total_blocks_created);
    printf("Total Blocks Relocated:   %lu\n", system->stats.total_blocks_relocated);
    printf("Total Blocks Freed:       %lu\n", system->stats.total_blocks_freed);
    printf("Relocation Failures:      %lu\n", system->stats.relocation_failures);
    printf("Fragmentation Events:     %lu\n", system->stats.fragmentation_events);
    printf("Current Blocks:           %zu\n", system->total_blocks);
    printf("Relocation Queue Size:    %zu\n", system->reloc_queue_size);

    pthread_mutex_unlock(&system->system_lock);
}

// Destroy Relocation System
void destroy_relocation_system(relocation_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->system_lock);

    // Free all blocks
    for (size_t i = 0; i < system->total_blocks; i++) {
        free(system->blocks[i]->data);
        free(system->blocks[i]);
    }

    // Free relocation queue
    for (size_t i = 0; i < system->reloc_queue_size; i++) {
        free(system->reloc_queue[i]);
    }

    free(system->blocks);
    free(system->reloc_queue);

    pthread_mutex_unlock(&system->system_lock);
    pthread_mutex_destroy(&system->system_lock);

    free(system);
}

// Demonstrate Relocation System
void demonstrate_relocation_system() {
    // Create Relocation System
    relocation_system_t *reloc_system = create_relocation_system(100, 50);

    // Create Sample Blocks
    block_metadata_t *data_blocks[5];
    for (int i = 0; i < 5; i++) {
        data_blocks[i] = create_block(
            reloc_system, 
            BLOCK_TYPE_DATA, 
            1024 * (i + 1)  // Varying block sizes
        );
    }

    // Simulate Block Relocations
    reloc_operation_t *reloc_ops[5];
    for (int i = 0; i < 5; i++) {
        reloc_ops[i] = initiate_block_relocation(
            reloc_system, 
            data_blocks[i], 
            (reloc_strategy_t)(i % 4)  // Cycle through strategies
        );
    }

    // Complete Relocations
    for (int i = 0; i < 5; i++) {
        if (reloc_ops[i]) {
            complete_block_relocation(reloc_system, reloc_ops[i]);
        }
    }

    // Free Some Blocks
    for (int i = 0; i < 3; i++) {
        free_block(reloc_system, data_blocks[i]);
    }

    // Print Statistics
    print_block_stats(reloc_system);

    // Cleanup
    destroy_relocation_system(reloc_system);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_relocation_system();

    return 0;
}
