#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>

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

// ARP Constants
#define MAX_ARP_ENTRIES    1024
#define MAX_PENDING_PKTS   64
#define ARP_CACHE_TIMEOUT  300    // 5 minutes
#define ARP_RETRY_TIME     1      // 1 second
#define MAX_ARP_RETRIES    3
#define TEST_DURATION      30     // seconds

// Hardware Types
#define ARPHRD_ETHER      1
#define ARPHRD_IEEE802    6

// Protocol Types
#define ETH_P_IP         0x0800
#define ETH_P_ARP        0x0806

// ARP Operation Codes
#define ARPOP_REQUEST    1
#define ARPOP_REPLY      2
#define ARPOP_RREQUEST   3
#define ARPOP_RREPLY     4

// Ethernet Address Length
#define ETH_ALEN         6
#define IPV4_ALEN        4

// ARP Entry States
typedef enum {
    ARP_NONE,
    ARP_INCOMPLETE,
    ARP_VALID,
    ARP_FAILED
} arp_state_t;

// MAC Address Structure
typedef struct {
    uint8_t addr[ETH_ALEN];
} mac_addr_t;

// ARP Packet Structure
typedef struct {
    uint16_t hw_type;
    uint16_t protocol;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t operation;
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;
} __attribute__((packed)) arp_packet_t;

// Pending Packet Structure
typedef struct pending_pkt {
    void *data;
    size_t len;
    struct pending_pkt *next;
} pending_pkt_t;

// ARP Entry Structure
typedef struct arp_entry {
    uint32_t ip;
    mac_addr_t mac;
    arp_state_t state;
    time_t created;
    time_t updated;
    int retry_count;
    pending_pkt_t *pending;
    struct arp_entry *next;
    pthread_mutex_t lock;
} arp_entry_t;

// Statistics Structure
typedef struct {
    uint64_t requests_sent;
    uint64_t requests_received;
    uint64_t replies_sent;
    uint64_t replies_received;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_timeouts;
    uint64_t retries;
    uint64_t failed_resolves;
} arp_stats_t;

// ARP Manager Structure
typedef struct {
    arp_entry_t *entries[MAX_ARP_ENTRIES];
    size_t nr_entries;
    bool running;
    pthread_mutex_t manager_lock;
    pthread_t gc_thread;
    pthread_t retry_thread;
    arp_stats_t stats;
} arp_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_arp_state_string(arp_state_t state);
void mac_addr_to_string(const mac_addr_t *mac, char *str);

arp_manager_t* create_arp_manager(void);
void destroy_arp_manager(arp_manager_t *manager);

arp_entry_t* create_arp_entry(uint32_t ip);
void destroy_arp_entry(arp_entry_t *entry);

int arp_add_entry(arp_manager_t *manager, uint32_t ip, const mac_addr_t *mac);
int arp_remove_entry(arp_manager_t *manager, uint32_t ip);
arp_entry_t* arp_find_entry(arp_manager_t *manager, uint32_t ip);

void arp_process_request(arp_manager_t *manager, const arp_packet_t *pkt);
void arp_process_reply(arp_manager_t *manager, const arp_packet_t *pkt);
void arp_send_request(arp_manager_t *manager, uint32_t target_ip);
void arp_send_reply(arp_manager_t *manager, const arp_packet_t *request);

void* gc_thread(void *arg);
void* retry_thread(void *arg);
void process_timeouts(arp_manager_t *manager);
void process_retries(arp_manager_t *manager);

void run_test(arp_manager_t *manager);
void calculate_stats(arp_manager_t *manager);
void print_test_stats(arp_manager_t *manager);
void demonstrate_arp(void);

