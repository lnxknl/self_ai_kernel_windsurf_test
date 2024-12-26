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

// Lock Modes
typedef enum {
    LOCK_MODE_NULL,
    LOCK_MODE_SHARED,
    LOCK_MODE_EXCLUSIVE,
    LOCK_MODE_CONCURRENT,
    LOCK_MODE_OPTIMISTIC
} lock_mode_t;

// Lock States
typedef enum {
    LOCK_STATE_FREE,
    LOCK_STATE_GRANTED,
    LOCK_STATE_WAITING,
    LOCK_STATE_CONVERTING,
    LOCK_STATE_DEADLOCK
} lock_state_t;

// Lock Flags
typedef enum {
    LOCK_FLAG_NONE        = 0,
    LOCK_FLAG_NOWAIT      = 1 << 0,
    LOCK_FLAG_VALBLOCK    = 1 << 1,
    LOCK_FLAG_PERSISTENT  = 1 << 2,
    LOCK_FLAG_RECOVERY    = 1 << 3
} lock_flags_t;

// Node Identifier
typedef struct {
    uint64_t node_id;
    char *node_name;
} node_id_t;

// Lock Resource
typedef struct {
    char *resource_name;
    uint64_t resource_id;
} lock_resource_t;

// Lock Request
typedef struct lock_request {
    node_id_t requester;
    lock_resource_t resource;
    lock_mode_t requested_mode;
    lock_mode_t granted_mode;
    lock_state_t state;
    lock_flags_t flags;
    time_t request_time;
    time_t timeout;
} lock_request_t;

// Lock Management Statistics
typedef struct {
    unsigned long total_lock_requests;
    unsigned long successful_locks;
    unsigned long failed_locks;
    unsigned long deadlock_detections;
    unsigned long mode_conversions;
} lock_stats_t;

// Distributed Lock Management System
typedef struct {
    lock_request_t **active_locks;
    size_t max_locks;
    size_t current_locks;
    
    node_id_t local_node;
    
    lock_stats_t stats;
    pthread_mutex_t system_lock;
} dlm_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_lock_mode_string(lock_mode_t mode);
const char* get_lock_state_string(lock_state_t state);

dlm_system_t* create_dlm_system(
    uint64_t node_id, 
    const char *node_name, 
    size_t max_locks
);

void destroy_dlm_system(dlm_system_t *system);

lock_request_t* acquire_lock(
    dlm_system_t *system,
    const char *resource_name,
    lock_mode_t mode,
    lock_flags_t flags
);

bool release_lock(
    dlm_system_t *system,
    lock_request_t *lock_request
);

bool convert_lock_mode(
    dlm_system_t *system,
    lock_request_t *lock_request,
    lock_mode_t new_mode
);

lock_request_t* find_lock_by_resource(
    dlm_system_t *system,
    const char *resource_name
);

bool detect_deadlock(
    dlm_system_t *system,
    lock_request_t *lock_request
);

void print_lock_stats(dlm_system_t *system);
void demonstrate_dlm_system();

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

