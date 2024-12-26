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

// Quota Group Types
typedef enum {
    QGROUP_TYPE_USER,
    QGROUP_TYPE_PROJECT,
    QGROUP_TYPE_SYSTEM,
    QGROUP_TYPE_CONTAINER,
    QGROUP_TYPE_DYNAMIC
} qgroup_type_t;

// Resource Types
typedef enum {
    RESOURCE_STORAGE,
    RESOURCE_MEMORY,
    RESOURCE_CPU,
    RESOURCE_NETWORK,
    RESOURCE_IOPS
} resource_type_t;

// Limit Enforcement Modes
typedef enum {
    LIMIT_MODE_SOFT,
    LIMIT_MODE_HARD,
    LIMIT_MODE_WARNING,
    LIMIT_MODE_ALERT
} limit_mode_t;

// Quota Limit Structure
typedef struct {
    resource_type_t resource;
    uint64_t current_usage;
    uint64_t soft_limit;
    uint64_t hard_limit;
    limit_mode_t enforcement_mode;
} quota_limit_t;

// Quota Group Statistics
typedef struct {
    unsigned long total_allocations;
    unsigned long total_frees;
    unsigned long limit_violations;
    unsigned long soft_limit_warnings;
    unsigned long hard_limit_blocks;
} qgroup_stats_t;

// Quota Group
typedef struct qgroup {
    uint64_t id;
    char *name;
    qgroup_type_t type;
    
    quota_limit_t *limits;
    size_t limit_count;
    
    struct qgroup *parent;
    struct qgroup **children;
    size_t child_count;
    
    qgroup_stats_t stats;
    pthread_mutex_t group_lock;
} qgroup_t;

// Quota Management System
typedef struct {
    qgroup_t *root_group;
    size_t max_groups;
    size_t current_groups;
    pthread_mutex_t system_lock;
} qgroup_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_qgroup_type_string(qgroup_type_t type);
const char* get_resource_type_string(resource_type_t type);
const char* get_limit_mode_string(limit_mode_t mode);

qgroup_system_t* create_qgroup_system(size_t max_groups);
void destroy_qgroup_system(qgroup_system_t *system);

qgroup_t* create_qgroup(
    qgroup_system_t *system,
    uint64_t id,
    const char *name,
    qgroup_type_t type
);

bool add_qgroup_limit(
    qgroup_t *group,
    resource_type_t resource,
    uint64_t soft_limit,
    uint64_t hard_limit,
    limit_mode_t mode
);

bool allocate_resource(
    qgroup_t *group,
    resource_type_t resource,
    uint64_t amount
);

bool free_resource(
    qgroup_t *group,
    resource_type_t resource,
    uint64_t amount
);

void print_qgroup_stats(qgroup_t *group);
void print_qgroup_system_summary(qgroup_system_t *system);

// Demonstration Functions
void demonstrate_qgroup_system();

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

