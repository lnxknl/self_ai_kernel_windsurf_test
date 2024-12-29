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

// SRCU Tree Constants
#define MAX_NODES           1024
#define MAX_READERS         32
#define MAX_WRITERS         8
#define MAX_DEPTH           16
#define SLEEP_MIN_US       100
#define SLEEP_MAX_US      1000
#define TEST_DURATION      30    // seconds
#define GRACE_PERIOD_US   1000   // 1ms

// Node Types
typedef enum {
    NODE_INTERNAL,
    NODE_LEAF
} node_type_t;

// Thread States
typedef enum {
    THREAD_INIT,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_FINISHED
} thread_state_t;

// Operation Types
typedef enum {
    OP_READ,
    OP_WRITE,
    OP_SLEEP,
    OP_TRAVERSE
} operation_t;

// Tree Node Structure
typedef struct node {
    void *data;
    size_t size;
    uint64_t version;
    node_type_t type;
    struct node *left;
    struct node *right;
    struct node *parent;
    pthread_mutex_t lock;
} node_t;

// SRCU Domain Structure
typedef struct {
    pthread_mutex_t lock;
    uint64_t completed;
    uint64_t current;
    uint64_t readers[MAX_READERS];
} srcu_domain_t;

// Reader Thread Structure
typedef struct {
    pthread_t thread;
    unsigned int id;
    thread_state_t state;
    uint64_t reads;
    uint64_t sleeps;
    uint64_t read_time;
    uint64_t sleep_time;
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

// Statistics Structure
typedef struct {
    uint64_t total_reads;
    uint64_t total_writes;
    uint64_t total_sleeps;
    uint64_t grace_periods;
    double avg_read_latency;
    double avg_write_latency;
    double avg_sleep_duration;
    double test_duration;
    uint64_t tree_depth;
    uint64_t node_count;
} srcu_stats_t;

// SRCU Tree Manager
typedef struct {
    node_t *root;
    srcu_domain_t domain;
    reader_thread_t readers[MAX_READERS];
    writer_thread_t writers[MAX_WRITERS];
    size_t nr_readers;
    size_t nr_writers;
    bool running;
    pthread_mutex_t manager_lock;
    srcu_stats_t stats;
} srcutree_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_thread_state_string(thread_state_t state);
const char* get_operation_string(operation_t op);

srcutree_t* create_srcutree(size_t nr_readers, size_t nr_writers);
void destroy_srcutree(srcutree_t *tree);

void* reader_thread(void *arg);
void* writer_thread(void *arg);
void run_test(srcutree_t *tree);

node_t* create_node(void *data, size_t size, node_type_t type);
void destroy_node(node_t *node);
int srcu_read_lock(srcu_domain_t *domain, unsigned int reader_id);
void srcu_read_unlock(srcu_domain_t *domain, unsigned int reader_id, int idx);
void srcu_synchronize(srcu_domain_t *domain);

void calculate_stats(srcutree_t *tree);
void print_test_stats(srcutree_t *tree);
void demonstrate_srcutree(void);

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
        case THREAD_SLEEPING: return "SLEEPING";
        case THREAD_FINISHED: return "FINISHED";
        default: return "UNKNOWN";
    }
}

const char* get_operation_string(operation_t op) {
    switch(op) {
        case OP_READ:     return "READ";
        case OP_WRITE:    return "WRITE";
        case OP_SLEEP:    return "SLEEP";
        case OP_TRAVERSE: return "TRAVERSE";
        default: return "UNKNOWN";
    }
}

// Create Tree Node
node_t* create_node(void *data, size_t size, node_type_t type) {
    node_t *node = malloc(sizeof(node_t));
    if (!node) return NULL;

    node->data = malloc(size);
    if (!node->data) {
        free(node);
        return NULL;
    }

    memcpy(node->data, data, size);
    node->size = size;
    node->version = 0;
    node->type = type;
    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;
    pthread_mutex_init(&node->lock, NULL);

    return node;
}

// Create SRCU Tree
srcutree_t* create_srcutree(size_t nr_readers, size_t nr_writers) {
    if (nr_readers > MAX_READERS || nr_writers > MAX_WRITERS) {
        LOG(LOG_LEVEL_ERROR, "Number of threads exceeds maximum");
        return NULL;
    }

    srcutree_t *tree = malloc(sizeof(srcutree_t));
    if (!tree) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate SRCU tree");
        return NULL;
    }

    // Initialize root node
    int root_data = 0;
    tree->root = create_node(&root_data, sizeof(int), NODE_INTERNAL);
    if (!tree->root) {
        free(tree);
        return NULL;
    }

    // Initialize SRCU domain
    pthread_mutex_init(&tree->domain.lock, NULL);
    tree->domain.completed = 0;
    tree->domain.current = 0;
    memset(tree->domain.readers, 0, sizeof(tree->domain.readers));

    // Initialize readers
    for (size_t i = 0; i < nr_readers; i++) {
        tree->readers[i].id = i;
        tree->readers[i].state = THREAD_INIT;
        tree->readers[i].reads = 0;
        tree->readers[i].sleeps = 0;
        tree->readers[i].read_time = 0;
        tree->readers[i].sleep_time = 0;
    }

    // Initialize writers
    for (size_t i = 0; i < nr_writers; i++) {
        tree->writers[i].id = i;
        tree->writers[i].state = THREAD_INIT;
        tree->writers[i].writes = 0;
        tree->writers[i].write_time = 0;
    }

    tree->nr_readers = nr_readers;
    tree->nr_writers = nr_writers;
    tree->running = false;
    pthread_mutex_init(&tree->manager_lock, NULL);
    memset(&tree->stats, 0, sizeof(srcu_stats_t));

    LOG(LOG_LEVEL_DEBUG, "Created SRCU tree with %zu readers, %zu writers",
        nr_readers, nr_writers);
    return tree;
}

