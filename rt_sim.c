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

// RT Scheduler Constants
#define MAX_CPUS           16
#define MAX_RT_TASKS       256
#define RT_PRIO_LEVELS     100
#define MAX_RUNTIME        1000000  // 1s
#define MIN_RUNTIME        1000     // 1ms
#define BANDWIDTH_CAP      950000   // 95% bandwidth cap
#define TEST_DURATION      30       // seconds

// RT Task States
typedef enum {
    RT_RUNNING,
    RT_READY,
    RT_BLOCKED,
    RT_SLEEPING,
    RT_DEAD
} rt_state_t;

// RT Task Types
typedef enum {
    RT_FIFO,
    RT_RR,
    RT_DEADLINE
} rt_type_t;

// RT Task Structure
typedef struct rt_task {
    unsigned int id;
    rt_type_t type;
    rt_state_t state;
    int priority;
    uint64_t period;
    uint64_t deadline;
    uint64_t runtime;
    uint64_t actual_runtime;
    uint64_t start_time;
    uint64_t completion_time;
    int cpu;
    bool throttled;
    struct rt_task *next;
} rt_task_t;

// RT Run Queue Structure
typedef struct {
    rt_task_t *tasks[RT_PRIO_LEVELS];
    size_t nr_tasks;
    uint64_t total_runtime;
    pthread_mutex_t lock;
} rt_rq_t;

// CPU Structure
typedef struct {
    unsigned int id;
    rt_rq_t *rt_rq;
    rt_task_t *current;
    uint64_t rt_runtime;
    uint64_t rt_period;
    bool rt_throttled;
    pthread_t thread;
    pthread_mutex_t lock;
} rt_cpu_t;

// RT Statistics Structure
typedef struct {
    uint64_t total_tasks;
    uint64_t completed_tasks;
    uint64_t missed_deadlines;
    uint64_t migrations;
    uint64_t preemptions;
    uint64_t throttles;
    double avg_response_time;
    double cpu_utilization;
    double bandwidth_usage;
    double test_duration;
} rt_stats_t;

// RT Scheduler Manager Structure
typedef struct {
    rt_cpu_t cpus[MAX_CPUS];
    rt_task_t *tasks[MAX_RT_TASKS];
    size_t nr_cpus;
    size_t nr_tasks;
    bool running;
    pthread_mutex_t manager_lock;
    pthread_t bandwidth_thread;
    rt_stats_t stats;
} rt_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_rt_state_string(rt_state_t state);
const char* get_rt_type_string(rt_type_t type);

rt_manager_t* create_rt_manager(size_t nr_cpus);
void destroy_rt_manager(rt_manager_t *manager);

rt_task_t* create_rt_task(rt_type_t type, int priority);
void destroy_rt_task(rt_task_t *task);

void* cpu_thread(void *arg);
void* bandwidth_thread(void *arg);
void schedule_rt_task(rt_manager_t *manager, rt_task_t *task);
void check_bandwidth(rt_manager_t *manager);

void run_test(rt_manager_t *manager);
void calculate_stats(rt_manager_t *manager);
void print_test_stats(rt_manager_t *manager);
void demonstrate_rt_scheduler(void);

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

const char* get_rt_state_string(rt_state_t state) {
    switch(state) {
        case RT_RUNNING:  return "RUNNING";
        case RT_READY:    return "READY";
        case RT_BLOCKED:  return "BLOCKED";
        case RT_SLEEPING: return "SLEEPING";
        case RT_DEAD:     return "DEAD";
        default: return "UNKNOWN";
    }
}

const char* get_rt_type_string(rt_type_t type) {
    switch(type) {
        case RT_FIFO:     return "FIFO";
        case RT_RR:       return "RR";
        case RT_DEADLINE: return "DEADLINE";
        default: return "UNKNOWN";
    }
}

// Create RT Run Queue
rt_rq_t* create_rt_rq(void) {
    rt_rq_t *rq = malloc(sizeof(rt_rq_t));
    if (!rq) return NULL;

    memset(rq->tasks, 0, sizeof(rq->tasks));
    rq->nr_tasks = 0;
    rq->total_runtime = 0;
    pthread_mutex_init(&rq->lock, NULL);

    return rq;
}

