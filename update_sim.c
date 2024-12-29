#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

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

// RCU Update Constants
#define MAX_CPUS           8
#define MAX_UPDATES        256
#define MAX_CALLBACKS      128
#define SYNC_INTERVAL_MS   10
#define MAX_BATCH_SIZE     16

// Update Types
typedef enum {
    UPDATE_IMMEDIATE,
    UPDATE_DEFERRED,
    UPDATE_ASYNC
} update_type_t;

// Update States
typedef enum {
    UPDATE_PENDING,
    UPDATE_IN_PROGRESS,
    UPDATE_COMPLETED,
    UPDATE_FAILED
} update_state_t;

// CPU States
typedef enum {
    CPU_ACTIVE,
    CPU_IDLE,
    CPU_OFFLINE
} cpu_state_t;

// Update Structure
typedef struct {
    unsigned int id;
    update_type_t type;
    update_state_t state;
    void *old_data;
    void *new_data;
    size_t data_size;
    uint64_t timestamp;
    uint64_t completion_time;
    bool needs_sync;
} update_t;

// Callback Structure
typedef struct {
    void (*func)(void *);
    void *arg;
    uint64_t deadline;
} callback_t;

// CPU Structure
typedef struct {
    unsigned int id;
    cpu_state_t state;
    update_t *current_update;
    callback_t callbacks[MAX_CALLBACKS];
    size_t nr_callbacks;
    uint64_t last_sync;
    pthread_mutex_t lock;
} rcu_cpu_t;

// Update Statistics
typedef struct {
    unsigned long updates_processed;
    unsigned long updates_deferred;
    unsigned long updates_failed;
    unsigned long callbacks_executed;
    unsigned long sync_points;
    double avg_update_time;
} update_stats_t;

// Update Manager
typedef struct {
    rcu_cpu_t cpus[MAX_CPUS];
    size_t nr_cpus;
    update_t updates[MAX_UPDATES];
    size_t nr_updates;
    pthread_t sync_thread;
    bool running;
    pthread_mutex_t manager_lock;
    update_stats_t stats;
} update_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_update_type_string(update_type_t type);
const char* get_update_state_string(update_state_t state);
const char* get_cpu_state_string(cpu_state_t state);

update_manager_t* create_update_manager(size_t nr_cpus);
void destroy_update_manager(update_manager_t *manager);

bool queue_update(update_manager_t *manager, update_type_t type,
                void *old_data, void *new_data, size_t data_size);
bool process_update(update_manager_t *manager, update_t *update);
void sync_updates(update_manager_t *manager);

void register_callback(update_manager_t *manager, unsigned int cpu_id,
                     void (*func)(void *), void *arg, uint64_t deadline);
void process_callbacks(update_manager_t *manager, unsigned int cpu_id);
void* sync_thread(void *arg);

void print_update_stats(update_manager_t *manager);
void demonstrate_updates(void);

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

