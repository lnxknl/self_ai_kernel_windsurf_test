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

// SCMI Protocol IDs
typedef enum {
    SCMI_PROTOCOL_BASE      = 0x10,
    SCMI_PROTOCOL_POWER     = 0x11,
    SCMI_PROTOCOL_SYSTEM    = 0x12,
    SCMI_PROTOCOL_PERF      = 0x13,
    SCMI_PROTOCOL_CLOCK     = 0x14,
    SCMI_PROTOCOL_SENSOR    = 0x15,
    SCMI_PROTOCOL_RESET     = 0x16
} scmi_protocol_t;

// Notification Types
typedef enum {
    NOTIFY_TYPE_POWER_STATE,
    NOTIFY_TYPE_PERFORMANCE,
    NOTIFY_TYPE_CLOCK_RATE,
    NOTIFY_TYPE_SENSOR_TRIP,
    NOTIFY_TYPE_SYSTEM_POWER,
    NOTIFY_TYPE_RESET_ISSUED
} notify_type_t;

// Message Priority
typedef enum {
    PRIORITY_LOW,
    PRIORITY_NORMAL,
    PRIORITY_HIGH,
    PRIORITY_CRITICAL
} message_priority_t;

// Notification Message
typedef struct {
    uint32_t msg_id;
    scmi_protocol_t protocol;
    notify_type_t type;
    message_priority_t priority;
    void *data;
    size_t data_size;
    time_t timestamp;
} notify_message_t;

// Notification Listener
typedef struct notify_listener {
    uint32_t id;
    scmi_protocol_t protocol;
    void (*callback)(notify_message_t *msg);
    struct notify_listener *next;
} notify_listener_t;

// Notification Queue
typedef struct {
    notify_message_t *messages;
    size_t capacity;
    size_t size;
    size_t head;
    size_t tail;
    pthread_mutex_t lock;
} notify_queue_t;

// SCMI Statistics
typedef struct {
    unsigned long messages_sent;
    unsigned long messages_dropped;
    unsigned long listeners_registered;
    unsigned long queue_overflows;
    double avg_processing_time;
} scmi_stats_t;

// SCMI Configuration
typedef struct {
    size_t queue_size;
    bool track_stats;
    unsigned int process_interval;
    size_t max_listeners;
} scmi_config_t;

// SCMI Notification Manager
typedef struct {
    notify_queue_t queue;
    notify_listener_t *listeners;
    size_t listener_count;
    scmi_config_t config;
    scmi_stats_t stats;
    pthread_mutex_t manager_lock;
    pthread_t process_thread;
    bool running;
} scmi_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_protocol_string(scmi_protocol_t protocol);
const char* get_notify_type_string(notify_type_t type);
const char* get_priority_string(message_priority_t priority);

scmi_manager_t* create_scmi_manager(scmi_config_t config);
void destroy_scmi_manager(scmi_manager_t *manager);

bool register_listener(
    scmi_manager_t *manager,
    uint32_t id,
    scmi_protocol_t protocol,
    void (*callback)(notify_message_t *msg)
);

bool unregister_listener(scmi_manager_t *manager, uint32_t id);

bool send_notification(
    scmi_manager_t *manager,
    scmi_protocol_t protocol,
    notify_type_t type,
    message_priority_t priority,
    void *data,
    size_t data_size
);

void process_notifications(scmi_manager_t *manager);
void* notification_processor(void *arg);
void print_scmi_stats(scmi_manager_t *manager);
void demonstrate_scmi(void);

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

