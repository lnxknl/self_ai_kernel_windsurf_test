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

// RCU Torture Constants
#define MAX_READERS          32
#define MAX_WRITERS          8
#define MAX_DATA_SIZE        4096
#define MAX_CALLBACKS        1024
#define TEST_DURATION        30    // seconds
#define GRACE_PERIOD_US      1000  // 1ms
#define STRESS_INTERVAL_US   100   // 100us
#define MAX_FAULTS          100

// Test Types
typedef enum {
    TEST_NORMAL,
    TEST_STRESS,
    TEST_FAULT_INJECTION,
    TEST_STARVATION,
    TEST_OVERLOAD
} test_type_t;

// Fault Types
typedef enum {
    FAULT_NONE,
    FAULT_DELAY,
    FAULT_MEMORY,
    FAULT_DEADLOCK,
    FAULT_RACE
} fault_type_t;

// Thread States
typedef enum {
    THREAD_INIT,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_CRASHED,
    THREAD_FINISHED
} thread_state_t;

// Data Node Structure
typedef struct node {
    void *data;
    size_t size;
    uint64_t version;
    struct node *next;
} node_t;

// RCU List Structure
typedef struct {
    node_t *head;
    pthread_mutex_t lock;
    uint64_t updates;
} rcu_list_t;

// Reader Thread Structure
typedef struct {
    pthread_t thread;
    unsigned int id;
    thread_state_t state;
    uint64_t reads;
    uint64_t read_errors;
    uint64_t read_time;
    struct timeval start_time;
    struct timeval end_time;
    bool fault_injected;
} reader_thread_t;

// Writer Thread Structure
typedef struct {
    pthread_t thread;
    unsigned int id;
    thread_state_t state;
    uint64_t writes;
    uint64_t write_errors;
    uint64_t write_time;
    struct timeval start_time;
    struct timeval end_time;
    bool fault_injected;
} writer_thread_t;

// Fault Injection Structure
typedef struct {
    fault_type_t type;
    unsigned int target_thread;
    bool is_reader;
    uint64_t trigger_time;
    uint64_t duration;
} fault_injection_t;

// Test Statistics
typedef struct {
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t read_errors;
    uint64_t write_errors;
    uint64_t grace_periods;
    uint64_t deadlocks;
    uint64_t races;
    double avg_read_latency;
    double avg_write_latency;
    double test_duration;
    uint64_t faults_injected;
    uint64_t faults_detected;
} test_stats_t;

// RCU Torture Manager
typedef struct {
    rcu_list_t *list;
    reader_thread_t readers[MAX_READERS];
    writer_thread_t writers[MAX_WRITERS];
    size_t nr_readers;
    size_t nr_writers;
    test_type_t test_type;
    fault_injection_t faults[MAX_FAULTS];
    size_t nr_faults;
    bool running;
    pthread_mutex_t manager_lock;
    test_stats_t stats;
} rcutorture_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_test_type_string(test_type_t type);
const char* get_fault_type_string(fault_type_t type);
const char* get_thread_state_string(thread_state_t state);

rcutorture_t* create_rcutorture(size_t nr_readers, size_t nr_writers, test_type_t type);
void destroy_rcutorture(rcutorture_t *torture);

void* reader_thread(void *arg);
void* writer_thread(void *arg);
void inject_fault(rcutorture_t *torture, fault_injection_t *fault);
void run_test(rcutorture_t *torture);

void calculate_stats(rcutorture_t *torture);
void print_test_stats(rcutorture_t *torture);
void demonstrate_rcutorture(void);

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

const char* get_test_type_string(test_type_t type) {
    switch(type) {
        case TEST_NORMAL:         return "NORMAL";
        case TEST_STRESS:         return "STRESS";
        case TEST_FAULT_INJECTION: return "FAULT_INJECTION";
        case TEST_STARVATION:     return "STARVATION";
        case TEST_OVERLOAD:       return "OVERLOAD";
        default: return "UNKNOWN";
    }
}

const char* get_fault_type_string(fault_type_t type) {
    switch(type) {
        case FAULT_NONE:     return "NONE";
        case FAULT_DELAY:    return "DELAY";
        case FAULT_MEMORY:   return "MEMORY";
        case FAULT_DEADLOCK: return "DEADLOCK";
        case FAULT_RACE:     return "RACE";
        default: return "UNKNOWN";
    }
}

