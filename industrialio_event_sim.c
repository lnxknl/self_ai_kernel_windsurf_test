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

// IIO Event Types
typedef enum {
    IIO_EV_TYPE_THRESH,
    IIO_EV_TYPE_MAG,
    IIO_EV_TYPE_ROC,
    IIO_EV_TYPE_THRESH_ADAPTIVE,
    IIO_EV_TYPE_MAG_ADAPTIVE
} iio_event_type_t;

// IIO Event Directions
typedef enum {
    IIO_EV_DIR_RISING,
    IIO_EV_DIR_FALLING,
    IIO_EV_DIR_EITHER,
    IIO_EV_DIR_NONE
} iio_event_dir_t;

// IIO Channel Types
typedef enum {
    IIO_VOLTAGE,
    IIO_CURRENT,
    IIO_POWER,
    IIO_ACCEL,
    IIO_ANGL_VEL,
    IIO_MAGN,
    IIO_TEMP,
    IIO_PRESSURE,
    IIO_HUMIDITYRELATIVE
} iio_chan_type_t;

// IIO Event
typedef struct {
    uint64_t id;
    iio_event_type_t type;
    iio_event_dir_t direction;
    iio_chan_type_t channel_type;
    int channel;
    double value;
    time_t timestamp;
} iio_event_t;

// IIO Event Queue
typedef struct {
    iio_event_t *events;
    size_t capacity;
    size_t size;
    size_t head;
    size_t tail;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
} iio_event_queue_t;

// IIO Device
typedef struct iio_device {
    unsigned int id;
    char name[32];
    iio_chan_type_t *channels;
    size_t num_channels;
    double *thresholds;
    bool *enabled;
    struct iio_device *next;
} iio_device_t;

// IIO Statistics
typedef struct {
    unsigned long events_generated;
    unsigned long events_processed;
    unsigned long queue_overflows;
    unsigned long threshold_violations;
    double avg_processing_time;
} iio_stats_t;

// IIO Configuration
typedef struct {
    size_t queue_size;
    size_t max_devices;
    bool track_stats;
    unsigned int sample_interval;
} iio_config_t;

// IIO Event Manager
typedef struct {
    iio_event_queue_t queue;
    iio_device_t *devices;
    size_t device_count;
    iio_config_t config;
    iio_stats_t stats;
    pthread_mutex_t manager_lock;
    pthread_t sample_thread;
    bool running;
} iio_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_event_type_string(iio_event_type_t type);
const char* get_event_dir_string(iio_event_dir_t dir);
const char* get_chan_type_string(iio_chan_type_t type);

iio_manager_t* create_iio_manager(iio_config_t config);
void destroy_iio_manager(iio_manager_t *manager);

iio_device_t* create_iio_device(
    unsigned int id,
    const char *name,
    iio_chan_type_t *channels,
    size_t num_channels
);

void destroy_iio_device(iio_device_t *device);

bool add_iio_device(iio_manager_t *manager, iio_device_t *device);
iio_device_t* find_iio_device(iio_manager_t *manager, unsigned int id);

bool push_event(iio_manager_t *manager, iio_event_t *event);
bool pop_event(iio_manager_t *manager, iio_event_t *event);

void sample_devices(iio_manager_t *manager);
void* device_sampler(void *arg);
void print_iio_stats(iio_manager_t *manager);
void demonstrate_iio(void);

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

