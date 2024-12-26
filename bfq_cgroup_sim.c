#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

// Simulated Process and Cgroup Structures
typedef struct process {
    int pid;
    int priority;
    int time_slice;
    int remaining_budget;
    struct process *next;
} process_t;

typedef struct cgroup {
    char name[64];
    int weight;
    int total_budget;
    int current_budget;
    process_t *processes;
    struct cgroup *next;
} cgroup_t;

// Global Cgroup Management
typedef struct {
    cgroup_t *groups;
    int total_weight;
} cgroup_scheduler_t;

// Function Prototypes
cgroup_scheduler_t* create_cgroup_scheduler();
cgroup_t* create_cgroup(cgroup_scheduler_t *scheduler, const char *name, int weight);
void add_process_to_cgroup(cgroup_t *cgroup, int pid, int priority);
void distribute_budget(cgroup_scheduler_t *scheduler);
void run_scheduler_simulation(cgroup_scheduler_t *scheduler);
void free_scheduler(cgroup_scheduler_t *scheduler);

// Create Cgroup Scheduler
cgroup_scheduler_t* create_cgroup_scheduler() {
    cgroup_scheduler_t *scheduler = malloc(sizeof(cgroup_scheduler_t));
    if (!scheduler) {
        fprintf(stderr, "Memory allocation failed for scheduler\n");
        return NULL;
    }
    
    scheduler->groups = NULL;
    scheduler->total_weight = 0;
    return scheduler;
}

// Create a New Cgroup
cgroup_t* create_cgroup(cgroup_scheduler_t *scheduler, const char *name, int weight) {
    if (!scheduler || weight <= 0) {
        return NULL;
    }

    cgroup_t *new_group = malloc(sizeof(cgroup_t));
    if (!new_group) {
        fprintf(stderr, "Memory allocation failed for cgroup\n");
        return NULL;
    }

    strncpy(new_group->name, name, sizeof(new_group->name) - 1);
    new_group->weight = weight;
    new_group->total_budget = 0;
    new_group->current_budget = 0;
    new_group->processes = NULL;
    
    // Link to scheduler
    new_group->next = scheduler->groups;
    scheduler->groups = new_group;
    scheduler->total_weight += weight;

    return new_group;
}

// Add Process to Cgroup
void add_process_to_cgroup(cgroup_t *cgroup, int pid, int priority) {
    if (!cgroup) return;

    process_t *new_process = malloc(sizeof(process_t));
    if (!new_process) {
        fprintf(stderr, "Memory allocation failed for process\n");
        return;
    }

    new_process->pid = pid;
    new_process->priority = priority;
    new_process->time_slice = 100 / priority;  // Simplified time slice
    new_process->remaining_budget = new_process->time_slice;
    
    // Add to cgroup's process list
    new_process->next = cgroup->processes;
    cgroup->processes = new_process;
}

// Distribute Budget Across Cgroups
void distribute_budget(cgroup_scheduler_t *scheduler) {
    int total_budget = 1000;  // Total system budget
    cgroup_t *current_group = scheduler->groups;

    // Reset budgets
    while (current_group) {
        current_group->current_budget = 
            (current_group->weight * total_budget) / scheduler->total_weight;
        current_group = current_group->next;
    }
}

// Run Scheduler Simulation
void run_scheduler_simulation(cgroup_scheduler_t *scheduler) {
    int total_time = 500;  // Total simulation time
    int current_time = 0;

    printf("Starting Cgroup Scheduler Simulation\n");
    printf("-------------------------------------\n");

    while (current_time < total_time) {
        cgroup_t *current_group = scheduler->groups;
        
        // Distribute budget at the start
        if (current_time == 0) {
            distribute_budget(scheduler);
        }

        // Round-robin scheduling within each cgroup
        while (current_group) {
            process_t *current_process = current_group->processes;
            process_t *prev_process = NULL;

            while (current_process) {
                // Check if cgroup has budget
                if (current_group->current_budget > 0) {
                    int execution_time = (current_process->remaining_budget < current_group->current_budget) 
                        ? current_process->remaining_budget 
                        : current_group->current_budget;

                    printf("Time %d: Group %s, PID %d executed for %d time units\n", 
                           current_time, current_group->name, 
                           current_process->pid, execution_time);

                    current_process->remaining_budget -= execution_time;
                    current_group->current_budget -= execution_time;

                    // Reset process budget if depleted
                    if (current_process->remaining_budget <= 0) {
                        current_process->remaining_budget = current_process->time_slice;
                    }
                }

                // Move to next process
                prev_process = current_process;
                current_process = current_process->next;

                // Wrap around to first process if needed
                if (!current_process && prev_process) {
                    current_process = current_group->processes;
                }
            }

            current_group = current_group->next;
        }

        current_time++;
    }
}

// Free Scheduler Resources
void free_scheduler(cgroup_scheduler_t *scheduler) {
    if (!scheduler) return;

    cgroup_t *current_group = scheduler->groups;
    while (current_group) {
        cgroup_t *next_group = current_group->next;
        
        // Free processes in the cgroup
        process_t *current_process = current_group->processes;
        while (current_process) {
            process_t *next_process = current_process->next;
            free(current_process);
            current_process = next_process;
        }

        free(current_group);
        current_group = next_group;
    }

    free(scheduler);
}

// Demonstration Function
void demonstrate_cgroup_scheduler() {
    // Create scheduler
    cgroup_scheduler_t *scheduler = create_cgroup_scheduler();
    if (!scheduler) {
        fprintf(stderr, "Failed to create scheduler\n");
        return;
    }

    // Create cgroups
    cgroup_t *web_group = create_cgroup(scheduler, "web", 50);
    cgroup_t *db_group = create_cgroup(scheduler, "database", 30);
    cgroup_t *batch_group = create_cgroup(scheduler, "batch", 20);

    // Add processes to cgroups
    add_process_to_cgroup(web_group, 101, 5);
    add_process_to_cgroup(web_group, 102, 3);
    
    add_process_to_cgroup(db_group, 201, 4);
    add_process_to_cgroup(db_group, 202, 2);
    
    add_process_to_cgroup(batch_group, 301, 1);
    add_process_to_cgroup(batch_group, 302, 1);

    // Run simulation
    run_scheduler_simulation(scheduler);

    // Clean up
    free_scheduler(scheduler);
}

int main(void) {
    demonstrate_cgroup_scheduler();
    return 0;
}
