#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

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

// Register Types
typedef enum {
    REG_TYPE_CONTROL,
    REG_TYPE_STATUS,
    REG_TYPE_DATA,
    REG_TYPE_CONFIG,
    REG_TYPE_INTERRUPT
} register_type_t;

// Register Access Permissions
typedef enum {
    REG_PERM_READ_ONLY,
    REG_PERM_WRITE_ONLY,
    REG_PERM_READ_WRITE,
    REG_PERM_NO_ACCESS
} register_permission_t;

// Node Color for Red-Black Tree
typedef enum {
    RB_COLOR_RED,
    RB_COLOR_BLACK
} rb_color_t;

// Register Cache Entry
typedef struct reg_cache_entry {
    uint32_t address;            // Register address
    uint32_t value;              // Register value
    register_type_t type;        // Register type
    register_permission_t perm;  // Access permissions
    bool is_dirty;               // Modified but not synced
    bool is_cached;              // Currently in cache

    // Red-Black Tree Node Properties
    rb_color_t color;
    struct reg_cache_entry *parent;
    struct reg_cache_entry *left;
    struct reg_cache_entry *right;
} reg_cache_entry_t;

// Register Cache Statistics
typedef struct {
    unsigned long total_reads;
    unsigned long total_writes;
    unsigned long cache_hits;
    unsigned long cache_misses;
    unsigned long dirty_entries;
} reg_cache_stats_t;

// Register Cache Configuration
typedef struct {
    size_t max_cache_size;       // Maximum number of entries
    bool write_through;          // Immediate write to hardware
    bool cache_enabled;          // Cache functionality enabled
} reg_cache_config_t;

// Register Cache Management Structure
typedef struct {
    reg_cache_entry_t *root;     // Root of Red-Black Tree
    reg_cache_stats_t stats;     // Performance statistics
    reg_cache_config_t config;   // Cache configuration
} reg_cache_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_reg_type_string(register_type_t type);
const char* get_reg_perm_string(register_permission_t perm);

reg_cache_t* create_register_cache(size_t max_size, bool write_through);
void destroy_register_cache(reg_cache_t *cache);

reg_cache_entry_t* create_cache_entry(
    uint32_t address, 
    uint32_t value, 
    register_type_t type, 
    register_permission_t perm
);

void rotate_left(reg_cache_t *cache, reg_cache_entry_t *x);
void rotate_right(reg_cache_t *cache, reg_cache_entry_t *x);
void insert_fixup(reg_cache_t *cache, reg_cache_entry_t *z);
void delete_fixup(reg_cache_t *cache, reg_cache_entry_t *x);

void insert_register_entry(reg_cache_t *cache, reg_cache_entry_t *entry);
reg_cache_entry_t* find_register_entry(reg_cache_t *cache, uint32_t address);
void delete_register_entry(reg_cache_t *cache, uint32_t address);

uint32_t read_register(reg_cache_t *cache, uint32_t address);
void write_register(reg_cache_t *cache, uint32_t address, uint32_t value);
void sync_dirty_registers(reg_cache_t *cache);

void print_cache_stats(reg_cache_t *cache);
void demonstrate_register_cache();

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

// Utility Function: Get Register Type String
const char* get_reg_type_string(register_type_t type) {
    switch(type) {
        case REG_TYPE_CONTROL:    return "CONTROL";
        case REG_TYPE_STATUS:     return "STATUS";
        case REG_TYPE_DATA:       return "DATA";
        case REG_TYPE_CONFIG:     return "CONFIG";
        case REG_TYPE_INTERRUPT:  return "INTERRUPT";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Register Permission String
const char* get_reg_perm_string(register_permission_t perm) {
    switch(perm) {
        case REG_PERM_READ_ONLY:  return "READ_ONLY";
        case REG_PERM_WRITE_ONLY: return "WRITE_ONLY";
        case REG_PERM_READ_WRITE: return "READ_WRITE";
        case REG_PERM_NO_ACCESS:  return "NO_ACCESS";
        default: return "UNKNOWN";
    }
}

// Create Register Cache
reg_cache_t* create_register_cache(size_t max_size, bool write_through) {
    reg_cache_t *cache = malloc(sizeof(reg_cache_t));
    if (!cache) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate memory for register cache");
        return NULL;
    }

    // Initialize cache
    cache->root = NULL;
    cache->config.max_cache_size = max_size;
    cache->config.write_through = write_through;
    cache->config.cache_enabled = true;

    // Reset statistics
    memset(&cache->stats, 0, sizeof(reg_cache_stats_t));

    return cache;
}

// Create Cache Entry
reg_cache_entry_t* create_cache_entry(
    uint32_t address, 
    uint32_t value, 
    register_type_t type, 
    register_permission_t perm
) {
    reg_cache_entry_t *entry = malloc(sizeof(reg_cache_entry_t));
    if (!entry) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate memory for cache entry");
        return NULL;
    }

    entry->address = address;
    entry->value = value;
    entry->type = type;
    entry->perm = perm;
    entry->is_dirty = false;
    entry->is_cached = true;

    // Red-Black Tree initialization
    entry->color = RB_COLOR_RED;
    entry->parent = NULL;
    entry->left = NULL;
    entry->right = NULL;

    return entry;
}

