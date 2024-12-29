#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>

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

// CFS Constants
#define MIN_GRANULARITY   1000000    // 1ms in ns
#define TARGET_LATENCY    20000000   // 20ms in ns
#define MINIMUM_TIMESLICE 1000000    // 1ms in ns
#define DEFAULT_WEIGHT    1024
#define NICE_0_LOAD      1024

// Process States
typedef enum {
    TASK_RUNNING,
    TASK_INTERRUPTIBLE,
    TASK_UNINTERRUPTIBLE,
    TASK_STOPPED,
    TASK_TRACED
} task_state_t;

// Red-Black Tree Colors
typedef enum {
    RB_RED,
    RB_BLACK
} rb_color_t;

// Red-Black Tree Node
typedef struct rb_node {
    struct rb_node *parent;
    struct rb_node *left;
    struct rb_node *right;
    rb_color_t color;
    uint64_t key;
} rb_node_t;

// Process Structure
typedef struct task_struct {
    pid_t pid;
    char name[32];
    task_state_t state;
    int nice;
    unsigned int weight;
    uint64_t vruntime;
    uint64_t exec_start;
    uint64_t sum_exec_runtime;
    uint64_t prev_sum_exec_runtime;
    rb_node_t node;
    struct task_struct *next;
} task_t;

// CFS Run Queue
typedef struct {
    rb_node_t *rb_root;
    task_t *curr;
    unsigned int nr_running;
    uint64_t min_vruntime;
    uint64_t clock;
    pthread_mutex_t lock;
} cfs_rq_t;

// CPU Structure
typedef struct {
    unsigned int id;
    cfs_rq_t *cfs;
    uint64_t clock;
    bool online;
} cpu_t;

// CFS Statistics
typedef struct {
    unsigned long context_switches;
    unsigned long migrations;
    unsigned long load_balance_calls;
    unsigned long tasks_created;
    unsigned long tasks_destroyed;
    double avg_wait_time;
    double avg_runtime;
} cfs_stats_t;

// CFS Configuration
typedef struct {
    unsigned int nr_cpus;
    bool load_balance;
    unsigned int balance_interval;
    bool track_stats;
} cfs_config_t;

// CFS Manager
typedef struct {
    cpu_t *cpus;
    size_t nr_cpus;
    cfs_config_t config;
    cfs_stats_t stats;
    pthread_mutex_t manager_lock;
    pthread_t balance_thread;
    bool running;
} cfs_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_task_state_string(task_state_t state);

cfs_manager_t* create_cfs_manager(cfs_config_t config);
void destroy_cfs_manager(cfs_manager_t *manager);

task_t* create_task(pid_t pid, const char *name, int nice);
void destroy_task(task_t *task);

void rb_insert(cfs_rq_t *cfs, task_t *task);
void rb_erase(cfs_rq_t *cfs, task_t *task);
task_t* rb_leftmost(rb_node_t *node);
void rb_replace_node(cfs_rq_t *cfs, rb_node_t *old, rb_node_t *new);

void update_curr(cfs_rq_t *cfs, uint64_t now);
void update_min_vruntime(cfs_rq_t *cfs);
uint64_t calc_delta_fair(uint64_t delta, unsigned int weight);
task_t* pick_next_task(cfs_rq_t *cfs);

void enqueue_task(cpu_t *cpu, task_t *task);
void dequeue_task(cpu_t *cpu, task_t *task);
void schedule(cpu_t *cpu);

void load_balance(cfs_manager_t *manager);
void* load_balancer(void *arg);
void print_cfs_stats(cfs_manager_t *manager);
void demonstrate_cfs(void);

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
        case TASK_TRACED:         return "TRACED";
        default: return "UNKNOWN";
    }
}

// Create CFS Manager
cfs_manager_t* create_cfs_manager(cfs_config_t config) {
    cfs_manager_t *manager = malloc(sizeof(cfs_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate CFS manager");
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
        manager->cpus[i].cfs = calloc(1, sizeof(cfs_rq_t));
        if (!manager->cpus[i].cfs) {
            for (unsigned int j = 0; j < i; j++) {
                free(manager->cpus[j].cfs);
            }
            free(manager->cpus);
            free(manager);
            return NULL;
        }
        pthread_mutex_init(&manager->cpus[i].cfs->lock, NULL);
        manager->cpus[i].clock = 0;
        manager->cpus[i].online = true;
    }

    manager->nr_cpus = config.nr_cpus;
    manager->config = config;
    memset(&manager->stats, 0, sizeof(cfs_stats_t));
    pthread_mutex_init(&manager->manager_lock, NULL);
    manager->running = true;

    // Start load balancer thread if enabled
    if (config.load_balance) {
        pthread_create(&manager->balance_thread, NULL, load_balancer, manager);
    }

    LOG(LOG_LEVEL_DEBUG, "Created CFS manager with %u CPUs", config.nr_cpus);
    return manager;
}

// Create Task
task_t* create_task(pid_t pid, const char *name, int nice) {
    task_t *task = malloc(sizeof(task_t));
    if (!task) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate task");
        return NULL;
    }

    task->pid = pid;
    strncpy(task->name, name, sizeof(task->name) - 1);
    task->state = TASK_RUNNING;
    task->nice = nice;
    task->weight = NICE_0_LOAD - nice * 100;
    task->vruntime = 0;
    task->exec_start = 0;
    task->sum_exec_runtime = 0;
    task->prev_sum_exec_runtime = 0;
    task->next = NULL;

    memset(&task->node, 0, sizeof(rb_node_t));
    task->node.color = RB_RED;

    LOG(LOG_LEVEL_DEBUG, "Created task %s (PID: %d, Nice: %d)",
        name, pid, nice);
    return task;
}

