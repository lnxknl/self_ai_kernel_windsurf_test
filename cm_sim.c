#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

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

// Connection States
typedef enum {
    CM_STATE_IDLE,
    CM_STATE_LISTEN,
    CM_STATE_REQ_SENT,
    CM_STATE_REQ_RCVD,
    CM_STATE_ESTABLISHED,
    CM_STATE_DISCONNECTING,
    CM_STATE_ERROR,
    CM_STATE_CLOSED
} cm_connection_state_t;

// Connection Types
typedef enum {
    CM_CONN_TYPE_RELIABLE,
    CM_CONN_TYPE_UNRELIABLE,
    CM_CONN_TYPE_DATAGRAM,
    CM_CONN_TYPE_MULTICAST
} cm_connection_type_t;

// QoS (Quality of Service) Levels
typedef enum {
    QOS_BEST_EFFORT,
    QOS_GUARANTEED,
    QOS_LOW_LATENCY,
    QOS_HIGH_THROUGHPUT
} cm_qos_level_t;

// Connection Endpoint Addressing
typedef struct {
    char ip_address[46];  // IPv6 max length
    uint16_t port;
    uint64_t node_id;
} cm_endpoint_t;

// Connection Parameters
typedef struct {
    cm_connection_type_t type;
    cm_qos_level_t qos_level;
    size_t max_message_size;
    size_t receive_queue_depth;
    size_t send_queue_depth;
} cm_connection_params_t;

// Connection Event Types
typedef enum {
    CM_EVENT_CONNECT_REQUEST,
    CM_EVENT_CONNECT_RESPONSE,
    CM_EVENT_CONNECT_ESTABLISHED,
    CM_EVENT_DISCONNECT_REQUEST,
    CM_EVENT_DISCONNECT_RESPONSE,
    CM_EVENT_ERROR
} cm_event_type_t;

// Forward Declarations
struct cm_connection;
struct cm_event;

// Connection Event Callback
typedef void (*cm_event_callback_fn)(
    struct cm_connection *conn, 
    struct cm_event *event
);

// Connection Event Structure
typedef struct cm_event {
    cm_event_type_t type;
    void *data;
    size_t data_length;
    cm_event_callback_fn callback;
    struct cm_event *next;
} cm_event_t;

// Connection Statistics
typedef struct {
    unsigned long packets_sent;
    unsigned long packets_received;
    unsigned long bytes_sent;
    unsigned long bytes_received;
    unsigned long retransmissions;
    unsigned long timeouts;
} cm_connection_stats_t;

// Connection Management Structure
typedef struct cm_connection {
    uint64_t connection_id;
    cm_connection_state_t state;
    cm_endpoint_t local_endpoint;
    cm_endpoint_t remote_endpoint;
    cm_connection_params_t params;
    
    // Event Management
    cm_event_t *event_queue;
    size_t event_queue_depth;
    
    // Callbacks
    cm_event_callback_fn state_change_callback;
    cm_event_callback_fn error_callback;
    
    // Statistics
    cm_connection_stats_t stats;
    
    // Timeout and Retry Management
    time_t last_activity;
    int retry_count;
    
    // Linked List Management
    struct cm_connection *next;
} cm_connection_t;

// Connection Management System
typedef struct {
    cm_connection_t *connections;
    size_t max_connections;
    size_t current_connections;
    
    // Global Statistics
    unsigned long total_connections_established;
    unsigned long total_connection_failures;
} cm_system_t;

// Function Prototypes
const char* get_log_level_string(int level);
const char* get_connection_state_string(cm_connection_state_t state);
const char* get_connection_type_string(cm_connection_type_t type);
const char* get_qos_level_string(cm_qos_level_t level);

cm_system_t* create_cm_system(size_t max_connections);
void destroy_cm_system(cm_system_t *system);

cm_connection_t* create_connection(
    cm_system_t *system,
    cm_endpoint_t *local_endpoint,
    cm_connection_params_t *params
);

bool connect_to_endpoint(
    cm_connection_t *connection, 
    cm_endpoint_t *remote_endpoint
);

bool listen_for_connections(
    cm_connection_t *connection
);

bool accept_connection(
    cm_connection_t *listening_conn, 
    cm_connection_t *new_conn
);

