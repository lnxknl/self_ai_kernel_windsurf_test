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

// RCU Scale Constants
#define MAX_READERS      32
#define MAX_WRITERS      8
#define MAX_DATA_SIZE    1024
#define TEST_DURATION    10  // seconds
#define WARMUP_DURATION  2   // seconds
#define COOLDOWN_TIME    1   // seconds

// Test Types
typedef enum {
    TEST_READ_MOSTLY,
    TEST_WRITE_MOSTLY,
    TEST_MIXED_LOAD,
    TEST_STRESS_TEST
} test_type_t;

// Thread States
typedef enum {
    THREAD_IDLE,
    THREAD_RUNNING,
    THREAD_FINISHED
} thread_state_t;

// Data Structure
typedef struct {
    void *data;
    size_t size;
    uint64_t version;
} rcu_data_t;

// Reader Thread Structure
typedef struct {
    pthread_t thread;
    unsigned int id;
    thread_state_t state;
    uint64_t reads;
    uint64_t read_time;
    struct timeval start_time;
    struct timeval end_time;
} reader_thread_t;

// Writer Thread Structure
typedef struct {
    pthread_t thread;
    unsigned int id;
    thread_state_t state;
    uint64_t writes;
    uint64_t write_time;
    struct timeval start_time;
    struct timeval end_time;
} writer_thread_t;

// Test Statistics
typedef struct {
    uint64_t total_reads;
    uint64_t total_writes;
    double avg_read_latency;
    double avg_write_latency;
    double read_throughput;
    double write_throughput;
    double test_duration;
    uint64_t grace_periods;
} test_stats_t;

// RCU Scale Manager
typedef struct {
    rcu_data_t *data;
    reader_thread_t readers[MAX_READERS];
    writer_thread_t writers[MAX_WRITERS];
    size_t nr_readers;
    size_t nr_writers;
    test_type_t test_type;
    bool running;
    pthread_mutex_t manager_lock;
    pthread_rwlock_t data_lock;
    test_stats_t stats;
} rcuscale_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_test_type_string(test_type_t type);
const char* get_thread_state_string(thread_state_t state);

rcuscale_t* create_rcuscale(size_t nr_readers, size_t nr_writers, test_type_t type);
void destroy_rcuscale(rcuscale_t *scale);

void* reader_thread(void *arg);
void* writer_thread(void *arg);
void run_test(rcuscale_t *scale);

void calculate_stats(rcuscale_t *scale);
void print_test_stats(rcuscale_t *scale);
void demonstrate_rcuscale(void);

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

