#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Logging Macros
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

#define LOG(level, ...) do { \
    if (level >= current_log_level) { \
        printf("[%s] ", get_log_level_string(level)); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while(0)

// Global Log Level
static int current_log_level = LOG_LEVEL_INFO;

// Constants
#define MAX_SOCKETS      1024
#define MAX_PROTOCOLS    32
#define MAX_PORTS       65535
#define MIN_PORT        1024
#define MAX_BACKLOG     128
#define TEST_DURATION   30

// Socket States
typedef enum {
    SOCK_CLOSED,
    SOCK_LISTEN,
    SOCK_CONNECTING,
    SOCK_CONNECTED,
    SOCK_CLOSING
} socket_state_t;

// Protocol Types
typedef enum {
    PROTO_TCP,
    PROTO_UDP,
    PROTO_RAW,
    PROTO_MAX
} protocol_type_t;

// Socket Options
typedef struct {
    bool reuse_addr;
    bool reuse_port;
    bool keep_alive;
    bool no_delay;
    int send_buffer;
    int recv_buffer;
    int ttl;
    int tos;
} socket_options_t;

// Socket Structure
typedef struct {
    int id;
    protocol_type_t protocol;
    socket_state_t state;
    struct sockaddr_in local_addr;
    struct sockaddr_in remote_addr;
    socket_options_t options;
    void *send_queue;
    void *recv_queue;
    size_t send_queue_size;
    size_t recv_queue_size;
    pthread_mutex_t lock;
} inet_socket_t;

// Protocol Operations
typedef struct {
    int (*create)(inet_socket_t *sock);
    int (*release)(inet_socket_t *sock);
    int (*bind)(inet_socket_t *sock, struct sockaddr_in *addr);
    int (*connect)(inet_socket_t *sock, struct sockaddr_in *addr);
    int (*listen)(inet_socket_t *sock, int backlog);
    int (*accept)(inet_socket_t *sock, inet_socket_t *newsock);
    int (*send)(inet_socket_t *sock, const void *buf, size_t len);
    int (*recv)(inet_socket_t *sock, void *buf, size_t len);
    int (*close)(inet_socket_t *sock);
} protocol_ops_t;

// Statistics Structure
typedef struct {
    uint64_t total_sockets;
    uint64_t active_sockets;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t connections;
    uint64_t failed_connections;
    uint64_t resets;
    uint64_t timeouts;
} inet_stats_t;

// AF_INET Manager Structure
typedef struct {
    inet_socket_t *sockets[MAX_SOCKETS];
    protocol_ops_t *protocols[MAX_PROTOCOLS];
    size_t nr_sockets;
    size_t nr_protocols;
    bool running;
    pthread_mutex_t manager_lock;
    pthread_t accept_thread;
    pthread_t timeout_thread;
    inet_stats_t stats;
} inet_manager_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_protocol_string(protocol_type_t proto);
const char* get_socket_state_string(socket_state_t state);

inet_manager_t* create_inet_manager(void);
void destroy_inet_manager(inet_manager_t *manager);

inet_socket_t* create_socket(protocol_type_t proto);
void destroy_socket(inet_socket_t *sock);

int register_protocol(inet_manager_t *manager, protocol_type_t proto, protocol_ops_t *ops);
int unregister_protocol(inet_manager_t *manager, protocol_type_t proto);

// TCP Protocol Operations
int tcp_create(inet_socket_t *sock);
int tcp_release(inet_socket_t *sock);
int tcp_bind(inet_socket_t *sock, struct sockaddr_in *addr);
int tcp_connect(inet_socket_t *sock, struct sockaddr_in *addr);
int tcp_listen(inet_socket_t *sock, int backlog);
int tcp_accept(inet_socket_t *sock, inet_socket_t *newsock);
int tcp_send(inet_socket_t *sock, const void *buf, size_t len);
int tcp_recv(inet_socket_t *sock, void *buf, size_t len);
int tcp_close(inet_socket_t *sock);

// UDP Protocol Operations
int udp_create(inet_socket_t *sock);
int udp_release(inet_socket_t *sock);
int udp_bind(inet_socket_t *sock, struct sockaddr_in *addr);
int udp_connect(inet_socket_t *sock, struct sockaddr_in *addr);
int udp_send(inet_socket_t *sock, const void *buf, size_t len);
int udp_recv(inet_socket_t *sock, void *buf, size_t len);
int udp_close(inet_socket_t *sock);

void* accept_thread(void *arg);
void* timeout_thread(void *arg);
void process_timeouts(inet_manager_t *manager);

void run_test(inet_manager_t *manager);
void calculate_stats(inet_manager_t *manager);
void print_test_stats(inet_manager_t *manager);
void demonstrate_af_inet(void);

// Utility Functions
const char* get_log_level_string(int level) {
    switch(level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

const char* get_protocol_string(protocol_type_t proto) {
    switch(proto) {
        case PROTO_TCP: return "TCP";
        case PROTO_UDP: return "UDP";
        case PROTO_RAW: return "RAW";
        default: return "UNKNOWN";
    }
}

const char* get_socket_state_string(socket_state_t state) {
    switch(state) {
        case SOCK_CLOSED:     return "CLOSED";
        case SOCK_LISTEN:     return "LISTEN";
        case SOCK_CONNECTING: return "CONNECTING";
        case SOCK_CONNECTED:  return "CONNECTED";
        case SOCK_CLOSING:    return "CLOSING";
        default: return "UNKNOWN";
    }
}

// Create Socket
inet_socket_t* create_socket(protocol_type_t proto) {
    static int next_id = 0;
    inet_socket_t *sock = malloc(sizeof(inet_socket_t));
    if (!sock) return NULL;

    sock->id = next_id++;
    sock->protocol = proto;
    sock->state = SOCK_CLOSED;
    memset(&sock->local_addr, 0, sizeof(struct sockaddr_in));
    memset(&sock->remote_addr, 0, sizeof(struct sockaddr_in));
    
    // Default options
    sock->options.reuse_addr = false;
    sock->options.reuse_port = false;
    sock->options.keep_alive = false;
    sock->options.no_delay = false;
    sock->options.send_buffer = 8192;
    sock->options.recv_buffer = 8192;
    sock->options.ttl = 64;
    sock->options.tos = 0;

    sock->send_queue = NULL;
    sock->recv_queue = NULL;
    sock->send_queue_size = 0;
    sock->recv_queue_size = 0;
    pthread_mutex_init(&sock->lock, NULL);

    return sock;
}

// Create INET Manager
inet_manager_t* create_inet_manager(void) {
    inet_manager_t *manager = malloc(sizeof(inet_manager_t));
    if (!manager) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate inet manager");
        return NULL;
    }

    memset(manager->sockets, 0, sizeof(manager->sockets));
    memset(manager->protocols, 0, sizeof(manager->protocols));
    manager->nr_sockets = 0;
    manager->nr_protocols = 0;
    manager->running = false;
    pthread_mutex_init(&manager->manager_lock, NULL);
    memset(&manager->stats, 0, sizeof(inet_stats_t));

    // Register protocols
    protocol_ops_t tcp_ops = {
        .create = tcp_create,
        .release = tcp_release,
        .bind = tcp_bind,
        .connect = tcp_connect,
        .listen = tcp_listen,
        .accept = tcp_accept,
        .send = tcp_send,
        .recv = tcp_recv,
        .close = tcp_close
    };
    register_protocol(manager, PROTO_TCP, &tcp_ops);

    protocol_ops_t udp_ops = {
        .create = udp_create,
        .release = udp_release,
        .bind = udp_bind,
        .connect = udp_connect,
        .send = udp_send,
        .recv = udp_recv,
        .close = udp_close
    };
    register_protocol(manager, PROTO_UDP, &udp_ops);

    LOG(LOG_LEVEL_DEBUG, "Created inet manager");
    return manager;
}

// Register Protocol
int register_protocol(inet_manager_t *manager, protocol_type_t proto, protocol_ops_t *ops) {
    if (!manager || !ops || proto >= PROTO_MAX) return -1;

    pthread_mutex_lock(&manager->manager_lock);

    if (manager->protocols[proto]) {
        pthread_mutex_unlock(&manager->manager_lock);
        return -1;
    }

    protocol_ops_t *new_ops = malloc(sizeof(protocol_ops_t));
    if (!new_ops) {
        pthread_mutex_unlock(&manager->manager_lock);
        return -1;
    }

    memcpy(new_ops, ops, sizeof(protocol_ops_t));
    manager->protocols[proto] = new_ops;
    manager->nr_protocols++;

    pthread_mutex_unlock(&manager->manager_lock);
    LOG(LOG_LEVEL_DEBUG, "Registered protocol %s", get_protocol_string(proto));
    return 0;
}

// TCP Protocol Operations
int tcp_create(inet_socket_t *sock) {
    if (!sock) return -1;
    sock->state = SOCK_CLOSED;
    return 0;
}

int tcp_release(inet_socket_t *sock) {
    if (!sock) return -1;
    // Clean up resources
    return 0;
}

int tcp_bind(inet_socket_t *sock, struct sockaddr_in *addr) {
    if (!sock || !addr) return -1;
    memcpy(&sock->local_addr, addr, sizeof(struct sockaddr_in));
    return 0;
}

int tcp_connect(inet_socket_t *sock, struct sockaddr_in *addr) {
    if (!sock || !addr) return -1;
    sock->state = SOCK_CONNECTING;
    memcpy(&sock->remote_addr, addr, sizeof(struct sockaddr_in));
    // Simulate connection establishment
    sock->state = SOCK_CONNECTED;
    return 0;
}

int tcp_listen(inet_socket_t *sock, int backlog) {
    if (!sock || backlog <= 0) return -1;
    sock->state = SOCK_LISTEN;
    return 0;
}

int tcp_accept(inet_socket_t *sock, inet_socket_t *newsock) {
    if (!sock || !newsock) return -1;
    // Accept new connection
    newsock->state = SOCK_CONNECTED;
    return 0;
}

int tcp_send(inet_socket_t *sock, const void *buf, size_t len) {
    if (!sock || !buf || !len) return -1;
    // Simulate data transmission
    sock->send_queue_size += len;
    return len;
}

int tcp_recv(inet_socket_t *sock, void *buf, size_t len) {
    if (!sock || !buf || !len) return -1;
    // Simulate data reception
    sock->recv_queue_size += len;
    return len;
}

int tcp_close(inet_socket_t *sock) {
    if (!sock) return -1;
    sock->state = SOCK_CLOSING;
    // Clean up connection
    sock->state = SOCK_CLOSED;
    return 0;
}

// UDP Protocol Operations
int udp_create(inet_socket_t *sock) {
    if (!sock) return -1;
    sock->state = SOCK_CLOSED;
    return 0;
}

int udp_release(inet_socket_t *sock) {
    if (!sock) return -1;
    // Clean up resources
    return 0;
}

int udp_bind(inet_socket_t *sock, struct sockaddr_in *addr) {
    if (!sock || !addr) return -1;
    memcpy(&sock->local_addr, addr, sizeof(struct sockaddr_in));
    return 0;
}

int udp_connect(inet_socket_t *sock, struct sockaddr_in *addr) {
    if (!sock || !addr) return -1;
    memcpy(&sock->remote_addr, addr, sizeof(struct sockaddr_in));
    sock->state = SOCK_CONNECTED;
    return 0;
}

int udp_send(inet_socket_t *sock, const void *buf, size_t len) {
    if (!sock || !buf || !len) return -1;
    // Simulate datagram transmission
    sock->send_queue_size += len;
    return len;
}

int udp_recv(inet_socket_t *sock, void *buf, size_t len) {
    if (!sock || !buf || !len) return -1;
    // Simulate datagram reception
    sock->recv_queue_size += len;
    return len;
}

int udp_close(inet_socket_t *sock) {
    if (!sock) return -1;
    sock->state = SOCK_CLOSED;
    return 0;
}

// Accept Thread
void* accept_thread(void *arg) {
    inet_manager_t *manager = (inet_manager_t*)arg;

    while (manager->running) {
        // Process listening sockets
        pthread_mutex_lock(&manager->manager_lock);
        
        for (size_t i = 0; i < manager->nr_sockets; i++) {
            inet_socket_t *sock = manager->sockets[i];
            if (sock && sock->state == SOCK_LISTEN) {
                // Simulate incoming connection
                if (rand() % 100 < 10) {  // 10% chance of new connection
                    inet_socket_t *newsock = create_socket(sock->protocol);
                    if (newsock) {
                        if (manager->protocols[sock->protocol]->accept(sock, newsock) == 0) {
                            if (manager->nr_sockets < MAX_SOCKETS) {
                                manager->sockets[manager->nr_sockets++] = newsock;
                                manager->stats.connections++;
                            } else {
                                destroy_socket(newsock);
                                manager->stats.failed_connections++;
                            }
                        } else {
                            destroy_socket(newsock);
                            manager->stats.failed_connections++;
                        }
                    }
                }
            }
        }

        pthread_mutex_unlock(&manager->manager_lock);
        usleep(100000);  // 100ms interval
    }

    return NULL;
}

// Timeout Thread
void* timeout_thread(void *arg) {
    inet_manager_t *manager = (inet_manager_t*)arg;

    while (manager->running) {
        process_timeouts(manager);
        usleep(1000000);  // 1s interval
    }

    return NULL;
}

// Process Timeouts
void process_timeouts(inet_manager_t *manager) {
    if (!manager) return;

    pthread_mutex_lock(&manager->manager_lock);

    for (size_t i = 0; i < manager->nr_sockets; i++) {
        inet_socket_t *sock = manager->sockets[i];
        if (sock && sock->state == SOCK_CONNECTING) {
            // Simulate connection timeout
            if (rand() % 100 < 5) {  // 5% chance of timeout
                sock->state = SOCK_CLOSED;
                manager->stats.timeouts++;
            }
        }
    }

    pthread_mutex_unlock(&manager->manager_lock);
}

// Run Test
void run_test(inet_manager_t *manager) {
    if (!manager) return;

    LOG(LOG_LEVEL_INFO, "Starting AF_INET test...");

    // Create test sockets
    for (size_t i = 0; i < 10; i++) {
        protocol_type_t proto = (rand() % 2) ? PROTO_TCP : PROTO_UDP;
        inet_socket_t *sock = create_socket(proto);
        if (sock) {
            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(MIN_PORT + rand() % (MAX_PORTS - MIN_PORT));

            if (manager->protocols[proto]->bind(sock, &addr) == 0) {
                if (proto == PROTO_TCP) {
                    manager->protocols[proto]->listen(sock, MAX_BACKLOG);
                }
                if (manager->nr_sockets < MAX_SOCKETS) {
                    manager->sockets[manager->nr_sockets++] = sock;
                    manager->stats.total_sockets++;
                    manager->stats.active_sockets++;
                } else {
                    destroy_socket(sock);
                }
            } else {
                destroy_socket(sock);
            }
        }
    }

    // Start threads
    manager->running = true;
    pthread_create(&manager->accept_thread, NULL, accept_thread, manager);
    pthread_create(&manager->timeout_thread, NULL, timeout_thread, manager);

    // Run test
    sleep(TEST_DURATION);

    // Stop threads
    manager->running = false;
    pthread_join(manager->accept_thread, NULL);
    pthread_join(manager->timeout_thread, NULL);

    // Calculate statistics
    calculate_stats(manager);
}

// Calculate Statistics
void calculate_stats(inet_manager_t *manager) {
    if (!manager) return;

    // Update socket statistics
    size_t active = 0;
    for (size_t i = 0; i < manager->nr_sockets; i++) {
        inet_socket_t *sock = manager->sockets[i];
        if (sock) {
            if (sock->state != SOCK_CLOSED) {
                active++;
            }
            manager->stats.bytes_sent += sock->send_queue_size;
            manager->stats.bytes_received += sock->recv_queue_size;
        }
    }
    manager->stats.active_sockets = active;
}

// Print Test Statistics
void print_test_stats(inet_manager_t *manager) {
    if (!manager) return;

    printf("\nAF_INET Test Results:\n");
    printf("-------------------------\n");
    printf("Test Duration:     %d seconds\n", TEST_DURATION);
    printf("Total Sockets:     %lu\n", manager->stats.total_sockets);
    printf("Active Sockets:    %lu\n", manager->stats.active_sockets);
    printf("Bytes Sent:        %lu\n", manager->stats.bytes_sent);
    printf("Bytes Received:    %lu\n", manager->stats.bytes_received);
    printf("Connections:       %lu\n", manager->stats.connections);
    printf("Failed Connects:   %lu\n", manager->stats.failed_connections);
    printf("Resets:           %lu\n", manager->stats.resets);
    printf("Timeouts:         %lu\n", manager->stats.timeouts);

    // Print socket details
    printf("\nSocket Details:\n");
    for (size_t i = 0; i < manager->nr_sockets; i++) {
        inet_socket_t *sock = manager->sockets[i];
        if (sock) {
            printf("  Socket %d:\n", sock->id);
            printf("    Protocol:    %s\n", get_protocol_string(sock->protocol));
            printf("    State:       %s\n", get_socket_state_string(sock->state));
            printf("    Local Port:  %d\n", ntohs(sock->local_addr.sin_port));
            if (sock->state == SOCK_CONNECTED) {
                printf("    Remote Port: %d\n", ntohs(sock->remote_addr.sin_port));
            }
        }
    }
}

// Destroy Socket
void destroy_socket(inet_socket_t *sock) {
    if (!sock) return;
    pthread_mutex_destroy(&sock->lock);
    free(sock);
}

// Destroy INET Manager
void destroy_inet_manager(inet_manager_t *manager) {
    if (!manager) return;

    // Clean up sockets
    for (size_t i = 0; i < manager->nr_sockets; i++) {
        if (manager->sockets[i]) {
            destroy_socket(manager->sockets[i]);
        }
    }

    // Clean up protocols
    for (size_t i = 0; i < PROTO_MAX; i++) {
        free(manager->protocols[i]);
    }

    pthread_mutex_destroy(&manager->manager_lock);
    free(manager);
    LOG(LOG_LEVEL_DEBUG, "Destroyed inet manager");
}

// Demonstrate AF_INET
void demonstrate_af_inet(void) {
    printf("Starting AF_INET demonstration...\n");

    // Create and run AF_INET test
    inet_manager_t *manager = create_inet_manager();
    if (manager) {
        run_test(manager);
        print_test_stats(manager);
        destroy_inet_manager(manager);
    }
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Seed random number generator
    srand(time(NULL));

    // Run demonstration
    demonstrate_af_inet();

    return 0;
}
