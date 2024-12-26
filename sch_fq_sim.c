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

// Flow Classification Types
typedef enum {
    FLOW_TYPE_TCP,
    FLOW_TYPE_UDP,
    FLOW_TYPE_ICMP,
    FLOW_TYPE_CUSTOM,
    FLOW_TYPE_SYSTEM
} flow_type_t;

// Packet Scheduling Modes
typedef enum {
    SCHED_MODE_STRICT,
    SCHED_MODE_WEIGHTED,
    SCHED_MODE_DRR,      // Deficit Round Robin
    SCHED_MODE_WFQ,      // Weighted Fair Queuing
    SCHED_MODE_ADAPTIVE
} scheduling_mode_t;

// Packet State
typedef enum {
    PACKET_STATE_QUEUED,
    PACKET_STATE_READY,
    PACKET_STATE_TRANSMITTING,
    PACKET_STATE_COMPLETED,
    PACKET_STATE_DROPPED
} packet_state_t;

// Network Flow Structure
typedef struct network_flow {
    uint64_t flow_id;
    flow_type_t type;
    
    uint64_t total_bytes_sent;
    uint64_t total_packets_sent;
    
    double weight;
    uint64_t quantum;
    
    struct network_packet *packet_queue;
    struct network_flow *next;
} network_flow_t;

// Network Packet Structure
typedef struct network_packet {
    uint64_t packet_id;
    size_t packet_size;
    
    network_flow_t *flow;
    packet_state_t state;
    
    uint64_t arrival_time;
    uint64_t transmission_time;
    
    struct network_packet *next;
} network_packet_t;

// Fair Queuing Scheduler Configuration
typedef struct {
    scheduling_mode_t mode;
    uint64_t total_bandwidth;
    size_t max_flows;
    size_t max_queue_depth;
    
    bool strict_priority;
} fq_config_t;

// Fair Queuing Scheduler Statistics
typedef struct {
    unsigned long total_packets_received;
    unsigned long total_packets_transmitted;
    unsigned long total_packets_dropped;
    unsigned long flow_starvation_events;
    unsigned long queue_overflows;
} fq_stats_t;

// Fair Queuing Scheduler System
typedef struct {
    network_flow_t *flow_list;
    
    fq_config_t configuration;
    fq_stats_t stats;
    
    uint64_t current_time;
    size_t current_flow_count;
    
    pthread_mutex_t scheduler_lock;
} fq_scheduler_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_flow_type_string(flow_type_t type);
const char* get_scheduling_mode_string(scheduling_mode_t mode);
const char* get_packet_state_string(packet_state_t state);

fq_scheduler_t* create_fq_scheduler(
    scheduling_mode_t mode,
    uint64_t total_bandwidth,
    size_t max_flows,
    size_t max_queue_depth
);

void destroy_fq_scheduler(fq_scheduler_t *scheduler);

network_flow_t* create_network_flow(
    fq_scheduler_t *scheduler,
    flow_type_t type,
    double weight
);

network_packet_t* create_network_packet(
    network_flow_t *flow,
    size_t packet_size
);

bool enqueue_packet(
    fq_scheduler_t *scheduler,
    network_packet_t *packet
);

network_packet_t* dequeue_packet(
    fq_scheduler_t *scheduler
);

bool transmit_packet(
    fq_scheduler_t *scheduler,
    network_packet_t *packet
);

void update_scheduler_time(
    fq_scheduler_t *scheduler,
    uint64_t current_time
);

void print_fq_stats(fq_scheduler_t *scheduler);
void demonstrate_fq_scheduler();

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

