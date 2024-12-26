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

// BPF Local Storage Types
typedef enum {
    BPF_STORAGE_TYPE_TASK,
    BPF_STORAGE_TYPE_INODE,
    BPF_STORAGE_TYPE_SOCKET,
    BPF_STORAGE_TYPE_CGROUP,
    BPF_STORAGE_TYPE_DEVICE
} bpf_storage_type_t;

// Storage Access Permissions
typedef enum {
    BPF_STORAGE_PERM_NONE     = 0,
    BPF_STORAGE_PERM_READ     = 1 << 0,
    BPF_STORAGE_PERM_WRITE    = 1 << 1,
    BPF_STORAGE_PERM_EXEC     = 1 << 2
} bpf_storage_perm_t;

// Storage Flags
typedef enum {
    BPF_STORAGE_FLAG_ATOMIC   = 1 << 0,
    BPF_STORAGE_FLAG_PERSIST  = 1 << 1,
    BPF_STORAGE_FLAG_SHARED   = 1 << 2,
    BPF_STORAGE_FLAG_NOCOPY   = 1 << 3
} bpf_storage_flags_t;

// Local Storage Entry
typedef struct bpf_local_storage_entry {
    uint64_t key;
    void *value;
    size_t value_size;
    
    bpf_storage_type_t type;
    bpf_storage_perm_t permissions;
    bpf_storage_flags_t flags;
    
    time_t creation_time;
    time_t last_modified;
    
    struct bpf_local_storage_entry *next;
} bpf_local_storage_entry_t;

// Local Storage Management Statistics
typedef struct {
    unsigned long total_entries_created;
    unsigned long total_entries_deleted;
    unsigned long total_lookups;
    unsigned long total_updates;
    unsigned long collision_resolutions;
    unsigned long atomic_operations;
} bpf_storage_stats_t;

// Local Storage Management System
typedef struct {
    bpf_local_storage_entry_t **buckets;
    size_t bucket_count;
    size_t total_entries;
    
    bpf_storage_stats_t stats;
    pthread_mutex_t system_lock;
} bpf_local_storage_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_storage_type_string(bpf_storage_type_t type);
const char* get_storage_perm_string(bpf_storage_perm_t perm);

bpf_local_storage_system_t* create_bpf_storage_system(size_t bucket_count);
void destroy_bpf_storage_system(bpf_local_storage_system_t *system);

bpf_local_storage_entry_t* create_storage_entry(
    bpf_local_storage_system_t *system,
    uint64_t key,
    void *value,
    size_t value_size,
    bpf_storage_type_t type,
    bpf_storage_perm_t permissions,
    bpf_storage_flags_t flags
);

bool delete_storage_entry(
    bpf_local_storage_system_t *system,
    uint64_t key
);

void* lookup_storage_entry(
    bpf_local_storage_system_t *system,
    uint64_t key
);

bool update_storage_entry(
    bpf_local_storage_system_t *system,
    uint64_t key,
    void *new_value,
    size_t new_value_size
);

bool atomic_storage_operation(
    bpf_local_storage_system_t *system,
    uint64_t key,
    void *update_func(void *current_value)
);

void print_bpf_storage_stats(bpf_local_storage_system_t *system);
void demonstrate_bpf_storage_system();

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

