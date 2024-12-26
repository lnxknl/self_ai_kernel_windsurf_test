#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

// Logging and Debugging Macros
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

// Memory Domain Types
typedef enum {
    DOMAIN_CPU,
    DOMAIN_GPU,
    DOMAIN_NETWORK,
    DOMAIN_ACCELERATOR,
    DOMAIN_EXTERNAL
} memory_domain_t;

// Buffer Flags
typedef enum {
    BUFFER_FLAG_NONE       = 0x00,
    BUFFER_FLAG_SHARED     = 0x01,
    BUFFER_FLAG_IMPORTED   = 0x02,
    BUFFER_FLAG_EXPORTED   = 0x04,
    BUFFER_FLAG_MAPPED     = 0x08,
    BUFFER_FLAG_CACHED     = 0x10,
    BUFFER_FLAG_DIRTY      = 0x20
} buffer_flag_t;

// Buffer Access Permissions
typedef enum {
    PERM_NONE      = 0x00,
    PERM_READ      = 0x01,
    PERM_WRITE     = 0x02,
    PERM_READWRITE = 0x03
} buffer_perm_t;

// Buffer Synchronization Modes
typedef enum {
    SYNC_NONE,
    SYNC_FULL,
    SYNC_PARTIAL,
    SYNC_LAZY
} sync_mode_t;

// Forward Declarations
struct prime_buffer;
struct prime_device;

// Buffer Synchronization Callback
typedef bool (*sync_callback_fn)(
    struct prime_buffer *buffer, 
    memory_domain_t source, 
    memory_domain_t destination
);

// Buffer Descriptor
typedef struct prime_buffer {
    void *data;                 // Actual buffer data
    size_t size;                // Buffer size
    uint64_t unique_id;         // Unique identifier
    memory_domain_t origin;     // Original memory domain
    buffer_flag_t flags;        // Buffer flags
    buffer_perm_t permissions;  // Access permissions

    // Synchronization metadata
    sync_mode_t sync_mode;
    sync_callback_fn sync_handler;
    
    // Tracking and reference counting
    int ref_count;
    struct prime_device *owner;

    // Linked list for device tracking
    struct prime_buffer *next;
} prime_buffer_t;

// Device Descriptor
typedef struct prime_device {
    memory_domain_t domain;     // Device memory domain
    char *device_name;          // Device identifier
    prime_buffer_t *buffers;    // Linked list of buffers
    size_t total_buffer_size;   // Total managed buffer size
    int buffer_count;           // Number of managed buffers
} prime_device_t;

// Prime Management Statistics
typedef struct {
    unsigned long total_buffers_created;
    unsigned long total_buffers_imported;
    unsigned long total_buffers_exported;
    unsigned long sync_operations;
    unsigned long sync_failures;
} prime_stats_t;

// Prime Buffer Management System
typedef struct {
    prime_device_t *devices;    // Linked list of devices
    prime_stats_t stats;        // Management statistics
    size_t max_buffer_size;     // Maximum allowed buffer size
    size_t total_memory_limit;  // Total memory management limit
} prime_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_domain_string(memory_domain_t domain);
const char* get_sync_mode_string(sync_mode_t mode);

prime_system_t* create_prime_system(size_t max_buffer_size, size_t memory_limit);
void destroy_prime_system(prime_system_t *system);

prime_device_t* create_prime_device(
    prime_system_t *system, 
    memory_domain_t domain, 
    const char *device_name
);

prime_buffer_t* create_prime_buffer(
    prime_device_t *device,
    size_t size,
    buffer_flag_t flags,
    buffer_perm_t permissions
);

bool export_prime_buffer(
    prime_buffer_t *buffer, 
    prime_device_t *destination_device
);

bool import_prime_buffer(
    prime_device_t *source_device, 
    prime_buffer_t *buffer, 
    prime_device_t *destination_device
);

bool synchronize_buffer(
    prime_buffer_t *buffer, 
    memory_domain_t source, 
    memory_domain_t destination
);

void print_prime_system_stats(prime_system_t *system);
void demonstrate_prime_buffer_management();

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

