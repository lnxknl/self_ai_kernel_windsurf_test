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

// BBR Constants
#define BBR_SCALE 8
#define BBR_UNIT (1 << BBR_SCALE)
#define BBR_MAX_DATAGRAM_SIZE 1500
#define BBR_MIN_WINDOW_SIZE 4
#define BBR_MAX_WINDOW_SIZE (64 * 1024)
#define BBR_RTT_PROBE_INTERVAL 10
#define BBR_STARTUP_GROWTH_TARGET 1.25
#define BBR_HIGH_GAIN 2.885
#define BBR_DRAIN_GAIN (1.0 / 2.885)
#define BBR_PROBE_RTT_GAIN 0.75
#define BBR_MIN_CWND_GAIN 0.3
#define BBR_PROBE_RTT_DURATION_MS 200
#define TEST_DURATION 30

// BBR States
typedef enum {
    BBR_STARTUP,
    BBR_DRAIN,
    BBR_PROBE_BW,
    BBR_PROBE_RTT
} bbr_state_t;

// BBR Mode
typedef enum {
    BBR_MEASURING_BW,
    BBR_NOT_IN_RECOVERY,
    BBR_IS_RESTARTING,
    BBR_IN_RECOVERY
} bbr_mode_t;

// RTT Sample Structure
typedef struct {
    uint32_t rtt_us;
    uint32_t delivered;
    uint64_t timestamp;
} rtt_sample_t;

// Bandwidth Sample Structure
typedef struct {
    uint32_t bandwidth;    // bytes per second
    uint32_t rtt_us;
    uint32_t delivered;
    bool is_app_limited;
} bw_sample_t;

// BBR Connection Structure
typedef struct {
    // Current state
    bbr_state_t state;
    bbr_mode_t mode;
    
    // Window and rate control
    uint32_t min_rtt_us;
    uint32_t max_bw;
    uint32_t cwnd;
    uint32_t pacing_rate;
    uint32_t sending_rate;
    
    // Bandwidth estimation
    uint32_t btl_bw;
    uint32_t full_bw;
    int full_bw_cnt;
    bool full_bw_reached;
    
    // RTT measurements
    uint32_t min_rtt_stamp;
    uint32_t probe_rtt_done_stamp;
    bool probe_rtt_round_done;
    
    // Pacing gain cycle
    double pacing_gain;
    double cwnd_gain;
    int cycle_idx;
    
    // Statistics
    uint64_t total_bytes_sent;
    uint64_t total_bytes_acked;
    uint32_t lost_packets;
    uint32_t delivered_packets;
    
    // Locks
    pthread_mutex_t lock;
} bbr_connection_t;

// BBR Manager Structure
typedef struct {
    bbr_connection_t *connections;
    size_t nr_connections;
    bool running;
    pthread_t monitor_thread;
    pthread_mutex_t manager_lock;
} bbr_manager_t;

// Statistics Structure
typedef struct {
    uint64_t bytes_sent;
    uint64_t bytes_acked;
    uint32_t lost_packets;
    uint32_t delivered_packets;
    uint32_t avg_rtt_us;
    uint32_t min_rtt_us;
    uint32_t max_rtt_us;
    uint32_t avg_bandwidth;
    uint32_t peak_bandwidth;
} bbr_stats_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_bbr_state_string(bbr_state_t state);

bbr_manager_t* create_bbr_manager(size_t nr_connections);
void destroy_bbr_manager(bbr_manager_t *manager);

bbr_connection_t* create_bbr_connection(void);
void destroy_bbr_connection(bbr_connection_t *conn);

void bbr_init(bbr_connection_t *conn);
void bbr_update_model(bbr_connection_t *conn, const bw_sample_t *sample);
void bbr_update_control(bbr_connection_t *conn);
void bbr_check_state_transitions(bbr_connection_t *conn);
void bbr_update_gains(bbr_connection_t *conn);

void bbr_enter_startup(bbr_connection_t *conn);
void bbr_enter_drain(bbr_connection_t *conn);
void bbr_enter_probe_bw(bbr_connection_t *conn);
void bbr_enter_probe_rtt(bbr_connection_t *conn);

