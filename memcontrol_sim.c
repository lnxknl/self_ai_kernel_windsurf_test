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

// Resource Types
typedef enum {
    RESOURCE_TYPE_MEMORY,
    RESOURCE_TYPE_SWAP,
    RESOURCE_TYPE_KERNEL,
    RESOURCE_TYPE_USER,
    RESOURCE_TYPE_CACHE
} resource_type_t;

// Memory Pressure Levels
typedef enum {
    PRESSURE_LEVEL_LOW,
    PRESSURE_LEVEL_MEDIUM,
    PRESSURE_LEVEL_HIGH,
    PRESSURE_LEVEL_CRITICAL
} pressure_level_t;

// Memory Control Flags
typedef enum {
    MEMCTL_FLAG_NONE       = 0,
    MEMCTL_FLAG_RECLAIM    = 1 << 0,
    MEMCTL_FLAG_COMPACT    = 1 << 1,
    MEMCTL_FLAG_OOM_KILL   = 1 << 2,
    MEMCTL_FLAG_SOFT_LIMIT = 1 << 3
} memctl_flags_t;

// Memory Allocation Policy
typedef enum {
    ALLOC_POLICY_DEFAULT,
    ALLOC_POLICY_STRICT,
    ALLOC_POLICY_RELAXED,
    ALLOC_POLICY_NUMA,
    ALLOC_POLICY_HUGEPAGE
} alloc_policy_t;

// Memory Control Group
typedef struct memory_control_group {
    char *name;
    uint64_t memory_limit;
    uint64_t current_usage;
    uint64_t peak_usage;
    
    resource_type_t type;
    pressure_level_t pressure_level;
    memctl_flags_t flags;
    alloc_policy_t allocation_policy;
    
    struct memory_control_group *parent;
    struct memory_control_group **children;
    size_t child_count;
    
    time_t creation_time;
    time_t last_updated;
} memory_control_group_t;

// Memory Management Statistics
typedef struct {
    unsigned long total_allocations;
    unsigned long total_frees;
    unsigned long reclaim_attempts;
    unsigned long reclaim_successes;
    unsigned long oom_kills;
    unsigned long pressure_events;
} memctl_stats_t;

// Memory Control System
typedef struct {
    memory_control_group_t *root_group;
    size_t total_groups;
    size_t max_groups;
    
    memctl_stats_t stats;
    pthread_mutex_t system_lock;
} memctl_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_resource_type_string(resource_type_t type);
const char* get_pressure_level_string(pressure_level_t level);
const char* get_alloc_policy_string(alloc_policy_t policy);

memctl_system_t* create_memctl_system(size_t max_groups);
void destroy_memctl_system(memctl_system_t *system);

memory_control_group_t* create_memory_control_group(
    memctl_system_t *system,
    const char *name,
    uint64_t memory_limit,
    resource_type_t type,
    alloc_policy_t allocation_policy
);

bool delete_memory_control_group(
    memctl_system_t *system,
    memory_control_group_t *group
);

bool update_memory_usage(
    memory_control_group_t *group,
    int64_t usage_change
);

bool apply_memory_pressure(
    memory_control_group_t *group,
    pressure_level_t pressure_level
);

bool reclaim_memory(
    memory_control_group_t *group,
    uint64_t reclaim_size
);

memory_control_group_t* find_memory_control_group(
    memctl_system_t *system,
    const char *name
);

void print_memctl_stats(memctl_system_t *system);
void demonstrate_memctl_system();

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