// SRCU Read Lock
int srcu_read_lock(srcu_domain_t *domain, unsigned int reader_id) {
    pthread_mutex_lock(&domain->lock);
    int idx = domain->current;
    domain->readers[reader_id] = idx + 1;
    pthread_mutex_unlock(&domain->lock);
    return idx;
}

// SRCU Read Unlock
void srcu_read_unlock(srcu_domain_t *domain, unsigned int reader_id, int idx) {
    pthread_mutex_lock(&domain->lock);
    if (domain->readers[reader_id] == idx + 1) {
        domain->readers[reader_id] = 0;
    }
    pthread_mutex_unlock(&domain->lock);
}

// SRCU Synchronize
void srcu_synchronize(srcu_domain_t *domain) {
    pthread_mutex_lock(&domain->lock);
    
    // Switch to new grace period
    domain->current ^= 1;
    domain->completed++;

    // Wait for readers
    for (size_t i = 0; i < MAX_READERS; i++) {
        while (domain->readers[i] == (domain->current ^ 1) + 1) {
            pthread_mutex_unlock(&domain->lock);
            usleep(1);  // Give up CPU briefly
            pthread_mutex_lock(&domain->lock);
        }
    }

    pthread_mutex_unlock(&domain->lock);
}

// Reader Thread
void* reader_thread(void *arg) {
    reader_thread_t *reader = (reader_thread_t*)arg;
    srcutree_t *tree = (srcutree_t*)((void**)arg)[1];

    gettimeofday(&reader->start_time, NULL);
    reader->state = THREAD_RUNNING;

    while (tree->running) {
        struct timeval start, end;
        gettimeofday(&start, NULL);

        // Read operation with potential sleep
        int idx = srcu_read_lock(&tree->domain, reader->id);
        
        // Traverse tree
        node_t *current = tree->root;
        while (current) {
            if (rand() % 100 < 30) {  // 30% chance to sleep
                reader->state = THREAD_SLEEPING;
                usleep(SLEEP_MIN_US + rand() % (SLEEP_MAX_US - SLEEP_MIN_US));
                reader->sleeps++;
                reader->state = THREAD_RUNNING;
            }
            
            if (current->left && rand() % 2) {
                current = current->left;
            } else if (current->right) {
                current = current->right;
            } else {
                break;
            }
            reader->reads++;
        }

        srcu_read_unlock(&tree->domain, reader->id, idx);

        gettimeofday(&end, NULL);
        reader->read_time += (end.tv_sec - start.tv_sec) * 1000000 +
                            (end.tv_usec - start.tv_usec);

        // Small delay between operations
        usleep(100);
    }

    gettimeofday(&reader->end_time, NULL);
    reader->state = THREAD_FINISHED;
    return NULL;
}

// Writer Thread
void* writer_thread(void *arg) {
    writer_thread_t *writer = (writer_thread_t*)arg;
    srcutree_t *tree = (srcutree_t*)((void**)arg)[1];

    gettimeofday(&writer->start_time, NULL);
    writer->state = THREAD_RUNNING;

    while (tree->running) {
        struct timeval start, end;
        gettimeofday(&start, NULL);

        // Write operation
        node_t *current = tree->root;
        bool wrote = false;

        while (current && !wrote) {
            pthread_mutex_lock(&current->lock);
            
            if (!current->left || !current->right) {
                // Create new child node
                int new_data = rand();
                node_t *new_node = create_node(&new_data, sizeof(int), NODE_LEAF);
                if (new_node) {
                    if (!current->left) {
                        current->left = new_node;
                    } else {
                        current->right = new_node;
                    }
                    new_node->parent = current;
                    wrote = true;
                    writer->writes++;
                }
            }
            
            pthread_mutex_unlock(&current->lock);

            if (!wrote) {
                current = (rand() % 2) ? current->left : current->right;
            }
        }

        // Synchronize after write
        srcu_synchronize(&tree->domain);

        gettimeofday(&end, NULL);
        writer->write_time += (end.tv_sec - start.tv_sec) * 1000000 +
                             (end.tv_usec - start.tv_usec);

        // Delay between writes
        usleep(500);
    }

    gettimeofday(&writer->end_time, NULL);
    writer->state = THREAD_FINISHED;
    return NULL;
}