const char* get_thread_state_string(thread_state_t state) {
    switch(state) {
        case THREAD_INIT:     return "INIT";
        case THREAD_RUNNING:  return "RUNNING";
        case THREAD_BLOCKED:  return "BLOCKED";
        case THREAD_CRASHED:  return "CRASHED";
        case THREAD_FINISHED: return "FINISHED";
        default: return "UNKNOWN";
    }
}

// Create RCU List
rcu_list_t* create_rcu_list(void) {
    rcu_list_t *list = malloc(sizeof(rcu_list_t));
    if (!list) return NULL;

    list->head = NULL;
    pthread_mutex_init(&list->lock, NULL);
    list->updates = 0;
    return list;
}

// Create RCU Torture Manager
rcutorture_t* create_rcutorture(size_t nr_readers, size_t nr_writers, test_type_t type) {
    if (nr_readers > MAX_READERS || nr_writers > MAX_WRITERS) {
        LOG(LOG_LEVEL_ERROR, "Number of threads exceeds maximum");
        return NULL;
    }

    rcutorture_t *torture = malloc(sizeof(rcutorture_t));
    if (!torture) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate RCU torture manager");
        return NULL;
    }

    torture->list = create_rcu_list();
    if (!torture->list) {
        free(torture);
        return NULL;
    }

    // Initialize readers
    for (size_t i = 0; i < nr_readers; i++) {
        torture->readers[i].id = i;
        torture->readers[i].state = THREAD_INIT;
        torture->readers[i].reads = 0;
        torture->readers[i].read_errors = 0;
        torture->readers[i].read_time = 0;
        torture->readers[i].fault_injected = false;
    }

    // Initialize writers
    for (size_t i = 0; i < nr_writers; i++) {
        torture->writers[i].id = i;
        torture->writers[i].state = THREAD_INIT;
        torture->writers[i].writes = 0;
        torture->writers[i].write_errors = 0;
        torture->writers[i].write_time = 0;
        torture->writers[i].fault_injected = false;
    }

    torture->nr_readers = nr_readers;
    torture->nr_writers = nr_writers;
    torture->test_type = type;
    torture->nr_faults = 0;
    torture->running = false;
    pthread_mutex_init(&torture->manager_lock, NULL);
    memset(&torture->stats, 0, sizeof(test_stats_t));

    LOG(LOG_LEVEL_DEBUG, "Created RCU torture manager with %zu readers, %zu writers",
        nr_readers, nr_writers);
    return torture;
}

// Reader Thread
void* reader_thread(void *arg) {
    reader_thread_t *reader = (reader_thread_t*)arg;
    rcutorture_t *torture = (rcutorture_t*)((void**)arg)[1];

    gettimeofday(&reader->start_time, NULL);
    reader->state = THREAD_RUNNING;

    while (torture->running) {
        struct timeval start, end;
        gettimeofday(&start, NULL);

        // Read operation
        pthread_mutex_lock(&torture->list->lock);
        node_t *current = torture->list->head;
        while (current) {
            if (reader->fault_injected) {
                // Simulate fault condition
                usleep(rand() % 1000);  // Random delay
            }
            current = current->next;
            reader->reads++;
        }
        pthread_mutex_unlock(&torture->list->lock);

        gettimeofday(&end, NULL);
        reader->read_time += (end.tv_sec - start.tv_sec) * 1000000 +
                            (end.tv_usec - start.tv_usec);

        // Add stress based on test type
        switch (torture->test_type) {
            case TEST_STRESS:
                usleep(10);  // Minimal delay
                break;
            case TEST_STARVATION:
                usleep(1000);  // Long delay
                break;
            case TEST_OVERLOAD:
                // No delay, continuous reads
                break;
            default:
                usleep(100);  // Normal delay
                break;
        }
    }

    gettimeofday(&reader->end_time, NULL);
    reader->state = THREAD_FINISHED;
    return NULL;
}

