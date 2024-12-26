#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

// Simulated I/O Request Types
typedef enum {
    IO_READ,
    IO_WRITE
} io_type_t;

// I/O Request Structure
typedef struct io_request {
    int pid;            // Process ID
    io_type_t type;     // Read or Write
    unsigned long sector;  // Starting sector
    size_t size;        // Size of request in sectors
    int priority;       // Request priority
    int wait_time;      // Time spent waiting
    struct io_request *next;
} io_request_t;

// Process/Application Context
typedef struct process_context {
    int pid;
    char name[64];
    int weight;         // Scheduling weight
    int current_budget; // Remaining budget
    int max_budget;     // Maximum budget per scheduling period
    io_request_t *queue;// Queue of I/O requests
    struct process_context *next;
} process_context_t;

// I/O Scheduler Structure
typedef struct {
    process_context_t *processes;
    io_request_t *dispatch_queue;
    int total_weight;
    int total_budget;
    int current_time;
} bfq_scheduler_t;

// Function Prototypes
bfq_scheduler_t* create_bfq_scheduler();
process_context_t* add_process(bfq_scheduler_t *scheduler, const char *name, int weight);
void add_io_request(process_context_t *process, io_type_t type, unsigned long sector, size_t size);
void run_bfq_simulation(bfq_scheduler_t *scheduler);
void free_bfq_scheduler(bfq_scheduler_t *scheduler);

// Create BFQ Scheduler
bfq_scheduler_t* create_bfq_scheduler() {
    bfq_scheduler_t *scheduler = malloc(sizeof(bfq_scheduler_t));
    if (!scheduler) {
        fprintf(stderr, "Memory allocation failed for scheduler\n");
        return NULL;
    }
    
    scheduler->processes = NULL;
    scheduler->dispatch_queue = NULL;
    scheduler->total_weight = 0;
    scheduler->total_budget = 1000;  // Total I/O budget
    scheduler->current_time = 0;
    
    return scheduler;
}

// Add Process to Scheduler
process_context_t* add_process(bfq_scheduler_t *scheduler, const char *name, int weight) {
    if (!scheduler || weight <= 0) {
        return NULL;
    }

    process_context_t *new_process = malloc(sizeof(process_context_t));
    if (!new_process) {
        fprintf(stderr, "Memory allocation failed for process\n");
        return NULL;
    }

    // Generate unique PID
    static int next_pid = 1;
    new_process->pid = next_pid++;
    
    strncpy(new_process->name, name, sizeof(new_process->name) - 1);
    new_process->weight = weight;
    new_process->max_budget = (weight * scheduler->total_budget) / 100;
    new_process->current_budget = new_process->max_budget;
    new_process->queue = NULL;
    
    // Link to scheduler
    new_process->next = scheduler->processes;
    scheduler->processes = new_process;
    scheduler->total_weight += weight;

    return new_process;
}

// Add I/O Request to Process Queue
void add_io_request(process_context_t *process, io_type_t type, unsigned long sector, size_t size) {
    if (!process) return;

    io_request_t *new_request = malloc(sizeof(io_request_t));
    if (!new_request) {
        fprintf(stderr, "Memory allocation failed for I/O request\n");
        return;
    }

    new_request->pid = process->pid;
    new_request->type = type;
    new_request->sector = sector;
    new_request->size = size;
    new_request->priority = 5;  // Default priority
    new_request->wait_time = 0;
    new_request->next = NULL;

    // Add to process's request queue
    if (!process->queue) {
        process->queue = new_request;
    } else {
        // Add to end of queue
        io_request_t *current = process->queue;
        while (current->next) {
            current = current->next;
        }
        current->next = new_request;
    }
}

// Dispatch I/O Requests
void dispatch_io_requests(bfq_scheduler_t *scheduler) {
    process_context_t *current_process = scheduler->processes;
    
    while (current_process) {
        // If process has requests and budget
        if (current_process->queue && current_process->current_budget > 0) {
            io_request_t *request = current_process->queue;
            
            // Simulate I/O dispatch
            int dispatch_cost = request->size / 10;  // Simplified cost calculation
            
            if (dispatch_cost <= current_process->current_budget) {
                printf("Time %d: Dispatching %s request for %s (PID %d): Sector %lu, Size %zu\n", 
                       scheduler->current_time,
                       request->type == IO_READ ? "READ" : "WRITE",
                       current_process->name, 
                       current_process->pid, 
                       request->sector, 
                       request->size);
                
                // Deduct budget
                current_process->current_budget -= dispatch_cost;
                
                // Remove request from queue
                current_process->queue = request->next;
                free(request);
            }
        }
        
        current_process = current_process->next;
    }
}

// Redistribute Budget
void redistribute_budget(bfq_scheduler_t *scheduler) {
    process_context_t *current_process = scheduler->processes;
    
    while (current_process) {
        // Reset budget based on weight
        current_process->current_budget = 
            (current_process->weight * scheduler->total_budget) / 100;
        
        current_process = current_process->next;
    }
}

// Run BFQ Simulation
void run_bfq_simulation(bfq_scheduler_t *scheduler) {
    int simulation_time = 100;
    
    printf("Starting BFQ I/O Scheduler Simulation\n");
    printf("-------------------------------------\n");

    for (scheduler->current_time = 0; 
         scheduler->current_time < simulation_time; 
         scheduler->current_time++) {
        
        // Redistribute budget every 20 time units
        if (scheduler->current_time % 20 == 0) {
            redistribute_budget(scheduler);
        }
        
        // Dispatch I/O requests
        dispatch_io_requests(scheduler);
    }
}

// Free Scheduler Resources
void free_bfq_scheduler(bfq_scheduler_t *scheduler) {
    if (!scheduler) return;

    // Free processes and their request queues
    process_context_t *current_process = scheduler->processes;
    while (current_process) {
        process_context_t *next_process = current_process->next;
        
        // Free I/O request queue
        io_request_t *current_request = current_process->queue;
        while (current_request) {
            io_request_t *next_request = current_request->next;
            free(current_request);
            current_request = next_request;
        }
        
        free(current_process);
        current_process = next_process;
    }

    free(scheduler);
}

// Demonstration Function
void demonstrate_bfq_scheduler() {
    // Create scheduler
    bfq_scheduler_t *scheduler = create_bfq_scheduler();
    if (!scheduler) {
        fprintf(stderr, "Failed to create scheduler\n");
        return;
    }

    // Create processes with different weights
    process_context_t *web_server = add_process(scheduler, "WebServer", 50);
    process_context_t *database = add_process(scheduler, "Database", 30);
    process_context_t *backup = add_process(scheduler, "Backup", 20);

    // Add I/O requests for each process
    // Web Server: Frequent small read/write operations
    add_io_request(web_server, IO_READ, 1000, 10);
    add_io_request(web_server, IO_WRITE, 1100, 15);
    add_io_request(web_server, IO_READ, 1200, 8);

    // Database: Large sequential read/write operations
    add_io_request(database, IO_READ, 5000, 100);
    add_io_request(database, IO_WRITE, 5200, 80);

    // Backup: Large but less frequent operations
    add_io_request(backup, IO_READ, 10000, 200);

    // Run simulation
    run_bfq_simulation(scheduler);

    // Clean up
    free_bfq_scheduler(scheduler);
}

int main(void) {
    demonstrate_bfq_scheduler();
    return 0;
}
