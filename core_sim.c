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

// Scheduler Constants
#define MAX_CPUS           16
#define MAX_TASKS          1024
#define MAX_PRIORITY       100
#define MIN_PRIORITY       0
#define DEFAULT_TIMESLICE  100000  // 100ms
#define LOAD_BALANCE_INTERVAL 1000000  // 1s
#define TEST_DURATION      30     // seconds

// Task States
typedef enum {
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_SLEEPING,
    TASK_DEAD
} task_state_t;

// Task Types
typedef enum {
    TASK_NORMAL,
    TASK_RT,
    TASK_IDLE
} task_type_t;

// CPU States
typedef enum {
    CPU_ACTIVE,
    CPU_IDLE,
    CPU_OFFLINE
} cpu_state_t;

// Task Structure
typedef struct task {
    unsigned int id;
    task_type_t type;
    task_state_t state;
    int priority;
    uint64_t runtime;
    uint64_t timeslice;
    uint64_t deadline;
    int cpu;
    struct task *next;
} task_t;

// Run Queue Structure
typedef struct {
    task_t *head;
    size_t count;
    uint64_t total_weight;
    pthread_mutex_t lock;
} run_queue_t;

// CPU Structure
typedef struct {
    unsigned int id;
    cpu_state_t state;
    run_queue_t *queue;
    task_t *current;
    uint64_t idle_time;
    uint64_t busy_time;
    pthread_t thread;
    pthread_mutex_t lock;
} cpu_t;

// Statistics Structure
typedef struct {
    uint64_t total_tasks;
    uint64_t completed_tasks;
    uint64_t migrations;
    uint64_t context_switches;
    uint64_t load_balances;
    double avg_latency;
    double cpu_utilization;
    double load_imbalance;
    double test_duration;
} sched_stats_t;

// Scheduler Manager Structure
typedef struct {
    cpu_t cpus[MAX_CPUS];
    task_t *tasks[MAX_TASKS];
    size_t nr_cpus;
    size_t nr_tasks;
    bool running;
    pthread_mutex_t manager_lock;
    pthread_t load_balancer;
    sched_stats_t stats;
} sched_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_task_state_string(task_state_t state);
const char* get_task_type_string(task_type_t type);
const char* get_cpu_state_string(cpu_state_t state);

sched_manager_t* create_sched_manager(size_t nr_cpus);
void destroy_sched_manager(sched_manager_t *manager);

task_t* create_task(task_type_t type, int priority);
void destroy_task(task_t *task);

void* cpu_thread(void *arg);
void* load_balancer_thread(void *arg);
void schedule_task(sched_manager_t *manager, task_t *task);
void balance_load(sched_manager_t *manager);

void run_test(sched_manager_t *manager);
void calculate_stats(sched_manager_t *manager);
void print_test_stats(sched_manager_t *manager);
void demonstrate_scheduler(void);

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
        case TASK_RUNNING:  return "RUNNING";
        case TASK_READY:    return "READY";
        case TASK_BLOCKED:  return "BLOCKED";
        case TASK_SLEEPING: return "SLEEPING";
        case TASK_DEAD:     return "DEAD";
        default: return "UNKNOWN";
    }
}

const char* get_task_type_string(task_type_t type) {
    switch(type) {
        case TASK_NORMAL: return "NORMAL";
        case TASK_RT:     return "RT";
        case TASK_IDLE:   return "IDLE";
        default: return "UNKNOWN";
    }
}

const char* get_cpu_state_string(cpu_state_t state) {
    switch(state) {
        case CPU_ACTIVE:  return "ACTIVE";
        case CPU_IDLE:    return "IDLE";
        case CPU_OFFLINE: return "OFFLINE";
        default: return "UNKNOWN";
    }
}

// Create Run Queue
run_queue_t* create_run_queue(void) {
    run_queue_t *queue = malloc(sizeof(run_queue_t));
    if (!queue) return NULL;

    queue->head = NULL;
    queue->count = 0;
    queue->total_weight = 0;
    pthread_mutex_init(&queue->lock, NULL);

    return queue;
}