// Red-Black Tree Operations
void rb_insert(cfs_rq_t *cfs, task_t *task) {
    rb_node_t **new = &cfs->rb_root;
    rb_node_t *parent = NULL;

    while (*new) {
        parent = *new;
        task_t *entry = container_of(parent, task_t, node);

        if (task->vruntime < entry->vruntime)
            new = &parent->left;
        else
            new = &parent->right;
    }

    task->node.parent = parent;
    task->node.left = NULL;
    task->node.right = NULL;
    task->node.color = RB_RED;
    *new = &task->node;

    // Rebalance tree
    rb_node_t *node = &task->node;
    while (node != cfs->rb_root && node->parent->color == RB_RED) {
        if (node->parent == node->parent->parent->left) {
            rb_node_t *uncle = node->parent->parent->right;
            if (uncle && uncle->color == RB_RED) {
                node->parent->color = RB_BLACK;
                uncle->color = RB_BLACK;
                node->parent->parent->color = RB_RED;
                node = node->parent->parent;
            } else {
                if (node == node->parent->right) {
                    node = node->parent;
                    // Left rotate
                }
                node->parent->color = RB_BLACK;
                node->parent->parent->color = RB_RED;
                // Right rotate
            }
        } else {
            // Mirror case
        }
    }
    cfs->rb_root->color = RB_BLACK;
}

// Update Current Task
void update_curr(cfs_rq_t *cfs, uint64_t now) {
    task_t *curr = cfs->curr;
    if (!curr)
        return;

    uint64_t delta_exec = now - curr->exec_start;
    curr->sum_exec_runtime += delta_exec;
    curr->vruntime += calc_delta_fair(delta_exec, curr->weight);
    curr->exec_start = now;

    update_min_vruntime(cfs);
}

// Calculate Fair Delta
uint64_t calc_delta_fair(uint64_t delta, unsigned int weight) {
    uint64_t fact = NICE_0_LOAD;
    uint64_t delta_fair = (delta * fact) / weight;
    return delta_fair;
}

// Update Minimum Virtual Runtime
void update_min_vruntime(cfs_rq_t *cfs) {
    task_t *leftmost = rb_leftmost(cfs->rb_root);
    if (leftmost)
        cfs->min_vruntime = leftmost->vruntime;
}

// Pick Next Task
task_t* pick_next_task(cfs_rq_t *cfs) {
    if (!cfs->rb_root)
        return NULL;

    task_t *next = rb_leftmost(cfs->rb_root);
    if (!next)
        return NULL;

    rb_erase(cfs, next);
    return next;
}

// Enqueue Task
void enqueue_task(cpu_t *cpu, task_t *task) {
    if (!cpu || !task) return;

    pthread_mutex_lock(&cpu->cfs->lock);

    task->exec_start = cpu->clock;
    if (task->vruntime < cpu->cfs->min_vruntime)
        task->vruntime = cpu->cfs->min_vruntime;

    rb_insert(cpu->cfs, task);
    cpu->cfs->nr_running++;

    pthread_mutex_unlock(&cpu->cfs->lock);

    LOG(LOG_LEVEL_DEBUG, "Enqueued task %s on CPU %u", task->name, cpu->id);
}

// Dequeue Task
void dequeue_task(cpu_t *cpu, task_t *task) {
    if (!cpu || !task) return;

    pthread_mutex_lock(&cpu->cfs->lock);

    rb_erase(cpu->cfs, task);
    cpu->cfs->nr_running--;

    pthread_mutex_unlock(&cpu->cfs->lock);

    LOG(LOG_LEVEL_DEBUG, "Dequeued task %s from CPU %u", task->name, cpu->id);
}

// Schedule
void schedule(cpu_t *cpu) {
    if (!cpu) return;

    pthread_mutex_lock(&cpu->cfs->lock);

    // Update current task
    update_curr(cpu->cfs, cpu->clock);

    // Get next task
    task_t *prev = cpu->cfs->curr;
    task_t *next = pick_next_task(cpu->cfs);

    if (prev) {
        // Re-enqueue previous task if still runnable
        if (prev->state == TASK_RUNNING)
            rb_insert(cpu->cfs, prev);
    }

    // Switch to next task
    cpu->cfs->curr = next;
    if (next)
        next->exec_start = cpu->clock;

    cpu->clock += MINIMUM_TIMESLICE;

    pthread_mutex_unlock(&cpu->cfs->lock);

    LOG(LOG_LEVEL_DEBUG, "CPU %u switched from %s to %s",
        cpu->id,
        prev ? prev->name : "idle",
        next ? next->name : "idle");
}

