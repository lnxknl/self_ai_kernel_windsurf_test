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

// NOCB RCU Constants
#define MAX_CPUS            16
#define MAX_CALLBACKS       1024
#define MAX_NOCB_WORKERS   4
#define CALLBACK_BATCH_SIZE 32
#define TEST_DURATION      30     // seconds
#define GRACE_PERIOD_US    1000   // 1ms

// Callback States
typedef enum {
    CB_IDLE,
    CB_PENDING,
    CB_PROCESSING,
    CB_DONE
} callback_state_t;

// Thread States
typedef enum {
    THREAD_INIT,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_FINISHED
} thread_state_t;

// Callback Structure
typedef struct callback {
    void (*func)(void *);
    void *arg;
    callback_state_t state;
    uint64_t enqueue_time;
    uint64_t start_time;
    uint64_t end_time;
    struct callback *next;
} callback_t;

// Callback Queue Structure
typedef struct {
    callback_t *head;
    callback_t *tail;
    size_t count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} callback_queue_t;

// CPU Structure
typedef struct {
    unsigned int id;
    callback_queue_t *queue;
    uint64_t callbacks_posted;
    uint64_t callbacks_processed;
    bool nocb_enabled;
} rcu_cpu_t;

// NOCB Worker Structure
typedef struct {
    pthread_t thread;
    unsigned int id;
    thread_state_t state;
    uint64_t processed;
    uint64_t processing_time;
    struct timeval start_time;
    struct timeval end_time;
    callback_queue_t *queue;
} nocb_worker_t;

// Statistics Structure
typedef struct {
    uint64_t total_callbacks;
    uint64_t processed_callbacks;
    uint64_t pending_callbacks;
    uint64_t grace_periods;
    double avg_processing_time;
    double avg_queue_length;
    double test_duration;
    uint64_t nocb_wakeups;
    uint64_t batch_completions;
} nocb_stats_t;

// NOCB RCU Manager
typedef struct {
    rcu_cpu_t cpus[MAX_CPUS];
    nocb_worker_t workers[MAX_NOCB_WORKERS];
    size_t nr_cpus;
    size_t nr_workers;
    bool running;
    pthread_mutex_t manager_lock;
    nocb_stats_t stats;
} nocb_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_thread_state_string(thread_state_t state);
const char* get_callback_state_string(callback_state_t state);

nocb_manager_t* create_nocb_manager(size_t nr_cpus, size_t nr_workers);
void destroy_nocb_manager(nocb_manager_t *manager);

callback_queue_t* create_callback_queue(void);
void destroy_callback_queue(callback_queue_t *queue);
void enqueue_callback(callback_queue_t *queue, callback_t *cb);
callback_t* dequeue_callback(callback_queue_t *queue);
callback_t* dequeue_batch(callback_queue_t *queue, size_t max_count);

void* nocb_worker_thread(void *arg);
void run_test(nocb_manager_t *manager);
void simulate_cpu_activity(nocb_manager_t *manager);

void calculate_stats(nocb_manager_t *manager);
void print_test_stats(nocb_manager_t *manager);
void demonstrate_nocb(void);

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

const char* get_thread_state_string(thread_state_t state) {
    switch(state) {
        case THREAD_INIT:     return "INIT";
        case THREAD_RUNNING:  return "RUNNING";
        case THREAD_BLOCKED:  return "BLOCKED";
        case THREAD_FINISHED: return "FINISHED";
        default: return "UNKNOWN";
    }
}

const char* get_callback_state_string(callback_state_t state) {
    switch(state) {
        case CB_IDLE:       return "IDLE";
        case CB_PENDING:    return "PENDING";
        case CB_PROCESSING: return "PROCESSING";
        case CB_DONE:       return "DONE";
        default: return "UNKNOWN";
    }
}

// Create Callback Queue
callback_queue_t* create_callback_queue(void) {
    callback_queue_t *queue = malloc(sizeof(callback_queue_t));
    if (!queue) return NULL;

    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->cond, NULL);

    return queue;
}

