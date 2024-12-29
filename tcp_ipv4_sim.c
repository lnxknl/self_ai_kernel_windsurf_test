/*
 * TCP/IPv4 Protocol Simulation
 * Based on Linux kernel implementation
 * 
 * This simulation implements key aspects of TCP/IPv4 including:
 * - Connection establishment (3-way handshake)
 * - Connection state management
 * - Socket operations
 * - Packet handling and routing
 * - MD5 signature support
 * - Error handling (ICMP)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <openssl/md5.h>

/* Constants */
#define TCP_HEADER_LEN 20
#define IP_HEADER_LEN 20
#define MAX_PACKET_SIZE 1500
#define MAX_BACKLOG 128
#define TCP_INIT_WINDOW 65535
#define TCP_MAX_RETRIES 5
#define TCP_TIMEOUT_MS 1000
#define TCP_MAX_SOCKETS 1024
#define TCP_HASH_SIZE 1024
#define TCP_TIME_WAIT 60

/* TCP States */
enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECV,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSING,
    TCP_TIME_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK
};

/* TCP Flags */
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20
#define TCP_FLAG_ECE 0x40
#define TCP_FLAG_CWR 0x80

/* Socket Structure */
struct tcp_sock {
    int state;
    uint32_t saddr;
    uint32_t daddr;
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack_seq;
    uint16_t window;
    uint32_t ts_recent;
    uint32_t ts_recent_stamp;
    struct tcp_sock *next;
    pthread_mutex_t lock;
    
    /* Send/Receive Buffers */
    char *send_buf;
    char *recv_buf;
    int send_size;
    int recv_size;
    int send_head;
    int send_tail;
    int recv_head;
    int recv_tail;
    
    /* Timers */
    struct {
        int retransmit;
        int persist;
        int keepalive;
        int timewait;
    } timers;
    
    /* Statistics */
    struct {
        uint64_t packets_in;
        uint64_t packets_out;
        uint64_t bytes_in;
        uint64_t bytes_out;
        uint64_t retransmits;
        uint64_t drops;
    } stats;
    
    /* MD5 Authentication */
    int md5_enabled;
    char md5_key[16];
};

/* Hash Table for Socket Lookup */
struct tcp_hashbucket {
    struct tcp_sock *chain;
    pthread_rwlock_t lock;
};

/* Global Variables */
static struct tcp_hashbucket tcp_hashtable[TCP_HASH_SIZE];
static pthread_mutex_t tcp_create_lock = PTHREAD_MUTEX_INITIALIZER;
static int tcp_port_counter = 1024;

/* Function Declarations */
static uint32_t tcp_init_sequence(void);
static void tcp_init_sock(struct tcp_sock *sk);
static void tcp_destroy_sock(struct tcp_sock *sk);
static uint16_t tcp_checksum(const void *buff, size_t len);
static int tcp_hash_function(uint32_t saddr, uint32_t daddr, uint16_t sport, uint16_t dport);
static struct tcp_sock *tcp_lookup(uint32_t saddr, uint32_t daddr, uint16_t sport, uint16_t dport);
static void tcp_hash_insert(struct tcp_sock *sk);
static void tcp_hash_remove(struct tcp_sock *sk);
static int tcp_process_syn(struct tcp_sock *sk, const struct tcphdr *th);
static int tcp_process_ack(struct tcp_sock *sk, const struct tcphdr *th);
static int tcp_send_syn(struct tcp_sock *sk);
static int tcp_send_ack(struct tcp_sock *sk);
static int tcp_send_fin(struct tcp_sock *sk);
static int tcp_send_rst(struct tcp_sock *sk);
static void tcp_md5_hash_header(char *hash, const struct tcp_sock *sk, const struct tcphdr *th);

/* Initialize TCP Socket */
static void tcp_init_sock(struct tcp_sock *sk) {
    memset(sk, 0, sizeof(*sk));
    sk->state = TCP_CLOSED;
    sk->window = TCP_INIT_WINDOW;
    sk->seq = tcp_init_sequence();
    pthread_mutex_init(&sk->lock, NULL);
    
    /* Allocate buffers */
    sk->send_buf = malloc(TCP_INIT_WINDOW);
    sk->recv_buf = malloc(TCP_INIT_WINDOW);
    sk->send_size = TCP_INIT_WINDOW;
    sk->recv_size = TCP_INIT_WINDOW;
}

