#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>

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

// Alarm Timer Constants
#define MAX_ALARMS          256
#define MAX_HANDLERS        64
#define MIN_ALARM_INTERVAL  1000     // 1ms minimum interval
#define MAX_ALARM_INTERVAL  86400000 // 24h maximum interval
#define TEST_DURATION       30       // seconds

// Alarm Types
typedef enum {
    ALARM_REALTIME,
    ALARM_BOOTTIME,
    ALARM_MONOTONIC,
    ALARM_POWEROFF
} alarm_type_t;

// Alarm States
typedef enum {
    ALARM_STATE_INACTIVE,
    ALARM_STATE_WAITING,
    ALARM_STATE_FIRED,
    ALARM_STATE_EXPIRED
} alarm_state_t;

// Alarm Flags
typedef enum {
    ALARM_FLAG_NONE      = 0,
    ALARM_FLAG_RELATIVE  = 1 << 0,
    ALARM_FLAG_PERIODIC  = 1 << 1,
    ALARM_FLAG_WAKE_SYSTEM = 1 << 2
} alarm_flags_t;

// Alarm Structure
typedef struct alarm {
    unsigned int id;
    alarm_type_t type;
    alarm_state_t state;
    alarm_flags_t flags;
    uint64_t expires;
    uint64_t period;
    void (*callback)(void *data);
    void *data;
    struct alarm *next;
} alarm_t;

// Alarm Queue Structure
typedef struct {
    alarm_t *head;
    size_t count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} alarm_queue_t;

// Alarm Statistics Structure
typedef struct {
    uint64_t total_alarms;
    uint64_t active_alarms;
    uint64_t fired_alarms;
    uint64_t expired_alarms;
    uint64_t periodic_alarms;
    uint64_t wakeup_events;
    double avg_latency;
    double max_latency;
    double min_latency;
    double test_duration;
} alarm_stats_t;

// Alarm Manager Structure
typedef struct {
    alarm_queue_t queues[4];  // One queue per alarm type
    alarm_t *alarms[MAX_ALARMS];
    size_t nr_alarms;
    bool running;
    bool system_suspended;
    pthread_mutex_t manager_lock;
    pthread_t timer_thread;
    pthread_t suspend_thread;
    alarm_stats_t stats;
} alarm_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_alarm_type_string(alarm_type_t type);
const char* get_alarm_state_string(alarm_state_t state);

alarm_manager_t* create_alarm_manager(void);
void destroy_alarm_manager(alarm_manager_t *manager);

alarm_t* create_alarm(alarm_type_t type, alarm_flags_t flags,
    uint64_t expires, void (*callback)(void *data), void *data);
void destroy_alarm(alarm_t *alarm);

int set_alarm(alarm_manager_t *manager, alarm_t *alarm);
int cancel_alarm(alarm_manager_t *manager, unsigned int alarm_id);

void* timer_thread(void *arg);
void* suspend_thread(void *arg);
void process_alarms(alarm_manager_t *manager);
void simulate_system_suspend(alarm_manager_t *manager);

void run_test(alarm_manager_t *manager);
void calculate_stats(alarm_manager_t *manager);
void print_test_stats(alarm_manager_t *manager);
void demonstrate_alarms(void);

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

const char* get_alarm_type_string(alarm_type_t type) {
    switch(type) {
        case ALARM_REALTIME:  return "REALTIME";
        case ALARM_BOOTTIME:  return "BOOTTIME";
        case ALARM_MONOTONIC: return "MONOTONIC";
        case ALARM_POWEROFF:  return "POWEROFF";
        default: return "UNKNOWN";
    }
}

const char* get_alarm_state_string(alarm_state_t state) {
    switch(state) {
        case ALARM_STATE_INACTIVE: return "INACTIVE";
        case ALARM_STATE_WAITING:  return "WAITING";
        case ALARM_STATE_FIRED:    return "FIRED";
        case ALARM_STATE_EXPIRED:  return "EXPIRED";
        default: return "UNKNOWN";
    }
}

// Create Alarm Queue
alarm_queue_t* create_alarm_queue(void) {
    alarm_queue_t *queue = malloc(sizeof(alarm_queue_t));
    if (!queue) return NULL;

    queue->head = NULL;
    queue->count = 0;
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->cond, NULL);

    return queue;
}