// Create Task
task_t* create_task(task_type_t type, int priority) {
    task_t *task = malloc(sizeof(task_t));
    if (!task) return NULL;

    static unsigned int next_id = 0;
    task->id = next_id++;
    task->type = type;
    task->state = TASK_READY;
    task->priority = priority;
    task->runtime = 0;
    task->timeslice = DEFAULT_TIMESLICE;
    task->deadline = 0;
    task->cpu = -1;
    task->next = NULL;

    return task;
}

// Create Scheduler Manager
sched_manager_t* create_sched_manager(size_t nr_cpus) {
    if (nr_cpus > MAX_CPUS) {
        LOG(LOG_LEVEL_ERROR, "Number of CPUs exceeds maximum");
        return NULL;
    }

    sched_manager_t *manager = malloc(sizeof(sched_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate scheduler manager");
        return NULL;
    }

    // Initialize CPUs
    for (size_t i = 0; i < nr_cpus; i++) {
        manager->cpus[i].id = i;
        manager->cpus[i].state = CPU_ACTIVE;
        manager->cpus[i].queue = create_run_queue();
        if (!manager->cpus[i].queue) {
            // Cleanup and return
            for (size_t j = 0; j < i; j++) {
                pthread_mutex_destroy(&manager->cpus[j].lock);
                free(manager->cpus[j].queue);
            }
            free(manager);
            return NULL;
        }
        manager->cpus[i].current = NULL;
        manager->cpus[i].idle_time = 0;
        manager->cpus[i].busy_time = 0;
        pthread_mutex_init(&manager->cpus[i].lock, NULL);
    }

    manager->nr_cpus = nr_cpus;
    manager->nr_tasks = 0;
    manager->running = false;
    pthread_mutex_init(&manager->manager_lock, NULL);
    memset(&manager->stats, 0, sizeof(sched_stats_t));

    LOG(LOG_LEVEL_DEBUG, "Created scheduler manager with %zu CPUs", nr_cpus);
    return manager;
}

// CPU Thread
void* cpu_thread(void *arg) {
    cpu_t *cpu = (cpu_t*)arg;
    sched_manager_t *manager = (sched_manager_t*)((void**)arg)[1];

    while (manager->running) {
        pthread_mutex_lock(&cpu->lock);

        if (cpu->current) {
            // Process current task
            task_t *task = cpu->current;
            
            // Simulate task execution
            usleep(task->timeslice);
            task->runtime += task->timeslice;
            cpu->busy_time += task->timeslice;

            // Check if task is complete
            if (task->runtime >= task->deadline) {
                task->state = TASK_DEAD;
                cpu->current = NULL;
                manager->stats.completed_tasks++;
            } else {
                // Put task back in queue
                task->state = TASK_READY;
                pthread_mutex_lock(&cpu->queue->lock);
                task->next = cpu->queue->head;
                cpu->queue->head = task;
                cpu->queue->count++;
                pthread_mutex_unlock(&cpu->queue->lock);
                cpu->current = NULL;
            }

            manager->stats.context_switches++;
        }

        // Get next task
        pthread_mutex_lock(&cpu->queue->lock);
        if (cpu->queue->head) {
            cpu->current = cpu->queue->head;
            cpu->queue->head = cpu->queue->head->next;
            cpu->queue->count--;
            cpu->current->state = TASK_RUNNING;
            cpu->current->next = NULL;
            cpu->state = CPU_ACTIVE;
        } else {
            cpu->state = CPU_IDLE;
            cpu->idle_time += DEFAULT_TIMESLICE;
            usleep(DEFAULT_TIMESLICE);
        }
        pthread_mutex_unlock(&cpu->queue->lock);

        pthread_mutex_unlock(&cpu->lock);
    }

    return NULL;
}

// Load Balancer Thread
void* load_balancer_thread(void *arg) {
    sched_manager_t *manager = (sched_manager_t*)arg;

    while (manager->running) {
        balance_load(manager);
        usleep(LOAD_BALANCE_INTERVAL);
    }

    return NULL;
}

// Schedule Task
void schedule_task(sched_manager_t *manager, task_t *task) {
    if (!manager || !task) return;

    // Find least loaded CPU
    size_t target_cpu = 0;
    size_t min_tasks = UINT64_MAX;

    for (size_t i = 0; i < manager->nr_cpus; i++) {
        pthread_mutex_lock(&manager->cpus[i].queue->lock);
        if (manager->cpus[i].queue->count < min_tasks) {
            min_tasks = manager->cpus[i].queue->count;
            target_cpu = i;
        }
        pthread_mutex_unlock(&manager->cpus[i].queue->lock);
    }

    // Assign task to CPU
    cpu_t *cpu = &manager->cpus[target_cpu];
    pthread_mutex_lock(&cpu->queue->lock);
    task->cpu = target_cpu;
    task->next = cpu->queue->head;
    cpu->queue->head = task;
    cpu->queue->count++;
    pthread_mutex_unlock(&cpu->queue->lock);

    LOG(LOG_LEVEL_DEBUG, "Scheduled task %u (type: %s) to CPU %zu",
        task->id, get_task_type_string(task->type), target_cpu);
}

// Balance Load
void balance_load(sched_manager_t *manager) {
    if (!manager) return;

    // Find most and least loaded CPUs
    size_t max_cpu = 0, min_cpu = 0;
    size_t max_tasks = 0, min_tasks = UINT64_MAX;

    for (size_t i = 0; i < manager->nr_cpus; i++) {
        pthread_mutex_lock(&manager->cpus[i].queue->lock);
        size_t count = manager->cpus[i].queue->count;
        if (count > max_tasks) {
            max_tasks = count;
            max_cpu = i;
        }
        if (count < min_tasks) {
            min_tasks = count;
            min_cpu = i;
        }
        pthread_mutex_unlock(&manager->cpus[i].queue->lock);
    }

    // Balance if necessary
    if (max_tasks - min_tasks > 1) {
        cpu_t *src_cpu = &manager->cpus[max_cpu];
        cpu_t *dst_cpu = &manager->cpus[min_cpu];

        pthread_mutex_lock(&src_cpu->queue->lock);
        pthread_mutex_lock(&dst_cpu->queue->lock);

        // Move task from most loaded to least loaded CPU
        if (src_cpu->queue->head) {
            task_t *task = src_cpu->queue->head;
            src_cpu->queue->head = task->next;
            src_cpu->queue->count--;

            task->next = dst_cpu->queue->head;
            dst_cpu->queue->head = task;
            dst_cpu->queue->count++;
            task->cpu = min_cpu;

            manager->stats.migrations++;
        }

        pthread_mutex_unlock(&dst_cpu->queue->lock);
        pthread_mutex_unlock(&src_cpu->queue->lock);

        manager->stats.load_balances++;
    }
}

// Run Test
void run_test(sched_manager_t *manager) {
    if (!manager) return;

    LOG(LOG_LEVEL_INFO, "Starting scheduler test...");

    // Create initial tasks
    for (size_t i = 0; i < MAX_TASKS/2; i++) {
        task_t *task = create_task(TASK_NORMAL, rand() % MAX_PRIORITY);
        if (task) {
            task->deadline = (rand() % 10 + 1) * DEFAULT_TIMESLICE;
            schedule_task(manager, task);
            manager->tasks[manager->nr_tasks++] = task;
            manager->stats.total_tasks++;
        }
    }

    // Start CPU threads
    manager->running = true;
    void *thread_args[2];
    thread_args[1] = manager;

    for (size_t i = 0; i < manager->nr_cpus; i++) {
        thread_args[0] = &manager->cpus[i];
        pthread_create(&manager->cpus[i].thread, NULL, cpu_thread, thread_args);
    }

    // Start load balancer
    pthread_create(&manager->load_balancer, NULL, load_balancer_thread, manager);

    // Run test
    time_t start_time = time(NULL);
    while (time(NULL) - start_time < TEST_DURATION) {
        // Periodically add new tasks
        if (manager->nr_tasks < MAX_TASKS && rand() % 100 < 10) {
            task_t *task = create_task(
                rand() % 10 == 0 ? TASK_RT : TASK_NORMAL,
                rand() % MAX_PRIORITY
            );
            if (task) {
                task->deadline = (rand() % 10 + 1) * DEFAULT_TIMESLICE;
                schedule_task(manager, task);
                manager->tasks[manager->nr_tasks++] = task;
                manager->stats.total_tasks++;
            }
        }
        usleep(100000);  // 100ms
    }

    // Stop threads
    manager->running = false;

    // Wait for threads to finish
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        pthread_join(manager->cpus[i].thread, NULL);
    }
    pthread_join(manager->load_balancer, NULL);

    // Calculate statistics
    calculate_stats(manager);
}