/* Generate Initial Sequence Number */
static uint32_t tcp_init_sequence(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint32_t)ts.tv_sec << 10) | ((uint32_t)ts.tv_nsec & 0x3FF);
}

/* Calculate TCP Checksum */
static uint16_t tcp_checksum(const void *buff, size_t len) {
    const uint16_t *ptr = buff;
    uint32_t sum = 0;
    size_t i;
    
    for (i = 0; i < len/2; i++)
        sum += ptr[i];
    
    if (len & 1)
        sum += ((uint8_t *)buff)[len-1];
    
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    
    return ~sum;
}

/* Hash Function for Socket Lookup */
static int tcp_hash_function(uint32_t saddr, uint32_t daddr, uint16_t sport, uint16_t dport) {
    return (saddr ^ daddr ^ ((uint32_t)sport << 16) ^ dport) & (TCP_HASH_SIZE - 1);
}

/* Socket Lookup */
static struct tcp_sock *tcp_lookup(uint32_t saddr, uint32_t daddr, uint16_t sport, uint16_t dport) {
    int hash = tcp_hash_function(saddr, daddr, sport, dport);
    struct tcp_sock *sk;
    
    pthread_rwlock_rdlock(&tcp_hashtable[hash].lock);
    for (sk = tcp_hashtable[hash].chain; sk; sk = sk->next) {
        if (sk->saddr == saddr && sk->daddr == daddr &&
            sk->sport == sport && sk->dport == dport)
            break;
    }
    pthread_rwlock_unlock(&tcp_hashtable[hash].lock);
    
    return sk;
}

/* Insert Socket into Hash Table */
static void tcp_hash_insert(struct tcp_sock *sk) {
    int hash = tcp_hash_function(sk->saddr, sk->daddr, sk->sport, sk->dport);
    
    pthread_rwlock_wrlock(&tcp_hashtable[hash].lock);
    sk->next = tcp_hashtable[hash].chain;
    tcp_hashtable[hash].chain = sk;
    pthread_rwlock_unlock(&tcp_hashtable[hash].lock);
}

/* Remove Socket from Hash Table */
static void tcp_hash_remove(struct tcp_sock *sk) {
    int hash = tcp_hash_function(sk->saddr, sk->daddr, sk->sport, sk->dport);
    struct tcp_sock **psk;
    
    pthread_rwlock_wrlock(&tcp_hashtable[hash].lock);
    for (psk = &tcp_hashtable[hash].chain; *psk; psk = &(*psk)->next) {
        if (*psk == sk) {
            *psk = sk->next;
            break;
        }
    }
    pthread_rwlock_unlock(&tcp_hashtable[hash].lock);
}

/* Process Incoming SYN Packet */
static int tcp_process_syn(struct tcp_sock *sk, const struct tcphdr *th) {
    if (sk->state != TCP_LISTEN)
        return -1;
    
    sk->daddr = ntohl(th->source);
    sk->ack_seq = ntohl(th->seq) + 1;
    sk->state = TCP_SYN_RECV;
    
    return tcp_send_syn(sk);
}

/* Process Incoming ACK Packet */
static int tcp_process_ack(struct tcp_sock *sk, const struct tcphdr *th) {
    uint32_t ack = ntohl(th->ack_seq);
    
    if (ack <= sk->seq)
        return 0;
    
    sk->seq = ack;
    
    switch (sk->state) {
        case TCP_SYN_SENT:
            if (th->syn) {
                sk->ack_seq = ntohl(th->seq) + 1;
                sk->state = TCP_ESTABLISHED;
                return tcp_send_ack(sk);
            }
            break;
            
        case TCP_SYN_RECV:
            sk->state = TCP_ESTABLISHED;
            break;
            
        case TCP_FIN_WAIT1:
            sk->state = TCP_FIN_WAIT2;
            break;
            
        case TCP_CLOSING:
            sk->state = TCP_TIME_WAIT;
            /* Start TIME_WAIT timer */
            break;
            
        case TCP_LAST_ACK:
            sk->state = TCP_CLOSED;
            tcp_destroy_sock(sk);
            break;
    }
    
    return 0;
}

