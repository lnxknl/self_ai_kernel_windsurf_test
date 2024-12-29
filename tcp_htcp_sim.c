// SPDX-License-Identifier: GPL-2.0-only
/*
 * H-TCP Simulator: Implementation of H-TCP congestion control
 * Based on the paper:
 * R.N.Shorten, D.J.Leith:
 *   "H-TCP: TCP for high-speed and long-distance networks"
 *   Proc. PFLDnet, Argonne, 2004.
 * https://www.hamilton.ie/net/htcp3.pdf
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

/* H-TCP Parameters */
#define ALPHA_BASE       (1<<7)  /* 1.0 with shift << 7 */
#define BETA_MIN        (1<<6)  /* 0.5 with shift << 7 */
#define BETA_MAX        102     /* 0.8 with shift << 7 */
#define HZ              1000    /* System timer frequency */
#define DEFAULT_PORT    8889
#define BACKLOG         5
#define MAX_CONNECTIONS 100
#define MAX_PACKET_SIZE 1500
#define INITIAL_WINDOW  10

/* Network simulation parameters */
#define MIN_RTT_MS      20      /* Minimum RTT in milliseconds */
#define MAX_RTT_MS      200     /* Maximum RTT in milliseconds */
#define LOSS_RATE       0.001   /* Packet loss rate */
#define BW_MBPS         100     /* Link bandwidth in Mbps */

/* Global configuration */
static int use_rtt_scaling = 1;
static int use_bandwidth_switch = 1;
static volatile bool running = true;

/* H-TCP Connection State */
struct htcp {
    uint32_t alpha;         /* Fixed point arith, << 7 */
    uint8_t  beta;          /* Fixed point arith, << 7 */
    uint8_t  modeswitch;    /* Delay modeswitch until first congestion */
    uint16_t pkts_acked;
    uint32_t packetcount;
    uint32_t minRTT;        /* Minimum RTT seen */
    uint32_t maxRTT;        /* Maximum RTT seen */
    uint32_t last_cong;     /* Time since last congestion */
    uint32_t undo_last_cong;

    uint32_t undo_maxRTT;
    uint32_t undo_old_maxB;

    /* Bandwidth estimation */
    uint32_t minB;          /* Minimum bandwidth */
    uint32_t maxB;          /* Maximum bandwidth */
    uint32_t old_maxB;      /* Previous maximum bandwidth */
    uint32_t Bi;            /* Current bandwidth estimate */
    uint32_t lasttime;      /* Last bandwidth measurement time */

    /* Congestion control state */
    uint32_t snd_cwnd;      /* Congestion window */
    uint32_t ssthresh;      /* Slow start threshold */
    
    /* RTT measurement */
    uint32_t srtt;          /* Smoothed RTT */
    uint32_t rttvar;        /* RTT variance */
    
    /* Simulation specific */
    pthread_mutex_t lock;
    bool in_recovery;
    uint32_t recovery_start;
    uint32_t last_ack;
    uint32_t bytes_acked;
    uint32_t total_bytes;
};

/* Connection state */
struct connection {
    int fd;
    struct sockaddr_in addr;
    struct htcp htcp;
    pthread_t thread;
    bool active;
};

/* Global variables */
static struct connection connections[MAX_CONNECTIONS];
static pthread_mutex_t connections_lock = PTHREAD_MUTEX_INITIALIZER;

/* Time utilities */
static uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint32_t get_jiffies(void) {
    return get_time_us() / (1000000 / HZ);
}

/* H-TCP core functions */
static inline uint32_t htcp_cong_time(const struct htcp *ca) {
    return get_jiffies() - ca->last_cong;
}

static inline uint32_t htcp_ccount(const struct htcp *ca) {
    return htcp_cong_time(ca) / (ca->minRTT ? : 1);
}

static void htcp_reset(struct htcp *ca) {
    ca->undo_last_cong = ca->last_cong;
    ca->undo_maxRTT = ca->maxRTT;
    ca->undo_old_maxB = ca->old_maxB;
    ca->last_cong = get_jiffies();
    ca->in_recovery = true;
    ca->recovery_start = get_jiffies();
}

static uint32_t htcp_cwnd_undo(struct htcp *ca) {
    if (ca->undo_last_cong) {
        ca->last_cong = ca->undo_last_cong;
        ca->maxRTT = ca->undo_maxRTT;
        ca->old_maxB = ca->undo_old_maxB;
        ca->undo_last_cong = 0;
    }
    return ca->snd_cwnd;
}