bool disconnect_connection(
    cm_connection_t *connection
);

void enqueue_event(
    cm_connection_t *connection, 
    cm_event_type_t type, 
    void *data, 
    size_t data_length
);

void process_connection_events(
    cm_connection_t *connection
);

void print_connection_stats(
    cm_connection_t *connection
);

void demonstrate_connection_management();

// Utility Function: Get Log Level String
const char* get_log_level_string(int level) {
    switch(level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Connection State String
const char* get_connection_state_string(cm_connection_state_t state) {
    switch(state) {
        case CM_STATE_IDLE:           return "IDLE";
        case CM_STATE_LISTEN:         return "LISTEN";
        case CM_STATE_REQ_SENT:       return "REQUEST_SENT";
        case CM_STATE_REQ_RCVD:       return "REQUEST_RECEIVED";
        case CM_STATE_ESTABLISHED:    return "ESTABLISHED";
        case CM_STATE_DISCONNECTING:  return "DISCONNECTING";
        case CM_STATE_ERROR:          return "ERROR";
        case CM_STATE_CLOSED:         return "CLOSED";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get Connection Type String
const char* get_connection_type_string(cm_connection_type_t type) {
    switch(type) {
        case CM_CONN_TYPE_RELIABLE:     return "RELIABLE";
        case CM_CONN_TYPE_UNRELIABLE:   return "UNRELIABLE";
        case CM_CONN_TYPE_DATAGRAM:     return "DATAGRAM";
        case CM_CONN_TYPE_MULTICAST:    return "MULTICAST";
        default: return "UNKNOWN";
    }
}

// Utility Function: Get QoS Level String
const char* get_qos_level_string(cm_qos_level_t level) {
    switch(level) {
        case QOS_BEST_EFFORT:       return "BEST_EFFORT";
        case QOS_GUARANTEED:        return "GUARANTEED";
        case QOS_LOW_LATENCY:       return "LOW_LATENCY";
        case QOS_HIGH_THROUGHPUT:   return "HIGH_THROUGHPUT";
        default: return "UNKNOWN";
    }
}

// Create Connection Management System
cm_system_t* create_cm_system(size_t max_connections) {
    cm_system_t *system = malloc(sizeof(cm_system_t));
    if (!system) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate CM system");
        return NULL;
    }

    system->connections = NULL;
    system->max_connections = max_connections;
    system->current_connections = 0;
    system->total_connections_established = 0;
    system->total_connection_failures = 0;

    return system;
}

// Create Connection
cm_connection_t* create_connection(
    cm_system_t *system,
    cm_endpoint_t *local_endpoint,
    cm_connection_params_t *params
) {
    if (!system || system->current_connections >= system->max_connections) {
        LOG(LOG_LEVEL_ERROR, "Cannot create connection: system limit reached");
        return NULL;
    }

    cm_connection_t *connection = malloc(sizeof(cm_connection_t));
    if (!connection) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate connection");
        return NULL;
    }

    // Initialize connection
    connection->connection_id = (uint64_t)connection;
    connection->state = CM_STATE_IDLE;
    
    // Copy local endpoint
    if (local_endpoint) {
        memcpy(&connection->local_endpoint, local_endpoint, sizeof(cm_endpoint_t));
    } else {
        memset(&connection->local_endpoint, 0, sizeof(cm_endpoint_t));
    }
    
    // Copy connection parameters
    if (params) {
        memcpy(&connection->params, params, sizeof(cm_connection_params_t));
    } else {
        // Default parameters
        connection->params.type = CM_CONN_TYPE_RELIABLE;
        connection->params.qos_level = QOS_BEST_EFFORT;
        connection->params.max_message_size = 65536;
        connection->params.receive_queue_depth = 256;
        connection->params.send_queue_depth = 256;
    }

    // Initialize event queue
    connection->event_queue = NULL;
    connection->event_queue_depth = 0;

    // Reset callbacks
    connection->state_change_callback = NULL;
    connection->error_callback = NULL;

    // Reset statistics
    memset(&connection->stats, 0, sizeof(cm_connection_stats_t));

    // Reset timeout and retry
    connection->last_activity = time(NULL);
    connection->retry_count = 0;

    // Link to system
    connection->next = system->connections;
    system->connections = connection;
    system->current_connections++;

    LOG(LOG_LEVEL_DEBUG, "Created connection: ID %lu, Type: %s", 
        connection->connection_id, 
        get_connection_type_string(connection->params.type));

    return connection;
}

// Connect to Remote Endpoint
bool connect_to_endpoint(
    cm_connection_t *connection, 
    cm_endpoint_t *remote_endpoint
) {
    if (!connection || !remote_endpoint) return false;

    // Check current state
    if (connection->state != CM_STATE_IDLE) {
        LOG(LOG_LEVEL_WARN, "Cannot connect: Invalid state");
        return false;
    }

    // Copy remote endpoint
    memcpy(&connection->remote_endpoint, remote_endpoint, sizeof(cm_endpoint_t));

    // Update connection state
    connection->state = CM_STATE_REQ_SENT;
    connection->last_activity = time(NULL);

    // Enqueue connection request event
    enqueue_event(
        connection, 
        CM_EVENT_CONNECT_REQUEST, 
        remote_endpoint, 
        sizeof(cm_endpoint_t)
    );

    LOG(LOG_LEVEL_INFO, "Connecting to %s:%d", 
        remote_endpoint->ip_address, remote_endpoint->port);

    return true;
}

// Listen for Incoming Connections
bool listen_for_connections(cm_connection_t *connection) {
    if (!connection) return false;

    // Check current state
    if (connection->state != CM_STATE_IDLE) {
        LOG(LOG_LEVEL_WARN, "Cannot listen: Invalid state");
        return false;
    }

    // Update connection state
    connection->state = CM_STATE_LISTEN;

    LOG(LOG_LEVEL_INFO, "Listening on %s:%d", 
        connection->local_endpoint.ip_address, 
        connection->local_endpoint.port);

    return true;
}

// Accept Incoming Connection
bool accept_connection(
    cm_connection_t *listening_conn, 
    cm_connection_t *new_conn
) {
    if (!listening_conn || !new_conn) return false;

    // Validate listening connection state
    if (listening_conn->state != CM_STATE_LISTEN) {
        LOG(LOG_LEVEL_WARN, "Cannot accept: Listening connection not in LISTEN state");
        return false;
    }

    // Validate new connection state
    if (new_conn->state != CM_STATE_REQ_RCVD) {
        LOG(LOG_LEVEL_WARN, "Cannot accept: New connection not in REQUEST_RECEIVED state");
        return false;
    }

    // Update connection states
    new_conn->state = CM_STATE_ESTABLISHED;
    listening_conn->last_activity = time(NULL);
    new_conn->last_activity = time(NULL);

    // Enqueue connection established event
    enqueue_event(
        new_conn, 
        CM_EVENT_CONNECT_ESTABLISHED, 
        &new_conn->remote_endpoint, 
        sizeof(cm_endpoint_t)
    );

    LOG(LOG_LEVEL_INFO, "Accepted connection from %s:%d", 
        new_conn->remote_endpoint.ip_address, 
        new_conn->remote_endpoint.port);

    return true;
}

// Disconnect Connection
bool disconnect_connection(cm_connection_t *connection) {
    if (!connection) return false;

    // Check current state
    if (connection->state != CM_STATE_ESTABLISHED) {
        LOG(LOG_LEVEL_WARN, "Cannot disconnect: Connection not established");
        return false;
    }

    // Update connection state
    connection->state = CM_STATE_DISCONNECTING;
    connection->last_activity = time(NULL);

    // Enqueue disconnect request event
    enqueue_event(
        connection, 
        CM_EVENT_DISCONNECT_REQUEST, 
        NULL, 
        0
    );

    LOG(LOG_LEVEL_INFO, "Disconnecting from %s:%d", 
        connection->remote_endpoint.ip_address, 
        connection->remote_endpoint.port);

    return true;
}

// Enqueue Connection Event
void enqueue_event(
    cm_connection_t *connection, 
    cm_event_type_t type, 
    void *data, 
    size_t data_length
) {
    if (!connection) return;

    cm_event_t *event = malloc(sizeof(cm_event_t));
    if (!event) {
        LOG(LOG_LEVEL_ERROR, "Failed to allocate event");
        return;
    }

    event->type = type;
    event->data = data ? malloc(data_length) : NULL;
    if (data && event->data) {
        memcpy(event->data, data, data_length);
    }
    event->data_length = data_length;
    event->callback = NULL;
    event->next = connection->event_queue;
    connection->event_queue = event;
    connection->event_queue_depth++;
}

// Process Connection Events
void process_connection_events(cm_connection_t *connection) {
    if (!connection) return;

    cm_event_t *event = connection->event_queue;
    while (event) {
        // Log event details
        LOG(LOG_LEVEL_DEBUG, "Processing event: %d", event->type);

        // Call event callback if set
        if (event->callback) {
            event->callback(connection, event);
        }

        // Move to next event
        cm_event_t *next = event->next;
        
        // Free event data
        free(event->data);
        free(event);
        
        event = next;
    }

    // Reset event queue
    connection->event_queue = NULL;
    connection->event_queue_depth = 0;
}

// Print Connection Statistics
void print_connection_stats(cm_connection_t *connection) {
    if (!connection) return;

    printf("\nConnection Statistics:\n");
    printf("---------------------\n");
    printf("Connection ID:      %lu\n", connection->connection_id);
    printf("State:              %s\n", get_connection_state_string(connection->state));
    printf("Type:               %s\n", get_connection_type_string(connection->params.type));
    printf("QoS Level:          %s\n", get_qos_level_string(connection->params.qos_level));
    printf("Packets Sent:       %lu\n", connection->stats.packets_sent);
    printf("Packets Received:   %lu\n", connection->stats.packets_received);
    printf("Bytes Sent:         %lu\n", connection->stats.bytes_sent);
    printf("Bytes Received:     %lu\n", connection->stats.bytes_received);
    printf("Retransmissions:    %lu\n", connection->stats.retransmissions);
    printf("Timeouts:           %lu\n", connection->stats.timeouts);
}

// Destroy Connection
void destroy_connection(cm_connection_t *connection, cm_system_t *system) {
    if (!connection || !system) return;

    // Remove from system's connection list
    cm_connection_t **current = &system->connections;
    while (*current) {
        if (*current == connection) {
            *current = connection->next;
            break;
        }
        current = &(*current)->next;
    }

    // Process any remaining events
    process_connection_events(connection);

    // Free connection
    free(connection);

    // Update system statistics
    system->current_connections--;
}

// Destroy Connection Management System
void destroy_cm_system(cm_system_t *system) {
    if (!system) return;

    // Destroy all connections
    while (system->connections) {
        destroy_connection(system->connections, system);
    }

    free(system);
}

// Demonstration Function
void demonstrate_connection_management() {
    // Create Connection Management System
    cm_system_t *cm_system = create_cm_system(10);

    // Create Local Endpoint
    cm_endpoint_t local_endpoint = {
        .ip_address = "192.168.1.100",
        .port = 8080,
        .node_id = 1
    };

    // Create Remote Endpoint
    cm_endpoint_t remote_endpoint = {
        .ip_address = "192.168.1.101",
        .port = 9090,
        .node_id = 2
    };

    // Connection Parameters
    cm_connection_params_t params = {
        .type = CM_CONN_TYPE_RELIABLE,
        .qos_level = QOS_GUARANTEED,
        .max_message_size = 65536,
        .receive_queue_depth = 256,
        .send_queue_depth = 256
    };

    // Create Listening Connection
    cm_connection_t *listener = create_connection(
        cm_system, 
        &local_endpoint, 
        &params
    );

    // Start Listening
    listen_for_connections(listener);

    // Create Client Connection
    cm_connection_t *client = create_connection(
        cm_system, 
        NULL,  // Use system-assigned local endpoint 
        &params
    );

    // Connect to Remote Endpoint
    connect_to_endpoint(client, &remote_endpoint);

    // Simulate Connection Acceptance
    client->state = CM_STATE_REQ_RCVD;
    accept_connection(listener, client);

    // Print Connection Statistics
    print_connection_stats(client);

    // Disconnect
    disconnect_connection(client);

    // Cleanup
    destroy_cm_system(cm_system);
}

int main(void) {
    // Set log level
    current_log_level = LOG_LEVEL_INFO;

    // Run demonstration
    demonstrate_connection_management();

    return 0;
}
