#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
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
    PROC_STATE_RUNNING,
    PROC_STATE_SLEEPING,
    PROC_STATE_DISK_SLEEP,
    PROC_STATE_STOPPED,
    PROC_STATE_ZOMBIE
} proc_state_t;

// Memory Statistics
typedef struct {
    size_t virtual_size;
    size_t resident_size;
    size_t shared_size;
    size_t text_size;
    size_t data_size;
} mem_stats_t;

// CPU Statistics
typedef struct {
    unsigned long user_time;
    unsigned long system_time;
    unsigned long guest_time;
    unsigned long io_wait;
    float cpu_usage;
} cpu_stats_t;

// IO Statistics
typedef struct {
    unsigned long read_bytes;
    unsigned long write_bytes;
    unsigned long read_ops;
    unsigned long write_ops;
} io_stats_t;

// Process Information
typedef struct process_info {
    pid_t pid;
    pid_t ppid;
    char name[256];
    char cmdline[1024];
    proc_state_t state;
    
    mem_stats_t mem;
    cpu_stats_t cpu;
    io_stats_t io;
    
    time_t start_time;
    uid_t uid;
    gid_t gid;
    
    struct process_info *next;
} process_info_t;

// Process Manager Configuration
typedef struct {
    size_t max_processes;
    bool track_memory;
    bool track_cpu;
    bool track_io;
    unsigned int update_interval;
} proc_config_t;

// Process Manager Statistics
typedef struct {
    unsigned long total_processes;
    unsigned long running_processes;
    unsigned long sleeping_processes;
    unsigned long zombie_processes;
    double avg_cpu_usage;
    size_t total_memory_used;
} proc_stats_t;

// Process Information Manager
typedef struct {
    process_info_t *process_list;
    proc_config_t config;
    proc_stats_t stats;
    
    pthread_mutex_t proc_lock;
    bool running;
} proc_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_proc_state_string(proc_state_t state);

proc_manager_t* create_proc_manager(proc_config_t config);
void destroy_proc_manager(proc_manager_t *manager);

process_info_t* create_process_info(
    pid_t pid,
    pid_t ppid,
    const char *name,
    const char *cmdline
);

bool add_process(proc_manager_t *manager, process_info_t *process);
bool remove_process(proc_manager_t *manager, pid_t pid);
process_info_t* find_process(proc_manager_t *manager, pid_t pid);

void update_process_stats(proc_manager_t *manager, process_info_t *process);
void print_process_info(process_info_t *process);
void print_proc_stats(proc_manager_t *manager);

void simulate_process_activity(proc_manager_t *manager);
void demonstrate_proc_manager(void);

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

// Utility Function: Get Process State String
const char* get_proc_state_string(proc_state_t state) {
    switch(state) {
        case PROC_STATE_RUNNING:     return "RUNNING";
        case PROC_STATE_SLEEPING:    return "SLEEPING";
        case PROC_STATE_DISK_SLEEP:  return "DISK_SLEEP";
        case PROC_STATE_STOPPED:     return "STOPPED";
        case PROC_STATE_ZOMBIE:      return "ZOMBIE";
        default: return "UNKNOWN";
    }
}

// Create Process Manager
proc_manager_t* create_proc_manager(proc_config_t config) {
    proc_manager_t *manager = malloc(sizeof(proc_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate process manager");
        return NULL;
    }

    manager->process_list = NULL;
    manager->config = config;
    memset(&manager->stats, 0, sizeof(proc_stats_t));
    
    pthread_mutex_init(&manager->proc_lock, NULL);
    manager->running = true;

    LOG(LOG_LEVEL_DEBUG, "Created process manager");
    return manager;
}

// Create Process Information
process_info_t* create_process_info(
    pid_t pid,
    pid_t ppid,
    const char *name,
    const char *cmdline
) {
    process_info_t *process = malloc(sizeof(process_info_t));
    if (!process) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate process info");
        return NULL;
    }

    process->pid = pid;
    process->ppid = ppid;
    strncpy(process->name, name, sizeof(process->name) - 1);
    strncpy(process->cmdline, cmdline, sizeof(process->cmdline) - 1);
    
    process->state = PROC_STATE_RUNNING;
    memset(&process->mem, 0, sizeof(mem_stats_t));
    memset(&process->cpu, 0, sizeof(cpu_stats_t));
    memset(&process->io, 0, sizeof(io_stats_t));
    
    process->start_time = time(NULL);
    process->uid = getuid();
    process->gid = getgid();
    
    process->next = NULL;

    LOG(LOG_LEVEL_DEBUG, "Created process info for PID %d", pid);
    return process;
}