// Calculate Statistics
void calculate_stats(sched_manager_t *manager) {
    if (!manager) return;

    uint64_t total_runtime = 0;
    uint64_t total_latency = 0;
    uint64_t total_busy_time = 0;

    for (size_t i = 0; i < manager->nr_tasks; i++) {
        if (manager->tasks[i]) {
            total_runtime += manager->tasks[i]->runtime;
            total_latency += manager->tasks[i]->runtime - manager->tasks[i]->deadline;
        }
    }

    for (size_t i = 0; i < manager->nr_cpus; i++) {
        total_busy_time += manager->cpus[i].busy_time;
    }

    if (manager->stats.completed_tasks > 0) {
        manager->stats.avg_latency = 
            (double)total_latency / manager->stats.completed_tasks;
    }

    manager->stats.cpu_utilization = 
        (double)total_busy_time / (TEST_DURATION * 1000000 * manager->nr_cpus);

    // Calculate load imbalance
    size_t max_queue = 0, min_queue = UINT64_MAX;
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        size_t count = manager->cpus[i].queue->count;
        if (count > max_queue) max_queue = count;
        if (count < min_queue) min_queue = count;
    }
    manager->stats.load_imbalance = max_queue - min_queue;

    manager->stats.test_duration = TEST_DURATION;
}