/* Send SYN Packet */
static int tcp_send_syn(struct tcp_sock *sk) {
    struct {
        struct tcphdr th;
        char opt[4];  /* MSS option */
    } packet;
    
    memset(&packet, 0, sizeof(packet));
    packet.th.source = htons(sk->sport);
    packet.th.dest = htons(sk->dport);
    packet.th.seq = htonl(sk->seq);
    packet.th.doff = (sizeof(packet) >> 2);
    packet.th.syn = 1;
    packet.th.window = htons(sk->window);
    
    /* Add MSS option */
    packet.opt[0] = 2;  /* Kind: MSS */
    packet.opt[1] = 4;  /* Length */
    *((uint16_t *)&packet.opt[2]) = htons(1460);  /* MSS value */
    
    packet.th.check = tcp_checksum(&packet, sizeof(packet));
    
    /* Send packet using raw socket */
    sk->stats.packets_out++;
    sk->stats.bytes_out += sizeof(packet);
    
    return 0;
}

/* Send ACK Packet */
static int tcp_send_ack(struct tcp_sock *sk) {
    struct tcphdr th;
    
    memset(&th, 0, sizeof(th));
    th.source = htons(sk->sport);
    th.dest = htons(sk->dport);
    th.seq = htonl(sk->seq);
    th.ack_seq = htonl(sk->ack_seq);
    th.doff = sizeof(th) >> 2;
    th.ack = 1;
    th.window = htons(sk->window);
    
    th.check = tcp_checksum(&th, sizeof(th));
    
    /* Send packet using raw socket */
    sk->stats.packets_out++;
    sk->stats.bytes_out += sizeof(th);
    
    return 0;
}

/* Send FIN Packet */
static int tcp_send_fin(struct tcp_sock *sk) {
    struct tcphdr th;
    
    memset(&th, 0, sizeof(th));
    th.source = htons(sk->sport);
    th.dest = htons(sk->dport);
    th.seq = htonl(sk->seq);
    th.ack_seq = htonl(sk->ack_seq);
    th.doff = sizeof(th) >> 2;
    th.fin = 1;
    th.ack = 1;
    th.window = htons(sk->window);
    
    th.check = tcp_checksum(&th, sizeof(th));
    
    /* Send packet using raw socket */
    sk->stats.packets_out++;
    sk->stats.bytes_out += sizeof(th);
    sk->seq++;
    
    return 0;
}

/* Send RST Packet */
static int tcp_send_rst(struct tcp_sock *sk) {
    struct tcphdr th;
    
    memset(&th, 0, sizeof(th));
    th.source = htons(sk->sport);
    th.dest = htons(sk->dport);
    th.seq = htonl(sk->seq);
    th.doff = sizeof(th) >> 2;
    th.rst = 1;
    
    th.check = tcp_checksum(&th, sizeof(th));
    
    /* Send packet using raw socket */
    sk->stats.packets_out++;
    sk->stats.bytes_out += sizeof(th);
    
    return 0;
}

/* MD5 Authentication */
static void tcp_md5_hash_header(char *hash, const struct tcp_sock *sk, const struct tcphdr *th) {
    MD5_CTX ctx;
    
    MD5_Init(&ctx);
    
    /* Hash TCP pseudo-header */
    MD5_Update(&ctx, &sk->saddr, sizeof(sk->saddr));
    MD5_Update(&ctx, &sk->daddr, sizeof(sk->daddr));
    MD5_Update(&ctx, th, sizeof(*th));
    
    /* Add key */
    if (sk->md5_enabled)
        MD5_Update(&ctx, sk->md5_key, sizeof(sk->md5_key));
    
    MD5_Final((unsigned char *)hash, &ctx);
}

