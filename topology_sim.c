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

// Topology Constants
#define MAX_NUMA_NODES    8
#define MAX_SOCKETS      4
#define MAX_CORES        16
#define MAX_THREADS      4
#define CACHE_LINE_SIZE  64

// Cache Levels
typedef enum {
    CACHE_L1,
    CACHE_L2,
    CACHE_L3,
    CACHE_MAX_LEVEL
} cache_level_t;

// CPU States
typedef enum {
    CPU_ONLINE,
    CPU_OFFLINE,
    CPU_HOTPLUG,
    CPU_FAILED
} cpu_state_t;

// Memory Access Types
typedef enum {
    MEM_LOCAL,
    MEM_REMOTE,
    MEM_FOREIGN
} mem_access_t;

// Cache Structure
typedef struct {
    size_t size;
    size_t line_size;
    size_t associativity;
    unsigned int latency;
    bool shared;
} cache_t;

// CPU Core Structure
typedef struct {
    unsigned int id;
    cpu_state_t state;
    cache_t caches[CACHE_MAX_LEVEL];
    unsigned long mips;
    unsigned int frequency;
    struct {
        unsigned long hits;
        unsigned long misses;
        unsigned long cycles;
    } stats;
} cpu_core_t;

// Socket Structure
typedef struct {
    unsigned int id;
    cpu_core_t *cores;
    size_t num_cores;
    cache_t l3_cache;
    unsigned long memory_bandwidth;
} socket_t;

// NUMA Node Structure
typedef struct {
    unsigned int id;
    socket_t *sockets;
    size_t num_sockets;
    size_t memory_size;
    unsigned int memory_latency;
    struct {
        unsigned long local_accesses;
        unsigned long remote_accesses;
        unsigned long memory_used;
    } stats;
} numa_node_t;

// Topology Statistics
typedef struct {
    unsigned long cache_hits;
    unsigned long cache_misses;
    unsigned long memory_accesses;
    unsigned long cpu_migrations;
    double avg_memory_latency;
} topo_stats_t;

// Topology Configuration
typedef struct {
    size_t num_nodes;
    size_t num_sockets_per_node;
    size_t num_cores_per_socket;
    bool track_stats;
} topo_config_t;

// Topology Manager
typedef struct {
    numa_node_t *nodes;
    size_t num_nodes;
    topo_config_t config;
    topo_stats_t stats;
    pthread_mutex_t manager_lock;
} topo_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_cpu_state_string(cpu_state_t state);
const char* get_cache_level_string(cache_level_t level);
const char* get_mem_access_string(mem_access_t type);

topo_manager_t* create_topo_manager(topo_config_t config);
void destroy_topo_manager(topo_manager_t *manager);

bool init_numa_node(numa_node_t *node, unsigned int id, size_t num_sockets);
bool init_socket(socket_t *socket, unsigned int id, size_t num_cores);
bool init_core(cpu_core_t *core, unsigned int id);

unsigned int calculate_memory_latency(
    topo_manager_t *manager,
    unsigned int source_node,
    unsigned int target_node
);

bool access_memory(
    topo_manager_t *manager,
    unsigned int node_id,
    size_t size,
    mem_access_t type
);

void print_topology(topo_manager_t *manager);
void print_topo_stats(topo_manager_t *manager);
void demonstrate_topology(void);

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