// Load Balance
void load_balance(cfs_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    if (manager->config.track_stats)
        manager->stats.load_balance_calls++;

    // Simple load balancing: move tasks from busy to idle CPUs
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        cpu_t *src_cpu = &manager->cpus[i];
        if (!src_cpu->online) continue;

        pthread_mutex_lock(&src_cpu->cfs->lock);
        unsigned int src_load = src_cpu->cfs->nr_running;
        pthread_mutex_unlock(&src_cpu->cfs->lock);

        if (src_load <= 1) continue;

        // Find least loaded CPU
        cpu_t *dst_cpu = NULL;
        unsigned int min_load = UINT_MAX;

        for (size_t j = 0; j < manager->nr_cpus; j++) {
            if (i == j) continue;

            cpu_t *cpu = &manager->cpus[j];
            if (!cpu->online) continue;

            pthread_mutex_lock(&cpu->cfs->lock);
            unsigned int load = cpu->cfs->nr_running;
            pthread_mutex_unlock(&cpu->cfs->lock);

            if (load < min_load) {
                min_load = load;
                dst_cpu = cpu;
            }
        }

        // Migrate task if beneficial
        if (dst_cpu && min_load + 1 < src_load) {
            task_t *task = pick_next_task(src_cpu->cfs);
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
    cfs_manager_t *manager = (cfs_manager_t*)arg;

    while (manager->running) {
        load_balance(manager);
        usleep(manager->config.balance_interval * 1000);
    }

    return NULL;
}

// Print CFS Statistics
void print_cfs_stats(cfs_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    printf("\nCFS Statistics:\n");
    printf("--------------\n");
    printf("Context Switches:    %lu\n", manager->stats.context_switches);
    printf("Task Migrations:     %lu\n", manager->stats.migrations);
    printf("Load Balance Calls:  %lu\n", manager->stats.load_balance_calls);
    printf("Tasks Created:       %lu\n", manager->stats.tasks_created);
    printf("Tasks Destroyed:     %lu\n", manager->stats.tasks_destroyed);
    printf("Average Wait Time:   %.2f ms\n", manager->stats.avg_wait_time);
    printf("Average Runtime:     %.2f ms\n", manager->stats.avg_runtime);

    // Print CPU details
    for (size_t i = 0; i < manager->nr_cpus; i++) {
        cpu_t *cpu = &manager->cpus[i];
        printf("\nCPU %zu:\n", i);
        printf("  Status:    %s\n", cpu->online ? "Online" : "Offline");
        printf("  Clock:     %lu ns\n", cpu->clock);
        
        pthread_mutex_lock(&cpu->cfs->lock);
        printf("  Tasks:     %u\n", cpu->cfs->nr_running);
        printf("  Current:   %s\n", 
            cpu->cfs->curr ? cpu->cfs->curr->name : "idle");
        pthread_mutex_unlock(&cpu->cfs->lock);
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Destroy Task
void destroy_task(task_t *task) {
    if (!task) return;
    free(task);
}

// Destroy CFS Manager
void destroy_cfs_manager(cfs_manager_t *manager) {
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
        
        pthread_mutex_lock(&cpu->cfs->lock);
        // Clean up tasks in run queue
        while (cpu->cfs->rb_root) {
            task_t *task = pick_next_task(cpu->cfs);
            if (task) destroy_task(task);
        }
        pthread_mutex_unlock(&cpu->cfs->lock);
        
        pthread_mutex_destroy(&cpu->cfs->lock);
        free(cpu->cfs);
    }

    free(manager->cpus);

    pthread_mutex_unlock(&manager->manager_lock);
    pthread_mutex_destroy(&manager->manager_lock);

    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed CFS manager");
}

// Demonstrate CFS
void demonstrate_cfs(void) {
    // Create CFS configuration
    cfs_config_t config = {
        .nr_cpus = 4,
        .load_balance = true,
        .balance_interval = 100,
        .track_stats = true
    };

    // Create CFS manager
    cfs_manager_t *manager = create_cfs_manager(config);
    if (!manager) return;

    // Create sample tasks
    const char *task_names[] = {
        "system_daemon",
        "user_process",
        "batch_job",
        "background_task",
        "interactive_app",
        "compiler",
        "video_encoder",
        "database"
    };
    
    int nice_values[] = {-20, -10, 0, 10, 0, 0, -5, 5};

    for (int i = 0; i < 8; i++) {
        task_t *task = create_task(i + 1, task_names[i], nice_values[i]);
        if (task) {
            enqueue_task(&manager->cpus[i % manager->nr_cpus], task);
            manager->stats.tasks_created++;
        }
    }

    // Simulate scheduling
    for (int i = 0; i < 20; i++) {
        for (size_t cpu = 0; cpu < manager->nr_cpus; cpu++) {
            schedule(&manager->cpus[cpu]);
            manager->stats.context_switches++;
        }
        usleep(50000);  // 50ms delay
    }

    // Print statistics
    print_cfs_stats(manager);

    // Cleanup
    destroy_cfs_manager(manager);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_cfs();

    return 0;
}
