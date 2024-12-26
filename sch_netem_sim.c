#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <math.h>

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

// Network Conditions
typedef enum {
    CONDITION_NORMAL,
    CONDITION_CONGESTED,
    CONDITION_LOSSY,
    CONDITION_HIGH_LATENCY,
    CONDITION_VARIABLE
} network_condition_t;

// Packet Types
typedef enum {
    PACKET_TYPE_TCP,
    PACKET_TYPE_UDP,
    PACKET_TYPE_ICMP,
    PACKET_TYPE_CUSTOM
} packet_type_t;

// Packet State
typedef enum {
    PACKET_STATE_QUEUED,
    PACKET_STATE_DELAYED,
    PACKET_STATE_CORRUPTED,
    PACKET_STATE_DROPPED,
    PACKET_STATE_DELIVERED
} packet_state_t;

// Network Packet Structure
typedef struct network_packet {
    uint64_t packet_id;
    packet_type_t type;
    size_t size;
    
    uint64_t arrival_time;
    uint64_t scheduled_time;
    uint64_t delivery_time;
    
    packet_state_t state;
    bool is_corrupted;
    bool is_reordered;
    
    struct network_packet *next;
} network_packet_t;

// Network Emulator Configuration
typedef struct {
    // Delay parameters
    uint64_t delay_mean;      // Mean delay in microseconds
    uint64_t delay_jitter;    // Delay variation in microseconds
    double delay_correlation; // Correlation with previous delay
    
    // Loss parameters
    double loss_rate;        // Packet loss probability
    double loss_correlation; // Correlation with previous loss
    
    // Corruption parameters
    double corruption_rate;  // Packet corruption probability
    
    // Reordering parameters
    double reorder_rate;     // Packet reordering probability
    uint64_t reorder_gap;    // Gap for reordered packets
    
    // Rate control
    uint64_t rate_limit;     // Bandwidth limit in bps
    size_t queue_limit;      // Maximum queue size
} netem_config_t;

// Network Emulator Statistics
typedef struct {
    unsigned long total_packets;
    unsigned long delayed_packets;
    unsigned long dropped_packets;
    unsigned long corrupted_packets;
    unsigned long reordered_packets;
    unsigned long delivered_packets;
    
    uint64_t total_delay;
    uint64_t min_delay;
    uint64_t max_delay;
    
    double avg_queue_size;
    size_t max_queue_size;
} netem_stats_t;

// Network Emulator System
typedef struct {
    network_packet_t *packet_queue;
    size_t queue_size;
    
    netem_config_t config;
    netem_stats_t stats;
    
    uint64_t last_delay;
    bool last_loss;
    
    pthread_mutex_t netem_lock;
} netem_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_packet_type_string(packet_type_t type);
const char* get_packet_state_string(packet_state_t state);

netem_system_t* create_netem_system(netem_config_t config);
void destroy_netem_system(netem_system_t *system);

network_packet_t* create_network_packet(
    uint64_t packet_id,
    packet_type_t type,
    size_t size
);

bool enqueue_packet(
    netem_system_t *system,
    network_packet_t *packet
);

network_packet_t* process_packet(
    netem_system_t *system,
    uint64_t current_time
);

uint64_t calculate_delay(netem_system_t *system);
bool should_drop_packet(netem_system_t *system);
bool should_corrupt_packet(netem_system_t *system);
bool should_reorder_packet(netem_system_t *system);

void print_netem_stats(netem_system_t *system);
void demonstrate_netem_system();

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

