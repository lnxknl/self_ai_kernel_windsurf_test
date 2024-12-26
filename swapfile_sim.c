#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

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

// Constants
#define PAGE_SIZE 4096
#define MAX_SWAP_ENTRIES 1024
#define SWAP_CLUSTER_SIZE 8

// Swap Entry States
typedef enum {
    SWAP_ENTRY_FREE,
    SWAP_ENTRY_USED,
    SWAP_ENTRY_BAD
} swap_entry_state_t;

// Swap Entry Structure
typedef struct {
    unsigned long index;
    void *data;
    swap_entry_state_t state;
    unsigned long access_count;
    time_t last_access;
} swap_entry_t;

// Swap Area Structure
typedef struct {
    char *filename;
    size_t size;
    size_t used;
    swap_entry_t *entries;
    unsigned long *bitmap;
    size_t bitmap_size;
    pthread_mutex_t lock;
} swap_area_t;

// Swap Statistics
typedef struct {
    unsigned long total_pages;
    unsigned long used_pages;
    unsigned long bad_pages;
    unsigned long reads;
    unsigned long writes;
    unsigned long hits;
    unsigned long misses;
    double avg_access_time;
} swap_stats_t;

// Swap Configuration
typedef struct {
    size_t min_size;
    size_t max_size;
    bool verify_pages;
    unsigned int cluster_size;
    bool track_stats;
} swap_config_t;

// Swap Manager
typedef struct {
    swap_area_t *areas;
    size_t area_count;
    swap_config_t config;
    swap_stats_t stats;
    pthread_mutex_t manager_lock;
} swap_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_entry_state_string(swap_entry_state_t state);

swap_manager_t* create_swap_manager(swap_config_t config);
void destroy_swap_manager(swap_manager_t *manager);

swap_area_t* create_swap_area(const char *filename, size_t size);
void destroy_swap_area(swap_area_t *area);

int swap_out(swap_manager_t *manager, void *data, size_t size);
void* swap_in(swap_manager_t *manager, unsigned long index);

bool mark_swap_entry(swap_area_t *area, unsigned long index, swap_entry_state_t state);
void print_swap_stats(swap_manager_t *manager);
void demonstrate_swap(void);

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

// Utility Function: Get Entry State String
const char* get_entry_state_string(swap_entry_state_t state) {
    switch(state) {
        case SWAP_ENTRY_FREE:  return "FREE";
        case SWAP_ENTRY_USED:  return "USED";
        case SWAP_ENTRY_BAD:   return "BAD";
        default: return "UNKNOWN";
    }
}

// Create Swap Manager
swap_manager_t* create_swap_manager(swap_config_t config) {
    swap_manager_t *manager = malloc(sizeof(swap_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate swap manager");
        return NULL;
    }

    manager->areas = NULL;
    manager->area_count = 0;
    manager->config = config;
    memset(&manager->stats, 0, sizeof(swap_stats_t));
    
    pthread_mutex_init(&manager->manager_lock, NULL);

    LOG(LOG_LEVEL_DEBUG, "Created swap manager");
    return manager;
}

// Create Swap Area
swap_area_t* create_swap_area(const char *filename, size_t size) {
    swap_area_t *area = malloc(sizeof(swap_area_t));
    if (!area) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate swap area");
        return NULL;
    }

    area->filename = strdup(filename);
    area->size = size;
    area->used = 0;
    
    // Allocate entries
    area->entries = calloc(MAX_SWAP_ENTRIES, sizeof(swap_entry_t));
    if (!area->entries) {
        free(area->filename);
        free(area);
        return NULL;
    }

    // Initialize bitmap
    area->bitmap_size = (MAX_SWAP_ENTRIES + 7) / 8;
    area->bitmap = calloc(area->bitmap_size, sizeof(unsigned long));
    if (!area->bitmap) {
        free(area->entries);
        free(area->filename);
        free(area);
        return NULL;
    }

    pthread_mutex_init(&area->lock, NULL);

    LOG(LOG_LEVEL_DEBUG, "Created swap area %s with size %zu", 
        filename, size);
    return area;
}

