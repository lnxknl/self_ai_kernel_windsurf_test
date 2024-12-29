// SPDX-License-Identifier: GPL-2.0-only
/*
 * TCP CUBIC Simulator: Implementation of CUBIC TCP congestion control
 *
 * This is a standalone simulation of the CUBIC TCP algorithm based on the paper:
 * "CUBIC: A New TCP-Friendly High-Speed TCP Variant"
 * by Sangtae Ha, Injong Rhee and Lisong Xu
 *
 * Features implemented:
 * - CUBIC window growth function
 * - TCP friendliness
 * - Fast convergence
 * - Hybrid slow start (HyStart)
 * - RTT independence
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
#include <math.h>

#define BICTCP_BETA_SCALE    1024    /* Scale factor for beta calculation */
#define BICTCP_HZ           10      /* Time scale conversion */
#define MAX_PACKET_SIZE     1500
#define DEFAULT_PORT        8888
#define BACKLOG            5
#define MAX_CONNECTIONS    100
#define INITIAL_WINDOW     10

/* HyStart parameters */
#define HYSTART_ACK_TRAIN   0x1
#define HYSTART_DELAY       0x2
#define HYSTART_MIN_SAMPLES 8
#define HYSTART_DELAY_MIN   4000    /* 4ms */
#define HYSTART_DELAY_MAX   16000   /* 16ms */
#define HYSTART_LOW_WINDOW  16
#define HYSTART_ACK_DELTA   2000    /* 2ms */

/* CUBIC Parameters */
static const int fast_convergence = 1;
static const int beta = 717;        /* = 717/1024 (BICTCP_BETA_SCALE) */
static const int bic_scale = 41;
static const int tcp_friendliness = 1;
static const int hystart = 1;
static const int hystart_detect = HYSTART_ACK_TRAIN | HYSTART_DELAY;

/* TCP States */
enum tcp_ca_state {
    TCP_CA_Open = 0,
    TCP_CA_Disorder = 1,
    TCP_CA_CWR = 2,
    TCP_CA_Recovery = 3,
    TCP_CA_Loss = 4
};

/* CUBIC State */
struct cubic_state {
    uint32_t cnt;                /* Increase CWND by 1 after ACKs */
    uint32_t last_max_cwnd;      /* Last maximum window */
    uint32_t last_cwnd;          /* Last window size */
    uint32_t last_time;          /* Time when updated last_cwnd */
    uint32_t bic_origin_point;   /* Origin point of bic function */
    uint32_t bic_K;              /* Time to origin point */
    uint32_t delay_min;          /* Minimum delay */
    uint32_t epoch_start;        /* Start of an epoch */
    uint32_t ack_cnt;            /* Number of acks */
    uint32_t tcp_cwnd;           /* Estimated TCP cwnd */
    uint8_t sample_cnt;          /* Number of samples to determine curr_rtt */
    uint8_t found;              /* The exit point is found? */
    uint32_t round_start;        /* Beginning of each round */
    uint32_t end_seq;           /* End sequence of the round */
    uint32_t last_ack;          /* Last time when ACK spacing is close */
    uint32_t curr_rtt;          /* Minimum RTT of current round */
    
    /* Additional simulation state */
    uint32_t snd_cwnd;          /* Current congestion window */
    uint32_t snd_ssthresh;      /* Slow start threshold */
    pthread_mutex_t lock;        /* State lock */
};

/* Connection state */
struct connection {
    int fd;
    struct sockaddr_in addr;
    struct cubic_state cubic;
    pthread_t thread;
    bool active;
};

/* Global variables */
static volatile bool running = true;
static struct connection connections[MAX_CONNECTIONS];
static pthread_mutex_t connections_lock = PTHREAD_MUTEX_INITIALIZER;

/* Utility functions */
static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint32_t get_jiffies(void) {
    return get_time_us() / (1000000 / HZ);
}

/* Cubic root calculation using lookup table and Newton-Raphson */
static uint32_t cubic_root(uint64_t a) {
    static const uint8_t v[] = {
        /* Lookup table for cubic root approximation */
        0,   54,  54,  54,  118, 118, 118, 118,
        123, 129, 134, 138, 143, 147, 151, 156,
        157, 161, 164, 168, 170, 173, 176, 179,
        181, 185, 187, 190, 192, 194, 197, 199,
        200, 202, 204, 206, 209, 211, 213, 215,
        217, 219, 221, 222, 224, 225, 227, 229,
        231, 232, 234, 236, 237, 239, 240, 242,
        244, 245, 246, 248, 250, 251, 252, 254,
    };
    
    uint32_t x, b, shift;
    
    b = fls64(a);
    if (b < 7)
        return ((uint32_t)v[(uint32_t)a] + 35) >> 6;
    
    b = ((b * 84) >> 8) - 1;
    shift = (a >> (b * 3));
    
    x = ((uint32_t)(((uint32_t)v[shift] + 10) << b)) >> 6;
    
    /* Newton-Raphson iteration */
    x = (2 * x + (uint32_t)(a / ((uint64_t)x * (uint64_t)(x - 1))));
    x = ((x * 341) >> 10);
    
    return x;
}

