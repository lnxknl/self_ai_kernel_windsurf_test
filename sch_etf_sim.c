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

// Packet Priority Levels
typedef enum {
    PRIORITY_LOWEST,
    PRIORITY_LOW,
    PRIORITY_NORMAL,
    PRIORITY_HIGH,
    PRIORITY_HIGHEST
} packet_priority_t;

// Transmission Modes
typedef enum {
    TRANS_MODE_STRICT,
    TRANS_MODE_WEIGHTED,
    TRANS_MODE_DEADLINE,
    TRANS_MODE_REALTIME,
    TRANS_MODE_ADAPTIVE
} transmission_mode_t;

// Packet State
typedef enum {
    PACKET_STATE_QUEUED,
    PACKET_STATE_READY,
    PACKET_STATE_TRANSMITTING,
    PACKET_STATE_COMPLETED,
    PACKET_STATE_DROPPED
} packet_state_t;

// Network Packet Structure
typedef struct network_packet {
    uint64_t packet_id;
    size_t packet_size;
    
    packet_priority_t priority;
    packet_state_t state;
    
    uint64_t arrival_time;
    uint64_t transmission_deadline;
    uint64_t completion_time;
    
    double transmission_time;
    
    struct network_packet *next;
} network_packet_t;

// ETF Scheduler Configuration
typedef struct {
    transmission_mode_t mode;
    uint64_t bandwidth_limit;
    uint64_t max_queue_depth;
    uint64_t cycle_time;
    
    bool strict_priority;
    bool deadline_enforcement;
} etf_config_t;

// ETF Scheduler Statistics
typedef struct {
    unsigned long total_packets_received;
    unsigned long total_packets_transmitted;
    unsigned long total_packets_dropped;
    unsigned long deadline_misses;
    unsigned long queue_overflows;
} etf_stats_t;

// ETF Scheduler System
typedef struct {
    network_packet_t *packet_queue;
    network_packet_t *transmission_queue;
    
    etf_config_t configuration;
    etf_stats_t stats;
    
    uint64_t current_time;
    size_t current_queue_depth;
    
    pthread_mutex_t scheduler_lock;
} etf_scheduler_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_priority_string(packet_priority_t priority);
const char* get_transmission_mode_string(transmission_mode_t mode);
const char* get_packet_state_string(packet_state_t state);

etf_scheduler_t* create_etf_scheduler(
    transmission_mode_t mode,
    uint64_t bandwidth_limit,
    uint64_t max_queue_depth
);

void destroy_etf_scheduler(etf_scheduler_t *scheduler);

network_packet_t* create_network_packet(
    uint64_t packet_id,
    size_t packet_size,
    packet_priority_t priority,
    uint64_t transmission_deadline
);

bool enqueue_packet(
    etf_scheduler_t *scheduler,
    network_packet_t *packet
);

network_packet_t* dequeue_packet(
    etf_scheduler_t *scheduler
);

bool transmit_packet(
    etf_scheduler_t *scheduler,
    network_packet_t *packet
);

void update_scheduler_time(
    etf_scheduler_t *scheduler,
    uint64_t current_time
);

void print_etf_stats(etf_scheduler_t *scheduler);
void demonstrate_etf_scheduler();

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