// Add Process to Manager
bool add_process(proc_manager_t *manager, process_info_t *process) {
    if (!manager || !process) return false;

    pthread_mutex_lock(&manager->proc_lock);

    if (manager->stats.total_processes >= manager->config.max_processes) {
        pthread_mutex_unlock(&manager->proc_lock);
        return false;
    }

    process->next = manager->process_list;
    manager->process_list = process;
    manager->stats.total_processes++;

    if (process->state == PROC_STATE_RUNNING)
        manager->stats.running_processes++;
    else if (process->state == PROC_STATE_SLEEPING)
        manager->stats.sleeping_processes++;
    else if (process->state == PROC_STATE_ZOMBIE)
        manager->stats.zombie_processes++;

    pthread_mutex_unlock(&manager->proc_lock);

    LOG(LOG_LEVEL_DEBUG, "Added process %d to manager", process->pid);
    return true;
}

// Remove Process from Manager
bool remove_process(proc_manager_t *manager, pid_t pid) {
    if (!manager) return false;

    pthread_mutex_lock(&manager->proc_lock);

    process_info_t *current = manager->process_list;
    process_info_t *prev = NULL;

    while (current) {
        if (current->pid == pid) {
            if (prev)
                prev->next = current->next;
            else
                manager->process_list = current->next;

            manager->stats.total_processes--;
            
            if (current->state == PROC_STATE_RUNNING)
                manager->stats.running_processes--;
            else if (current->state == PROC_STATE_SLEEPING)
                manager->stats.sleeping_processes--;
            else if (current->state == PROC_STATE_ZOMBIE)
                manager->stats.zombie_processes--;

            free(current);
            pthread_mutex_unlock(&manager->proc_lock);
            
            LOG(LOG_LEVEL_DEBUG, "Removed process %d from manager", pid);
            return true;
        }

        prev = current;
        current = current->next;
    }

    pthread_mutex_unlock(&manager->proc_lock);
    return false;
}

// Find Process in Manager
process_info_t* find_process(proc_manager_t *manager, pid_t pid) {
    if (!manager) return NULL;

    pthread_mutex_lock(&manager->proc_lock);

    process_info_t *current = manager->process_list;
    while (current) {
        if (current->pid == pid) {
            pthread_mutex_unlock(&manager->proc_lock);
            return current;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&manager->proc_lock);
    return NULL;
}

// Update Process Statistics
void update_process_stats(proc_manager_t *manager, process_info_t *process) {
    if (!manager || !process) return;

    pthread_mutex_lock(&manager->proc_lock);

    // Simulate CPU usage
    process->cpu.user_time += rand() % 100;
    process->cpu.system_time += rand() % 50;
    process->cpu.cpu_usage = 
        (float)(process->cpu.user_time + process->cpu.system_time) / 100.0f;

    // Simulate memory usage
    process->mem.virtual_size = rand() % (1024 * 1024 * 1024);
    process->mem.resident_size = process->mem.virtual_size / 4;
    process->mem.shared_size = process->mem.resident_size / 8;

    // Simulate I/O operations
    process->io.read_bytes += rand() % 10000;
    process->io.write_bytes += rand() % 5000;
    process->io.read_ops++;
    process->io.write_ops++;

    // Update manager statistics
    manager->stats.avg_cpu_usage = 0;
    manager->stats.total_memory_used = 0;

    process_info_t *current = manager->process_list;
    while (current) {
        manager->stats.avg_cpu_usage += current->cpu.cpu_usage;
        manager->stats.total_memory_used += current->mem.resident_size;
        current = current->next;
    }

    if (manager->stats.total_processes > 0) {
        manager->stats.avg_cpu_usage /= manager->stats.total_processes;
    }

    pthread_mutex_unlock(&manager->proc_lock);

    LOG(LOG_LEVEL_DEBUG, "Updated stats for process %d", process->pid);
}

// Print Process Information
void print_process_info(process_info_t *process) {
    if (!process) return;

    printf("\nProcess Information:\n");
    printf("-------------------\n");
    printf("PID:        %d\n", process->pid);
    printf("PPID:       %d\n", process->ppid);
    printf("Name:       %s\n", process->name);
    printf("Command:    %s\n", process->cmdline);
    printf("State:      %s\n", get_proc_state_string(process->state));
    printf("Start Time: %ld\n", process->start_time);
    printf("\nMemory Usage:\n");
    printf("Virtual:  %zu bytes\n", process->mem.virtual_size);
    printf("Resident: %zu bytes\n", process->mem.resident_size);
    printf("Shared:   %zu bytes\n", process->mem.shared_size);
    printf("\nCPU Usage:\n");
    printf("User:   %lu\n", process->cpu.user_time);
    printf("System: %lu\n", process->cpu.system_time);
    printf("Usage:  %.2f%%\n", process->cpu.cpu_usage * 100);
    printf("\nI/O Statistics:\n");
    printf("Read:  %lu bytes (%lu ops)\n", 
        process->io.read_bytes, process->io.read_ops);
    printf("Write: %lu bytes (%lu ops)\n", 
        process->io.write_bytes, process->io.write_ops);
}

// Print Process Manager Statistics
void print_proc_stats(proc_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->proc_lock);

    printf("\nProcess Manager Statistics:\n");
    printf("-------------------------\n");
    printf("Total Processes:    %lu\n", manager->stats.total_processes);
    printf("Running Processes:  %lu\n", manager->stats.running_processes);
    printf("Sleeping Processes: %lu\n", manager->stats.sleeping_processes);
    printf("Zombie Processes:   %lu\n", manager->stats.zombie_processes);
    printf("Average CPU Usage:  %.2f%%\n", 
        manager->stats.avg_cpu_usage * 100);
    printf("Total Memory Used:  %zu bytes\n", 
        manager->stats.total_memory_used);

    pthread_mutex_unlock(&manager->proc_lock);
}

