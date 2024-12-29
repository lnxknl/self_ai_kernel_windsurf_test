// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * TCP Protocol Implementation Simulation
 * This is a standalone simulation of core TCP functionality
 * that can be compiled and run independently of the Linux kernel.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdbool.h>

#define TCP_MAX_WINDOW 65535
#define TCP_DEFAULT_WINDOW 8192
#define TCP_MIN_MSS 536
#define TCP_DEFAULT_MSS 1460
#define TCP_MAX_RETRIES 15
#define TCP_RTO_MIN 200  // Minimum RTO in milliseconds
#define TCP_RTO_MAX 120000  // Maximum RTO in milliseconds
#define TCP_INIT_RTO 3000  // Initial RTO in milliseconds
#define TCP_MAX_PACKETS 1000

// TCP States
enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECV,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_CLOSING,
    TCP_MAX_STATES
};

// TCP Flags
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20

// TCP Options
struct tcp_options {
    uint16_t mss;
    uint8_t window_scale;
    bool sack_permitted;
    bool timestamps;
    uint32_t ts_val;
    uint32_t ts_ecr;
};

// TCP Segment Structure
struct tcp_segment {
    uint32_t seq;
    uint32_t ack;
    uint16_t window;
    uint8_t flags;
    struct tcp_options opts;
    char *data;
    size_t len;
    struct timespec sent_time;
    int retries;
    struct tcp_segment *next;
};

// TCP Socket Structure
struct tcp_sock {
    enum tcp_state state;
    uint32_t snd_una;    // oldest unacknowledged sequence number
    uint32_t snd_nxt;    // next sequence number to be sent
    uint32_t snd_wnd;    // send window
    uint32_t rcv_nxt;    // next sequence number expected
    uint32_t rcv_wnd;    // receive window
    uint32_t iss;        // initial send sequence number
    uint32_t irs;        // initial receive sequence number
    uint16_t mss;        // maximum segment size
    
    // Congestion Control
    uint32_t cwnd;       // congestion window
    uint32_t ssthresh;   // slow start threshold
    bool in_slow_start;
    bool in_recovery;
    uint32_t recover;    // recovery point
    
    // RTT Estimation
    uint32_t srtt;       // smoothed round-trip time
    uint32_t rttvar;     // round-trip time variation
    uint32_t rto;        // retransmission timeout
    bool rtt_measured;
    
    // Nagle's Algorithm
    bool nodelay;        // TCP_NODELAY option
    struct tcp_segment *unsent_data;
    
    // Retransmission Queue
    struct tcp_segment *retrans_queue;
    pthread_mutex_t retrans_lock;
    
    // Send/Receive Buffers
    char *send_buf;
    size_t send_buf_size;
    size_t send_buf_used;
    char *recv_buf;
    size_t recv_buf_size;
    size_t recv_buf_used;
    
    // Socket Options
    bool cork;
    int keepalive_time;
    int keepalive_intvl;
    int keepalive_probes;
    
    // Timestamps
    struct timespec last_ack_time;
    struct timespec last_send_time;
};

// Helper Functions
static uint32_t tcp_random_iss(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
}

static uint32_t tcp_time_stamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void tcp_init_sock(struct tcp_sock *tp) {
    memset(tp, 0, sizeof(*tp));
    tp->state = TCP_CLOSED;
    tp->mss = TCP_DEFAULT_MSS;
    tp->snd_wnd = TCP_DEFAULT_WINDOW;
    tp->rcv_wnd = TCP_DEFAULT_WINDOW;
    tp->cwnd = 2 * tp->mss;
    tp->ssthresh = TCP_MAX_WINDOW;
    tp->in_slow_start = true;
    tp->rto = TCP_INIT_RTO;
    tp->send_buf_size = TCP_MAX_WINDOW;
    tp->recv_buf_size = TCP_MAX_WINDOW;
    tp->send_buf = malloc(tp->send_buf_size);
    tp->recv_buf = malloc(tp->recv_buf_size);
    pthread_mutex_init(&tp->retrans_lock, NULL);
}

static void tcp_cleanup_sock(struct tcp_sock *tp) {
    struct tcp_segment *seg, *next;
    
    // Clean up retransmission queue
    pthread_mutex_lock(&tp->retrans_lock);
    seg = tp->retrans_queue;
    while (seg) {
        next = seg->next;
        free(seg->data);
        free(seg);
        seg = next;
    }
    pthread_mutex_unlock(&tp->retrans_lock);
    
    // Clean up unsent data queue
    seg = tp->unsent_data;
    while (seg) {
        next = seg->next;
        free(seg->data);
        free(seg);
        seg = next;
    }
    
    // Free buffers
    free(tp->send_buf);
    free(tp->recv_buf);
    
    pthread_mutex_destroy(&tp->retrans_lock);
}