// Utility Functions
const char* get_log_level_string(int level) {
    switch(level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

const char* get_arp_state_string(arp_state_t state) {
    switch(state) {
        case ARP_NONE:       return "NONE";
        case ARP_INCOMPLETE: return "INCOMPLETE";
        case ARP_VALID:      return "VALID";
        case ARP_FAILED:     return "FAILED";
        default: return "UNKNOWN";
    }
}

void mac_addr_to_string(const mac_addr_t *mac, char *str) {
    snprintf(str, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
        mac->addr[0], mac->addr[1], mac->addr[2],
        mac->addr[3], mac->addr[4], mac->addr[5]);
}

// Create ARP Entry
arp_entry_t* create_arp_entry(uint32_t ip) {
    arp_entry_t *entry = malloc(sizeof(arp_entry_t));
    if (!entry) return NULL;

    entry->ip = ip;
    memset(&entry->mac, 0, sizeof(mac_addr_t));
    entry->state = ARP_INCOMPLETE;
    entry->created = time(NULL);
    entry->updated = entry->created;
    entry->retry_count = 0;
    entry->pending = NULL;
    entry->next = NULL;
    pthread_mutex_init(&entry->lock, NULL);

    return entry;
}

// Create ARP Manager
arp_manager_t* create_arp_manager(void) {
    arp_manager_t *manager = malloc(sizeof(arp_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate ARP manager");
        return NULL;
    }

    memset(manager->entries, 0, sizeof(manager->entries));
    manager->nr_entries = 0;
    manager->running = false;
    pthread_mutex_init(&manager->manager_lock, NULL);
    memset(&manager->stats, 0, sizeof(arp_stats_t));

    LOG(LOG_LEVEL_DEBUG, "Created ARP manager");
    return manager;
}

// Add ARP Entry
int arp_add_entry(arp_manager_t *manager, uint32_t ip, const mac_addr_t *mac) {
    if (!manager || !mac) return -1;

    pthread_mutex_lock(&manager->manager_lock);

    // Check if entry exists
    arp_entry_t *entry = arp_find_entry(manager, ip);
    if (entry) {
        pthread_mutex_lock(&entry->lock);
        memcpy(&entry->mac, mac, sizeof(mac_addr_t));
        entry->state = ARP_VALID;
        entry->updated = time(NULL);
        pthread_mutex_unlock(&entry->lock);
    } else {
        // Create new entry
        if (manager->nr_entries >= MAX_ARP_ENTRIES) {
            pthread_mutex_unlock(&manager->manager_lock);
            return -1;
        }

        entry = create_arp_entry(ip);
        if (!entry) {
            pthread_mutex_unlock(&manager->manager_lock);
            return -1;
        }

        memcpy(&entry->mac, mac, sizeof(mac_addr_t));
        entry->state = ARP_VALID;
        manager->entries[manager->nr_entries++] = entry;
    }

    pthread_mutex_unlock(&manager->manager_lock);

    char mac_str[18];
    mac_addr_to_string(mac, mac_str);
    LOG(LOG_LEVEL_DEBUG, "Added ARP entry: IP %s -> MAC %s",
        inet_ntoa((struct in_addr){.s_addr = ip}), mac_str);
    return 0;
}

// Find ARP Entry
arp_entry_t* arp_find_entry(arp_manager_t *manager, uint32_t ip) {
    if (!manager) return NULL;

    for (size_t i = 0; i < manager->nr_entries; i++) {
        if (manager->entries[i] && manager->entries[i]->ip == ip) {
            return manager->entries[i];
        }
    }
    return NULL;
}

// Process ARP Request
void arp_process_request(arp_manager_t *manager, const arp_packet_t *pkt) {
    if (!manager || !pkt) return;

    manager->stats.requests_received++;

    // Add sender to cache
    mac_addr_t sender_mac;
    memcpy(sender_mac.addr, pkt->sender_mac, ETH_ALEN);
    arp_add_entry(manager, pkt->sender_ip, &sender_mac);

    // Send reply if we're the target
    if (pkt->target_ip == htonl(INADDR_ANY)) {  // Simulate our IP
        arp_send_reply(manager, pkt);
    }
}

// Process ARP Reply
void arp_process_reply(arp_manager_t *manager, const arp_packet_t *pkt) {
    if (!manager || !pkt) return;

    manager->stats.replies_received++;

    // Add sender to cache
    mac_addr_t sender_mac;
    memcpy(sender_mac.addr, pkt->sender_mac, ETH_ALEN);
    arp_add_entry(manager, pkt->sender_ip, &sender_mac);
}

// Send ARP Request
void arp_send_request(arp_manager_t *manager, uint32_t target_ip) {
    if (!manager) return;

    arp_packet_t request = {
        .hw_type = htons(ARPHRD_ETHER),
        .protocol = htons(ETH_P_IP),
        .hw_len = ETH_ALEN,
        .proto_len = IPV4_ALEN,
        .operation = htons(ARPOP_REQUEST),
        .sender_ip = htonl(INADDR_ANY),  // Simulate our IP
        .target_ip = target_ip
    };

    // Simulate broadcast
    memset(request.target_mac, 0xFF, ETH_ALEN);

    manager->stats.requests_sent++;
    LOG(LOG_LEVEL_DEBUG, "Sent ARP request for IP %s",
        inet_ntoa((struct in_addr){.s_addr = target_ip}));
}

// Send ARP Reply
void arp_send_reply(arp_manager_t *manager, const arp_packet_t *request) {
    if (!manager || !request) return;

    arp_packet_t reply = {
        .hw_type = htons(ARPHRD_ETHER),
        .protocol = htons(ETH_P_IP),
        .hw_len = ETH_ALEN,
        .proto_len = IPV4_ALEN,
        .operation = htons(ARPOP_REPLY),
        .sender_ip = request->target_ip,
        .target_ip = request->sender_ip
    };

    // Set sender/target MAC addresses
    memcpy(reply.target_mac, request->sender_mac, ETH_ALEN);
    // Simulate our MAC address
    uint8_t our_mac[ETH_ALEN] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(reply.sender_mac, our_mac, ETH_ALEN);

    manager->stats.replies_sent++;
    LOG(LOG_LEVEL_DEBUG, "Sent ARP reply to IP %s",
        inet_ntoa((struct in_addr){.s_addr = request->sender_ip}));
}

// GC Thread
void* gc_thread(void *arg) {
    arp_manager_t *manager = (arp_manager_t*)arg;

    while (manager->running) {
        process_timeouts(manager);
        sleep(1);
    }

    return NULL;
}

// Process Timeouts
void process_timeouts(arp_manager_t *manager) {
    if (!manager) return;

    time_t now = time(NULL);
    pthread_mutex_lock(&manager->manager_lock);

    for (size_t i = 0; i < manager->nr_entries; i++) {
        arp_entry_t *entry = manager->entries[i];
        if (!entry) continue;

        pthread_mutex_lock(&entry->lock);
        if (entry->state == ARP_VALID &&
            (now - entry->updated) > ARP_CACHE_TIMEOUT) {
            entry->state = ARP_NONE;
            manager->stats.cache_timeouts++;
            LOG(LOG_LEVEL_DEBUG, "ARP entry timed out: IP %s",
                inet_ntoa((struct in_addr){.s_addr = entry->ip}));
        }
        pthread_mutex_unlock(&entry->lock);
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Retry Thread
void* retry_thread(void *arg) {
    arp_manager_t *manager = (arp_manager_t*)arg;

    while (manager->running) {
        process_retries(manager);
        usleep(100000);  // 100ms interval
    }

    return NULL;
}

// Process Retries
void process_retries(arp_manager_t *manager) {
    if (!manager) return;

    time_t now = time(NULL);
    pthread_mutex_lock(&manager->manager_lock);

    for (size_t i = 0; i < manager->nr_entries; i++) {
        arp_entry_t *entry = manager->entries[i];
        if (!entry) continue;

        pthread_mutex_lock(&entry->lock);
        if (entry->state == ARP_INCOMPLETE &&
            (now - entry->updated) > ARP_RETRY_TIME) {
            if (entry->retry_count < MAX_ARP_RETRIES) {
                arp_send_request(manager, entry->ip);
                entry->retry_count++;
                entry->updated = now;
                manager->stats.retries++;
            } else {
                entry->state = ARP_FAILED;
                manager->stats.failed_resolves++;
                LOG(LOG_LEVEL_DEBUG, "ARP resolution failed: IP %s",
                    inet_ntoa((struct in_addr){.s_addr = entry->ip}));
            }
        }
        pthread_mutex_unlock(&entry->lock);
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Run Test
void run_test(arp_manager_t *manager) {
    if (!manager) return;

    LOG(LOG_LEVEL_INFO, "Starting ARP test...");

    // Start threads
    manager->running = true;
    pthread_create(&manager->gc_thread, NULL, gc_thread, manager);
    pthread_create(&manager->retry_thread, NULL, retry_thread, manager);

    // Simulate ARP traffic
    for (int i = 0; i < TEST_DURATION; i++) {
        // Simulate incoming requests
        if (rand() % 100 < 30) {  // 30% chance
            arp_packet_t request = {
                .hw_type = htons(ARPHRD_ETHER),
                .protocol = htons(ETH_P_IP),
                .hw_len = ETH_ALEN,
                .proto_len = IPV4_ALEN,
                .operation = htons(ARPOP_REQUEST),
                .sender_ip = htonl(rand()),
                .target_ip = htonl(INADDR_ANY)
            };
            uint8_t random_mac[ETH_ALEN];
            for (int j = 0; j < ETH_ALEN; j++) {
                random_mac[j] = rand() % 256;
            }
            memcpy(request.sender_mac, random_mac, ETH_ALEN);
            arp_process_request(manager, &request);
        }

        // Simulate outgoing requests
        if (rand() % 100 < 20) {  // 20% chance
            arp_send_request(manager, htonl(rand()));
        }

        sleep(1);
    }

    // Stop threads
    manager->running = false;
    pthread_join(manager->gc_thread, NULL);
    pthread_join(manager->retry_thread, NULL);

    // Calculate statistics
    calculate_stats(manager);
}

// Calculate Statistics
void calculate_stats(arp_manager_t *manager) {
    if (!manager) return;

    // Count current cache entries by state
    size_t valid = 0, incomplete = 0, failed = 0;
    for (size_t i = 0; i < manager->nr_entries; i++) {
        arp_entry_t *entry = manager->entries[i];
        if (!entry) continue;

        switch (entry->state) {
            case ARP_VALID: valid++; break;
            case ARP_INCOMPLETE: incomplete++; break;
            case ARP_FAILED: failed++; break;
            default: break;
        }
    }

    LOG(LOG_LEVEL_INFO, "Cache entries: %zu valid, %zu incomplete, %zu failed",
        valid, incomplete, failed);
}

// Print Test Statistics
void print_test_stats(arp_manager_t *manager) {
    if (!manager) return;

    printf("\nARP Test Results:\n");
    printf("-------------------------\n");
    printf("Test Duration:     %d seconds\n", TEST_DURATION);
    printf("Requests Sent:     %lu\n", manager->stats.requests_sent);
    printf("Requests Received: %lu\n", manager->stats.requests_received);
    printf("Replies Sent:      %lu\n", manager->stats.replies_sent);
    printf("Replies Received:  %lu\n", manager->stats.replies_received);
    printf("Cache Hits:        %lu\n", manager->stats.cache_hits);
    printf("Cache Misses:      %lu\n", manager->stats.cache_misses);
    printf("Cache Timeouts:    %lu\n", manager->stats.cache_timeouts);
    printf("Retries:          %lu\n", manager->stats.retries);
    printf("Failed Resolves:   %lu\n", manager->stats.failed_resolves);

    // Print cache details
    printf("\nCache Details:\n");
    for (size_t i = 0; i < manager->nr_entries; i++) {
        arp_entry_t *entry = manager->entries[i];
        if (!entry) continue;

        char mac_str[18];
        mac_addr_to_string(&entry->mac, mac_str);
        printf("  IP: %-15s  MAC: %-17s  State: %s\n",
            inet_ntoa((struct in_addr){.s_addr = entry->ip}),
            mac_str, get_arp_state_string(entry->state));
    }
}

// Destroy ARP Entry
void destroy_arp_entry(arp_entry_t *entry) {
    if (!entry) return;

    // Free pending packets
    pending_pkt_t *pkt = entry->pending;
    while (pkt) {
        pending_pkt_t *next = pkt->next;
        free(pkt->data);
        free(pkt);
        pkt = next;
    }

    pthread_mutex_destroy(&entry->lock);
    free(entry);
}

// Destroy ARP Manager
void destroy_arp_manager(arp_manager_t *manager) {
    if (!manager) return;

    // Clean up entries
    for (size_t i = 0; i < manager->nr_entries; i++) {
        if (manager->entries[i]) {
            destroy_arp_entry(manager->entries[i]);
        }
    }

    pthread_mutex_destroy(&manager->manager_lock);
    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed ARP manager");
}

// Demonstrate ARP
void demonstrate_arp(void) {
    printf("Starting ARP demonstration...\n");

    // Create and run ARP test
    arp_manager_t *manager = create_arp_manager();
    if (manager) {
        run_test(manager);
        print_test_stats(manager);
        destroy_arp_manager(manager);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_arp();

    return 0;
}
