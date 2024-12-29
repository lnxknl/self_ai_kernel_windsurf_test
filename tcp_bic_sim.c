#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
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

// BIC Constants
#define BIC_SCALE      41              // Scale factor for fixed point arithmetic
#define BIC_BETA       819             // Beta = 0.8 (819/1024)
#define BIC_MAX_INCREMENT 16           // Maximum window increment
#define BIC_LOW_WINDOW  14             // Low window threshold
#define BIC_FAST_CONVERGENCE 1         // Enable fast convergence
#define BIC_MAX_WINDOW  (64 * 1024)    // Maximum window size
#define BIC_MIN_WINDOW  4              // Minimum window size
#define TEST_DURATION   30             // Test duration in seconds

// BIC States
typedef enum {
    BIC_SLOW_START,
    BIC_CONGESTION_AVOIDANCE,
    BIC_MAX_PROBING,
    BIC_BINARY_SEARCH
} bic_state_t;

// Packet Event Types
typedef enum {
    EVENT_ACK,
    EVENT_LOSS,
    EVENT_TIMEOUT
} packet_event_t;

// BIC Connection Structure
typedef struct {
    // Window management
    uint32_t cwnd;              // Current window size
    uint32_t last_max_cwnd;     // Last maximum window
    uint32_t last_cwnd;         // Window size before reduction
    uint32_t min_cwnd;          // Minimum window size
    uint32_t max_cwnd;          // Maximum window size
    uint32_t ssthresh;          // Slow start threshold
    
    // BIC specific variables
    uint32_t cnt;               // Binary increase counter
    uint32_t bic_origin_point;  // Origin point of binary search
    uint32_t bic_target_win;    // Target window of binary search
    uint32_t bic_k;             // Time to origin point
    
    // Current state
    bic_state_t state;
    bool fast_convergence;
    
    // Statistics
    uint64_t total_bytes_sent;
    uint64_t total_bytes_acked;
    uint32_t lost_packets;
    uint32_t delivered_packets;
    uint32_t timeouts;
    uint32_t rtt_samples;
    uint32_t min_rtt;
    uint32_t max_rtt;
    uint32_t sum_rtt;
    
    // Locks
    pthread_mutex_t lock;
} bic_connection_t;

// BIC Manager Structure
typedef struct {
    bic_connection_t *connections;
    size_t nr_connections;
    bool running;
    pthread_t monitor_thread;
    pthread_mutex_t manager_lock;
} bic_manager_t;

// Statistics Structure
typedef struct {
    uint64_t bytes_sent;
    uint64_t bytes_acked;
    uint32_t lost_packets;
    uint32_t delivered_packets;
    uint32_t timeouts;
    uint32_t avg_rtt;
    uint32_t min_rtt;
    uint32_t max_rtt;
    double avg_cwnd;
    uint32_t max_cwnd;
} bic_stats_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_bic_state_string(bic_state_t state);

bic_manager_t* create_bic_manager(size_t nr_connections);
void destroy_bic_manager(bic_manager_t *manager);

bic_connection_t* create_bic_connection(void);
void destroy_bic_connection(bic_connection_t *conn);

void bic_init(bic_connection_t *conn);
void bic_reset(bic_connection_t *conn);
void bic_update_window(bic_connection_t *conn, packet_event_t event);
void bic_check_fast_convergence(bic_connection_t *conn);
uint32_t bic_update(bic_connection_t *conn);
void bic_handle_loss(bic_connection_t *conn);
void bic_handle_timeout(bic_connection_t *conn);

void* monitor_thread(void *arg);
void run_test(bic_manager_t *manager);
void calculate_stats(bic_manager_t *manager, bic_stats_t *stats);
void print_test_stats(const bic_stats_t *stats);
void demonstrate_bic(void);