// Utility Function: Get Resource Type String
const char* get_resource_type_string(resource_type_t type) {
    switch(type) {
        case RESOURCE_TYPE_MEMORY:  return "MEMORY";
        case RESOURCE_TYPE_SWAP:    return "SWAP";
        case RESOURCE_TYPE_KERNEL:  return "KERNEL";
        case RESOURCE_TYPE_USER:    return "USER";
        case RESOURCE_TYPE_CACHE:   return "CACHE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Pressure Level String
const char* get_pressure_level_string(pressure_level_t level) {
    switch(level) {
        case PRESSURE_LEVEL_LOW:        return "LOW";
        case PRESSURE_LEVEL_MEDIUM:     return "MEDIUM";
        case PRESSURE_LEVEL_HIGH:       return "HIGH";
        case PRESSURE_LEVEL_CRITICAL:   return "CRITICAL";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Allocation Policy String
const char* get_alloc_policy_string(alloc_policy_t policy) {
    switch(policy) {
        case ALLOC_POLICY_DEFAULT:   return "DEFAULT";
        case ALLOC_POLICY_STRICT:    return "STRICT";
        case ALLOC_POLICY_RELAXED:   return "RELAXED";
        case ALLOC_POLICY_NUMA:      return "NUMA";
        case ALLOC_POLICY_HUGEPAGE:  return "HUGEPAGE";
        default: return "UNKNOWN";
    }
}

// Create Memory Control System
memctl_system_t* create_memctl_system(size_t max_groups) {
    memctl_system_t *system = malloc(sizeof(memctl_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate memory control system");
        return NULL;
    }

    // Create root memory control group
    system->root_group = malloc(sizeof(memory_control_group_t));
    if (!system->root_group) {
        free(system);
        return NULL;
    }

    // Initialize root group
    system->root_group->name = strdup("root");
    system->root_group->memory_limit = UINT64_MAX;
    system->root_group->current_usage = 0;
    system->root_group->peak_usage = 0;
    system->root_group->type = RESOURCE_TYPE_MEMORY;
    system->root_group->pressure_level = PRESSURE_LEVEL_LOW;
    system->root_group->flags = MEMCTL_FLAG_NONE;
    system->root_group->allocation_policy = ALLOC_POLICY_DEFAULT;
    system->root_group->parent = NULL;
    system->root_group->children = NULL;
    system->root_group->child_count = 0;
    system->root_group->creation_time = time(NULL);
    system->root_group->last_updated = system->root_group->creation_time;

    system->total_groups = 1;  // Root group
    system->max_groups = max_groups;

    // Reset statistics
    memset(&system->stats, 0, sizeof(memctl_stats_t));

    // Initialize system lock
    pthread_mutex_init(&system->system_lock, NULL);

    LOG(LOG_LEVEL_DEBUG, "Created Memory Control System with max %zu groups", max_groups);

    return system;
}

// Create Memory Control Group
memory_control_group_t* create_memory_control_group(
    memctl_system_t *system,
    const char *name,
    uint64_t memory_limit,
    resource_type_t type,
    alloc_policy_t allocation_policy
) {
    if (!system || system->total_groups >= system->max_groups) {
        LOG(LOG_LEVEL_ERROR, "Cannot create memory control group: system limit reached");
        return NULL;
    }

    memory_control_group_t *group = malloc(sizeof(memory_control_group_t));
    if (!group) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate memory control group");
        return NULL;
    }

    // Initialize group properties
    group->name = strdup(name);
    group->memory_limit = memory_limit;
    group->current_usage = 0;
    group->peak_usage = 0;
    group->type = type;
    group->pressure_level = PRESSURE_LEVEL_LOW;
    group->flags = MEMCTL_FLAG_SOFT_LIMIT;
    group->allocation_policy = allocation_policy;
    
    // Link to root group
    group->parent = system->root_group;
    group->children = NULL;
    group->child_count = 0;
    
    group->creation_time = time(NULL);
    group->last_updated = group->creation_time;

    // Add to root group's children
    pthread_mutex_lock(&system->system_lock);
    
    system->root_group->children = realloc(
        system->root_group->children, 
        (system->root_group->child_count + 1) * sizeof(memory_control_group_t*)
    );
    
    if (!system->root_group->children) {
        free(group->name);
        free(group);
        pthread_mutex_unlock(&system->system_lock);
        return NULL;
    }
    
    system->root_group->children[system->root_group->child_count] = group;
    system->root_group->child_count++;
    
    system->total_groups++;

    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Created memory control group %s, Limit %lu", name, memory_limit);

    return group;
}

// Delete Memory Control Group
bool delete_memory_control_group(
    memctl_system_t *system,
    memory_control_group_t *group
) {
    if (!system || !group) return false;

    pthread_mutex_lock(&system->system_lock);

    // Cannot delete root group
    if (group == system->root_group) {
        pthread_mutex_unlock(&system->system_lock);
        return false;
    }

    // Remove from parent's children
    for (size_t i = 0; i < group->parent->child_count; i++) {
        if (group->parent->children[i] == group) {
            // Shift remaining children
            for (size_t j = i; j < group->parent->child_count - 1; j++) {
                group->parent->children[j] = group->parent->children[j + 1];
            }
            group->parent->child_count--;
            break;
        }
    }

    // Free children
    for (size_t i = 0; i < group->child_count; i++) {
        free(group->children[i]->name);
        free(group->children[i]);
    }
    free(group->children);

    // Free group
    free(group->name);
    free(group);

    system->total_groups--;

    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Deleted memory control group");

    return true;
}

// Update Memory Usage
bool update_memory_usage(
    memory_control_group_t *group,
    int64_t usage_change
) {
    if (!group) return false;

    pthread_mutex_lock(&group->parent->system_lock);

    // Update current usage
    group->current_usage += usage_change;
    
    // Update peak usage
    if (group->current_usage > group->peak_usage) {
        group->peak_usage = group->current_usage;
    }

    // Check memory limit
    if (group->current_usage > group->memory_limit) {
        group->flags |= MEMCTL_FLAG_RECLAIM;
        group->pressure_level = PRESSURE_LEVEL_HIGH;
    }

    group->last_updated = time(NULL);

    pthread_mutex_unlock(&group->parent->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Updated group %s usage: %ld", group->name, usage_change);

    return true;
}

// Apply Memory Pressure
bool apply_memory_pressure(
    memory_control_group_t *group,
    pressure_level_t pressure_level
) {
    if (!group) return false;

    pthread_mutex_lock(&group->parent->system_lock);

    group->pressure_level = pressure_level;
    group->parent->stats.pressure_events++;

    // Escalate actions based on pressure level
    switch (pressure_level) {
        case PRESSURE_LEVEL_HIGH:
            group->flags |= MEMCTL_FLAG_RECLAIM;
            break;
        case PRESSURE_LEVEL_CRITICAL:
            group->flags |= MEMCTL_FLAG_OOM_KILL;
            group->parent->stats.oom_kills++;
            break;
        default:
            break;
    }

    group->last_updated = time(NULL);

    pthread_mutex_unlock(&group->parent->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Applied %s pressure to group %s", 
        get_pressure_level_string(pressure_level), group->name);

    return true;
}

// Reclaim Memory
bool reclaim_memory(
    memory_control_group_t *group,
    uint64_t reclaim_size
) {
    if (!group) return false;

    pthread_mutex_lock(&group->parent->system_lock);

    group->parent->stats.reclaim_attempts++;

    // Simulate memory reclamation
    if (group->current_usage >= reclaim_size) {
        group->current_usage -= reclaim_size;
        group->flags &= ~MEMCTL_FLAG_RECLAIM;
        group->pressure_level = PRESSURE_LEVEL_LOW;
        
        group->parent->stats.reclaim_successes++;

        pthread_mutex_unlock(&group->parent->system_lock);

        LOG(LOG_LEVEL_DEBUG, "Reclaimed %lu bytes from group %s", 
            reclaim_size, group->name);

        return true;
    }

    pthread_mutex_unlock(&group->parent->system_lock);
    return false;
}

// Find Memory Control Group
memory_control_group_t* find_memory_control_group(
    memctl_system_t *system,
    const char *name
) {
    if (!system || !name) return NULL;

    pthread_mutex_lock(&system->system_lock);

    // Search root group's children
    for (size_t i = 0; i < system->root_group->child_count; i++) {
        memory_control_group_t *group = system->root_group->children[i];
        if (strcmp(group->name, name) == 0) {
            pthread_mutex_unlock(&system->system_lock);
            return group;
        }
    }

    pthread_mutex_unlock(&system->system_lock);
    return NULL;
}

// Print Memory Control Statistics
void print_memctl_stats(memctl_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->system_lock);

    printf("\nMemory Control System Statistics:\n");
    printf("--------------------------------\n");
    printf("Total Memory Control Groups: %zu\n", system->total_groups);
    printf("Total Allocations:          %lu\n", system->stats.total_allocations);
    printf("Total Frees:                %lu\n", system->stats.total_frees);
    printf("Reclaim Attempts:           %lu\n", system->stats.reclaim_attempts);
    printf("Reclaim Successes:          %lu\n", system->stats.reclaim_successes);
    printf("OOM Kills:                  %lu\n", system->stats.oom_kills);
    printf("Pressure Events:            %lu\n", system->stats.pressure_events);

    pthread_mutex_unlock(&system->system_lock);
}

// Destroy Memory Control System
void destroy_memctl_system(memctl_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->system_lock);

    // Free root group's children
    for (size_t i = 0; i < system->root_group->child_count; i++) {
        memory_control_group_t *group = system->root_group->children[i];
        free(group->name);
        free(group->children);
        free(group);
    }

    // Free root group
    free(system->root_group->name);
    free(system->root_group->children);
    free(system->root_group);

    pthread_mutex_unlock(&system->system_lock);
    pthread_mutex_destroy(&system->system_lock);

    free(system);
}

// Demonstrate Memory Control System
void demonstrate_memctl_system() {
    // Create Memory Control System
    memctl_system_t *memctl_system = create_memctl_system(10);

    // Create Memory Control Groups
    memory_control_group_t *groups[5];
    for (int i = 0; i < 5; i++) {
        char group_name[32];
        snprintf(group_name, sizeof(group_name), "group_%d", i);
        
        groups[i] = create_memory_control_group(
            memctl_system, 
            group_name,
            1024 * 1024 * (i + 1),  // Varying memory limits
            (resource_type_t)(i % 5),  // Cycle through resource types
            (alloc_policy_t)((i + 1) % 5)  // Cycle through allocation policies
        );
    }

    // Simulate Memory Usage
    for (int i = 0; i < 5; i++) {
        if (groups[i]) {
            // Simulate memory allocation and usage changes
            update_memory_usage(
                groups[i], 
                1024 * 1024 * (i + 1)  // Varying usage changes
            );

            // Apply memory pressure
            apply_memory_pressure(
                groups[i], 
                (pressure_level_t)(i % 4)  // Cycle through pressure levels
            );

            // Attempt memory reclamation for high-pressure groups
            if (groups[i]->pressure_level >= PRESSURE_LEVEL_HIGH) {
                reclaim_memory(
                    groups[i], 
                    512 * 1024  // Reclaim 512KB
                );
            }
        }
    }

    // Find and Manipulate a Group
    memory_control_group_t *found_group = find_memory_control_group(
        memctl_system, 
        "group_2"
    );

    if (found_group) {
        // Simulate additional operations on found group
        update_memory_usage(found_group, -256 * 1024);  // Reduce usage
    }

    // Print Statistics
    print_memctl_stats(memctl_system);

    // Cleanup
    destroy_memctl_system(memctl_system);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_memctl_system();

    return 0;
}