// Create NOCB Manager
nocb_manager_t* create_nocb_manager(size_t nr_cpus, size_t nr_workers) {
    if (nr_cpus > MAX_CPUS || nr_workers > MAX_NOCB_WORKERS) {
        LOG(LOG_LEVEL_ERROR, "Number of CPUs or workers exceeds maximum");
        return NULL;
    }

    nocb_manager_t *manager = malloc(sizeof(nocb_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate NOCB manager");
        return NULL;
    }

    // Initialize CPUs
    for (size_t i = 0; i < nr_cpus; i++) {
        manager->cpus[i].id = i;
        manager->cpus[i].queue = create_callback_queue();
        if (!manager->cpus[i].queue) {
            // Cleanup and return
            for (size_t j = 0; j < i; j++) {
                destroy_callback_queue(manager->cpus[j].queue);
            }
            free(manager);
            return NULL;
        }
        manager->cpus[i].callbacks_posted = 0;
        manager->cpus[i].callbacks_processed = 0;
        manager->cpus[i].nocb_enabled = (i % 2 == 0);  // Enable NOCB for even CPUs
    }

    // Initialize workers
    for (size_t i = 0; i < nr_workers; i++) {
        manager->workers[i].id = i;
        manager->workers[i].state = THREAD_INIT;
        manager->workers[i].processed = 0;
        manager->workers[i].processing_time = 0;
        manager->workers[i].queue = create_callback_queue();
        if (!manager->workers[i].queue) {
            // Cleanup and return
            for (size_t j = 0; j < nr_cpus; j++) {
                destroy_callback_queue(manager->cpus[j].queue);
            }
            for (size_t j = 0; j < i; j++) {
                destroy_callback_queue(manager->workers[j].queue);
            }
            free(manager);
            return NULL;
        }
    }

    manager->nr_cpus = nr_cpus;
    manager->nr_workers = nr_workers;
    manager->running = false;
    pthread_mutex_init(&manager->manager_lock, NULL);
    memset(&manager->stats, 0, sizeof(nocb_stats_t));

    LOG(LOG_LEVEL_DEBUG, "Created NOCB manager with %zu CPUs, %zu workers",
        nr_cpus, nr_workers);
    return manager;
}

// Example callback function
void test_callback(void *arg) {
    int *value = (int*)arg;
    LOG(LOG_LEVEL_DEBUG, "Processing callback with value: %d", *value);
    usleep(rand() % 1000);  // Simulate processing time
}

// NOCB Worker Thread
void* nocb_worker_thread(void *arg) {
    nocb_worker_t *worker = (nocb_worker_t*)arg;
    nocb_manager_t *manager = (nocb_manager_t*)((void**)arg)[1];

    gettimeofday(&worker->start_time, NULL);
    worker->state = THREAD_RUNNING;

    while (manager->running) {
        // Try to get a batch of callbacks
        callback_t *batch = NULL;
        size_t batch_size = 0;

        // Check all CPU queues round-robin
        for (size_t i = 0; i < manager->nr_cpus && batch_size < CALLBACK_BATCH_SIZE; i++) {
            rcu_cpu_t *cpu = &manager->cpus[i];
            if (!cpu->nocb_enabled) continue;

            callback_t *cb = dequeue_callback(cpu->queue);
            if (cb) {
                cb->next = batch;
                batch = cb;
                batch_size++;
            }
        }

        if (batch) {
            // Process the batch
            struct timeval start, end;
            gettimeofday(&start, NULL);

            callback_t *cb = batch;
            while (cb) {
                cb->state = CB_PROCESSING;
                cb->start_time = time(NULL);
                
                // Execute callback
                cb->func(cb->arg);
                
                cb->state = CB_DONE;
                cb->end_time = time(NULL);
                worker->processed++;

                callback_t *next = cb->next;
                free(cb);
                cb = next;
            }

            gettimeofday(&end, NULL);
            worker->processing_time += (end.tv_sec - start.tv_sec) * 1000000 +
                                     (end.tv_usec - start.tv_usec);

            manager->stats.batch_completions++;
        } else {
            // No work available, sleep briefly
            worker->state = THREAD_BLOCKED;
            usleep(100);
            worker->state = THREAD_RUNNING;
        }
    }

    gettimeofday(&worker->end_time, NULL);
    worker->state = THREAD_FINISHED;
    return NULL;
}

// Simulate CPU Activity
void simulate_cpu_activity(nocb_manager_t *manager) {
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        rcu_cpu_t *cpu = &manager->cpus[i];
        
        // Generate some callbacks
        int num_callbacks = rand() % 10 + 1;
        for (int j = 0; j < num_callbacks; j++) {
            callback_t *cb = malloc(sizeof(callback_t));
            if (cb) {
                int *value = malloc(sizeof(int));
                if (value) {
                    *value = rand();
                    cb->func = test_callback;
                    cb->arg = value;
                    cb->state = CB_PENDING;
                    cb->enqueue_time = time(NULL);
                    cb->next = NULL;

                    enqueue_callback(cpu->queue, cb);
                    cpu->callbacks_posted++;
                    manager->stats.total_callbacks++;
                } else {
                    free(cb);
                }
            }
        }
    }
}

// Run Test
void run_test(nocb_manager_t *manager) {
    if (!manager) return;

    LOG(LOG_LEVEL_INFO, "Starting NOCB RCU test...");

    // Start worker threads
    manager->running = true;
    void *thread_args[2];
    thread_args[1] = manager;

    for (size_t i = 0; i < manager->nr_workers; i++) {
        thread_args[0] = &manager->workers[i];
        pthread_create(&manager->workers[i].thread, NULL, nocb_worker_thread, thread_args);
    }

    // Run test
    time_t start_time = time(NULL);
    while (time(NULL) - start_time < TEST_DURATION) {
        simulate_cpu_activity(manager);
        usleep(1000);  // 1ms between activity bursts
    }

    // Stop workers
    manager->running = false;

    // Wait for workers to finish
    for (size_t i = 0; i < manager->nr_workers; i++) {
        pthread_join(manager->workers[i].thread, NULL);
    }

    // Calculate statistics
    calculate_stats(manager);
}