void* monitor_thread(void *arg);
void run_test(bbr_manager_t *manager);
void calculate_stats(bbr_manager_t *manager, bbr_stats_t *stats);
void print_test_stats(const bbr_stats_t *stats);
void demonstrate_bbr(void);

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

const char* get_bbr_state_string(bbr_state_t state) {
    switch(state) {
        case BBR_STARTUP:   return "STARTUP";
        case BBR_DRAIN:     return "DRAIN";
        case BBR_PROBE_BW:  return "PROBE_BW";
        case BBR_PROBE_RTT: return "PROBE_RTT";
        default: return "UNKNOWN";
    }
}

// Create BBR Connection
bbr_connection_t* create_bbr_connection(void) {
    bbr_connection_t *conn = malloc(sizeof(bbr_connection_t));
    if (!conn) return NULL;

    memset(conn, 0, sizeof(bbr_connection_t));
    pthread_mutex_init(&conn->lock, NULL);
    bbr_init(conn);

    return conn;
}

// Initialize BBR Connection
void bbr_init(bbr_connection_t *conn) {
    if (!conn) return;

    conn->state = BBR_STARTUP;
    conn->mode = BBR_NOT_IN_RECOVERY;
    conn->min_rtt_us = UINT32_MAX;
    conn->max_bw = 0;
    conn->cwnd = BBR_MIN_WINDOW_SIZE * BBR_MAX_DATAGRAM_SIZE;
    conn->pacing_rate = BBR_HIGH_GAIN * BBR_MAX_DATAGRAM_SIZE;
    conn->btl_bw = 0;
    conn->full_bw = 0;
    conn->full_bw_cnt = 0;
    conn->full_bw_reached = false;
    conn->pacing_gain = BBR_HIGH_GAIN;
    conn->cwnd_gain = BBR_HIGH_GAIN;
}

