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

// Wait Queue Constants
#define MAX_WAIT_QUEUES   64
#define MAX_TASKS         256
#define MAX_EVENTS        1024
#define MAX_WAIT_TIME     10000000  // 10s in microseconds
#define MIN_WAIT_TIME     1000      // 1ms in microseconds
#define TEST_DURATION     30        // seconds

// Task States
typedef enum {
    TASK_RUNNING,
    TASK_WAITING,
    TASK_READY,
    TASK_DEAD
} task_state_t;

// Wait Conditions
typedef enum {
    WAIT_NONE,
    WAIT_TIMEOUT,
    WAIT_EVENT,
    WAIT_ALL,
    WAIT_ANY
} wait_condition_t;

// Event Types
typedef enum {
    EVENT_NONE,
    EVENT_SIGNAL,
    EVENT_BROADCAST,
    EVENT_TIMEOUT,
    EVENT_ERROR
} event_type_t;

// Task Structure
typedef struct task {
    unsigned int id;
    task_state_t state;
    wait_condition_t wait_condition;
    uint64_t wait_start;
    uint64_t wait_timeout;
    size_t nr_events;
    unsigned int events[MAX_EVENTS];
    struct task *next;
} task_t;

// Wait Queue Structure
typedef struct wait_queue {
    unsigned int id;
    task_t *waiting_tasks;
    size_t nr_tasks;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} wait_queue_t;

// Statistics Structure
typedef struct {
    uint64_t total_tasks;
    uint64_t total_waits;
    uint64_t total_wakeups;
    uint64_t total_timeouts;
    uint64_t total_signals;
    uint64_t total_broadcasts;
    double avg_wait_time;
    double max_wait_time;
    double test_duration;
} wait_stats_t;

// Wait Queue Manager Structure
typedef struct {
    wait_queue_t queues[MAX_WAIT_QUEUES];
    task_t *tasks[MAX_TASKS];
    size_t nr_queues;
    size_t nr_tasks;
    bool running;
    pthread_mutex_t manager_lock;
    pthread_t timeout_thread;
    pthread_t event_thread;
    wait_stats_t stats;
} wait_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_task_state_string(task_state_t state);
const char* get_wait_condition_string(wait_condition_t condition);
const char* get_event_type_string(event_type_t type);

wait_manager_t* create_wait_manager(void);
void destroy_wait_manager(wait_manager_t *manager);

task_t* create_task(void);
void destroy_task(task_t *task);

void* timeout_thread(void *arg);
void* event_thread(void *arg);
void check_timeouts(wait_manager_t *manager);
void generate_events(wait_manager_t *manager);

bool wait_event(wait_manager_t *manager, task_t *task, 
               unsigned int event_id, uint64_t timeout);
bool wait_all_events(wait_manager_t *manager, task_t *task, 
                    unsigned int *events, size_t nr_events, uint64_t timeout);
bool wait_any_event(wait_manager_t *manager, task_t *task, 
                   unsigned int *events, size_t nr_events, uint64_t timeout);

void signal_event(wait_manager_t *manager, unsigned int event_id);
void broadcast_event(wait_manager_t *manager, unsigned int event_id);
void wake_up_task(wait_manager_t *manager, task_t *task, event_type_t event_type);

void run_test(wait_manager_t *manager);
void calculate_stats(wait_manager_t *manager);
void print_test_stats(wait_manager_t *manager);
void demonstrate_wait_queue(void);

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

const char* get_task_state_string(task_state_t state) {
    switch(state) {
        case TASK_RUNNING: return "RUNNING";
        case TASK_WAITING: return "WAITING";
        case TASK_READY:   return "READY";
        case TASK_DEAD:    return "DEAD";
        default: return "UNKNOWN";
    }
}

const char* get_wait_condition_string(wait_condition_t condition) {
    switch(condition) {
        case WAIT_NONE:    return "NONE";
        case WAIT_TIMEOUT: return "TIMEOUT";
        case WAIT_EVENT:   return "EVENT";
        case WAIT_ALL:     return "ALL";
        case WAIT_ANY:     return "ANY";
        default: return "UNKNOWN";
    }
}

