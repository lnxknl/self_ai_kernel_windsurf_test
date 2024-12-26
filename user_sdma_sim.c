#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

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

// Memory Transfer Modes
typedef enum {
    SDMA_MODE_SCATTER_GATHER,
    SDMA_MODE_DIRECT,
    SDMA_MODE_STREAMING,
    SDMA_MODE_INTERRUPT_DRIVEN
} sdma_transfer_mode_t;

// Transfer Direction
typedef enum {
    SDMA_DIR_HOST_TO_DEVICE,
    SDMA_DIR_DEVICE_TO_HOST,
    SDMA_DIR_PEER_TO_PEER
} sdma_transfer_direction_t;

// Transfer Priority
typedef enum {
    SDMA_PRIORITY_LOW,
    SDMA_PRIORITY_NORMAL,
    SDMA_PRIORITY_HIGH,
    SDMA_PRIORITY_CRITICAL
} sdma_transfer_priority_t;

// Scatter-Gather Entry
typedef struct {
    void *address;
    size_t length;
} sdma_sg_entry_t;

// Transfer Descriptor
typedef struct sdma_transfer {
    uint64_t transfer_id;
    sdma_transfer_mode_t mode;
    sdma_transfer_direction_t direction;
    sdma_transfer_priority_t priority;
    
    sdma_sg_entry_t *sg_list;
    size_t sg_entries;
    
    size_t total_transfer_size;
    bool is_completed;
    bool has_error;
    
    struct sdma_transfer *next;
} sdma_transfer_t;

// SDMA Channel Statistics
typedef struct {
    unsigned long total_transfers;
    unsigned long successful_transfers;
    unsigned long failed_transfers;
    unsigned long bytes_transferred;
    unsigned long interrupt_count;
} sdma_channel_stats_t;

// SDMA Channel Configuration
typedef struct {
    uint32_t channel_id;
    size_t max_transfer_size;
    size_t max_sg_entries;
    bool interrupt_enabled;
    bool error_handling_enabled;
} sdma_channel_config_t;

// SDMA Channel
typedef struct sdma_channel {
    sdma_channel_config_t config;
    sdma_channel_stats_t stats;
    
    sdma_transfer_t *transfer_queue;
    sdma_transfer_t *current_transfer;
    
    // Callback for transfer completion
    void (*transfer_complete_callback)(struct sdma_channel *channel, sdma_transfer_t *transfer);
    
    // Callback for error handling
    void (*error_callback)(struct sdma_channel *channel, sdma_transfer_t *transfer);
} sdma_channel_t;

// SDMA System
typedef struct {
    sdma_channel_t *channels;
    size_t num_channels;
    size_t max_channels;
} sdma_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_transfer_mode_string(sdma_transfer_mode_t mode);
const char* get_transfer_direction_string(sdma_transfer_direction_t dir);
const char* get_transfer_priority_string(sdma_transfer_priority_t priority);

sdma_system_t* create_sdma_system(size_t max_channels);
void destroy_sdma_system(sdma_system_t *system);

sdma_channel_t* create_sdma_channel(
    sdma_system_t *system, 
    sdma_channel_config_t *config
);

sdma_transfer_t* create_sdma_transfer(
    sdma_channel_t *channel,
    sdma_transfer_mode_t mode,
    sdma_transfer_direction_t direction,
    sdma_transfer_priority_t priority,
    sdma_sg_entry_t *sg_list,
    size_t sg_entries
);

bool submit_transfer(
    sdma_channel_t *channel, 
    sdma_transfer_t *transfer
);

bool process_transfer(
    sdma_channel_t *channel, 
    sdma_transfer_t *transfer
);

void cancel_transfer(
    sdma_channel_t *channel, 
    sdma_transfer_t *transfer
);

void print_channel_stats(sdma_channel_t *channel);
void demonstrate_sdma_system();

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

