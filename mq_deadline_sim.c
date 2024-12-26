#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <time.h>

// Logging and Debugging Macros
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

// Global Log Level Configuration
static int current_log_level = LOG_LEVEL_INFO;

// I/O Request Types and Priorities
typedef enum {
    IO_TYPE_READ,
    IO_TYPE_WRITE,
    IO_TYPE_DISCARD,
    IO_TYPE_FLUSH
} io_type_t;

typedef enum {
    IO_PRIO_REALTIME,
    IO_PRIO_HIGH,
    IO_PRIO_NORMAL,
    IO_PRIO_LOW,
    IO_PRIO_IDLE
} io_priority_t;

// Advanced I/O Request Structure
typedef struct io_request {
    unsigned long long request_id;   // Unique request identifier
    int pid;                         // Process ID
    io_type_t type;                  // Type of I/O operation
    io_priority_t priority;          // Request priority
    unsigned long sector;            // Starting sector
    size_t size;                     // Size of request in sectors
    unsigned long long submission_time;  // Time of request submission
    unsigned long long deadline;     // Deadline for request completion
    unsigned long long completion_time;  // Time of request completion
    int retry_count;                 // Number of dispatch attempts
    bool is_dispatched;              // Dispatch status
    bool is_completed;               // Completion status

    // Linked list pointers for different queues
    struct io_request *next_fifo;    // FIFO queue link
    struct io_request *next_sorted;  // Sorted deadline queue link
} io_request_t;

// Queue Management Structure
typedef struct {
    io_request_t *read_fifo_head;
    io_request_t *read_fifo_tail;
    io_request_t *write_fifo_head;
    io_request_t *write_fifo_tail;
    
    io_request_t *read_sorted_head;
    io_request_t *write_sorted_head;
    
    int read_fifo_count;
    int write_fifo_count;
} io_queue_t;

// Scheduler Configuration and Statistics
typedef struct {
    // Deadline Configuration
    unsigned long long read_expire;     // Read request expiration time
    unsigned long long write_expire;    // Write request expiration time
    unsigned long long max_deadline;    // Maximum allowed deadline
    
    // Dispatch Limits
    int max_read_dispatch;              // Max read requests per dispatch
    int max_write_dispatch;             // Max write requests per dispatch
    
    // Statistics
    unsigned long long total_requests;
    unsigned long long completed_requests;
    unsigned long long expired_requests;
    unsigned long long dispatched_requests;
    
    // Performance Metrics
    double avg_read_latency;
    double avg_write_latency;
} scheduler_config_t;

// Multi-Queue Deadline Scheduler
typedef struct {
    io_queue_t queue;
    scheduler_config_t config;
    unsigned long long current_time;
} mq_deadline_scheduler_t;

// Utility Function Prototypes
const char* get_log_level_string(int level);
const char* get_io_type_string(io_type_t type);
const char* get_io_priority_string(io_priority_t priority);

mq_deadline_scheduler_t* create_mq_deadline_scheduler();
void destroy_mq_deadline_scheduler(mq_deadline_scheduler_t *scheduler);

io_request_t* create_io_request(
    int pid, 
    io_type_t type, 
    io_priority_t priority, 
    unsigned long sector, 
    size_t size
);

void enqueue_request(
    mq_deadline_scheduler_t *scheduler, 
    io_request_t *request
);

io_request_t* dequeue_request(
    mq_deadline_scheduler_t *scheduler, 
    bool is_read
);

void dispatch_requests(mq_deadline_scheduler_t *scheduler);
void update_scheduler_statistics(mq_deadline_scheduler_t *scheduler);

// Simulation and Demonstration Functions
void run_mq_deadline_simulation(mq_deadline_scheduler_t *scheduler);
void demonstrate_mq_deadline_scheduler();