// Run Test
void run_test(srcutree_t *tree) {
    if (!tree) return;

    LOG(LOG_LEVEL_INFO, "Starting SRCU tree test...");

    // Start threads
    tree->running = true;
    void *thread_args[2];
    thread_args[1] = tree;

    for (size_t i = 0; i < tree->nr_readers; i++) {
        thread_args[0] = &tree->readers[i];
        pthread_create(&tree->readers[i].thread, NULL, reader_thread, thread_args);
    }

    for (size_t i = 0; i < tree->nr_writers; i++) {
        thread_args[0] = &tree->writers[i];
        pthread_create(&tree->writers[i].thread, NULL, writer_thread, thread_args);
    }

    // Run test
    sleep(TEST_DURATION);

    // Stop threads
    tree->running = false;

    // Wait for threads to finish
    for (size_t i = 0; i < tree->nr_readers; i++) {
        pthread_join(tree->readers[i].thread, NULL);
    }
    for (size_t i = 0; i < tree->nr_writers; i++) {
        pthread_join(tree->writers[i].thread, NULL);
    }

    // Calculate statistics
    calculate_stats(tree);
}

// Calculate Statistics
void calculate_stats(srcutree_t *tree) {
    if (!tree) return;

    tree->stats.total_reads = 0;
    tree->stats.total_writes = 0;
    tree->stats.total_sleeps = 0;
    uint64_t total_read_time = 0;
    uint64_t total_write_time = 0;
    uint64_t total_sleep_time = 0;

    // Calculate reader statistics
    for (size_t i = 0; i < tree->nr_readers; i++) {
        reader_thread_t *reader = &tree->readers[i];
        tree->stats.total_reads += reader->reads;
        tree->stats.total_sleeps += reader->sleeps;
        total_read_time += reader->read_time;
        total_sleep_time += reader->sleep_time;
    }

    // Calculate writer statistics
    for (size_t i = 0; i < tree->nr_writers; i++) {
        writer_thread_t *writer = &tree->writers[i];
        tree->stats.total_writes += writer->writes;
        total_write_time += writer->write_time;
    }

    // Calculate averages
    if (tree->stats.total_reads > 0) {
        tree->stats.avg_read_latency = 
            (double)total_read_time / tree->stats.total_reads;
    }
    if (tree->stats.total_writes > 0) {
        tree->stats.avg_write_latency = 
            (double)total_write_time / tree->stats.total_writes;
    }
    if (tree->stats.total_sleeps > 0) {
        tree->stats.avg_sleep_duration = 
            (double)total_sleep_time / tree->stats.total_sleeps;
    }

    tree->stats.test_duration = TEST_DURATION;
    tree->stats.grace_periods = tree->domain.completed;
}

// Print Test Statistics
void print_test_stats(srcutree_t *tree) {
    if (!tree) return;

    printf("\nSRCU Tree Test Results:\n");
    printf("-------------------------\n");
    printf("Test Duration:       %.2f seconds\n", tree->stats.test_duration);
    printf("Total Reads:         %lu\n", tree->stats.total_reads);
    printf("Total Writes:        %lu\n", tree->stats.total_writes);
    printf("Total Sleeps:        %lu\n", tree->stats.total_sleeps);
    printf("Avg Read Latency:    %.2f us\n", tree->stats.avg_read_latency);
    printf("Avg Write Latency:   %.2f us\n", tree->stats.avg_write_latency);
    printf("Avg Sleep Duration:  %.2f us\n", tree->stats.avg_sleep_duration);
    printf("Grace Periods:       %lu\n", tree->stats.grace_periods);

    // Print thread details
    printf("\nReader Threads:\n");
    for (size_t i = 0; i < tree->nr_readers; i++) {
        reader_thread_t *reader = &tree->readers[i];
        printf("  Reader %zu: %s, %lu reads, %lu sleeps\n",
            i, get_thread_state_string(reader->state),
            reader->reads, reader->sleeps);
    }

    printf("\nWriter Threads:\n");
    for (size_t i = 0; i < tree->nr_writers; i++) {
        writer_thread_t *writer = &tree->writers[i];
        printf("  Writer %zu: %s, %lu writes\n",
            i, get_thread_state_string(writer->state),
            writer->writes);
    }
}

// Destroy Tree Node
void destroy_node(node_t *node) {
    if (!node) return;

    destroy_node(node->left);
    destroy_node(node->right);
    
    pthread_mutex_destroy(&node->lock);
    free(node->data);
    free(node);
}

// Destroy SRCU Tree
void destroy_srcutree(srcutree_t *tree) {
    if (!tree) return;

    destroy_node(tree->root);
    pthread_mutex_destroy(&tree->domain.lock);
    pthread_mutex_destroy(&tree->manager_lock);
    free(tree);
    LOG(LOG_LEVEL_DEBUG, "Destroyed SRCU tree");
}

// Demonstrate SRCU Tree
void demonstrate_srcutree(void) {
    printf("Starting SRCU tree demonstration...\n");

    // Create and run SRCU tree test
    srcutree_t *tree = create_srcutree(8, 2);
    if (tree) {
        run_test(tree);
        print_test_stats(tree);
        destroy_srcutree(tree);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_srcutree();

    return 0;
}