// Print Test Statistics
void print_test_stats(sched_manager_t *manager) {
    if (!manager) return;

    printf("\nScheduler Test Results:\n");
    printf("-------------------------\n");
    printf("Test Duration:      %.2f seconds\n", manager->stats.test_duration);
    printf("Total Tasks:        %lu\n", manager->stats.total_tasks);
    printf("Completed Tasks:    %lu\n", manager->stats.completed_tasks);
    printf("Context Switches:   %lu\n", manager->stats.context_switches);
    printf("Task Migrations:    %lu\n", manager->stats.migrations);
    printf("Load Balances:      %lu\n", manager->stats.load_balances);
    printf("Avg Task Latency:   %.2f us\n", manager->stats.avg_latency);
    printf("CPU Utilization:    %.2f%%\n", manager->stats.cpu_utilization * 100);
    printf("Load Imbalance:     %.2f\n", manager->stats.load_imbalance);

    // Print CPU details
    printf("\nCPU Details:\n");
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        cpu_t *cpu = &manager->cpus[i];
        printf("  CPU %zu:\n", i);
        printf("    State:     %s\n", get_cpu_state_string(cpu->state));
        printf("    Queue Size: %zu\n", cpu->queue->count);
        printf("    Busy Time:  %lu us\n", cpu->busy_time);
        printf("    Idle Time:  %lu us\n", cpu->idle_time);
    }
}

// Destroy Task
void destroy_task(task_t *task) {
    free(task);
}

// Destroy Run Queue
void destroy_run_queue(run_queue_t *queue) {
    if (!queue) return;

    task_t *current = queue->head;
    while (current) {
        task_t *next = current->next;
        destroy_task(current);
        current = next;
    }

    pthread_mutex_destroy(&queue->lock);
    free(queue);
}

// Destroy Scheduler Manager
void destroy_sched_manager(sched_manager_t *manager) {
    if (!manager) return;

    // Clean up tasks
    for (size_t i = 0; i < manager->nr_tasks; i++) {
        destroy_task(manager->tasks[i]);
    }

    // Clean up CPUs
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        pthread_mutex_destroy(&manager->cpus[i].lock);
        destroy_run_queue(manager->cpus[i].queue);
    }

    pthread_mutex_destroy(&manager->manager_lock);
    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed scheduler manager");
}

// Demonstrate Scheduler
void demonstrate_scheduler(void) {
    printf("Starting scheduler demonstration...\n");

    // Create and run scheduler test
    sched_manager_t *manager = create_sched_manager(8);
    if (manager) {
        run_test(manager);
        print_test_stats(manager);
        destroy_sched_manager(manager);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_scheduler();

    return 0;
}
