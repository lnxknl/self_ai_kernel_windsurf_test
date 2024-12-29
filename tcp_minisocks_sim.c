/*
 * TCP Mini-Sockets Simulation
 * Based on Linux kernel's TCP mini-sockets implementation
 * 
 * This simulation implements:
 * - TIME_WAIT state management
 * - Request socket handling
 * - Connection establishment
 * - TCP options processing
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
#include <netinet/tcp.h>

/* Constants */
#define TCP_TIMEWAIT_LEN (60 * 2)  // 2 minutes in seconds
#define TCP_FIN_TIMEOUT (60)       // 1 minute in seconds
#define MAX_TCP_HEADER_SIZE 60
#define TCP_WINDOW_DEFAULT 65535
#define TCP_INIT_CWND 10
#define TCP_MAX_REORDERING 3
#define MAX_TCP_FASTOPEN_COOKIE 16

/* TCP States */
enum {
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

/* TCP Substate for TIME_WAIT */
enum {
    TCP_TIME_WAIT_SUBSTATE = 1,
    TCP_FIN_WAIT2_SUBSTATE
};

/* TCP Options */
struct tcp_options_received {
    uint32_t tsval;
    uint32_t tsecr;
    uint16_t mss;
    uint8_t wscale;
    bool saw_tstamp;
    bool saw_sack;
    bool sack_ok;
    bool wscale_ok;
};

/* TCP Fast Open Cookie */
struct tcp_fastopen_cookie {
    uint8_t val[MAX_TCP_FASTOPEN_COOKIE];
    uint8_t len;
    bool exp;  // Experimental option
};

/* Request Socket */
struct request_sock {
    struct request_sock *next;
    uint32_t rcv_nxt;
    uint32_t snt_isn;
    uint16_t mss;
    uint8_t num_retrans;
    uint8_t syn_ack_retries;
    
    struct tcp_fastopen_cookie fastopen_cookie;
    struct tcp_options_received opt;
    
    /* Addressing */
    uint32_t saddr;
    uint32_t daddr;
    uint16_t sport;
    uint16_t dport;
    
    /* Timestamps */
    uint32_t ts_recent;
    uint32_t ts_recent_stamp;
    uint32_t expires;
    
    /* Window */
    uint16_t window_clamp;
    uint8_t rcv_wscale;
    bool sack_ok;
    bool tstamp_ok;
    bool wscale_ok;
};

/* TIME_WAIT Socket */
struct tcp_timewait_sock {
    /* Common fields */
    uint32_t tw_rcv_nxt;
    uint32_t tw_snd_nxt;
    uint32_t tw_rcv_wnd;
    uint32_t tw_ts_recent;
    uint32_t tw_ts_recent_stamp;
    uint32_t tw_ts_offset;
    
    /* Addressing */
    uint32_t tw_saddr;
    uint32_t tw_daddr;
    uint16_t tw_sport;
    uint16_t tw_dport;
    
    /* State */
    uint8_t tw_substate;
    time_t tw_timeout;
    
    /* Rate limiting */
    time_t tw_last_oow_ack_time;
    