// Utility Function Implementations
const char* get_log_level_string(int level) {
    switch(level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

const char* get_io_type_string(io_type_t type) {
    switch(type) {
        case IO_TYPE_READ:    return "READ";
        case IO_TYPE_WRITE:   return "WRITE";
        case IO_TYPE_DISCARD: return "DISCARD";
        case IO_TYPE_FLUSH:   return "FLUSH";
        default: return "UNKNOWN";
    }
}

const char* get_io_priority_string(io_priority_t priority) {
    switch(priority) {
        case IO_PRIO_REALTIME: return "REALTIME";
        case IO_PRIO_HIGH:     return "HIGH";
        case IO_PRIO_NORMAL:   return "NORMAL";
        case IO_PRIO_LOW:      return "LOW";
        case IO_PRIO_IDLE:     return "IDLE";
        default: return "UNKNOWN";
    }
}

// Scheduler Creation
mq_deadline_scheduler_t* create_mq_deadline_scheduler() {
    mq_deadline_scheduler_t *scheduler = malloc(sizeof(mq_deadline_scheduler_t));
    if (!scheduler) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate memory for scheduler");
        return NULL;
    }

    // Initialize queues
    memset(&scheduler->queue, 0, sizeof(io_queue_t));

    // Default configuration
    scheduler->config.read_expire = 500;     // 500 time units
    scheduler->config.write_expire = 1000;   // 1000 time units
    scheduler->config.max_deadline = 5000;   // Maximum 5000 time units
    scheduler->config.max_read_dispatch = 4; // Max 4 read requests per dispatch
    scheduler->config.max_write_dispatch = 2; // Max 2 write requests per dispatch

    // Reset statistics
    scheduler->config.total_requests = 0;
    scheduler->config.completed_requests = 0;
    scheduler->config.expired_requests = 0;
    scheduler->config.dispatched_requests = 0;
    scheduler->config.avg_read_latency = 0.0;
    scheduler->config.avg_write_latency = 0.0;

    scheduler->current_time = 0;

    return scheduler;
}

// Request Creation
io_request_t* create_io_request(
    int pid, 
    io_type_t type, 
    io_priority_t priority, 
    unsigned long sector, 
    size_t size
) {
    static unsigned long long next_request_id = 1;
    
    io_request_t *request = malloc(sizeof(io_request_t));
    if (!request) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate memory for I/O request");
        return NULL;
    }

    request->request_id = next_request_id++;
    request->pid = pid;
    request->type = type;
    request->priority = priority;
    request->sector = sector;
    request->size = size;
    request->submission_time = 0;
    request->deadline = 0;
    request->completion_time = 0;
    request->retry_count = 0;
    request->is_dispatched = false;
    request->is_completed = false;
    request->next_fifo = NULL;
    request->next_sorted = NULL;

    return request;
}

// Request Enqueuing
void enqueue_request(
    mq_deadline_scheduler_t *scheduler, 
    io_request_t *request
) {
    if (!scheduler || !request) return;

    request->submission_time = scheduler->current_time;
    request->deadline = request->submission_time + 
        (request->type == IO_TYPE_READ ? 
            scheduler->config.read_expire : 
            scheduler->config.write_expire);

    // Enqueue in FIFO
    if (request->type == IO_TYPE_READ) {
        if (!scheduler->queue.read_fifo_head) {
            scheduler->queue.read_fifo_head = request;
            scheduler->queue.read_fifo_tail = request;
        } else {
            scheduler->queue.read_fifo_tail->next_fifo = request;
            scheduler->queue.read_fifo_tail = request;
        }
        scheduler->queue.read_fifo_count++;
    } else if (request->type == IO_TYPE_WRITE) {
        if (!scheduler->queue.write_fifo_head) {
            scheduler->queue.write_fifo_head = request;
            scheduler->queue.write_fifo_tail = request;
        } else {
            scheduler->queue.write_fifo_tail->next_fifo = request;
            scheduler->queue.write_fifo_tail = request;
        }
        scheduler->queue.write_fifo_count++;
    }

    // Update scheduler statistics
    scheduler->config.total_requests++;
}

// Request Dequeuing
io_request_t* dequeue_request(
    mq_deadline_scheduler_t *scheduler, 
    bool is_read
) {
    if (!scheduler) return NULL;

    io_request_t *request = NULL;
    io_request_t **fifo_head = is_read ? 
        &scheduler->queue.read_fifo_head : 
        &scheduler->queue.write_fifo_head;
    int *fifo_count = is_read ? 
        &scheduler->queue.read_fifo_count : 
        &scheduler->queue.write_fifo_count;

    // Dequeue from FIFO
    if (*fifo_head) {
        request = *fifo_head;
        *fifo_head = request->next_fifo;
        (*fifo_count)--;

        // Reset links
        request->next_fifo = NULL;
    }

    return request;
}

// Request Dispatching
void dispatch_requests(mq_deadline_scheduler_t *scheduler) {
    int read_dispatched = 0;
    int write_dispatched = 0;

    // Dispatch read requests first (prioritized)
    while (read_dispatched < scheduler->config.max_read_dispatch &&
           scheduler->queue.read_fifo_head) {
        io_request_t *read_request = dequeue_request(scheduler, true);
        
        if (read_request->deadline < scheduler->current_time) {
            // Request expired
            scheduler->config.expired_requests++;
            LOG(LOG_LEVEL_WARN, "Read Request %llu expired", read_request->request_id);
            free(read_request);
            continue;
        }

        // Simulate request dispatch
        read_request->is_dispatched = true;
        read_request->completion_time = scheduler->current_time + 
            (read_request->size / 10);  // Simplified completion time

        LOG(LOG_LEVEL_INFO, "Dispatching Read Request %llu", read_request->request_id);
        read_dispatched++;
        scheduler->config.dispatched_requests++;
    }

    // Dispatch write requests
    while (write_dispatched < scheduler->config.max_write_dispatch &&
           scheduler->queue.write_fifo_head) {
        io_request_t *write_request = dequeue_request(scheduler, false);
        
        if (write_request->deadline < scheduler->current_time) {
            // Request expired
            scheduler->config.expired_requests++;
            LOG(LOG_LEVEL_WARN, "Write Request %llu expired", write_request->request_id);
            free(write_request);
            continue;
        }

        // Simulate request dispatch
        write_request->is_dispatched = true;
        write_request->completion_time = scheduler->current_time + 
            (write_request->size / 5);  // Slightly faster write

        LOG(LOG_LEVEL_INFO, "Dispatching Write Request %llu", write_request->request_id);
        write_dispatched++;
        scheduler->config.dispatched_requests++;
    }
}