// Utility Function: Get Protocol String
const char* get_protocol_string(scmi_protocol_t protocol) {
    switch(protocol) {
        case SCMI_PROTOCOL_BASE:   return "BASE";
        case SCMI_PROTOCOL_POWER:  return "POWER";
        case SCMI_PROTOCOL_SYSTEM: return "SYSTEM";
        case SCMI_PROTOCOL_PERF:   return "PERFORMANCE";
        case SCMI_PROTOCOL_CLOCK:  return "CLOCK";
        case SCMI_PROTOCOL_SENSOR: return "SENSOR";
        case SCMI_PROTOCOL_RESET:  return "RESET";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Notification Type String
const char* get_notify_type_string(notify_type_t type) {
    switch(type) {
        case NOTIFY_TYPE_POWER_STATE:  return "POWER_STATE";
        case NOTIFY_TYPE_PERFORMANCE:  return "PERFORMANCE";
        case NOTIFY_TYPE_CLOCK_RATE:   return "CLOCK_RATE";
        case NOTIFY_TYPE_SENSOR_TRIP:  return "SENSOR_TRIP";
        case NOTIFY_TYPE_SYSTEM_POWER: return "SYSTEM_POWER";
        case NOTIFY_TYPE_RESET_ISSUED: return "RESET_ISSUED";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Priority String
const char* get_priority_string(message_priority_t priority) {
    switch(priority) {
        case PRIORITY_LOW:      return "LOW";
        case PRIORITY_NORMAL:   return "NORMAL";
        case PRIORITY_HIGH:     return "HIGH";
        case PRIORITY_CRITICAL: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

// Create SCMI Manager
scmi_manager_t* create_scmi_manager(scmi_config_t config) {
    scmi_manager_t *manager = malloc(sizeof(scmi_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate SCMI manager");
        return NULL;
    }

    // Initialize queue
    manager->queue.messages = calloc(config.queue_size, 
        sizeof(notify_message_t));
    if (!manager->queue.messages) {
        free(manager);
        return NULL;
    }

    manager->queue.capacity = config.queue_size;
    manager->queue.size = 0;
    manager->queue.head = 0;
    manager->queue.tail = 0;
    pthread_mutex_init(&manager->queue.lock, NULL);

    // Initialize manager
    manager->listeners = NULL;
    manager->listener_count = 0;
    manager->config = config;
    memset(&manager->stats, 0, sizeof(scmi_stats_t));
    pthread_mutex_init(&manager->manager_lock, NULL);
    manager->running = true;

    // Start processor thread
    pthread_create(&manager->process_thread, NULL, 
        notification_processor, manager);

    LOG(LOG_LEVEL_DEBUG, "Created SCMI manager");
    return manager;
}

// Register Notification Listener
bool register_listener(
    scmi_manager_t *manager,
    uint32_t id,
    scmi_protocol_t protocol,
    void (*callback)(notify_message_t *msg)
) {
    if (!manager || !callback) return false;

    pthread_mutex_lock(&manager->manager_lock);

    if (manager->listener_count >= manager->config.max_listeners) {
        pthread_mutex_unlock(&manager->manager_lock);
        return false;
    }

    notify_listener_t *listener = malloc(sizeof(notify_listener_t));
    if (!listener) {
        pthread_mutex_unlock(&manager->manager_lock);
        return false;
    }

    listener->id = id;
    listener->protocol = protocol;
    listener->callback = callback;
    listener->next = manager->listeners;
    manager->listeners = listener;
    manager->listener_count++;

    if (manager->config.track_stats)
        manager->stats.listeners_registered++;

    pthread_mutex_unlock(&manager->manager_lock);

    LOG(LOG_LEVEL_DEBUG, "Registered listener %u for protocol %s",
        id, get_protocol_string(protocol));
    return true;
}

// Unregister Notification Listener
bool unregister_listener(scmi_manager_t *manager, uint32_t id) {
    if (!manager) return false;

    pthread_mutex_lock(&manager->manager_lock);

    notify_listener_t *current = manager->listeners;
    notify_listener_t *prev = NULL;

    while (current) {
        if (current->id == id) {
            if (prev)
                prev->next = current->next;
            else
                manager->listeners = current->next;

            manager->listener_count--;
            free(current);
            
            pthread_mutex_unlock(&manager->manager_lock);
            return true;
        }
        prev = current;
        current = current->next;
    }

    pthread_mutex_unlock(&manager->manager_lock);
    return false;
}

// Send Notification
bool send_notification(
    scmi_manager_t *manager,
    scmi_protocol_t protocol,
    notify_type_t type,
    message_priority_t priority,
    void *data,
    size_t data_size
) {
    if (!manager) return false;

    pthread_mutex_lock(&manager->queue.lock);

    // Check if queue is full
    if (manager->queue.size >= manager->queue.capacity) {
        if (manager->config.track_stats) {
            manager->stats.messages_dropped++;
            manager->stats.queue_overflows++;
        }
        pthread_mutex_unlock(&manager->queue.lock);
        return false;
    }

    // Add message to queue
    notify_message_t *msg = &manager->queue.messages[manager->queue.tail];
    msg->msg_id = rand();
    msg->protocol = protocol;
    msg->type = type;
    msg->priority = priority;
    msg->data = malloc(data_size);
    
    if (msg->data) {
        memcpy(msg->data, data, data_size);
        msg->data_size = data_size;
        msg->timestamp = time(NULL);

        manager->queue.tail = (manager->queue.tail + 1) % 
            manager->queue.capacity;
        manager->queue.size++;

        if (manager->config.track_stats)
            manager->stats.messages_sent++;
    }

    pthread_mutex_unlock(&manager->queue.lock);

    LOG(LOG_LEVEL_DEBUG, "Sent notification: Protocol=%s, Type=%s, Priority=%s",
        get_protocol_string(protocol),
        get_notify_type_string(type),
        get_priority_string(priority));
    return true;
}

// Process Notifications
void process_notifications(scmi_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->queue.lock);

    while (manager->queue.size > 0) {
        notify_message_t *msg = &manager->queue.messages[manager->queue.head];
        
        pthread_mutex_lock(&manager->manager_lock);
        
        // Notify all interested listeners
        notify_listener_t *listener = manager->listeners;
        while (listener) {
            if (listener->protocol == msg->protocol) {
                struct timespec start_time, end_time;
                clock_gettime(CLOCK_MONOTONIC, &start_time);
                
                listener->callback(msg);
                
                if (manager->config.track_stats) {
                    clock_gettime(CLOCK_MONOTONIC, &end_time);
                    double process_time = 
                        (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                        (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
                        
                    manager->stats.avg_processing_time = 
                        (manager->stats.avg_processing_time * 
                            (manager->stats.messages_sent - 1) + 
                            process_time) /
                        manager->stats.messages_sent;
                }
            }
            listener = listener->next;
        }
        
        pthread_mutex_unlock(&manager->manager_lock);

        // Free message data and advance queue
        free(msg->data);
        manager->queue.head = (manager->queue.head + 1) % 
            manager->queue.capacity;
        manager->queue.size--;
    }

    pthread_mutex_unlock(&manager->queue.lock);
}

// Notification Processor Thread
void* notification_processor(void *arg) {
    scmi_manager_t *manager = (scmi_manager_t*)arg;

    while (manager->running) {
        process_notifications(manager);
        usleep(manager->config.process_interval * 1000);
    }

    return NULL;
}

// Print SCMI Statistics
void print_scmi_stats(scmi_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    printf("\nSCMI Manager Statistics:\n");
    printf("----------------------\n");
    printf("Messages Sent:     %lu\n", manager->stats.messages_sent);
    printf("Messages Dropped:  %lu\n", manager->stats.messages_dropped);
    printf("Queue Overflows:   %lu\n", manager->stats.queue_overflows);
    printf("Active Listeners:  %zu\n", manager->listener_count);
    printf("Total Listeners:   %lu\n", manager->stats.listeners_registered);
    printf("Avg Process Time:  %.2f ms\n", manager->stats.avg_processing_time);

    pthread_mutex_unlock(&manager->manager_lock);
}

// Example Notification Callbacks
void power_callback(notify_message_t *msg) {
    printf("Power notification received: %s\n",
        (char*)msg->data);
}

void performance_callback(notify_message_t *msg) {
    printf("Performance notification received: %s\n",
        (char*)msg->data);
}

void sensor_callback(notify_message_t *msg) {
    printf("Sensor notification received: %s\n",
        (char*)msg->data);
}

// Destroy SCMI Manager
void destroy_scmi_manager(scmi_manager_t *manager) {
    if (!manager) return;

    // Stop processor thread
    manager->running = false;
    pthread_join(manager->process_thread, NULL);

    pthread_mutex_lock(&manager->manager_lock);

    // Free all listeners
    notify_listener_t *listener = manager->listeners;
    while (listener) {
        notify_listener_t *next = listener->next;
        free(listener);
        listener = next;
    }

    // Free all queued messages
    for (size_t i = 0; i < manager->queue.capacity; i++) {
        if (manager->queue.messages[i].data) {
            free(manager->queue.messages[i].data);
        }
    }

    free(manager->queue.messages);

    pthread_mutex_unlock(&manager->manager_lock);
    pthread_mutex_destroy(&manager->manager_lock);
    pthread_mutex_destroy(&manager->queue.lock);

    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed SCMI manager");
}

// Demonstrate SCMI
void demonstrate_scmi(void) {
    // Create SCMI configuration
    scmi_config_t config = {
        .queue_size = 100,
        .track_stats = true,
        .process_interval = 100,
        .max_listeners = 10
    };

    // Create SCMI manager
    scmi_manager_t *manager = create_scmi_manager(config);
    if (!manager) return;

    // Register listeners
    register_listener(manager, 1, SCMI_PROTOCOL_POWER, power_callback);
    register_listener(manager, 2, SCMI_PROTOCOL_PERF, performance_callback);
    register_listener(manager, 3, SCMI_PROTOCOL_SENSOR, sensor_callback);

    // Send sample notifications
    const char *messages[] = {
        "Power state changed to low power",
        "Performance level increased to boost",
        "Temperature sensor threshold exceeded",
        "Clock rate changed to 1GHz",
        "System entering sleep state"
    };

    scmi_protocol_t protocols[] = {
        SCMI_PROTOCOL_POWER,
        SCMI_PROTOCOL_PERF,
        SCMI_PROTOCOL_SENSOR,
        SCMI_PROTOCOL_CLOCK,
        SCMI_PROTOCOL_SYSTEM
    };

    notify_type_t types[] = {
        NOTIFY_TYPE_POWER_STATE,
        NOTIFY_TYPE_PERFORMANCE,
        NOTIFY_TYPE_SENSOR_TRIP,
        NOTIFY_TYPE_CLOCK_RATE,
        NOTIFY_TYPE_SYSTEM_POWER
    };

    for (int i = 0; i < 5; i++) {
        send_notification(
            manager,
            protocols[i],
            types[i],
            PRIORITY_NORMAL,
            (void*)messages[i],
            strlen(messages[i]) + 1
        );
        usleep(100000);  // 100ms delay
    }

    // Wait for processing
    sleep(1);

    // Print statistics
    print_scmi_stats(manager);

    // Cleanup
    destroy_scmi_manager(manager);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_scmi();

    return 0;
}