// Calculate Statistics
void calculate_stats(nocb_manager_t *manager) {
    if (!manager) return;

    manager->stats.processed_callbacks = 0;
    manager->stats.pending_callbacks = 0;
    uint64_t total_processing_time = 0;
    uint64_t total_queue_length = 0;

    // Calculate worker statistics
    for (size_t i = 0; i < manager->nr_workers; i++) {
        nocb_worker_t *worker = &manager->workers[i];
        manager->stats.processed_callbacks += worker->processed;
        total_processing_time += worker->processing_time;
    }

    // Calculate CPU statistics
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        rcu_cpu_t *cpu = &manager->cpus[i];
        total_queue_length += cpu->queue->count;
    }

    // Calculate averages
    if (manager->stats.processed_callbacks > 0) {
        manager->stats.avg_processing_time = 
            (double)total_processing_time / manager->stats.processed_callbacks;
    }
    manager->stats.avg_queue_length = 
        (double)total_queue_length / manager->nr_cpus;

    manager->stats.test_duration = TEST_DURATION;
    manager->stats.pending_callbacks = 
        manager->stats.total_callbacks - manager->stats.processed_callbacks;
}

// Print Test Statistics
void print_test_stats(nocb_manager_t *manager) {
    if (!manager) return;

    printf("\nNOCB RCU Test Results:\n");
    printf("-------------------------\n");
    printf("Test Duration:        %.2f seconds\n", manager->stats.test_duration);
    printf("Total Callbacks:      %lu\n", manager->stats.total_callbacks);
    printf("Processed Callbacks:  %lu\n", manager->stats.processed_callbacks);
    printf("Pending Callbacks:    %lu\n", manager->stats.pending_callbacks);
    printf("Avg Processing Time:  %.2f us\n", manager->stats.avg_processing_time);
    printf("Avg Queue Length:     %.2f\n", manager->stats.avg_queue_length);
    printf("Batch Completions:    %lu\n", manager->stats.batch_completions);

    // Print worker details
    printf("\nWorker Threads:\n");
    for (size_t i = 0; i < manager->nr_workers; i++) {
        nocb_worker_t *worker = &manager->workers[i];
        printf("  Worker %zu: %s, %lu callbacks processed\n",
            i, get_thread_state_string(worker->state),
            worker->processed);
    }

    // Print CPU details
    printf("\nCPUs:\n");
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        rcu_cpu_t *cpu = &manager->cpus[i];
        printf("  CPU %zu: %s, Posted: %lu, Queue: %zu\n",
            i, cpu->nocb_enabled ? "NOCB" : "CB",
            cpu->callbacks_posted, cpu->queue->count);
    }
}

// Destroy Callback Queue
void destroy_callback_queue(callback_queue_t *queue) {
    if (!queue) return;

    // Free remaining callbacks
    callback_t *current = queue->head;
    while (current) {
        callback_t *next = current->next;
        free(current->arg);
        free(current);
        current = next;
    }

    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->cond);
    free(queue);
}

// Destroy NOCB Manager
void destroy_nocb_manager(nocb_manager_t *manager) {
    if (!manager) return;

    // Clean up CPU queues
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        destroy_callback_queue(manager->cpus[i].queue);
    }

    // Clean up worker queues
    for (size_t i = 0; i < manager->nr_workers; i++) {
        destroy_callback_queue(manager->workers[i].queue);
    }

    pthread_mutex_destroy(&manager->manager_lock);
    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed NOCB manager");
}

// Queue Operations
void enqueue_callback(callback_queue_t *queue, callback_t *cb) {
    if (!queue || !cb) return;

    pthread_mutex_lock(&queue->lock);
    
    if (!queue->head) {
        queue->head = cb;
        queue->tail = cb;
    } else {
        queue->tail->next = cb;
        queue->tail = cb;
    }
    queue->count++;

    pthread_cond_signal(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
}

callback_t* dequeue_callback(callback_queue_t *queue) {
    if (!queue) return NULL;

    pthread_mutex_lock(&queue->lock);
    
    callback_t *cb = queue->head;
    if (cb) {
        queue->head = cb->next;
        if (!queue->head) {
            queue->tail = NULL;
        }
        cb->next = NULL;
        queue->count--;
    }

    pthread_mutex_unlock(&queue->lock);
    return cb;
}

// Demonstrate NOCB
void demonstrate_nocb(void) {
    printf("Starting NOCB RCU demonstration...\n");

    // Create and run NOCB test
    nocb_manager_t *manager = create_nocb_manager(8, 2);
    if (manager) {
        run_test(manager);
        print_test_stats(manager);
        destroy_nocb_manager(manager);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_nocb();

    return 0;
}