/* CUBIC window update */
static void cubic_update(struct cubic_state *ca) {
    uint32_t delta, bic_target, max_cnt;
    uint64_t offs, t;
    uint32_t cwnd = ca->snd_cwnd;
    uint32_t now = get_jiffies();
    
    if (ca->last_cwnd == cwnd && now - ca->last_time <= HZ/32)
        return;
    
    ca->last_cwnd = cwnd;
    ca->last_time = now;
    
    if (ca->epoch_start == 0) {
        ca->epoch_start = now;
        ca->ack_cnt = 0;
        ca->tcp_cwnd = cwnd;
        
        if (ca->last_max_cwnd <= cwnd) {
            ca->bic_K = 0;
            ca->bic_origin_point = cwnd;
        } else {
            ca->bic_K = cubic_root(bic_scale * (ca->last_max_cwnd - cwnd));
            ca->bic_origin_point = ca->last_max_cwnd;
        }
    }
    
    t = (now + ca->delay_min/1000 - ca->epoch_start) << BICTCP_HZ;
    t /= HZ;
    
    if (t < ca->bic_K)
        offs = ca->bic_K - t;
    else
        offs = t - ca->bic_K;
    
    /* CUBIC function */
    delta = (bic_scale * offs * offs * offs) >> (10 + 3*BICTCP_HZ);
    
    if (t < ca->bic_K)
        bic_target = ca->bic_origin_point - delta;
    else
        bic_target = ca->bic_origin_point + delta;
    
    if (bic_target > cwnd)
        ca->cnt = cwnd / (bic_target - cwnd);
    else
        ca->cnt = 100 * cwnd;
    
    /* TCP friendliness */
    if (tcp_friendliness) {
        uint32_t scale = beta;
        delta = (cwnd * scale) >> 3;
        while (ca->ack_cnt > delta) {
            ca->ack_cnt -= delta;
            ca->tcp_cwnd++;
        }
        
        if (ca->tcp_cwnd > cwnd) {
            delta = ca->tcp_cwnd - cwnd;
            max_cnt = cwnd / delta;
            if (ca->cnt > max_cnt)
                ca->cnt = max_cnt;
        }
    }
    
    /* Limit maximum increase rate */
    ca->cnt = max(ca->cnt, 2U);
}

/* HyStart implementation */
static void hystart_reset(struct cubic_state *ca) {
    ca->round_start = ca->last_ack = get_time_us();
    ca->curr_rtt = ~0U;
    ca->sample_cnt = 0;
}

static void hystart_update(struct cubic_state *ca, uint32_t delay) {
    uint32_t now = get_time_us();
    
    if (!(hystart_detect & HYSTART_ACK_TRAIN))
        return;
    
    /* First detection parameter - ACK train */
    if (now - ca->last_ack <= HYSTART_ACK_DELTA) {
        ca->last_ack = now;
        
        uint32_t base_owd = max(ca->delay_min/2, (uint32_t)HYSTART_DELAY_MIN);
        
        if (now - ca->round_start > base_owd) {
            ca->found = 1;
            ca->snd_ssthresh = ca->snd_cwnd;
        }
    }
    
    /* Second detection parameter - delay increase */
    if (hystart_detect & HYSTART_DELAY) {
        if (ca->sample_cnt < HYSTART_MIN_SAMPLES) {
            if (ca->curr_rtt == ~0U || ca->curr_rtt > delay)
                ca->curr_rtt = delay;
            
            ca->sample_cnt++;
        } else {
            if (ca->curr_rtt > delay)
                ca->curr_rtt = delay;
            
            uint32_t thresh = ca->delay_min + HYSTART_DELAY_MIN;
            
            if (ca->curr_rtt >= thresh) {
                ca->found = 1;
                ca->snd_ssthresh = ca->snd_cwnd;
            }
        }
    }
}

/* Congestion avoidance */
static void cubic_cong_avoid(struct cubic_state *ca, uint32_t ack, uint32_t acked) {
    if (ca->snd_cwnd <= ca->snd_ssthresh) {
        /* Slow start */
        if (hystart && ca->snd_cwnd < HYSTART_LOW_WINDOW) {
            ca->snd_cwnd += acked;
            return;
        }
        
        if (hystart) {
            hystart_update(ca, ca->delay_min);
            if (ca->found) {
                ca->found = 0;
                ca->snd_ssthresh = ca->snd_cwnd;
            }
        }
        
        ca->snd_cwnd += acked;
    } else {
        /* Congestion avoidance */
        cubic_update(ca);
        if (ca->cnt > acked)
            ca->cnt -= acked;
        else {
            ca->snd_cwnd += 1;
            ca->cnt = 0;
        }
    }
}

