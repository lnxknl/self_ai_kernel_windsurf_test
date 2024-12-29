/*
 * TCP Metrics Simulation
 * Based on Linux kernel's TCP metrics implementation
 * 
 * This simulation implements TCP metrics tracking and caching including:
 * - RTT measurements
 * - Congestion window tracking
 * - TCP Fast Open support
 * - Metrics persistence and aging
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/time.h>
#include <arpa/inet.h>

/* Constants */
#define TCP_METRICS_TIMEOUT (60 * 60)  // 1 hour in seconds
#define TCP_METRICS_RECLAIM_DEPTH 5
#define TCP_METRICS_MAX_ENTRIES 1024
#define TCP_FASTOPEN_KEY_LENGTH 16
#define USEC_PER_MSEC 1000

/* TCP Metric Types */
enum tcp_metric_index {
    TCP_METRIC_RTT,
    TCP_METRIC_RTTVAR,
    TCP_METRIC_SSTHRESH,
    TCP_METRIC_CWND,
    TCP_METRIC_REORDERING,
    TCP_METRIC_MAX
};

/* Address Structure */
struct inetpeer_addr {
    uint32_t family;  // AF_INET or AF_INET6
    union {
        uint32_t v4;  // IPv4 address
        struct {
            uint32_t v6[4];  // IPv6 address
        };
    } addr;
};

/* TCP Fast Open Cookie */
struct tcp_fastopen_cookie {
    uint8_t key[TCP_FASTOPEN_KEY_LENGTH];
    uint8_t len;
    bool exp;  // Cookie is experimental
};

/* TCP Fast Open Metrics */
struct tcp_fastopen_metrics {
    uint16_t mss;
    uint16_t syn_loss:10,
             try_exp:2;
    time_t last_syn_loss;
    struct tcp_fastopen_cookie cookie;
};

/* TCP Metrics Block */
struct tcp_metrics_block {
    struct tcp_metrics_block *next;
    struct inetpeer_addr saddr;  // Source address
    struct inetpeer_addr daddr;  // Destination address
    time_t timestamp;  // Last update timestamp
    uint32_t locks;  // Metric locks
    uint32_t values[TCP_METRIC_MAX];  // Metric values
    struct tcp_fastopen_metrics fastopen;
    pthread_mutex_t lock;
};

/* Hash Table Bucket */
struct metrics_bucket {
    struct tcp_metrics_block *chain;
    pthread_mutex_t lock;
};

/* Global Variables */
static struct metrics_bucket *metrics_hash = NULL;
static size_t metrics_hash_size = TCP_METRICS_MAX_ENTRIES;
static pthread_mutex_t metrics_lock = PTHREAD_MUTEX_INITIALIZER;

/* Function Declarations */
static uint32_t hash_addr(const struct inetpeer_addr *addr);
static bool addr_equal(const struct inetpeer_addr *a, const struct inetpeer_addr *b);
static struct tcp_metrics_block *metrics_alloc(void);
static void metrics_free(struct tcp_metrics_block *tm);
static struct tcp_metrics_block *metrics_lookup(const struct inetpeer_addr *saddr,
                                              const struct inetpeer_addr *daddr);
static void metrics_update(struct tcp_metrics_block *tm, 
                         enum tcp_metric_index idx,
                         uint32_t value);
static uint32_t metrics_read(struct tcp_metrics_block *tm,
                           enum tcp_metric_index idx);
static void metrics_lock(struct tcp_metrics_block *tm,
                        enum tcp_metric_index idx);
static bool metrics_locked(struct tcp_metrics_block *tm,
                         enum tcp_metric_index idx);
static void fastopen_update(struct tcp_metrics_block *tm,
                          uint16_t mss,
                          const struct tcp_fastopen_cookie *cookie);
static void metrics_age(struct tcp_metrics_block *tm);

/* Address Hash Function */
static uint32_t hash_addr(const struct inetpeer_addr *addr) {
    uint32_t hash = 0;
    size_t len = (addr->family == AF_INET) ? 4 : 16;
    const uint32_t *ptr = (addr->family == AF_INET) ? &addr->addr.v4 : addr->addr.v6;
    
    while (len >= 4) {
        hash = (hash << 5) - hash + *ptr++;
        len -= 4;
    }
    
    return hash % metrics_hash_size;
}

/* Address Comparison */
static bool addr_equal(const struct inetpeer_addr *a, const struct inetpeer_addr *b) {
    if (a->family != b->family)
        return false;
        
    if (a->family == AF_INET)
        return a->addr.v4 == b->addr.v4;
        
    return memcmp(a->addr.v6, b->addr.v6, sizeof(a->addr.v6)) == 0;
}

