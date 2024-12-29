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

// RCU Constants
#define MAX_CPUS        8
#define MAX_NODES       1024
#define GRACE_PERIOD_MS 10
#define MAX_CALLBACKS   256

// RCU Node States
typedef enum {
    NODE_ACTIVE,
    NODE_DELETED,
    NODE_RECYCLED
} node_state_t;

// RCU CPU States
typedef enum {
    CPU_ONLINE,
    CPU_OFFLINE,
    CPU_IDLE
} cpu_state_t;

// RCU Node Structure
typedef struct rcu_node {
    int key;
    void *data;
    size_t data_size;
    node_state_t state;
    uint64_t version;
    struct rcu_node *next;
} rcu_node_t;

// RCU Callback Structure
typedef struct {
    void (*func)(void *);
    void *arg;
    uint64_t grace_period;
} rcu_callback_t;

// CPU Structure
typedef struct {
    unsigned int id;
    cpu_state_t state;
    uint64_t quiescent_count;
    uint64_t last_quiescent;
    rcu_callback_t callbacks[MAX_CALLBACKS];
    size_t nr_callbacks;
    pthread_mutex_t lock;
} rcu_cpu_t;

// RCU Statistics
typedef struct {
    unsigned long updates;
    unsigned long reads;
    unsigned long grace_periods;
    unsigned long callbacks_invoked;
    unsigned long nodes_freed;
    double avg_grace_period;
} rcu_stats_t;

// RCU Tree Manager
typedef struct {
    rcu_node_t *root;
    rcu_cpu_t cpus[MAX_CPUS];
    size_t nr_cpus;
    uint64_t grace_period;
    pthread_t grace_thread;
    bool running;
    pthread_mutex_t tree_lock;
    rcu_stats_t stats;
} rcu_tree_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_node_state_string(node_state_t state);
const char* get_cpu_state_string(cpu_state_t state);

rcu_tree_t* create_rcu_tree(size_t nr_cpus);
void destroy_rcu_tree(rcu_tree_t *tree);

rcu_node_t* rcu_alloc_node(int key, void *data, size_t data_size);
void rcu_free_node(rcu_node_t *node);

bool rcu_insert(rcu_tree_t *tree, int key, void *data, size_t data_size);
bool rcu_delete(rcu_tree_t *tree, int key);
void* rcu_read(rcu_tree_t *tree, int key);

void rcu_read_lock(rcu_tree_t *tree, unsigned int cpu_id);
void rcu_read_unlock(rcu_tree_t *tree, unsigned int cpu_id);
void rcu_synchronize(rcu_tree_t *tree);

void rcu_register_callback(rcu_tree_t *tree, unsigned int cpu_id,
                         void (*func)(void *), void *arg);
void rcu_process_callbacks(rcu_tree_t *tree, unsigned int cpu_id);
void* grace_period_thread(void *arg);

void print_rcu_stats(rcu_tree_t *tree);
void demonstrate_rcu(void);

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