// Utility Function: Get Storage Type String
const char* get_storage_type_string(bpf_storage_type_t type) {
    switch(type) {
        case BPF_STORAGE_TYPE_TASK:     return "TASK";
        case BPF_STORAGE_TYPE_INODE:    return "INODE";
        case BPF_STORAGE_TYPE_SOCKET:   return "SOCKET";
        case BPF_STORAGE_TYPE_CGROUP:   return "CGROUP";
        case BPF_STORAGE_TYPE_DEVICE:   return "DEVICE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Storage Permission String
const char* get_storage_perm_string(bpf_storage_perm_t perm) {
    static char perm_str[64];
    snprintf(perm_str, sizeof(perm_str), "%s%s%s",
        (perm & BPF_STORAGE_PERM_READ)  ? "R" : "-",
        (perm & BPF_STORAGE_PERM_WRITE) ? "W" : "-",
        (perm & BPF_STORAGE_PERM_EXEC)  ? "X" : "-"
    );
    return perm_str;
}

// Hash Function for Key Distribution
static size_t hash_key(uint64_t key, size_t bucket_count) {
    // Simple hash function to distribute keys
    return (key * 2654435761) % bucket_count;
}

// Create BPF Local Storage System
bpf_local_storage_system_t* create_bpf_storage_system(size_t bucket_count) {
    bpf_local_storage_system_t *system = malloc(sizeof(bpf_local_storage_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate BPF storage system");
        return NULL;
    }

    system->buckets = calloc(bucket_count, sizeof(bpf_local_storage_entry_t*));
    if (!system->buckets) {
        free(system);
        return NULL;
    }

    system->bucket_count = bucket_count;
    system->total_entries = 0;

    // Reset statistics
    memset(&system->stats, 0, sizeof(bpf_storage_stats_t));

    // Initialize system lock
    pthread_mutex_init(&system->system_lock, NULL);

    LOG(LOG_LEVEL_DEBUG, "Created BPF Storage System with %zu buckets", bucket_count);

    return system;
}

// Create Storage Entry
bpf_local_storage_entry_t* create_storage_entry(
    bpf_local_storage_system_t *system,
    uint64_t key,
    void *value,
    size_t value_size,
    bpf_storage_type_t type,
    bpf_storage_perm_t permissions,
    bpf_storage_flags_t flags
) {
    if (!system) return NULL;

    pthread_mutex_lock(&system->system_lock);

    // Check for existing entry
    size_t bucket_index = hash_key(key, system->bucket_count);
    bpf_local_storage_entry_t *existing = system->buckets[bucket_index];
    
    while (existing) {
        if (existing->key == key) {
            pthread_mutex_unlock(&system->system_lock);
            LOG(LOG_LEVEL_WARN, "Entry with key %lu already exists", key);
            return NULL;
        }
        existing = existing->next;
    }

    // Create new entry
    bpf_local_storage_entry_t *entry = malloc(sizeof(bpf_local_storage_entry_t));
    if (!entry) {
        pthread_mutex_unlock(&system->system_lock);
        return NULL;
    }

    // Allocate value
    entry->value = malloc(value_size);
    if (!entry->value) {
        free(entry);
        pthread_mutex_unlock(&system->system_lock);
        return NULL;
    }

    // Copy value
    memcpy(entry->value, value, value_size);

    // Set entry properties
    entry->key = key;
    entry->value_size = value_size;
    entry->type = type;
    entry->permissions = permissions;
    entry->flags = flags;
    
    entry->creation_time = time(NULL);
    entry->last_modified = entry->creation_time;

    // Link into bucket (chaining for collision resolution)
    entry->next = system->buckets[bucket_index];
    system->buckets[bucket_index] = entry;

    system->total_entries++;
    system->stats.total_entries_created++;

    // Check for collisions
    size_t collision_count = 0;
    existing = entry->next;
    while (existing) {
        collision_count++;
        existing = existing->next;
    }
    if (collision_count > 0) {
        system->stats.collision_resolutions += collision_count;
        LOG(LOG_LEVEL_DEBUG, "Collision resolved in bucket %zu, count: %zu", 
            bucket_index, collision_count);
    }

    pthread_mutex_unlock(&system->system_lock);

    LOG(LOG_LEVEL_DEBUG, "Created %s storage entry, Key %lu, Size %zu", 
        get_storage_type_string(type), key, value_size);

    return entry;
}

// Delete Storage Entry
bool delete_storage_entry(
    bpf_local_storage_system_t *system,
    uint64_t key
) {
    if (!system) return false;

    pthread_mutex_lock(&system->system_lock);

    size_t bucket_index = hash_key(key, system->bucket_count);
    bpf_local_storage_entry_t *current = system->buckets[bucket_index];
    bpf_local_storage_entry_t *prev = NULL;

    while (current) {
        if (current->key == key) {
            // Unlink entry
            if (prev) {
                prev->next = current->next;
            } else {
                system->buckets[bucket_index] = current->next;
            }

            // Free entry
            free(current->value);
            free(current);

            system->total_entries--;
            system->stats.total_entries_deleted++;

            pthread_mutex_unlock(&system->system_lock);

            LOG(LOG_LEVEL_DEBUG, "Deleted storage entry with key %lu", key);
            return true;
        }

        prev = current;
        current = current->next;
    }

    pthread_mutex_unlock(&system->system_lock);
    return false;
}

// Lookup Storage Entry
void* lookup_storage_entry(
    bpf_local_storage_system_t *system,
    uint64_t key
) {
    if (!system) return NULL;

    pthread_mutex_lock(&system->system_lock);

    size_t bucket_index = hash_key(key, system->bucket_count);
    bpf_local_storage_entry_t *current = system->buckets[bucket_index];

    while (current) {
        if (current->key == key) {
            system->stats.total_lookups++;
            current->last_modified = time(NULL);
            
            pthread_mutex_unlock(&system->system_lock);
            return current->value;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&system->system_lock);
    return NULL;
}

// Update Storage Entry
bool update_storage_entry(
    bpf_local_storage_system_t *system,
    uint64_t key,
    void *new_value,
    size_t new_value_size
) {
    if (!system) return false;

    pthread_mutex_lock(&system->system_lock);

    size_t bucket_index = hash_key(key, system->bucket_count);
    bpf_local_storage_entry_t *current = system->buckets[bucket_index];

    while (current) {
        if (current->key == key) {
            // Reallocate value if size changed
            if (current->value_size != new_value_size) {
                void *temp = realloc(current->value, new_value_size);
                if (!temp) {
                    pthread_mutex_unlock(&system->system_lock);
                    return false;
                }
                current->value = temp;
                current->value_size = new_value_size;
            }

            // Copy new value
            memcpy(current->value, new_value, new_value_size);

            current->last_modified = time(NULL);
            system->stats.total_updates++;

            pthread_mutex_unlock(&system->system_lock);

            LOG(LOG_LEVEL_DEBUG, "Updated storage entry with key %lu", key);
            return true;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&system->system_lock);
    return false;
}

// Atomic Storage Operation
bool atomic_storage_operation(
    bpf_local_storage_system_t *system,
    uint64_t key,
    void *update_func(void *current_value)
) {
    if (!system || !update_func) return false;

    pthread_mutex_lock(&system->system_lock);

    size_t bucket_index = hash_key(key, system->bucket_count);
    bpf_local_storage_entry_t *current = system->buckets[bucket_index];

    while (current) {
        if (current->key == key) {
            // Perform atomic update
            void *updated_value = update_func(current->value);
            if (updated_value) {
                memcpy(current->value, updated_value, current->value_size);
                current->last_modified = time(NULL);
                system->stats.atomic_operations++;

                pthread_mutex_unlock(&system->system_lock);

                LOG(LOG_LEVEL_DEBUG, "Performed atomic operation on entry %lu", key);
                return true;
            }

            pthread_mutex_unlock(&system->system_lock);
            return false;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&system->system_lock);
    return false;
}

// Print BPF Storage Statistics
void print_bpf_storage_stats(bpf_local_storage_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->system_lock);

    printf("\nBPF Local Storage System Statistics:\n");
    printf("------------------------------------\n");
    printf("Total Entries Created:      %lu\n", system->stats.total_entries_created);
    printf("Total Entries Deleted:      %lu\n", system->stats.total_entries_deleted);
    printf("Total Lookups:              %lu\n", system->stats.total_lookups);
    printf("Total Updates:              %lu\n", system->stats.total_updates);
    printf("Collision Resolutions:      %lu\n", system->stats.collision_resolutions);
    printf("Atomic Operations:          %lu\n", system->stats.atomic_operations);
    printf("Current Active Entries:     %zu\n", system->total_entries);

    pthread_mutex_unlock(&system->system_lock);
}

// Destroy BPF Local Storage System
void destroy_bpf_storage_system(bpf_local_storage_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->system_lock);

    // Free all entries in each bucket
    for (size_t i = 0; i < system->bucket_count; i++) {
        bpf_local_storage_entry_t *current = system->buckets[i];
        while (current) {
            bpf_local_storage_entry_t *next = current->next;
            free(current->value);
            free(current);
            current = next;
        }
    }

    free(system->buckets);

    pthread_mutex_unlock(&system->system_lock);
    pthread_mutex_destroy(&system->system_lock);

    free(system);
}

// Atomic Update Function Example
void* increment_counter(void *current_value) {
    int *counter = (int*)current_value;
    (*counter)++;
    return current_value;
}

// Demonstrate BPF Local Storage System
void demonstrate_bpf_storage_system() {
    // Create BPF Storage System
    bpf_local_storage_system_t *storage_system = create_bpf_storage_system(100);

    // Create Sample Entries
    int values[5] = {10, 20, 30, 40, 50};
    bpf_local_storage_entry_t *entries[5];

    for (int i = 0; i < 5; i++) {
        entries[i] = create_storage_entry(
            storage_system,
            i + 1000,  // Unique keys
            &values[i],
            sizeof(int),
            (bpf_storage_type_t)(i % 5),  // Cycle through storage types
            BPF_STORAGE_PERM_READ | BPF_STORAGE_PERM_WRITE,
            BPF_STORAGE_FLAG_ATOMIC
        );
    }

    // Lookup Entries
    for (int i = 0; i < 5; i++) {
        if (entries[i]) {
            int *value = (int*)lookup_storage_entry(
                storage_system, 
                entries[i]->key
            );
            
            if (value) {
                LOG(LOG_LEVEL_INFO, "Looked up value: %d", *value);
            }
        }
    }

    // Update Entries
    for (int i = 0; i < 3; i++) {
        if (entries[i]) {
            int new_value = values[i] * 2;
            update_storage_entry(
                storage_system, 
                entries[i]->key, 
                &new_value, 
                sizeof(int)
            );
        }
    }

    // Atomic Operations
    for (int i = 0; i < 2; i++) {
        if (entries[i]) {
            atomic_storage_operation(
                storage_system, 
                entries[i]->key, 
                increment_counter
            );
        }
    }

    // Print Statistics
    print_bpf_storage_stats(storage_system);

    // Cleanup
    destroy_bpf_storage_system(storage_system);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_bpf_storage_system();

    return 0;
}