// Create Alarm
alarm_t* create_alarm(alarm_type_t type, alarm_flags_t flags,
    uint64_t expires, void (*callback)(void *data), void *data) {
    static unsigned int next_id = 0;
    alarm_t *alarm = malloc(sizeof(alarm_t));
    if (!alarm) return NULL;

    alarm->id = next_id++;
    alarm->type = type;
    alarm->state = ALARM_STATE_INACTIVE;
    alarm->flags = flags;
    alarm->expires = expires;
    alarm->period = (flags & ALARM_FLAG_PERIODIC) ? expires : 0;
    alarm->callback = callback;
    alarm->data = data;
    alarm->next = NULL;

    return alarm;
}

// Create Alarm Manager
alarm_manager_t* create_alarm_manager(void) {
    alarm_manager_t *manager = malloc(sizeof(alarm_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate alarm manager");
        return NULL;
    }

    // Initialize alarm queues
    for (size_t i = 0; i < 4; i++) {
        alarm_queue_t *queue = create_alarm_queue();
        if (!queue) {
            // Cleanup and return
            for (size_t j = 0; j < i; j++) {
                pthread_mutex_destroy(&manager->queues[j].lock);
                pthread_cond_destroy(&manager->queues[j].cond);
                free(&manager->queues[j]);
            }
            free(manager);
            return NULL;
        }
        memcpy(&manager->queues[i], queue, sizeof(alarm_queue_t));
        free(queue);
    }

    manager->nr_alarms = 0;
    manager->running = false;
    manager->system_suspended = false;
    pthread_mutex_init(&manager->manager_lock, NULL);
    memset(&manager->stats, 0, sizeof(alarm_stats_t));

    LOG(LOG_LEVEL_DEBUG, "Created alarm manager");
    return manager;
}

// Set Alarm
int set_alarm(alarm_manager_t *manager, alarm_t *alarm) {
    if (!manager || !alarm || alarm->type >= 4) return -1;

    pthread_mutex_lock(&manager->manager_lock);
    if (manager->nr_alarms >= MAX_ALARMS) {
        pthread_mutex_unlock(&manager->manager_lock);
        return -1;
    }

    // Add to manager's alarm list
    manager->alarms[manager->nr_alarms++] = alarm;

    // Add to appropriate queue
    alarm_queue_t *queue = &manager->queues[alarm->type];
    pthread_mutex_lock(&queue->lock);

    // Insert in sorted order by expiry time
    alarm_t **pp = &queue->head;
    while (*pp && (*pp)->expires <= alarm->expires) {
        pp = &(*pp)->next;
    }
    alarm->next = *pp;
    *pp = alarm;
    queue->count++;

    alarm->state = ALARM_STATE_WAITING;
    manager->stats.total_alarms++;
    manager->stats.active_alarms++;

    if (alarm->flags & ALARM_FLAG_PERIODIC) {
        manager->stats.periodic_alarms++;
    }

    pthread_mutex_unlock(&queue->lock);
    pthread_mutex_unlock(&manager->manager_lock);

    LOG(LOG_LEVEL_DEBUG, "Set alarm %u (type: %s, expires: %lu)",
        alarm->id, get_alarm_type_string(alarm->type), alarm->expires);
    return 0;
}

// Cancel Alarm
int cancel_alarm(alarm_manager_t *manager, unsigned int alarm_id) {
    if (!manager) return -1;

    pthread_mutex_lock(&manager->manager_lock);
    alarm_t *alarm = NULL;
    size_t index;

    // Find alarm in manager's list
    for (index = 0; index < manager->nr_alarms; index++) {
        if (manager->alarms[index]->id == alarm_id) {
            alarm = manager->alarms[index];
            break;
        }
    }

    if (!alarm) {
        pthread_mutex_unlock(&manager->manager_lock);
        return -1;
    }

    // Remove from queue
    alarm_queue_t *queue = &manager->queues[alarm->type];
    pthread_mutex_lock(&queue->lock);

    alarm_t **pp = &queue->head;
    while (*pp && (*pp)->id != alarm_id) {
        pp = &(*pp)->next;
    }
    if (*pp) {
        *pp = (*pp)->next;
        queue->count--;
    }

    alarm->state = ALARM_STATE_INACTIVE;
    manager->stats.active_alarms--;

    pthread_mutex_unlock(&queue->lock);

    // Remove from manager's list
    for (size_t i = index; i < manager->nr_alarms - 1; i++) {
        manager->alarms[i] = manager->alarms[i + 1];
    }
    manager->nr_alarms--;

    pthread_mutex_unlock(&manager->manager_lock);

    LOG(LOG_LEVEL_DEBUG, "Cancelled alarm %u", alarm_id);
    return 0;
}

// Timer Thread
void* timer_thread(void *arg) {
    alarm_manager_t *manager = (alarm_manager_t*)arg;

    while (manager->running) {
        if (!manager->system_suspended) {
            process_alarms(manager);
        }
        usleep(1000);  // 1ms tick
    }

    return NULL;
}

// Process Alarms
void process_alarms(alarm_manager_t *manager) {
    if (!manager) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t current_time = now.tv_sec * 1000000000ULL + now.tv_nsec;

    // Process each queue
    for (size_t i = 0; i < 4; i++) {
        alarm_queue_t *queue = &manager->queues[i];
        pthread_mutex_lock(&queue->lock);

        alarm_t *alarm = queue->head;
        alarm_t *prev = NULL;

        while (alarm && alarm->expires <= current_time) {
            // Fire alarm
            if (alarm->callback) {
                alarm->callback(alarm->data);
            }

            alarm->state = ALARM_STATE_FIRED;
            manager->stats.fired_alarms++;

            // Calculate latency
            double latency = (current_time - alarm->expires) / 1000000.0;  // ms
            if (latency > manager->stats.max_latency) {
                manager->stats.max_latency = latency;
            }
            if (latency < manager->stats.min_latency || 
                manager->stats.min_latency == 0) {
                manager->stats.min_latency = latency;
            }

            // Handle periodic alarms
            if (alarm->flags & ALARM_FLAG_PERIODIC) {
                alarm->expires += alarm->period;
                // Reinsert in sorted order
                alarm_t **pp = &queue->head;
                while (*pp && (*pp)->expires <= alarm->expires) {
                    pp = &(*pp)->next;
                }
                if (prev) prev->next = alarm->next;
                else queue->head = alarm->next;
                alarm->next = *pp;
                *pp = alarm;
            } else {
                // Remove non-periodic alarm
                if (prev) prev->next = alarm->next;
                else queue->head = alarm->next;
                queue->count--;
                alarm->state = ALARM_STATE_EXPIRED;
                manager->stats.expired_alarms++;
                manager->stats.active_alarms--;
            }

            alarm = alarm->next;
        }

        pthread_mutex_unlock(&queue->lock);
    }
}

// Suspend Thread
void* suspend_thread(void *arg) {
    alarm_manager_t *manager = (alarm_manager_t*)arg;

    while (manager->running) {
        simulate_system_suspend(manager);
        sleep(rand() % 10 + 5);  // Random interval between suspends
    }

    return NULL;
}

// Simulate System Suspend
void simulate_system_suspend(alarm_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);
    manager->system_suspended = true;
    LOG(LOG_LEVEL_INFO, "System entering suspend state");

    // Check for wake alarms
    bool has_wake_alarm = false;
    for (size_t i = 0; i < 4; i++) {
        alarm_queue_t *queue = &manager->queues[i];
        pthread_mutex_lock(&queue->lock);

        alarm_t *alarm = queue->head;
        while (alarm) {
            if (alarm->flags & ALARM_FLAG_WAKE_SYSTEM) {
                has_wake_alarm = true;
                manager->stats.wakeup_events++;
                break;
            }
            alarm = alarm->next;
        }

        pthread_mutex_unlock(&queue->lock);
        if (has_wake_alarm) break;
    }

    // Simulate suspend
    sleep(1);

    // Wake up
    manager->system_suspended = false;
    LOG(LOG_LEVEL_INFO, "System resuming from suspend (wake alarm: %s)",
        has_wake_alarm ? "yes" : "no");

    pthread_mutex_unlock(&manager->manager_lock);
}