// Utility Function: Get Flow Type String
const char* get_flow_type_string(flow_type_t type) {
    switch(type) {
        case FLOW_TYPE_TCP:     return "TCP";
        case FLOW_TYPE_UDP:     return "UDP";
        case FLOW_TYPE_ICMP:    return "ICMP";
        case FLOW_TYPE_CUSTOM:  return "CUSTOM";
        case FLOW_TYPE_SYSTEM:  return "SYSTEM";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Scheduling Mode String
const char* get_scheduling_mode_string(scheduling_mode_t mode) {
    switch(mode) {
        case SCHED_MODE_STRICT:     return "STRICT";
        case SCHED_MODE_WEIGHTED:   return "WEIGHTED";
        case SCHED_MODE_DRR:        return "DRR";
        case SCHED_MODE_WFQ:        return "WFQ";
        case SCHED_MODE_ADAPTIVE:   return "ADAPTIVE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Packet State String
const char* get_packet_state_string(packet_state_t state) {
    switch(state) {
        case PACKET_STATE_QUEUED:        return "QUEUED";
        case PACKET_STATE_READY:         return "READY";
        case PACKET_STATE_TRANSMITTING:  return "TRANSMITTING";
        case PACKET_STATE_COMPLETED:     return "COMPLETED";
        case PACKET_STATE_DROPPED:       return "DROPPED";
        default: return "UNKNOWN";
    }
}

// Create Fair Queuing Scheduler
fq_scheduler_t* create_fq_scheduler(
    scheduling_mode_t mode,
    uint64_t total_bandwidth,
    size_t max_flows,
    size_t max_queue_depth
) {
    fq_scheduler_t *scheduler = malloc(sizeof(fq_scheduler_t));
    if (!scheduler) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate Fair Queuing scheduler");
        return NULL;
    }

    // Initialize configuration
    scheduler->configuration.mode = mode;
    scheduler->configuration.total_bandwidth = total_bandwidth;
    scheduler->configuration.max_flows = max_flows;
    scheduler->configuration.max_queue_depth = max_queue_depth;
    scheduler->configuration.strict_priority = false;

    // Initialize flow list
    scheduler->flow_list = NULL;
    scheduler->current_flow_count = 0;

    // Reset statistics
    memset(&scheduler->stats, 0, sizeof(fq_stats_t));

    // Initialize current time
    scheduler->current_time = 0;

    // Initialize scheduler lock
    pthread_mutex_init(&scheduler->scheduler_lock, NULL);

    LOG(LOG_LEVEL_DEBUG, "Created Fair Queuing Scheduler with mode %s", 
        get_scheduling_mode_string(mode));

    return scheduler;
}

// Create Network Flow
network_flow_t* create_network_flow(
    fq_scheduler_t *scheduler,
    flow_type_t type,
    double weight
) {
    if (!scheduler) return NULL;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    // Check flow limit
    if (scheduler->current_flow_count >= scheduler->configuration.max_flows) {
        pthread_mutex_unlock(&scheduler->scheduler_lock);
        LOG(LOG_LEVEL_WARN, "Maximum flow count reached");
        return NULL;
    }

    network_flow_t *flow = malloc(sizeof(network_flow_t));
    if (!flow) {
        pthread_mutex_unlock(&scheduler->scheduler_lock);
        return NULL;
    }

    // Initialize flow properties
    flow->flow_id = rand();
    flow->type = type;
    flow->total_bytes_sent = 0;
    flow->total_packets_sent = 0;
    flow->weight = weight > 0 ? weight : 1.0;
    flow->quantum = (uint64_t)(scheduler->configuration.total_bandwidth * flow->weight);
    flow->packet_queue = NULL;

    // Link to flow list
    flow->next = scheduler->flow_list;
    scheduler->flow_list = flow;
    scheduler->current_flow_count++;

    pthread_mutex_unlock(&scheduler->scheduler_lock);

    LOG(LOG_LEVEL_DEBUG, "Created %s flow, ID %lu, Weight %.2f", 
        get_flow_type_string(type), flow->flow_id, flow->weight);

    return flow;
}

// Create Network Packet
network_packet_t* create_network_packet(
    network_flow_t *flow,
    size_t packet_size
) {
    if (!flow) return NULL;

    network_packet_t *packet = malloc(sizeof(network_packet_t));
    if (!packet) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate network packet");
        return NULL;
    }

    packet->packet_id = rand();
    packet->packet_size = packet_size;
    packet->flow = flow;
    packet->state = PACKET_STATE_QUEUED;
    packet->arrival_time = time(NULL);
    packet->transmission_time = 0;
    packet->next = NULL;

    LOG(LOG_LEVEL_DEBUG, "Created packet %lu for flow %lu, Size %zu", 
        packet->packet_id, flow->flow_id, packet_size);

    return packet;
}

// Enqueue Packet
bool enqueue_packet(
    fq_scheduler_t *scheduler,
    network_packet_t *packet
) {
    if (!scheduler || !packet) return false;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    // Find the flow for this packet
    network_flow_t *flow = packet->flow;
    if (!flow) {
        pthread_mutex_unlock(&scheduler->scheduler_lock);
        return false;
    }

    // Check queue depth for this flow
    size_t queue_depth = 0;
    network_packet_t *current = flow->packet_queue;
    while (current) {
        queue_depth++;
        current = current->next;
    }

    if (queue_depth >= scheduler->configuration.max_queue_depth) {
        scheduler->stats.queue_overflows++;
        packet->state = PACKET_STATE_DROPPED;
        
        pthread_mutex_unlock(&scheduler->scheduler_lock);
        
        LOG(LOG_LEVEL_WARN, "Packet %lu dropped: flow queue full", packet->packet_id);
        return false;
    }

    // Enqueue packet to flow's queue
    packet->next = flow->packet_queue;
    flow->packet_queue = packet;

    scheduler->stats.total_packets_received++;

    pthread_mutex_unlock(&scheduler->scheduler_lock);

    LOG(LOG_LEVEL_DEBUG, "Enqueued packet %lu to flow %lu", 
        packet->packet_id, flow->flow_id);

    return true;
}

// Dequeue Packet
network_packet_t* dequeue_packet(
    fq_scheduler_t *scheduler
) {
    if (!scheduler) return NULL;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    network_flow_t *current_flow = scheduler->flow_list;
    network_packet_t *selected_packet = NULL;
    network_flow_t *selected_flow = NULL;

    // Implement Deficit Round Robin (DRR) scheduling
    while (current_flow) {
        if (current_flow->packet_queue) {
            network_packet_t *packet = current_flow->packet_queue;
            
            // Check if packet can be transmitted within flow's quantum
            if (packet->packet_size <= current_flow->quantum) {
                // Remove packet from flow's queue
                current_flow->packet_queue = packet->next;
                packet->next = NULL;

                // Update flow's quantum and statistics
                current_flow->quantum -= packet->packet_size;
                current_flow->total_bytes_sent += packet->packet_size;
                current_flow->total_packets_sent++;

                selected_packet = packet;
                selected_flow = current_flow;
                break;
            }
        }

        current_flow = current_flow->next;
    }

    // Handle flow starvation
    if (!selected_packet) {
        scheduler->stats.flow_starvation_events++;
    }

    pthread_mutex_unlock(&scheduler->scheduler_lock);

    return selected_packet;
}

// Transmit Packet
bool transmit_packet(
    fq_scheduler_t *scheduler,
    network_packet_t *packet
) {
    if (!scheduler || !packet) return false;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    // Simulate transmission
    packet->state = PACKET_STATE_TRANSMITTING;
    packet->transmission_time = scheduler->current_time + 
        (packet->packet_size / scheduler->configuration.total_bandwidth);

    scheduler->stats.total_packets_transmitted++;

    pthread_mutex_unlock(&scheduler->scheduler_lock);

    LOG(LOG_LEVEL_DEBUG, "Transmitting packet %lu from flow %lu", 
        packet->packet_id, packet->flow->flow_id);

    return true;
}

// Update Scheduler Time
void update_scheduler_time(
    fq_scheduler_t *scheduler,
    uint64_t current_time
) {
    if (!scheduler) return;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    scheduler->current_time = current_time;

    // Replenish flow quantums periodically
    network_flow_t *current_flow = scheduler->flow_list;
    while (current_flow) {
        current_flow->quantum = (uint64_t)(
            scheduler->configuration.total_bandwidth * current_flow->weight
        );
        current_flow = current_flow->next;
    }

    pthread_mutex_unlock(&scheduler->scheduler_lock);
}

// Print Fair Queuing Scheduler Statistics
void print_fq_stats(fq_scheduler_t *scheduler) {
    if (!scheduler) return;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    printf("\nFair Queuing Scheduler Statistics:\n");
    printf("----------------------------------\n");
    printf("Scheduling Mode:        %s\n", 
        get_scheduling_mode_string(scheduler->configuration.mode));
    printf("Total Bandwidth:        %lu\n", 
        scheduler->configuration.total_bandwidth);
    printf("Max Flows:              %zu\n", 
        scheduler->configuration.max_flows);
    printf("Total Packets Received: %lu\n", 
        scheduler->stats.total_packets_received);
    printf("Total Packets Sent:     %lu\n", 
        scheduler->stats.total_packets_transmitted);
    printf("Total Packets Dropped:  %lu\n", 
        scheduler->stats.total_packets_dropped);
    printf("Flow Starvation Events: %lu\n", 
        scheduler->stats.flow_starvation_events);
    printf("Queue Overflows:        %lu\n", 
        scheduler->stats.queue_overflows);

    pthread_mutex_unlock(&scheduler->scheduler_lock);
}

// Destroy Fair Queuing Scheduler
void destroy_fq_scheduler(fq_scheduler_t *scheduler) {
    if (!scheduler) return;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    // Free all flows and their packets
    network_flow_t *current_flow = scheduler->flow_list;
    while (current_flow) {
        network_flow_t *next_flow = current_flow->next;

        // Free packets in flow's queue
        network_packet_t *current_packet = current_flow->packet_queue;
        while (current_packet) {
            network_packet_t *next_packet = current_packet->next;
            free(current_packet);
            current_packet = next_packet;
        }

        free(current_flow);
        current_flow = next_flow;
    }

    pthread_mutex_unlock(&scheduler->scheduler_lock);
    pthread_mutex_destroy(&scheduler->scheduler_lock);

    free(scheduler);
}

// Demonstrate Fair Queuing Scheduler
void demonstrate_fq_scheduler() {
    // Create Fair Queuing Scheduler
    fq_scheduler_t *fq_scheduler = create_fq_scheduler(
        SCHED_MODE_DRR,
        1000,  // 1000 Mbps bandwidth
        10,    // Max 10 flows
        100    // Max 100 packets per flow
    );

    // Create Sample Flows
    network_flow_t *flows[5];
    for (int i = 0; i < 5; i++) {
        flows[i] = create_network_flow(
            fq_scheduler, 
            (flow_type_t)(i % 5),  // Cycle through flow types
            1.0 + (i * 0.5)  // Varying weights
        );
    }

    // Create and Enqueue Packets
    for (int i = 0; i < 5; i++) {
        if (flows[i]) {
            for (int j = 0; j < 20; j++) {
                network_packet_t *packet = create_network_packet(
                    flows[i], 
                    1024 * (j + 1)  // Varying packet sizes
                );

                if (packet) {
                    enqueue_packet(fq_scheduler, packet);
                }
            }
        }
    }

    // Simulate Packet Transmission
    for (int i = 0; i < 50; i++) {
        // Update scheduler time
        update_scheduler_time(
            fq_scheduler, 
            i * 1000  // Increment time
        );

        // Dequeue and transmit packets
        network_packet_t *packet = dequeue_packet(fq_scheduler);
        if (packet) {
            transmit_packet(fq_scheduler, packet);
        }
    }

    // Print Statistics
    print_fq_stats(fq_scheduler);

    // Cleanup
    destroy_fq_scheduler(fq_scheduler);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_fq_scheduler();

    return 0;
}