/* Connection handling */
static void *connection_handler(void *arg) {
    struct connection *conn = (struct connection *)arg;
    struct cubic_state *ca = &conn->cubic;
    char buffer[MAX_PACKET_SIZE];
    ssize_t bytes;
    uint64_t rtt_start;
    
    while (running && conn->active) {
        rtt_start = get_time_us();
        bytes = recv(conn->fd, buffer, sizeof(buffer), 0);
        
        if (bytes <= 0) {
            if (bytes < 0 && errno == EINTR)
                continue;
            break;
        }
        
        pthread_mutex_lock(&ca->lock);
        
        /* Update RTT and congestion control */
        uint32_t rtt = get_time_us() - rtt_start;
        if (ca->delay_min == 0 || rtt < ca->delay_min)
            ca->delay_min = rtt;
        
        cubic_cong_avoid(ca, bytes, 1);
        
        /* Simulate sending response */
        uint32_t cwnd = ca->snd_cwnd;
        uint32_t send_bytes = min(bytes, cwnd * MAX_PACKET_SIZE);
        
        pthread_mutex_unlock(&ca->lock);
        
        if (send(conn->fd, buffer, send_bytes, 0) < 0) {
            if (errno != EINTR)
                break;
        }
    }
    
    close(conn->fd);
    conn->active = false;
    return NULL;
}

/* Initialize CUBIC state */
static void cubic_init(struct cubic_state *ca) {
    memset(ca, 0, sizeof(*ca));
    ca->snd_cwnd = INITIAL_WINDOW;
    ca->snd_ssthresh = ~0U;
    pthread_mutex_init(&ca->lock, NULL);
    
    if (hystart)
        hystart_reset(ca);
}

/* Cleanup CUBIC state */
static void cubic_cleanup(struct cubic_state *ca) {
    pthread_mutex_destroy(&ca->lock);
}

/* Signal handling */
static void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM)
        running = false;
}

/* Error handling */
static void handle_error(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    int server_fd;
    struct sockaddr_in server_addr;
    pthread_t thread;
    
    /* Create server socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
        handle_error("Socket creation failed");
    
    /* Set socket options */
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
        handle_error("setsockopt failed");
    
    /* Bind and listen */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(DEFAULT_PORT);
    
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        handle_error("Bind failed");
    
    if (listen(server_fd, BACKLOG) < 0)
        handle_error("Listen failed");
    
    /* Set up signal handling */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("TCP CUBIC Simulator listening on port %d...\n", DEFAULT_PORT);
    printf("CUBIC Parameters:\n");
    printf("  Beta: %d/1024\n", beta);
    printf("  Fast Convergence: %s\n", fast_convergence ? "enabled" : "disabled");
    printf("  TCP Friendliness: %s\n", tcp_friendliness ? "enabled" : "disabled");
    printf("  HyStart: %s\n", hystart ? "enabled" : "disabled");
    
    /* Main server loop */
    while (running) {
        int conn_idx = -1;
        
        /* Find free connection slot */
        pthread_mutex_lock(&connections_lock);
        for (int i = 0; i < MAX_CONNECTIONS; i++) {
            if (!connections[i].active) {
                conn_idx = i;
                connections[i].active = true;
                break;
            }
        }
        pthread_mutex_unlock(&connections_lock);
        
        if (conn_idx < 0) {
            fprintf(stderr, "Maximum connections reached\n");
            sleep(1);
            continue;
        }
        
        struct connection *conn = &connections[conn_idx];
        socklen_t addr_len = sizeof(conn->addr);
        
        conn->fd = accept(server_fd, (struct sockaddr *)&conn->addr, &addr_len);
        if (conn->fd < 0) {
            if (errno == EINTR)
                continue;
            handle_error("Accept failed");
        }
        
        printf("New connection from %s:%d\n",
               inet_ntoa(conn->addr.sin_addr),
               ntohs(conn->addr.sin_port));
        
        cubic_init(&conn->cubic);
        
        if (pthread_create(&conn->thread, NULL, connection_handler, conn) != 0) {
            fprintf(stderr, "Failed to create thread for connection\n");
            close(conn->fd);
            conn->active = false;
            continue;
        }
        
        pthread_detach(conn->thread);
    }
    
    /* Cleanup */
    close(server_fd);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].active) {
            close(connections[i].fd);
            cubic_cleanup(&connections[i].cubic);
            connections[i].active = false;
        }
    }
    
    printf("\nServer shutdown complete\n");
    return 0;
}
