/*
 * TCP Output Simulation
 * Based on Linux kernel's TCP output implementation
 * 
 * This simulation implements:
 * - TCP packet transmission
 * - Window management
 * - MSS/TSO handling
 * - Nagle algorithm
 * - Congestion control
 * - Retransmission
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <errno.h>

/* Constants */
#define TCP_MAX_QUEUE 1024
#define TCP_DEFAULT_WINDOW 65535
#define TCP_MAX_WINDOW 1048576
#define TCP_DEFAULT_MSS 536
#define TCP_MAX_MSS 65535
#define TCP_MIN_MSS 88
#define TCP_TIMER_INTERVAL_MS 100
#define TCP_MAX_RETRIES 15
#define TCP_RTO_MIN 200  /* 200ms */
#define TCP_RTO_MAX 120000  /* 120 seconds */
#define TCP_INIT_RTO 3000  /* 3 seconds */

/* TCP Options */
#define TCPOPT_NOP 1
#define TCPOPT_EOL 0
#define TCPOPT_MSS 2
#define TCPOPT_WINDOW 3
#define TCPOPT_SACK_PERMITTED 4
#define TCPOPT_TIMESTAMP 8

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
    struct tcp_segment *next;
    uint32_t seq;
    uint32_t end_seq;
    uint32_t when;  /* Timestamp when sent */
    uint16_t len;
    uint8_t flags;
    uint8_t retries;
    bool acked;
    char data[0];
};

/* TCP Options */
struct tcp_options {
    uint16_t mss;
    uint8_t wscale;
    bool sack_ok;
    bool timestamps;
    uint32_t tsval;
    uint32_t tsecr;
};

/* TCP Socket */
struct tcp_sock {
    /* Connection Info */
    uint32_t saddr;
    uint32_t daddr;
    uint16_t sport;
    uint16_t dport;
    int state;
    
    /* Sequence Numbers */
    uint32_t snd_una;    /* First unacknowledged sequence number */
    uint32_t snd_nxt;    /* Next sequence number to send */
    uint32_t snd_wnd;    /* Send window */
    uint32_t rcv_nxt;    /* Next sequence number expected */
    uint32_t rcv_wnd;    /* Receive window */
    
    /* Congestion Control */
    uint32_t cwnd;       /* Congestion window */
    uint32_t ssthresh;   /* Slow start threshold */
    uint32_t rto;        /* Retransmission timeout */
    uint32_t srtt;       /* Smoothed RTT */
    uint32_t rttvar;     /* RTT variance */
    uint32_t rtt_seq;    /* Sequence number being timed */
    uint32_t packets_out;/* Packets in flight */
    bool in_recovery;    /* In fast recovery? */
    uint32_t recover;    /* Recovery point */
    
    /* Send/Receive Queues */
    struct tcp_segment *send_head;
    struct tcp_segment *send_tail;
    struct tcp_segment *retrans_head;
    
    /* TCP Options */
    struct tcp_options opts;
    
    /* Nagle's Algorithm */
    bool nodelay;        /* TCP_NODELAY set? */
    bool cork;           /* TCP_CORK set? */
    uint32_t mss_cache;  /* Current MSS including TCP options */
    
    /* Timers */
    uint32_t rto_timer;  /* Retransmission timer */
    uint32_t probe_timer;/* Zero window probe timer */
    
    /* Locks */
    pthread_mutex_t lock;
};

/* Get current time in milliseconds */
static uint32_t tcp_time_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

/* Initialize TCP socket */
static void tcp_init_sock(struct tcp_sock *tp) {
    memset(tp, 0, sizeof(*tp));
    tp->state = TCP_CLOSE;
    tp->snd_wnd = TCP_DEFAULT_WINDOW;
    tp->rcv_wnd = TCP_DEFAULT_WINDOW;
    tp->cwnd = 10;  /* Initial cwnd = 10 segments */
    tp->ssthresh = TCP_MAX_WINDOW;
    tp->rto = TCP_INIT_RTO;
    tp->mss_cache = TCP_DEFAULT_MSS;
    pthread_mutex_init(&tp->lock, NULL);
}

/* Calculate current RTT sample and update RTO */
static void tcp_rtt_estimator(struct tcp_sock *tp, uint32_t mrtt) {
    /* RFC6298 RTT estimation */
    if (tp->srtt == 0) {
        /* First measurement */
        tp->srtt = mrtt;
        tp->rttvar = mrtt / 2;
    } else {
        /* Update RTTVAR and SRTT */
        uint32_t delta = abs((int)(tp->srtt - mrtt));
        tp->rttvar = (3 * tp->rttvar + delta) / 4;
        tp->srtt = (7 * tp->srtt + mrtt) / 8;
    }
    
    /* Update RTO */
    tp->rto = tp->srtt + (tp->rttvar << 2);
    if (tp->rto < TCP_RTO_MIN)
        tp->rto = TCP_RTO_MIN;
    if (tp->rto > TCP_RTO_MAX)
        tp->rto = TCP_RTO_MAX;
}

