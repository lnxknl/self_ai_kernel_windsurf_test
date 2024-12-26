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

// HTB Class States
typedef enum {
    HTB_CAN_SEND,      // Class can send data
    HTB_CANT_SEND,     // Class cannot send data
    HTB_MAY_BORROW     // Class may borrow tokens from parent
} htb_state_t;

// Traffic Types
typedef enum {
    TRAFFIC_BEST_EFFORT,
    TRAFFIC_INTERACTIVE,
    TRAFFIC_BULK,
    TRAFFIC_REALTIME,
    TRAFFIC_SYSTEM
} traffic_type_t;

// HTB Class Structure
typedef struct htb_class {
    uint32_t class_id;
    struct htb_class *parent;
    struct htb_class **children;
    size_t child_count;
    size_t max_children;

    // Rate parameters
    uint64_t rate;           // Guaranteed rate
    uint64_t ceil;           // Maximum rate
    uint64_t burst;          // Burst size
    uint64_t cburst;         // Ceiling burst size

    // Token bucket state
    uint64_t tokens;         // Current tokens
    uint64_t ctokens;        // Current ceiling tokens
    uint64_t last_update;    // Last update time

    // Class statistics
    uint64_t bytes_sent;
    uint64_t packets_sent;
    uint64_t drops;
    uint64_t overlimits;

    // Class state
    htb_state_t state;
    traffic_type_t traffic_type;
    double priority;
} htb_class_t;

// HTB Scheduler Configuration
typedef struct {
    uint64_t rate;           // Root rate
    uint64_t ceil;           // Root ceiling
    size_t max_classes;      // Maximum number of classes
    uint64_t quantum;        // Base quantum
    bool rate_estimator;     // Enable rate estimation
} htb_config_t;

// HTB Scheduler Statistics
typedef struct {
    unsigned long total_packets;
    unsigned long total_bytes;
    unsigned long dropped_packets;
    unsigned long overlimit_events;
    double avg_latency;
} htb_stats_t;

// HTB Scheduler System
typedef struct {
    htb_class_t *root_class;
    size_t class_count;
    
    htb_config_t config;
    htb_stats_t stats;
    
    uint64_t current_time;
    pthread_mutex_t htb_lock;
} htb_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_traffic_type_string(traffic_type_t type);
const char* get_htb_state_string(htb_state_t state);

htb_system_t* create_htb_system(htb_config_t config);
void destroy_htb_system(htb_system_t *system);

htb_class_t* create_htb_class(
    htb_system_t *system,
    htb_class_t *parent,
    uint32_t class_id,
    uint64_t rate,
    uint64_t ceil,
    traffic_type_t type
);

bool update_tokens(htb_class_t *class, uint64_t current_time);
bool can_send(htb_class_t *class, size_t size);
bool charge_class(htb_class_t *class, size_t size);
void update_class_state(htb_class_t *class);

void print_htb_stats(htb_system_t *system);
void demonstrate_htb_system();

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