// Rotate Left in Red-Black Tree
void rotate_left(reg_cache_t *cache, reg_cache_entry_t *x) {
    reg_cache_entry_t *y = x->right;
    x->right = y->left;
    
    if (y->left != NULL) {
        y->left->parent = x;
    }
    
    y->parent = x->parent;
    
    if (x->parent == NULL) {
        cache->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    
    y->left = x;
    x->parent = y;
}

// Rotate Right in Red-Black Tree
void rotate_right(reg_cache_t *cache, reg_cache_entry_t *x) {
    reg_cache_entry_t *y = x->left;
    x->left = y->right;
    
    if (y->right != NULL) {
        y->right->parent = x;
    }
    
    y->parent = x->parent;
    
    if (x->parent == NULL) {
        cache->root = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }
    
    y->right = x;
    x->parent = y;
}

// Fix Red-Black Tree after Insertion
void insert_fixup(reg_cache_t *cache, reg_cache_entry_t *z) {
    while (z->parent != NULL && z->parent->color == RB_COLOR_RED) {
        if (z->parent == z->parent->parent->left) {
            reg_cache_entry_t *y = z->parent->parent->right;
            
            if (y != NULL && y->color == RB_COLOR_RED) {
                z->parent->color = RB_COLOR_BLACK;
                y->color = RB_COLOR_BLACK;
                z->parent->parent->color = RB_COLOR_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    rotate_left(cache, z);
                }
                
                z->parent->color = RB_COLOR_BLACK;
                z->parent->parent->color = RB_COLOR_RED;
                rotate_right(cache, z->parent->parent);
            }
        } else {
            reg_cache_entry_t *y = z->parent->parent->left;
            
            if (y != NULL && y->color == RB_COLOR_RED) {
                z->parent->color = RB_COLOR_BLACK;
                y->color = RB_COLOR_BLACK;
                z->parent->parent->color = RB_COLOR_RED;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rotate_right(cache, z);
                }
                
                z->parent->color = RB_COLOR_BLACK;
                z->parent->parent->color = RB_COLOR_RED;
                rotate_left(cache, z->parent->parent);
            }
        }
        
        if (z == cache->root) {
            break;
        }
    }
    
    cache->root->color = RB_COLOR_BLACK;
}

// Insert Register Entry
void insert_register_entry(reg_cache_t *cache, reg_cache_entry_t *entry) {
    if (!cache || !entry) return;

    // First insertion
    if (cache->root == NULL) {
        cache->root = entry;
        entry->color = RB_COLOR_BLACK;
        return;
    }

    // Standard BST insertion
    reg_cache_entry_t *parent = NULL;
    reg_cache_entry_t *current = cache->root;
    
    while (current != NULL) {
        parent = current;
        if (entry->address < current->address) {
            current = current->left;
        } else if (entry->address > current->address) {
            current = current->right;
        } else {
            // Address already exists, update value
            current->value = entry->value;
            current->is_dirty = true;
            free(entry);
            return;
        }
    }

    // Link the new node
    entry->parent = parent;
    if (entry->address < parent->address) {
        parent->left = entry;
    } else {
        parent->right = entry;
    }

    // Fix Red-Black Tree properties
    insert_fixup(cache, entry);
}

// Find Register Entry
reg_cache_entry_t* find_register_entry(reg_cache_t *cache, uint32_t address) {
    if (!cache) return NULL;

    reg_cache_entry_t *current = cache->root;
    while (current != NULL) {
        if (address == current->address) {
            return current;
        } else if (address < current->address) {
            current = current->left;
        } else {
            current = current->right;
        }
    }

    return NULL;
}

