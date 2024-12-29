#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

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

// RCU Constants
#define MAX_CPUS           8
#define MAX_SEGMENTS       4
#define MAX_CBS_PER_SEG    64
#define GRACE_PERIOD_MS    10
#define MAX_BATCH_SIZE     16

// Callback States
typedef enum {
    CB_IDLE,
    CB_PENDING,
    CB_PROCESSING,
    CB_DONE
} cb_state_t;

// Segment States
typedef enum {
    SEG_EMPTY,
    SEG_FILLING,
    SEG_FULL,
    SEG_PROCESSING
} seg_state_t;

// CPU States
typedef enum {
    CPU_ACTIVE,
    CPU_IDLE,
    CPU_OFFLINE
} cpu_state_t;

// Callback Structure
typedef struct {
    void (*func)(void *);
    void *arg;
    uint64_t enqueue_time;
    uint64_t grace_period;
    cb_state_t state;
} callback_t;

// Segment Structure
typedef struct {
    callback_t callbacks[MAX_CBS_PER_SEG];
    size_t head;
    size_t tail;
    size_t count;
    seg_state_t state;
    uint64_t start_time;
} segment_t;

// CPU Structure
typedef struct {
    unsigned int id;
    cpu_state_t state;
    uint64_t last_processing;
    pthread_mutex_t lock;
} rcu_cpu_t;

// Statistics Structure
typedef struct {
    unsigned long callbacks_queued;
    unsigned long callbacks_processed;
    unsigned long segments_filled;
    unsigned long grace_periods;
    double avg_processing_time;
    double avg_queue_length;
} segcb_stats_t;

// Segmented Callback List
typedef struct {
    segment_t segments[MAX_SEGMENTS];
    size_t current_segment;
    rcu_cpu_t cpus[MAX_CPUS];
    size_t nr_cpus;
    pthread_t processor_thread;
    bool running;
    pthread_mutex_t list_lock;
    segcb_stats_t stats;
} segcblist_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_cb_state_string(cb_state_t state);
const char* get_seg_state_string(seg_state_t state);
const char* get_cpu_state_string(cpu_state_t state);

segcblist_t* create_segcblist(size_t nr_cpus);
void destroy_segcblist(segcblist_t *list);

bool enqueue_callback(segcblist_t *list, void (*func)(void *), void *arg);
void process_segment(segcblist_t *list, segment_t *seg);
void advance_segment(segcblist_t *list);

void* processor_thread(void *arg);
void print_segcb_stats(segcblist_t *list);
void demonstrate_segcblist(void);

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