const char* get_event_type_string(event_type_t type) {
    switch(type) {
        case EVENT_NONE:      return "NONE";
        case EVENT_SIGNAL:    return "SIGNAL";
        case EVENT_BROADCAST: return "BROADCAST";
        case EVENT_TIMEOUT:   return "TIMEOUT";
        case EVENT_ERROR:     return "ERROR";
        default: return "UNKNOWN";
    }
}

// Create Task
task_t* create_task(void) {
    task_t *task = malloc(sizeof(task_t));
    if (!task) return NULL;

    static unsigned int next_id = 0;
    task->id = next_id++;
    task->state = TASK_READY;
    task->wait_condition = WAIT_NONE;
    task->wait_start = 0;
    task->wait_timeout = 0;
    task->nr_events = 0;
    task->next = NULL;

    return task;
}

// Create Wait Queue
wait_queue_t* create_wait_queue(unsigned int id) {
    wait_queue_t *queue = &((wait_manager_t*)NULL)->queues[id];
    queue->id = id;
    queue->waiting_tasks = NULL;
    queue->nr_tasks = 0;
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->cond, NULL);
    return queue;
}

// Create Wait Manager
wait_manager_t* create_wait_manager(void) {
    wait_manager_t *manager = malloc(sizeof(wait_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate wait manager");
        return NULL;
    }

    // Initialize wait queues
    for (size_t i = 0; i < MAX_WAIT_QUEUES; i++) {
        create_wait_queue(i);
    }

    manager->nr_queues = MAX_WAIT_QUEUES;
    manager->nr_tasks = 0;
    manager->running = false;
    pthread_mutex_init(&manager->manager_lock, NULL);
    memset(&manager->stats, 0, sizeof(wait_stats_t));

    LOG(LOG_LEVEL_DEBUG, "Created wait manager with %zu queues", MAX_WAIT_QUEUES);
    return manager;
}

// Wait Event
bool wait_event(wait_manager_t *manager, task_t *task, 
               unsigned int event_id, uint64_t timeout) {
    if (!manager || !task || event_id >= MAX_EVENTS) return false;

    wait_queue_t *queue = &manager->queues[event_id];
    pthread_mutex_lock(&queue->lock);

    // Set up task wait state
    task->state = TASK_WAITING;
    task->wait_condition = WAIT_EVENT;
    task->wait_start = time(NULL);
    task->wait_timeout = timeout;
    task->events[0] = event_id;
    task->nr_events = 1;

    // Add task to wait queue
    task->next = queue->waiting_tasks;
    queue->waiting_tasks = task;
    queue->nr_tasks++;

    manager->stats.total_waits++;

    pthread_mutex_unlock(&queue->lock);
    return true;
}

// Wait All Events
bool wait_all_events(wait_manager_t *manager, task_t *task, 
                    unsigned int *events, size_t nr_events, uint64_t timeout) {
    if (!manager || !task || !events || nr_events == 0 || 
        nr_events > MAX_EVENTS) return false;

    // Set up task wait state
    task->state = TASK_WAITING;
    task->wait_condition = WAIT_ALL;
    task->wait_start = time(NULL);
    task->wait_timeout = timeout;
    memcpy(task->events, events, nr_events * sizeof(unsigned int));
    task->nr_events = nr_events;

    // Add task to all wait queues
    for (size_t i = 0; i < nr_events; i++) {
        wait_queue_t *queue = &manager->queues[events[i]];
        pthread_mutex_lock(&queue->lock);
        task->next = queue->waiting_tasks;
        queue->waiting_tasks = task;
        queue->nr_tasks++;
        pthread_mutex_unlock(&queue->lock);
    }

    manager->stats.total_waits++;
    return true;
}

// Signal Event
void signal_event(wait_manager_t *manager, unsigned int event_id) {
    if (!manager || event_id >= MAX_EVENTS) return;

    wait_queue_t *queue = &manager->queues[event_id];
    pthread_mutex_lock(&queue->lock);

    task_t *task = queue->waiting_tasks;
    task_t *prev = NULL;

    while (task) {
        task_t *next = task->next;

        if (task->wait_condition == WAIT_EVENT ||
            (task->wait_condition == WAIT_ANY && 
             task->events[0] == event_id)) {
            // Remove task from queue
            if (prev) {
                prev->next = next;
            } else {
                queue->waiting_tasks = next;
            }
            queue->nr_tasks--;

            // Wake up task
            wake_up_task(manager, task, EVENT_SIGNAL);
        } else {
            prev = task;
        }

        task = next;
    }

    manager->stats.total_signals++;
    pthread_mutex_unlock(&queue->lock);
}

// Broadcast Event
void broadcast_event(wait_manager_t *manager, unsigned int event_id) {
    if (!manager || event_id >= MAX_EVENTS) return;

    wait_queue_t *queue = &manager->queues[event_id];
    pthread_mutex_lock(&queue->lock);

    // Wake up all waiting tasks
    task_t *task = queue->waiting_tasks;
    while (task) {
        task_t *next = task->next;
        wake_up_task(manager, task, EVENT_BROADCAST);
        task = next;
    }

    // Clear queue
    queue->waiting_tasks = NULL;
    queue->nr_tasks = 0;

    manager->stats.total_broadcasts++;
    pthread_mutex_unlock(&queue->lock);
}

// Wake Up Task
void wake_up_task(wait_manager_t *manager, task_t *task, event_type_t event_type) {
    if (!manager || !task) return;

    task->state = TASK_READY;
    task->wait_condition = WAIT_NONE;
    
    // Calculate wait time for statistics
    uint64_t wait_time = time(NULL) - task->wait_start;
    if (wait_time > manager->stats.max_wait_time) {
        manager->stats.max_wait_time = wait_time;
    }

    manager->stats.total_wakeups++;
    
    if (event_type == EVENT_TIMEOUT) {
        manager->stats.total_timeouts++;
    }

    LOG(LOG_LEVEL_DEBUG, "Task %u woken up by %s after %lu us",
        task->id, get_event_type_string(event_type), wait_time);
}

// Timeout Thread
void* timeout_thread(void *arg) {
    wait_manager_t *manager = (wait_manager_t*)arg;

    while (manager->running) {
        check_timeouts(manager);
        usleep(1000);  // Check every 1ms
    }

    return NULL;
}

// Check Timeouts
void check_timeouts(wait_manager_t *manager) {
    if (!manager) return;

    uint64_t current_time = time(NULL);

    for (size_t i = 0; i < manager->nr_queues; i++) {
        wait_queue_t *queue = &manager->queues[i];
        pthread_mutex_lock(&queue->lock);

        task_t *task = queue->waiting_tasks;
        task_t *prev = NULL;

        while (task) {
            task_t *next = task->next;

            if (task->wait_timeout > 0 && 
                current_time - task->wait_start >= task->wait_timeout) {
                // Remove task from queue
                if (prev) {
                    prev->next = next;
                } else {
                    queue->waiting_tasks = next;
                }
                queue->nr_tasks--;

                // Wake up task
                wake_up_task(manager, task, EVENT_TIMEOUT);
            } else {
                prev = task;
            }

            task = next;
        }

        pthread_mutex_unlock(&queue->lock);
    }
}

// Event Thread
void* event_thread(void *arg) {
    wait_manager_t *manager = (wait_manager_t*)arg;

    while (manager->running) {
        generate_events(manager);
        usleep(100000);  // Generate events every 100ms
    }

    return NULL;
}

// Generate Events
void generate_events(wait_manager_t *manager) {
    if (!manager) return;

    // Randomly generate events
    for (size_t i = 0; i < manager->nr_queues; i++) {
        if (rand() % 100 < 5) {  // 5% chance of event
            if (rand() % 10 < 2) {  // 20% chance of broadcast
                broadcast_event(manager, i);
            } else {
                signal_event(manager, i);
            }
        }
    }
}

// Run Test
void run_test(wait_manager_t *manager) {
    if (!manager) return;

    LOG(LOG_LEVEL_INFO, "Starting wait queue test...");

    // Create initial tasks
    for (size_t i = 0; i < MAX_TASKS/2; i++) {
        task_t *task = create_task();
        if (task) {
            manager->tasks[manager->nr_tasks++] = task;
            manager->stats.total_tasks++;

            // Randomly make task wait
            if (rand() % 2) {
                unsigned int event_id = rand() % MAX_EVENTS;
                uint64_t timeout = rand() % MAX_WAIT_TIME + MIN_WAIT_TIME;
                wait_event(manager, task, event_id, timeout);
            }
        }
    }

    // Start threads
    manager->running = true;
    pthread_create(&manager->timeout_thread, NULL, timeout_thread, manager);
    pthread_create(&manager->event_thread, NULL, event_thread, manager);

    // Run test
    sleep(TEST_DURATION);

    // Stop threads
    manager->running = false;
    pthread_join(manager->timeout_thread, NULL);
    pthread_join(manager->event_thread, NULL);

    // Calculate statistics
    calculate_stats(manager);
}

// Calculate Statistics
void calculate_stats(wait_manager_t *manager) {
    if (!manager) return;

    if (manager->stats.total_waits > 0) {
        manager->stats.avg_wait_time = 
            (double)manager->stats.total_wakeups / manager->stats.total_waits;
    }

    manager->stats.test_duration = TEST_DURATION;
}

// Print Test Statistics
void print_test_stats(wait_manager_t *manager) {
    if (!manager) return;

    printf("\nWait Queue Test Results:\n");
    printf("-------------------------\n");
    printf("Test Duration:     %.2f seconds\n", manager->stats.test_duration);
    printf("Total Tasks:       %lu\n", manager->stats.total_tasks);
    printf("Total Waits:       %lu\n", manager->stats.total_waits);
    printf("Total Wakeups:     %lu\n", manager->stats.total_wakeups);
    printf("Total Timeouts:    %lu\n", manager->stats.total_timeouts);
    printf("Total Signals:     %lu\n", manager->stats.total_signals);
    printf("Total Broadcasts:  %lu\n", manager->stats.total_broadcasts);
    printf("Avg Wait Time:     %.2f us\n", manager->stats.avg_wait_time);
    printf("Max Wait Time:     %.2f us\n", manager->stats.max_wait_time);

    // Print queue details
    printf("\nQueue Details:\n");
    for (size_t i = 0; i < manager->nr_queues; i++) {
        wait_queue_t *queue = &manager->queues[i];
        if (queue->nr_tasks > 0) {
            printf("  Queue %u: %zu tasks waiting\n", queue->id, queue->nr_tasks);
        }
    }
}

// Destroy Task
void destroy_task(task_t *task) {
    free(task);
}

// Destroy Wait Queue
void destroy_wait_queue(wait_queue_t *queue) {
    if (!queue) return;

    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->cond);
}

// Destroy Wait Manager
void destroy_wait_manager(wait_manager_t *manager) {
    if (!manager) return;

    // Clean up tasks
    for (size_t i = 0; i < manager->nr_tasks; i++) {
        destroy_task(manager->tasks[i]);
    }

    // Clean up queues
    for (size_t i = 0; i < manager->nr_queues; i++) {
        destroy_wait_queue(&manager->queues[i]);
    }

    pthread_mutex_destroy(&manager->manager_lock);
    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed wait manager");
}

// Demonstrate Wait Queue
void demonstrate_wait_queue(void) {
    printf("Starting wait queue demonstration...\n");

    // Create and run wait queue test
    wait_manager_t *manager = create_wait_manager();
    if (manager) {
        run_test(manager);
        print_test_stats(manager);
        destroy_wait_manager(manager);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_wait_queue();

    return 0;
}
