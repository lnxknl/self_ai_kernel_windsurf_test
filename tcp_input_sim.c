/*
 * TCP Input Processing Simulation
 * Based on Linux TCP implementation
 * 
 * This simulation implements key aspects of TCP input processing including:
 * - RTT estimation and RTO calculation
 * - SACK processing and scoreboard management
 * - Congestion control state machine
 * - Receive buffer management
 * - ACK processing
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#define TCP_MAX_SACKS 4
#define TCP_TIMEOUT_INIT 1000  /* Initial RTO value in ms */
#define TCP_RTO_MIN 200       /* Minimum RTO value in ms */
#define TCP_RTO_MAX 120000    /* Maximum RTO value in ms */
#define TCP_REORDERING 3      /* Maximum reordering distance */
#define TCP_MSS 1460          /* Maximum segment size */
#define TCP_INIT_CWND 10      /* Initial congestion window in segments */
#define TCP_MAX_WINDOW 65535  /* Maximum window size */
#define TCP_BUFFER_SIZE (256 * 1024)  /* Socket buffer size */

/* TCP Flags */
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20
#define TCP_FLAG_ECE 0x40
#define TCP_FLAG_CWR 0x80

/* TCP States */
enum tcp_state {
    TCP_ESTABLISHED = 1,
    TCP_SYN_SENT,
    TCP_SYN_RECV,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_TIME_WAIT,
    TCP_CLOSE,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_LISTEN,
    TCP_CLOSING
};

/* TCP Segment */
struct tcp_segment {
    uint32_t seq;
    uint32_t ack_seq;
    uint16_t flags;
    uint16_t window;
    uint32_t length;
    struct timeval timestamp;
    uint8_t *data;
    struct tcp_segment *next;
};

/* SACK Block */
struct tcp_sack_block {
    uint32_t start_seq;
    uint32_t end_seq;
};

/* TCP Socket Structure */
struct tcp_sock {
    /* Connection Info */
    int state;
    uint32_t snd_una;    /* First unacknowledged sequence number */
    uint32_t snd_nxt;    /* Next sequence number to send */
    uint32_t rcv_nxt;    /* Next sequence number expected */
    uint32_t snd_wnd;    /* Send window */
    uint32_t rcv_wnd;    /* Receive window */
    
    /* Congestion Control */
    uint32_t snd_cwnd;   /* Congestion window */
    uint32_t ssthresh;   /* Slow start threshold */
    uint32_t lost_out;   /* Lost packets */
    uint32_t retrans_out; /* Retransmitted packets */
    uint32_t sacked_out;  /* SACKed packets */
    
    /* RTT Measurement */
    uint32_t srtt;       /* Smoothed RTT in usec */
    uint32_t rttvar;     /* RTT variance */
    uint32_t rto;        /* Retransmission timeout */
    struct timeval last_rtt_measure;
    
    /* SACK Info */
    struct tcp_sack_block sack_table[TCP_MAX_SACKS];
    int num_sacks;
    
    /* Reordering Detection */
    int reordering;      /* Packet reordering metric */
    uint32_t high_seq;   /* Highest sequence number sent */
    
    /* Send/Receive Buffers */
    struct tcp_segment *send_head;
    struct tcp_segment *send_tail;
    struct tcp_segment *receive_queue;
    
    /* Locks */
    pthread_mutex_t sock_lock;
    
    /* Statistics */
    uint64_t total_retrans;
    uint64_t total_sacks;
    uint64_t total_packets;
    uint64_t total_bytes;
};

/* RTT Measurement Functions */
static void tcp_rtt_estimator(struct tcp_sock *tp, long mrtt_us) {
    long m = mrtt_us;
    uint32_t srtt = tp->srtt;

    /* If this is the first measurement, initialize srtt and rttvar */
    if (srtt == 0) {
        tp->srtt = m << 3;
        tp->rttvar = m << 1;
        return;
    }

    /* Update RTTVAR first */
    m -= (srtt >> 3);
    tp->rttvar += (abs(m) - tp->rttvar) >> 2;

    /* Update SRTT */
    tp->srtt += m >> 3;

    /* Update RTO */
    tp->rto = (tp->srtt >> 3) + tp->rttvar;
    
    /* Bound RTO */
    if (tp->rto < TCP_RTO_MIN)
        tp->rto = TCP_RTO_MIN;
    else if (tp->rto > TCP_RTO_MAX)
        tp->rto = TCP_RTO_MAX;
}