// Find Free Swap Entry
static int find_free_entry(swap_area_t *area) {
    for (size_t i = 0; i < MAX_SWAP_ENTRIES; i++) {
        if (area->entries[i].state == SWAP_ENTRY_FREE) {
            return i;
        }
    }
    return -1;
}

// Swap Out Operation
int swap_out(swap_manager_t *manager, void *data, size_t size) {
    if (!manager || !data || size == 0) return -EINVAL;

    pthread_mutex_lock(&manager->manager_lock);

    // Find swap area with enough space
    swap_area_t *area = NULL;
    for (size_t i = 0; i < manager->area_count; i++) {
        if (manager->areas[i].size - manager->areas[i].used >= size) {
            area = &manager->areas[i];
            break;
        }
    }

    if (!area) {
        pthread_mutex_unlock(&manager->manager_lock);
        return -ENOSPC;
    }

    pthread_mutex_lock(&area->lock);

    // Find free entry
    int index = find_free_entry(area);
    if (index < 0) {
        pthread_mutex_unlock(&area->lock);
        pthread_mutex_unlock(&manager->manager_lock);
        return -ENOSPC;
    }

    // Allocate and copy data
    area->entries[index].data = malloc(size);
    if (!area->entries[index].data) {
        pthread_mutex_unlock(&area->lock);
        pthread_mutex_unlock(&manager->manager_lock);
        return -ENOMEM;
    }

    memcpy(area->entries[index].data, data, size);
    area->entries[index].index = index;
    area->entries[index].state = SWAP_ENTRY_USED;
    area->entries[index].access_count = 0;
    area->entries[index].last_access = time(NULL);

    area->used += size;

    // Update bitmap
    area->bitmap[index / 8] |= (1 << (index % 8));

    // Update statistics
    if (manager->config.track_stats) {
        manager->stats.writes++;
        manager->stats.used_pages++;
    }

    pthread_mutex_unlock(&area->lock);
    pthread_mutex_unlock(&manager->manager_lock);

    LOG(LOG_LEVEL_DEBUG, "Swapped out %zu bytes to entry %d", size, index);
    return index;
}

