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

// CPU Frequency Constants
#define MAX_CPUS            8
#define MIN_FREQ           800000   // 800 MHz
#define MAX_FREQ          4000000   // 4 GHz
#define FREQ_STEP          100000   // 100 MHz
#define DEFAULT_RATE_LIMIT     5    // 5ms
#define MAX_LOAD            100     // 100%
#define UTIL_THRESHOLD       80     // 80%

// Power States
typedef enum {
    POWER_PERFORMANCE,
    POWER_NORMAL,
    POWER_POWERSAVE,
    POWER_AUTONOMOUS
} power_state_t;

// CPU States
typedef enum {
    CPU_ACTIVE,
    CPU_IDLE,
    CPU_OFFLINE
} cpu_state_t;

// Frequency Statistics
typedef struct {
    unsigned long switches;
    unsigned long up_transitions;
    unsigned long down_transitions;
    double avg_frequency;
    double energy_usage;
    uint64_t time_in_state[40];  // For 40 frequency steps
} freq_stats_t;

// CPU Structure
typedef struct {
    unsigned int id;
    unsigned int curr_freq;
    unsigned int target_freq;
    unsigned int max_freq;
    unsigned int min_freq;
    unsigned int util;
    cpu_state_t state;
    freq_stats_t stats;
    uint64_t last_update;
    pthread_mutex_t lock;
} cpu_t;

// Policy Structure
typedef struct {
    unsigned int up_rate_limit_us;
    unsigned int down_rate_limit_us;
    unsigned int freq_step;
    power_state_t power_state;
    bool boost_enabled;
    double util_threshold;
} policy_t;

// Scheduler Utility Manager
typedef struct {
    cpu_t cpus[MAX_CPUS];
    policy_t policy;
    unsigned int nr_cpus;
    pthread_t monitor_thread;
    bool running;
    pthread_mutex_t manager_lock;
} schedutil_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_power_state_string(power_state_t state);
const char* get_cpu_state_string(cpu_state_t state);

schedutil_t* create_schedutil(unsigned int nr_cpus);
void destroy_schedutil(schedutil_t *util);

void set_power_state(schedutil_t *util, power_state_t state);
void update_cpu_util(schedutil_t *util, unsigned int cpu_id, unsigned int util);
void update_cpu_freq(schedutil_t *util, unsigned int cpu_id);
void* monitor_thread(void *arg);

unsigned int calc_target_freq(schedutil_t *util, cpu_t *cpu);
void apply_freq_constraints(schedutil_t *util, cpu_t *cpu);
void update_freq_stats(cpu_t *cpu, unsigned int old_freq);
void print_schedutil_stats(schedutil_t *util);
void demonstrate_schedutil(void);

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