// RTT Estimation
static void tcp_update_rtt(struct tcp_sock *tp, uint32_t measured_rtt) {
    if (!tp->rtt_measured) {
        tp->srtt = measured_rtt;
        tp->rttvar = measured_rtt / 2;
        tp->rtt_measured = true;
    } else {
        tp->rttvar = (3 * tp->rttvar + abs((int)(tp->srtt - measured_rtt))) / 4;
        tp->srtt = (7 * tp->srtt + measured_rtt) / 8;
    }
    
    // Update RTO (RFC 6298)
    tp->rto = tp->srtt + 4 * tp->rttvar;
    if (tp->rto < TCP_RTO_MIN)
        tp->rto = TCP_RTO_MIN;
    if (tp->rto > TCP_RTO_MAX)
        tp->rto = TCP_RTO_MAX;
}

// Congestion Control
static void tcp_enter_slow_start(struct tcp_sock *tp) {
    tp->cwnd = 2 * tp->mss;
    tp->in_slow_start = true;
    tp->in_recovery = false;
}

static void tcp_enter_cong_avoid(struct tcp_sock *tp) {
    tp->in_slow_start = false;
    tp->in_recovery = false;
}

static void tcp_cong_avoid(struct tcp_sock *tp, uint32_t ack) {
    if (tp->in_slow_start) {
        tp->cwnd += tp->mss;
        if (tp->cwnd >= tp->ssthresh)
            tcp_enter_cong_avoid(tp);
    } else {
        // Additive increase
        tp->cwnd += (tp->mss * tp->mss) / tp->cwnd;
    }
}

static void tcp_enter_recovery(struct tcp_sock *tp) {
    tp->ssthresh = tp->cwnd / 2;
    if (tp->ssthresh < 2 * tp->mss)
        tp->ssthresh = 2 * tp->mss;
    tp->cwnd = tp->ssthresh;
    tp->in_recovery = true;
    tp->recover = tp->snd_nxt;
}

// Segment Management
static struct tcp_segment *tcp_alloc_segment(const char *data, size_t len) {
    struct tcp_segment *seg = malloc(sizeof(*seg));
    if (!seg)
        return NULL;
    
    memset(seg, 0, sizeof(*seg));
    if (len > 0) {
        seg->data = malloc(len);
        if (!seg->data) {
            free(seg);
            return NULL;
        }
        memcpy(seg->data, data, len);
    }
    seg->len = len;
    clock_gettime(CLOCK_MONOTONIC, &seg->sent_time);
    return seg;
}

static void tcp_queue_segment(struct tcp_sock *tp, struct tcp_segment *seg) {
    pthread_mutex_lock(&tp->retrans_lock);
    seg->next = tp->retrans_queue;
    tp->retrans_queue = seg;
    pthread_mutex_unlock(&tp->retrans_lock);
}

static void tcp_clean_retrans_queue(struct tcp_sock *tp, uint32_t ack) {
    struct tcp_segment *seg, *prev = NULL, *next;
    
    pthread_mutex_lock(&tp->retrans_lock);
    seg = tp->retrans_queue;
    while (seg) {
        next = seg->next;
        if (SEQ_LT(seg->seq + seg->len, ack)) {
            // Segment fully acknowledged
            if (prev)
                prev->next = next;
            else
                tp->retrans_queue = next;
            free(seg->data);
            free(seg);
        } else {
            prev = seg;
        }
        seg = next;
    }
    pthread_mutex_unlock(&tp->retrans_lock);
}

// Nagle's Algorithm Implementation
static bool tcp_nagle_check(struct tcp_sock *tp, size_t len) {
    if (tp->nodelay)
        return true;
    
    if (len >= tp->mss)
        return true;
    
    if (!tp->unsent_data && SEQ_EQ(tp->snd_una, tp->snd_nxt))
        return true;
    
    return false;
}

// Main TCP Output Function
static int tcp_transmit(struct tcp_sock *tp, const char *data, size_t len, int flags) {
    struct tcp_segment *seg;
    size_t curr_len;
    
    while (len > 0) {
        curr_len = min(len, tp->mss);
        
        // Check Nagle's algorithm
        if (!tcp_nagle_check(tp, curr_len)) {
            // Queue data for later transmission
            seg = tcp_alloc_segment(data, curr_len);
            if (!seg)
                return -ENOMEM;
            
            seg->next = tp->unsent_data;
            tp->unsent_data = seg;
            return 0;
        }
        
        // Create and queue segment
        seg = tcp_alloc_segment(data, curr_len);
        if (!seg)
            return -ENOMEM;
        
        seg->seq = tp->snd_nxt;
        seg->flags = flags | TCP_FLAG_ACK;
        seg->window = min(tp->rcv_wnd, TCP_MAX_WINDOW);
        
        // Queue for retransmission
        tcp_queue_segment(tp, seg);
        
        // Update sequence numbers
        tp->snd_nxt += curr_len;
        data += curr_len;
        len -= curr_len;
        
        // Update timestamps
        clock_gettime(CLOCK_MONOTONIC, &tp->last_send_time);
    }
    
    return 0;
}