// Run Test
void run_test(alarm_manager_t *manager) {
    if (!manager) return;

    LOG(LOG_LEVEL_INFO, "Starting alarm timer test...");

    // Create test alarms
    for (size_t i = 0; i < MAX_ALARMS/4; i++) {
        // Create various types of alarms
        alarm_type_t type = rand() % 4;
        alarm_flags_t flags = ALARM_FLAG_NONE;
        if (rand() % 2) flags |= ALARM_FLAG_RELATIVE;
        if (rand() % 3 == 0) flags |= ALARM_FLAG_PERIODIC;
        if (rand() % 4 == 0) flags |= ALARM_FLAG_WAKE_SYSTEM;

        uint64_t expires = rand() % 
            (MAX_ALARM_INTERVAL - MIN_ALARM_INTERVAL) + MIN_ALARM_INTERVAL;

        alarm_t *alarm = create_alarm(type, flags, expires,
            NULL, NULL);  // No callback for test
        if (alarm) {
            set_alarm(manager, alarm);
        }
    }

    // Start threads
    manager->running = true;
    pthread_create(&manager->timer_thread, NULL, timer_thread, manager);
    pthread_create(&manager->suspend_thread, NULL, suspend_thread, manager);

    // Run test
    sleep(TEST_DURATION);

    // Stop threads
    manager->running = false;
    pthread_join(manager->timer_thread, NULL);
    pthread_join(manager->suspend_thread, NULL);

    // Calculate statistics
    calculate_stats(manager);
}

