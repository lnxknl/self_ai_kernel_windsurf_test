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

// HRTimer Constants
#define MAX_CPUS           16
#define MAX_TIMERS         1024
#define MAX_BASES          4
#define MIN_HRTIMER_DELTA  100     // 100ns minimum delta
#define MAX_HRTIMER_DELTA  1000000 // 1ms maximum delta
#define TEST_DURATION      30      // seconds

// Clock Bases
typedef enum {
    HRTIMER_BASE_MONOTONIC,
    HRTIMER_BASE_REALTIME,
    HRTIMER_BASE_BOOTTIME,
    HRTIMER_BASE_TAI
} hrtimer_base_type_t;

// Timer States
typedef enum {
    HRTIMER_STATE_INACTIVE,
    HRTIMER_STATE_ENQUEUED,
    HRTIMER_STATE_CALLBACK,
    HRTIMER_STATE_EXPIRED
} hrtimer_state_t;

// Timer Modes
typedef enum {
    HRTIMER_MODE_ABS,
    HRTIMER_MODE_REL,
    HRTIMER_MODE_PINNED,
    HRTIMER_MODE_SOFT
} hrtimer_mode_t;

// Timer Structure
typedef struct hrtimer {
    unsigned int id;
    hrtimer_base_type_t base;
    hrtimer_state_t state;
    hrtimer_mode_t mode;
    uint64_t expires;
    uint64_t softexpires;
    int (*function)(void *data);
    void *data;
    int cpu;
    struct hrtimer *next;
} hrtimer_t;

// CPU Base Structure
typedef struct {
    hrtimer_t *timers[MAX_BASES];
    uint64_t resolution;
    uint64_t offset;
    size_t nr_timers;
    pthread_mutex_t lock;
} cpu_base_t;

// Statistics Structure
typedef struct {
    uint64_t total_timers;
    uint64_t active_timers;
    uint64_t expired_timers;
    uint64_t migrations;
    uint64_t overruns;
    double avg_precision;
    double max_precision;
    double min_precision;
    double test_duration;
} hrtimer_stats_t;

// HRTimer Manager Structure
typedef struct {
    cpu_base_t cpu_bases[MAX_CPUS];
    hrtimer_t *timers[MAX_TIMERS];
    size_t nr_cpus;
    size_t nr_timers;
    bool running;
    pthread_mutex_t manager_lock;
    pthread_t timer_thread;
    pthread_t migration_thread;
    hrtimer_stats_t stats;
} hrtimer_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_base_type_string(hrtimer_base_type_t base);
const char* get_timer_state_string(hrtimer_state_t state);
const char* get_timer_mode_string(hrtimer_mode_t mode);

hrtimer_manager_t* create_hrtimer_manager(size_t nr_cpus);
void destroy_hrtimer_manager(hrtimer_manager_t *manager);

hrtimer_t* create_hrtimer(hrtimer_base_type_t base, hrtimer_mode_t mode,
    int (*function)(void *data), void *data);
void destroy_hrtimer(hrtimer_t *timer);

int start_hrtimer(hrtimer_manager_t *manager, hrtimer_t *timer, uint64_t expires);
int cancel_hrtimer(hrtimer_manager_t *manager, hrtimer_t *timer);

void* timer_thread(void *arg);
void* migration_thread(void *arg);
void process_timers(hrtimer_manager_t *manager);
void migrate_timers(hrtimer_manager_t *manager);

void run_test(hrtimer_manager_t *manager);
void calculate_stats(hrtimer_manager_t *manager);
void print_test_stats(hrtimer_manager_t *manager);
void demonstrate_hrtimers(void);

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

const char* get_base_type_string(hrtimer_base_type_t base) {
    switch(base) {
        case HRTIMER_BASE_MONOTONIC: return "MONOTONIC";
        case HRTIMER_BASE_REALTIME:  return "REALTIME";
        case HRTIMER_BASE_BOOTTIME:  return "BOOTTIME";
        case HRTIMER_BASE_TAI:       return "TAI";
        default: return "UNKNOWN";
    }
}

const char* get_timer_state_string(hrtimer_state_t state) {
    switch(state) {
        case HRTIMER_STATE_INACTIVE: return "INACTIVE";
        case HRTIMER_STATE_ENQUEUED: return "ENQUEUED";
        case HRTIMER_STATE_CALLBACK: return "CALLBACK";
        case HRTIMER_STATE_EXPIRED:  return "EXPIRED";
        default: return "UNKNOWN";
    }
}