/* Socket Destruction */
static void tcp_destroy_sock(struct tcp_sock *sk) {
    tcp_hash_remove(sk);
    
    pthread_mutex_destroy(&sk->lock);
    free(sk->send_buf);
    free(sk->recv_buf);
    free(sk);
}

/* Main TCP Processing Function */
static void *tcp_process_packet(void *arg) {
    struct tcp_sock *sk = arg;
    char packet[MAX_PACKET_SIZE];
    struct iphdr *iph;
    struct tcphdr *th;
    ssize_t len;
    int ret;
    
    while (1) {
        /* Receive packet (simulated) */
        len = /* receive packet */;
        if (len < 0)
            continue;
            
        if (len < sizeof(*iph) + sizeof(*th))
            continue;
            
        iph = (struct iphdr *)packet;
        th = (struct tcphdr *)(packet + sizeof(*iph));
        
        /* Verify checksum */
        if (tcp_checksum(th, len - sizeof(*iph)) != 0)
            continue;
            
        pthread_mutex_lock(&sk->lock);
        
        /* Process packet based on state */
        switch (sk->state) {
            case TCP_LISTEN:
                if (th->syn)
                    ret = tcp_process_syn(sk, th);
                break;
                
            case TCP_SYN_SENT:
                if (th->syn && th->ack)
                    ret = tcp_process_ack(sk, th);
                break;
                
            case TCP_ESTABLISHED:
                if (th->fin) {
                    sk->state = TCP_CLOSE_WAIT;
                    sk->ack_seq++;
                    ret = tcp_send_ack(sk);
                } else if (th->ack) {
                    ret = tcp_process_ack(sk, th);
                }
                break;
                
            case TCP_FIN_WAIT1:
            case TCP_FIN_WAIT2:
                if (th->fin) {
                    sk->state = TCP_TIME_WAIT;
                    sk->ack_seq++;
                    ret = tcp_send_ack(sk);
                    /* Start TIME_WAIT timer */
                } else if (th->ack) {
                    ret = tcp_process_ack(sk, th);
                }
                break;
                
            case TCP_CLOSING:
            case TCP_LAST_ACK:
                if (th->ack)
                    ret = tcp_process_ack(sk, th);
                break;
                
            case TCP_TIME_WAIT:
                if (th->fin) {
                    ret = tcp_send_ack(sk);
                    /* Restart TIME_WAIT timer */
                }
                break;
        }
        
        pthread_mutex_unlock(&sk->lock);
        
        /* Update statistics */
        sk->stats.packets_in++;
        sk->stats.bytes_in += len;
    }
    
    return NULL;
}

/* Initialize TCP Subsystem */
int tcp_init(void) {
    int i;
    
    /* Initialize hash table */
    for (i = 0; i < TCP_HASH_SIZE; i++) {
        tcp_hashtable[i].chain = NULL;
        pthread_rwlock_init(&tcp_hashtable[i].lock, NULL);
    }
    
    return 0;
}

/* Create TCP Socket */
struct tcp_sock *tcp_create_sock(void) {
    struct tcp_sock *sk = malloc(sizeof(*struct tcp_sock));
    if (!sk)
        return NULL;
        
    tcp_init_sock(sk);
    return sk;
}

/* Main Function - Example Usage */
int main(int argc, char *argv[]) {
    struct tcp_sock *sk;
    pthread_t thread;
    
    /* Initialize TCP subsystem */
    if (tcp_init() < 0) {
        fprintf(stderr, "Failed to initialize TCP\n");
        return 1;
    }
    
    /* Create socket */
    sk = tcp_create_sock();
    if (!sk) {
        fprintf(stderr, "Failed to create socket\n");
        return 1;
    }
    
    /* Set up socket */
    sk->saddr = inet_addr("192.168.1.1");
    sk->sport = 12345;
    sk->state = TCP_LISTEN;
    
    /* Insert into hash table */
    tcp_hash_insert(sk);
    
    /* Create processing thread */
    pthread_create(&thread, NULL, tcp_process_packet, sk);
    
    /* Wait for thread */
    pthread_join(thread, NULL);
    
    /* Cleanup */
    tcp_destroy_sock(sk);
    
    return 0;
}
