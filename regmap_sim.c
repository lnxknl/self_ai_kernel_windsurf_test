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

// Register Access Modes
typedef enum {
    REG_MODE_NONE        = 0x00,
    REG_MODE_READ        = 0x01,
    REG_MODE_WRITE       = 0x02,
    REG_MODE_READ_WRITE  = 0x03,
    REG_MODE_VOLATILE    = 0x04,
    REG_MODE_NO_ACCESS   = 0x08
} register_mode_t;

// Register Types
typedef enum {
    REG_TYPE_CONTROL,
    REG_TYPE_STATUS,
    REG_TYPE_DATA,
    REG_TYPE_CONFIG,
    REG_TYPE_INTERRUPT,
    REG_TYPE_RESERVED
} register_type_t;

// Register Range Structure
typedef struct register_range {
    uint32_t start;
    uint32_t end;
    register_mode_t mode;
    register_type_t type;
    struct register_range *next;
} register_range_t;

// Register Validation Function Type
typedef bool (*reg_validate_fn)(uint32_t value);

// Register Callback Function Types
typedef uint32_t (*reg_read_fn)(void *context);
typedef void (*reg_write_fn)(void *context, uint32_t value);

// Register Descriptor
typedef struct {
    uint32_t address;
    uint32_t default_value;
    register_mode_t mode;
    register_type_t type;
    reg_validate_fn validate;
    reg_read_fn read_callback;
    reg_write_fn write_callback;
    void *callback_context;
} register_descriptor_t;

// Register Map Configuration
typedef struct {
    register_range_t *ranges;
    register_descriptor_t *descriptors;
    size_t num_descriptors;
    bool endian_swap;
    bool cache_enabled;
} regmap_config_t;

// Register Map Statistics
typedef struct {
    unsigned long total_reads;
    unsigned long total_writes;
    unsigned long read_errors;
    unsigned long write_errors;
    unsigned long range_violations;
} regmap_stats_t;

// Register Map Structure
typedef struct {
    regmap_config_t config;
    regmap_stats_t stats;
    uint32_t *cache;
    size_t cache_size;
} regmap_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_reg_mode_string(register_mode_t mode);
const char* get_reg_type_string(register_type_t type);

regmap_t* create_register_map(size_t cache_size);
void destroy_register_map(regmap_t *regmap);

void add_register_range(
    regmap_t *regmap, 
    uint32_t start, 
    uint32_t end, 
    register_mode_t mode, 
    register_type_t type
);

void add_register_descriptor(
    regmap_t *regmap,
    uint32_t address,
    uint32_t default_value,
    register_mode_t mode,
    register_type_t type,
    reg_validate_fn validate,
    reg_read_fn read_callback,
    reg_write_fn write_callback,
    void *callback_context
);

bool validate_register_access(
    regmap_t *regmap, 
    uint32_t address, 
    bool is_write
);

uint32_t read_register(regmap_t *regmap, uint32_t address);
void write_register(regmap_t *regmap, uint32_t address, uint32_t value);

void print_regmap_stats(regmap_t *regmap);
void demonstrate_register_map();

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

// Utility Function: Get Register Mode String
const char* get_reg_mode_string(register_mode_t mode) {
    switch(mode) {
        case REG_MODE_NONE:       return "NONE";
        case REG_MODE_READ:       return "READ";
        case REG_MODE_WRITE:      return "WRITE";
        case REG_MODE_READ_WRITE: return "READ_WRITE";
        case REG_MODE_VOLATILE:   return "VOLATILE";
        case REG_MODE_NO_ACCESS:  return "NO_ACCESS";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Register Type String
const char* get_reg_type_string(register_type_t type) {
    switch(type) {
        case REG_TYPE_CONTROL:    return "CONTROL";
        case REG_TYPE_STATUS:     return "STATUS";
        case REG_TYPE_DATA:       return "DATA";
        case REG_TYPE_CONFIG:     return "CONFIG";
        case REG_TYPE_INTERRUPT:  return "INTERRUPT";
        case REG_TYPE_RESERVED:   return "RESERVED";
        default: return "UNKNOWN";
    }
}

// Create Register Map
regmap_t* create_register_map(size_t cache_size) {
    regmap_t *regmap = malloc(sizeof(regmap_t));
    if (!regmap) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate memory for register map");
        return NULL;
    }

    // Initialize configuration
    regmap->config.ranges = NULL;
    regmap->config.descriptors = NULL;
    regmap->config.num_descriptors = 0;
    regmap->config.endian_swap = false;
    regmap->config.cache_enabled = true;

    // Initialize cache
    regmap->cache = calloc(cache_size, sizeof(uint32_t));
    if (!regmap->cache) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate cache memory");
        free(regmap);
        return NULL;
    }
    regmap->cache_size = cache_size;

    // Reset statistics
    memset(&regmap->stats, 0, sizeof(regmap_stats_t));

    return regmap;
}