// Utility Function: Get Packet Type String
const char* get_packet_type_string(packet_type_t type) {
    switch(type) {
        case PACKET_TYPE_TCP:    return "TCP";
        case PACKET_TYPE_UDP:    return "UDP";
        case PACKET_TYPE_ICMP:   return "ICMP";
        case PACKET_TYPE_CUSTOM: return "CUSTOM";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Packet State String
const char* get_packet_state_string(packet_state_t state) {
    switch(state) {
        case PACKET_STATE_QUEUED:     return "QUEUED";
        case PACKET_STATE_DELAYED:    return "DELAYED";
        case PACKET_STATE_CORRUPTED:  return "CORRUPTED";
        case PACKET_STATE_DROPPED:    return "DROPPED";
        case PACKET_STATE_DELIVERED:  return "DELIVERED";
        default: return "UNKNOWN";
    }
}

// Create Network Emulator System
netem_system_t* create_netem_system(netem_config_t config) {
    netem_system_t *system = malloc(sizeof(netem_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate network emulator system");
        return NULL;
    }

    // Initialize configuration
    system->config = config;
    
    // Initialize queue
    system->packet_queue = NULL;
    system->queue_size = 0;
    
    // Reset statistics
    memset(&system->stats, 0, sizeof(netem_stats_t));
    system->stats.min_delay = UINT64_MAX;
    
    // Initialize correlation tracking
    system->last_delay = 0;
    system->last_loss = false;
    
    // Initialize system lock
    pthread_mutex_init(&system->netem_lock, NULL);

    LOG(LOG_LEVEL_DEBUG, "Created Network Emulator System");
    return system;
}

// Create Network Packet
network_packet_t* create_network_packet(
    uint64_t packet_id,
    packet_type_t type,
    size_t size
) {
    network_packet_t *packet = malloc(sizeof(network_packet_t));
    if (!packet) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate network packet");
        return NULL;
    }

    packet->packet_id = packet_id;
    packet->type = type;
    packet->size = size;
    
    packet->arrival_time = 0;
    packet->scheduled_time = 0;
    packet->delivery_time = 0;
    
    packet->state = PACKET_STATE_QUEUED;
    packet->is_corrupted = false;
    packet->is_reordered = false;
    
    packet->next = NULL;

    return packet;
}

// Calculate Network Delay
uint64_t calculate_delay(netem_system_t *system) {
    double random = (double)rand() / RAND_MAX;
    double correlation = system->config.delay_correlation;
    
    // Calculate correlated random delay
    uint64_t new_delay = system->config.delay_mean + 
        (uint64_t)(system->config.delay_jitter * 
            (correlation * (system->last_delay - system->config.delay_mean) / 
                system->config.delay_jitter + 
            sqrt(1 - correlation * correlation) * (2 * random - 1)));
    
    system->last_delay = new_delay;
    return new_delay;
}

// Check if Packet Should be Dropped
bool should_drop_packet(netem_system_t *system) {
    double random = (double)rand() / RAND_MAX;
    double correlation = system->config.loss_correlation;
    
    // Calculate correlated loss probability
    double loss_prob = system->config.loss_rate * 
        (correlation * (system->last_loss ? 1 : 0) + (1 - correlation) * random);
    
    system->last_loss = (random < loss_prob);
    return system->last_loss;
}

// Check if Packet Should be Corrupted
bool should_corrupt_packet(netem_system_t *system) {
    double random = (double)rand() / RAND_MAX;
    return random < system->config.corruption_rate;
}

// Check if Packet Should be Reordered
bool should_reorder_packet(netem_system_t *system) {
    double random = (double)rand() / RAND_MAX;
    return random < system->config.reorder_rate;
}

// Enqueue Packet
bool enqueue_packet(
    netem_system_t *system,
    network_packet_t *packet
) {
    if (!system || !packet) return false;

    pthread_mutex_lock(&system->netem_lock);

    // Check queue limit
    if (system->queue_size >= system->config.queue_limit) {
        packet->state = PACKET_STATE_DROPPED;
        system->stats.dropped_packets++;
        
        pthread_mutex_unlock(&system->netem_lock);
        return false;
    }

    // Record arrival time
    packet->arrival_time = time(NULL);

    // Add to queue
    packet->next = system->packet_queue;
    system->packet_queue = packet;
    system->queue_size++;

    // Update statistics
    system->stats.total_packets++;
    if (system->queue_size > system->stats.max_queue_size) {
        system->stats.max_queue_size = system->queue_size;
    }

    pthread_mutex_unlock(&system->netem_lock);
    return true;
}

// Process Packet
network_packet_t* process_packet(
    netem_system_t *system,
    uint64_t current_time
) {
    if (!system || !system->packet_queue) return NULL;

    pthread_mutex_lock(&system->netem_lock);

    network_packet_t *packet = system->packet_queue;
    system->packet_queue = packet->next;
    system->queue_size--;

    // Apply network conditions
    if (should_drop_packet(system)) {
        packet->state = PACKET_STATE_DROPPED;
        system->stats.dropped_packets++;
    } else {
        uint64_t delay = calculate_delay(system);
        packet->scheduled_time = current_time + delay;
        
        if (should_corrupt_packet(system)) {
            packet->is_corrupted = true;
            packet->state = PACKET_STATE_CORRUPTED;
            system->stats.corrupted_packets++;
        }
        
        if (should_reorder_packet(system)) {
            packet->is_reordered = true;
            packet->scheduled_time += system->config.reorder_gap;
            system->stats.reordered_packets++;
        }
        
        if (packet->state == PACKET_STATE_QUEUED) {
            packet->state = PACKET_STATE_DELAYED;
            system->stats.delayed_packets++;
        }
        
        // Update delay statistics
        system->stats.total_delay += delay;
        if (delay < system->stats.min_delay) system->stats.min_delay = delay;
        if (delay > system->stats.max_delay) system->stats.max_delay = delay;
    }

    pthread_mutex_unlock(&system->netem_lock);
    return packet;
}

// Print Network Emulator Statistics
void print_netem_stats(netem_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->netem_lock);

    printf("\nNetwork Emulator Statistics:\n");
    printf("----------------------------\n");
    printf("Total Packets:      %lu\n", system->stats.total_packets);
    printf("Delayed Packets:    %lu\n", system->stats.delayed_packets);
    printf("Dropped Packets:    %lu\n", system->stats.dropped_packets);
    printf("Corrupted Packets:  %lu\n", system->stats.corrupted_packets);
    printf("Reordered Packets:  %lu\n", system->stats.reordered_packets);
    printf("Delivered Packets:  %lu\n", system->stats.delivered_packets);
    
    if (system->stats.total_packets > 0) {
        printf("\nDelay Statistics:\n");
        printf("Min Delay: %lu us\n", system->stats.min_delay);
        printf("Max Delay: %lu us\n", system->stats.max_delay);
        printf("Avg Delay: %lu us\n", 
            system->stats.total_delay / system->stats.total_packets);
    }
    
    printf("\nQueue Statistics:\n");
    printf("Current Queue Size: %zu\n", system->queue_size);
    printf("Max Queue Size:     %zu\n", system->stats.max_queue_size);

    pthread_mutex_unlock(&system->netem_lock);
}

// Destroy Network Emulator System
void destroy_netem_system(netem_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->netem_lock);

    // Free all packets in queue
    network_packet_t *current = system->packet_queue;
    while (current) {
        network_packet_t *next = current->next;
        free(current);
        current = next;
    }

    pthread_mutex_unlock(&system->netem_lock);
    pthread_mutex_destroy(&system->netem_lock);

    free(system);
}

