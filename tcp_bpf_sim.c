// SPDX-License-Identifier: GPL-2.0
/*
 * TCP BPF Simulator
 * This is a simplified simulation of the Linux kernel's TCP BPF functionality
 * for educational purposes. It demonstrates the core concepts of TCP packet
 * processing with BPF hooks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdatomic.h>
#include <stdbool.h>

#define MAX_PACKET_SIZE 65535
#define MAX_QUEUE_SIZE 1024
#define MAX_BPF_PROGRAMS 32
#define MAX_MSG_SIZE 8192
#define DEFAULT_PORT 8888
#define BACKLOG 5

// Simulated BPF program types
enum bpf_prog_type {
    BPF_PROG_TYPE_SOCKET_FILTER,
    BPF_PROG_TYPE_TCP_LISTEN,
    BPF_PROG_TYPE_TCP_DATA,
    BPF_PROG_TYPE_MAX
};

// Simulated socket message structure
struct sk_msg {
    void *data;
    size_t len;
    size_t offset;
    struct sk_msg *next;
    atomic_int refcount;
    bool in_use;
};

// Simulated socket buffer structure
struct sk_buff {
    unsigned char *head;
    unsigned char *data;
    size_t len;
    size_t data_len;
    atomic_int users;
    struct sk_buff *next;
    struct sk_buff *prev;
};

// Simulated BPF program structure
struct bpf_prog {
    enum bpf_prog_type type;
    int (*filter)(void *ctx, void *data, size_t len);
    void *private_data;
    char name[32];
    atomic_int refcount;
};

// Simulated socket structure
struct sock {
    int fd;
    struct sockaddr_in addr;
    struct sk_buff_head rx_queue;
    struct sk_buff_head tx_queue;
    struct bpf_prog *progs[BPF_PROG_TYPE_MAX];
    pthread_mutex_t lock;
    atomic_bool closed;
    void *sk_user_data;
};

// Socket buffer queue
struct sk_buff_head {
    struct sk_buff *first;
    struct sk_buff *last;
    int qlen;
    pthread_mutex_t lock;
};

// Global variables
static atomic_int g_active_connections = 0;
static pthread_mutex_t g_prog_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct bpf_prog *g_prog_array[MAX_BPF_PROGRAMS];
static int g_prog_count = 0;

// Forward declarations
static void sk_buff_head_init(struct sk_buff_head *list);
static void sk_buff_head_destroy(struct sk_buff_head *list);
static int sk_buff_queue_tail(struct sk_buff_head *list, struct sk_buff *skb);
static struct sk_buff *sk_buff_dequeue(struct sk_buff_head *list);

// Memory management functions
static void *safe_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "Memory allocation failed\n");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

// Initialize socket buffer head
static void sk_buff_head_init(struct sk_buff_head *list) {
    list->first = NULL;
    list->last = NULL;
    list->qlen = 0;
    pthread_mutex_init(&list->lock, NULL);
}

// Destroy socket buffer head
static void sk_buff_head_destroy(struct sk_buff_head *list) {
    pthread_mutex_lock(&list->lock);
    struct sk_buff *skb = list->first;
    while (skb) {
        struct sk_buff *next = skb->next;
        free(skb->head);
        free(skb);
        skb = next;
    }
    list->first = list->last = NULL;
    list->qlen = 0;
    pthread_mutex_unlock(&list->lock);
    pthread_mutex_destroy(&list->lock);
}

// Create new socket buffer
static struct sk_buff *alloc_skb(size_t size) {
    struct sk_buff *skb = safe_malloc(sizeof(*skb));
    memset(skb, 0, sizeof(*skb));
    
    skb->head = safe_malloc(size);
    skb->data = skb->head;
    skb->len = 0;
    skb->data_len = size;
    atomic_init(&skb->users, 1);
    skb->next = skb->prev = NULL;
    
    return skb;
}

// Queue socket buffer to tail
static int sk_buff_queue_tail(struct sk_buff_head *list, struct sk_buff *skb) {
    pthread_mutex_lock(&list->lock);
    
    if (list->qlen >= MAX_QUEUE_SIZE) {
        pthread_mutex_unlock(&list->lock);
        return -ENOBUFS;
    }
    
    skb->next = NULL;
    skb->prev = list->last;
    
    if (list->last)
        list->last->next = skb;
    else
        list->first = skb;
    
    list->last = skb;
    list->qlen++;
    
    pthread_mutex_unlock(&list->lock);
    return 0;
}

// Dequeue socket buffer
static struct sk_buff *sk_buff_dequeue(struct sk_buff_head *list) {
    pthread_mutex_lock(&list->lock);
    
    struct sk_buff *skb = list->first;
    if (skb) {
        list->first = skb->next;
        if (skb->next)
            skb->next->prev = NULL;
        else
            list->last = NULL;
        
        list->qlen--;
        skb->next = skb->prev = NULL;
    }
    
    pthread_mutex_unlock(&list->lock);
    return skb;
}

// BPF program management
static int register_bpf_prog(struct bpf_prog *prog) {
    pthread_mutex_lock(&g_prog_mutex);
    
    if (g_prog_count >= MAX_BPF_PROGRAMS) {
        pthread_mutex_unlock(&g_prog_mutex);
        return -ENOMEM;
    }
    
    g_prog_array[g_prog_count++] = prog;
    atomic_fetch_add(&prog->refcount, 1);
    
    pthread_mutex_unlock(&g_prog_mutex);
    return 0;
}

static void unregister_bpf_prog(struct bpf_prog *prog) {
    pthread_mutex_lock(&g_prog_mutex);
    
    for (int i = 0; i < g_prog_count; i++) {
        if (g_prog_array[i] == prog) {
            memmove(&g_prog_array[i], &g_prog_array[i + 1],
                   (g_prog_count - i - 1) * sizeof(struct bpf_prog *));
            g_prog_count--;
            break;
        }
    }
    
    if (atomic_fetch_sub(&prog->refcount, 1) == 1) {
        free(prog->private_data);
        free(prog);
    }
    
    pthread_mutex_unlock(&g_prog_mutex);
}

// Socket operations
static struct sock *create_socket(void) {
    struct sock *sk = safe_malloc(sizeof(*sk));
    memset(sk, 0, sizeof(*sk));
    
    sk->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sk->fd < 0) {
        free(sk);
        return NULL;
    }
    
    sk_buff_head_init(&sk->rx_queue);
    sk_buff_head_init(&sk->tx_queue);
    pthread_mutex_init(&sk->lock, NULL);
    atomic_init(&sk->closed, false);
    
    return sk;
}

static void destroy_socket(struct sock *sk) {
    if (!sk)
        return;
    
    atomic_store(&sk->closed, true);
    
    pthread_mutex_lock(&sk->lock);
    
    close(sk->fd);
    sk_buff_head_destroy(&sk->rx_queue);
    sk_buff_head_destroy(&sk->tx_queue);
    
    for (int i = 0; i < BPF_PROG_TYPE_MAX; i++) {
        if (sk->progs[i])
            unregister_bpf_prog(sk->progs[i]);
    }
    
    pthread_mutex_unlock(&sk->lock);
    pthread_mutex_destroy(&sk->lock);
    
    free(sk);
}

// TCP operations simulation
static int tcp_v4_connect(struct sock *sk, struct sockaddr_in *addr) {
    int ret;
    
    pthread_mutex_lock(&sk->lock);
    ret = connect(sk->fd, (struct sockaddr *)addr, sizeof(*addr));
    pthread_mutex_unlock(&sk->lock);
    
    return ret;
}

static int tcp_v4_accept(struct sock *sk, struct sock **new_sk) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    int new_fd;
    
    pthread_mutex_lock(&sk->lock);
    new_fd = accept(sk->fd, (struct sockaddr *)&addr, &addr_len);
    pthread_mutex_unlock(&sk->lock);
    
    if (new_fd < 0)
        return -errno;
    
    *new_sk = create_socket();
    if (!*new_sk) {
        close(new_fd);
        return -ENOMEM;
    }
    
    (*new_sk)->fd = new_fd;
    (*new_sk)->addr = addr;
    
    atomic_fetch_add(&g_active_connections, 1);
    return 0;
}

// BPF hook simulation
static int tcp_bpf_hook(struct sock *sk, struct sk_buff *skb,
                       enum bpf_prog_type type) {
    struct bpf_prog *prog;
    int ret = 0;
    
    pthread_mutex_lock(&sk->lock);
    prog = sk->progs[type];
    if (prog) {
        ret = prog->filter(sk, skb->data, skb->len);
    }
    pthread_mutex_unlock(&sk->lock);
    
    return ret;
}

// Data transmission simulation
static int tcp_sendmsg_locked(struct sock *sk, const void *buf, size_t len) {
    struct sk_buff *skb;
    int ret;
    
    skb = alloc_skb(len);
    if (!skb)
        return -ENOMEM;
    
    memcpy(skb->data, buf, len);
    skb->len = len;
    
    ret = tcp_bpf_hook(sk, skb, BPF_PROG_TYPE_TCP_DATA);
    if (ret < 0) {
        free(skb->head);
        free(skb);
        return ret;
    }
    
    ret = send(sk->fd, skb->data, skb->len, 0);
    
    free(skb->head);
    free(skb);
    
    return ret;
}

static int tcp_recvmsg_locked(struct sock *sk, void *buf, size_t len) {
    struct sk_buff *skb;
    int ret;
    
    skb = alloc_skb(len);
    if (!skb)
        return -ENOMEM;
    
    ret = recv(sk->fd, skb->data, len, 0);
    if (ret > 0) {
        skb->len = ret;
        ret = tcp_bpf_hook(sk, skb, BPF_PROG_TYPE_TCP_DATA);
        if (ret >= 0) {
            memcpy(buf, skb->data, skb->len);
            ret = skb->len;
        }
    }
    
    free(skb->head);
    free(skb);
    
    return ret;
}

// Example BPF program
static int example_filter(void *ctx, void *data, size_t len) {
    // Simple packet inspection
    if (len < 1)
        return -EINVAL;
    
    // Allow packets starting with 'A'
    if (((char *)data)[0] == 'A')
        return len;
    
    return -EPERM;
}

// Server implementation
static void *server_worker(void *arg) {
    struct sock *listen_sk = (struct sock *)arg;
    struct sock *client_sk;
    char buf[MAX_MSG_SIZE];
    int ret;
    
    while (!atomic_load(&listen_sk->closed)) {
        ret = tcp_v4_accept(listen_sk, &client_sk);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        
        printf("New client connected\n");
        
        while (!atomic_load(&client_sk->closed)) {
            ret = tcp_recvmsg_locked(client_sk, buf, sizeof(buf));
            if (ret <= 0)
                break;
            
            printf("Received %d bytes: %.*s\n", ret, ret, buf);
            
            // Echo back
            ret = tcp_sendmsg_locked(client_sk, buf, ret);
            if (ret < 0)
                break;
        }
        
        printf("Client disconnected\n");
        destroy_socket(client_sk);
        atomic_fetch_sub(&g_active_connections, 1);
    }
    
    return NULL;
}

// Main function demonstrating usage
int main(int argc, char **argv) {
    struct sock *listen_sk;
    struct bpf_prog *prog;
    struct sockaddr_in addr;
    pthread_t worker_thread;
    int ret;
    
    // Create listening socket
    listen_sk = create_socket();
    if (!listen_sk) {
        fprintf(stderr, "Failed to create socket\n");
        return 1;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(listen_sk->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind and listen
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DEFAULT_PORT);
    
    ret = bind(listen_sk->fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        perror("bind failed");
        destroy_socket(listen_sk);
        return 1;
    }
    
    ret = listen(listen_sk->fd, BACKLOG);
    if (ret < 0) {
        perror("listen failed");
        destroy_socket(listen_sk);
        return 1;
    }
    
    // Create and register BPF program
    prog = safe_malloc(sizeof(*prog));
    memset(prog, 0, sizeof(*prog));
    prog->type = BPF_PROG_TYPE_TCP_DATA;
    prog->filter = example_filter;
    strncpy(prog->name, "example_filter", sizeof(prog->name) - 1);
    atomic_init(&prog->refcount, 1);
    
    ret = register_bpf_prog(prog);
    if (ret < 0) {
        fprintf(stderr, "Failed to register BPF program\n");
        free(prog);
        destroy_socket(listen_sk);
        return 1;
    }
    
    // Attach BPF program to socket
    listen_sk->progs[BPF_PROG_TYPE_TCP_DATA] = prog;
    
    printf("TCP BPF simulator listening on port %d...\n", DEFAULT_PORT);
    
    // Create worker thread
    ret = pthread_create(&worker_thread, NULL, server_worker, listen_sk);
    if (ret < 0) {
        fprintf(stderr, "Failed to create worker thread\n");
        destroy_socket(listen_sk);
        return 1;
    }
    
    // Wait for input to quit
    printf("Press Enter to quit...\n");
    getchar();
    
    // Cleanup
    atomic_store(&listen_sk->closed, true);
    pthread_join(worker_thread, NULL);
    destroy_socket(listen_sk);
    
    printf("Server shutdown complete\n");
    return 0;
}