// Add Register Range
void add_register_range(
    regmap_t *regmap, 
    uint32_t start, 
    uint32_t end, 
    register_mode_t mode, 
    register_type_t type
) {
    if (!regmap) return;

    register_range_t *new_range = malloc(sizeof(register_range_t));
    if (!new_range) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate memory for register range");
        return;
    }

    new_range->start = start;
    new_range->end = end;
    new_range->mode = mode;
    new_range->type = type;
    new_range->next = regmap->config.ranges;
    regmap->config.ranges = new_range;

    LOG(LOG_LEVEL_DEBUG, "Added register range: 0x%x - 0x%x, Mode: %s, Type: %s", 
        start, end, get_reg_mode_string(mode), get_reg_type_string(type));
}

// Add Register Descriptor
void add_register_descriptor(
    regmap_t *regmap,
    uint32_t address,
    uint32_t default_value,
    register_mode_t mode,
    register_type_t type,
    reg_validate_fn validate,
    reg_read_fn read_callback,
    reg_write_fn write_callback,
    void *callback_context
) {
    if (!regmap) return;

    // Reallocate descriptors array
    size_t new_size = regmap->config.num_descriptors + 1;
    register_descriptor_t *new_descriptors = realloc(
        regmap->config.descriptors, 
        new_size * sizeof(register_descriptor_t)
    );

    if (!new_descriptors) {
        LOG(LOG_LEVEL_ERROR, "Failed to reallocate descriptors");
        return;
    }

    regmap->config.descriptors = new_descriptors;
    register_descriptor_t *descriptor = 
        &regmap->config.descriptors[regmap->config.num_descriptors];

    descriptor->address = address;
    descriptor->default_value = default_value;
    descriptor->mode = mode;
    descriptor->type = type;
    descriptor->validate = validate;
    descriptor->read_callback = read_callback;
    descriptor->write_callback = write_callback;
    descriptor->callback_context = callback_context;

    // Set default value in cache
    if (address < regmap->cache_size) {
        regmap->cache[address] = default_value;
    }

    regmap->config.num_descriptors++;

    LOG(LOG_LEVEL_DEBUG, "Added register descriptor: 0x%x, Default: 0x%x, Mode: %s", 
        address, default_value, get_reg_mode_string(mode));
}

// Validate Register Access
bool validate_register_access(
    regmap_t *regmap, 
    uint32_t address, 
    bool is_write
) {
    if (!regmap) return false;

    // Check register ranges
    register_range_t *current_range = regmap->config.ranges;
    while (current_range) {
        if (address >= current_range->start && address <= current_range->end) {
            register_mode_t mode = current_range->mode;
            
            if (is_write && !(mode & REG_MODE_WRITE)) {
                regmap->stats.write_errors++;
                LOG(LOG_LEVEL_WARN, "Write access denied to register 0x%x", address);
                return false;
            }
            
            if (!is_write && !(mode & REG_MODE_READ)) {
                regmap->stats.read_errors++;
                LOG(LOG_LEVEL_WARN, "Read access denied to register 0x%x", address);
                return false;
            }
            
            return true;
        }
        current_range = current_range->next;
    }

    // No matching range found
    regmap->stats.range_violations++;
    LOG(LOG_LEVEL_WARN, "Access to unregistered address 0x%x", address);
    return false;
}

// Read Register
uint32_t read_register(regmap_t *regmap, uint32_t address) {
    if (!regmap) return 0;

    regmap->stats.total_reads++;

    // Validate access
    if (!validate_register_access(regmap, address, false)) {
        return 0;
    }

    // Find descriptor for this address
    for (size_t i = 0; i < regmap->config.num_descriptors; i++) {
        register_descriptor_t *descriptor = 
            &regmap->config.descriptors[i];
        
        if (descriptor->address == address) {
            // Check for read callback
            if (descriptor->read_callback) {
                return descriptor->read_callback(descriptor->callback_context);
            }
            
            // Use cached value if cache is enabled
            if (regmap->config.cache_enabled && 
                address < regmap->cache_size) {
                return regmap->cache[address];
            }
        }
    }

    LOG(LOG_LEVEL_WARN, "No descriptor found for read at 0x%x", address);
    return 0;
}