// Utility Function: Get CPU State String
const char* get_cpu_state_string(cpu_state_t state) {
    switch(state) {
        case CPU_ONLINE:  return "ONLINE";
        case CPU_OFFLINE: return "OFFLINE";
        case CPU_HOTPLUG: return "HOTPLUG";
        case CPU_FAILED:  return "FAILED";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Cache Level String
const char* get_cache_level_string(cache_level_t level) {
    switch(level) {
        case CACHE_L1: return "L1";
        case CACHE_L2: return "L2";
        case CACHE_L3: return "L3";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Memory Access Type String
const char* get_mem_access_string(mem_access_t type) {
    switch(type) {
        case MEM_LOCAL:   return "LOCAL";
        case MEM_REMOTE:  return "REMOTE";
        case MEM_FOREIGN: return "FOREIGN";
        default: return "UNKNOWN";
    }
}

// Create Topology Manager
topo_manager_t* create_topo_manager(topo_config_t config) {
    topo_manager_t *manager = malloc(sizeof(topo_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate topology manager");
        return NULL;
    }

    manager->nodes = calloc(config.num_nodes, sizeof(numa_node_t));
    if (!manager->nodes) {
        free(manager);
        return NULL;
    }

    // Initialize NUMA nodes
    for (size_t i = 0; i < config.num_nodes; i++) {
        if (!init_numa_node(&manager->nodes[i], i, config.num_sockets_per_node)) {
            for (size_t j = 0; j < i; j++) {
                free(manager->nodes[j].sockets);
            }
            free(manager->nodes);
            free(manager);
            return NULL;
        }
    }

    manager->num_nodes = config.num_nodes;
    manager->config = config;
    memset(&manager->stats, 0, sizeof(topo_stats_t));
    pthread_mutex_init(&manager->manager_lock, NULL);

    LOG(LOG_LEVEL_DEBUG, "Created topology manager with %zu NUMA nodes", 
        config.num_nodes);
    return manager;
}

// Initialize NUMA Node
bool init_numa_node(numa_node_t *node, unsigned int id, size_t num_sockets) {
    node->id = id;
    node->sockets = calloc(num_sockets, sizeof(socket_t));
    if (!node->sockets) return false;

    node->num_sockets = num_sockets;
    node->memory_size = 16UL * 1024 * 1024 * 1024;  // 16GB per node
    node->memory_latency = 100;  // Base memory latency in ns
    memset(&node->stats, 0, sizeof(node->stats));

    // Initialize sockets
    for (size_t i = 0; i < num_sockets; i++) {
        if (!init_socket(&node->sockets[i], i, MAX_CORES)) {
            for (size_t j = 0; j < i; j++) {
                free(node->sockets[j].cores);
            }
            free(node->sockets);
            return false;
        }
    }

    return true;
}

// Initialize Socket
bool init_socket(socket_t *socket, unsigned int id, size_t num_cores) {
    socket->id = id;
    socket->cores = calloc(num_cores, sizeof(cpu_core_t));
    if (!socket->cores) return false;

    socket->num_cores = num_cores;
    socket->memory_bandwidth = 100000;  // MB/s

    // Initialize L3 cache
    socket->l3_cache.size = 32 * 1024 * 1024;  // 32MB L3
    socket->l3_cache.line_size = CACHE_LINE_SIZE;
    socket->l3_cache.associativity = 16;
    socket->l3_cache.latency = 40;  // ns
    socket->l3_cache.shared = true;

    // Initialize cores
    for (size_t i = 0; i < num_cores; i++) {
        if (!init_core(&socket->cores[i], i)) {
            return false;
        }
    }

    return true;
}

// Initialize Core
bool init_core(cpu_core_t *core, unsigned int id) {
    core->id = id;
    core->state = CPU_ONLINE;
    core->mips = 1000;  // Million instructions per second
    core->frequency = 3000;  // MHz
    memset(&core->stats, 0, sizeof(core->stats));

    // Initialize caches
    // L1 Cache
    core->caches[CACHE_L1].size = 32 * 1024;  // 32KB
    core->caches[CACHE_L1].line_size = CACHE_LINE_SIZE;
    core->caches[CACHE_L1].associativity = 8;
    core->caches[CACHE_L1].latency = 4;  // ns
    core->caches[CACHE_L1].shared = false;

    // L2 Cache
    core->caches[CACHE_L2].size = 256 * 1024;  // 256KB
    core->caches[CACHE_L2].line_size = CACHE_LINE_SIZE;
    core->caches[CACHE_L2].associativity = 8;
    core->caches[CACHE_L2].latency = 12;  // ns
    core->caches[CACHE_L2].shared = false;

    return true;
}

// Calculate Memory Latency
unsigned int calculate_memory_latency(
    topo_manager_t *manager,
    unsigned int source_node,
    unsigned int target_node
) {
    if (!manager || source_node >= manager->num_nodes || 
        target_node >= manager->num_nodes)
        return UINT_MAX;

    // Base latency of target node
    unsigned int latency = manager->nodes[target_node].memory_latency;

    // Add inter-node communication overhead
    if (source_node != target_node) {
        latency += 50 * abs((int)source_node - (int)target_node);
    }

    return latency;
}

// Access Memory
bool access_memory(
    topo_manager_t *manager,
    unsigned int node_id,
    size_t size,
    mem_access_t type
) {
    if (!manager || node_id >= manager->num_nodes) return false;

    pthread_mutex_lock(&manager->manager_lock);

    numa_node_t *node = &manager->nodes[node_id];
    bool success = true;

    // Update statistics
    switch (type) {
        case MEM_LOCAL:
            node->stats.local_accesses++;
            break;
        case MEM_REMOTE:
            node->stats.remote_accesses++;
            break;
        case MEM_FOREIGN:
            // Foreign accesses count as remote
            node->stats.remote_accesses++;
            break;
    }

    node->stats.memory_used += size;
    
    if (manager->config.track_stats) {
        manager->stats.memory_accesses++;
        unsigned int latency = calculate_memory_latency(manager, 0, node_id);
        manager->stats.avg_memory_latency = 
            (manager->stats.avg_memory_latency * 
                (manager->stats.memory_accesses - 1) + latency) /
            manager->stats.memory_accesses;
    }

    pthread_mutex_unlock(&manager->manager_lock);
    return success;
}

// Print Topology
void print_topology(topo_manager_t *manager) {
    if (!manager) return;

    printf("\nSystem Topology:\n");
    printf("--------------\n");

    for (size_t node = 0; node < manager->num_nodes; node++) {
        numa_node_t *numa = &manager->nodes[node];
        printf("\nNUMA Node %zu:\n", node);
        printf("  Memory: %zu GB\n", numa->memory_size / (1024*1024*1024));
        printf("  Base Latency: %u ns\n", numa->memory_latency);

        for (size_t sock = 0; sock < numa->num_sockets; sock++) {
            socket_t *socket = &numa->sockets[sock];
            printf("\n  Socket %zu:\n", sock);
            printf("    L3 Cache: %zu MB\n", 
                socket->l3_cache.size / (1024*1024));
            printf("    Memory Bandwidth: %lu MB/s\n", 
                socket->memory_bandwidth);

            for (size_t core = 0; core < socket->num_cores; core++) {
                cpu_core_t *cpu = &socket->cores[core];
                printf("\n    Core %zu:\n", core);
                printf("      State: %s\n", 
                    get_cpu_state_string(cpu->state));
                printf("      Frequency: %u MHz\n", cpu->frequency);
                printf("      MIPS: %lu\n", cpu->mips);
                
                for (int cache = 0; cache < CACHE_MAX_LEVEL-1; cache++) {
                    printf("      %s Cache: %zu KB\n",
                        get_cache_level_string(cache),
                        cpu->caches[cache].size / 1024);
                }
            }
        }
    }
}

// Print Topology Statistics
void print_topo_stats(topo_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    printf("\nTopology Statistics:\n");
    printf("------------------\n");
    printf("Cache Hits:        %lu\n", manager->stats.cache_hits);
    printf("Cache Misses:      %lu\n", manager->stats.cache_misses);
    printf("Memory Accesses:   %lu\n", manager->stats.memory_accesses);
    printf("CPU Migrations:    %lu\n", manager->stats.cpu_migrations);
    printf("Avg Memory Latency: %.2f ns\n", manager->stats.avg_memory_latency);

    // Print per-node statistics
    for (size_t i = 0; i < manager->num_nodes; i++) {
        numa_node_t *node = &manager->nodes[i];
        printf("\nNUMA Node %zu Statistics:\n", i);
        printf("  Local Accesses:  %lu\n", node->stats.local_accesses);
        printf("  Remote Accesses: %lu\n", node->stats.remote_accesses);
        printf("  Memory Used:     %lu MB\n", 
            node->stats.memory_used / (1024*1024));
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Destroy Topology Manager
void destroy_topo_manager(topo_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    // Free all nodes
    for (size_t i = 0; i < manager->num_nodes; i++) {
        numa_node_t *node = &manager->nodes[i];
        
        // Free all sockets
        for (size_t j = 0; j < node->num_sockets; j++) {
            socket_t *socket = &node->sockets[j];
            free(socket->cores);
        }
        free(node->sockets);
    }

    free(manager->nodes);

    pthread_mutex_unlock(&manager->manager_lock);
    pthread_mutex_destroy(&manager->manager_lock);

    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed topology manager");
}

// Demonstrate Topology
void demonstrate_topology(void) {
    // Create topology configuration
    topo_config_t config = {
        .num_nodes = 2,
        .num_sockets_per_node = 2,
        .num_cores_per_socket = 8,
        .track_stats = true
    };

    // Create topology manager
    topo_manager_t *manager = create_topo_manager(config);
    if (!manager) return;

    // Print initial topology
    print_topology(manager);

    // Simulate memory accesses
    for (unsigned int i = 0; i < manager->num_nodes; i++) {
        // Local access
        access_memory(manager, i, 1024*1024, MEM_LOCAL);
        
        // Remote access
        access_memory(manager, i, 1024*1024, MEM_REMOTE);
        
        // Foreign access
        access_memory(manager, i, 1024*1024, MEM_FOREIGN);
    }

    // Print statistics
    print_topo_stats(manager);

    // Cleanup
    destroy_topo_manager(manager);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_topology();

    return 0;
}