// Retransmission Timer Handler
static void *tcp_retransmit_timer(void *arg) {
    struct tcp_sock *tp = arg;
    struct tcp_segment *seg;
    struct timespec now, diff;
    uint32_t rto_ms;
    
    while (tp->state != TCP_CLOSED) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        
        pthread_mutex_lock(&tp->retrans_lock);
        seg = tp->retrans_queue;
        while (seg) {
            timespec_sub(&diff, &now, &seg->sent_time);
            rto_ms = tp->rto * (1 << seg->retries);
            
            if (diff.tv_sec * 1000 + diff.tv_nsec / 1000000 >= rto_ms) {
                // Retransmission timeout
                if (seg->retries >= TCP_MAX_RETRIES) {
                    // Connection failure
                    tp->state = TCP_CLOSED;
                    pthread_mutex_unlock(&tp->retrans_lock);
                    return NULL;
                }
                
                // Exponential backoff
                seg->retries++;
                clock_gettime(CLOCK_MONOTONIC, &seg->sent_time);
                
                // Adjust congestion control
                tp->ssthresh = max(tp->cwnd / 2, 2 * tp->mss);
                tp->cwnd = tp->mss;
                tp->in_slow_start = true;
            }
            seg = seg->next;
        }
        pthread_mutex_unlock(&tp->retrans_lock);
        
        usleep(100000);  // Sleep for 100ms
    }
    
    return NULL;
}

// TCP State Machine
static void tcp_set_state(struct tcp_sock *tp, enum tcp_state state) {
    enum tcp_state oldstate = tp->state;
    tp->state = state;
    
    switch (state) {
        case TCP_ESTABLISHED:
            if (oldstate != TCP_ESTABLISHED)
                printf("Connection established\n");
            break;
            
        case TCP_CLOSE:
            if (oldstate == TCP_CLOSE_WAIT || oldstate == TCP_ESTABLISHED)
                printf("Connection reset\n");
            break;
            
        default:
            if (oldstate == TCP_ESTABLISHED)
                printf("Connection closed\n");
            break;
    }
}

// Main TCP Input Processing
static void tcp_rcv(struct tcp_sock *tp, struct tcp_segment *seg) {
    uint32_t old_snd_una;
    
    // Basic validation
    if (seg->flags & TCP_FLAG_RST) {
        tcp_set_state(tp, TCP_CLOSED);
        return;
    }
    
    // Process ACKs
    if (seg->flags & TCP_FLAG_ACK) {
        old_snd_una = tp->snd_una;
        if (SEQ_GT(seg->ack, tp->snd_una) && SEQ_LEQ(seg->ack, tp->snd_nxt)) {
            tp->snd_una = seg->ack;
            
            // Update RTT if possible
            if (old_snd_una != tp->snd_una) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                uint32_t rtt = (now.tv_sec - tp->last_send_time.tv_sec) * 1000 +
                (now.tv_nsec - tp->last_send_time.tv_nsec) / 1000000;
                tcp_update_rtt(tp, rtt);
            }
            
            // Clean up retransmission queue
            tcp_clean_retrans_queue(tp, seg->ack);
            
            // Update congestion control
            tcp_cong_avoid(tp, seg->ack);
        }
    }
    
    // Process data
    if (seg->len > 0) {
        if (SEQ_GE(seg->seq, tp->rcv_nxt) &&
            SEQ_LT(seg->seq, tp->rcv_nxt + tp->rcv_wnd)) {
            // In-sequence data
            if (tp->recv_buf_used + seg->len <= tp->recv_buf_size) {
                memcpy(tp->recv_buf + tp->recv_buf_used, seg->data, seg->len);
                tp->recv_buf_used += seg->len;
                tp->rcv_nxt += seg->len;
            }
        }
        
        // Send ACK
        struct tcp_segment *ack = tcp_alloc_segment(NULL, 0);
        if (ack) {
            ack->flags = TCP_FLAG_ACK;
            ack->ack = tp->rcv_nxt;
            ack->window = min(tp->rcv_wnd, TCP_MAX_WINDOW);
            tcp_transmit(tp, NULL, 0, TCP_FLAG_ACK);
            free(ack);
        }
    }
}

// Example Usage
int main(void) {
    struct tcp_sock tcp;
    pthread_t timer_thread;
    
    // Initialize TCP socket
    tcp_init_sock(&tcp);
    
    // Start retransmission timer
    pthread_create(&timer_thread, NULL, tcp_retransmit_timer, &tcp);
    
    // Simulate TCP connection
    tcp.iss = tcp_random_iss();
    tcp.snd_nxt = tcp.iss + 1;
    tcp_set_state(&tcp, TCP_SYN_SENT);
    
    // Simulate data transmission
    const char *data = "Hello, TCP!";
    tcp_transmit(&tcp, data, strlen(data), 0);
    
    // Wait for a while
    sleep(5);
    
    // Cleanup
    tcp_set_state(&tcp, TCP_CLOSED);
    pthread_join(timer_thread, NULL);
    tcp_cleanup_sock(&tcp);
    
    return 0;
}