// Update Scheduler Statistics
void update_scheduler_statistics(mq_deadline_scheduler_t *scheduler) {
    // Placeholder for more advanced statistical tracking
    double total_read_latency = 0.0;
    double total_write_latency = 0.0;
    int read_count = 0, write_count = 0;

    // Simulate tracking of completed requests
    // In a real implementation, this would track actual request completions
    if (scheduler->config.completed_requests > 0) {
        scheduler->config.avg_read_latency = total_read_latency / read_count;
        scheduler->config.avg_write_latency = total_write_latency / write_count;
    }
}

// Simulation Runner
void run_mq_deadline_simulation(mq_deadline_scheduler_t *scheduler) {
    unsigned long long simulation_duration = 1000;

    LOG(LOG_LEVEL_INFO, "Starting Multi-Queue Deadline I/O Scheduler Simulation");

    for (scheduler->current_time = 0; 
         scheduler->current_time < simulation_duration; 
         scheduler->current_time++) {
        
        // Periodic statistics update
        if (scheduler->current_time % 100 == 0) {
            update_scheduler_statistics(scheduler);
        }

        // Dispatch requests
        dispatch_requests(scheduler);
    }

    // Final statistics
    LOG(LOG_LEVEL_INFO, "Simulation Complete");
    LOG(LOG_LEVEL_INFO, "Total Requests: %llu", scheduler->config.total_requests);
    LOG(LOG_LEVEL_INFO, "Dispatched Requests: %llu", scheduler->config.dispatched_requests);
    LOG(LOG_LEVEL_INFO, "Expired Requests: %llu", scheduler->config.expired_requests);
    LOG(LOG_LEVEL_INFO, "Avg Read Latency: %.2f", scheduler->config.avg_read_latency);
    LOG(LOG_LEVEL_INFO, "Avg Write Latency: %.2f", scheduler->config.avg_write_latency);
}

// Demonstration Function
void demonstrate_mq_deadline_scheduler() {
    // Create scheduler
    mq_deadline_scheduler_t *scheduler = create_mq_deadline_scheduler();
    if (!scheduler) {
        LOG(LOG_LEVEL_ERROR, "Failed to create scheduler");
        return;
    }

    // Simulate various I/O workloads
    // Simulating different processes with varying I/O patterns
    
    // High-priority read-heavy workload (e.g., database)
    for (int i = 0; i < 10; i++) {
        io_request_t *req = create_io_request(
            1000,                   // Database PID
            IO_TYPE_READ,           // Read type
            IO_PRIO_HIGH,           // High priority
            i * 1000,               // Sector
            64                      // Size
        );
        enqueue_request(scheduler, req);
    }

    // Normal priority write workload (e.g., log writing)
    for (int i = 0; i < 15; i++) {
        io_request_t *req = create_io_request(
            2000,                   // Log Writer PID
            IO_TYPE_WRITE,          // Write type
            IO_PRIO_NORMAL,         // Normal priority
            i * 500,                // Sector
            32                      // Size
        );
        enqueue_request(scheduler, req);
    }

    // Low-priority background workload
    for (int i = 0; i < 5; i++) {
        io_request_t *req = create_io_request(
            3000,                   // Backup Process PID
            IO_TYPE_READ,           // Read type
            IO_PRIO_LOW,            // Low priority
            i * 2000,               // Sector
            128                     // Size
        );
        enqueue_request(scheduler, req);
    }

    // Run simulation
    run_mq_deadline_simulation(scheduler);

    // Cleanup
    destroy_mq_deadline_scheduler(scheduler);
}

// Scheduler Destruction
void destroy_mq_deadline_scheduler(mq_deadline_scheduler_t *scheduler) {
    if (!scheduler) return;

    // Free any remaining requests in FIFO queues
    io_request_t *current_read = scheduler->queue.read_fifo_head;
    while (current_read) {
        io_request_t *next = current_read->next_fifo;
        free(current_read);
        current_read = next;
    }

    io_request_t *current_write = scheduler->queue.write_fifo_head;
    while (current_write) {
        io_request_t *next = current_write->next_fifo;
        free(current_write);
        current_write = next;
    }

    free(scheduler);
}

int main(void) {
    // Set log level for detailed output
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_mq_deadline_scheduler();

    return 0;
}