static void measure_rtt(struct htcp *ca, uint32_t rtt_us) {
    uint32_t srtt = rtt_us / 1000;  /* Convert to ms */
    
    /* Update minimum RTT */
    if (ca->minRTT > srtt || !ca->minRTT)
        ca->minRTT = srtt;

    /* Update maximum RTT when not in recovery */
    if (!ca->in_recovery) {
        if (ca->maxRTT < ca->minRTT)
            ca->maxRTT = ca->minRTT;
        if (ca->maxRTT < srtt && srtt <= ca->maxRTT + 20)
            ca->maxRTT = srtt;
    }

    /* Update SRTT and RTTVAR using standard TCP calculations */
    if (ca->srtt == 0) {
        ca->srtt = srtt;
        ca->rttvar = srtt / 2;
    } else {
        uint32_t delta = abs((int)(srtt - ca->srtt));
        ca->rttvar = (3 * ca->rttvar + delta) / 4;
        ca->srtt = (7 * ca->srtt + srtt) / 8;
    }
}

static void measure_achieved_throughput(struct htcp *ca, uint32_t acked, uint32_t rtt_us) {
    uint32_t now = get_jiffies();
    
    if (!ca->in_recovery)
        ca->pkts_acked = acked;

    if (rtt_us > 0)
        measure_rtt(ca, rtt_us);

    if (!use_bandwidth_switch)
        return;

    /* Bandwidth estimation */
    ca->packetcount += acked;
    
    if (ca->packetcount >= ca->snd_cwnd - (ca->alpha >> 7 ? : 1) &&
        now - ca->lasttime >= ca->minRTT && ca->minRTT > 0) {
        
        uint32_t cur_Bi = ca->packetcount * HZ / (now - ca->lasttime);

        if (htcp_ccount(ca) <= 3) {
            /* Just after backoff */
            ca->minB = ca->maxB = ca->Bi = cur_Bi;
        } else {
            ca->Bi = (3 * ca->Bi + cur_Bi) / 4;
            if (ca->Bi > ca->maxB)
                ca->maxB = ca->Bi;
            if (ca->minB > ca->maxB)
                ca->minB = ca->maxB;
        }
        
        ca->packetcount = 0;
        ca->lasttime = now;
    }
}

static void htcp_beta_update(struct htcp *ca) {
    if (use_bandwidth_switch) {
        uint32_t maxB = ca->maxB;
        uint32_t old_maxB = ca->old_maxB;

        ca->old_maxB = maxB;
        if (!(5 * maxB >= 4 * old_maxB && 5 * maxB <= 6 * old_maxB)) {
            ca->beta = BETA_MIN;
            ca->modeswitch = 0;
            return;
        }
    }

    if (ca->modeswitch && ca->minRTT > 10 && ca->maxRTT) {
        ca->beta = (ca->minRTT << 7) / ca->maxRTT;
        if (ca->beta < BETA_MIN)
            ca->beta = BETA_MIN;
        else if (ca->beta > BETA_MAX)
            ca->beta = BETA_MAX;
    } else {
        ca->beta = BETA_MIN;
        ca->modeswitch = 1;
    }
}

static void htcp_alpha_update(struct htcp *ca) {
    uint32_t minRTT = ca->minRTT;
    uint32_t factor = 1;
    uint32_t diff = htcp_cong_time(ca);

    if (diff > HZ) {
        diff -= HZ;
        factor = 1 + (10 * diff + ((diff / 2) * (diff / 2) / HZ)) / HZ;
    }

    if (use_rtt_scaling && minRTT) {
        uint32_t scale = (HZ << 3) / (10 * minRTT);
        scale = min(max(scale, 1U << 2), 10U << 3);
        factor = (factor << 3) / scale;
        if (!factor)
            factor = 1;
    }

    ca->alpha = 2 * factor * ((1 << 7) - ca->beta);
    if (!ca->alpha)
        ca->alpha = ALPHA_BASE;
}

static void htcp_param_update(struct htcp *ca) {
    htcp_beta_update(ca);
    htcp_alpha_update(ca);

    /* Add slowly fading memory for maxRTT */
    if (ca->minRTT > 0 && ca->maxRTT > ca->minRTT)
        ca->maxRTT = ca->minRTT + ((ca->maxRTT - ca->minRTT) * 95) / 100;
}

static uint32_t htcp_recalc_ssthresh(struct htcp *ca) {
    htcp_param_update(ca);
    return max((ca->snd_cwnd * ca->beta) >> 7, 2U);
}

static void htcp_cong_avoid(struct htcp *ca, uint32_t acked) {
    if (ca->snd_cwnd <= ca->ssthresh) {
        /* Slow start */
        ca->snd_cwnd += acked;
    } else {
        /* Congestion avoidance */
        uint32_t cnt = ca->snd_cwnd / (ca->alpha >> 7);
        if (cnt > acked * 2)
            cnt = acked * 2;
        
        ca->bytes_acked += acked;
        if (ca->bytes_acked >= cnt) {
            ca->bytes_acked = 0;
            ca->snd_cwnd++;
        }
        htcp_alpha_update(ca);
    }
}