/* Allocate new TCP segment */
static struct tcp_segment *tcp_alloc_segment(uint32_t size) {
    struct tcp_segment *seg = malloc(sizeof(*seg) + size);
    if (seg) {
        memset(seg, 0, sizeof(*seg));
        seg->len = size;
    }
    return seg;
}

/* Free TCP segment */
static void tcp_free_segment(struct tcp_segment *seg) {
    free(seg);
}

/* Queue segment for transmission */
static void tcp_queue_segment(struct tcp_sock *tp, struct tcp_segment *seg) {
    seg->next = NULL;
    if (tp->send_tail)
        tp->send_tail->next = seg;
    else
        tp->send_head = seg;
    tp->send_tail = seg;
}

/* Remove segment from transmission queue */
static void tcp_unqueue_segment(struct tcp_sock *tp, struct tcp_segment *seg) {
    struct tcp_segment *prev = NULL;
    struct tcp_segment *curr = tp->send_head;
    
    while (curr && curr != seg) {
        prev = curr;
        curr = curr->next;
    }
    
    if (curr) {
        if (prev)
            prev->next = curr->next;
        else
            tp->send_head = curr->next;
            
        if (curr == tp->send_tail)
            tp->send_tail = prev;
    }
}

/* Check if Nagle's algorithm allows transmission */
static bool tcp_nagle_check(struct tcp_sock *tp, struct tcp_segment *seg) {
    if (tp->nodelay)
        return true;
        
    if (tp->cork)
        return false;
        
    /* Allow if segment size >= MSS or no unacked data */
    if (seg->len >= tp->mss_cache || tp->packets_out == 0)
        return true;
        
    return false;
}

/* Congestion control - slow start */
static void tcp_slow_start(struct tcp_sock *tp) {
    tp->cwnd += 1;
    if (tp->cwnd >= tp->ssthresh)
        tp->cwnd = tp->ssthresh;
}

/* Congestion control - congestion avoidance */
static void tcp_cong_avoid(struct tcp_sock *tp) {
    tp->cwnd += 1;
}

/* Handle RTO timeout */
static void tcp_handle_rto(struct tcp_sock *tp) {
    /* RFC5681 congestion control */
    tp->ssthresh = max(tp->cwnd/2, 2);
    tp->cwnd = 1;
    tp->rto *= 2;  /* Exponential backoff */
    
    /* Move first unacked segment to retransmission queue */
    struct tcp_segment *seg = tp->send_head;
    while (seg && seg->acked)
        seg = seg->next;
        
    if (seg) {
        tcp_unqueue_segment(tp, seg);
        seg->next = tp->retrans_head;
        tp->retrans_head = seg;
        seg->retries++;
    }
}

/* Process incoming ACK */
static void tcp_handle_ack(struct tcp_sock *tp, uint32_t ack_seq, uint32_t win) {
    struct tcp_segment *seg = tp->send_head;
    bool new_data = false;
    
    /* Update send window */
    tp->snd_wnd = win;
    
    /* Process acknowledged segments */
    while (seg) {
        if (SEQ_LT(seg->seq, ack_seq)) {
            /* Segment fully acknowledged */
            if (!seg->acked) {
                seg->acked = true;
                tp->packets_out--;
                new_data = true;
                
                /* Update RTT if we were measuring it */
                if (seg->seq == tp->rtt_seq) {
                    uint32_t mrtt = tcp_time_now() - seg->when;
                    tcp_rtt_estimator(tp, mrtt);
                    tp->rtt_seq = 0;
                }
            }
        }
        seg = seg->next;
    }
    
    /* Clean up acknowledged segments */
    while (tp->send_head && tp->send_head->acked) {
        seg = tp->send_head;
        tp->send_head = seg->next;
        if (tp->send_head == NULL)
            tp->send_tail = NULL;
        tcp_free_segment(seg);
    }
    
    /* Update congestion control */
    if (new_data) {
        if (tp->cwnd < tp->ssthresh)
            tcp_slow_start(tp);
        else
            tcp_cong_avoid(tp);
    }
}