// Swap In Operation
void* swap_in(swap_manager_t *manager, unsigned long index) {
    if (!manager) return NULL;

    pthread_mutex_lock(&manager->manager_lock);

    // Find swap area containing the entry
    swap_area_t *area = NULL;
    for (size_t i = 0; i < manager->area_count; i++) {
        if (index < MAX_SWAP_ENTRIES) {
            area = &manager->areas[i];
            break;
        }
        index -= MAX_SWAP_ENTRIES;
    }

    if (!area) {
        pthread_mutex_unlock(&manager->manager_lock);
        return NULL;
    }

    pthread_mutex_lock(&area->lock);

    // Check entry state
    if (area->entries[index].state != SWAP_ENTRY_USED) {
        pthread_mutex_unlock(&area->lock);
        pthread_mutex_unlock(&manager->manager_lock);
        
        if (manager->config.track_stats)
            manager->stats.misses++;
            
        return NULL;
    }

    // Update statistics
    if (manager->config.track_stats) {
        manager->stats.reads++;
        manager->stats.hits++;
        
        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        
        // Simulate read delay
        usleep(1000);
        
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        double access_time = 
            (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
            (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
            
        manager->stats.avg_access_time = 
            (manager->stats.avg_access_time * 
                (manager->stats.reads - 1) + access_time) /
            manager->stats.reads;
    }

    // Update entry statistics
    area->entries[index].access_count++;
    area->entries[index].last_access = time(NULL);

    void *data = area->entries[index].data;

    pthread_mutex_unlock(&area->lock);
    pthread_mutex_unlock(&manager->manager_lock);

    LOG(LOG_LEVEL_DEBUG, "Swapped in entry %lu", index);
    return data;
}

// Mark Swap Entry State
bool mark_swap_entry(
    swap_area_t *area,
    unsigned long index,
    swap_entry_state_t state
) {
    if (!area || index >= MAX_SWAP_ENTRIES) return false;

    pthread_mutex_lock(&area->lock);

    if (area->entries[index].state == state) {
        pthread_mutex_unlock(&area->lock);
        return true;
    }

    area->entries[index].state = state;
    
    if (state == SWAP_ENTRY_FREE) {
        area->bitmap[index / 8] &= ~(1 << (index % 8));
        if (area->entries[index].data) {
            free(area->entries[index].data);
            area->entries[index].data = NULL;
        }
    } else if (state == SWAP_ENTRY_BAD) {
        area->bitmap[index / 8] |= (1 << (index % 8));
    }

    pthread_mutex_unlock(&area->lock);

    LOG(LOG_LEVEL_DEBUG, "Marked entry %lu as %s", 
        index, get_entry_state_string(state));
    return true;
}

// Print Swap Statistics
void print_swap_stats(swap_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    printf("\nSwap Manager Statistics:\n");
    printf("----------------------\n");
    printf("Total Pages:      %lu\n", manager->stats.total_pages);
    printf("Used Pages:       %lu\n", manager->stats.used_pages);
    printf("Bad Pages:        %lu\n", manager->stats.bad_pages);
    printf("Total Reads:      %lu\n", manager->stats.reads);
    printf("Total Writes:     %lu\n", manager->stats.writes);
    printf("Cache Hits:       %lu\n", manager->stats.hits);
    printf("Cache Misses:     %lu\n", manager->stats.misses);
    printf("Avg Access Time:  %.2f ms\n", manager->stats.avg_access_time);

    pthread_mutex_unlock(&manager->manager_lock);
}

// Destroy Swap Area
void destroy_swap_area(swap_area_t *area) {
    if (!area) return;

    pthread_mutex_lock(&area->lock);

    // Free all entries
    for (size_t i = 0; i < MAX_SWAP_ENTRIES; i++) {
        if (area->entries[i].data) {
            free(area->entries[i].data);
        }
    }

    free(area->entries);
    free(area->bitmap);
    free(area->filename);

    pthread_mutex_unlock(&area->lock);
    pthread_mutex_destroy(&area->lock);

    free(area);
}

// Destroy Swap Manager
void destroy_swap_manager(swap_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    // Destroy all areas
    for (size_t i = 0; i < manager->area_count; i++) {
        destroy_swap_area(&manager->areas[i]);
    }

    free(manager->areas);

    pthread_mutex_unlock(&manager->manager_lock);
    pthread_mutex_destroy(&manager->manager_lock);

    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed swap manager");
}

// Demonstrate Swap
void demonstrate_swap(void) {
    // Create swap configuration
    swap_config_t config = {
        .min_size = PAGE_SIZE,
        .max_size = PAGE_SIZE * 1024,
        .verify_pages = true,
        .cluster_size = SWAP_CLUSTER_SIZE,
        .track_stats = true
    };

    // Create swap manager
    swap_manager_t *manager = create_swap_manager(config);
    if (!manager) return;

    // Create swap area
    swap_area_t *area = create_swap_area("swap.img", PAGE_SIZE * 100);
    if (area) {
        manager->areas = area;
        manager->area_count = 1;
    }

    // Demonstrate swap operations
    char test_data[PAGE_SIZE];
    memset(test_data, 'A', PAGE_SIZE);

    // Swap out some pages
    int indices[10];
    for (int i = 0; i < 10; i++) {
        test_data[0] = 'A' + i;
        indices[i] = swap_out(manager, test_data, PAGE_SIZE);
        LOG(LOG_LEVEL_INFO, "Swapped out page %d to entry %d", 
            i, indices[i]);
    }

    // Swap in some pages
    for (int i = 0; i < 5; i++) {
        void *data = swap_in(manager, indices[i]);
        if (data) {
            LOG(LOG_LEVEL_INFO, "Swapped in page %d: first char = %c", 
                i, *(char*)data);
        }
    }

    // Mark some entries as bad
    for (int i = 5; i < 7; i++) {
        mark_swap_entry(area, indices[i], SWAP_ENTRY_BAD);
    }

    // Print statistics
    print_swap_stats(manager);

    // Cleanup
    destroy_swap_manager(manager);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_swap();

    return 0;
}