// Utility Function: Get Test Type String
const char* get_test_type_string(test_type_t type) {
    switch(type) {
        case TEST_READ_MOSTLY:  return "READ_MOSTLY";
        case TEST_WRITE_MOSTLY: return "WRITE_MOSTLY";
        case TEST_MIXED_LOAD:   return "MIXED_LOAD";
        case TEST_STRESS_TEST:  return "STRESS_TEST";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Thread State String
const char* get_thread_state_string(thread_state_t state) {
    switch(state) {
        case THREAD_IDLE:     return "IDLE";
        case THREAD_RUNNING:  return "RUNNING";
        case THREAD_FINISHED: return "FINISHED";
        default: return "UNKNOWN";
    }
}

// Create RCU Scale Manager
rcuscale_t* create_rcuscale(size_t nr_readers, size_t nr_writers, test_type_t type) {
    if (nr_readers > MAX_READERS || nr_writers > MAX_WRITERS) {
        LOG(LOG_LEVEL_ERROR, "Number of threads exceeds maximum");
        return NULL;
    }

    rcuscale_t *scale = malloc(sizeof(rcuscale_t));
    if (!scale) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate RCU scale manager");
        return NULL;
    }

    // Initialize data
    scale->data = malloc(sizeof(rcu_data_t));
    if (!scale->data) {
        free(scale);
        return NULL;
    }
    scale->data->data = malloc(MAX_DATA_SIZE);
    if (!scale->data->data) {
        free(scale->data);
        free(scale);
        return NULL;
    }
    scale->data->size = MAX_DATA_SIZE;
    scale->data->version = 0;

    // Initialize readers
    for (size_t i = 0; i < nr_readers; i++) {
        scale->readers[i].id = i;
        scale->readers[i].state = THREAD_IDLE;
        scale->readers[i].reads = 0;
        scale->readers[i].read_time = 0;
    }

    // Initialize writers
    for (size_t i = 0; i < nr_writers; i++) {
        scale->writers[i].id = i;
        scale->writers[i].state = THREAD_IDLE;
        scale->writers[i].writes = 0;
        scale->writers[i].write_time = 0;
    }

    scale->nr_readers = nr_readers;
    scale->nr_writers = nr_writers;
    scale->test_type = type;
    scale->running = false;
    pthread_mutex_init(&scale->manager_lock, NULL);
    pthread_rwlock_init(&scale->data_lock, NULL);
    memset(&scale->stats, 0, sizeof(test_stats_t));

    LOG(LOG_LEVEL_DEBUG, "Created RCU scale manager with %zu readers, %zu writers",
        nr_readers, nr_writers);
    return scale;
}

// Reader Thread
void* reader_thread(void *arg) {
    reader_thread_t *reader = (reader_thread_t*)arg;
    rcuscale_t *scale = (rcuscale_t*)((void**)arg)[1];

    gettimeofday(&reader->start_time, NULL);
    reader->state = THREAD_RUNNING;

    while (scale->running) {
        struct timeval start, end;
        gettimeofday(&start, NULL);

        // Read operation
        pthread_rwlock_rdlock(&scale->data_lock);
        // Simulate read operation
        usleep(10);  // 10us read time
        reader->reads++;
        pthread_rwlock_unlock(&scale->data_lock);

        gettimeofday(&end, NULL);
        reader->read_time += (end.tv_sec - start.tv_sec) * 1000000 +
                            (end.tv_usec - start.tv_usec);

        // Add delay based on test type
        switch (scale->test_type) {
            case TEST_READ_MOSTLY:
                usleep(10);   // 10us between reads
                break;
            case TEST_WRITE_MOSTLY:
                usleep(100);  // 100us between reads
                break;
            case TEST_MIXED_LOAD:
                usleep(50);   // 50us between reads
                break;
            case TEST_STRESS_TEST:
                break;        // No delay
        }
    }

    gettimeofday(&reader->end_time, NULL);
    reader->state = THREAD_FINISHED;
    return NULL;
}

// Writer Thread
void* writer_thread(void *arg) {
    writer_thread_t *writer = (writer_thread_t*)arg;
    rcuscale_t *scale = (rcuscale_t*)((void**)arg)[1];

    gettimeofday(&writer->start_time, NULL);
    writer->state = THREAD_RUNNING;

    while (scale->running) {
        struct timeval start, end;
        gettimeofday(&start, NULL);

        // Write operation
        pthread_rwlock_wrlock(&scale->data_lock);
        // Simulate write operation
        usleep(50);  // 50us write time
        scale->data->version++;
        writer->writes++;
        pthread_rwlock_unlock(&scale->data_lock);

        gettimeofday(&end, NULL);
        writer->write_time += (end.tv_sec - start.tv_sec) * 1000000 +
                             (end.tv_usec - start.tv_usec);

        // Add delay based on test type
        switch (scale->test_type) {
            case TEST_READ_MOSTLY:
                usleep(1000); // 1ms between writes
                break;
            case TEST_WRITE_MOSTLY:
                usleep(100);  // 100us between writes
                break;
            case TEST_MIXED_LOAD:
                usleep(500);  // 500us between writes
                break;
            case TEST_STRESS_TEST:
                usleep(10);   // 10us between writes
                break;
        }
    }

    gettimeofday(&writer->end_time, NULL);
    writer->state = THREAD_FINISHED;
    return NULL;
}

// Run Test
void run_test(rcuscale_t *scale) {
    if (!scale) return;

    LOG(LOG_LEVEL_INFO, "Starting %s test...",
        get_test_type_string(scale->test_type));

    // Start threads
    scale->running = true;
    void *thread_args[2];
    thread_args[1] = scale;

    for (size_t i = 0; i < scale->nr_readers; i++) {
        thread_args[0] = &scale->readers[i];
        pthread_create(&scale->readers[i].thread, NULL, reader_thread, thread_args);
    }

    for (size_t i = 0; i < scale->nr_writers; i++) {
        thread_args[0] = &scale->writers[i];
        pthread_create(&scale->writers[i].thread, NULL, writer_thread, thread_args);
    }

    // Warm up
    sleep(WARMUP_DURATION);

    // Run test
    sleep(TEST_DURATION);

    // Stop threads
    scale->running = false;

    // Wait for threads to finish
    for (size_t i = 0; i < scale->nr_readers; i++) {
        pthread_join(scale->readers[i].thread, NULL);
    }
    for (size_t i = 0; i < scale->nr_writers; i++) {
        pthread_join(scale->writers[i].thread, NULL);
    }

    // Cool down
    sleep(COOLDOWN_TIME);

    // Calculate statistics
    calculate_stats(scale);
}

// Calculate Statistics
void calculate_stats(rcuscale_t *scale) {
    if (!scale) return;

    scale->stats.total_reads = 0;
    scale->stats.total_writes = 0;
    uint64_t total_read_time = 0;
    uint64_t total_write_time = 0;

    // Calculate reader statistics
    for (size_t i = 0; i < scale->nr_readers; i++) {
        reader_thread_t *reader = &scale->readers[i];
        scale->stats.total_reads += reader->reads;
        total_read_time += reader->read_time;
    }

    // Calculate writer statistics
    for (size_t i = 0; i < scale->nr_writers; i++) {
        writer_thread_t *writer = &scale->writers[i];
        scale->stats.total_writes += writer->writes;
        total_write_time += writer->write_time;
    }

    // Calculate averages
    if (scale->stats.total_reads > 0) {
        scale->stats.avg_read_latency = 
            (double)total_read_time / scale->stats.total_reads;
    }
    if (scale->stats.total_writes > 0) {
        scale->stats.avg_write_latency = 
            (double)total_write_time / scale->stats.total_writes;
    }

    // Calculate throughput
    scale->stats.test_duration = TEST_DURATION;
    scale->stats.read_throughput = 
        scale->stats.total_reads / scale->stats.test_duration;
    scale->stats.write_throughput = 
        scale->stats.total_writes / scale->stats.test_duration;
    scale->stats.grace_periods = scale->data->version;
}

// Print Test Statistics
void print_test_stats(rcuscale_t *scale) {
    if (!scale) return;

    printf("\nRCU Scale Test Results (%s):\n",
        get_test_type_string(scale->test_type));
    printf("-------------------------\n");
    printf("Test Duration:       %.2f seconds\n", scale->stats.test_duration);
    printf("Total Reads:         %lu\n", scale->stats.total_reads);
    printf("Total Writes:        %lu\n", scale->stats.total_writes);
    printf("Read Throughput:     %.2f ops/sec\n", scale->stats.read_throughput);
    printf("Write Throughput:    %.2f ops/sec\n", scale->stats.write_throughput);
    printf("Avg Read Latency:    %.2f us\n", scale->stats.avg_read_latency);
    printf("Avg Write Latency:   %.2f us\n", scale->stats.avg_write_latency);
    printf("Grace Periods:       %lu\n", scale->stats.grace_periods);

    // Print thread details
    printf("\nReader Threads:\n");
    for (size_t i = 0; i < scale->nr_readers; i++) {
        reader_thread_t *reader = &scale->readers[i];
        printf("  Reader %zu: %lu reads, %.2f us/read\n",
            i, reader->reads,
            reader->reads > 0 ? (double)reader->read_time / reader->reads : 0);
    }

    printf("\nWriter Threads:\n");
    for (size_t i = 0; i < scale->nr_writers; i++) {
        writer_thread_t *writer = &scale->writers[i];
        printf("  Writer %zu: %lu writes, %.2f us/write\n",
            i, writer->writes,
            writer->writes > 0 ? (double)writer->write_time / writer->writes : 0);
    }
}

// Destroy RCU Scale Manager
void destroy_rcuscale(rcuscale_t *scale) {
    if (!scale) return;

    free(scale->data->data);
    free(scale->data);
    pthread_mutex_destroy(&scale->manager_lock);
    pthread_rwlock_destroy(&scale->data_lock);
    free(scale);
    LOG(LOG_LEVEL_DEBUG, "Destroyed RCU scale manager");
}

// Demonstrate RCU Scale
void demonstrate_rcuscale(void) {
    printf("Starting RCU scale demonstration...\n");

    // Test 1: Read-mostly workload
    rcuscale_t *scale = create_rcuscale(8, 2, TEST_READ_MOSTLY);
    if (scale) {
        run_test(scale);
        print_test_stats(scale);
        destroy_rcuscale(scale);
    }

    // Test 2: Write-mostly workload
    scale = create_rcuscale(4, 4, TEST_WRITE_MOSTLY);
    if (scale) {
        run_test(scale);
        print_test_stats(scale);
        destroy_rcuscale(scale);
    }

    // Test 3: Mixed workload
    scale = create_rcuscale(6, 3, TEST_MIXED_LOAD);
    if (scale) {
        run_test(scale);
        print_test_stats(scale);
        destroy_rcuscale(scale);
    }

    // Test 4: Stress test
    scale = create_rcuscale(MAX_READERS, MAX_WRITERS, TEST_STRESS_TEST);
    if (scale) {
        run_test(scale);
        print_test_stats(scale);
        destroy_rcuscale(scale);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_rcuscale();

    return 0;
}