// Utility Function: Get Lock Mode String
const char* get_lock_mode_string(lock_mode_t mode) {
    switch(mode) {
        case LOCK_MODE_NULL:        return "NULL";
        case LOCK_MODE_SHARED:      return "SHARED";
        case LOCK_MODE_EXCLUSIVE:   return "EXCLUSIVE";
        case LOCK_MODE_CONCURRENT:  return "CONCURRENT";
        case LOCK_MODE_OPTIMISTIC:  return "OPTIMISTIC";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Lock State String
const char* get_lock_state_string(lock_state_t state) {
    switch(state) {
        case LOCK_STATE_FREE:        return "FREE";
        case LOCK_STATE_GRANTED:     return "GRANTED";
        case LOCK_STATE_WAITING:     return "WAITING";
        case LOCK_STATE_CONVERTING:  return "CONVERTING";
        case LOCK_STATE_DEADLOCK:    return "DEADLOCK";
        default: return "UNKNOWN";
    }
}

// Create Distributed Lock Management System
dlm_system_t* create_dlm_system(
    uint64_t node_id, 
    const char *node_name, 
    size_t max_locks
) {
    dlm_system_t *system = malloc(sizeof(dlm_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate DLM system");
        return NULL;
    }

    system->active_locks = malloc(sizeof(lock_request_t*) * max_locks);
    if (!system->active_locks) {
        free(system);
        return NULL;
    }

    system->max_locks = max_locks;
    system->current_locks = 0;

    // Set local node information
    system->local_node.node_id = node_id;
    system->local_node.node_name = strdup(node_name);

    // Reset statistics
    memset(&system->stats, 0, sizeof(lock_stats_t));

    // Initialize system lock
    pthread_mutex_init(&system->system_lock, NULL);

    LOG(LOG_LEVEL_DEBUG, "Created DLM system for node %s", node_name);

    return system;
}

// Acquire Lock
lock_request_t* acquire_lock(
    dlm_system_t *system,
    const char *resource_name,
    lock_mode_t mode,
    lock_flags_t flags
) {
    if (!system || !resource_name) return NULL;

    pthread_mutex_lock(&system->system_lock);

    // Check system lock capacity
    if (system->current_locks >= system->max_locks) {
        if (flags & LOCK_FLAG_NOWAIT) {
            pthread_mutex_unlock(&system->system_lock);
            LOG(LOG_LEVEL_WARN, "Lock system full, cannot acquire lock");
            return NULL;
        }
    }

    // Check for existing lock on the resource
    lock_request_t *existing_lock = find_lock_by_resource(system, resource_name);
    if (existing_lock) {
        // Compatibility check
        if (existing_lock->granted_mode == LOCK_MODE_EXCLUSIVE && 
            mode != LOCK_MODE_NULL) {
            if (flags & LOCK_FLAG_NOWAIT) {
                pthread_mutex_unlock(&system->system_lock);
                LOG(LOG_LEVEL_WARN, "Resource %s already locked exclusively", resource_name);
                return NULL;
            }
            existing_lock->state = LOCK_STATE_WAITING;
        }
    }

    // Create new lock request
    lock_request_t *lock_request = malloc(sizeof(lock_request_t));
    if (!lock_request) {
        pthread_mutex_unlock(&system->system_lock);
        return NULL;
    }

    lock_request->requester = system->local_node;
    lock_request->resource.resource_name = strdup(resource_name);
    lock_request->resource.resource_id = rand();
    lock_request->requested_mode = mode;
    lock_request->granted_mode = mode;
    lock_request->state = LOCK_STATE_GRANTED;
    lock_request->flags = flags;
    lock_request->request_time = time(NULL);
    lock_request->timeout = (flags & LOCK_FLAG_NOWAIT) ? 0 : time(NULL) + 30;

    // Add to active locks
    system->active_locks[system->current_locks] = lock_request;
    system->current_locks++;

    // Update statistics
    system->stats.total_lock_requests++;
    system->stats.successful_locks++;

    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Acquired %s lock on resource %s", 
        get_lock_mode_string(mode), resource_name);

    return lock_request;
}

// Release Lock
bool release_lock(
    dlm_system_t *system,
    lock_request_t *lock_request
) {
    if (!system || !lock_request) return false;

    pthread_mutex_lock(&system->system_lock);

    // Find and remove the lock
    for (size_t i = 0; i < system->current_locks; i++) {
        if (system->active_locks[i] == lock_request) {
            // Free resource name
            free(lock_request->resource.resource_name);

            // Shift remaining locks
            for (size_t j = i; j < system->current_locks - 1; j++) {
                system->active_locks[j] = system->active_locks[j + 1];
            }
            system->current_locks--;

            // Free lock request
            free(lock_request);

            pthread_mutex_unlock(&system->system_lock);

            LOG(LOG_LEVEL_DEBUG, "Released lock on resource");
            return true;
        }
    }

    pthread_mutex_unlock(&system->system_lock);
    return false;
}

// Convert Lock Mode
bool convert_lock_mode(
    dlm_system_t *system,
    lock_request_t *lock_request,
    lock_mode_t new_mode
) {
    if (!system || !lock_request) return false;

    pthread_mutex_lock(&system->system_lock);

    // Check mode compatibility
    if (lock_request->granted_mode == LOCK_MODE_EXCLUSIVE && 
        new_mode != LOCK_MODE_NULL) {
        lock_request->state = LOCK_STATE_CONVERTING;
        pthread_mutex_unlock(&system->system_lock);
        
        LOG(LOG_LEVEL_WARN, "Cannot convert from exclusive lock");
        return false;
    }

    lock_request->requested_mode = new_mode;
    lock_request->granted_mode = new_mode;
    lock_request->state = LOCK_STATE_GRANTED;

    system->stats.mode_conversions++;

    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Converted lock mode to %s", 
        get_lock_mode_string(new_mode));

    return true;
}

// Find Lock by Resource
lock_request_t* find_lock_by_resource(
    dlm_system_t *system,
    const char *resource_name
) {
    if (!system || !resource_name) return NULL;

    for (size_t i = 0; i < system->current_locks; i++) {
        if (strcmp(system->active_locks[i]->resource.resource_name, resource_name) == 0) {
            return system->active_locks[i];
        }
    }

    return NULL;
}

// Detect Deadlock
bool detect_deadlock(
    dlm_system_t *system,
    lock_request_t *lock_request
) {
    if (!system || !lock_request) return false;

    pthread_mutex_lock(&system->system_lock);

    // Simple deadlock detection based on timeout
    time_t current_time = time(NULL);
    if (lock_request->timeout > 0 && current_time > lock_request->timeout) {
        lock_request->state = LOCK_STATE_DEADLOCK;
        system->stats.deadlock_detections++;

        pthread_mutex_unlock(&system->system_lock);

        LOG(LOG_LEVEL_ERROR, "Deadlock detected for resource %s", 
            lock_request->resource.resource_name);

        return true;
    }

    pthread_mutex_unlock(&system->system_lock);
    return false;
}

// Print Lock Statistics
void print_lock_stats(dlm_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->system_lock);

    printf("\nDistributed Lock Management Statistics:\n");
    printf("--------------------------------------\n");
    printf("Node ID:               %lu\n", system->local_node.node_id);
    printf("Node Name:             %s\n", system->local_node.node_name);
    printf("Total Lock Requests:   %lu\n", system->stats.total_lock_requests);
    printf("Successful Locks:      %lu\n", system->stats.successful_locks);
    printf("Failed Locks:          %lu\n", system->stats.failed_locks);
    printf("Deadlock Detections:   %lu\n", system->stats.deadlock_detections);
    printf("Mode Conversions:      %lu\n", system->stats.mode_conversions);
    printf("Current Active Locks:  %zu\n", system->current_locks);

    pthread_mutex_unlock(&system->system_lock);
}