const char* get_timer_mode_string(hrtimer_mode_t mode) {
    switch(mode) {
        case HRTIMER_MODE_ABS:    return "ABS";
        case HRTIMER_MODE_REL:    return "REL";
        case HRTIMER_MODE_PINNED: return "PINNED";
        case HRTIMER_MODE_SOFT:   return "SOFT";
        default: return "UNKNOWN";
    }
}

// Create HRTimer
hrtimer_t* create_hrtimer(hrtimer_base_type_t base, hrtimer_mode_t mode,
    int (*function)(void *data), void *data) {
    static unsigned int next_id = 0;
    hrtimer_t *timer = malloc(sizeof(hrtimer_t));
    if (!timer) return NULL;

    timer->id = next_id++;
    timer->base = base;
    timer->state = HRTIMER_STATE_INACTIVE;
    timer->mode = mode;
    timer->expires = 0;
    timer->softexpires = 0;
    timer->function = function;
    timer->data = data;
    timer->cpu = -1;
    timer->next = NULL;

    return timer;
}

// Create HRTimer Manager
hrtimer_manager_t* create_hrtimer_manager(size_t nr_cpus) {
    if (nr_cpus > MAX_CPUS) {
        LOG(LOG_LEVEL_ERROR, "Number of CPUs exceeds maximum");
        return NULL;
    }

    hrtimer_manager_t *manager = malloc(sizeof(hrtimer_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate hrtimer manager");
        return NULL;
    }

    // Initialize CPU bases
    for (size_t i = 0; i < nr_cpus; i++) {
        cpu_base_t *base = &manager->cpu_bases[i];
        memset(base->timers, 0, sizeof(base->timers));
        base->resolution = 1;  // 1ns resolution
        base->offset = 0;
        base->nr_timers = 0;
        pthread_mutex_init(&base->lock, NULL);
    }

    manager->nr_cpus = nr_cpus;
    manager->nr_timers = 0;
    manager->running = false;
    pthread_mutex_init(&manager->manager_lock, NULL);
    memset(&manager->stats, 0, sizeof(hrtimer_stats_t));

    LOG(LOG_LEVEL_DEBUG, "Created hrtimer manager with %zu CPUs", nr_cpus);
    return manager;
}

// Start HRTimer
int start_hrtimer(hrtimer_manager_t *manager, hrtimer_t *timer, uint64_t expires) {
    if (!manager || !timer) return -1;

    pthread_mutex_lock(&manager->manager_lock);
    if (manager->nr_timers >= MAX_TIMERS) {
        pthread_mutex_unlock(&manager->manager_lock);
        return -1;
    }

    // Assign to CPU
    int target_cpu = (timer->mode == HRTIMER_MODE_PINNED && timer->cpu >= 0) ?
        timer->cpu : rand() % manager->nr_cpus;
    timer->cpu = target_cpu;

    // Add to manager's timer list
    manager->timers[manager->nr_timers++] = timer;

    // Add to CPU base
    cpu_base_t *cpu_base = &manager->cpu_bases[target_cpu];
    pthread_mutex_lock(&cpu_base->lock);

    timer->expires = expires;
    timer->softexpires = expires;
    timer->state = HRTIMER_STATE_ENQUEUED;

    // Insert in sorted order
    hrtimer_t **pp = &cpu_base->timers[timer->base];
    while (*pp && (*pp)->expires <= timer->expires) {
        pp = &(*pp)->next;
    }
    timer->next = *pp;
    *pp = timer;
    cpu_base->nr_timers++;

    manager->stats.total_timers++;
    manager->stats.active_timers++;

    pthread_mutex_unlock(&cpu_base->lock);
    pthread_mutex_unlock(&manager->manager_lock);

    LOG(LOG_LEVEL_DEBUG, "Started timer %u (base: %s, mode: %s) on CPU %d",
        timer->id, get_base_type_string(timer->base),
        get_timer_mode_string(timer->mode), target_cpu);
    return 0;
}

// Cancel HRTimer
int cancel_hrtimer(hrtimer_manager_t *manager, hrtimer_t *timer) {
    if (!manager || !timer || timer->cpu < 0) return -1;

    pthread_mutex_lock(&manager->manager_lock);
    cpu_base_t *cpu_base = &manager->cpu_bases[timer->cpu];
    pthread_mutex_lock(&cpu_base->lock);

    // Remove from CPU base
    hrtimer_t **pp = &cpu_base->timers[timer->base];
    while (*pp && *pp != timer) {
        pp = &(*pp)->next;
    }
    if (*pp) {
        *pp = timer->next;
        timer->next = NULL;
        timer->state = HRTIMER_STATE_INACTIVE;
        cpu_base->nr_timers--;
        manager->stats.active_timers--;
    }

    pthread_mutex_unlock(&cpu_base->lock);
    pthread_mutex_unlock(&manager->manager_lock);

    LOG(LOG_LEVEL_DEBUG, "Cancelled timer %u", timer->id);
    return 0;
}

// Timer Thread
void* timer_thread(void *arg) {
    hrtimer_manager_t *manager = (hrtimer_manager_t*)arg;

    while (manager->running) {
        process_timers(manager);
        usleep(100);  // 100us tick
    }

    return NULL;
}

// Process Timers
void process_timers(hrtimer_manager_t *manager) {
    if (!manager) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t current_time = now.tv_sec * 1000000000ULL + now.tv_nsec;

    // Process each CPU base
    for (size_t cpu = 0; cpu < manager->nr_cpus; cpu++) {
        cpu_base_t *cpu_base = &manager->cpu_bases[cpu];
        pthread_mutex_lock(&cpu_base->lock);

        // Process each base type
        for (size_t base = 0; base < MAX_BASES; base++) {
            hrtimer_t *timer = cpu_base->timers[base];
            hrtimer_t *prev = NULL;

            while (timer && timer->expires <= current_time) {
                // Execute timer callback
                if (timer->function) {
                    timer->state = HRTIMER_STATE_CALLBACK;
                    int restart = timer->function(timer->data);
                    
                    // Handle restart
                    if (restart) {
                        timer->expires += timer->softexpires;
                        // Reinsert in sorted order
                        hrtimer_t **pp = &cpu_base->timers[base];
                        while (*pp && (*pp)->expires <= timer->expires) {
                            pp = &(*pp)->next;
                        }
                        if (prev) prev->next = timer->next;
                        else cpu_base->timers[base] = timer->next;
                        timer->next = *pp;
                        *pp = timer;
                        timer->state = HRTIMER_STATE_ENQUEUED;
                    } else {
                        // Remove expired timer
                        if (prev) prev->next = timer->next;
                        else cpu_base->timers[base] = timer->next;
                        timer->state = HRTIMER_STATE_EXPIRED;
                        cpu_base->nr_timers--;
                        manager->stats.active_timers--;
                        manager->stats.expired_timers++;
                    }
                }

                // Calculate precision
                double precision = (current_time - timer->expires) / 1000.0;  // us
                if (precision > manager->stats.max_precision) {
                    manager->stats.max_precision = precision;
                }
                if (precision < manager->stats.min_precision || 
                    manager->stats.min_precision == 0) {
                    manager->stats.min_precision = precision;
                }

                timer = timer->next;
            }
        }

        pthread_mutex_unlock(&cpu_base->lock);
    }
}

// Migration Thread
void* migration_thread(void *arg) {
    hrtimer_manager_t *manager = (hrtimer_manager_t*)arg;

    while (manager->running) {
        migrate_timers(manager);
        usleep(1000000);  // 1ms interval
    }

    return NULL;
}

// Migrate Timers
void migrate_timers(hrtimer_manager_t *manager) {
    if (!manager) return;

    // Find imbalanced CPUs
    size_t max_cpu = 0, min_cpu = 0;
    size_t max_timers = 0, min_timers = SIZE_MAX;

    for (size_t i = 0; i < manager->nr_cpus; i++) {
        cpu_base_t *base = &manager->cpu_bases[i];
        if (base->nr_timers > max_timers) {
            max_timers = base->nr_timers;
            max_cpu = i;
        }
        if (base->nr_timers < min_timers) {
            min_timers = base->nr_timers;
            min_cpu = i;
        }
    }

    // Migrate if significant imbalance
    if (max_timers - min_timers > 2) {
        cpu_base_t *src_base = &manager->cpu_bases[max_cpu];
        cpu_base_t *dst_base = &manager->cpu_bases[min_cpu];

        pthread_mutex_lock(&src_base->lock);
        pthread_mutex_lock(&dst_base->lock);

        // Find a timer to migrate
        for (size_t base = 0; base < MAX_BASES; base++) {
            hrtimer_t **pp = &src_base->timers[base];
            while (*pp) {
                hrtimer_t *timer = *pp;
                if (timer->mode != HRTIMER_MODE_PINNED) {
                    // Remove from source
                    *pp = timer->next;
                    src_base->nr_timers--;

                    // Add to destination
                    timer->cpu = min_cpu;
                    timer->next = dst_base->timers[base];
                    dst_base->timers[base] = timer;
                    dst_base->nr_timers++;

                    manager->stats.migrations++;
                    goto done_migration;
                }
                pp = &(*pp)->next;
            }
        }

done_migration:
        pthread_mutex_unlock(&dst_base->lock);
        pthread_mutex_unlock(&src_base->lock);
    }
}

// Run Test
void run_test(hrtimer_manager_t *manager) {
    if (!manager) return;

    LOG(LOG_LEVEL_INFO, "Starting hrtimer test...");

    // Create test timers
    for (size_t i = 0; i < MAX_TIMERS/4; i++) {
        hrtimer_base_type_t base = rand() % MAX_BASES;
        hrtimer_mode_t mode = rand() % 4;
        hrtimer_t *timer = create_hrtimer(base, mode, NULL, NULL);
        if (timer) {
            uint64_t expires = rand() % 
                (MAX_HRTIMER_DELTA - MIN_HRTIMER_DELTA) + MIN_HRTIMER_DELTA;
            start_hrtimer(manager, timer, expires);
        }
    }

    // Start threads
    manager->running = true;
    pthread_create(&manager->timer_thread, NULL, timer_thread, manager);
    pthread_create(&manager->migration_thread, NULL, migration_thread, manager);

    // Run test
    sleep(TEST_DURATION);

    // Stop threads
    manager->running = false;
    pthread_join(manager->timer_thread, NULL);
    pthread_join(manager->migration_thread, NULL);

    // Calculate statistics
    calculate_stats(manager);
}

// Calculate Statistics
void calculate_stats(hrtimer_manager_t *manager) {
    if (!manager) return;

    if (manager->stats.expired_timers > 0) {
        manager->stats.avg_precision = 
            (manager->stats.max_precision + manager->stats.min_precision) / 2.0;
    }

    manager->stats.test_duration = TEST_DURATION;
}

// Print Test Statistics
void print_test_stats(hrtimer_manager_t *manager) {
    if (!manager) return;

    printf("\nHRTimer Test Results:\n");
    printf("-------------------------\n");
    printf("Test Duration:     %.2f seconds\n", manager->stats.test_duration);
    printf("Total Timers:      %lu\n", manager->stats.total_timers);
    printf("Active Timers:     %lu\n", manager->stats.active_timers);
    printf("Expired Timers:    %lu\n", manager->stats.expired_timers);
    printf("Timer Migrations:  %lu\n", manager->stats.migrations);
    printf("Timer Overruns:    %lu\n", manager->stats.overruns);
    printf("Avg Precision:     %.2f us\n", manager->stats.avg_precision);
    printf("Max Precision:     %.2f us\n", manager->stats.max_precision);
    printf("Min Precision:     %.2f us\n", manager->stats.min_precision);

    // Print CPU base details
    printf("\nCPU Base Details:\n");
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        cpu_base_t *base = &manager->cpu_bases[i];
        printf("  CPU %zu: %zu timers\n", i, base->nr_timers);
        for (size_t j = 0; j < MAX_BASES; j++) {
            size_t count = 0;
            hrtimer_t *timer = base->timers[j];
            while (timer) {
                count++;
                timer = timer->next;
            }
            if (count > 0) {
                printf("    %s: %zu timers\n",
                    get_base_type_string(j), count);
            }
        }
    }
}

// Destroy HRTimer
void destroy_hrtimer(hrtimer_t *timer) {
    free(timer);
}

// Destroy HRTimer Manager
void destroy_hrtimer_manager(hrtimer_manager_t *manager) {
    if (!manager) return;

    // Clean up timers
    for (size_t i = 0; i < manager->nr_timers; i++) {
        destroy_hrtimer(manager->timers[i]);
    }

    // Clean up CPU bases
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        pthread_mutex_destroy(&manager->cpu_bases[i].lock);
    }

    pthread_mutex_destroy(&manager->manager_lock);
    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed hrtimer manager");
}

// Demonstrate HRTimers
void demonstrate_hrtimers(void) {
    printf("Starting hrtimer demonstration...\n");

    // Create and run hrtimer test
    hrtimer_manager_t *manager = create_hrtimer_manager(8);
    if (manager) {
        run_test(manager);
        print_test_stats(manager);
        destroy_hrtimer_manager(manager);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_hrtimers();

    return 0;
}