// Utility Function: Get Event Type String
const char* get_event_type_string(iio_event_type_t type) {
    switch(type) {
        case IIO_EV_TYPE_THRESH:          return "THRESHOLD";
        case IIO_EV_TYPE_MAG:             return "MAGNITUDE";
        case IIO_EV_TYPE_ROC:             return "RATE_OF_CHANGE";
        case IIO_EV_TYPE_THRESH_ADAPTIVE: return "ADAPTIVE_THRESHOLD";
        case IIO_EV_TYPE_MAG_ADAPTIVE:    return "ADAPTIVE_MAGNITUDE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Event Direction String
const char* get_event_dir_string(iio_event_dir_t dir) {
    switch(dir) {
        case IIO_EV_DIR_RISING:  return "RISING";
        case IIO_EV_DIR_FALLING: return "FALLING";
        case IIO_EV_DIR_EITHER:  return "EITHER";
        case IIO_EV_DIR_NONE:    return "NONE";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Channel Type String
const char* get_chan_type_string(iio_chan_type_t type) {
    switch(type) {
        case IIO_VOLTAGE:           return "VOLTAGE";
        case IIO_CURRENT:           return "CURRENT";
        case IIO_POWER:             return "POWER";
        case IIO_ACCEL:             return "ACCELERATION";
        case IIO_ANGL_VEL:          return "ANGULAR_VELOCITY";
        case IIO_MAGN:              return "MAGNETIC";
        case IIO_TEMP:              return "TEMPERATURE";
        case IIO_PRESSURE:          return "PRESSURE";
        case IIO_HUMIDITYRELATIVE:  return "HUMIDITY";
        default: return "UNKNOWN";
    }
}

// Create IIO Manager
iio_manager_t* create_iio_manager(iio_config_t config) {
    iio_manager_t *manager = malloc(sizeof(iio_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate IIO manager");
        return NULL;
    }

    // Initialize queue
    manager->queue.events = calloc(config.queue_size, sizeof(iio_event_t));
    if (!manager->queue.events) {
        free(manager);
        return NULL;
    }

    manager->queue.capacity = config.queue_size;
    manager->queue.size = 0;
    manager->queue.head = 0;
    manager->queue.tail = 0;
    pthread_mutex_init(&manager->queue.lock, NULL);
    pthread_cond_init(&manager->queue.not_empty, NULL);

    // Initialize manager
    manager->devices = NULL;
    manager->device_count = 0;
    manager->config = config;
    memset(&manager->stats, 0, sizeof(iio_stats_t));
    pthread_mutex_init(&manager->manager_lock, NULL);
    manager->running = true;

    // Start sampler thread
    pthread_create(&manager->sample_thread, NULL, device_sampler, manager);

    LOG(LOG_LEVEL_DEBUG, "Created IIO manager");
    return manager;
}

// Create IIO Device
iio_device_t* create_iio_device(
    unsigned int id,
    const char *name,
    iio_chan_type_t *channels,
    size_t num_channels
) {
    iio_device_t *device = malloc(sizeof(iio_device_t));
    if (!device) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate IIO device");
        return NULL;
    }

    device->id = id;
    strncpy(device->name, name, sizeof(device->name) - 1);
    
    device->channels = malloc(num_channels * sizeof(iio_chan_type_t));
    device->thresholds = malloc(num_channels * sizeof(double));
    device->enabled = malloc(num_channels * sizeof(bool));
    
    if (!device->channels || !device->thresholds || !device->enabled) {
        free(device->channels);
        free(device->thresholds);
        free(device->enabled);
        free(device);
        return NULL;
    }

    memcpy(device->channels, channels, num_channels * sizeof(iio_chan_type_t));
    device->num_channels = num_channels;
    
    // Initialize thresholds and enabled flags
    for (size_t i = 0; i < num_channels; i++) {
        device->thresholds[i] = 0.0;
        device->enabled[i] = true;
    }
    
    device->next = NULL;

    LOG(LOG_LEVEL_DEBUG, "Created IIO device %s (ID: %u)", name, id);
    return device;
}

// Add IIO Device
bool add_iio_device(iio_manager_t *manager, iio_device_t *device) {
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

    LOG(LOG_LEVEL_DEBUG, "Added IIO device %s", device->name);
    return true;
}

// Find IIO Device
iio_device_t* find_iio_device(iio_manager_t *manager, unsigned int id) {
    if (!manager) return NULL;

    pthread_mutex_lock(&manager->manager_lock);

    iio_device_t *device = manager->devices;
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

// Push Event to Queue
bool push_event(iio_manager_t *manager, iio_event_t *event) {
    if (!manager || !event) return false;

    pthread_mutex_lock(&manager->queue.lock);

    if (manager->queue.size >= manager->queue.capacity) {
        if (manager->config.track_stats)
            manager->stats.queue_overflows++;
        pthread_mutex_unlock(&manager->queue.lock);
        return false;
    }

    manager->queue.events[manager->queue.tail] = *event;
    manager->queue.tail = (manager->queue.tail + 1) % manager->queue.capacity;
    manager->queue.size++;

    if (manager->config.track_stats)
        manager->stats.events_generated++;

    pthread_cond_signal(&manager->queue.not_empty);
    pthread_mutex_unlock(&manager->queue.lock);

    LOG(LOG_LEVEL_DEBUG, "Generated event: Type=%s, Direction=%s, Channel=%s",
        get_event_type_string(event->type),
        get_event_dir_string(event->direction),
        get_chan_type_string(event->channel_type));
    return true;
}

// Pop Event from Queue
bool pop_event(iio_manager_t *manager, iio_event_t *event) {
    if (!manager || !event) return false;

    pthread_mutex_lock(&manager->queue.lock);

    while (manager->queue.size == 0) {
        pthread_cond_wait(&manager->queue.not_empty, &manager->queue.lock);
    }

    *event = manager->queue.events[manager->queue.head];
    manager->queue.head = (manager->queue.head + 1) % manager->queue.capacity;
    manager->queue.size--;

    if (manager->config.track_stats)
        manager->stats.events_processed++;

    pthread_mutex_unlock(&manager->queue.lock);
    return true;
}

// Sample Devices
void sample_devices(iio_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    iio_device_t *device = manager->devices;
    while (device) {
        for (size_t i = 0; i < device->num_channels; i++) {
            if (!device->enabled[i]) continue;

            // Simulate sensor reading
            double value = (double)rand() / RAND_MAX * 100.0;
            
            // Check threshold
            if (value > device->thresholds[i]) {
                iio_event_t event = {
                    .id = rand(),
                    .type = IIO_EV_TYPE_THRESH,
                    .direction = IIO_EV_DIR_RISING,
                    .channel_type = device->channels[i],
                    .channel = i,
                    .value = value,
                    .timestamp = time(NULL)
                };

                push_event(manager, &event);

                if (manager->config.track_stats)
                    manager->stats.threshold_violations++;
            }
        }
        device = device->next;
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Device Sampler Thread
void* device_sampler(void *arg) {
    iio_manager_t *manager = (iio_manager_t*)arg;

    while (manager->running) {
        sample_devices(manager);
        usleep(manager->config.sample_interval * 1000);
    }

    return NULL;
}

// Print IIO Statistics
void print_iio_stats(iio_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    printf("\nIIO Manager Statistics:\n");
    printf("--------------------\n");
    printf("Events Generated:      %lu\n", manager->stats.events_generated);
    printf("Events Processed:      %lu\n", manager->stats.events_processed);
    printf("Queue Overflows:       %lu\n", manager->stats.queue_overflows);
    printf("Threshold Violations:  %lu\n", manager->stats.threshold_violations);
    printf("Avg Processing Time:   %.2f ms\n", manager->stats.avg_processing_time);

    // Print device details
    iio_device_t *device = manager->devices;
    while (device) {
        printf("\nDevice: %s (ID: %u)\n", device->name, device->id);
        printf("Channels:\n");
        for (size_t i = 0; i < device->num_channels; i++) {
            printf("  %zu: Type=%s, Threshold=%.2f, Enabled=%s\n",
                i,
                get_chan_type_string(device->channels[i]),
                device->thresholds[i],
                device->enabled[i] ? "true" : "false");
        }
        device = device->next;
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Destroy IIO Device
void destroy_iio_device(iio_device_t *device) {
    if (!device) return;

    free(device->channels);
    free(device->thresholds);
    free(device->enabled);
    free(device);
}

// Destroy IIO Manager
void destroy_iio_manager(iio_manager_t *manager) {
    if (!manager) return;

    // Stop sampler thread
    manager->running = false;
    pthread_join(manager->sample_thread, NULL);

    pthread_mutex_lock(&manager->manager_lock);

    // Destroy all devices
    iio_device_t *device = manager->devices;
    while (device) {
        iio_device_t *next = device->next;
        destroy_iio_device(device);
        device = next;
    }

    free(manager->queue.events);

    pthread_mutex_unlock(&manager->manager_lock);
    pthread_mutex_destroy(&manager->manager_lock);
    pthread_mutex_destroy(&manager->queue.lock);
    pthread_cond_destroy(&manager->queue.not_empty);

    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed IIO manager");
}

// Demonstrate IIO
void demonstrate_iio(void) {
    // Create IIO configuration
    iio_config_t config = {
        .queue_size = 100,
        .max_devices = 10,
        .track_stats = true,
        .sample_interval = 100
    };

    // Create IIO manager
    iio_manager_t *manager = create_iio_manager(config);
    if (!manager) return;

    // Create sample devices
    iio_chan_type_t temp_channels[] = {IIO_TEMP, IIO_HUMIDITYRELATIVE};
    iio_chan_type_t accel_channels[] = {IIO_ACCEL, IIO_ANGL_VEL, IIO_MAGN};
    iio_chan_type_t power_channels[] = {IIO_VOLTAGE, IIO_CURRENT, IIO_POWER};

    iio_device_t *temp_sensor = create_iio_device(1, "temp_sensor",
        temp_channels, 2);
    iio_device_t *accel_sensor = create_iio_device(2, "accel_sensor",
        accel_channels, 3);
    iio_device_t *power_monitor = create_iio_device(3, "power_monitor",
        power_channels, 3);

    if (temp_sensor && accel_sensor && power_monitor) {
        // Set some thresholds
        temp_sensor->thresholds[0] = 30.0;    // Temperature threshold
        temp_sensor->thresholds[1] = 80.0;    // Humidity threshold
        
        accel_sensor->thresholds[0] = 2.0;    // Acceleration threshold
        accel_sensor->thresholds[1] = 50.0;   // Angular velocity threshold
        accel_sensor->thresholds[2] = 100.0;  // Magnetic field threshold
        
        power_monitor->thresholds[0] = 240.0; // Voltage threshold
        power_monitor->thresholds[1] = 10.0;  // Current threshold
        power_monitor->thresholds[2] = 2000.0; // Power threshold

        // Add devices to manager
        add_iio_device(manager, temp_sensor);
        add_iio_device(manager, accel_sensor);
        add_iio_device(manager, power_monitor);
    }

    // Let the system run for a while
    sleep(2);

    // Print statistics
    print_iio_stats(manager);

    // Cleanup
    destroy_iio_manager(manager);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_iio();

    return 0;
}