// Utility Function: Get Traffic Type String
const char* get_traffic_type_string(traffic_type_t type) {
    switch(type) {
        case TRAFFIC_BEST_EFFORT:  return "BEST_EFFORT";
        case TRAFFIC_INTERACTIVE:  return "INTERACTIVE";
        case TRAFFIC_BULK:        return "BULK";
        case TRAFFIC_REALTIME:    return "REALTIME";
        case TRAFFIC_SYSTEM:      return "SYSTEM";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get HTB State String
const char* get_htb_state_string(htb_state_t state) {
    switch(state) {
        case HTB_CAN_SEND:    return "CAN_SEND";
        case HTB_CANT_SEND:   return "CANT_SEND";
        case HTB_MAY_BORROW:  return "MAY_BORROW";
        default: return "UNKNOWN";
    }
}

// Create HTB System
htb_system_t* create_htb_system(htb_config_t config) {
    htb_system_t *system = malloc(sizeof(htb_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate HTB system");
        return NULL;
    }

    // Initialize configuration
    system->config = config;
    
    // Reset statistics
    memset(&system->stats, 0, sizeof(htb_stats_t));
    
    // Initialize time
    system->current_time = time(NULL);
    
    // Create root class
    system->root_class = create_htb_class(
        system,
        NULL,
        1,
        config.rate,
        config.ceil,
        TRAFFIC_SYSTEM
    );

    if (!system->root_class) {
        free(system);
        return NULL;
    }

    system->class_count = 1;

    // Initialize system lock
    pthread_mutex_init(&system->htb_lock, NULL);

    LOG(LOG_LEVEL_DEBUG, "Created HTB System with rate %lu bps", config.rate);
    return system;
}

// Create HTB Class
htb_class_t* create_htb_class(
    htb_system_t *system,
    htb_class_t *parent,
    uint32_t class_id,
    uint64_t rate,
    uint64_t ceil,
    traffic_type_t type
) {
    if (!system || system->class_count >= system->config.max_classes) {
        return NULL;
    }

    htb_class_t *class = malloc(sizeof(htb_class_t));
    if (!class) {
        return NULL;
    }

    // Initialize class properties
    class->class_id = class_id;
    class->parent = parent;
    class->children = NULL;
    class->child_count = 0;
    class->max_children = 16;  // Default max children

    // Set rate parameters
    class->rate = rate;
    class->ceil = ceil;
    class->burst = rate / 8;    // 125ms worth of data
    class->cburst = ceil / 8;   // 125ms worth of data

    // Initialize token buckets
    class->tokens = class->burst;
    class->ctokens = class->cburst;
    class->last_update = system->current_time;

    // Reset statistics
    class->bytes_sent = 0;
    class->packets_sent = 0;
    class->drops = 0;
    class->overlimits = 0;

    // Set class attributes
    class->state = HTB_CAN_SEND;
    class->traffic_type = type;
    class->priority = 0;

    // Allocate children array
    class->children = malloc(class->max_children * sizeof(htb_class_t*));
    if (!class->children) {
        free(class);
        return NULL;
    }

    LOG(LOG_LEVEL_DEBUG, "Created HTB class %u, Rate %lu bps, Ceil %lu bps", 
        class_id, rate, ceil);

    return class;
}

// Update Tokens
bool update_tokens(htb_class_t *class, uint64_t current_time) {
    if (!class) return false;

    uint64_t delta = current_time - class->last_update;
    
    // Update regular tokens
    uint64_t tokens = class->tokens + delta * class->rate;
    if (tokens > class->burst) {
        tokens = class->burst;
    }
    class->tokens = tokens;

    // Update ceiling tokens
    uint64_t ctokens = class->ctokens + delta * class->ceil;
    if (ctokens > class->cburst) {
        ctokens = class->cburst;
    }
    class->ctokens = ctokens;

    class->last_update = current_time;
    return true;
}

// Check if Class Can Send
bool can_send(htb_class_t *class, size_t size) {
    if (!class) return false;

    // Check regular tokens
    if (class->tokens >= size) {
        return true;
    }

    // Check ceiling tokens if allowed to borrow
    if (class->state == HTB_MAY_BORROW && class->ctokens >= size) {
        return true;
    }

    return false;
}

// Charge Class for Transmission
bool charge_class(htb_class_t *class, size_t size) {
    if (!class) return false;

    // Try to use regular tokens first
    if (class->tokens >= size) {
        class->tokens -= size;
        class->bytes_sent += size;
        class->packets_sent++;
        return true;
    }

    // Try to use ceiling tokens if allowed
    if (class->state == HTB_MAY_BORROW && class->ctokens >= size) {
        class->ctokens -= size;
        class->bytes_sent += size;
        class->packets_sent++;
        class->overlimits++;
        return true;
    }

    class->drops++;
    return false;
}

// Update Class State
void update_class_state(htb_class_t *class) {
    if (!class) return;

    // Determine new state based on token availability
    if (class->tokens > 0) {
        class->state = HTB_CAN_SEND;
    } else if (class->ctokens > 0) {
        class->state = HTB_MAY_BORROW;
    } else {
        class->state = HTB_CANT_SEND;
    }
}

// Print HTB Statistics
void print_htb_stats(htb_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->htb_lock);

    printf("\nHierarchical Token Bucket (HTB) Statistics:\n");
    printf("------------------------------------------\n");
    printf("Total Packets:       %lu\n", system->stats.total_packets);
    printf("Total Bytes:         %lu\n", system->stats.total_bytes);
    printf("Dropped Packets:     %lu\n", system->stats.dropped_packets);
    printf("Overlimit Events:    %lu\n", system->stats.overlimit_events);
    printf("Average Latency:     %.2f ms\n", system->stats.avg_latency);

    // Print root class statistics
    htb_class_t *root = system->root_class;
    printf("\nRoot Class Statistics:\n");
    printf("Rate:               %lu bps\n", root->rate);
    printf("Ceiling:            %lu bps\n", root->ceil);
    printf("Bytes Sent:         %lu\n", root->bytes_sent);
    printf("Packets Sent:       %lu\n", root->packets_sent);
    printf("Drops:              %lu\n", root->drops);
    printf("Overlimits:         %lu\n", root->overlimits);
    printf("Current State:      %s\n", get_htb_state_string(root->state));

    pthread_mutex_unlock(&system->htb_lock);
}

// Destroy HTB System
void destroy_htb_system(htb_system_t *system) {
    if (!system) return;

    pthread_mutex_lock(&system->htb_lock);

    // Recursive function to free class hierarchy
    void free_class(htb_class_t *class) {
        if (!class) return;
        
        // Free children first
        for (size_t i = 0; i < class->child_count; i++) {
            free_class(class->children[i]);
        }
        
        free(class->children);
        free(class);
    }

    // Free entire class hierarchy
    free_class(system->root_class);

    pthread_mutex_unlock(&system->htb_lock);
    pthread_mutex_destroy(&system->htb_lock);

    free(system);
}

// Demonstrate HTB System
void demonstrate_htb_system() {
    // Create HTB configuration
    htb_config_t config = {
        .rate = 1000000000,    // 1 Gbps root rate
        .ceil = 2000000000,    // 2 Gbps root ceiling
        .max_classes = 100,     // Maximum 100 classes
        .quantum = 1500,        // MTU-sized quantum
        .rate_estimator = true  // Enable rate estimation
    };

    // Create HTB System
    htb_system_t *htb = create_htb_system(config);
    if (!htb) return;

    // Create child classes with different traffic types
    htb_class_t *classes[4];
    uint64_t rates[] = {100000000, 200000000, 300000000, 400000000};  // Different rates
    traffic_type_t types[] = {
        TRAFFIC_INTERACTIVE,
        TRAFFIC_BULK,
        TRAFFIC_REALTIME,
        TRAFFIC_BEST_EFFORT
    };

    for (int i = 0; i < 4; i++) {
        classes[i] = create_htb_class(
            htb,
            htb->root_class,
            i + 2,
            rates[i],
            rates[i] * 2,
            types[i]
        );

        if (classes[i]) {
            htb->root_class->children[htb->root_class->child_count++] = classes[i];
        }
    }

    // Simulate traffic through classes
    for (int i = 0; i < 1000; i++) {
        uint64_t current_time = time(NULL) + i;

        // Update tokens for all classes
        update_tokens(htb->root_class, current_time);
        for (int j = 0; j < 4; j++) {
            if (classes[j]) {
                update_tokens(classes[j], current_time);
            }
        }

        // Simulate packet transmission
        for (int j = 0; j < 4; j++) {
            if (classes[j]) {
                size_t packet_size = 1000 + (rand() % 1000);  // Random packet size
                
                if (can_send(classes[j], packet_size)) {
                    if (charge_class(classes[j], packet_size)) {
                        htb->stats.total_packets++;
                        htb->stats.total_bytes += packet_size;
                    } else {
                        htb->stats.dropped_packets++;
                    }
                } else {
                    htb->stats.overlimit_events++;
                }

                update_class_state(classes[j]);
            }
        }
    }

    // Print Statistics
    print_htb_stats(htb);

    // Cleanup
    destroy_htb_system(htb);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_htb_system();

    return 0;
}