// Create BBR Manager
bbr_manager_t* create_bbr_manager(size_t nr_connections) {
    bbr_manager_t *manager = malloc(sizeof(bbr_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate BBR manager");
        return NULL;
    }

    manager->connections = malloc(sizeof(bbr_connection_t) * nr_connections);
    if (!manager->connections) {
        free(manager);
        return NULL;
    }

    manager->nr_connections = nr_connections;
    manager->running = false;
    pthread_mutex_init(&manager->manager_lock, NULL);

    for (size_t i = 0; i < nr_connections; i++) {
        bbr_connection_t *conn = create_bbr_connection();
        if (conn) {
            memcpy(&manager->connections[i], conn, sizeof(bbr_connection_t));
            free(conn);
        }
    }

    LOG(LOG_LEVEL_DEBUG, "Created BBR manager with %zu connections", nr_connections);
    return manager;
}

// Update BBR Model
void bbr_update_model(bbr_connection_t *conn, const bw_sample_t *sample) {
    if (!conn || !sample) return;

    pthread_mutex_lock(&conn->lock);

    // Update minimum RTT
    if (sample->rtt_us < conn->min_rtt_us) {
        conn->min_rtt_us = sample->rtt_us;
        conn->min_rtt_stamp = time(NULL);
    }

    // Update maximum bandwidth
    if (sample->bandwidth > conn->max_bw && !sample->is_app_limited) {
        conn->max_bw = sample->bandwidth;
    }

    // Check if we've reached full bandwidth
    if (!conn->full_bw_reached) {
        if (conn->max_bw >= conn->full_bw * BBR_STARTUP_GROWTH_TARGET) {
            conn->full_bw = conn->max_bw;
            conn->full_bw_cnt = 0;
        } else if (++conn->full_bw_cnt >= 3) {
            conn->full_bw_reached = true;
        }
    }

    pthread_mutex_unlock(&conn->lock);
}

// Update BBR Control Parameters
void bbr_update_control(bbr_connection_t *conn) {
    if (!conn) return;

    pthread_mutex_lock(&conn->lock);

    // Update pacing rate
    uint32_t target_rate = (uint32_t)(conn->btl_bw * conn->pacing_gain);
    conn->pacing_rate = target_rate;

    // Update congestion window
    uint32_t target_cwnd = (uint32_t)(conn->btl_bw * conn->min_rtt_us * conn->cwnd_gain / 1000000);
    target_cwnd = target_cwnd < BBR_MIN_WINDOW_SIZE ? BBR_MIN_WINDOW_SIZE : target_cwnd;
    target_cwnd = target_cwnd > BBR_MAX_WINDOW_SIZE ? BBR_MAX_WINDOW_SIZE : target_cwnd;
    conn->cwnd = target_cwnd;

    pthread_mutex_unlock(&conn->lock);
}

// Check State Transitions
void bbr_check_state_transitions(bbr_connection_t *conn) {
    if (!conn) return;

    pthread_mutex_lock(&conn->lock);

    time_t now = time(NULL);
    bbr_state_t old_state = conn->state;

    switch (conn->state) {
        case BBR_STARTUP:
            if (conn->full_bw_reached) {
                bbr_enter_drain(conn);
            }
            break;

        case BBR_DRAIN:
            if (conn->pacing_rate <= conn->btl_bw) {
                bbr_enter_probe_bw(conn);
            }
            break;

        case BBR_PROBE_BW:
            if (now - conn->min_rtt_stamp > BBR_PROBE_RTT_INTERVAL) {
                bbr_enter_probe_rtt(conn);
            }
            break;

        case BBR_PROBE_RTT:
            if (now - conn->probe_rtt_done_stamp > BBR_PROBE_RTT_DURATION_MS/1000) {
                bbr_enter_probe_bw(conn);
            }
            break;
    }

    if (old_state != conn->state) {
        LOG(LOG_LEVEL_DEBUG, "State transition: %s -> %s",
            get_bbr_state_string(old_state),
            get_bbr_state_string(conn->state));
    }

    pthread_mutex_unlock(&conn->lock);
}

// State Transition Functions
void bbr_enter_startup(bbr_connection_t *conn) {
    if (!conn) return;
    conn->state = BBR_STARTUP;
    conn->pacing_gain = BBR_HIGH_GAIN;
    conn->cwnd_gain = BBR_HIGH_GAIN;
}

void bbr_enter_drain(bbr_connection_t *conn) {
    if (!conn) return;
    conn->state = BBR_DRAIN;
    conn->pacing_gain = BBR_DRAIN_GAIN;
    conn->cwnd_gain = BBR_HIGH_GAIN;
}

void bbr_enter_probe_bw(bbr_connection_t *conn) {
    if (!conn) return;
    conn->state = BBR_PROBE_BW;
    conn->pacing_gain = 1.0;
    conn->cwnd_gain = 2.0;
    conn->cycle_idx = 0;
}

void bbr_enter_probe_rtt(bbr_connection_t *conn) {
    if (!conn) return;
    conn->state = BBR_PROBE_RTT;
    conn->pacing_gain = BBR_PROBE_RTT_GAIN;
    conn->cwnd_gain = BBR_MIN_CWND_GAIN;
    conn->probe_rtt_done_stamp = time(NULL);
    conn->probe_rtt_round_done = false;
}

// Monitor Thread
void* monitor_thread(void *arg) {
    bbr_manager_t *manager = (bbr_manager_t*)arg;

    while (manager->running) {
        pthread_mutex_lock(&manager->manager_lock);

        for (size_t i = 0; i < manager->nr_connections; i++) {
            bbr_connection_t *conn = &manager->connections[i];
            
            // Simulate bandwidth sample
            bw_sample_t sample = {
                .bandwidth = rand() % 1000000 + 500000,  // 0.5-1.5 Mbps
                .rtt_us = rand() % 100000 + 10000,      // 10-110ms
                .delivered = BBR_MAX_DATAGRAM_SIZE,
                .is_app_limited = (rand() % 100) < 10    // 10% chance
            };

            // Update BBR state
            bbr_update_model(conn, &sample);
            bbr_check_state_transitions(conn);
            bbr_update_control(conn);

            // Simulate packet delivery
            conn->total_bytes_sent += BBR_MAX_DATAGRAM_SIZE;
            if (rand() % 100 < 98) {  // 98% delivery rate
                conn->total_bytes_acked += BBR_MAX_DATAGRAM_SIZE;
                conn->delivered_packets++;
            } else {
                conn->lost_packets++;
            }
        }

        pthread_mutex_unlock(&manager->manager_lock);
        usleep(10000);  // 10ms interval
    }

    return NULL;
}

// Run Test
void run_test(bbr_manager_t *manager) {
    if (!manager) return;

    LOG(LOG_LEVEL_INFO, "Starting BBR test with %zu connections...", manager->nr_connections);

    // Start monitor thread
    manager->running = true;
    pthread_create(&manager->monitor_thread, NULL, monitor_thread, manager);

    // Run for test duration
    sleep(TEST_DURATION);

    // Stop thread
    manager->running = false;
    pthread_join(manager->monitor_thread, NULL);

    // Calculate and print statistics
    bbr_stats_t stats;
    calculate_stats(manager, &stats);
    print_test_stats(&stats);
}

// Calculate Statistics
void calculate_stats(bbr_manager_t *manager, bbr_stats_t *stats) {
    if (!manager || !stats) return;

    memset(stats, 0, sizeof(bbr_stats_t));
    stats->min_rtt_us = UINT32_MAX;

    pthread_mutex_lock(&manager->manager_lock);

    for (size_t i = 0; i < manager->nr_connections; i++) {
        bbr_connection_t *conn = &manager->connections[i];
        
        stats->bytes_sent += conn->total_bytes_sent;
        stats->bytes_acked += conn->total_bytes_acked;
        stats->lost_packets += conn->lost_packets;
        stats->delivered_packets += conn->delivered_packets;
        
        if (conn->min_rtt_us < stats->min_rtt_us)
            stats->min_rtt_us = conn->min_rtt_us;
        if (conn->min_rtt_us > stats->max_rtt_us)
            stats->max_rtt_us = conn->min_rtt_us;
        
        stats->avg_rtt_us += conn->min_rtt_us;
        stats->avg_bandwidth += conn->btl_bw;
        if (conn->max_bw > stats->peak_bandwidth)
            stats->peak_bandwidth = conn->max_bw;
    }

    if (manager->nr_connections > 0) {
        stats->avg_rtt_us /= manager->nr_connections;
        stats->avg_bandwidth /= manager->nr_connections;
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Print Test Statistics
void print_test_stats(const bbr_stats_t *stats) {
    if (!stats) return;

    printf("\nBBR Test Results:\n");
    printf("-------------------------\n");
    printf("Test Duration:      %d seconds\n", TEST_DURATION);
    printf("Bytes Sent:         %lu\n", stats->bytes_sent);
    printf("Bytes Acked:        %lu\n", stats->bytes_acked);
    printf("Lost Packets:       %u\n", stats->lost_packets);
    printf("Delivered Packets:  %u\n", stats->delivered_packets);
    printf("Packet Loss Rate:   %.2f%%\n",
           100.0 * stats->lost_packets / (stats->lost_packets + stats->delivered_packets));
    printf("Average RTT:        %.2f ms\n", stats->avg_rtt_us / 1000.0);
    printf("Min RTT:           %.2f ms\n", stats->min_rtt_us / 1000.0);
    printf("Max RTT:           %.2f ms\n", stats->max_rtt_us / 1000.0);
    printf("Average Bandwidth:  %.2f Mbps\n", stats->avg_bandwidth / 125000.0);
    printf("Peak Bandwidth:    %.2f Mbps\n", stats->peak_bandwidth / 125000.0);
    printf("Goodput:           %.2f Mbps\n",
           (stats->bytes_acked * 8.0) / (TEST_DURATION * 1000000));
}

// Destroy BBR Connection
void destroy_bbr_connection(bbr_connection_t *conn) {
    if (!conn) return;
    pthread_mutex_destroy(&conn->lock);
}

// Destroy BBR Manager
void destroy_bbr_manager(bbr_manager_t *manager) {
    if (!manager) return;

    for (size_t i = 0; i < manager->nr_connections; i++) {
        destroy_bbr_connection(&manager->connections[i]);
    }

    free(manager->connections);
    pthread_mutex_destroy(&manager->manager_lock);
    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed BBR manager");
}

// Demonstrate BBR
void demonstrate_bbr(void) {
    printf("Starting BBR demonstration...\n");

    // Create and run BBR test with 5 connections
    bbr_manager_t *manager = create_bbr_manager(5);
    if (manager) {
        run_test(manager);
        destroy_bbr_manager(manager);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_bbr();

    return 0;
}