/* SACK Processing Functions */
static void tcp_clean_sack_table(struct tcp_sock *tp) {
    memset(tp->sack_table, 0, sizeof(tp->sack_table));
    tp->num_sacks = 0;
}

static void tcp_add_sack(struct tcp_sock *tp, uint32_t start_seq, uint32_t end_seq) {
    int i;
    
    /* Don't add if full */
    if (tp->num_sacks >= TCP_MAX_SACKS)
        return;
        
    /* Check for overlap with existing blocks */
    for (i = 0; i < tp->num_sacks; i++) {
        if (start_seq <= tp->sack_table[i].end_seq &&
            end_seq >= tp->sack_table[i].start_seq)
            return;
    }
    
    /* Add new SACK block */
    tp->sack_table[tp->num_sacks].start_seq = start_seq;
    tp->sack_table[tp->num_sacks].end_seq = end_seq;
    tp->num_sacks++;
    tp->total_sacks++;
}

/* Congestion Control Functions */
static void tcp_enter_loss(struct tcp_sock *tp) {
    /* Reduce congestion window */
    tp->ssthresh = max(tp->snd_cwnd >> 1, 2U);
    tp->snd_cwnd = tp->ssthresh;
    
    /* Reset SACK state */
    tcp_clean_sack_table(tp);
    
    /* Clear all in flight packets */
    tp->lost_out = 0;
    tp->retrans_out = 0;
    tp->sacked_out = 0;
}

static void tcp_cong_avoid(struct tcp_sock *tp) {
    if (tp->snd_cwnd <= tp->ssthresh) {
        /* Slow start */
        tp->snd_cwnd++;
    } else {
        /* Congestion avoidance */
        tp->snd_cwnd += TCP_MSS * TCP_MSS / tp->snd_cwnd;
    }
}

/* Segment Processing Functions */
static struct tcp_segment *tcp_alloc_segment(uint32_t seq, uint32_t len) {
    struct tcp_segment *seg = malloc(sizeof(*seg));
    if (!seg)
        return NULL;
        
    memset(seg, 0, sizeof(*seg));
    seg->seq = seq;
    seg->length = len;
    if (len > 0) {
        seg->data = malloc(len);
        if (!seg->data) {
            free(seg);
            return NULL;
        }
    }
    gettimeofday(&seg->timestamp, NULL);
    return seg;
}

static void tcp_free_segment(struct tcp_segment *seg) {
    if (seg->data)
        free(seg->data);
    free(seg);
}

static void tcp_queue_segment(struct tcp_sock *tp, struct tcp_segment *seg) {
    pthread_mutex_lock(&tp->sock_lock);
    
    if (!tp->send_head) {
        tp->send_head = tp->send_tail = seg;
    } else {
        tp->send_tail->next = seg;
        tp->send_tail = seg;
    }
    
    pthread_mutex_unlock(&tp->sock_lock);
}

/* ACK Processing */
static void tcp_ack_update_window(struct tcp_sock *tp, uint32_t ack, uint16_t window) {
    uint32_t old_snd_wnd = tp->snd_wnd;
    
    /* Update send window */
    tp->snd_wnd = window;
    
    /* Handle window update */
    if (tp->snd_wnd > old_snd_wnd) {
        /* Window grew, check if we can send more data */
        // Implementation would trigger sending more data
    }
}

static void tcp_clean_rtx_queue(struct tcp_sock *tp, uint32_t ack_seq) {
    struct tcp_segment *seg, *next;
    
    pthread_mutex_lock(&tp->sock_lock);
    
    seg = tp->send_head;
    while (seg && SEQ_LT(seg->seq + seg->length, ack_seq)) {
        next = seg->next;
        
        /* Measure RTT if possible */
        if (!(seg->flags & TCP_FLAG_RETRANS)) {
            struct timeval now;
            long rtt;
            
            gettimeofday(&now, NULL);
            rtt = (now.tv_sec - seg->timestamp.tv_sec) * 1000000 +
                  (now.tv_usec - seg->timestamp.tv_usec);
            tcp_rtt_estimator(tp, rtt);
        }
        
        tcp_free_segment(seg);
        tp->send_head = next;
        if (!next)
            tp->send_tail = NULL;
            
        seg = next;
    }
    
    pthread_mutex_unlock(&tp->sock_lock);
}