    struct tcp_fastopen_cookie fastopen_cookie;
    pthread_mutex_t lock;
};

/* Hash Table for TIME_WAIT Sockets */
#define TIMEWAIT_HASHBITS 8
#define TIMEWAIT_HASHSIZE (1 << TIMEWAIT_HASHBITS)

struct tw_hash_bucket {
    struct tcp_timewait_sock *chain;
    pthread_mutex_t lock;
};

static struct tw_hash_bucket tw_hash[TIMEWAIT_HASHSIZE];

/* Function Declarations */
static uint32_t tcp_tw_hash_func(uint32_t saddr, uint32_t daddr,
                                uint16_t sport, uint16_t dport);
static bool tcp_in_window(uint32_t seq, uint32_t end_seq,
                         uint32_t s_win, uint32_t e_win);
static void tcp_parse_options(const uint8_t *ptr, int len,
                            struct tcp_options_received *opt);
static bool tcp_paws_check(const struct tcp_timewait_sock *tw,
                          struct tcp_options_received *opt);
static void tcp_timewait_enter(struct tcp_timewait_sock *tw);
static void tcp_timewait_kill(struct tcp_timewait_sock *tw);
static struct tcp_timewait_sock *tcp_timewait_lookup(uint32_t saddr,
                                                    uint32_t daddr,
                                                    uint16_t sport,
                                                    uint16_t dport);
static void tcp_openreq_init(struct request_sock *req,
                            struct tcp_options_received *opt,
                            const struct tcphdr *th);

/* Hash Function for TIME_WAIT Sockets */
static uint32_t tcp_tw_hash_func(uint32_t saddr, uint32_t daddr,
                                uint16_t sport, uint16_t dport) {
    return (saddr ^ daddr ^ ((uint32_t)sport << 16) ^ dport) & (TIMEWAIT_HASHSIZE - 1);
}

/* Check if Sequence Number is in Window */
static bool tcp_in_window(uint32_t seq, uint32_t end_seq,
                         uint32_t s_win, uint32_t e_win) {
    if (seq == s_win)
        return true;
    if (end_seq > s_win && seq < e_win)
        return true;
    return seq == e_win && seq == end_seq;
}

/* Parse TCP Options */
static void tcp_parse_options(const uint8_t *ptr, int len,
                            struct tcp_options_received *opt) {
    int i;
    
    memset(opt, 0, sizeof(*opt));
    
    for (i = 0; i < len; ) {
        switch (ptr[i]) {
            case TCPOPT_EOL:
                return;
                
            case TCPOPT_NOP:
                i++;
                continue;
                
            case TCPOPT_MSS:
                if (i + 4 > len) return;
                opt->mss = (ptr[i+2] << 8) | ptr[i+3];
                i += 4;
                break;
                
            case TCPOPT_WINDOW:
                if (i + 3 > len) return;
                opt->wscale = ptr[i+2];
                opt->wscale_ok = true;
                i += 3;
                break;
                
            case TCPOPT_TIMESTAMP:
                if (i + 10 > len) return;
                opt->tsval = (ptr[i+2] << 24) | (ptr[i+3] << 16) |
                            (ptr[i+4] << 8) | ptr[i+5];
                opt->tsecr = (ptr[i+6] << 24) | (ptr[i+7] << 16) |
                            (ptr[i+8] << 8) | ptr[i+9];
                opt->saw_tstamp = true;
                i += 10;
                break;
                
            case TCPOPT_SACK_PERMITTED:
                if (i + 2 > len) return;
                opt->sack_ok = true;
                i += 2;
                break;
                
            default:
                if (i + 2 > len) return;
                i += ptr[i+1];
        }
    }
}

/* PAWS Check for TIME_WAIT Sockets */
static bool tcp_paws_check(const struct tcp_timewait_sock *tw,
                          struct tcp_options_received *opt) {
    if (!opt->saw_tstamp)
        return false;
        
    if ((int32_t)(opt->tsval - tw->tw_ts_recent) >= 0)
        return false;
        
    if ((uint32_t)time(NULL) >= tw->tw_ts_recent_stamp + TCP_PAWS_24DAYS)
        return false;
        