// Utility Function: Get Quota Group Type String
const char* get_qgroup_type_string(qgroup_type_t type) {
    switch(type) {
        case QGROUP_TYPE_USER:      return "USER";
        case QGROUP_TYPE_PROJECT:   return "PROJECT";
        case QGROUP_TYPE_SYSTEM:    return "SYSTEM";
        case QGROUP_TYPE_CONTAINER: return "CONTAINER";
        case QGROUP_TYPE_DYNAMIC:   return "DYNAMIC";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Resource Type String
const char* get_resource_type_string(resource_type_t type) {
    switch(type) {
        case RESOURCE_STORAGE:  return "STORAGE";
        case RESOURCE_MEMORY:   return "MEMORY";
        case RESOURCE_CPU:      return "CPU";
        case RESOURCE_NETWORK:  return "NETWORK";
        case RESOURCE_IOPS:     return "IOPS";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Limit Mode String
const char* get_limit_mode_string(limit_mode_t mode) {
    switch(mode) {
        case LIMIT_MODE_SOFT:     return "SOFT";
        case LIMIT_MODE_HARD:     return "HARD";
        case LIMIT_MODE_WARNING:  return "WARNING";
        case LIMIT_MODE_ALERT:    return "ALERT";
        default: return "UNKNOWN";
    }
}

// Create Quota Group System
qgroup_system_t* create_qgroup_system(size_t max_groups) {
    qgroup_system_t *system = malloc(sizeof(qgroup_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate quota group system");
        return NULL;
    }

    system->root_group = create_qgroup(
        system, 
        0, 
        "root", 
        QGROUP_TYPE_SYSTEM
    );

    if (!system->root_group) {
        free(system);
        return NULL;
    }

    system->max_groups = max_groups;
    system->current_groups = 1;  // Root group

    pthread_mutex_init(&system->system_lock, NULL);

    return system;
}

// Create Quota Group
qgroup_t* create_qgroup(
    qgroup_system_t *system,
    uint64_t id,
    const char *name,
    qgroup_type_t type
) {
    if (!system || system->current_groups >= system->max_groups) {
        LOG(LOG_LEVEL_ERROR, "Cannot create group: system limit reached");
        return NULL;
    }

    qgroup_t *group = malloc(sizeof(qgroup_t));
    if (!group) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate quota group");
        return NULL;
    }

    group->id = id;
    group->name = strdup(name);
    group->type = type;
    group->limits = NULL;
    group->limit_count = 0;
    group->parent = NULL;
    group->children = NULL;
    group->child_count = 0;

    memset(&group->stats, 0, sizeof(qgroup_stats_t));
    pthread_mutex_init(&group->group_lock, NULL);

    // Add to system
    pthread_mutex_lock(&system->system_lock);
    system->current_groups++;
    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Created Quota Group: %s (Type: %s)", 
        name, get_qgroup_type_string(type));

    return group;
}

// Add Quota Group Limit
bool add_qgroup_limit(
    qgroup_t *group,
    resource_type_t resource,
    uint64_t soft_limit,
    uint64_t hard_limit,
    limit_mode_t mode
) {
    if (!group) return false;

    pthread_mutex_lock(&group->group_lock);

    // Reallocate limits array
    quota_limit_t *new_limits = realloc(
        group->limits, 
        sizeof(quota_limit_t) * (group->limit_count + 1)
    );

    if (!new_limits) {
        pthread_mutex_unlock(&group->group_lock);
        LOG(LOG_LEVEL_ERROR, "Failed to add limit for group %s", group->name);
        return false;
    }

    group->limits = new_limits;
    
    // Add new limit
    group->limits[group->limit_count].resource = resource;
    group->limits[group->limit_count].current_usage = 0;
    group->limits[group->limit_count].soft_limit = soft_limit;
    group->limits[group->limit_count].hard_limit = hard_limit;
    group->limits[group->limit_count].enforcement_mode = mode;

    group->limit_count++;

    pthread_mutex_unlock(&group->group_lock);

    LOG(LOG_LEVEL_DEBUG, "Added %s limit to group %s: Soft=%lu, Hard=%lu", 
        get_resource_type_string(resource), 
        group->name, 
        soft_limit, 
        hard_limit);

    return true;
}

// Allocate Resource
bool allocate_resource(
    qgroup_t *group,
    resource_type_t resource,
    uint64_t amount
) {
    if (!group) return false;

    pthread_mutex_lock(&group->group_lock);

    for (size_t i = 0; i < group->limit_count; i++) {
        if (group->limits[i].resource == resource) {
            uint64_t new_usage = group->limits[i].current_usage + amount;

            // Check limits
            if (new_usage > group->limits[i].hard_limit) {
                group->stats.hard_limit_blocks++;
                pthread_mutex_unlock(&group->group_lock);
                
                LOG(LOG_LEVEL_WARN, "Hard limit exceeded for %s in group %s", 
                    get_resource_type_string(resource), group->name);
                
                return false;
            }

            if (new_usage > group->limits[i].soft_limit) {
                group->stats.soft_limit_warnings++;
                
                LOG(LOG_LEVEL_WARN, "Soft limit approaching for %s in group %s", 
                    get_resource_type_string(resource), group->name);
            }

            group->limits[i].current_usage = new_usage;
            group->stats.total_allocations++;

            pthread_mutex_unlock(&group->group_lock);

            LOG(LOG_LEVEL_DEBUG, "Allocated %lu %s to group %s", 
                amount, get_resource_type_string(resource), group->name);

            return true;
        }
    }

    pthread_mutex_unlock(&group->group_lock);
    return false;
}

// Free Resource
bool free_resource(
    qgroup_t *group,
    resource_type_t resource,
    uint64_t amount
) {
    if (!group) return false;

    pthread_mutex_lock(&group->group_lock);

    for (size_t i = 0; i < group->limit_count; i++) {
        if (group->limits[i].resource == resource) {
            if (amount > group->limits[i].current_usage) {
                group->limits[i].current_usage = 0;
            } else {
                group->limits[i].current_usage -= amount;
            }

            group->stats.total_frees++;

            pthread_mutex_unlock(&group->group_lock);

            LOG(LOG_LEVEL_DEBUG, "Freed %lu %s from group %s", 
                amount, get_resource_type_string(resource), group->name);

            return true;
        }
    }

    pthread_mutex_unlock(&group->group_lock);
    return false;
}

// Print Quota Group Statistics
void print_qgroup_stats(qgroup_t *group) {
    if (!group) return;

    pthread_mutex_lock(&group->group_lock);

    printf("\nQuota Group Statistics for %s:\n", group->name);
    printf("--------------------------------\n");
    printf("Group Type:            %s\n", get_qgroup_type_string(group->type));
    printf("Total Allocations:     %lu\n", group->stats.total_allocations);
    printf("Total Frees:           %lu\n", group->stats.total_frees);
    printf("Limit Violations:      %lu\n", group->stats.limit_violations);
    printf("Soft Limit Warnings:   %lu\n", group->stats.soft_limit_warnings);
    printf("Hard Limit Blocks:     %lu\n", group->stats.hard_limit_blocks);

    printf("\nResource Limits:\n");
    for (size_t i = 0; i < group->limit_count; i++) {
        printf("  %s: Usage=%lu, Soft=%lu, Hard=%lu, Mode=%s\n", 
            get_resource_type_string(group->limits[i].resource),
            group->limits[i].current_usage,
            group->limits[i].soft_limit,
            group->limits[i].hard_limit,
            get_limit_mode_string(group->limits[i].enforcement_mode)
        );
    }

    pthread_mutex_unlock(&group->group_lock);
}

// Print Quota Group System Summary
void print_qgroup_system_summary(qgroup_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->system_lock);

    printf("\nQuota Group System Summary:\n");
    printf("---------------------------\n");
    printf("Total Groups:          %zu\n", system->current_groups);
    printf("Max Groups:            %zu\n", system->max_groups);

    pthread_mutex_unlock(&system->system_lock);
}

// Destroy Quota Group
void destroy_qgroup(qgroup_t *group) {
    if (!group) return;

    free(group->name);
    free(group->limits);
    
    // Free children (simplified)
    free(group->children);
    
    pthread_mutex_destroy(&group->group_lock);
    free(group);
}

// Destroy Quota Group System
void destroy_qgroup_system(qgroup_system_t *system) {
    if (!system) return;

    // Destroy root group
    destroy_qgroup(system->root_group);

    pthread_mutex_destroy(&system->system_lock);
    free(system);
}

// Demonstrate Quota Group System
void demonstrate_qgroup_system() {
    // Create Quota Group System
    qgroup_system_t *qgroup_system = create_qgroup_system(10);

    // Create Sample Quota Groups
    qgroup_t *user_group = create_qgroup(
        qgroup_system, 
        1, 
        "user_group", 
        QGROUP_TYPE_USER
    );

    qgroup_t *project_group = create_qgroup(
        qgroup_system, 
        2, 
        "project_group", 
        QGROUP_TYPE_PROJECT
    );

    // Add Resource Limits
    add_qgroup_limit(
        user_group, 
        RESOURCE_STORAGE, 
        1024 * 1024 * 1024,   // 1 GB soft limit
        2048 * 1024 * 1024,   // 2 GB hard limit
        LIMIT_MODE_HARD
    );

    add_qgroup_limit(
        project_group, 
        RESOURCE_MEMORY, 
        4 * 1024 * 1024 * 1024ULL,    // 4 GB soft limit
        8 * 1024 * 1024 * 1024ULL,    // 8 GB hard limit
        LIMIT_MODE_WARNING
    );

    // Simulate Resource Allocation
    allocate_resource(user_group, RESOURCE_STORAGE, 500 * 1024 * 1024);
    allocate_resource(user_group, RESOURCE_STORAGE, 600 * 1024 * 1024);
    allocate_resource(user_group, RESOURCE_STORAGE, 1024 * 1024 * 1024);

    allocate_resource(project_group, RESOURCE_MEMORY, 3 * 1024 * 1024 * 1024ULL);
    allocate_resource(project_group, RESOURCE_MEMORY, 2 * 1024 * 1024 * 1024ULL);

    // Print Group Statistics
    print_qgroup_stats(user_group);
    print_qgroup_stats(project_group);

    // Print System Summary
    print_qgroup_system_summary(qgroup_system);

    // Cleanup
    destroy_qgroup_system(qgroup_system);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_qgroup_system();

    return 0;
}