/* Allocate New Metrics Block */
static struct tcp_metrics_block *metrics_alloc(void) {
    struct tcp_metrics_block *tm = calloc(1, sizeof(*tm));
    if (!tm)
        return NULL;
        
    pthread_mutex_init(&tm->lock, NULL);
    tm->timestamp = time(NULL);
    
    return tm;
}

/* Free Metrics Block */
static void metrics_free(struct tcp_metrics_block *tm) {
    if (!tm)
        return;
        
    pthread_mutex_destroy(&tm->lock);
    free(tm);
}

/* Lookup Metrics */
static struct tcp_metrics_block *metrics_lookup(const struct inetpeer_addr *saddr,
                                              const struct inetpeer_addr *daddr) {
    uint32_t hash = hash_addr(saddr) ^ hash_addr(daddr);
    struct metrics_bucket *bucket = &metrics_hash[hash];
    struct tcp_metrics_block *tm;
    
    pthread_mutex_lock(&bucket->lock);
    
    for (tm = bucket->chain; tm; tm = tm->next) {
        if (addr_equal(&tm->saddr, saddr) && addr_equal(&tm->daddr, daddr)) {
            metrics_age(tm);
            break;
        }
    }
    
    pthread_mutex_unlock(&bucket->lock);
    return tm;
}

/* Update Metric Value */
static void metrics_update(struct tcp_metrics_block *tm,
                         enum tcp_metric_index idx,
                         uint32_t value) {
    if (!tm || idx >= TCP_METRIC_MAX)
        return;
        
    pthread_mutex_lock(&tm->lock);
    
    if (!metrics_locked(tm, idx)) {
        tm->values[idx] = value;
        tm->timestamp = time(NULL);
    }
    
    pthread_mutex_unlock(&tm->lock);
}

/* Read Metric Value */
static uint32_t metrics_read(struct tcp_metrics_block *tm,
                           enum tcp_metric_index idx) {
    uint32_t value = 0;
    
    if (!tm || idx >= TCP_METRIC_MAX)
        return 0;
        
    pthread_mutex_lock(&tm->lock);
    value = tm->values[idx];
    pthread_mutex_unlock(&tm->lock);
    
    return value;
}

/* Lock Metric */
static void metrics_lock(struct tcp_metrics_block *tm,
                        enum tcp_metric_index idx) {
    if (!tm || idx >= TCP_METRIC_MAX)
        return;
        
    pthread_mutex_lock(&tm->lock);
    tm->locks |= (1 << idx);
    pthread_mutex_unlock(&tm->lock);
}

/* Check if Metric is Locked */
static bool metrics_locked(struct tcp_metrics_block *tm,
                         enum tcp_metric_index idx) {
    if (!tm || idx >= TCP_METRIC_MAX)
        return false;
        
    return (tm->locks & (1 << idx)) != 0;
}

/* Update Fast Open Data */
static void fastopen_update(struct tcp_metrics_block *tm,
                          uint16_t mss,
                          const struct tcp_fastopen_cookie *cookie) {
    if (!tm || !cookie)
        return;
        
    pthread_mutex_lock(&tm->lock);
    
    tm->fastopen.mss = mss;
    memcpy(&tm->fastopen.cookie, cookie, sizeof(*cookie));
    tm->timestamp = time(NULL);
    
    pthread_mutex_unlock(&tm->lock);
}

/* Age Metrics */
static void metrics_age(struct tcp_metrics_block *tm) {
    time_t now;
    
    if (!tm)
        return;
        
    now = time(NULL);
    
    if (now - tm->timestamp > TCP_METRICS_TIMEOUT) {
        pthread_mutex_lock(&tm->lock);
        memset(tm->values, 0, sizeof(tm->values));
        tm->timestamp = now;
        pthread_mutex_unlock(&tm->lock);
    }
}

/* Initialize Metrics System */
bool metrics_init(void) {
    size_t i;
    
    metrics_hash = calloc(metrics_hash_size, sizeof(*metrics_hash));
    if (!metrics_hash)
        return false;
        
    for (i = 0; i < metrics_hash_size; i++)
        pthread_mutex_init(&metrics_hash[i].lock, NULL);
        
    return true;
}

/* Cleanup Metrics System */
void metrics_cleanup(void) {
    size_t i;
    struct tcp_metrics_block *tm, *next;
    
    if (!metrics_hash)
        return;
        
    for (i = 0; i < metrics_hash_size; i++) {
        pthread_mutex_lock(&metrics_hash[i].lock);
        
        tm = metrics_hash[i].chain;
        while (tm) {
            next = tm->next;
            metrics_free(tm);
            tm = next;
        }
        
        pthread_mutex_unlock(&metrics_hash[i].lock);
        pthread_mutex_destroy(&metrics_hash[i].lock);
    }
    
    free(metrics_hash);
    metrics_hash = NULL;
}