// Read Register
uint32_t read_register(reg_cache_t *cache, uint32_t address) {
    if (!cache || !cache->config.cache_enabled) {
        LOG(LOG_LEVEL_WARN, "Cache not enabled");
        return 0;
    }

    cache->stats.total_reads++;
    reg_cache_entry_t *entry = find_register_entry(cache, address);

    if (entry) {
        cache->stats.cache_hits++;
        
        if (entry->perm == REG_PERM_WRITE_ONLY) {
            LOG(LOG_LEVEL_WARN, "Attempted read on write-only register");
            return 0;
        }

        LOG(LOG_LEVEL_DEBUG, "Cache hit: Read register 0x%x, value 0x%x", 
            address, entry->value);
        return entry->value;
    }

    cache->stats.cache_misses++;
    LOG(LOG_LEVEL_DEBUG, "Cache miss: Register 0x%x not found", address);
    return 0;
}

// Write Register
void write_register(reg_cache_t *cache, uint32_t address, uint32_t value) {
    if (!cache || !cache->config.cache_enabled) {
        LOG(LOG_LEVEL_WARN, "Cache not enabled");
        return;
    }

    cache->stats.total_writes++;
    reg_cache_entry_t *entry = find_register_entry(cache, address);

    if (!entry) {
        // Create new entry if not exists
        entry = create_cache_entry(
            address, 
            value, 
            REG_TYPE_DATA,  // Default type
            REG_PERM_READ_WRITE  // Default permission
        );
        
        if (entry) {
            insert_register_entry(cache, entry);
        }
    }

    if (entry) {
        if (entry->perm == REG_PERM_READ_ONLY) {
            LOG(LOG_LEVEL_WARN, "Attempted write to read-only register");
            return;
        }

        entry->value = value;
        entry->is_dirty = true;
        cache->stats.dirty_entries++;

        if (cache->config.write_through) {
            // Simulate immediate hardware write
            LOG(LOG_LEVEL_DEBUG, "Write-through: Register 0x%x = 0x%x", 
                address, value);
        }
    }
}

// Synchronize Dirty Registers
void sync_dirty_registers(reg_cache_t *cache) {
    if (!cache) return;

    LOG(LOG_LEVEL_INFO, "Synchronizing dirty registers");
    // In a real implementation, this would flush dirty entries to hardware
    cache->stats.dirty_entries = 0;
}

// Print Cache Statistics
void print_cache_stats(reg_cache_t *cache) {
    if (!cache) return;

    printf("\nRegister Cache Statistics:\n");
    printf("------------------------\n");
    printf("Total Reads:      %lu\n", cache->stats.total_reads);
    printf("Total Writes:     %lu\n", cache->stats.total_writes);
    printf("Cache Hits:       %lu\n", cache->stats.cache_hits);
    printf("Cache Misses:     %lu\n", cache->stats.cache_misses);
    printf("Dirty Entries:    %lu\n", cache->stats.dirty_entries);
}

// Demonstration Function
void demonstrate_register_cache() {
    // Create register cache
    reg_cache_t *cache = create_register_cache(100, true);
    if (!cache) {
        LOG(LOG_LEVEL_ERROR, "Failed to create register cache");
        return;
    }

    // Simulate various register operations
    // Simulating a hypothetical device with different register types

    // Control Registers
    write_register(cache, 0x1000, 0x0001);  // Device enable
    write_register(cache, 0x1004, 0x00FF);  // Interrupt mask

    // Status Registers
    write_register(cache, 0x2000, 0x0000);  // Initial status
    
    // Data Registers
    write_register(cache, 0x3000, 0x1234);  // Data register 1
    write_register(cache, 0x3004, 0x5678);  // Data register 2

    // Read some registers
    uint32_t control_reg = read_register(cache, 0x1000);
    uint32_t data_reg = read_register(cache, 0x3000);

    LOG(LOG_LEVEL_INFO, "Control Register: 0x%x", control_reg);
    LOG(LOG_LEVEL_INFO, "Data Register:    0x%x", data_reg);

    // Synchronize dirty registers
    sync_dirty_registers(cache);

    // Print statistics
    print_cache_stats(cache);

    // Cleanup
    destroy_register_cache(cache);
}

// Destroy Register Cache
void destroy_register_cache(reg_cache_t *cache) {
    if (!cache) return;

    // Recursive free of Red-Black Tree
    void free_tree(reg_cache_entry_t *node) {
        if (node == NULL) return;
        free_tree(node->left);
        free_tree(node->right);
        free(node);
    }

    free_tree(cache->root);
    free(cache);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_register_cache();

    return 0;
}