// Utility Function: Get Callback State String
const char* get_cb_state_string(cb_state_t state) {
    switch(state) {
        case CB_IDLE:       return "IDLE";
        case CB_PENDING:    return "PENDING";
        case CB_PROCESSING: return "PROCESSING";
        case CB_DONE:       return "DONE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Segment State String
const char* get_seg_state_string(seg_state_t state) {
    switch(state) {
        case SEG_EMPTY:      return "EMPTY";
        case SEG_FILLING:    return "FILLING";
        case SEG_FULL:       return "FULL";
        case SEG_PROCESSING: return "PROCESSING";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get CPU State String
const char* get_cpu_state_string(cpu_state_t state) {
    switch(state) {
        case CPU_ACTIVE:  return "ACTIVE";
        case CPU_IDLE:    return "IDLE";
        case CPU_OFFLINE: return "OFFLINE";
        default: return "UNKNOWN";
    }
}

// Create Segmented Callback List
segcblist_t* create_segcblist(size_t nr_cpus) {
    if (nr_cpus > MAX_CPUS) {
        LOG(LOG_LEVEL_ERROR, "Number of CPUs exceeds maximum");
        return NULL;
    }

    segcblist_t *list = malloc(sizeof(segcblist_t));
    if (!list) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate segmented callback list");
        return NULL;
    }

    // Initialize segments
    for (size_t i = 0; i < MAX_SEGMENTS; i++) {
        segment_t *seg = &list->segments[i];
        seg->head = 0;
        seg->tail = 0;
        seg->count = 0;
        seg->state = SEG_EMPTY;
        seg->start_time = 0;
    }

    // Initialize CPUs
    for (size_t i = 0; i < nr_cpus; i++) {
        list->cpus[i].id = i;
        list->cpus[i].state = CPU_ACTIVE;
        list->cpus[i].last_processing = 0;
        pthread_mutex_init(&list->cpus[i].lock, NULL);
    }

    list->current_segment = 0;
    list->nr_cpus = nr_cpus;
    pthread_mutex_init(&list->list_lock, NULL);
    memset(&list->stats, 0, sizeof(segcb_stats_t));
    list->running = true;

    // Start processor thread
    pthread_create(&list->processor_thread, NULL, processor_thread, list);

    LOG(LOG_LEVEL_DEBUG, "Created segmented callback list with %zu CPUs", nr_cpus);
    return list;
}

// Enqueue Callback
bool enqueue_callback(segcblist_t *list, void (*func)(void *), void *arg) {
    if (!list || !func) return false;

    pthread_mutex_lock(&list->list_lock);

    segment_t *seg = &list->segments[list->current_segment];
    
    // Check if current segment is full
    if (seg->count >= MAX_CBS_PER_SEG) {
        if (seg->state == SEG_FILLING) {
            seg->state = SEG_FULL;
            list->stats.segments_filled++;
            advance_segment(list);
            seg = &list->segments[list->current_segment];
        } else {
            pthread_mutex_unlock(&list->list_lock);
            return false;
        }
    }

    // Initialize segment if empty
    if (seg->state == SEG_EMPTY) {
        seg->state = SEG_FILLING;
        seg->start_time = time(NULL);
    }

    // Add callback to segment
    callback_t *cb = &seg->callbacks[seg->tail];
    cb->func = func;
    cb->arg = arg;
    cb->enqueue_time = time(NULL);
    cb->grace_period = list->stats.grace_periods;
    cb->state = CB_PENDING;

    seg->tail = (seg->tail + 1) % MAX_CBS_PER_SEG;
    seg->count++;
    list->stats.callbacks_queued++;

    // Update average queue length
    list->stats.avg_queue_length = 
        (list->stats.avg_queue_length * (list->stats.callbacks_queued - 1) +
         seg->count) / list->stats.callbacks_queued;

    pthread_mutex_unlock(&list->list_lock);
    return true;
}

// Process Segment
void process_segment(segcblist_t *list, segment_t *seg) {
    if (!list || !seg || seg->state != SEG_FULL) return;

    seg->state = SEG_PROCESSING;
    size_t processed = 0;

    // Process callbacks in batches
    while (processed < seg->count && processed < MAX_BATCH_SIZE) {
        callback_t *cb = &seg->callbacks[seg->head];
        
        if (cb->state == CB_PENDING) {
            cb->state = CB_PROCESSING;
            
            // Execute callback
            cb->func(cb->arg);
            
            cb->state = CB_DONE;
            list->stats.callbacks_processed++;

            // Update average processing time
            uint64_t process_time = time(NULL) - cb->enqueue_time;
            list->stats.avg_processing_time = 
                (list->stats.avg_processing_time * 
                 (list->stats.callbacks_processed - 1) + process_time) /
                list->stats.callbacks_processed;
        }

        seg->head = (seg->head + 1) % MAX_CBS_PER_SEG;
        processed++;
    }

    // Check if segment is fully processed
    if (processed >= seg->count) {
        seg->state = SEG_EMPTY;
        seg->head = 0;
        seg->tail = 0;
        seg->count = 0;
    }
}

// Advance Segment
void advance_segment(segcblist_t *list) {
    list->current_segment = (list->current_segment + 1) % MAX_SEGMENTS;
    
    // Initialize new segment if needed
    segment_t *new_seg = &list->segments[list->current_segment];
    if (new_seg->state == SEG_EMPTY) {
        new_seg->state = SEG_FILLING;
        new_seg->start_time = time(NULL);
    }
}

// Processor Thread
void* processor_thread(void *arg) {
    segcblist_t *list = (segcblist_t*)arg;

    while (list->running) {
        pthread_mutex_lock(&list->list_lock);

        // Process full segments
        for (size_t i = 0; i < MAX_SEGMENTS; i++) {
            segment_t *seg = &list->segments[i];
            if (seg->state == SEG_FULL) {
                process_segment(list, seg);
            }
        }

        // Update grace period
        list->stats.grace_periods++;

        pthread_mutex_unlock(&list->list_lock);
        usleep(GRACE_PERIOD_MS * 1000);
    }

    return NULL;
}

// Print Segmented Callback List Statistics
void print_segcb_stats(segcblist_t *list) {
    if (!list) return;

    printf("\nSegmented Callback List Statistics:\n");
    printf("----------------------------------\n");
    printf("Callbacks Queued:    %lu\n", list->stats.callbacks_queued);
    printf("Callbacks Processed: %lu\n", list->stats.callbacks_processed);
    printf("Segments Filled:     %lu\n", list->stats.segments_filled);
    printf("Grace Periods:       %lu\n", list->stats.grace_periods);
    printf("Avg Processing Time: %.2f seconds\n", list->stats.avg_processing_time);
    printf("Avg Queue Length:    %.2f\n", list->stats.avg_queue_length);

    // Print segment details
    for (size_t i = 0; i < MAX_SEGMENTS; i++) {
        segment_t *seg = &list->segments[i];
        printf("\nSegment %zu:\n", i);
        printf("  State: %s\n", get_seg_state_string(seg->state));
        printf("  Count: %zu\n", seg->count);
        if (seg->state != SEG_EMPTY) {
            printf("  Age: %lu seconds\n", time(NULL) - seg->start_time);
        }
    }

    // Print CPU details
    for (size_t i = 0; i < list->nr_cpus; i++) {
        rcu_cpu_t *cpu = &list->cpus[i];
        printf("\nCPU %zu:\n", i);
        printf("  State: %s\n", get_cpu_state_string(cpu->state));
        if (cpu->last_processing > 0) {
            printf("  Last Processing: %lu seconds ago\n",
                time(NULL) - cpu->last_processing);
        }
    }
}

// Example callback function
void test_callback(void *arg) {
    int *value = (int*)arg;
    LOG(LOG_LEVEL_DEBUG, "Test callback executed with value: %d", *value);
}

// Destroy Segmented Callback List
void destroy_segcblist(segcblist_t *list) {
    if (!list) return;

    // Stop processor thread
    list->running = false;
    pthread_join(list->processor_thread, NULL);

    // Clean up CPU mutexes
    for (size_t i = 0; i < list->nr_cpus; i++) {
        pthread_mutex_destroy(&list->cpus[i].lock);
    }

    pthread_mutex_destroy(&list->list_lock);
    free(list);
    LOG(LOG_LEVEL_DEBUG, "Destroyed segmented callback list");
}

// Demonstrate Segmented Callback List
void demonstrate_segcblist(void) {
    // Create segmented callback list with 4 CPUs
    segcblist_t *list = create_segcblist(4);
    if (!list) return;

    printf("Starting segmented callback list demonstration...\n");

    // Scenario 1: Fill a segment
    printf("\nScenario 1: Fill a segment\n");
    for (int i = 0; i < MAX_CBS_PER_SEG; i++) {
        int *value = malloc(sizeof(int));
        *value = i;
        enqueue_callback(list, test_callback, value);
    }

    // Scenario 2: Multiple segments
    printf("\nScenario 2: Multiple segments\n");
    for (int i = 0; i < MAX_CBS_PER_SEG * 2; i++) {
        int *value = malloc(sizeof(int));
        *value = i + 100;
        enqueue_callback(list, test_callback, value);
    }

    // Wait for processing
    sleep(2);

    // Print statistics
    print_segcb_stats(list);

    // Cleanup
    destroy_segcblist(list);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_segcblist();

    return 0;
}