// Simulate Process Activity
void simulate_process_activity(proc_manager_t *manager) {
    if (!manager) return;

    const char *sample_names[] = {
        "systemd", "bash", "chrome", "firefox", "nginx",
        "mysql", "postgres", "redis", "mongodb", "apache"
    };

    const char *sample_cmdlines[] = {
        "/sbin/init",
        "/bin/bash",
        "/usr/bin/chrome --no-sandbox",
        "/usr/bin/firefox",
        "/usr/sbin/nginx -g 'daemon off;'",
        "/usr/sbin/mysqld",
        "/usr/local/bin/postgres",
        "/usr/local/bin/redis-server",
        "/usr/bin/mongod",
        "/usr/sbin/apache2"
    };

    // Create sample processes
    for (int i = 0; i < 10; i++) {
        process_info_t *process = create_process_info(
            1000 + i,
            1,
            sample_names[i],
            sample_cmdlines[i]
        );

        if (process) {
            add_process(manager, process);
        }
    }

    // Simulate process activity
    for (int i = 0; i < 5; i++) {
        process_info_t *current = manager->process_list;
        while (current) {
            update_process_stats(manager, current);
            
            // Randomly change process state
            if (rand() % 10 == 0) {
                current->state = (proc_state_t)(rand() % 5);
            }
            
            current = current->next;
        }

        // Print current state
        print_proc_stats(manager);
        
        process_info_t *sample = find_process(manager, 1000);
        if (sample) {
            print_process_info(sample);
        }

        sleep(1);
    }
}

// Destroy Process Manager
void destroy_proc_manager(proc_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->proc_lock);

    process_info_t *current = manager->process_list;
    while (current) {
        process_info_t *next = current->next;
        free(current);
        current = next;
    }

    pthread_mutex_unlock(&manager->proc_lock);
    pthread_mutex_destroy(&manager->proc_lock);

    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed process manager");
}

// Demonstrate Process Manager
void demonstrate_proc_manager(void) {
    // Create process manager configuration
    proc_config_t config = {
        .max_processes = 1000,
        .track_memory = true,
        .track_cpu = true,
        .track_io = true,
        .update_interval = 1000
    };

    // Create process manager
    proc_manager_t *manager = create_proc_manager(config);
    if (!manager) return;

    // Run simulation
    simulate_process_activity(manager);

    // Cleanup
    destroy_proc_manager(manager);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_proc_manager();

    return 0;
}
