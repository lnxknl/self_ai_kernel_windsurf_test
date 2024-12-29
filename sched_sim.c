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

// Process States
typedef enum {
    TASK_RUNNING,
    TASK_INTERRUPTIBLE,
    TASK_UNINTERRUPTIBLE,
    TASK_STOPPED,
    TASK_ZOMBIE
} task_state_t;

// Scheduling Policies
typedef enum {
    SCHED_FIFO,
    SCHED_RR,
    SCHED_NORMAL,
    SCHED_BATCH,
    SCHED_IDLE
} sched_policy_t;

// Process Priority Ranges
#define MAX_PRIO        140
#define DEFAULT_PRIO    120
#define MIN_PRIO        100

// Time Slice Configuration
#define DEFAULT_TIMESLICE_MS 100
#define MIN_TIMESLICE_MS     10
#define MAX_TIMESLICE_MS     200

// Process Structure
typedef struct task_struct {
    pid_t pid;
    char name[32];
    task_state_t state;
    sched_policy_t policy;
    int priority;
    int static_priority;
    int dynamic_priority;
    unsigned long runtime;
    unsigned long deadline;
    unsigned long timeslice;
    time_t last_run;
    struct task_struct *next;
} task_t;

// Run Queue Structure
typedef struct {
    task_t *tasks[MAX_PRIO];
    unsigned int bitmap[MAX_PRIO / 32 + 1];
    size_t nr_running;
    pthread_mutex_t lock;
} runqueue_t;

// CPU Structure
typedef struct {
    unsigned int id;
    runqueue_t *rq;
    task_t *curr;
    unsigned long clock;
    bool online;
} cpu_t;

// Scheduler Statistics
typedef struct {
    unsigned long context_switches;
    unsigned long migrations;
    unsigned long load_balance_calls;
    unsigned long tasks_created;
    unsigned long tasks_destroyed;
    double avg_latency;
} sched_stats_t;

// Scheduler Configuration
typedef struct {
    unsigned int nr_cpus;
    bool load_balance;
    unsigned int balance_interval;
    bool track_stats;
} sched_config_t;

// Scheduler Manager
typedef struct {
    cpu_t *cpus;
    size_t nr_cpus;
    sched_config_t config;
    sched_stats_t stats;
    pthread_mutex_t manager_lock;
    pthread_t balance_thread;
    bool running;
} sched_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_task_state_string(task_state_t state);
const char* get_sched_policy_string(sched_policy_t policy);

sched_manager_t* create_sched_manager(sched_config_t config);
void destroy_sched_manager(sched_manager_t *manager);

task_t* create_task(
    pid_t pid,
    const char *name,
    sched_policy_t policy,
    int priority
);

void destroy_task(task_t *task);
bool enqueue_task(cpu_t *cpu, task_t *task);
task_t* dequeue_task(cpu_t *cpu);
task_t* pick_next_task(cpu_t *cpu);

void schedule(cpu_t *cpu);
void load_balance(sched_manager_t *manager);
void* load_balancer(void *arg);
void print_sched_stats(sched_manager_t *manager);
void demonstrate_scheduler(void);

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