// Utility Functions
const char* get_log_level_string(int level) {
    switch(level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

const char* get_bic_state_string(bic_state_t state) {
    switch(state) {
        case BIC_SLOW_START:           return "SLOW_START";
        case BIC_CONGESTION_AVOIDANCE: return "CONGESTION_AVOIDANCE";
        case BIC_MAX_PROBING:          return "MAX_PROBING";
        case BIC_BINARY_SEARCH:        return "BINARY_SEARCH";
        default: return "UNKNOWN";
    }
}

// Create BIC Connection
bic_connection_t* create_bic_connection(void) {
    bic_connection_t *conn = malloc(sizeof(bic_connection_t));
    if (!conn) return NULL;

    memset(conn, 0, sizeof(bic_connection_t));
    pthread_mutex_init(&conn->lock, NULL);
    bic_init(conn);

    return conn;
}

// Initialize BIC Connection
void bic_init(bic_connection_t *conn) {
    if (!conn) return;

    conn->cwnd = BIC_MIN_WINDOW;
    conn->last_max_cwnd = 0;
    conn->last_cwnd = 0;
    conn->min_cwnd = BIC_MIN_WINDOW;
    conn->max_cwnd = BIC_MAX_WINDOW;
    conn->ssthresh = BIC_MAX_WINDOW;
    
    conn->cnt = 0;
    conn->bic_origin_point = 0;
    conn->bic_target_win = 0;
    conn->bic_k = 0;
    
    conn->state = BIC_SLOW_START;
    conn->fast_convergence = BIC_FAST_CONVERGENCE;
    
    conn->min_rtt = UINT32_MAX;
    conn->max_rtt = 0;
    conn->sum_rtt = 0;
    conn->rtt_samples = 0;
}

// Create BIC Manager
bic_manager_t* create_bic_manager(size_t nr_connections) {
    bic_manager_t *manager = malloc(sizeof(bic_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate BIC manager");
        return NULL;
    }

    manager->connections = malloc(sizeof(bic_connection_t) * nr_connections);
    if (!manager->connections) {
        free(manager);
        return NULL;
    }

    manager->nr_connections = nr_connections;
    manager->running = false;
    pthread_mutex_init(&manager->manager_lock, NULL);

    for (size_t i = 0; i < nr_connections; i++) {
        bic_connection_t *conn = create_bic_connection();
        if (conn) {
            memcpy(&manager->connections[i], conn, sizeof(bic_connection_t));
            free(conn);
        }
    }

    LOG(LOG_LEVEL_DEBUG, "Created BIC manager with %zu connections", nr_connections);
    return manager;
}

// BIC Window Update
void bic_update_window(bic_connection_t *conn, packet_event_t event) {
    if (!conn) return;

    pthread_mutex_lock(&conn->lock);

    switch (event) {
        case EVENT_ACK:
            if (conn->state == BIC_SLOW_START) {
                if (conn->cwnd < conn->ssthresh) {
                    conn->cwnd++;
                } else {
                    conn->state = BIC_CONGESTION_AVOIDANCE;
                    conn->bic_origin_point = conn->cwnd;
                    conn->bic_target_win = conn->cwnd + BIC_MAX_INCREMENT;
                }
            } else {
                uint32_t increment = bic_update(conn);
                conn->cwnd += increment;
            }
            break;

        case EVENT_LOSS:
            bic_handle_loss(conn);
            break;

        case EVENT_TIMEOUT:
            bic_handle_timeout(conn);
            break;
    }

    // Ensure window stays within bounds
    if (conn->cwnd < conn->min_cwnd)
        conn->cwnd = conn->min_cwnd;
    if (conn->cwnd > conn->max_cwnd)
        conn->cwnd = conn->max_cwnd;

    pthread_mutex_unlock(&conn->lock);
}

// BIC Update Algorithm
uint32_t bic_update(bic_connection_t *conn) {
    if (!conn) return 0;

    uint32_t target = conn->bic_target_win;
    uint32_t current = conn->cwnd;
    uint32_t increment;

    if (current < target) {
        // Binary search increase
        increment = (target - current) >> 1;
        if (increment > BIC_MAX_INCREMENT)
            increment = BIC_MAX_INCREMENT;
    } else {
        // Max probing
        increment = 1;
        if (conn->cnt > BIC_LOW_WINDOW) {
            increment = conn->cnt / BIC_LOW_WINDOW;
            if (increment > BIC_MAX_INCREMENT)
                increment = BIC_MAX_INCREMENT;
        }
        target = current + increment;
    }

    conn->cnt++;
    return increment;
}

// Handle Packet Loss
void bic_handle_loss(bic_connection_t *conn) {
    if (!conn) return;

    conn->last_cwnd = conn->cwnd;
    conn->cwnd = conn->cwnd * BIC_BETA / 1024;
    conn->ssthresh = conn->cwnd;
    
    if (conn->fast_convergence && conn->cwnd < conn->last_max_cwnd) {
        conn->last_max_cwnd = conn->cwnd * (1024 + BIC_BETA) / 2048;
    } else {
        conn->last_max_cwnd = conn->cwnd;
    }
    
    conn->bic_origin_point = conn->cwnd;
    conn->bic_target_win = conn->cwnd + BIC_MAX_INCREMENT;
    conn->cnt = 0;
    conn->state = BIC_CONGESTION_AVOIDANCE;
}

// Handle Timeout
void bic_handle_timeout(bic_connection_t *conn) {
    if (!conn) return;

    conn->cwnd = BIC_MIN_WINDOW;
    conn->ssthresh = conn->cwnd;
    conn->cnt = 0;
    conn->state = BIC_SLOW_START;
    conn->timeouts++;
}

// Monitor Thread
void* monitor_thread(void *arg) {
    bic_manager_t *manager = (bic_manager_t*)arg;

    while (manager->running) {
        pthread_mutex_lock(&manager->manager_lock);

        for (size_t i = 0; i < manager->nr_connections; i++) {
            bic_connection_t *conn = &manager->connections[i];
            
            // Simulate packet events
            int event_type = rand() % 100;
            if (event_type < 98) {  // 98% ACK
                bic_update_window(conn, EVENT_ACK);
                conn->total_bytes_sent += conn->cwnd;
                conn->total_bytes_acked += conn->cwnd;
                conn->delivered_packets++;
            } else if (event_type < 99) {  // 1% loss
                bic_update_window(conn, EVENT_LOSS);
                conn->lost_packets++;
            } else {  // 1% timeout
                bic_update_window(conn, EVENT_TIMEOUT);
            }

            // Simulate RTT measurement
            uint32_t rtt = rand() % 100000 + 10000;  // 10-110ms
            conn->sum_rtt += rtt;
            conn->rtt_samples++;
            if (rtt < conn->min_rtt) conn->min_rtt = rtt;
            if (rtt > conn->max_rtt) conn->max_rtt = rtt;
        }

        pthread_mutex_unlock(&manager->manager_lock);
        usleep(10000);  // 10ms interval
    }

    return NULL;
}

// Run Test
void run_test(bic_manager_t *manager) {
    if (!manager) return;

    LOG(LOG_LEVEL_INFO, "Starting BIC test with %zu connections...", manager->nr_connections);

    // Start monitor thread
    manager->running = true;
    pthread_create(&manager->monitor_thread, NULL, monitor_thread, manager);

    // Run for test duration
    sleep(TEST_DURATION);

    // Stop thread
    manager->running = false;
    pthread_join(manager->monitor_thread, NULL);

    // Calculate and print statistics
    bic_stats_t stats;
    calculate_stats(manager, &stats);
    print_test_stats(&stats);
}

// Calculate Statistics
void calculate_stats(bic_manager_t *manager, bic_stats_t *stats) {
    if (!manager || !stats) return;

    memset(stats, 0, sizeof(bic_stats_t));
    stats->min_rtt = UINT32_MAX;
    double total_cwnd = 0;

    pthread_mutex_lock(&manager->manager_lock);

    for (size_t i = 0; i < manager->nr_connections; i++) {
        bic_connection_t *conn = &manager->connections[i];
        
        stats->bytes_sent += conn->total_bytes_sent;
        stats->bytes_acked += conn->total_bytes_acked;
        stats->lost_packets += conn->lost_packets;
        stats->delivered_packets += conn->delivered_packets;
        stats->timeouts += conn->timeouts;
        
        if (conn->min_rtt < stats->min_rtt)
            stats->min_rtt = conn->min_rtt;
        if (conn->max_rtt > stats->max_rtt)
            stats->max_rtt = conn->max_rtt;
        
        if (conn->rtt_samples > 0)
            stats->avg_rtt += conn->sum_rtt / conn->rtt_samples;
        
        total_cwnd += conn->cwnd;
        if (conn->cwnd > stats->max_cwnd)
            stats->max_cwnd = conn->cwnd;
    }

    if (manager->nr_connections > 0) {
        stats->avg_rtt /= manager->nr_connections;
        stats->avg_cwnd = total_cwnd / manager->nr_connections;
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Print Test Statistics
void print_test_stats(const bic_stats_t *stats) {
    if (!stats) return;

    printf("\nBIC Test Results:\n");
    printf("-------------------------\n");
    printf("Test Duration:      %d seconds\n", TEST_DURATION);
    printf("Bytes Sent:         %lu\n", stats->bytes_sent);
    printf("Bytes Acked:        %lu\n", stats->bytes_acked);
    printf("Lost Packets:       %u\n", stats->lost_packets);
    printf("Delivered Packets:  %u\n", stats->delivered_packets);
    printf("Timeouts:          %u\n", stats->timeouts);
    printf("Packet Loss Rate:   %.2f%%\n",
           100.0 * stats->lost_packets / (stats->lost_packets + stats->delivered_packets));
    printf("Average RTT:        %.2f ms\n", stats->avg_rtt / 1000.0);
    printf("Min RTT:           %.2f ms\n", stats->min_rtt / 1000.0);
    printf("Max RTT:           %.2f ms\n", stats->max_rtt / 1000.0);
    printf("Average CWND:      %.2f packets\n", stats->avg_cwnd);
    printf("Max CWND:          %u packets\n", stats->max_cwnd);
    printf("Goodput:           %.2f Mbps\n",
           (stats->bytes_acked * 8.0) / (TEST_DURATION * 1000000));
}

// Destroy BIC Connection
void destroy_bic_connection(bic_connection_t *conn) {
    if (!conn) return;
    pthread_mutex_destroy(&conn->lock);
}

// Destroy BIC Manager
void destroy_bic_manager(bic_manager_t *manager) {
    if (!manager) return;

    for (size_t i = 0; i < manager->nr_connections; i++) {
        destroy_bic_connection(&manager->connections[i]);
    }

    free(manager->connections);
    pthread_mutex_destroy(&manager->manager_lock);
    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed BIC manager");
}

// Demonstrate BIC
void demonstrate_bic(void) {
    printf("Starting BIC demonstration...\n");

    // Create and run BIC test with 5 connections
    bic_manager_t *manager = create_bic_manager(5);
    if (manager) {
        run_test(manager);
        destroy_bic_manager(manager);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_bic();

    return 0;
}