// Writer Thread
void* writer_thread(void *arg) {
    writer_thread_t *writer = (writer_thread_t*)arg;
    rcutorture_t *torture = (rcutorture_t*)((void**)arg)[1];

    gettimeofday(&writer->start_time, NULL);
    writer->state = THREAD_RUNNING;

    while (torture->running) {
        struct timeval start, end;
        gettimeofday(&start, NULL);

        // Write operation
        pthread_mutex_lock(&torture->list->lock);
        
        // Create new node
        node_t *new_node = malloc(sizeof(node_t));
        if (new_node) {
            new_node->data = malloc(MAX_DATA_SIZE);
            if (new_node->data) {
                new_node->size = MAX_DATA_SIZE;
                new_node->version = torture->list->updates++;
                new_node->next = torture->list->head;
                torture->list->head = new_node;
                writer->writes++;
            } else {
                free(new_node);
                writer->write_errors++;
            }
        } else {
            writer->write_errors++;
        }

        if (writer->fault_injected) {
            // Simulate fault condition
            usleep(rand() % 2000);  // Random longer delay
        }

        pthread_mutex_unlock(&torture->list->lock);

        gettimeofday(&end, NULL);
        writer->write_time += (end.tv_sec - start.tv_sec) * 1000000 +
                             (end.tv_usec - start.tv_usec);

        // Add stress based on test type
        switch (torture->test_type) {
            case TEST_STRESS:
                usleep(50);  // Quick writes
                break;
            case TEST_STARVATION:
                usleep(100);  // Normal writes during starvation
                break;
            case TEST_OVERLOAD:
                usleep(10);  // Very quick writes
                break;
            default:
                usleep(500);  // Normal delay
                break;
        }
    }

    gettimeofday(&writer->end_time, NULL);
    writer->state = THREAD_FINISHED;
    return NULL;
}

// Inject Fault
void inject_fault(rcutorture_t *torture, fault_injection_t *fault) {
    if (!torture || !fault) return;

    LOG(LOG_LEVEL_INFO, "Injecting %s fault into %s thread %u",
        get_fault_type_string(fault->type),
        fault->is_reader ? "reader" : "writer",
        fault->target_thread);

    if (fault->is_reader) {
        if (fault->target_thread < torture->nr_readers) {
            torture->readers[fault->target_thread].fault_injected = true;
        }
    } else {
        if (fault->target_thread < torture->nr_writers) {
            torture->writers[fault->target_thread].fault_injected = true;
        }
    }

    torture->stats.faults_injected++;
}

// Run Test
void run_test(rcutorture_t *torture) {
    if (!torture) return;

    LOG(LOG_LEVEL_INFO, "Starting %s test...",
        get_test_type_string(torture->test_type));

    // Start threads
    torture->running = true;
    void *thread_args[2];
    thread_args[1] = torture;

    for (size_t i = 0; i < torture->nr_readers; i++) {
        thread_args[0] = &torture->readers[i];
        pthread_create(&torture->readers[i].thread, NULL, reader_thread, thread_args);
    }

    for (size_t i = 0; i < torture->nr_writers; i++) {
        thread_args[0] = &torture->writers[i];
        pthread_create(&torture->writers[i].thread, NULL, writer_thread, thread_args);
    }

    // Inject faults during test
    if (torture->test_type == TEST_FAULT_INJECTION) {
        for (size_t i = 0; i < torture->nr_faults; i++) {
            usleep(torture->faults[i].trigger_time);
            inject_fault(torture, &torture->faults[i]);
        }
    }

    // Run test
    sleep(TEST_DURATION);

    // Stop threads
    torture->running = false;

    // Wait for threads to finish
    for (size_t i = 0; i < torture->nr_readers; i++) {
        pthread_join(torture->readers[i].thread, NULL);
    }
    for (size_t i = 0; i < torture->nr_writers; i++) {
        pthread_join(torture->writers[i].thread, NULL);
    }

    // Calculate statistics
    calculate_stats(torture);
}

// Calculate Statistics
void calculate_stats(rcutorture_t *torture) {
    if (!torture) return;

    torture->stats.total_reads = 0;
    torture->stats.total_writes = 0;
    torture->stats.read_errors = 0;
    torture->stats.write_errors = 0;
    uint64_t total_read_time = 0;
    uint64_t total_write_time = 0;

    // Calculate reader statistics
    for (size_t i = 0; i < torture->nr_readers; i++) {
        reader_thread_t *reader = &torture->readers[i];
        torture->stats.total_reads += reader->reads;
        torture->stats.read_errors += reader->read_errors;
        total_read_time += reader->read_time;
    }

    // Calculate writer statistics
    for (size_t i = 0; i < torture->nr_writers; i++) {
        writer_thread_t *writer = &torture->writers[i];
        torture->stats.total_writes += writer->writes;
        torture->stats.write_errors += writer->write_errors;
        total_write_time += writer->write_time;
    }

    // Calculate averages
    if (torture->stats.total_reads > 0) {
        torture->stats.avg_read_latency = 
            (double)total_read_time / torture->stats.total_reads;
    }
    if (torture->stats.total_writes > 0) {
        torture->stats.avg_write_latency = 
            (double)total_write_time / torture->stats.total_writes;
    }

    torture->stats.test_duration = TEST_DURATION;
    torture->stats.grace_periods = torture->list->updates;
}