/* Connection handling */
static void htcp_init(struct htcp *ca) {
    memset(ca, 0, sizeof(*ca));
    ca->alpha = ALPHA_BASE;
    ca->beta = BETA_MIN;
    ca->pkts_acked = 1;
    ca->snd_cwnd = INITIAL_WINDOW;
    ca->ssthresh = 0x7fffffff;
    ca->last_cong = get_jiffies();
    pthread_mutex_init(&ca->lock, NULL);
}

static void htcp_cleanup(struct htcp *ca) {
    pthread_mutex_destroy(&ca->lock);
}

static void simulate_network_conditions(struct htcp *ca, uint32_t *rtt_us, bool *packet_loss) {
    /* Simulate variable RTT */
    *rtt_us = MIN_RTT_MS * 1000 + 
              (rand() % (MAX_RTT_MS - MIN_RTT_MS)) * 1000;
    
    /* Simulate random packet loss */
    *packet_loss = ((double)rand() / RAND_MAX) < LOSS_RATE;
}

static void *connection_handler(void *arg) {
    struct connection *conn = (struct connection *)arg;
    struct htcp *ca = &conn->htcp;
    char buffer[MAX_PACKET_SIZE];
    ssize_t bytes;
    uint32_t rtt_us;
    bool packet_loss;
    
    while (running && conn->active) {
        /* Simulate receiving data */
        bytes = recv(conn->fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0) {
            if (bytes < 0 && errno == EINTR)
                continue;
            break;
        }

        pthread_mutex_lock(&ca->lock);
        
        /* Simulate network conditions */
        simulate_network_conditions(ca, &rtt_us, &packet_loss);
        
        if (packet_loss) {
            /* Handle packet loss */
            ca->ssthresh = htcp_recalc_ssthresh(ca);
            ca->snd_cwnd = ca->ssthresh;
            htcp_reset(ca);
        } else {
            /* Normal packet processing */
            measure_achieved_throughput(ca, bytes, rtt_us);
            htcp_cong_avoid(ca, bytes);
        }
        
        /* Simulate sending response */
        uint32_t send_window = min(ca->snd_cwnd, MAX_PACKET_SIZE);
        
        pthread_mutex_unlock(&ca->lock);
        
        if (send(conn->fd, buffer, send_window, 0) < 0) {
            if (errno != EINTR)
                break;
        }
        
        /* Add artificial delay to simulate RTT */
        usleep(rtt_us);
    }
    
    close(conn->fd);
    conn->active = false;
    return NULL;
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

/* Print statistics */
static void print_stats(struct htcp *ca) {
    printf("\nH-TCP Statistics:\n");
    printf("  Congestion Window: %u\n", ca->snd_cwnd);
    printf("  Slow Start Threshold: %u\n", ca->ssthresh);
    printf("  Alpha: %u\n", ca->alpha);
    printf("  Beta: %u\n", ca->beta);
    printf("  Min RTT: %u ms\n", ca->minRTT);
    printf("  Max RTT: %u ms\n", ca->maxRTT);
    printf("  Bandwidth: %u packets/s\n", ca->Bi);
}

int main(int argc, char *argv[]) {
    int server_fd;
    struct sockaddr_in server_addr;
    
    /* Initialize random number generator */
    srand(time(NULL));
    
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
    
    printf("H-TCP Simulator listening on port %d...\n", DEFAULT_PORT);
    printf("Configuration:\n");
    printf("  RTT Scaling: %s\n", use_rtt_scaling ? "enabled" : "disabled");
    printf("  Bandwidth Switch: %s\n", use_bandwidth_switch ? "enabled" : "disabled");
    printf("  Beta Min: %d/128\n", BETA_MIN);
    printf("  Beta Max: %d/128\n", BETA_MAX);
    printf("  Initial Window: %d\n", INITIAL_WINDOW);
    
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
        
        htcp_init(&conn->htcp);
        
        if (pthread_create(&conn->thread, NULL, connection_handler, conn) != 0) {
            fprintf(stderr, "Failed to create thread for connection\n");
            close(conn->fd);
            conn->active = false;
            continue;
        }
        
        pthread_detach(conn->thread);
        
        /* Print periodic statistics for the first connection */
        if (conn_idx == 0) {
            print_stats(&conn->htcp);
        }
    }
    
    /* Cleanup */
    close(server_fd);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].active) {
            close(connections[i].fd);
            htcp_cleanup(&connections[i].htcp);
            connections[i].active = false;
        }
    }
    
    printf("\nServer shutdown complete\n");
    return 0;
}