// Utility Function: Get Memory Domain String
const char* get_domain_string(memory_domain_t domain) {
    switch(domain) {
        case DOMAIN_CPU:         return "CPU";
        case DOMAIN_GPU:         return "GPU";
        case DOMAIN_NETWORK:     return "NETWORK";
        case DOMAIN_ACCELERATOR: return "ACCELERATOR";
        case DOMAIN_EXTERNAL:    return "EXTERNAL";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Sync Mode String
const char* get_sync_mode_string(sync_mode_t mode) {
    switch(mode) {
        case SYNC_NONE:     return "NONE";
        case SYNC_FULL:     return "FULL";
        case SYNC_PARTIAL:  return "PARTIAL";
        case SYNC_LAZY:     return "LAZY";
        default: return "UNKNOWN";
    }
}

// Create Prime Buffer Management System
prime_system_t* create_prime_system(size_t max_buffer_size, size_t memory_limit) {
    prime_system_t *system = malloc(sizeof(prime_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate prime system");
        return NULL;
    }

    system->devices = NULL;
    system->max_buffer_size = max_buffer_size;
    system->total_memory_limit = memory_limit;

    // Reset statistics
    memset(&system->stats, 0, sizeof(prime_stats_t));

    return system;
}

// Create Prime Device
prime_device_t* create_prime_device(
    prime_system_t *system, 
    memory_domain_t domain, 
    const char *device_name
) {
    if (!system) return NULL;

    prime_device_t *device = malloc(sizeof(prime_device_t));
    if (!device) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate prime device");
        return NULL;
    }

    device->domain = domain;
    device->device_name = strdup(device_name);
    device->buffers = NULL;
    device->total_buffer_size = 0;
    device->buffer_count = 0;

    // Link device to system
    device->next = system->devices;
    system->devices = device;

    LOG(LOG_LEVEL_DEBUG, "Created Prime Device: %s (Domain: %s)", 
        device_name, get_domain_string(domain));

    return device;
}

// Create Prime Buffer
prime_buffer_t* create_prime_buffer(
    prime_device_t *device,
    size_t size,
    buffer_flag_t flags,
    buffer_perm_t permissions
) {
    if (!device || size == 0) return NULL;

    // Check system-level memory limits
    prime_system_t *system = NULL;
    for (prime_device_t *dev = system->devices; dev; dev = dev->next) {
        if (dev == device) {
            system = container_of(dev, prime_system_t, devices);
            break;
        }
    }

    if (!system || size > system->max_buffer_size) {
        LOG(LOG_LEVEL_ERROR, "Buffer size exceeds system limits");
        return NULL;
    }

    prime_buffer_t *buffer = malloc(sizeof(prime_buffer_t));
    if (!buffer) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate prime buffer");
        return NULL;
    }

    buffer->data = malloc(size);
    if (!buffer->data) {
        free(buffer);
        LOG(LOG_LEVEL_ERROR, "Failed to allocate buffer data");
        return NULL;
    }

    buffer->size = size;
    buffer->unique_id = (uint64_t)buffer;  // Use address as unique ID
    buffer->origin = device->domain;
    buffer->flags = flags;
    buffer->permissions = permissions;
    buffer->sync_mode = SYNC_FULL;
    buffer->sync_handler = NULL;
    buffer->ref_count = 1;
    buffer->owner = device;

    // Link buffer to device
    buffer->next = device->buffers;
    device->buffers = buffer;
    device->total_buffer_size += size;
    device->buffer_count++;

    // Update system statistics
    system->stats.total_buffers_created++;

    LOG(LOG_LEVEL_DEBUG, "Created Prime Buffer: %lu bytes (Device: %s)", 
        size, device->device_name);

    return buffer;
}

// Export Prime Buffer
bool export_prime_buffer(
    prime_buffer_t *buffer, 
    prime_device_t *destination_device
) {
    if (!buffer || !destination_device) return false;

    // Check export permissions
    if (!(buffer->flags & BUFFER_FLAG_SHARED)) {
        LOG(LOG_LEVEL_WARN, "Buffer not marked for sharing");
        return false;
    }

    // Synchronize buffer between domains
    if (!synchronize_buffer(
        buffer, 
        buffer->origin, 
        destination_device->domain
    )) {
        return false;
    }

    // Update buffer metadata
    buffer->flags |= BUFFER_FLAG_EXPORTED;
    buffer->ref_count++;

    // Update system statistics
    prime_system_t *system = buffer->owner->next;
    system->stats.total_buffers_exported++;

    LOG(LOG_LEVEL_INFO, "Exported buffer from %s to %s", 
        get_domain_string(buffer->origin), 
        get_domain_string(destination_device->domain));

    return true;
}

// Import Prime Buffer
bool import_prime_buffer(
    prime_device_t *source_device, 
    prime_buffer_t *buffer, 
    prime_device_t *destination_device
) {
    if (!source_device || !buffer || !destination_device) return false;

    // Check import permissions
    if (!(buffer->flags & BUFFER_FLAG_SHARED)) {
        LOG(LOG_LEVEL_WARN, "Buffer not marked for sharing");
        return false;
    }

    // Synchronize buffer between domains
    if (!synchronize_buffer(
        buffer, 
        source_device->domain, 
        destination_device->domain
    )) {
        return false;
    }

    // Update buffer metadata
    buffer->flags |= BUFFER_FLAG_IMPORTED;
    buffer->ref_count++;

    // Update system statistics
    prime_system_t *system = buffer->owner->next;
    system->stats.total_buffers_imported++;

    LOG(LOG_LEVEL_INFO, "Imported buffer from %s to %s", 
        get_domain_string(source_device->domain), 
        get_domain_string(destination_device->domain));

    return true;
}

// Synchronize Buffer Between Domains
bool synchronize_buffer(
    prime_buffer_t *buffer, 
    memory_domain_t source, 
    memory_domain_t destination
) {
    if (!buffer) return false;

    // Check synchronization mode
    switch (buffer->sync_mode) {
        case SYNC_NONE:
            LOG(LOG_LEVEL_WARN, "No synchronization for buffer");
            return false;

        case SYNC_LAZY:
            LOG(LOG_LEVEL_DEBUG, "Lazy synchronization");
            buffer->flags |= BUFFER_FLAG_DIRTY;
            break;

        case SYNC_PARTIAL:
            LOG(LOG_LEVEL_DEBUG, "Partial synchronization");
            // Implement partial sync logic here
            break;

        case SYNC_FULL:
            LOG(LOG_LEVEL_DEBUG, "Full synchronization");
            // Perform full buffer synchronization
            break;
    }

    // Call custom sync handler if provided
    if (buffer->sync_handler) {
        bool sync_result = buffer->sync_handler(
            buffer, source, destination
        );

        if (!sync_result) {
            LOG(LOG_LEVEL_ERROR, "Custom sync handler failed");
            return false;
        }
    }

    // Update system statistics
    prime_system_t *system = buffer->owner->next;
    system->stats.sync_operations++;

    return true;
}

// Print Prime System Statistics
void print_prime_system_stats(prime_system_t *system) {
    if (!system) return;

    printf("\nPrime Buffer Management Statistics:\n");
    printf("-----------------------------------\n");
    printf("Total Buffers Created:  %lu\n", system->stats.total_buffers_created);
    printf("Total Buffers Imported: %lu\n", system->stats.total_buffers_imported);
    printf("Total Buffers Exported: %lu\n", system->stats.total_buffers_exported);
    printf("Sync Operations:        %lu\n", system->stats.sync_operations);
    printf("Sync Failures:          %lu\n", system->stats.sync_failures);
}

// Destroy Prime Buffer
void destroy_prime_buffer(prime_buffer_t *buffer) {
    if (!buffer) return;

    buffer->ref_count--;
    if (buffer->ref_count > 0) {
        return;
    }

    // Decrease device buffer tracking
    if (buffer->owner) {
        buffer->owner->total_buffer_size -= buffer->size;
        buffer->owner->buffer_count--;
    }

    // Free buffer data
    free(buffer->data);
    free(buffer);
}

// Destroy Prime Device
void destroy_prime_device(prime_device_t *device) {
    if (!device) return;

    // Free device name
    free(device->device_name);

    // Free all buffers
    prime_buffer_t *buffer = device->buffers;
    while (buffer) {
        prime_buffer_t *next = buffer->next;
        destroy_prime_buffer(buffer);
        buffer = next;
    }

    free(device);
}

// Destroy Prime System
void destroy_prime_system(prime_system_t *system) {
    if (!system) return;

    // Destroy all devices
    prime_device_t *device = system->devices;
    while (device) {
        prime_device_t *next = device->next;
        destroy_prime_device(device);
        device = next;
    }

    free(system);
}

// Example Synchronization Handler
bool example_sync_handler(
    prime_buffer_t *buffer, 
    memory_domain_t source, 
    memory_domain_t destination
) {
    LOG(LOG_LEVEL_DEBUG, "Custom sync from %s to %s", 
        get_domain_string(source), 
        get_domain_string(destination));
    
    // Simulate complex synchronization logic
    if (source == DOMAIN_GPU && destination == DOMAIN_CPU) {
        // Specific handling for GPU to CPU transfer
        memcpy(buffer->data, buffer->data, buffer->size);
        return true;
    }

    return false;
}

// Demonstration Function
void demonstrate_prime_buffer_management() {
    // Create Prime System
    prime_system_t *system = create_prime_system(
        1024 * 1024,    // Max buffer size: 1MB
        16 * 1024 * 1024 // Total memory limit: 16MB
    );

    // Create Devices
    prime_device_t *cpu_device = create_prime_device(
        system, DOMAIN_CPU, "MainCPU"
    );

    prime_device_t *gpu_device = create_prime_device(
        system, DOMAIN_GPU, "DiscreteGPU"
    );

    // Create Buffers
    prime_buffer_t *cpu_buffer = create_prime_buffer(
        cpu_device,
        4096,                  // 4KB buffer
        BUFFER_FLAG_SHARED,    // Sharable
        PERM_READWRITE         // Read-write permissions
    );

    // Export Buffer from CPU to GPU
    export_prime_buffer(cpu_buffer, gpu_device);

    // Import Buffer to GPU
    import_prime_buffer(cpu_device, cpu_buffer, gpu_device);

    // Print Statistics
    print_prime_system_stats(system);

    // Cleanup
    destroy_prime_system(system);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_prime_buffer_management();

    return 0;
}
