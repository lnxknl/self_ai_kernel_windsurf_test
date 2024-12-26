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

// Counter Types
typedef enum {
    COUNTER_TYPE_ABSOLUTE,
    COUNTER_TYPE_WRAPPING,
    COUNTER_TYPE_DIFFERENTIAL,
    COUNTER_TYPE_PULSE
} counter_type_t;

// Counter Modes
typedef enum {
    COUNTER_MODE_NORMAL,
    COUNTER_MODE_QUADRATURE,
    COUNTER_MODE_PULSE_DIRECTION,
    COUNTER_MODE_TWO_PULSE
} counter_mode_t;

// Counter States
typedef enum {
    COUNTER_STATE_DISABLED,
    COUNTER_STATE_IDLE,
    COUNTER_STATE_RUNNING,
    COUNTER_STATE_ERROR
} counter_state_t;

// Counter Channel
typedef struct {
    unsigned int id;
    char name[32];
    uint64_t value;
    uint64_t min_value;
    uint64_t max_value;
    bool enabled;
    pthread_mutex_t lock;
} counter_channel_t;

// Counter Device
typedef struct counter_device {
    unsigned int id;
    char name[32];
    counter_type_t type;
    counter_mode_t mode;
    counter_state_t state;
    
    counter_channel_t *channels;
    size_t num_channels;
    
    uint64_t count;
    uint64_t overflow;
    time_t last_update;
    
    pthread_mutex_t lock;
    struct counter_device *next;
} counter_device_t;

// Counter Statistics
typedef struct {
    unsigned long total_counts;
    unsigned long overflows;
    unsigned long underflows;
    unsigned long errors;
    double count_rate;
    time_t uptime;
} counter_stats_t;

// Counter Configuration
typedef struct {
    size_t max_devices;
    size_t max_channels;
    bool track_stats;
    unsigned int update_interval;
} counter_config_t;

// Counter Manager
typedef struct {
    counter_device_t *devices;
    size_t device_count;
    counter_config_t config;
    counter_stats_t stats;
    pthread_mutex_t manager_lock;
    bool running;
} counter_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_counter_type_string(counter_type_t type);
const char* get_counter_mode_string(counter_mode_t mode);
const char* get_counter_state_string(counter_state_t state);

counter_manager_t* create_counter_manager(counter_config_t config);
void destroy_counter_manager(counter_manager_t *manager);

counter_device_t* create_counter_device(
    unsigned int id,
    const char *name,
    counter_type_t type,
    counter_mode_t mode,
    size_t num_channels
);

void destroy_counter_device(counter_device_t *device);

bool add_counter_device(counter_manager_t *manager, counter_device_t *device);
counter_device_t* find_counter_device(counter_manager_t *manager, unsigned int id);

int start_counter(counter_device_t *device);
int stop_counter(counter_device_t *device);
int reset_counter(counter_device_t *device);

uint64_t read_counter(counter_device_t *device, unsigned int channel);
int write_counter(counter_device_t *device, unsigned int channel, uint64_t value);

void update_counter_stats(counter_manager_t *manager);
void print_counter_stats(counter_manager_t *manager);
void demonstrate_counter(void);

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