// Utility Function: Get Node State String
const char* get_node_state_string(node_state_t state) {
    switch(state) {
        case NODE_ACTIVE:   return "ACTIVE";
        case NODE_DELETED:  return "DELETED";
        case NODE_RECYCLED: return "RECYCLED";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get CPU State String
const char* get_cpu_state_string(cpu_state_t state) {
    switch(state) {
        case CPU_ONLINE:  return "ONLINE";
        case CPU_OFFLINE: return "OFFLINE";
        case CPU_IDLE:    return "IDLE";
        default: return "UNKNOWN";
    }
}

// Create RCU Tree
rcu_tree_t* create_rcu_tree(size_t nr_cpus) {
    if (nr_cpus > MAX_CPUS) {
        LOG(LOG_LEVEL_ERROR, "Number of CPUs exceeds maximum");
        return NULL;
    }

    rcu_tree_t *tree = malloc(sizeof(rcu_tree_t));
    if (!tree) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate RCU tree");
        return NULL;
    }

    tree->root = NULL;
    tree->nr_cpus = nr_cpus;
    tree->grace_period = 0;
    tree->running = true;
    pthread_mutex_init(&tree->tree_lock, NULL);
    memset(&tree->stats, 0, sizeof(rcu_stats_t));

    // Initialize CPUs
    for (size_t i = 0; i < nr_cpus; i++) {
        tree->cpus[i].id = i;
        tree->cpus[i].state = CPU_ONLINE;
        tree->cpus[i].quiescent_count = 0;
        tree->cpus[i].last_quiescent = 0;
        tree->cpus[i].nr_callbacks = 0;
        pthread_mutex_init(&tree->cpus[i].lock, NULL);
    }

    // Start grace period thread
    pthread_create(&tree->grace_thread, NULL, grace_period_thread, tree);

    LOG(LOG_LEVEL_DEBUG, "Created RCU tree with %zu CPUs", nr_cpus);
    return tree;
}

// Allocate RCU Node
rcu_node_t* rcu_alloc_node(int key, void *data, size_t data_size) {
    rcu_node_t *node = malloc(sizeof(rcu_node_t));
    if (!node) return NULL;

    node->key = key;
    node->data = malloc(data_size);
    if (!node->data) {
        free(node);
        return NULL;
    }

    memcpy(node->data, data, data_size);
    node->data_size = data_size;
    node->state = NODE_ACTIVE;
    node->version = 0;
    node->next = NULL;

    return node;
}

// Insert Node
bool rcu_insert(rcu_tree_t *tree, int key, void *data, size_t data_size) {
    if (!tree || !data) return false;

    pthread_mutex_lock(&tree->tree_lock);

    // Check if key already exists
    rcu_node_t *curr = tree->root;
    while (curr) {
        if (curr->key == key && curr->state == NODE_ACTIVE) {
            pthread_mutex_unlock(&tree->tree_lock);
            return false;
        }
        curr = curr->next;
    }

    // Create new node
    rcu_node_t *node = rcu_alloc_node(key, data, data_size);
    if (!node) {
        pthread_mutex_unlock(&tree->tree_lock);
        return false;
    }

    // Insert at head
    node->next = tree->root;
    tree->root = node;
    tree->stats.updates++;

    pthread_mutex_unlock(&tree->tree_lock);
    return true;
}

// Delete Node
bool rcu_delete(rcu_tree_t *tree, int key) {
    if (!tree) return false;

    pthread_mutex_lock(&tree->tree_lock);

    rcu_node_t *curr = tree->root;
    while (curr) {
        if (curr->key == key && curr->state == NODE_ACTIVE) {
            curr->state = NODE_DELETED;
            curr->version = tree->grace_period;
            tree->stats.updates++;
            pthread_mutex_unlock(&tree->tree_lock);
            return true;
        }
        curr = curr->next;
    }

    pthread_mutex_unlock(&tree->tree_lock);
    return false;
}

// Read Node
void* rcu_read(rcu_tree_t *tree, int key) {
    if (!tree) return NULL;

    void *result = NULL;
    rcu_node_t *curr = tree->root;

    while (curr) {
        if (curr->key == key && curr->state == NODE_ACTIVE) {
            result = curr->data;
            break;
        }
        curr = curr->next;
    }

    tree->stats.reads++;
    return result;
}

// RCU Read Lock
void rcu_read_lock(rcu_tree_t *tree, unsigned int cpu_id) {
    if (!tree || cpu_id >= tree->nr_cpus) return;

    rcu_cpu_t *cpu = &tree->cpus[cpu_id];
    pthread_mutex_lock(&cpu->lock);
    cpu->quiescent_count++;
    pthread_mutex_unlock(&cpu->lock);
}

// RCU Read Unlock
void rcu_read_unlock(rcu_tree_t *tree, unsigned int cpu_id) {
    if (!tree || cpu_id >= tree->nr_cpus) return;

    rcu_cpu_t *cpu = &tree->cpus[cpu_id];
    pthread_mutex_lock(&cpu->lock);
    cpu->last_quiescent = tree->grace_period;
    pthread_mutex_unlock(&cpu->lock);
}

// Register RCU Callback
void rcu_register_callback(rcu_tree_t *tree, unsigned int cpu_id,
                         void (*func)(void *), void *arg) {
    if (!tree || cpu_id >= tree->nr_cpus || !func) return;

    rcu_cpu_t *cpu = &tree->cpus[cpu_id];
    pthread_mutex_lock(&cpu->lock);

    if (cpu->nr_callbacks < MAX_CALLBACKS) {
        rcu_callback_t *cb = &cpu->callbacks[cpu->nr_callbacks++];
        cb->func = func;
        cb->arg = arg;
        cb->grace_period = tree->grace_period;
    }

    pthread_mutex_unlock(&cpu->lock);
}

// Process RCU Callbacks
void rcu_process_callbacks(rcu_tree_t *tree, unsigned int cpu_id) {
    if (!tree || cpu_id >= tree->nr_cpus) return;

    rcu_cpu_t *cpu = &tree->cpus[cpu_id];
    pthread_mutex_lock(&cpu->lock);

    size_t i = 0;
    while (i < cpu->nr_callbacks) {
        rcu_callback_t *cb = &cpu->callbacks[i];
        if (cb->grace_period < tree->grace_period) {
            // Callback is ready to be processed
            cb->func(cb->arg);
            tree->stats.callbacks_invoked++;

            // Remove callback by shifting array
            memmove(&cpu->callbacks[i], &cpu->callbacks[i + 1],
                   (cpu->nr_callbacks - i - 1) * sizeof(rcu_callback_t));
            cpu->nr_callbacks--;
        } else {
            i++;
        }
    }

    pthread_mutex_unlock(&cpu->lock);
}

// Grace Period Thread
void* grace_period_thread(void *arg) {
    rcu_tree_t *tree = (rcu_tree_t*)arg;
    uint64_t start_time;

    while (tree->running) {
        start_time = time(NULL);
        
        // Wait for grace period
        usleep(GRACE_PERIOD_MS * 1000);

        // Check for quiescent state on all CPUs
        bool all_quiescent = true;
        for (size_t i = 0; i < tree->nr_cpus; i++) {
            rcu_cpu_t *cpu = &tree->cpus[i];
            if (cpu->state == CPU_ONLINE && 
                cpu->last_quiescent < tree->grace_period) {
                all_quiescent = false;
                break;
            }
        }

        if (all_quiescent) {
            // Start new grace period
            tree->grace_period++;
            tree->stats.grace_periods++;

            // Update average grace period duration
            uint64_t duration = time(NULL) - start_time;
            tree->stats.avg_grace_period = 
                (tree->stats.avg_grace_period * (tree->stats.grace_periods - 1) +
                 duration) / tree->stats.grace_periods;

            // Process callbacks
            for (size_t i = 0; i < tree->nr_cpus; i++) {
                rcu_process_callbacks(tree, i);
            }

            // Clean up deleted nodes
            pthread_mutex_lock(&tree->tree_lock);
            rcu_node_t *curr = tree->root;
            rcu_node_t *prev = NULL;

            while (curr) {
                if (curr->state == NODE_DELETED && 
                    curr->version < tree->grace_period) {
                    // Remove node
                    if (prev)
                        prev->next = curr->next;
                    else
                        tree->root = curr->next;

                    rcu_node_t *to_free = curr;
                    curr = curr->next;
                    rcu_free_node(to_free);
                    tree->stats.nodes_freed++;
                } else {
                    prev = curr;
                    curr = curr->next;
                }
            }
            pthread_mutex_unlock(&tree->tree_lock);
        }
    }

    return NULL;
}

// Free RCU Node
void rcu_free_node(rcu_node_t *node) {
    if (!node) return;
    free(node->data);
    free(node);
}

// Print RCU Statistics
void print_rcu_stats(rcu_tree_t *tree) {
    if (!tree) return;

    printf("\nRCU Tree Statistics:\n");
    printf("-----------------\n");
    printf("Updates:           %lu\n", tree->stats.updates);
    printf("Reads:            %lu\n", tree->stats.reads);
    printf("Grace Periods:    %lu\n", tree->stats.grace_periods);
    printf("Callbacks Invoked: %lu\n", tree->stats.callbacks_invoked);
    printf("Nodes Freed:      %lu\n", tree->stats.nodes_freed);
    printf("Avg Grace Period: %.2f seconds\n", tree->stats.avg_grace_period);

    // Print CPU details
    for (size_t i = 0; i < tree->nr_cpus; i++) {
        rcu_cpu_t *cpu = &tree->cpus[i];
        printf("\nCPU %zu:\n", i);
        printf("  State: %s\n", get_cpu_state_string(cpu->state));
        printf("  Quiescent Count: %lu\n", cpu->quiescent_count);
        printf("  Pending Callbacks: %zu\n", cpu->nr_callbacks);
    }
}

// Destroy RCU Tree
void destroy_rcu_tree(rcu_tree_t *tree) {
    if (!tree) return;

    // Stop grace period thread
    tree->running = false;
    pthread_join(tree->grace_thread, NULL);

    // Free all nodes
    rcu_node_t *curr = tree->root;
    while (curr) {
        rcu_node_t *next = curr->next;
        rcu_free_node(curr);
        curr = next;
    }

    // Clean up mutexes
    for (size_t i = 0; i < tree->nr_cpus; i++) {
        pthread_mutex_destroy(&tree->cpus[i].lock);
    }
    pthread_mutex_destroy(&tree->tree_lock);

    free(tree);
    LOG(LOG_LEVEL_DEBUG, "Destroyed RCU tree");
}

// Demonstrate RCU
void demonstrate_rcu(void) {
    // Create RCU tree with 4 CPUs
    rcu_tree_t *tree = create_rcu_tree(4);
    if (!tree) return;

    printf("Starting RCU tree demonstration...\n");

    // Scenario 1: Basic operations
    printf("\nScenario 1: Basic operations\n");
    
    // Insert data
    int data1 = 100, data2 = 200, data3 = 300;
    rcu_insert(tree, 1, &data1, sizeof(int));
    rcu_insert(tree, 2, &data2, sizeof(int));
    rcu_insert(tree, 3, &data3, sizeof(int));

    // Read data
    for (unsigned int cpu = 0; cpu < tree->nr_cpus; cpu++) {
        rcu_read_lock(tree, cpu);
        int *value = rcu_read(tree, 2);
        if (value) printf("CPU %u read value: %d\n", cpu, *value);
        rcu_read_unlock(tree, cpu);
    }

    // Delete data
    rcu_delete(tree, 2);

    // Scenario 2: Concurrent operations
    printf("\nScenario 2: Concurrent operations\n");
    
    for (int i = 0; i < 10; i++) {
        // Simulate concurrent reads and updates
        for (unsigned int cpu = 0; cpu < tree->nr_cpus; cpu++) {
            rcu_read_lock(tree, cpu);
            rcu_read(tree, 1);
            rcu_read_unlock(tree, cpu);
        }

        int new_data = i * 1000;
        rcu_insert(tree, i + 10, &new_data, sizeof(int));
        
        if (i % 2 == 0) {
            rcu_delete(tree, i + 8);
        }

        usleep(1000);  // 1ms delay
    }

    // Wait for grace period
    usleep(GRACE_PERIOD_MS * 2000);

    // Print statistics
    print_rcu_stats(tree);

    // Cleanup
    destroy_rcu_tree(tree);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_rcu();

    return 0;
}