// Calculate Statistics
void calculate_stats(alarm_manager_t *manager) {
    if (!manager) return;

    if (manager->stats.fired_alarms > 0) {
        manager->stats.avg_latency = 
            (manager->stats.max_latency + manager->stats.min_latency) / 2.0;
    }

    manager->stats.test_duration = TEST_DURATION;
}

// Print Test Statistics
void print_test_stats(alarm_manager_t *manager) {
    if (!manager) return;

    printf("\nAlarm Timer Test Results:\n");
    printf("-------------------------\n");
    printf("Test Duration:     %.2f seconds\n", manager->stats.test_duration);
    printf("Total Alarms:      %lu\n", manager->stats.total_alarms);
    printf("Active Alarms:     %lu\n", manager->stats.active_alarms);
    printf("Fired Alarms:      %lu\n", manager->stats.fired_alarms);
    printf("Expired Alarms:    %lu\n", manager->stats.expired_alarms);
    printf("Periodic Alarms:   %lu\n", manager->stats.periodic_alarms);
    printf("Wakeup Events:     %lu\n", manager->stats.wakeup_events);
    printf("Avg Latency:       %.2f ms\n", manager->stats.avg_latency);
    printf("Max Latency:       %.2f ms\n", manager->stats.max_latency);
    printf("Min Latency:       %.2f ms\n", manager->stats.min_latency);

    // Print queue details
    printf("\nQueue Details:\n");
    for (size_t i = 0; i < 4; i++) {
        alarm_queue_t *queue = &manager->queues[i];
        printf("  %s Queue: %zu alarms\n",
            get_alarm_type_string(i), queue->count);
    }
}

// Destroy Alarm
void destroy_alarm(alarm_t *alarm) {
    free(alarm);
}

// Destroy Alarm Queue
void destroy_alarm_queue(alarm_queue_t *queue) {
    if (!queue) return;

    alarm_t *current = queue->head;
    while (current) {
        alarm_t *next = current->next;
        destroy_alarm(current);
        current = next;
    }

    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->cond);
}

// Destroy Alarm Manager
void destroy_alarm_manager(alarm_manager_t *manager) {
    if (!manager) return;

    // Clean up alarms
    for (size_t i = 0; i < manager->nr_alarms; i++) {
        destroy_alarm(manager->alarms[i]);
    }

    // Clean up queues
    for (size_t i = 0; i < 4; i++) {
        destroy_alarm_queue(&manager->queues[i]);
    }

    pthread_mutex_destroy(&manager->manager_lock);
    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed alarm manager");
}

// Demonstrate Alarms
void demonstrate_alarms(void) {
    printf("Starting alarm timer demonstration...\n");

    // Create and run alarm timer test
    alarm_manager_t *manager = create_alarm_manager();
    if (manager) {
        run_test(manager);
        print_test_stats(manager);
        destroy_alarm_manager(manager);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_alarms();

    return 0;
}