// Destroy Distributed Lock Management System
void destroy_dlm_system(dlm_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->system_lock);

    // Free active locks
    for (size_t i = 0; i < system->current_locks; i++) {
        free(system->active_locks[i]->resource.resource_name);
        free(system->active_locks[i]);
    }

    // Free node name
    free(system->local_node.node_name);

    // Free locks array
    free(system->active_locks);

    pthread_mutex_unlock(&system->system_lock);
    pthread_mutex_destroy(&system->system_lock);

    free(system);
}

// Demonstrate Distributed Lock Management System
void demonstrate_dlm_system() {
    // Create DLM System
    dlm_system_t *dlm_system = create_dlm_system(
        1234,           // Node ID
        "node_01",      // Node Name
        100             // Max Locks
    );

    // Simulate Lock Scenarios
    lock_request_t *locks[5];

    // Acquire Locks
    locks[0] = acquire_lock(
        dlm_system, 
        "resource_1", 
        LOCK_MODE_SHARED, 
        LOCK_FLAG_NONE
    );

    locks[1] = acquire_lock(
        dlm_system, 
        "resource_2", 
        LOCK_MODE_EXCLUSIVE, 
        LOCK_FLAG_NOWAIT
    );

    // Convert Lock Mode
    if (locks[0]) {
        convert_lock_mode(
            dlm_system, 
            locks[0], 
            LOCK_MODE_EXCLUSIVE
        );
    }

    // Simulate Deadlock Detection
    if (locks[1]) {
        detect_deadlock(dlm_system, locks[1]);
    }

    // Release Locks
    for (int i = 0; i < 2; i++) {
        if (locks[i]) {
            release_lock(dlm_system, locks[i]);
        }
    }

    // Print Statistics
    print_lock_stats(dlm_system);

    // Cleanup
    destroy_dlm_system(dlm_system);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_dlm_system();

    return 0;
}