// Print Test Statistics
void print_test_stats(rcutorture_t *torture) {
    if (!torture) return;

    printf("\nRCU Torture Test Results (%s):\n",
        get_test_type_string(torture->test_type));
    printf("-------------------------\n");
    printf("Test Duration:       %.2f seconds\n", torture->stats.test_duration);
    printf("Total Reads:         %lu\n", torture->stats.total_reads);
    printf("Total Writes:        %lu\n", torture->stats.total_writes);
    printf("Read Errors:         %lu\n", torture->stats.read_errors);
    printf("Write Errors:        %lu\n", torture->stats.write_errors);
    printf("Avg Read Latency:    %.2f us\n", torture->stats.avg_read_latency);
    printf("Avg Write Latency:   %.2f us\n", torture->stats.avg_write_latency);
    printf("Grace Periods:       %lu\n", torture->stats.grace_periods);
    printf("Faults Injected:     %lu\n", torture->stats.faults_injected);
    printf("Faults Detected:     %lu\n", torture->stats.faults_detected);

    // Print thread details
    printf("\nReader Threads:\n");
    for (size_t i = 0; i < torture->nr_readers; i++) {
        reader_thread_t *reader = &torture->readers[i];
        printf("  Reader %zu: %s, %lu reads, %lu errors\n",
            i, get_thread_state_string(reader->state),
            reader->reads, reader->read_errors);
    }

    printf("\nWriter Threads:\n");
    for (size_t i = 0; i < torture->nr_writers; i++) {
        writer_thread_t *writer = &torture->writers[i];
        printf("  Writer %zu: %s, %lu writes, %lu errors\n",
            i, get_thread_state_string(writer->state),
            writer->writes, writer->write_errors);
    }
}

// Destroy RCU Torture Manager
void destroy_rcutorture(rcutorture_t *torture) {
    if (!torture) return;

    // Clean up RCU list
    if (torture->list) {
        node_t *current = torture->list->head;
        while (current) {
            node_t *next = current->next;
            free(current->data);
            free(current);
            current = next;
        }
        pthread_mutex_destroy(&torture->list->lock);
        free(torture->list);
    }

    pthread_mutex_destroy(&torture->manager_lock);
    free(torture);
    LOG(LOG_LEVEL_DEBUG, "Destroyed RCU torture manager");
}

// Demonstrate RCU Torture
void demonstrate_rcutorture(void) {
    printf("Starting RCU torture demonstration...\n");

    // Test 1: Normal operation
    rcutorture_t *torture = create_rcutorture(4, 2, TEST_NORMAL);
    if (torture) {
        run_test(torture);
        print_test_stats(torture);
        destroy_rcutorture(torture);
    }

    // Test 2: Stress test
    torture = create_rcutorture(8, 4, TEST_STRESS);
    if (torture) {
        run_test(torture);
        print_test_stats(torture);
        destroy_rcutorture(torture);
    }

    // Test 3: Fault injection
    torture = create_rcutorture(4, 2, TEST_FAULT_INJECTION);
    if (torture) {
        // Add some faults
        fault_injection_t fault = {
            .type = FAULT_DELAY,
            .is_reader = true,
            .target_thread = 0,
            .trigger_time = 1000000,  // 1s
            .duration = 500000        // 500ms
        };
        torture->faults[torture->nr_faults++] = fault;

        fault.type = FAULT_MEMORY;
        fault.is_reader = false;
        fault.target_thread = 1;
        fault.trigger_time = 2000000;  // 2s
        torture->faults[torture->nr_faults++] = fault;

        run_test(torture);
        print_test_stats(torture);
        destroy_rcutorture(torture);
    }

    // Test 4: Starvation test
    torture = create_rcutorture(6, 2, TEST_STARVATION);
    if (torture) {
        run_test(torture);
        print_test_stats(torture);
        destroy_rcutorture(torture);
    }

    // Test 5: Overload test
    torture = create_rcutorture(MAX_READERS, MAX_WRITERS, TEST_OVERLOAD);
    if (torture) {
        run_test(torture);
        print_test_stats(torture);
        destroy_rcutorture(torture);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_rcutorture();

    return 0;
}