// Write Register
void write_register(regmap_t *regmap, uint32_t address, uint32_t value) {
    if (!regmap) return;

    regmap->stats.total_writes++;

    // Validate access
    if (!validate_register_access(regmap, address, true)) {
        return;
    }

    // Find descriptor for this address
    for (size_t i = 0; i < regmap->config.num_descriptors; i++) {
        register_descriptor_t *descriptor = 
            &regmap->config.descriptors[i];
        
        if (descriptor->address == address) {
            // Validate value if validation function exists
            if (descriptor->validate && 
                !descriptor->validate(value)) {
                LOG(LOG_LEVEL_WARN, "Value validation failed for 0x%x", address);
                regmap->stats.write_errors++;
                return;
            }

            // Call write callback if exists
            if (descriptor->write_callback) {
                descriptor->write_callback(descriptor->callback_context, value);
            }

            // Update cache if enabled
            if (regmap->config.cache_enabled && 
                address < regmap->cache_size) {
                regmap->cache[address] = value;
            }

            return;
        }
    }

    LOG(LOG_LEVEL_WARN, "No descriptor found for write at 0x%x", address);
}

// Print Register Map Statistics
void print_regmap_stats(regmap_t *regmap) {
    if (!regmap) return;

    printf("\nRegister Map Statistics:\n");
    printf("----------------------\n");
    printf("Total Reads:        %lu\n", regmap->stats.total_reads);
    printf("Total Writes:       %lu\n", regmap->stats.total_writes);
    printf("Read Errors:        %lu\n", regmap->stats.read_errors);
    printf("Write Errors:       %lu\n", regmap->stats.write_errors);
    printf("Range Violations:   %lu\n", regmap->stats.range_violations);
}

// Destroy Register Map
void destroy_register_map(regmap_t *regmap) {
    if (!regmap) return;

    // Free register ranges
    register_range_t *current_range = regmap->config.ranges;
    while (current_range) {
        register_range_t *next = current_range->next;
        free(current_range);
        current_range = next;
    }

    // Free descriptors
    free(regmap->config.descriptors);

    // Free cache
    free(regmap->cache);

    // Free regmap
    free(regmap);
}

// Example Validation Function
bool validate_interrupt_mask(uint32_t value) {
    // Ensure only valid interrupt bits are set
    return (value & 0xFFFF) == value;
}

// Example Read Callback
uint32_t read_status_register(void *context) {
    // Simulated status reading logic
    int *status = (int*)context;
    return *status;
}

// Example Write Callback
void write_control_register(void *context, uint32_t value) {
    // Simulated control register write logic
    int *control = (int*)context;
    *control = value;
    LOG(LOG_LEVEL_DEBUG, "Control register updated: 0x%x", value);
}

// Demonstration Function
void demonstrate_register_map() {
    // Create register map
    regmap_t *regmap = create_register_map(1024);
    if (!regmap) {
        LOG(LOG_LEVEL_ERROR, "Failed to create register map");
        return;
    }

    // Simulate device-specific register configuration
    // Control Registers
    int control_state = 0;
    add_register_descriptor(
        regmap,
        0x1000,               // Address
        0x0000,               // Default value
        REG_MODE_READ_WRITE,  // Mode
        REG_TYPE_CONTROL,     // Type
        NULL,                 // No validation
        NULL,                 // No read callback
        write_control_register,  // Write callback
        &control_state        // Callback context
    );

    // Interrupt Mask Registers
    add_register_descriptor(
        regmap,
        0x1004,               // Address
        0x0000,               // Default value
        REG_MODE_READ_WRITE,  // Mode
        REG_TYPE_INTERRUPT,   // Type
        validate_interrupt_mask,  // Validation function
        NULL,                 // No read callback
        NULL,                 // No write callback
        NULL                  // No context
    );

    // Status Registers
    int status_state = 0x00FF;
    add_register_descriptor(
        regmap,
        0x2000,               // Address
        0x0000,               // Default value
        REG_MODE_READ,        // Read-only
        REG_TYPE_STATUS,      // Type
        NULL,                 // No validation
        read_status_register, // Read callback
        NULL,                 // No write callback
        &status_state         // Callback context
    );

    // Define register ranges
    add_register_range(
        regmap, 
        0x1000, 0x1FFF,       // Control and Interrupt Range
        REG_MODE_READ_WRITE,  // Access mode
        REG_TYPE_CONTROL      // Type
    );

    add_register_range(
        regmap, 
        0x2000, 0x2FFF,       // Status Register Range
        REG_MODE_READ,        // Read-only
        REG_TYPE_STATUS       // Type
    );

    // Demonstrate register operations
    LOG(LOG_LEVEL_INFO, "Writing to control register");
    write_register(regmap, 0x1000, 0x0001);

    LOG(LOG_LEVEL_INFO, "Writing to interrupt mask");
    write_register(regmap, 0x1004, 0x00FF);

    LOG(LOG_LEVEL_INFO, "Reading status register");
    uint32_t status = read_register(regmap, 0x2000);
    LOG(LOG_LEVEL_INFO, "Status: 0x%x", status);

    // Print statistics
    print_regmap_stats(regmap);

    // Cleanup
    destroy_register_map(regmap);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_register_map();

    return 0;
}
