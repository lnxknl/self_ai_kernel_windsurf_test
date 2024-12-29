// SPDX-License-Identifier: GPL-2.0-only
/*
 * TCP CDG (CAIA Delay-Gradient) Congestion Control Algorithm Simulator
 *
 * This is a standalone simulation of the CDG congestion control algorithm
 * based on the paper:
 *   D.A. Hayes and G. Armitage. "Revisiting TCP congestion control using
 *   delay gradients." In IFIP Networking, pages 328-341. Springer, 2011.
 *
 * This simulation demonstrates the key concepts of CDG including:
 * - Delay gradient calculation and window management
 * - Hybrid Slow Start implementation
 * - Backoff mechanisms and ineffectual backoff detection
 * - Shadow window mechanism
 * - Loss tolerance heuristics
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
#include <math.h>
#include <stdbool.h>
#include <stdatomic.h>

#define MAX_PACKET_SIZE 1500
#define DEFAULT_PORT 8888
#define BACKLOG 5
#define MAX_CONNECTIONS 100
#define RTT_WINDOW 8
#define GRADIENT_WINDOW 8
#define HYSTART_MIN_SAMPLES 8

// Configuration parameters
static const int window = 8;
static const unsigned int backoff_beta = 0.7071 * 1024; // sqrt 0.5
static const unsigned int backoff_factor = 42;
static const unsigned int hystart_detect = 3;
static const unsigned int use_ineff = 5;
static const bool use_shadow = true;
static const bool use_tolerance = false;

// CDG specific structures
struct cdg_minmax {
    int32_t min;
    int32_t max;
};

enum cdg_state {
    CDG_UNKNOWN = 0,
    CDG_NONFULL = 1,
    CDG_FULL = 2,
    CDG_BACKOFF = 3
};

struct cdg {
    struct cdg_minmax rtt;
    struct cdg_minmax rtt_prev;
    struct cdg_minmax *gradients;
    struct cdg_minmax gsum;
    bool gfilled;
    uint8_t tail;
    uint8_t state;
    uint8_t delack;
    uint32_t rtt_seq;
    uint32_t shadow_wnd;
    uint16_t backoff_cnt;
    uint16_t sample_cnt;
    int32_t delay_min;
    uint32_t last_ack;
    uint32_t round_start;
    
    // Additional simulation specific fields
    uint32_t cwnd;
    uint32_t ssthresh;
    uint32_t rtt_us;
    uint32_t min_rtt_us;
    uint32_t prior_cwnd;
    bool in_recovery;
    pthread_mutex_t lock;
};

// Connection state
struct connection {
    int fd;
    struct sockaddr_in addr;
    struct cdg ca;
    pthread_t thread;
    bool active;
};

// Global variables
static volatile bool running = true;
static struct connection connections[MAX_CONNECTIONS];
static pthread_mutex_t connections_lock = PTHREAD_MUTEX_INITIALIZER;

// Utility functions
static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint32_t nexp_u32(uint32_t ux) {
    static const uint16_t v[] = {
        65535,
        65518, 65501, 65468, 65401, 65267, 65001, 64470, 63422,
        61378, 57484, 50423, 38795, 22965, 8047, 987, 14,
    };
    uint32_t msb = ux >> 8;
    uint32_t res;
    int i;

    if (msb > 65535)
        return 0;

    res = 0xFFFFFFFF - (ux & 0xff) * (0xFFFFFFFF / 1000000);

    for (i = 1; msb; i++, msb >>= 1) {
        uint32_t y = v[i & -(msb & 1)] + 1;
        res = ((uint64_t)res * y) >> 16;
    }

    return res;
}

// CDG Algorithm Implementation
static void cdg_init(struct cdg *ca) {
    memset(ca, 0, sizeof(*ca));
    ca->gradients = calloc(window, sizeof(struct cdg_minmax));
    ca->cwnd = 10;
    ca->ssthresh = 0xFFFFFFFF;
    pthread_mutex_init(&ca->lock, NULL);
}

static void cdg_release(struct cdg *ca) {
    free(ca->gradients);
    pthread_mutex_destroy(&ca->lock);
}

static int32_t cdg_grad(struct cdg *ca) {
    int32_t gmin = ca->rtt.min - ca->rtt_prev.min;
    int32_t gmax = ca->rtt.max - ca->rtt_prev.max;
    int32_t grad;

    if (ca->gradients) {
        ca->gsum.min += gmin - ca->gradients[ca->tail].min;
        ca->gsum.max += gmax - ca->gradients[ca->tail].max;
        ca->gradients[ca->tail].min = gmin;
        ca->gradients[ca->tail].max = gmax;
        ca->tail = (ca->tail + 1) & (window - 1);
        gmin = ca->gsum.min;
        gmax = ca->gsum.max;
    }

    grad = gmin > 0 ? gmin : gmax;

    if (!ca->gfilled) {
        if (!ca->gradients && window > 1)
            grad *= window;
        else if (ca->tail == 0)
            ca->gfilled = true;
        else
            grad = (grad * window) / (int)ca->tail;
    }

    if (gmin <= -32 || gmax <= -32)
        ca->backoff_cnt = 0;

    if (use_tolerance) {
        gmin = (gmin + 32) / 64;
        gmax = (gmax + 32) / 64;

        if (gmin > 0 && gmax <= 0)
            ca->state = CDG_FULL;
        else if ((gmin > 0 && gmax > 0) || gmax < 0)
            ca->state = CDG_NONFULL;
    }

    return grad;
}

static bool cdg_backoff(struct cdg *ca, uint32_t grad) {
    uint32_t backoff_prob = 0;
    bool window_just_limited = false;

    if (ca->shadow_wnd && ca->shadow_wnd > ca->cwnd) {
        window_just_limited = true;
        ca->shadow_wnd = 0;
    }

    if (grad <= 0 || !use_ineff || ca->backoff_cnt >= use_ineff)
        return false;

    backoff_prob = nexp_u32(grad * backoff_factor);
    
    if (window_just_limited)
        backoff_prob = ~0;

    if ((prandom_u32() & 0x7FFFFFFF) > backoff_prob)
        return false;

    ca->backoff_cnt++;
    return true;
}

static void cdg_hystart_update(struct cdg *ca) {
    if (ca->delay_min == 0)
        return;

    uint64_t now = get_time_us();

    if (hystart_detect & 1) {  // ACK train detection
        if (ca->last_ack == 0) {
            ca->last_ack = now;
            ca->round_start = now;
        } else if (now - ca->last_ack < 3000) {  // 3ms
            uint32_t base_owd = ca->delay_min / 2;
            if (base_owd < 125)
                base_owd = 125;

            ca->last_ack = now;
            if (now - ca->round_start > base_owd) {
                ca->ssthresh = ca->cwnd;
                return;
            }
        }
    }

    if (hystart_detect & 2) {  // Delay increase detection
        if (ca->sample_cnt < HYSTART_MIN_SAMPLES) {
            ca->sample_cnt++;
        } else {
            uint32_t thresh = ca->delay_min + ca->delay_min / 8;
            if (thresh < 125)
                thresh = 125;

            if (ca->rtt.min > thresh)
                ca->ssthresh = ca->cwnd;
        }
    }
}

static void cdg_cong_avoid(struct cdg *ca, uint32_t ack, uint32_t acked) {
    uint32_t prior_cwnd;
    uint32_t cwnd = ca->cwnd;
    int32_t grad;

    if (ca->ssthresh != 0xFFFFFFFF && cwnd < ca->ssthresh) {
        // Slow start
        cwnd += acked;
        if (cwnd >= ca->ssthresh)
            cwnd = ca->ssthresh;
    } else {
        // Congestion avoidance
        prior_cwnd = cwnd;
        grad = cdg_grad(ca);

        if (cdg_backoff(ca, grad)) {
            cwnd = (cwnd * backoff_beta) >> 10;
            ca->shadow_wnd = max(ca->shadow_wnd, prior_cwnd);
        } else {
            cwnd += acked;
        }

        if (ca->shadow_wnd) {
            if (ca->shadow_wnd > cwnd)
                ca->shadow_wnd -= acked;
            else
                ca->shadow_wnd = 0;
        }
    }

    ca->cwnd = max(cwnd, 2U);
}

static void cdg_pkts_acked(struct cdg *ca, uint32_t rtt_us) {
    if (rtt_us == 0)
        return;

    ca->rtt_us = rtt_us;
    ca->min_rtt_us = ca->min_rtt_us ? min(ca->min_rtt_us, rtt_us) : rtt_us;

    if (ca->rtt_seq == 0 || rtt_us < ca->rtt.min)
        ca->rtt.min = rtt_us;
    if (rtt_us > ca->rtt.max)
        ca->rtt.max = rtt_us;

    if (++ca->rtt_seq >= window) {
        ca->rtt_seq = 0;
        ca->rtt_prev = ca->rtt;
        ca->rtt.min = ca->rtt.max = rtt_us;
    }

    if (ca->delay_min == 0 || rtt_us <= ca->delay_min)
        ca->delay_min = rtt_us;

    if (ca->cwnd < ca->ssthresh)
        cdg_hystart_update(ca);
}

// Networking Implementation
static void handle_error(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void cleanup_connection(struct connection *conn) {
    if (conn->active) {
        close(conn->fd);
        cdg_release(&conn->ca);
        conn->active = false;
    }
}

static void *connection_handler(void *arg) {
    struct connection *conn = (struct connection *)arg;
    struct cdg *ca = &conn->ca;
    char buffer[MAX_PACKET_SIZE];
    ssize_t bytes;
    uint64_t send_time, recv_time;

    while (running && conn->active) {
        send_time = get_time_us();
        bytes = recv(conn->fd, buffer, sizeof(buffer), 0);
        recv_time = get_time_us();

        if (bytes <= 0) {
            if (bytes < 0 && errno == EINTR)
                continue;
            break;
        }

        pthread_mutex_lock(&ca->lock);
        
        // Update RTT and congestion control
        uint32_t rtt = recv_time - send_time;
        cdg_pkts_acked(ca, rtt);
        cdg_cong_avoid(ca, bytes, 1);

        // Simulate sending response
        uint32_t cwnd = ca->cwnd;
        uint32_t send_bytes = min(bytes, cwnd * MAX_PACKET_SIZE);
        
        pthread_mutex_unlock(&ca->lock);

        if (send(conn->fd, buffer, send_bytes, 0) < 0) {
            if (errno != EINTR)
                break;
        }
    }

    cleanup_connection(conn);
    return NULL;
}

static void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM)
        running = false;
}

static int find_free_connection(void) {
    int i;
    pthread_mutex_lock(&connections_lock);
    for (i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            connections[i].active = true;
            pthread_mutex_unlock(&connections_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&connections_lock);
    return -1;
}

int main(int argc, char *argv[]) {
    int server_fd, conn_idx;
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);

    // Initialize server
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        handle_error("socket creation failed");

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
        handle_error("setsockopt failed");

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(DEFAULT_PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        handle_error("bind failed");

    if (listen(server_fd, BACKLOG) < 0)
        handle_error("listen failed");

    // Set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("TCP CDG Simulator listening on port %d...\n", DEFAULT_PORT);

    // Main server loop
    while (running) {
        conn_idx = find_free_connection();
        if (conn_idx < 0) {
            fprintf(stderr, "Maximum connections reached\n");
            sleep(1);
            continue;
        }

        struct connection *conn = &connections[conn_idx];
        conn->fd = accept(server_fd, (struct sockaddr *)&conn->addr, &addr_len);
        
        if (conn->fd < 0) {
            if (errno == EINTR)
                continue;
            handle_error("accept failed");
        }

        printf("New connection from %s:%d\n",
               inet_ntoa(conn->addr.sin_addr),
               ntohs(conn->addr.sin_port));

        cdg_init(&conn->ca);

        if (pthread_create(&conn->thread, NULL, connection_handler, conn) != 0) {
            fprintf(stderr, "Failed to create thread for connection\n");
            cleanup_connection(conn);
            continue;
        }

        pthread_detach(conn->thread);
    }

    // Cleanup
    close(server_fd);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].active)
            cleanup_connection(&connections[i]);
    }

    printf("\nServer shutdown complete\n");
    return 0;
}