// Create RT Task
rt_task_t* create_rt_task(rt_type_t type, int priority) {
    if (priority >= RT_PRIO_LEVELS) return NULL;

    rt_task_t *task = malloc(sizeof(rt_task_t));
    if (!task) return NULL;

    static unsigned int next_id = 0;
    task->id = next_id++;
    task->type = type;
    task->state = RT_READY;
    task->priority = priority;
    task->period = (type == RT_DEADLINE) ? 
        (rand() % (MAX_RUNTIME - MIN_RUNTIME) + MIN_RUNTIME) : 0;
    task->deadline = (type == RT_DEADLINE) ? 
        task->period : MAX_RUNTIME;
    task->runtime = (rand() % (MAX_RUNTIME/10)) + MIN_RUNTIME;
    task->actual_runtime = 0;
    task->start_time = 0;
    task->completion_time = 0;
    task->cpu = -1;
    task->throttled = false;
    task->next = NULL;

    return task;
}

// Create RT Manager
rt_manager_t* create_rt_manager(size_t nr_cpus) {
    if (nr_cpus > MAX_CPUS) {
        LOG(LOG_LEVEL_ERROR, "Number of CPUs exceeds maximum");
        return NULL;
    }

    rt_manager_t *manager = malloc(sizeof(rt_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate RT manager");
        return NULL;
    }

    // Initialize CPUs
    for (size_t i = 0; i < nr_cpus; i++) {
        manager->cpus[i].id = i;
        manager->cpus[i].rt_rq = create_rt_rq();
        if (!manager->cpus[i].rt_rq) {
            // Cleanup and return
            for (size_t j = 0; j < i; j++) {
                pthread_mutex_destroy(&manager->cpus[j].lock);
                free(manager->cpus[j].rt_rq);
            }
            free(manager);
            return NULL;
        }
        manager->cpus[i].current = NULL;
        manager->cpus[i].rt_runtime = 0;
        manager->cpus[i].rt_period = MAX_RUNTIME;
        manager->cpus[i].rt_throttled = false;
        pthread_mutex_init(&manager->cpus[i].lock, NULL);
    }

    manager->nr_cpus = nr_cpus;
    manager->nr_tasks = 0;
    manager->running = false;
    pthread_mutex_init(&manager->manager_lock, NULL);
    memset(&manager->stats, 0, sizeof(rt_stats_t));

    LOG(LOG_LEVEL_DEBUG, "Created RT manager with %zu CPUs", nr_cpus);
    return manager;
}

// CPU Thread
void* cpu_thread(void *arg) {
    rt_cpu_t *cpu = (rt_cpu_t*)arg;
    rt_manager_t *manager = (rt_manager_t*)((void**)arg)[1];

    while (manager->running) {
        pthread_mutex_lock(&cpu->lock);

        if (cpu->rt_throttled) {
            pthread_mutex_unlock(&cpu->lock);
            usleep(1000);  // Wait if throttled
            continue;
        }

        if (cpu->current) {
            // Process current task
            rt_task_t *task = cpu->current;
            
            // Simulate task execution
            usleep(task->runtime);
            task->actual_runtime += task->runtime;
            cpu->rt_runtime += task->runtime;

            // Check if task is complete
            if (task->actual_runtime >= task->runtime) {
                task->completion_time = time(NULL);
                if (task->completion_time - task->start_time > task->deadline) {
                    manager->stats.missed_deadlines++;
                }
                task->state = RT_DEAD;
                cpu->current = NULL;
                manager->stats.completed_tasks++;
            } else if (task->type == RT_RR) {
                // Round Robin time slice expired
                task->state = RT_READY;
                pthread_mutex_lock(&cpu->rt_rq->lock);
                task->next = cpu->rt_rq->tasks[task->priority];
                cpu->rt_rq->tasks[task->priority] = task;
                cpu->rt_rq->nr_tasks++;
                pthread_mutex_unlock(&cpu->rt_rq->lock);
                cpu->current = NULL;
                manager->stats.preemptions++;
            }
        }

        // Get next task
        pthread_mutex_lock(&cpu->rt_rq->lock);
        rt_task_t *next_task = NULL;
        for (int prio = 0; prio < RT_PRIO_LEVELS; prio++) {
            if (cpu->rt_rq->tasks[prio]) {
                next_task = cpu->rt_rq->tasks[prio];
                cpu->rt_rq->tasks[prio] = next_task->next;
                cpu->rt_rq->nr_tasks--;
                next_task->next = NULL;
                break;
            }
        }
        pthread_mutex_unlock(&cpu->rt_rq->lock);

        if (next_task) {
            cpu->current = next_task;
            next_task->state = RT_RUNNING;
            next_task->start_time = time(NULL);
        } else {
            usleep(1000);  // Idle
        }

        pthread_mutex_unlock(&cpu->lock);
    }

    return NULL;
}

// Bandwidth Thread
void* bandwidth_thread(void *arg) {
    rt_manager_t *manager = (rt_manager_t*)arg;

    while (manager->running) {
        check_bandwidth(manager);
        usleep(10000);  // Check every 10ms
    }

    return NULL;
}

// Check Bandwidth
void check_bandwidth(rt_manager_t *manager) {
    if (!manager) return;

    for (size_t i = 0; i < manager->nr_cpus; i++) {
        rt_cpu_t *cpu = &manager->cpus[i];
        pthread_mutex_lock(&cpu->lock);

        // Check if CPU exceeded bandwidth cap
        if (cpu->rt_runtime > BANDWIDTH_CAP) {
            if (!cpu->rt_throttled) {
                cpu->rt_throttled = true;
                manager->stats.throttles++;
                LOG(LOG_LEVEL_WARN, "CPU %zu throttled: runtime %lu > cap %d",
                    i, cpu->rt_runtime, BANDWIDTH_CAP);
            }
        } else if (cpu->rt_throttled) {
            cpu->rt_throttled = false;
            LOG(LOG_LEVEL_INFO, "CPU %zu unthrottled", i);
        }

        // Reset runtime counter periodically
        if (cpu->rt_runtime > cpu->rt_period) {
            cpu->rt_runtime = 0;
        }

        pthread_mutex_unlock(&cpu->lock);
    }
}

// Schedule RT Task
void schedule_rt_task(rt_manager_t *manager, rt_task_t *task) {
    if (!manager || !task) return;

    // Find least loaded CPU
    size_t target_cpu = 0;
    size_t min_tasks = UINT64_MAX;

    for (size_t i = 0; i < manager->nr_cpus; i++) {
        pthread_mutex_lock(&manager->cpus[i].rt_rq->lock);
        if (manager->cpus[i].rt_rq->nr_tasks < min_tasks) {
            min_tasks = manager->cpus[i].rt_rq->nr_tasks;
            target_cpu = i;
        }
        pthread_mutex_unlock(&manager->cpus[i].rt_rq->lock);
    }

    // Assign task to CPU
    rt_cpu_t *cpu = &manager->cpus[target_cpu];
    pthread_mutex_lock(&cpu->rt_rq->lock);
    task->cpu = target_cpu;
    task->next = cpu->rt_rq->tasks[task->priority];
    cpu->rt_rq->tasks[task->priority] = task;
    cpu->rt_rq->nr_tasks++;
    pthread_mutex_unlock(&cpu->rt_rq->lock);

    LOG(LOG_LEVEL_DEBUG, "Scheduled RT task %u (type: %s, prio: %d) to CPU %zu",
        task->id, get_rt_type_string(task->type), task->priority, target_cpu);
}

// Run Test
void run_test(rt_manager_t *manager) {
    if (!manager) return;

    LOG(LOG_LEVEL_INFO, "Starting RT scheduler test...");

    // Create initial tasks
    for (size_t i = 0; i < MAX_RT_TASKS/2; i++) {
        rt_task_t *task = create_rt_task(
            rand() % 3,  // Random task type
            rand() % RT_PRIO_LEVELS
        );
        if (task) {
            schedule_rt_task(manager, task);
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

    // Start bandwidth monitor
    pthread_create(&manager->bandwidth_thread, NULL, bandwidth_thread, manager);

    // Run test
    time_t start_time = time(NULL);
    while (time(NULL) - start_time < TEST_DURATION) {
        // Periodically add new tasks
        if (manager->nr_tasks < MAX_RT_TASKS && rand() % 100 < 10) {
            rt_task_t *task = create_rt_task(
                rand() % 3,
                rand() % RT_PRIO_LEVELS
            );
            if (task) {
                schedule_rt_task(manager, task);
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
    pthread_join(manager->bandwidth_thread, NULL);

    // Calculate statistics
    calculate_stats(manager);
}

// Calculate Statistics
void calculate_stats(rt_manager_t *manager) {
    if (!manager) return;

    uint64_t total_response_time = 0;
    uint64_t total_runtime = 0;

    for (size_t i = 0; i < manager->nr_tasks; i++) {
        if (manager->tasks[i] && manager->tasks[i]->completion_time > 0) {
            total_response_time += 
                manager->tasks[i]->completion_time - manager->tasks[i]->start_time;
            total_runtime += manager->tasks[i]->actual_runtime;
        }
    }

    if (manager->stats.completed_tasks > 0) {
        manager->stats.avg_response_time = 
            (double)total_response_time / manager->stats.completed_tasks;
    }

    manager->stats.cpu_utilization = 
        (double)total_runtime / (TEST_DURATION * 1000000 * manager->nr_cpus);

    // Calculate bandwidth usage
    uint64_t total_bandwidth = 0;
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        total_bandwidth += manager->cpus[i].rt_runtime;
    }
    manager->stats.bandwidth_usage = 
        (double)total_bandwidth / (TEST_DURATION * 1000000 * manager->nr_cpus);

    manager->stats.test_duration = TEST_DURATION;
}

// Print Test Statistics
void print_test_stats(rt_manager_t *manager) {
    if (!manager) return;

    printf("\nRT Scheduler Test Results:\n");
    printf("-------------------------\n");
    printf("Test Duration:      %.2f seconds\n", manager->stats.test_duration);
    printf("Total Tasks:        %lu\n", manager->stats.total_tasks);
    printf("Completed Tasks:    %lu\n", manager->stats.completed_tasks);
    printf("Missed Deadlines:   %lu\n", manager->stats.missed_deadlines);
    printf("Task Migrations:    %lu\n", manager->stats.migrations);
    printf("Preemptions:        %lu\n", manager->stats.preemptions);
    printf("Throttle Events:    %lu\n", manager->stats.throttles);
    printf("Avg Response Time:  %.2f ms\n", manager->stats.avg_response_time);
    printf("CPU Utilization:    %.2f%%\n", manager->stats.cpu_utilization * 100);
    printf("Bandwidth Usage:    %.2f%%\n", manager->stats.bandwidth_usage * 100);

    // Print CPU details
    printf("\nCPU Details:\n");
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        rt_cpu_t *cpu = &manager->cpus[i];
        printf("  CPU %zu:\n", i);
        printf("    Tasks:      %zu\n", cpu->rt_rq->nr_tasks);
        printf("    Runtime:    %lu us\n", cpu->rt_runtime);
        printf("    Throttled:  %s\n", cpu->rt_throttled ? "Yes" : "No");
    }
}

// Destroy RT Run Queue
void destroy_rt_rq(rt_rq_t *rq) {
    if (!rq) return;

    for (int i = 0; i < RT_PRIO_LEVELS; i++) {
        rt_task_t *current = rq->tasks[i];
        while (current) {
            rt_task_t *next = current->next;
            free(current);
            current = next;
        }
    }

    pthread_mutex_destroy(&rq->lock);
    free(rq);
}

// Destroy RT Task
void destroy_rt_task(rt_task_t *task) {
    free(task);
}

// Destroy RT Manager
void destroy_rt_manager(rt_manager_t *manager) {
    if (!manager) return;

    // Clean up tasks
    for (size_t i = 0; i < manager->nr_tasks; i++) {
        destroy_rt_task(manager->tasks[i]);
    }

    // Clean up CPUs
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        pthread_mutex_destroy(&manager->cpus[i].lock);
        destroy_rt_rq(manager->cpus[i].rt_rq);
    }

    pthread_mutex_destroy(&manager->manager_lock);
    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed RT manager");
}

// Demonstrate RT Scheduler
void demonstrate_rt_scheduler(void) {
    printf("Starting RT scheduler demonstration...\n");

    // Create and run RT scheduler test
    rt_manager_t *manager = create_rt_manager(8);
    if (manager) {
        run_test(manager);
        print_test_stats(manager);
        destroy_rt_manager(manager);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_rt_scheduler();

    return 0;
}