// Demonstrate Network Emulator System
void demonstrate_netem_system() {
    // Create configuration
    netem_config_t config = {
        .delay_mean = 100000,        // 100ms mean delay
        .delay_jitter = 50000,       // 50ms jitter
        .delay_correlation = 0.5,     // 50% delay correlation
        
        .loss_rate = 0.01,           // 1% packet loss
        .loss_correlation = 0.2,      // 20% loss correlation
        
        .corruption_rate = 0.001,     // 0.1% corruption rate
        
        .reorder_rate = 0.05,        // 5% reordering rate
        .reorder_gap = 200000,       // 200ms reordering gap
        
        .rate_limit = 1000000,       // 1 Mbps bandwidth limit
        .queue_limit = 1000          // Max 1000 packets in queue
    };

    // Create Network Emulator System
    netem_system_t *netem = create_netem_system(config);

    // Simulate network traffic
    for (int i = 0; i < 1000; i++) {
        // Create packet with random type and size
        network_packet_t *packet = create_network_packet(
            i + 1,
            (packet_type_t)(rand() % 4),
            1000 + (rand() % 1000)
        );

        if (packet) {
            // Enqueue packet
            if (enqueue_packet(netem, packet)) {
                // Process packet
                network_packet_t *processed = process_packet(
                    netem,
                    time(NULL) * 1000000
                );

                if (processed && processed->state != PACKET_STATE_DROPPED) {
                    processed->state = PACKET_STATE_DELIVERED;
                    netem->stats.delivered_packets++;
                }

                if (processed != packet) {
                    free(processed);
                }
            }
            free(packet);
        }
    }

    // Print Statistics
    print_netem_stats(netem);

    // Cleanup
    destroy_netem_system(netem);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_netem_system();

    return 0;
}