    return true;
}

/* Enter TIME_WAIT State */
static void tcp_timewait_enter(struct tcp_timewait_sock *tw) {
    uint32_t hash = tcp_tw_hash_func(tw->tw_saddr, tw->tw_daddr,
                                    tw->tw_sport, tw->tw_dport);
    struct tw_hash_bucket *bucket = &tw_hash[hash];
    
    tw->tw_timeout = time(NULL) + TCP_TIMEWAIT_LEN;
    
    pthread_mutex_lock(&bucket->lock);
    tw->next = bucket->chain;
    bucket->chain = tw;
    pthread_mutex_unlock(&bucket->lock);
}

/* Kill TIME_WAIT Socket */
static void tcp_timewait_kill(struct tcp_timewait_sock *tw) {
    uint32_t hash = tcp_tw_hash_func(tw->tw_saddr, tw->tw_daddr,
                                    tw->tw_sport, tw->tw_dport);
    struct tw_hash_bucket *bucket = &tw_hash[hash];
    struct tcp_timewait_sock **pprev;
    
    pthread_mutex_lock(&bucket->lock);
    
    pprev = &bucket->chain;
    while (*pprev) {
        if (*pprev == tw) {
            *pprev = tw->next;
            break;
        }
        pprev = &(*pprev)->next;
    }
    
    pthread_mutex_unlock(&bucket->lock);
    
    pthread_mutex_destroy(&tw->lock);
    free(tw);
}

/* Lookup TIME_WAIT Socket */
static struct tcp_timewait_sock *tcp_timewait_lookup(uint32_t saddr,
                                                    uint32_t daddr,
                                                    uint16_t sport,
                                                    uint16_t dport) {
    uint32_t hash = tcp_tw_hash_func(saddr, daddr, sport, dport);
    struct tw_hash_bucket *bucket = &tw_hash[hash];
    struct tcp_timewait_sock *tw;
    
    pthread_mutex_lock(&bucket->lock);
    
    for (tw = bucket->chain; tw; tw = tw->next) {
        if (tw->tw_saddr == saddr && tw->tw_daddr == daddr &&
            tw->tw_sport == sport && tw->tw_dport == dport)
            break;
    }
    
    pthread_mutex_unlock(&bucket->lock);
    return tw;
}

/* Initialize Request Socket */
static void tcp_openreq_init(struct request_sock *req,
                            struct tcp_options_received *opt,
                            const struct tcphdr *th) {
    req->rcv_nxt = ntohl(th->seq) + 1;
    req->snt_isn = rand();  // Should use better ISN generation
    req->mss = opt->mss ? : 536;
    req->num_retrans = 0;
    req->syn_ack_retries = 0;
    
    /* Copy TCP options */
    memcpy(&req->opt, opt, sizeof(*opt));
    
    /* Window parameters */
    req->window_clamp = TCP_WINDOW_DEFAULT;
    req->rcv_wscale = opt->wscale_ok ? opt->wscale : 0;
    req->sack_ok = opt->sack_ok;
    req->tstamp_ok = opt->saw_tstamp;
    req->wscale_ok = opt->wscale_ok;
    
    if (opt->saw_tstamp) {
        req->ts_recent = opt->tsval;
        req->ts_recent_stamp = time(NULL);
    }
}

/* Process Incoming Segment for TIME_WAIT Socket */
static int tcp_timewait_state_process(struct tcp_timewait_sock *tw,
                                     struct tcphdr *th,
                                     uint32_t seq, uint32_t end_seq) {
    struct tcp_options_received tmp_opt;
    bool paws_reject = false;
    
    /* Parse options */
    if (th->doff > sizeof(*th)/4 && tw->tw_ts_recent_stamp) {
        tcp_parse_options((uint8_t *)(th + 1), (th->doff*4) - sizeof(*th),
                         &tmp_opt);
        
        if (tmp_opt.saw_tstamp) {
            paws_reject = tcp_paws_check(tw, &tmp_opt);
        }
    }
    
    /* Process based on substate */
    if (tw->tw_substate == TCP_FIN_WAIT2_SUBSTATE) {
        /* FIN-WAIT-2 processing */
        if (paws_reject || !tcp_in_window(seq, end_seq,
                                         tw->tw_rcv_nxt,
                                         tw->tw_rcv_nxt + tw->tw_rcv_wnd))
            return 1;  // Send ACK
            
        if (th->rst)
            return 0;  // Kill socket
            
        if (th->syn && !before(seq, tw->tw_rcv_nxt))
            return 2;  // Send RST
            
        if (!th->ack || !after(end_seq, tw->tw_rcv_nxt) ||
            end_seq == seq)
            return -1;  // Drop segment
            
        if (th->fin && end_seq == tw->tw_rcv_nxt + 1) {
            /* Enter TIME-WAIT state */
            tw->tw_substate = TCP_TIME_WAIT_SUBSTATE;
            tw->tw_rcv_nxt = end_seq;
            
            if (tmp_opt.saw_tstamp) {
                tw->tw_ts_recent = tmp_opt.tsval;
                tw->tw_ts_recent_stamp = time(NULL);
            }
            
            tw->tw_timeout = time(NULL) + TCP_TIMEWAIT_LEN;
            return 1;  // Send ACK
        }
        
        return 2;  // Send RST
    } else {
        /* TIME-WAIT processing */
        if (!paws_reject &&
            seq == tw->tw_rcv_nxt &&
            (seq == end_seq || th->rst)) {
            if (th->rst)
                return 0;  // Kill socket
                
            /* Update timestamp if present */
            if (tmp_opt.saw_tstamp) {
                tw->tw_ts_recent = tmp_opt.tsval;
                tw->tw_ts_recent_stamp = time(NULL);
            }
            
            /* Restart TIME-WAIT */
            tw->tw_timeout = time(NULL) + TCP_TIMEWAIT_LEN;
            return 1;  // Send ACK
        }
        
        if (paws_reject || !tcp_in_window(seq, end_seq,
                                         tw->tw_rcv_nxt,
                                         tw->tw_rcv_nxt + tw->tw_rcv_wnd))
            return 1;  // Send ACK
    }
    
    return -1;  // Drop segment
}

/* Initialize TIME_WAIT Hash Table */
static void tcp_timewait_init(void) {
    int i;
    
    for (i = 0; i < TIMEWAIT_HASHSIZE; i++) {
        tw_hash[i].chain = NULL;
        pthread_mutex_init(&tw_hash[i].lock, NULL);
    }
}

/* Cleanup TIME_WAIT Hash Table */
static void tcp_timewait_cleanup(void) {
    int i;
    struct tcp_timewait_sock *tw, *next;
    
    for (i = 0; i < TIMEWAIT_HASHSIZE; i++) {
        pthread_mutex_lock(&tw_hash[i].lock);
        
        tw = tw_hash[i].chain;
        while (tw) {
            next = tw->next;
            pthread_mutex_destroy(&tw->lock);
            free(tw);
            tw = next;
        }
        
        pthread_mutex_unlock(&tw_hash[i].lock);
        pthread_mutex_destroy(&tw_hash[i].lock);
    }
}

/* Example Usage */
int main(void) {
    struct tcp_timewait_sock *tw;
    struct tcphdr th;
    struct tcp_options_received opt;
    uint32_t seq, end_seq;
    int ret;
    
    /* Initialize TIME_WAIT system */
    tcp_timewait_init();
    
    /* Create a TIME_WAIT socket */
    tw = calloc(1, sizeof(*tw));
    if (!tw) {
        fprintf(stderr, "Failed to allocate TIME_WAIT socket\n");
        return 1;
    }
    
    pthread_mutex_init(&tw->lock, NULL);
    
    /* Set up socket */
    tw->tw_saddr = inet_addr("192.168.1.1");
    tw->tw_daddr = inet_addr("192.168.1.2");
    tw->tw_sport = htons(12345);
    tw->tw_dport = htons(80);
    tw->tw_rcv_nxt = 1000;
    tw->tw_rcv_wnd = TCP_WINDOW_DEFAULT;
    tw->tw_substate = TCP_FIN_WAIT2_SUBSTATE;
    
    /* Enter TIME_WAIT state */
    tcp_timewait_enter(tw);
    
    /* Simulate incoming segment */
    memset(&th, 0, sizeof(th));
    th.source = htons(80);
    th.dest = htons(12345);
    th.seq = htonl(1000);
    th.ack_seq = htonl(2000);
    th.fin = 1;
    th.ack = 1;
    th.doff = sizeof(th)/4;
    
    seq = ntohl(th.seq);
    end_seq = seq + 1;  // +1 for FIN
    
    /* Process segment */
    ret = tcp_timewait_state_process(tw, &th, seq, end_seq);
    printf("Process result: %d\n", ret);
    
    /* Cleanup */
    tcp_timewait_cleanup();
    
    return 0;
}