// Utility Function: Get Counter Type String
const char* get_counter_type_string(counter_type_t type) {
    switch(type) {
        case COUNTER_TYPE_ABSOLUTE:      return "ABSOLUTE";
        case COUNTER_TYPE_WRAPPING:      return "WRAPPING";
        case COUNTER_TYPE_DIFFERENTIAL:  return "DIFFERENTIAL";
        case COUNTER_TYPE_PULSE:         return "PULSE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Counter Mode String
const char* get_counter_mode_string(counter_mode_t mode) {
    switch(mode) {
        case COUNTER_MODE_NORMAL:          return "NORMAL";
        case COUNTER_MODE_QUADRATURE:      return "QUADRATURE";
        case COUNTER_MODE_PULSE_DIRECTION: return "PULSE_DIRECTION";
        case COUNTER_MODE_TWO_PULSE:       return "TWO_PULSE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Counter State String
const char* get_counter_state_string(counter_state_t state) {
    switch(state) {
        case COUNTER_STATE_DISABLED: return "DISABLED";
        case COUNTER_STATE_IDLE:     return "IDLE";
        case COUNTER_STATE_RUNNING:  return "RUNNING";
        case COUNTER_STATE_ERROR:    return "ERROR";
        default: return "UNKNOWN";
    }
}

// Create Counter Manager
counter_manager_t* create_counter_manager(counter_config_t config) {
    counter_manager_t *manager = malloc(sizeof(counter_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate counter manager");
        return NULL;
    }

    manager->devices = NULL;
    manager->device_count = 0;
    manager->config = config;
    memset(&manager->stats, 0, sizeof(counter_stats_t));
    
    pthread_mutex_init(&manager->manager_lock, NULL);
    manager->running = true;
    manager->stats.uptime = time(NULL);

    LOG(LOG_LEVEL_DEBUG, "Created counter manager");
    return manager;
}

// Create Counter Device
counter_device_t* create_counter_device(
    unsigned int id,
    const char *name,
    counter_type_t type,
    counter_mode_t mode,
    size_t num_channels
) {
    counter_device_t *device = malloc(sizeof(counter_device_t));
    if (!device) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate counter device");
        return NULL;
    }

    device->id = id;
    strncpy(device->name, name, sizeof(device->name) - 1);
    device->type = type;
    device->mode = mode;
    device->state = COUNTER_STATE_DISABLED;
    
    device->channels = calloc(num_channels, sizeof(counter_channel_t));
    if (!device->channels) {
        free(device);
        return NULL;
    }
    
    device->num_channels = num_channels;
    device->count = 0;
    device->overflow = 0;
    device->last_update = time(NULL);
    
    pthread_mutex_init(&device->lock, NULL);
    device->next = NULL;

    // Initialize channels
    for (size_t i = 0; i < num_channels; i++) {
        device->channels[i].id = i;
        snprintf(device->channels[i].name, sizeof(device->channels[i].name),
                "channel_%zu", i);
        device->channels[i].value = 0;
        device->channels[i].min_value = 0;
        device->channels[i].max_value = UINT64_MAX;
        device->channels[i].enabled = true;
        pthread_mutex_init(&device->channels[i].lock, NULL);
    }

    LOG(LOG_LEVEL_DEBUG, "Created counter device %s (ID: %u)", name, id);
    return device;
}

// Add Counter Device
bool add_counter_device(counter_manager_t *manager, counter_device_t *device) {
    if (!manager || !device) return false;

    pthread_mutex_lock(&manager->manager_lock);

    if (manager->device_count >= manager->config.max_devices) {
        pthread_mutex_unlock(&manager->manager_lock);
        return false;
    }

    device->next = manager->devices;
    manager->devices = device;
    manager->device_count++;

    pthread_mutex_unlock(&manager->manager_lock);

    LOG(LOG_LEVEL_DEBUG, "Added counter device %s", device->name);
    return true;
}

// Find Counter Device
counter_device_t* find_counter_device(
    counter_manager_t *manager,
    unsigned int id
) {
    if (!manager) return NULL;

    pthread_mutex_lock(&manager->manager_lock);

    counter_device_t *device = manager->devices;
    while (device) {
        if (device->id == id) {
            pthread_mutex_unlock(&manager->manager_lock);
            return device;
        }
        device = device->next;
    }

    pthread_mutex_unlock(&manager->manager_lock);
    return NULL;
}

// Start Counter
int start_counter(counter_device_t *device) {
    if (!device) return -EINVAL;

    pthread_mutex_lock(&device->lock);

    if (device->state == COUNTER_STATE_RUNNING) {
        pthread_mutex_unlock(&device->lock);
        return -EBUSY;
    }

    device->state = COUNTER_STATE_RUNNING;
    device->last_update = time(NULL);

    pthread_mutex_unlock(&device->lock);

    LOG(LOG_LEVEL_DEBUG, "Started counter device %s", device->name);
    return 0;
}

// Stop Counter
int stop_counter(counter_device_t *device) {
    if (!device) return -EINVAL;

    pthread_mutex_lock(&device->lock);

    if (device->state != COUNTER_STATE_RUNNING) {
        pthread_mutex_unlock(&device->lock);
        return -EINVAL;
    }

    device->state = COUNTER_STATE_IDLE;

    pthread_mutex_unlock(&device->lock);

    LOG(LOG_LEVEL_DEBUG, "Stopped counter device %s", device->name);
    return 0;
}

// Reset Counter
int reset_counter(counter_device_t *device) {
    if (!device) return -EINVAL;

    pthread_mutex_lock(&device->lock);

    device->count = 0;
    device->overflow = 0;
    
    for (size_t i = 0; i < device->num_channels; i++) {
        pthread_mutex_lock(&device->channels[i].lock);
        device->channels[i].value = 0;
        pthread_mutex_unlock(&device->channels[i].lock);
    }

    pthread_mutex_unlock(&device->lock);

    LOG(LOG_LEVEL_DEBUG, "Reset counter device %s", device->name);
    return 0;
}

// Read Counter Channel
uint64_t read_counter(counter_device_t *device, unsigned int channel) {
    if (!device || channel >= device->num_channels) return 0;

    pthread_mutex_lock(&device->channels[channel].lock);
    uint64_t value = device->channels[channel].value;
    pthread_mutex_unlock(&device->channels[channel].lock);

    return value;
}

// Write Counter Channel
int write_counter(
    counter_device_t *device,
    unsigned int channel,
    uint64_t value
) {
    if (!device || channel >= device->num_channels) return -EINVAL;

    pthread_mutex_lock(&device->channels[channel].lock);
    
    if (!device->channels[channel].enabled) {
        pthread_mutex_unlock(&device->channels[channel].lock);
        return -EACCES;
    }

    device->channels[channel].value = value;
    pthread_mutex_unlock(&device->channels[channel].lock);

    return 0;
}

// Update Counter Statistics
void update_counter_stats(counter_manager_t *manager) {
    if (!manager || !manager->config.track_stats) return;

    pthread_mutex_lock(&manager->manager_lock);

    counter_device_t *device = manager->devices;
    while (device) {
        pthread_mutex_lock(&device->lock);
        
        if (device->state == COUNTER_STATE_RUNNING) {
            // Simulate counter updates
            for (size_t i = 0; i < device->num_channels; i++) {
                pthread_mutex_lock(&device->channels[i].lock);
                
                if (device->channels[i].enabled) {
                    uint64_t old_value = device->channels[i].value;
                    uint64_t new_value = old_value;

                    switch (device->type) {
                        case COUNTER_TYPE_ABSOLUTE:
                            new_value += rand() % 10;
                            break;
                            
                        case COUNTER_TYPE_WRAPPING:
                            new_value = (old_value + rand() % 10) % 1000;
                            break;
                            
                        case COUNTER_TYPE_DIFFERENTIAL:
                            new_value += (rand() % 20) - 10;
                            break;
                            
                        case COUNTER_TYPE_PULSE:
                            new_value = rand() % 2;
                            break;
                    }

                    device->channels[i].value = new_value;
                    
                    if (new_value > device->channels[i].max_value)
                        manager->stats.overflows++;
                    else if (new_value < device->channels[i].min_value)
                        manager->stats.underflows++;
                }
                
                pthread_mutex_unlock(&device->channels[i].lock);
            }

            device->count++;
            manager->stats.total_counts++;
        }
        
        pthread_mutex_unlock(&device->lock);
        device = device->next;
    }

    // Update count rate
    time_t current_time = time(NULL);
    time_t elapsed = current_time - manager->stats.uptime;
    if (elapsed > 0) {
        manager->stats.count_rate = 
            (double)manager->stats.total_counts / elapsed;
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Print Counter Statistics
void print_counter_stats(counter_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    printf("\nCounter Manager Statistics:\n");
    printf("-------------------------\n");
    printf("Total Counts:    %lu\n", manager->stats.total_counts);
    printf("Overflows:       %lu\n", manager->stats.overflows);
    printf("Underflows:      %lu\n", manager->stats.underflows);
    printf("Errors:          %lu\n", manager->stats.errors);
    printf("Count Rate:      %.2f counts/sec\n", manager->stats.count_rate);
    printf("Uptime:          %ld seconds\n", 
        time(NULL) - manager->stats.uptime);

    // Print device details
    counter_device_t *device = manager->devices;
    while (device) {
        printf("\nDevice: %s (ID: %u)\n", device->name, device->id);
        printf("Type:  %s\n", get_counter_type_string(device->type));
        printf("Mode:  %s\n", get_counter_mode_string(device->mode));
        printf("State: %s\n", get_counter_state_string(device->state));
        
        for (size_t i = 0; i < device->num_channels; i++) {
            printf("Channel %zu: %lu\n", i, device->channels[i].value);
        }
        
        device = device->next;
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Destroy Counter Device
void destroy_counter_device(counter_device_t *device) {
    if (!device) return;

    // Destroy channels
    for (size_t i = 0; i < device->num_channels; i++) {
        pthread_mutex_destroy(&device->channels[i].lock);
    }
    
    free(device->channels);
    pthread_mutex_destroy(&device->lock);
    free(device);
}

// Destroy Counter Manager
void destroy_counter_manager(counter_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    // Destroy all devices
    counter_device_t *device = manager->devices;
    while (device) {
        counter_device_t *next = device->next;
        destroy_counter_device(device);
        device = next;
    }

    pthread_mutex_unlock(&manager->manager_lock);
    pthread_mutex_destroy(&manager->manager_lock);

    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed counter manager");
}

// Demonstrate Counter
void demonstrate_counter(void) {
    // Create counter configuration
    counter_config_t config = {
        .max_devices = 10,
        .max_channels = 8,
        .track_stats = true,
        .update_interval = 100
    };

    // Create counter manager
    counter_manager_t *manager = create_counter_manager(config);
    if (!manager) return;

    // Create sample counter devices
    const char *device_names[] = {
        "frequency_counter",
        "pulse_counter",
        "quadrature_encoder"
    };
    
    counter_type_t device_types[] = {
        COUNTER_TYPE_ABSOLUTE,
        COUNTER_TYPE_PULSE,
        COUNTER_TYPE_DIFFERENTIAL
    };
    
    counter_mode_t device_modes[] = {
        COUNTER_MODE_NORMAL,
        COUNTER_MODE_PULSE_DIRECTION,
        COUNTER_MODE_QUADRATURE
    };

    for (int i = 0; i < 3; i++) {
        counter_device_t *device = create_counter_device(
            i,
            device_names[i],
            device_types[i],
            device_modes[i],
            4  // 4 channels per device
        );

        if (device) {
            add_counter_device(manager, device);
            start_counter(device);
        }
    }

    // Simulate counter operations
    for (int i = 0; i < 5; i++) {
        update_counter_stats(manager);
        usleep(100000);  // 100ms delay
    }

    // Print statistics
    print_counter_stats(manager);

    // Cleanup
    destroy_counter_manager(manager);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_counter();

    return 0;
}