// Utility Function: Get Task State String
const char* get_task_state_string(task_state_t state) {
    switch(state) {
        case TASK_RUNNING:        return "RUNNING";
        case TASK_INTERRUPTIBLE:  return "INTERRUPTIBLE";
        case TASK_UNINTERRUPTIBLE: return "UNINTERRUPTIBLE";
        case TASK_STOPPED:        return "STOPPED";
        case TASK_ZOMBIE:         return "ZOMBIE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Scheduling Policy String
const char* get_sched_policy_string(sched_policy_t policy) {
    switch(policy) {
        case SCHED_FIFO:    return "FIFO";
        case SCHED_RR:      return "ROUND_ROBIN";
        case SCHED_NORMAL:  return "NORMAL";
        case SCHED_BATCH:   return "BATCH";
        case SCHED_IDLE:    return "IDLE";
        default: return "UNKNOWN";
    }
}

// Create Scheduler Manager
sched_manager_t* create_sched_manager(sched_config_t config) {
    sched_manager_t *manager = malloc(sizeof(sched_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate scheduler manager");
        return NULL;
    }

    manager->cpus = calloc(config.nr_cpus, sizeof(cpu_t));
    if (!manager->cpus) {
        free(manager);
        return NULL;
    }

    // Initialize CPUs
    for (unsigned int i = 0; i < config.nr_cpus; i++) {
        manager->cpus[i].id = i;
        manager->cpus[i].rq = calloc(1, sizeof(runqueue_t));
        if (!manager->cpus[i].rq) {
            for (unsigned int j = 0; j < i; j++) {
                free(manager->cpus[j].rq);
            }
            free(manager->cpus);
            free(manager);
            return NULL;
        }
        pthread_mutex_init(&manager->cpus[i].rq->lock, NULL);
        manager->cpus[i].curr = NULL;
        manager->cpus[i].clock = 0;
        manager->cpus[i].online = true;
    }

    manager->nr_cpus = config.nr_cpus;
    manager->config = config;
    memset(&manager->stats, 0, sizeof(sched_stats_t));
    pthread_mutex_init(&manager->manager_lock, NULL);
    manager->running = true;

    // Start load balancer thread if enabled
    if (config.load_balance) {
        pthread_create(&manager->balance_thread, NULL, load_balancer, manager);
    }

    LOG(LOG_LEVEL_DEBUG, "Created scheduler manager with %u CPUs", config.nr_cpus);
    return manager;
}

// Create Task
task_t* create_task(
    pid_t pid,
    const char *name,
    sched_policy_t policy,
    int priority
) {
    task_t *task = malloc(sizeof(task_t));
    if (!task) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate task");
        return NULL;
    }

    task->pid = pid;
    strncpy(task->name, name, sizeof(task->name) - 1);
    task->state = TASK_RUNNING;
    task->policy = policy;
    task->priority = priority;
    task->static_priority = priority;
    task->dynamic_priority = priority;
    task->runtime = 0;
    task->deadline = 0;
    task->timeslice = DEFAULT_TIMESLICE_MS;
    task->last_run = time(NULL);
    task->next = NULL;

    LOG(LOG_LEVEL_DEBUG, "Created task %s (PID: %d, Policy: %s, Priority: %d)",
        name, pid, get_sched_policy_string(policy), priority);
    return task;
}

// Enqueue Task
bool enqueue_task(cpu_t *cpu, task_t *task) {
    if (!cpu || !task) return false;

    pthread_mutex_lock(&cpu->rq->lock);

    // Add task to appropriate priority queue
    task->next = cpu->rq->tasks[task->priority];
    cpu->rq->tasks[task->priority] = task;
    cpu->rq->bitmap[task->priority / 32] |= (1 << (task->priority % 32));
    cpu->rq->nr_running++;

    pthread_mutex_unlock(&cpu->rq->lock);

    LOG(LOG_LEVEL_DEBUG, "Enqueued task %s on CPU %u", task->name, cpu->id);
    return true;
}

// Dequeue Task
task_t* dequeue_task(cpu_t *cpu) {
    if (!cpu) return NULL;

    pthread_mutex_lock(&cpu->rq->lock);

    // Find highest priority task
    task_t *task = NULL;
    for (int prio = 0; prio < MAX_PRIO; prio++) {
        if (cpu->rq->bitmap[prio / 32] & (1 << (prio % 32))) {
            task = cpu->rq->tasks[prio];
            cpu->rq->tasks[prio] = task->next;
            if (!cpu->rq->tasks[prio]) {
                cpu->rq->bitmap[prio / 32] &= ~(1 << (prio % 32));
            }
            cpu->rq->nr_running--;
            break;
        }
    }

    pthread_mutex_unlock(&cpu->rq->lock);

    if (task) {
        task->next = NULL;
        LOG(LOG_LEVEL_DEBUG, "Dequeued task %s from CPU %u", 
            task->name, cpu->id);
    }

    return task;
}

// Pick Next Task
task_t* pick_next_task(cpu_t *cpu) {
    if (!cpu) return NULL;

    task_t *next = dequeue_task(cpu);
    if (next) {
        // Update task timings
        next->last_run = time(NULL);
        
        // Reset timeslice based on policy
        switch (next->policy) {
            case SCHED_FIFO:
                next->timeslice = MAX_TIMESLICE_MS;
                break;
            case SCHED_RR:
                next->timeslice = DEFAULT_TIMESLICE_MS;
                break;
            case SCHED_NORMAL:
                next->timeslice = DEFAULT_TIMESLICE_MS * 
                    (140 - next->priority) / 40;
                break;
            case SCHED_BATCH:
                next->timeslice = MAX_TIMESLICE_MS;
                break;
            case SCHED_IDLE:
                next->timeslice = MIN_TIMESLICE_MS;
                break;
        }
    }

    return next;
}

// Schedule
void schedule(cpu_t *cpu) {
    if (!cpu) return;

    // Get next task
    task_t *prev = cpu->curr;
    task_t *next = pick_next_task(cpu);

    if (prev) {
        // Update runtime statistics
        prev->runtime += time(NULL) - prev->last_run;
        
        // Handle task based on state
        if (prev->state == TASK_RUNNING) {
            enqueue_task(cpu, prev);
        }
    }

    // Switch to next task
    cpu->curr = next;
    cpu->clock++;

    LOG(LOG_LEVEL_DEBUG, "CPU %u switched from %s to %s",
        cpu->id,
        prev ? prev->name : "idle",
        next ? next->name : "idle");
}

// Load Balance
void load_balance(sched_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    if (manager->config.track_stats)
        manager->stats.load_balance_calls++;

    // Simple load balancing: move tasks from busy to idle CPUs
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        cpu_t *src_cpu = &manager->cpus[i];
        if (!src_cpu->online) continue;

        pthread_mutex_lock(&src_cpu->rq->lock);
        size_t src_load = src_cpu->rq->nr_running;
        pthread_mutex_unlock(&src_cpu->rq->lock);

        if (src_load <= 1) continue;

        // Find least loaded CPU
        cpu_t *dst_cpu = NULL;
        size_t min_load = SIZE_MAX;

        for (size_t j = 0; j < manager->nr_cpus; j++) {
            if (i == j) continue;

            cpu_t *cpu = &manager->cpus[j];
            if (!cpu->online) continue;

            pthread_mutex_lock(&cpu->rq->lock);
            size_t load = cpu->rq->nr_running;
            pthread_mutex_unlock(&cpu->rq->lock);

            if (load < min_load) {
                min_load = load;
                dst_cpu = cpu;
            }
        }

        // Migrate task if beneficial
        if (dst_cpu && min_load + 1 < src_load) {
            task_t *task = dequeue_task(src_cpu);
            if (task) {
                enqueue_task(dst_cpu, task);
                if (manager->config.track_stats)
                    manager->stats.migrations++;
            }
        }
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Load Balancer Thread
void* load_balancer(void *arg) {
    sched_manager_t *manager = (sched_manager_t*)arg;

    while (manager->running) {
        load_balance(manager);
        usleep(manager->config.balance_interval * 1000);
    }

    return NULL;
}

// Print Scheduler Statistics
void print_sched_stats(sched_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    printf("\nScheduler Statistics:\n");
    printf("-------------------\n");
    printf("Context Switches:    %lu\n", manager->stats.context_switches);
    printf("Task Migrations:     %lu\n", manager->stats.migrations);
    printf("Load Balance Calls:  %lu\n", manager->stats.load_balance_calls);
    printf("Tasks Created:       %lu\n", manager->stats.tasks_created);
    printf("Tasks Destroyed:     %lu\n", manager->stats.tasks_destroyed);
    printf("Average Latency:     %.2f ms\n", manager->stats.avg_latency);

    // Print CPU details
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        cpu_t *cpu = &manager->cpus[i];
        printf("\nCPU %zu:\n", i);
        printf("  Status:    %s\n", cpu->online ? "Online" : "Offline");
        printf("  Current:   %s\n", cpu->curr ? cpu->curr->name : "idle");
        printf("  Clock:     %lu\n", cpu->clock);
        
        pthread_mutex_lock(&cpu->rq->lock);
        printf("  Run Queue: %zu tasks\n", cpu->rq->nr_running);
        pthread_mutex_unlock(&cpu->rq->lock);
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Destroy Task
void destroy_task(task_t *task) {
    if (!task) return;
    free(task);
}

// Destroy Scheduler Manager
void destroy_sched_manager(sched_manager_t *manager) {
    if (!manager) return;

    // Stop load balancer thread
    manager->running = false;
    if (manager->config.load_balance) {
        pthread_join(manager->balance_thread, NULL);
    }

    pthread_mutex_lock(&manager->manager_lock);

    // Clean up CPUs
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        cpu_t *cpu = &manager->cpus[i];
        
        // Clean up runqueue
        pthread_mutex_lock(&cpu->rq->lock);
        for (int prio = 0; prio < MAX_PRIO; prio++) {
            task_t *task = cpu->rq->tasks[prio];
            while (task) {
                task_t *next = task->next;
                destroy_task(task);
                task = next;
            }
        }
        pthread_mutex_unlock(&cpu->rq->lock);
        pthread_mutex_destroy(&cpu->rq->lock);
        free(cpu->rq);
    }

    free(manager->cpus);

    pthread_mutex_unlock(&manager->manager_lock);
    pthread_mutex_destroy(&manager->manager_lock);

    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed scheduler manager");
}

// Demonstrate Scheduler
void demonstrate_scheduler(void) {
    // Create scheduler configuration
    sched_config_t config = {
        .nr_cpus = 4,
        .load_balance = true,
        .balance_interval = 100,
        .track_stats = true
    };

    // Create scheduler manager
    sched_manager_t *manager = create_sched_manager(config);
    if (!manager) return;

    // Create sample tasks
    const char *task_names[] = {
        "system_daemon",
        "user_process",
        "batch_job",
        "background_task"
    };
    
    sched_policy_t policies[] = {
        SCHED_FIFO,
        SCHED_RR,
        SCHED_NORMAL,
        SCHED_BATCH
    };
    
    int priorities[] = {110, 120, 130, 140};

    for (int i = 0; i < 4; i++) {
        task_t *task = create_task(i + 1, task_names[i], policies[i], 
            priorities[i]);
        if (task) {
            enqueue_task(&manager->cpus[i % manager->nr_cpus], task);
            manager->stats.tasks_created++;
        }
    }

    // Simulate scheduling
    for (int i = 0; i < 10; i++) {
        for (size_t cpu = 0; cpu < manager->nr_cpus; cpu++) {
            schedule(&manager->cpus[cpu]);
            manager->stats.context_switches++;
        }
        usleep(100000);  // 100ms delay
    }

    // Print statistics
    print_sched_stats(manager);

    // Cleanup
    destroy_sched_manager(manager);
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