// Utility Function: Get Transfer Mode String
const char* get_transfer_mode_string(sdma_transfer_mode_t mode) {
    switch(mode) {
        case SDMA_MODE_SCATTER_GATHER:     return "SCATTER_GATHER";
        case SDMA_MODE_DIRECT:             return "DIRECT";
        case SDMA_MODE_STREAMING:          return "STREAMING";
        case SDMA_MODE_INTERRUPT_DRIVEN:   return "INTERRUPT_DRIVEN";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Transfer Direction String
const char* get_transfer_direction_string(sdma_transfer_direction_t dir) {
    switch(dir) {
        case SDMA_DIR_HOST_TO_DEVICE:  return "HOST_TO_DEVICE";
        case SDMA_DIR_DEVICE_TO_HOST:  return "DEVICE_TO_HOST";
        case SDMA_DIR_PEER_TO_PEER:    return "PEER_TO_PEER";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Transfer Priority String
const char* get_transfer_priority_string(sdma_transfer_priority_t priority) {
    switch(priority) {
        case SDMA_PRIORITY_LOW:        return "LOW";
        case SDMA_PRIORITY_NORMAL:     return "NORMAL";
        case SDMA_PRIORITY_HIGH:       return "HIGH";
        case SDMA_PRIORITY_CRITICAL:   return "CRITICAL";
        default: return "UNKNOWN";
    }
}

// Create SDMA System
sdma_system_t* create_sdma_system(size_t max_channels) {
    sdma_system_t *system = malloc(sizeof(sdma_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate SDMA system");
        return NULL;
    }

    system->channels = calloc(max_channels, sizeof(sdma_channel_t));
    if (!system->channels) {
        free(system);
        LOG(LOG_LEVEL_ERROR, "Failed to allocate SDMA channels");
        return NULL;
    }

    system->num_channels = 0;
    system->max_channels = max_channels;

    return system;
}

// Create SDMA Channel
sdma_channel_t* create_sdma_channel(
    sdma_system_t *system, 
    sdma_channel_config_t *config
) {
    if (!system || system->num_channels >= system->max_channels) {
        LOG(LOG_LEVEL_ERROR, "Cannot create SDMA channel: system limit reached");
        return NULL;
    }

    sdma_channel_t *channel = &system->channels[system->num_channels];

    // Set channel configuration
    if (config) {
        channel->config = *config;
    } else {
        // Default configuration
        channel->config.channel_id = system->num_channels;
        channel->config.max_transfer_size = 1024 * 1024;  // 1MB
        channel->config.max_sg_entries = 16;
        channel->config.interrupt_enabled = true;
        channel->config.error_handling_enabled = true;
    }

    // Reset statistics
    memset(&channel->stats, 0, sizeof(sdma_channel_stats_t));

    // Reset transfer queue
    channel->transfer_queue = NULL;
    channel->current_transfer = NULL;

    // Reset callbacks
    channel->transfer_complete_callback = NULL;
    channel->error_callback = NULL;

    system->num_channels++;

    LOG(LOG_LEVEL_DEBUG, "Created SDMA Channel: ID %u", channel->config.channel_id);

    return channel;
}

// Create SDMA Transfer
sdma_transfer_t* create_sdma_transfer(
    sdma_channel_t *channel,
    sdma_transfer_mode_t mode,
    sdma_transfer_direction_t direction,
    sdma_transfer_priority_t priority,
    sdma_sg_entry_t *sg_list,
    size_t sg_entries
) {
    if (!channel || sg_entries == 0 || sg_entries > channel->config.max_sg_entries) {
        LOG(LOG_LEVEL_ERROR, "Invalid transfer parameters");
        return NULL;
    }

    sdma_transfer_t *transfer = malloc(sizeof(sdma_transfer_t));
    if (!transfer) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate transfer");
        return NULL;
    }

    // Generate unique transfer ID
    transfer->transfer_id = (uint64_t)transfer;

    // Set transfer parameters
    transfer->mode = mode;
    transfer->direction = direction;
    transfer->priority = priority;

    // Allocate and copy scatter-gather list
    transfer->sg_list = malloc(sg_entries * sizeof(sdma_sg_entry_t));
    if (!transfer->sg_list) {
        free(transfer);
        LOG(LOG_LEVEL_ERROR, "Failed to allocate scatter-gather list");
        return NULL;
    }
    memcpy(transfer->sg_list, sg_list, sg_entries * sizeof(sdma_sg_entry_t));
    transfer->sg_entries = sg_entries;

    // Calculate total transfer size
    transfer->total_transfer_size = 0;
    for (size_t i = 0; i < sg_entries; i++) {
        transfer->total_transfer_size += sg_list[i].length;
    }

    // Check transfer size limit
    if (transfer->total_transfer_size > channel->config.max_transfer_size) {
        free(transfer->sg_list);
        free(transfer);
        LOG(LOG_LEVEL_ERROR, "Transfer size exceeds channel limit");
        return NULL;
    }

    // Initialize transfer state
    transfer->is_completed = false;
    transfer->has_error = false;
    transfer->next = NULL;

    LOG(LOG_LEVEL_DEBUG, "Created Transfer: ID %lu, Size %zu bytes, Mode %s", 
        transfer->transfer_id, 
        transfer->total_transfer_size, 
        get_transfer_mode_string(mode));

    return transfer;
}

// Submit Transfer to Channel
bool submit_transfer(
    sdma_channel_t *channel, 
    sdma_transfer_t *transfer
) {
    if (!channel || !transfer) return false;

    // Add transfer to queue
    transfer->next = channel->transfer_queue;
    channel->transfer_queue = transfer;

    // Update channel statistics
    channel->stats.total_transfers++;

    LOG(LOG_LEVEL_INFO, "Submitted Transfer: ID %lu", transfer->transfer_id);

    return true;
}

// Process Transfer
bool process_transfer(
    sdma_channel_t *channel, 
    sdma_transfer_t *transfer
) {
    if (!channel || !transfer) return false;

    // Simulate transfer processing
    for (size_t i = 0; i < transfer->sg_entries; i++) {
        sdma_sg_entry_t *entry = &transfer->sg_list[i];
        
        // Simulate data transfer
        // In a real implementation, this would interact with hardware
        memset(entry->address, 0xAA, entry->length);

        LOG(LOG_LEVEL_DEBUG, "Processed SG Entry: Addr %p, Length %zu", 
            entry->address, entry->length);
    }

    // Mark transfer as completed
    transfer->is_completed = true;
    channel->stats.successful_transfers++;
    channel->stats.bytes_transferred += transfer->total_transfer_size;

    // Trigger completion callback
    if (channel->transfer_complete_callback) {
        channel->transfer_complete_callback(channel, transfer);
    }

    // Simulate interrupt if enabled
    if (channel->config.interrupt_enabled) {
        channel->stats.interrupt_count++;
        LOG(LOG_LEVEL_DEBUG, "Transfer Interrupt: ID %lu", transfer->transfer_id);
    }

    return true;
}

// Cancel Transfer
void cancel_transfer(
    sdma_channel_t *channel, 
    sdma_transfer_t *transfer
) {
    if (!channel || !transfer) return;

    // Mark transfer as failed
    transfer->has_error = true;
    channel->stats.failed_transfers++;

    // Trigger error callback
    if (channel->config.error_handling_enabled && channel->error_callback) {
        channel->error_callback(channel, transfer);
    }

    LOG(LOG_LEVEL_WARN, "Cancelled Transfer: ID %lu", transfer->transfer_id);
}

// Print Channel Statistics
void print_channel_stats(sdma_channel_t *channel) {
    if (!channel) return;

    printf("\nSDMA Channel Statistics:\n");
    printf("----------------------\n");
    printf("Channel ID:             %u\n", channel->config.channel_id);
    printf("Total Transfers:        %lu\n", channel->stats.total_transfers);
    printf("Successful Transfers:   %lu\n", channel->stats.successful_transfers);
    printf("Failed Transfers:       %lu\n", channel->stats.failed_transfers);
    printf("Bytes Transferred:      %lu\n", channel->stats.bytes_transferred);
    printf("Interrupt Count:        %lu\n", channel->stats.interrupt_count);
}

// Destroy Transfer
void destroy_transfer(sdma_transfer_t *transfer) {
    if (!transfer) return;

    free(transfer->sg_list);
    free(transfer);
}

// Destroy SDMA Channel
void destroy_sdma_channel(sdma_channel_t *channel) {
    if (!channel) return;

    // Free transfer queue
    sdma_transfer_t *transfer = channel->transfer_queue;
    while (transfer) {
        sdma_transfer_t *next = transfer->next;
        destroy_transfer(transfer);
        transfer = next;
    }
}

// Destroy SDMA System
void destroy_sdma_system(sdma_system_t *system) {
    if (!system) return;

    // Destroy all channels
    for (size_t i = 0; i < system->num_channels; i++) {
        destroy_sdma_channel(&system->channels[i]);
    }

    free(system->channels);
    free(system);
}

// Example Transfer Complete Callback
void transfer_complete_handler(
    sdma_channel_t *channel, 
    sdma_transfer_t *transfer
) {
    LOG(LOG_LEVEL_INFO, "Transfer %lu completed successfully", transfer->transfer_id);
}

// Example Error Callback
void error_handler(
    sdma_channel_t *channel, 
    sdma_transfer_t *transfer
) {
    LOG(LOG_LEVEL_ERROR, "Transfer %lu failed", transfer->transfer_id);
}

// Demonstration Function
void demonstrate_sdma_system() {
    // Create SDMA System
    sdma_system_t *sdma_system = create_sdma_system(4);

    // Channel Configuration
    sdma_channel_config_t channel_config = {
        .channel_id = 0,
        .max_transfer_size = 2 * 1024 * 1024,  // 2MB
        .max_sg_entries = 16,
        .interrupt_enabled = true,
        .error_handling_enabled = true
    };

    // Create SDMA Channel
    sdma_channel_t *channel = create_sdma_channel(
        sdma_system, 
        &channel_config
    );

    // Set Channel Callbacks
    channel->transfer_complete_callback = transfer_complete_handler;
    channel->error_callback = error_handler;

    // Prepare Scatter-Gather List
    void *buffer1 = malloc(64 * 1024);  // 64KB
    void *buffer2 = malloc(128 * 1024); // 128KB
    
    sdma_sg_entry_t sg_list[] = {
        { .address = buffer1, .length = 64 * 1024 },
        { .address = buffer2, .length = 128 * 1024 }
    };

    // Create Transfer
    sdma_transfer_t *transfer = create_sdma_transfer(
        channel,
        SDMA_MODE_SCATTER_GATHER,
        SDMA_DIR_HOST_TO_DEVICE,
        SDMA_PRIORITY_NORMAL,
        sg_list,
        2
    );

    // Submit Transfer
    submit_transfer(channel, transfer);

    // Process Transfer
    process_transfer(channel, transfer);

    // Print Channel Statistics
    print_channel_stats(channel);

    // Cleanup
    free(buffer1);
    free(buffer2);
    destroy_sdma_system(sdma_system);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_sdma_system();

    return 0;
}