// Utility Function: Get Priority String
const char* get_priority_string(packet_priority_t priority) {
    switch(priority) {
        case PRIORITY_LOWEST:   return "LOWEST";
        case PRIORITY_LOW:      return "LOW";
        case PRIORITY_NORMAL:   return "NORMAL";
        case PRIORITY_HIGH:     return "HIGH";
        case PRIORITY_HIGHEST:  return "HIGHEST";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Transmission Mode String
const char* get_transmission_mode_string(transmission_mode_t mode) {
    switch(mode) {
        case TRANS_MODE_STRICT:     return "STRICT";
        case TRANS_MODE_WEIGHTED:   return "WEIGHTED";
        case TRANS_MODE_DEADLINE:   return "DEADLINE";
        case TRANS_MODE_REALTIME:   return "REALTIME";
        case TRANS_MODE_ADAPTIVE:   return "ADAPTIVE";
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

// Create ETF Scheduler
etf_scheduler_t* create_etf_scheduler(
    transmission_mode_t mode,
    uint64_t bandwidth_limit,
    uint64_t max_queue_depth
) {
    etf_scheduler_t *scheduler = malloc(sizeof(etf_scheduler_t));
    if (!scheduler) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate ETF scheduler");
        return NULL;
    }

    // Initialize configuration
    scheduler->configuration.mode = mode;
    scheduler->configuration.bandwidth_limit = bandwidth_limit;
    scheduler->configuration.max_queue_depth = max_queue_depth;
    scheduler->configuration.cycle_time = 100000;  // 100ms default
    scheduler->configuration.strict_priority = true;
    scheduler->configuration.deadline_enforcement = true;

    // Initialize queues
    scheduler->packet_queue = NULL;
    scheduler->transmission_queue = NULL;

    // Reset statistics
    memset(&scheduler->stats, 0, sizeof(etf_stats_t));

    // Initialize time and queue depth
    scheduler->current_time = 0;
    scheduler->current_queue_depth = 0;

    // Initialize scheduler lock
    pthread_mutex_init(&scheduler->scheduler_lock, NULL);

    LOG(LOG_LEVEL_DEBUG, "Created ETF Scheduler with mode %s", 
        get_transmission_mode_string(mode));

    return scheduler;
}

// Create Network Packet
network_packet_t* create_network_packet(
    uint64_t packet_id,
    size_t packet_size,
    packet_priority_t priority,
    uint64_t transmission_deadline
) {
    network_packet_t *packet = malloc(sizeof(network_packet_t));
    if (!packet) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate network packet");
        return NULL;
    }

    packet->packet_id = packet_id;
    packet->packet_size = packet_size;
    packet->priority = priority;
    packet->state = PACKET_STATE_QUEUED;
    
    packet->arrival_time = time(NULL);
    packet->transmission_deadline = transmission_deadline;
    packet->completion_time = 0;
    
    // Calculate transmission time based on packet size and bandwidth
    packet->transmission_time = (double)packet_size / (1024 * 1024);  // MB/s
    
    packet->next = NULL;

    LOG(LOG_LEVEL_DEBUG, "Created packet %lu, Size %zu, Priority %s", 
        packet_id, packet_size, get_priority_string(priority));

    return packet;
}

// Enqueue Packet
bool enqueue_packet(
    etf_scheduler_t *scheduler,
    network_packet_t *packet
) {
    if (!scheduler || !packet) return false;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    // Check queue depth
    if (scheduler->current_queue_depth >= scheduler->configuration.max_queue_depth) {
        scheduler->stats.queue_overflows++;
        packet->state = PACKET_STATE_DROPPED;
        
        pthread_mutex_unlock(&scheduler->scheduler_lock);
        
        LOG(LOG_LEVEL_WARN, "Packet %lu dropped: queue full", packet->packet_id);
        return false;
    }

    // Add to packet queue (sorted by deadline)
    network_packet_t *current = scheduler->packet_queue;
    network_packet_t *prev = NULL;

    while (current && current->transmission_deadline < packet->transmission_deadline) {
        prev = current;
        current = current->next;
    }

    if (prev) {
        prev->next = packet;
    } else {
        scheduler->packet_queue = packet;
    }
    packet->next = current;

    scheduler->current_queue_depth++;
    scheduler->stats.total_packets_received++;

    pthread_mutex_unlock(&scheduler->scheduler_lock);

    LOG(LOG_LEVEL_DEBUG, "Enqueued packet %lu", packet->packet_id);

    return true;
}

// Dequeue Packet
network_packet_t* dequeue_packet(
    etf_scheduler_t *scheduler
) {
    if (!scheduler) return NULL;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    if (!scheduler->packet_queue) {
        pthread_mutex_unlock(&scheduler->scheduler_lock);
        return NULL;
    }

    // Remove from front of queue
    network_packet_t *packet = scheduler->packet_queue;
    scheduler->packet_queue = packet->next;
    packet->next = NULL;

    scheduler->current_queue_depth--;

    pthread_mutex_unlock(&scheduler->scheduler_lock);

    LOG(LOG_LEVEL_DEBUG, "Dequeued packet %lu", packet->packet_id);

    return packet;
}

// Transmit Packet
bool transmit_packet(
    etf_scheduler_t *scheduler,
    network_packet_t *packet
) {
    if (!scheduler || !packet) return false;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    // Check transmission deadline
    if (scheduler->current_time > packet->transmission_deadline) {
        packet->state = PACKET_STATE_DROPPED;
        scheduler->stats.deadline_misses++;
        
        pthread_mutex_unlock(&scheduler->scheduler_lock);
        
        LOG(LOG_LEVEL_WARN, "Packet %lu missed deadline", packet->packet_id);
        return false;
    }

    // Simulate transmission
    packet->state = PACKET_STATE_TRANSMITTING;
    packet->completion_time = scheduler->current_time + 
        (uint64_t)(packet->transmission_time * 1000000);  // Convert to microseconds

    // Add to transmission queue
    packet->next = scheduler->transmission_queue;
    scheduler->transmission_queue = packet;

    scheduler->stats.total_packets_transmitted++;

    pthread_mutex_unlock(&scheduler->scheduler_lock);

    LOG(LOG_LEVEL_DEBUG, "Transmitting packet %lu", packet->packet_id);

    return true;
}

// Update Scheduler Time
void update_scheduler_time(
    etf_scheduler_t *scheduler,
    uint64_t current_time
) {
    if (!scheduler) return;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    scheduler->current_time = current_time;

    // Remove completed packets from transmission queue
    network_packet_t *current = scheduler->transmission_queue;
    network_packet_t *prev = NULL;
    network_packet_t *next = NULL;

    while (current) {
        next = current->next;

        if (current->completion_time <= current_time) {
            current->state = PACKET_STATE_COMPLETED;

            // Remove from transmission queue
            if (prev) {
                prev->next = next;
            } else {
                scheduler->transmission_queue = next;
            }
        } else {
            prev = current;
        }

        current = next;
    }

    pthread_mutex_unlock(&scheduler->scheduler_lock);
}

// Print ETF Scheduler Statistics
void print_etf_stats(etf_scheduler_t *scheduler) {
    if (!scheduler) return;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    printf("\nEarliest Transmission First (ETF) Scheduler Statistics:\n");
    printf("----------------------------------------------------\n");
    printf("Transmission Mode:      %s\n", 
        get_transmission_mode_string(scheduler->configuration.mode));
    printf("Bandwidth Limit:        %lu MB/s\n", 
        scheduler->configuration.bandwidth_limit);
    printf("Max Queue Depth:        %lu\n", 
        scheduler->configuration.max_queue_depth);
    printf("Total Packets Received: %lu\n", 
        scheduler->stats.total_packets_received);
    printf("Total Packets Sent:     %lu\n", 
        scheduler->stats.total_packets_transmitted);
    printf("Total Packets Dropped:  %lu\n", 
        scheduler->stats.total_packets_dropped);
    printf("Deadline Misses:        %lu\n", 
        scheduler->stats.deadline_misses);
    printf("Queue Overflows:        %lu\n", 
        scheduler->stats.queue_overflows);

    pthread_mutex_unlock(&scheduler->scheduler_lock);
}

// Destroy ETF Scheduler
void destroy_etf_scheduler(etf_scheduler_t *scheduler) {
    if (!scheduler) return;

    pthread_mutex_lock(&scheduler->scheduler_lock);

    // Free packet queue
    network_packet_t *current = scheduler->packet_queue;
    while (current) {
        network_packet_t *next = current->next;
        free(current);
        current = next;
    }

    // Free transmission queue
    current = scheduler->transmission_queue;
    while (current) {
        network_packet_t *next = current->next;
        free(current);
        current = next;
    }

    pthread_mutex_unlock(&scheduler->scheduler_lock);
    pthread_mutex_destroy(&scheduler->scheduler_lock);

    free(scheduler);
}

// Demonstrate ETF Scheduler
void demonstrate_etf_scheduler() {
    // Create ETF Scheduler
    etf_scheduler_t *etf_scheduler = create_etf_scheduler(
        TRANS_MODE_DEADLINE,
        100,  // 100 MB/s bandwidth
        100   // Max 100 packets in queue
    );

    // Create Sample Packets
    network_packet_t *packets[10];
    uint64_t base_time = time(NULL) * 1000000;  // Convert to microseconds

    for (int i = 0; i < 10; i++) {
        packets[i] = create_network_packet(
            i + 1000,  // Unique packet IDs
            1024 * (i + 1),  // Varying packet sizes
            (packet_priority_t)(i % 5),  // Cycle through priorities
            base_time + (i + 1) * 50000  // Staggered deadlines
        );

        // Enqueue packets
        if (packets[i]) {
            enqueue_packet(etf_scheduler, packets[i]);
        }
    }

    // Simulate Packet Transmission
    for (int i = 0; i < 5; i++) {
        // Update scheduler time
        update_scheduler_time(
            etf_scheduler, 
            base_time + (i + 1) * 100000
        );

        // Dequeue and transmit packets
        network_packet_t *packet = dequeue_packet(etf_scheduler);
        if (packet) {
            transmit_packet(etf_scheduler, packet);
        }
    }

    // Print Statistics
    print_etf_stats(etf_scheduler);

    // Cleanup
    destroy_etf_scheduler(etf_scheduler);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_etf_scheduler();

    return 0;
}