// Utility Function: Get Update Type String
const char* get_update_type_string(update_type_t type) {
    switch(type) {
        case UPDATE_IMMEDIATE: return "IMMEDIATE";
        case UPDATE_DEFERRED:  return "DEFERRED";
        case UPDATE_ASYNC:     return "ASYNC";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Update State String
const char* get_update_state_string(update_state_t state) {
    switch(state) {
        case UPDATE_PENDING:     return "PENDING";
        case UPDATE_IN_PROGRESS: return "IN_PROGRESS";
        case UPDATE_COMPLETED:   return "COMPLETED";
        case UPDATE_FAILED:      return "FAILED";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get CPU State String
const char* get_cpu_state_string(cpu_state_t state) {
    switch(state) {
        case CPU_ACTIVE:  return "ACTIVE";
        case CPU_IDLE:    return "IDLE";
        case CPU_OFFLINE: return "OFFLINE";
        default: return "UNKNOWN";
    }
}

// Create Update Manager
update_manager_t* create_update_manager(size_t nr_cpus) {
    if (nr_cpus > MAX_CPUS) {
        LOG(LOG_LEVEL_ERROR, "Number of CPUs exceeds maximum");
        return NULL;
    }

    update_manager_t *manager = malloc(sizeof(update_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate update manager");
        return NULL;
    }

    // Initialize CPUs
    for (size_t i = 0; i < nr_cpus; i++) {
        manager->cpus[i].id = i;
        manager->cpus[i].state = CPU_ACTIVE;
        manager->cpus[i].current_update = NULL;
        manager->cpus[i].nr_callbacks = 0;
        manager->cpus[i].last_sync = 0;
        pthread_mutex_init(&manager->cpus[i].lock, NULL);
    }

    manager->nr_cpus = nr_cpus;
    manager->nr_updates = 0;
    pthread_mutex_init(&manager->manager_lock, NULL);
    memset(&manager->stats, 0, sizeof(update_stats_t));
    manager->running = true;

    // Start sync thread
    pthread_create(&manager->sync_thread, NULL, sync_thread, manager);

    LOG(LOG_LEVEL_DEBUG, "Created update manager with %zu CPUs", nr_cpus);
    return manager;
}

// Queue Update
bool queue_update(update_manager_t *manager, update_type_t type,
                void *old_data, void *new_data, size_t data_size) {
    if (!manager || !new_data) return false;

    pthread_mutex_lock(&manager->manager_lock);

    if (manager->nr_updates >= MAX_UPDATES) {
        pthread_mutex_unlock(&manager->manager_lock);
        return false;
    }

    update_t *update = &manager->updates[manager->nr_updates++];
    update->id = manager->nr_updates;
    update->type = type;
    update->state = UPDATE_PENDING;
    update->old_data = old_data;
    update->new_data = malloc(data_size);
    if (!update->new_data) {
        manager->nr_updates--;
        pthread_mutex_unlock(&manager->manager_lock);
        return false;
    }

    memcpy(update->new_data, new_data, data_size);
    update->data_size = data_size;
    update->timestamp = time(NULL);
    update->completion_time = 0;
    update->needs_sync = (type != UPDATE_IMMEDIATE);

    pthread_mutex_unlock(&manager->manager_lock);

    LOG(LOG_LEVEL_DEBUG, "Queued %s update %u",
        get_update_type_string(type), update->id);
    return true;
}

// Process Update
bool process_update(update_manager_t *manager, update_t *update) {
    if (!manager || !update) return false;

    bool success = false;
    update->state = UPDATE_IN_PROGRESS;

    // Simulate update processing
    switch (update->type) {
        case UPDATE_IMMEDIATE:
            // Process immediately
            success = true;
            break;

        case UPDATE_DEFERRED:
            // Wait for sync point
            if (update->needs_sync) {
                manager->stats.updates_deferred++;
                return false;
            }
            success = true;
            break;

        case UPDATE_ASYNC:
            // Process asynchronously
            success = true;
            break;
    }

    if (success) {
        update->state = UPDATE_COMPLETED;
        update->completion_time = time(NULL);
        manager->stats.updates_processed++;

        // Update average processing time
        uint64_t process_time = update->completion_time - update->timestamp;
        manager->stats.avg_update_time = 
            (manager->stats.avg_update_time * 
             (manager->stats.updates_processed - 1) + process_time) /
            manager->stats.updates_processed;
    } else {
        update->state = UPDATE_FAILED;
        manager->stats.updates_failed++;
    }

    return success;
}

// Sync Updates
void sync_updates(update_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    // Process pending updates in batches
    size_t batch_size = 0;
    for (size_t i = 0; i < manager->nr_updates && batch_size < MAX_BATCH_SIZE; i++) {
        update_t *update = &manager->updates[i];
        
        if (update->state == UPDATE_PENDING && update->needs_sync) {
            update->needs_sync = false;
            process_update(manager, update);
            batch_size++;
        }
    }

    // Remove completed updates
    size_t i = 0;
    while (i < manager->nr_updates) {
        if (manager->updates[i].state == UPDATE_COMPLETED) {
            free(manager->updates[i].new_data);
            
            // Shift remaining updates
            memmove(&manager->updates[i], &manager->updates[i + 1],
                   (manager->nr_updates - i - 1) * sizeof(update_t));
            manager->nr_updates--;
        } else {
            i++;
        }
    }

    manager->stats.sync_points++;

    pthread_mutex_unlock(&manager->manager_lock);
}

// Register Callback
void register_callback(update_manager_t *manager, unsigned int cpu_id,
                     void (*func)(void *), void *arg, uint64_t deadline) {
    if (!manager || cpu_id >= manager->nr_cpus || !func) return;

    rcu_cpu_t *cpu = &manager->cpus[cpu_id];
    pthread_mutex_lock(&cpu->lock);

    if (cpu->nr_callbacks < MAX_CALLBACKS) {
        callback_t *cb = &cpu->callbacks[cpu->nr_callbacks++];
        cb->func = func;
        cb->arg = arg;
        cb->deadline = deadline;
    }

    pthread_mutex_unlock(&cpu->lock);
}

// Process Callbacks
void process_callbacks(update_manager_t *manager, unsigned int cpu_id) {
    if (!manager || cpu_id >= manager->nr_cpus) return;

    rcu_cpu_t *cpu = &manager->cpus[cpu_id];
    pthread_mutex_lock(&cpu->lock);

    uint64_t now = time(NULL);
    size_t i = 0;
    while (i < cpu->nr_callbacks) {
        callback_t *cb = &cpu->callbacks[i];
        if (now >= cb->deadline) {
            // Execute callback
            cb->func(cb->arg);
            manager->stats.callbacks_executed++;

            // Remove callback
            memmove(&cpu->callbacks[i], &cpu->callbacks[i + 1],
                   (cpu->nr_callbacks - i - 1) * sizeof(callback_t));
            cpu->nr_callbacks--;
        } else {
            i++;
        }
    }

    pthread_mutex_unlock(&cpu->lock);
}

// Sync Thread
void* sync_thread(void *arg) {
    update_manager_t *manager = (update_manager_t*)arg;

    while (manager->running) {
        // Sync updates
        sync_updates(manager);

        // Process callbacks on each CPU
        for (size_t i = 0; i < manager->nr_cpus; i++) {
            process_callbacks(manager, i);
        }

        usleep(SYNC_INTERVAL_MS * 1000);
    }

    return NULL;
}

// Print Update Statistics
void print_update_stats(update_manager_t *manager) {
    if (!manager) return;

    printf("\nUpdate Manager Statistics:\n");
    printf("------------------------\n");
    printf("Updates Processed:   %lu\n", manager->stats.updates_processed);
    printf("Updates Deferred:    %lu\n", manager->stats.updates_deferred);
    printf("Updates Failed:      %lu\n", manager->stats.updates_failed);
    printf("Callbacks Executed:  %lu\n", manager->stats.callbacks_executed);
    printf("Sync Points:        %lu\n", manager->stats.sync_points);
    printf("Avg Update Time:    %.2f seconds\n", manager->stats.avg_update_time);

    // Print CPU details
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        rcu_cpu_t *cpu = &manager->cpus[i];
        printf("\nCPU %zu:\n", i);
        printf("  State: %s\n", get_cpu_state_string(cpu->state));
        printf("  Pending Callbacks: %zu\n", cpu->nr_callbacks);
        if (cpu->current_update) {
            printf("  Current Update: %u (%s)\n",
                cpu->current_update->id,
                get_update_state_string(cpu->current_update->state));
        }
    }
}

// Destroy Update Manager
void destroy_update_manager(update_manager_t *manager) {
    if (!manager) return;

    // Stop sync thread
    manager->running = false;
    pthread_join(manager->sync_thread, NULL);

    pthread_mutex_lock(&manager->manager_lock);

    // Clean up updates
    for (size_t i = 0; i < manager->nr_updates; i++) {
        free(manager->updates[i].new_data);
    }

    // Clean up CPU mutexes
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        pthread_mutex_destroy(&manager->cpus[i].lock);
    }

    pthread_mutex_unlock(&manager->manager_lock);
    pthread_mutex_destroy(&manager->manager_lock);

    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed update manager");
}

// Example callback function
void cleanup_callback(void *arg) {
    int *value = (int*)arg;
    LOG(LOG_LEVEL_DEBUG, "Cleanup callback executed with value: %d", *value);
}

// Demonstrate Updates
void demonstrate_updates(void) {
    // Create update manager with 4 CPUs
    update_manager_t *manager = create_update_manager(4);
    if (!manager) return;

    printf("Starting update manager demonstration...\n");

    // Scenario 1: Immediate updates
    printf("\nScenario 1: Immediate updates\n");
    for (int i = 0; i < 5; i++) {
        int old_value = i;
        int new_value = i * 10;
        queue_update(manager, UPDATE_IMMEDIATE, &old_value, &new_value, sizeof(int));
    }

    // Scenario 2: Deferred updates
    printf("\nScenario 2: Deferred updates\n");
    for (int i = 5; i < 10; i++) {
        int old_value = i;
        int new_value = i * 10;
        queue_update(manager, UPDATE_DEFERRED, &old_value, &new_value, sizeof(int));
    }

    // Scenario 3: Async updates with callbacks
    printf("\nScenario 3: Async updates with callbacks\n");
    for (int i = 0; i < 5; i++) {
        int *value = malloc(sizeof(int));
        *value = i;
        register_callback(manager, i % manager->nr_cpus,
                        cleanup_callback, value,
                        time(NULL) + 2);
    }

    // Wait for updates to process
    sleep(3);

    // Print statistics
    print_update_stats(manager);

    // Cleanup
    destroy_update_manager(manager);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_updates();

    return 0;
}
