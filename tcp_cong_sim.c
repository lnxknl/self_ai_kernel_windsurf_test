// SPDX-License-Identifier: GPL-2.0-only
/*
 * TCP Congestion Control Framework Simulator
 *
 * This is a standalone simulation of the Linux TCP congestion control framework
 * that demonstrates the pluggable congestion control architecture and implements
 * the classic TCP Reno algorithm.
 *
 * The simulation includes:
 * - Pluggable congestion control framework
 * - TCP Reno congestion control implementation
 * - Dynamic congestion control registration/unregistration
 * - Socket state management and congestion control operations
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>

#define MAX_CA_NAME 16
#define MAX_CONGESTION_CONTROLS 16
#define DEFAULT_PORT 8888
#define BACKLOG 5
#define MAX_CONNECTIONS 100
#define MAX_PACKET_SIZE 1500
#define MIN_CWND 2
#define INITIAL_CWND 10

// TCP congestion states
enum tcp_ca_state {
    TCP_CA_Open = 0,
    TCP_CA_Disorder = 1,
    TCP_CA_CWR = 2,
    TCP_CA_Recovery = 3,
    TCP_CA_Loss = 4
};

// TCP congestion control flags
enum tcp_cong_flags {
    TCP_CONG_NON_RESTRICTED = 1,
    TCP_CONG_NEEDS_ECN = 2
};

// Forward declarations
struct tcp_sock;
struct sock;
struct tcp_congestion_ops;

// TCP socket structure
struct tcp_sock {
    uint32_t snd_cwnd;           // Sending congestion window
    uint32_t snd_ssthresh;       // Slow start threshold
    uint32_t prior_cwnd;         // Cwnd before congestion
    uint32_t lost_out;           // Lost packets
    uint32_t retrans_out;        // Retransmitted packets
    uint32_t high_seq;           // Highest sequence sent
    uint32_t snd_nxt;           // Next sequence to send
    uint32_t snd_una;           // First unacknowledged sequence
    uint32_t rcv_nxt;           // Next sequence expected
    uint32_t copied_seq;         // Head of yet unread data
    uint32_t rcv_wnd;           // Receive window
    uint8_t ca_state;           // Congestion state
    bool is_cwnd_limited;       // Congestion window limited?
    struct tcp_congestion_ops *ca_ops;  // Congestion control operations
    void *ca_priv;              // Private congestion control data
};

// Generic socket structure
struct sock {
    int fd;                     // Socket file descriptor
    struct sockaddr_in addr;    // Socket address
    struct tcp_sock tcp;        // TCP specific data
    pthread_mutex_t lock;       // Socket lock
    bool active;               // Is socket active?
};

// Congestion control operations
struct tcp_congestion_ops {
    struct tcp_congestion_ops *next;  // Next in list
    char name[MAX_CA_NAME];          // Algorithm name
    uint32_t key;                    // Hash key
    uint32_t flags;                  // Algorithm flags
    
    // Required operations
    uint32_t (*ssthresh)(struct sock *sk);
    void (*cong_avoid)(struct sock *sk, uint32_t ack, uint32_t acked);
    uint32_t (*undo_cwnd)(struct sock *sk);
    
    // Optional operations
    void (*set_state)(struct sock *sk, uint8_t new_state);
    void (*cwnd_event)(struct sock *sk, enum tcp_ca_state ev);
    void (*in_ack_event)(struct sock *sk, uint32_t flags);
    void (*pkts_acked)(struct sock *sk, uint32_t num_acked, uint32_t rtt_us);
    void (*init)(struct sock *sk);
    void (*release)(struct sock *sk);
};

// Global variables
static struct tcp_congestion_ops *registered_ca[MAX_CONGESTION_CONTROLS];
static int num_registered_ca = 0;
static pthread_mutex_t ca_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool running = true;
static struct sock *connections[MAX_CONNECTIONS];
static pthread_mutex_t connections_lock = PTHREAD_MUTEX_INITIALIZER;

// Utility functions
static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint32_t jhash(const char *key, size_t len) {
    uint32_t hash = 0;
    for (size_t i = 0; i < len; i++) {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

// TCP Reno implementation
static uint32_t tcp_reno_ssthresh(struct sock *sk) {
    struct tcp_sock *tp = &sk->tcp;
    return max(tp->snd_cwnd >> 1U, 2U);
}

static void tcp_reno_cong_avoid(struct sock *sk, uint32_t ack, uint32_t acked) {
    struct tcp_sock *tp = &sk->tcp;
    
    if (tp->snd_cwnd <= tp->snd_ssthresh) {
        // Slow start
        while (acked > 0) {
            tp->snd_cwnd++;
            acked--;
        }
    } else {
        // Congestion avoidance
        tp->snd_cwnd_cnt += acked;
        if (tp->snd_cwnd_cnt >= tp->snd_cwnd) {
            tp->snd_cwnd_cnt = 0;
            tp->snd_cwnd++;
        }
    }
}

static uint32_t tcp_reno_undo_cwnd(struct sock *sk) {
    return (&sk->tcp)->prior_cwnd;
}

// Congestion control framework
static int tcp_register_congestion_control(struct tcp_congestion_ops *ca) {
    if (!ca->ssthresh || !ca->undo_cwnd || !ca->cong_avoid) {
        fprintf(stderr, "Required operations missing for %s\n", ca->name);
        return -1;
    }
    
    pthread_mutex_lock(&ca_mutex);
    
    if (num_registered_ca >= MAX_CONGESTION_CONTROLS) {
        pthread_mutex_unlock(&ca_mutex);
        return -1;
    }
    
    // Generate key from name
    ca->key = jhash(ca->name, strlen(ca->name));
    
    // Check for duplicates
    for (int i = 0; i < num_registered_ca; i++) {
        if (registered_ca[i]->key == ca->key) {
            pthread_mutex_unlock(&ca_mutex);
            return -1;
        }
    }
    
    registered_ca[num_registered_ca++] = ca;
    pthread_mutex_unlock(&ca_mutex);
    return 0;
}

static void tcp_unregister_congestion_control(struct tcp_congestion_ops *ca) {
    pthread_mutex_lock(&ca_mutex);
    
    for (int i = 0; i < num_registered_ca; i++) {
        if (registered_ca[i] == ca) {
            memmove(&registered_ca[i], &registered_ca[i + 1],
                    (num_registered_ca - i - 1) * sizeof(struct tcp_congestion_ops *));
            num_registered_ca--;
            break;
        }
    }
    
    pthread_mutex_unlock(&ca_mutex);
}

static struct tcp_congestion_ops *tcp_ca_find(const char *name) {
    pthread_mutex_lock(&ca_mutex);
    
    for (int i = 0; i < num_registered_ca; i++) {
        if (strcmp(registered_ca[i]->name, name) == 0) {
            pthread_mutex_unlock(&ca_mutex);
            return registered_ca[i];
        }
    }
    
    pthread_mutex_unlock(&ca_mutex);
    return NULL;
}

static void tcp_init_congestion_control(struct sock *sk) {
    struct tcp_sock *tp = &sk->tcp;
    
    if (!tp->ca_ops) {
        // Default to Reno
        tp->ca_ops = tcp_ca_find("reno");
    }
    
    if (tp->ca_ops->init)
        tp->ca_ops->init(sk);
}

static void tcp_cleanup_congestion_control(struct sock *sk) {
    struct tcp_sock *tp = &sk->tcp;
    
    if (tp->ca_ops && tp->ca_ops->release)
        tp->ca_ops->release(sk);
}

// Socket management
static struct sock *create_socket(void) {
    struct sock *sk = calloc(1, sizeof(*sk));
    if (!sk)
        return NULL;
    
    sk->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sk->fd < 0) {
        free(sk);
        return NULL;
    }
    
    pthread_mutex_init(&sk->lock, NULL);
    sk->tcp.snd_cwnd = INITIAL_CWND;
    sk->tcp.snd_ssthresh = 0xFFFFFFFF;
    sk->active = true;
    
    return sk;
}

static void destroy_socket(struct sock *sk) {
    if (!sk)
        return;
    
    tcp_cleanup_congestion_control(sk);
    close(sk->fd);
    pthread_mutex_destroy(&sk->lock);
    free(sk);
}

// Connection handling
static void *connection_handler(void *arg) {
    struct sock *sk = (struct sock *)arg;
    char buffer[MAX_PACKET_SIZE];
    ssize_t bytes;
    uint64_t rtt_start;
    
    tcp_init_congestion_control(sk);
    
    while (running && sk->active) {
        rtt_start = get_time_us();
        bytes = recv(sk->fd, buffer, sizeof(buffer), 0);
        
        if (bytes <= 0) {
            if (bytes < 0 && errno == EINTR)
                continue;
            break;
        }
        
        pthread_mutex_lock(&sk->lock);
        
        // Update RTT and congestion control
        uint32_t rtt = get_time_us() - rtt_start;
        if (sk->tcp.ca_ops->pkts_acked)
            sk->tcp.ca_ops->pkts_acked(sk, 1, rtt);
        
        // Update congestion window
        sk->tcp.ca_ops->cong_avoid(sk, bytes, 1);
        
        // Simulate sending response
        uint32_t cwnd = sk->tcp.snd_cwnd;
        uint32_t send_bytes = min(bytes, cwnd * MAX_PACKET_SIZE);
        
        pthread_mutex_unlock(&sk->lock);
        
        if (send(sk->fd, buffer, send_bytes, 0) < 0) {
            if (errno != EINTR)
                break;
        }
    }
    
    destroy_socket(sk);
    return NULL;
}

// Server implementation
static void handle_error(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM)
        running = false;
}

// TCP Reno congestion control operations
static struct tcp_congestion_ops tcp_reno = {
    .name = "reno",
    .flags = TCP_CONG_NON_RESTRICTED,
    .ssthresh = tcp_reno_ssthresh,
    .cong_avoid = tcp_reno_cong_avoid,
    .undo_cwnd = tcp_reno_undo_cwnd,
};

int main(int argc, char *argv[]) {
    int server_fd;
    struct sockaddr_in server_addr;
    pthread_t thread;
    
    // Register TCP Reno
    if (tcp_register_congestion_control(&tcp_reno) < 0)
        handle_error("Failed to register TCP Reno");
    
    // Create server socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        handle_error("Socket creation failed");
    
    // Set socket options
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
        handle_error("setsockopt failed");
    
    // Bind and listen
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(DEFAULT_PORT);
    
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        handle_error("Bind failed");
    
    if (listen(server_fd, BACKLOG) < 0)
        handle_error("Listen failed");
    
    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("TCP Congestion Control Simulator listening on port %d...\n", DEFAULT_PORT);
    printf("Using TCP Reno congestion control\n");
    
    // Main server loop
    while (running) {
        struct sock *sk = create_socket();
        if (!sk) {
            fprintf(stderr, "Failed to create socket\n");
            continue;
        }
        
        socklen_t addr_len = sizeof(sk->addr);
        sk->fd = accept(server_fd, (struct sockaddr *)&sk->addr, &addr_len);
        
        if (sk->fd < 0) {
            if (errno == EINTR)
                continue;
            destroy_socket(sk);
            handle_error("Accept failed");
        }
        
        printf("New connection from %s:%d\n",
               inet_ntoa(sk->addr.sin_addr),
               ntohs(sk->addr.sin_port));
        
        if (pthread_create(&thread, NULL, connection_handler, sk) != 0) {
            fprintf(stderr, "Failed to create thread for connection\n");
            destroy_socket(sk);
            continue;
        }
        
        pthread_detach(thread);
    }
    
    // Cleanup
    close(server_fd);
    tcp_unregister_congestion_control(&tcp_reno);
    
    printf("\nServer shutdown complete\n");
    return 0;
}