// Utility Function: Get Power State String
const char* get_power_state_string(power_state_t state) {
    switch(state) {
        case POWER_PERFORMANCE: return "PERFORMANCE";
        case POWER_NORMAL:      return "NORMAL";
        case POWER_POWERSAVE:   return "POWERSAVE";
        case POWER_AUTONOMOUS:  return "AUTONOMOUS";
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

// Create Scheduler Utility
schedutil_t* create_schedutil(unsigned int nr_cpus) {
    if (nr_cpus > MAX_CPUS) {
        LOG(LOG_LEVEL_ERROR, "Number of CPUs exceeds maximum");
        return NULL;
    }

    schedutil_t *util = malloc(sizeof(schedutil_t));
    if (!util) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate schedutil");
        return NULL;
    }

    // Initialize CPUs
    for (unsigned int i = 0; i < nr_cpus; i++) {
        cpu_t *cpu = &util->cpus[i];
        cpu->id = i;
        cpu->curr_freq = MIN_FREQ;
        cpu->target_freq = MIN_FREQ;
        cpu->max_freq = MAX_FREQ;
        cpu->min_freq = MIN_FREQ;
        cpu->util = 0;
        cpu->state = CPU_ACTIVE;
        memset(&cpu->stats, 0, sizeof(freq_stats_t));
        cpu->last_update = 0;
        pthread_mutex_init(&cpu->lock, NULL);
    }

    // Initialize policy
    util->policy.up_rate_limit_us = DEFAULT_RATE_LIMIT * 1000;
    util->policy.down_rate_limit_us = DEFAULT_RATE_LIMIT * 1000;
    util->policy.freq_step = FREQ_STEP;
    util->policy.power_state = POWER_NORMAL;
    util->policy.boost_enabled = true;
    util->policy.util_threshold = UTIL_THRESHOLD;

    util->nr_cpus = nr_cpus;
    pthread_mutex_init(&util->manager_lock, NULL);
    util->running = true;

    // Start monitor thread
    pthread_create(&util->monitor_thread, NULL, monitor_thread, util);

    LOG(LOG_LEVEL_DEBUG, "Created schedutil with %u CPUs", nr_cpus);
    return util;
}

// Set Power State
void set_power_state(schedutil_t *util, power_state_t state) {
    if (!util) return;

    pthread_mutex_lock(&util->manager_lock);
    
    util->policy.power_state = state;
    
    // Adjust policy based on power state
    switch (state) {
        case POWER_PERFORMANCE:
            util->policy.up_rate_limit_us = 1000;    // 1ms
            util->policy.down_rate_limit_us = 5000;  // 5ms
            util->policy.util_threshold = 70;        // More aggressive
            util->policy.boost_enabled = true;
            break;
            
        case POWER_NORMAL:
            util->policy.up_rate_limit_us = 5000;    // 5ms
            util->policy.down_rate_limit_us = 10000; // 10ms
            util->policy.util_threshold = 80;
            util->policy.boost_enabled = true;
            break;
            
        case POWER_POWERSAVE:
            util->policy.up_rate_limit_us = 10000;   // 10ms
            util->policy.down_rate_limit_us = 5000;  // 5ms
            util->policy.util_threshold = 90;        // More conservative
            util->policy.boost_enabled = false;
            break;
            
        case POWER_AUTONOMOUS:
            // Let the hardware decide
            break;
    }

    pthread_mutex_unlock(&util->manager_lock);
    
    LOG(LOG_LEVEL_INFO, "Power state changed to %s", 
        get_power_state_string(state));
}

// Update CPU Utilization
void update_cpu_util(schedutil_t *util, unsigned int cpu_id, unsigned int util) {
    if (!util || cpu_id >= util->nr_cpus) return;

    cpu_t *cpu = &util->cpus[cpu_id];
    pthread_mutex_lock(&cpu->lock);
    
    cpu->util = util > MAX_LOAD ? MAX_LOAD : util;
    
    // Update frequency if needed
    uint64_t now = time(NULL) * 1000000;  // Convert to microseconds
    uint64_t delta = now - cpu->last_update;
    
    if (delta >= util->policy.up_rate_limit_us) {
        update_cpu_freq(util, cpu_id);
        cpu->last_update = now;
    }
    
    pthread_mutex_unlock(&cpu->lock);
}

// Calculate Target Frequency
unsigned int calc_target_freq(schedutil_t *util, cpu_t *cpu) {
    unsigned int target_freq;
    
    // Calculate target frequency based on utilization
    if (cpu->util >= util->policy.util_threshold) {
        // High utilization: scale up frequency
        target_freq = (cpu->util * MAX_FREQ) / MAX_LOAD;
        if (util->policy.boost_enabled)
            target_freq += FREQ_STEP;  // Add boost
    } else {
        // Low utilization: scale down frequency
        target_freq = ((cpu->util + 10) * MAX_FREQ) / MAX_LOAD;
    }
    
    // Round to nearest frequency step
    target_freq = (target_freq / FREQ_STEP) * FREQ_STEP;
    
    return target_freq;
}

// Apply Frequency Constraints
void apply_freq_constraints(schedutil_t *util, cpu_t *cpu) {
    // Apply hardware limits
    if (cpu->target_freq > cpu->max_freq)
        cpu->target_freq = cpu->max_freq;
    if (cpu->target_freq < cpu->min_freq)
        cpu->target_freq = cpu->min_freq;
        
    // Apply policy constraints
    switch (util->policy.power_state) {
        case POWER_PERFORMANCE:
            // Allow maximum frequency
            break;
            
        case POWER_POWERSAVE:
            // Limit to 75% of maximum
            if (cpu->target_freq > (cpu->max_freq * 3) / 4)
                cpu->target_freq = (cpu->max_freq * 3) / 4;
            break;
            
        case POWER_NORMAL:
        case POWER_AUTONOMOUS:
            // Use calculated target
            break;
    }
}

// Update CPU Frequency
void update_cpu_freq(schedutil_t *util, unsigned int cpu_id) {
    if (!util || cpu_id >= util->nr_cpus) return;

    cpu_t *cpu = &util->cpus[cpu_id];
    unsigned int old_freq = cpu->curr_freq;
    
    // Calculate new target frequency
    cpu->target_freq = calc_target_freq(util, cpu);
    
    // Apply constraints
    apply_freq_constraints(util, cpu);
    
    // Update current frequency if different
    if (cpu->target_freq != cpu->curr_freq) {
        cpu->curr_freq = cpu->target_freq;
        update_freq_stats(cpu, old_freq);
        
        LOG(LOG_LEVEL_DEBUG, 
            "CPU %u frequency changed: %u -> %u MHz (util: %u%%)",
            cpu->id, 
            old_freq / 1000, 
            cpu->curr_freq / 1000,
            cpu->util);
    }
}

// Update Frequency Statistics
void update_freq_stats(cpu_t *cpu, unsigned int old_freq) {
    cpu->stats.switches++;
    
    if (cpu->curr_freq > old_freq)
        cpu->stats.up_transitions++;
    else if (cpu->curr_freq < old_freq)
        cpu->stats.down_transitions++;
        
    // Update time in state
    unsigned int idx = (old_freq - MIN_FREQ) / FREQ_STEP;
    if (idx < 40)
        cpu->stats.time_in_state[idx]++;
        
    // Update average frequency
    cpu->stats.avg_frequency = 
        (cpu->stats.avg_frequency * (cpu->stats.switches - 1) + 
         cpu->curr_freq) / cpu->stats.switches;
         
    // Estimate energy usage (simplified model)
    double power = pow(cpu->curr_freq / 1000000.0, 2);  // f^2 power model
    cpu->stats.energy_usage += power;
}

// Monitor Thread
void* monitor_thread(void *arg) {
    schedutil_t *util = (schedutil_t*)arg;
    
    while (util->running) {
        // Check each CPU
        for (unsigned int i = 0; i < util->nr_cpus; i++) {
            cpu_t *cpu = &util->cpus[i];
            
            if (cpu->state != CPU_ACTIVE)
                continue;
                
            pthread_mutex_lock(&cpu->lock);
            
            // Update frequency if needed
            uint64_t now = time(NULL) * 1000000;
            uint64_t delta = now - cpu->last_update;
            
            if (delta >= util->policy.down_rate_limit_us) {
                update_cpu_freq(util, i);
                cpu->last_update = now;
            }
            
            pthread_mutex_unlock(&cpu->lock);
        }
        
        usleep(1000);  // 1ms sleep
    }
    
    return NULL;
}

// Print Scheduler Utility Statistics
void print_schedutil_stats(schedutil_t *util) {
    if (!util) return;

    printf("\nScheduler Utility Statistics:\n");
    printf("----------------------------\n");
    printf("Power State: %s\n", 
        get_power_state_string(util->policy.power_state));
    printf("Boost Enabled: %s\n", 
        util->policy.boost_enabled ? "Yes" : "No");
        
    for (unsigned int i = 0; i < util->nr_cpus; i++) {
        cpu_t *cpu = &util->cpus[i];
        
        printf("\nCPU %u Statistics:\n", i);
        printf("  State: %s\n", get_cpu_state_string(cpu->state));
        printf("  Current Frequency: %u MHz\n", cpu->curr_freq / 1000);
        printf("  Current Utilization: %u%%\n", cpu->util);
        printf("  Frequency Switches: %lu\n", cpu->stats.switches);
        printf("  Up Transitions: %lu\n", cpu->stats.up_transitions);
        printf("  Down Transitions: %lu\n", cpu->stats.down_transitions);
        printf("  Average Frequency: %.2f MHz\n", 
            cpu->stats.avg_frequency / 1000);
        printf("  Estimated Energy Usage: %.2f units\n", 
            cpu->stats.energy_usage);
            
        printf("  Time in State:\n");
        for (int j = 0; j < 40; j++) {
            if (cpu->stats.time_in_state[j] > 0) {
                printf("    %4d MHz: %lu\n", 
                    (MIN_FREQ + j * FREQ_STEP) / 1000,
                    cpu->stats.time_in_state[j]);
            }
        }
    }
}

// Destroy Scheduler Utility
void destroy_schedutil(schedutil_t *util) {
    if (!util) return;

    // Stop monitor thread
    util->running = false;
    pthread_join(util->monitor_thread, NULL);

    // Clean up mutexes
    for (unsigned int i = 0; i < util->nr_cpus; i++) {
        pthread_mutex_destroy(&util->cpus[i].lock);
    }
    pthread_mutex_destroy(&util->manager_lock);

    free(util);
    LOG(LOG_LEVEL_DEBUG, "Destroyed schedutil");
}

// Demonstrate Scheduler Utility
void demonstrate_schedutil(void) {
    // Create schedutil with 4 CPUs
    schedutil_t *util = create_schedutil(4);
    if (!util) return;

    // Simulate different scenarios
    printf("Starting CPU frequency scaling demonstration...\n");

    // Scenario 1: Normal workload
    printf("\nScenario 1: Normal workload\n");
    set_power_state(util, POWER_NORMAL);
    for (int i = 0; i < 10; i++) {
        for (unsigned int cpu = 0; cpu < util->nr_cpus; cpu++) {
            update_cpu_util(util, cpu, 30 + rand() % 40);  // 30-70% util
        }
        usleep(10000);  // 10ms
    }

    // Scenario 2: High performance workload
    printf("\nScenario 2: High performance workload\n");
    set_power_state(util, POWER_PERFORMANCE);
    for (int i = 0; i < 10; i++) {
        for (unsigned int cpu = 0; cpu < util->nr_cpus; cpu++) {
            update_cpu_util(util, cpu, 70 + rand() % 30);  // 70-100% util
        }
        usleep(10000);
    }

    // Scenario 3: Power saving mode
    printf("\nScenario 3: Power saving mode\n");
    set_power_state(util, POWER_POWERSAVE);
    for (int i = 0; i < 10; i++) {
        for (unsigned int cpu = 0; cpu < util->nr_cpus; cpu++) {
            update_cpu_util(util, cpu, rand() % 30);  // 0-30% util
        }
        usleep(10000);
    }

    // Print final statistics
    print_schedutil_stats(util);

    // Cleanup
    destroy_schedutil(util);
}

int main(void) {
    // Set random seed
    srand(time(NULL));

    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_schedutil();

    return 0;
}