/* Add New Metrics Entry */
struct tcp_metrics_block *metrics_add(const struct inetpeer_addr *saddr,
                                    const struct inetpeer_addr *daddr) {
    uint32_t hash = hash_addr(saddr) ^ hash_addr(daddr);
    struct metrics_bucket *bucket = &metrics_hash[hash];
    struct tcp_metrics_block *tm;
    
    tm = metrics_lookup(saddr, daddr);
    if (tm)
        return tm;
        
    tm = metrics_alloc();
    if (!tm)
        return NULL;
        
    memcpy(&tm->saddr, saddr, sizeof(*saddr));
    memcpy(&tm->daddr, daddr, sizeof(*daddr));
    
    pthread_mutex_lock(&bucket->lock);
    tm->next = bucket->chain;
    bucket->chain = tm;
    pthread_mutex_unlock(&bucket->lock);
    
    return tm;
}

/* Remove Metrics Entry */
void metrics_remove(const struct inetpeer_addr *saddr,
                   const struct inetpeer_addr *daddr) {
    uint32_t hash = hash_addr(saddr) ^ hash_addr(daddr);
    struct metrics_bucket *bucket = &metrics_hash[hash];
    struct tcp_metrics_block *tm, **pprev;
    
    pthread_mutex_lock(&bucket->lock);
    
    pprev = &bucket->chain;
    while ((tm = *pprev) != NULL) {
        if (addr_equal(&tm->saddr, saddr) && addr_equal(&tm->daddr, daddr)) {
            *pprev = tm->next;
            metrics_free(tm);
            break;
        }
        pprev = &tm->next;
    }
    
    pthread_mutex_unlock(&bucket->lock);
}

/* Print Metrics for Debugging */
void metrics_print(const struct inetpeer_addr *saddr,
                  const struct inetpeer_addr *daddr) {
    struct tcp_metrics_block *tm;
    char src[INET6_ADDRSTRLEN], dst[INET6_ADDRSTRLEN];
    
    tm = metrics_lookup(saddr, daddr);
    if (!tm) {
        printf("No metrics found\n");
        return;
    }
    
    if (saddr->family == AF_INET) {
        inet_ntop(AF_INET, &saddr->addr.v4, src, sizeof(src));
        inet_ntop(AF_INET, &daddr->addr.v4, dst, sizeof(dst));
    } else {
        inet_ntop(AF_INET6, saddr->addr.v6, src, sizeof(src));
        inet_ntop(AF_INET6, daddr->addr.v6, dst, sizeof(dst));
    }
    
    printf("TCP Metrics for %s -> %s:\n", src, dst);
    printf("  RTT: %u usec\n", metrics_read(tm, TCP_METRIC_RTT));
    printf("  RTTVAR: %u usec\n", metrics_read(tm, TCP_METRIC_RTTVAR));
    printf("  SSTHRESH: %u\n", metrics_read(tm, TCP_METRIC_SSTHRESH));
    printf("  CWND: %u\n", metrics_read(tm, TCP_METRIC_CWND));
    printf("  REORDERING: %u\n", metrics_read(tm, TCP_METRIC_REORDERING));
    printf("  Fast Open MSS: %u\n", tm->fastopen.mss);
    printf("  Fast Open SYN Loss: %u\n", tm->fastopen.syn_loss);
}

/* Example Usage */
int main(void) {
    struct inetpeer_addr saddr = {
        .family = AF_INET,
        .addr.v4 = 0x0100007f  // 127.0.0.1
    };
    struct inetpeer_addr daddr = {
        .family = AF_INET,
        .addr.v4 = 0x0200007f  // 127.0.0.2
    };
    struct tcp_metrics_block *tm;
    struct tcp_fastopen_cookie cookie = {
        .len = 8,
        .exp = false,
        .key = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08}
    };
    
    if (!metrics_init()) {
        fprintf(stderr, "Failed to initialize metrics system\n");
        return 1;
    }
    
    /* Add and update metrics */
    tm = metrics_add(&saddr, &daddr);
    if (!tm) {
        fprintf(stderr, "Failed to add metrics\n");
        metrics_cleanup();
        return 1;
    }
    
    metrics_update(tm, TCP_METRIC_RTT, 100000);  // 100ms
    metrics_update(tm, TCP_METRIC_RTTVAR, 20000);  // 20ms
    metrics_update(tm, TCP_METRIC_SSTHRESH, 65535);
    metrics_update(tm, TCP_METRIC_CWND, 10);
    metrics_update(tm, TCP_METRIC_REORDERING, 3);
    
    fastopen_update(tm, 1460, &cookie);
    
    /* Print metrics */
    metrics_print(&saddr, &daddr);
    
    /* Cleanup */
    metrics_cleanup();
    
    return 0;
}