/* Transmit TCP segment */
static int tcp_transmit_segment(struct tcp_sock *tp, struct tcp_segment *seg) {
    struct sockaddr_in dest;
    struct tcphdr th;
    struct msghdr msg;
    struct iovec iov[2];
    char control[TCP_MAX_MSS];
    int ret;
    
    /* Build TCP header */
    memset(&th, 0, sizeof(th));
    th.source = htons(tp->sport);
    th.dest = htons(tp->dport);
    th.seq = htonl(seg->seq);
    th.ack_seq = htonl(tp->rcv_nxt);
    th.window = htons(tp->rcv_wnd);
    th.doff = 5;  /* 5 * 4 = 20 bytes */
    th.flags = seg->flags;
    
    /* Add TCP options */
    if (seg->seq == tp->snd_nxt) {  /* First transmission */
        char *ptr = control;
        
        /* MSS option */
        if (tp->opts.mss) {
            *ptr++ = TCPOPT_MSS;
            *ptr++ = 4;
            *((uint16_t*)ptr) = htons(tp->opts.mss);
            ptr += 2;
        }
        
        /* Window scale option */
        if (tp->opts.wscale) {
            *ptr++ = TCPOPT_WINDOW;
            *ptr++ = 3;
            *ptr++ = tp->opts.wscale;
        }
        
        /* SACK permitted option */
        if (tp->opts.sack_ok) {
            *ptr++ = TCPOPT_SACK_PERMITTED;
            *ptr++ = 2;
        }
        
        /* Timestamp option */
        if (tp->opts.timestamps) {
            *ptr++ = TCPOPT_TIMESTAMP;
            *ptr++ = 10;
            *((uint32_t*)ptr) = htonl(tcp_time_now());
            ptr += 4;
            *((uint32_t*)ptr) = htonl(tp->opts.tsecr);
            ptr += 4;
        }
        
        /* Update header length */
        th.doff = (ptr - control + 3) / 4;
    }
    
    /* Prepare socket address */
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = tp->daddr;
    dest.sin_port = htons(tp->dport);
    
    /* Set up IO vector */
    iov[0].iov_base = &th;
    iov[0].iov_len = th.doff * 4;
    iov[1].iov_base = seg->data;
    iov[1].iov_len = seg->len;
    
    /* Set up message header */
    memset(&msg, 0, sizeof(msg));
    msg.msg_name = &dest;
    msg.msg_namelen = sizeof(dest);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    
    /* Send segment */
    ret = sendmsg(tp->sock_fd, &msg, 0);
    if (ret > 0) {
        seg->when = tcp_time_now();
        if (!tp->rtt_seq)
            tp->rtt_seq = seg->seq;  /* Start RTT measurement */
        tp->packets_out++;
    }
    
    return ret;
}

/* Main transmission function */
static void tcp_write_xmit(struct tcp_sock *tp) {
    struct tcp_segment *seg;
    uint32_t cwnd_bytes;
    uint32_t now = tcp_time_now();
    
    /* First handle retransmissions */
    while (tp->retrans_head) {
        seg = tp->retrans_head;
        if (seg->retries >= TCP_MAX_RETRIES) {
            /* Too many retries - drop connection */
            tp->state = TCP_CLOSE;
            return;
        }
        
        if (tcp_transmit_segment(tp, seg) > 0) {
            tp->retrans_head = seg->next;
            tcp_queue_segment(tp, seg);
        } else {
            break;  /* Transmission failed */
        }
    }
    
    /* Calculate available congestion window in bytes */
    cwnd_bytes = tp->cwnd * tp->mss_cache;
    
    /* Send new segments */
    seg = tp->send_head;
    while (seg && !seg->acked) {
        /* Check congestion window */
        if (tp->packets_out * tp->mss_cache >= cwnd_bytes)
            break;
            
        /* Check send window */
        if (SEQ_GT(seg->end_seq, tp->snd_una + tp->snd_wnd))
            break;
            
        /* Check Nagle's algorithm */
        if (!tcp_nagle_check(tp, seg))
            break;
            
        /* Transmit segment */
        if (tcp_transmit_segment(tp, seg) <= 0)
            break;
            
        seg = seg->next;
    }
    
    /* Check for RTO timeout */
    seg = tp->send_head;
    while (seg && !seg->acked) {
        if (now - seg->when >= tp->rto) {
            tcp_handle_rto(tp);
            break;
        }
        seg = seg->next;
    }
}

/* Example usage */
int main(void) {
    struct tcp_sock tp;
    struct tcp_segment *seg;
    char data[] = "Hello, TCP!";
    int i;
    
    /* Initialize socket */
    tcp_init_sock(&tp);
    tp.state = TCP_ESTABLISHED;
    tp.saddr = inet_addr("192.168.1.1");
    tp.daddr = inet_addr("192.168.1.2");
    tp.sport = 12345;
    tp.dport = 80;
    
    /* Set up TCP options */
    tp.opts.mss = 1460;
    tp.opts.wscale = 7;
    tp.opts.sack_ok = true;
    tp.opts.timestamps = true;
    
    /* Queue some test segments */
    for (i = 0; i < 5; i++) {
        seg = tcp_alloc_segment(strlen(data));
        if (seg) {
            memcpy(seg->data, data, strlen(data));
            seg->seq = tp.snd_nxt;
            seg->end_seq = seg->seq + seg->len;
            tp.snd_nxt = seg->end_seq;
            tcp_queue_segment(&tp, seg);
        }
    }
    
    /* Main transmission loop */
    while (tp.state == TCP_ESTABLISHED) {
        tcp_write_xmit(&tp);
        usleep(TCP_TIMER_INTERVAL_MS * 1000);
    }
    
    /* Cleanup */
    while (tp.send_head) {
        seg = tp.send_head;
        tp.send_head = seg->next;
        tcp_free_segment(seg);
    }
    
    while (tp.retrans_head) {
        seg = tp.retrans_head;
        tp.retrans_head = seg->next;
        tcp_free_segment(seg);
    }
    
    pthread_mutex_destroy(&tp.lock);
    return 0;
}