/* Main Input Processing Function */
void tcp_input_process(struct tcp_sock *tp, const uint8_t *packet, size_t len) {
    struct tcp_segment seg;
    uint32_t seq, ack_seq;
    uint16_t flags, window;
    
    /* Parse TCP header */
    if (len < 20)
        return;
        
    seq = ntohl(*(uint32_t *)(packet + 4));
    ack_seq = ntohl(*(uint32_t *)(packet + 8));
    flags = ntohs(*(uint16_t *)(packet + 12)) & 0xFF;
    window = ntohs(*(uint16_t *)(packet + 14));
    
    /* Process ACK */
    if (flags & TCP_FLAG_ACK) {
        /* Update window */
        tcp_ack_update_window(tp, ack_seq, window);
        
        /* Clean retransmission queue */
        tcp_clean_rtx_queue(tp, ack_seq);
        
        /* Update congestion control */
        tcp_cong_avoid(tp);
    }
    
    /* Process SACK blocks if present */
    if (len > 20) {
        const uint8_t *ptr = packet + 20;
        const uint8_t *end = packet + len;
        
        while (ptr + 8 <= end) {
            uint32_t start = ntohl(*(uint32_t *)ptr);
            uint32_t end_seq = ntohl(*(uint32_t *)(ptr + 4));
            
            tcp_add_sack(tp, start, end_seq);
            ptr += 8;
        }
    }
    
    /* Process data */
    if (len > 20 && (flags & TCP_FLAG_PSH)) {
        size_t data_len = len - 20;
        struct tcp_segment *new_seg;
        
        /* Allocate and queue new segment */
        new_seg = tcp_alloc_segment(seq, data_len);
        if (new_seg) {
            memcpy(new_seg->data, packet + 20, data_len);
            tcp_queue_segment(tp, new_seg);
            
            tp->total_packets++;
            tp->total_bytes += data_len;
        }
    }
}

/* Socket Creation and Initialization */
struct tcp_sock *tcp_sock_create(void) {
    struct tcp_sock *tp = malloc(sizeof(*tp));
    if (!tp)
        return NULL;
        
    memset(tp, 0, sizeof(*tp));
    
    /* Initialize congestion control */
    tp->snd_cwnd = TCP_INIT_CWND;
    tp->ssthresh = TCP_MAX_WINDOW;
    
    /* Initialize RTT measurement */
    tp->rto = TCP_TIMEOUT_INIT;
    
    /* Initialize reordering detection */
    tp->reordering = TCP_REORDERING;
    
    /* Initialize mutex */
    pthread_mutex_init(&tp->sock_lock, NULL);
    
    return tp;
}

void tcp_sock_destroy(struct tcp_sock *tp) {
    struct tcp_segment *seg, *next;
    
    /* Free send queue */
    seg = tp->send_head;
    while (seg) {
        next = seg->next;
        tcp_free_segment(seg);
        seg = next;
    }
    
    /* Free receive queue */
    seg = tp->receive_queue;
    while (seg) {
        next = seg->next;
        tcp_free_segment(seg);
        seg = next;
    }
    
    pthread_mutex_destroy(&tp->sock_lock);
    free(tp);
}

/* Statistics Functions */
void tcp_print_stats(const struct tcp_sock *tp) {
    printf("TCP Statistics:\n");
    printf("  Total Packets: %lu\n", tp->total_packets);
    printf("  Total Bytes: %lu\n", tp->total_bytes);
    printf("  Total Retransmissions: %lu\n", tp->total_retrans);
    printf("  Total SACKs: %lu\n", tp->total_sacks);
    printf("  Current CWND: %u\n", tp->snd_cwnd);
    printf("  Current SSTHRESH: %u\n", tp->ssthresh);
    printf("  Current RTO: %u ms\n", tp->rto / 1000);
    printf("  SRTT: %u us\n", tp->srtt >> 3);
    printf("  RTTVAR: %u us\n", tp->rttvar);
}

/* Main Function - Example Usage */
int main(int argc, char *argv[]) {
    struct tcp_sock *tp;
    uint8_t packet[2048];
    size_t len;
    int i;
    
    /* Create TCP socket */
    tp = tcp_sock_create();
    if (!tp) {
        fprintf(stderr, "Failed to create TCP socket\n");
        return 1;
    }
    
    /* Simulate receiving packets */
    for (i = 0; i < 1000; i++) {
        /* Simulate packet arrival */
        len = /* Generate packet data */;
        tcp_input_process(tp, packet, len);
        
        /* Periodically print statistics */
        if ((i + 1) % 100 == 0)
            tcp_print_stats(tp);
    }
    
    /* Cleanup */
    tcp_sock_destroy(tp);
    return 0;
}
