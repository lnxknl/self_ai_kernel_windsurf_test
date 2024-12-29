/*
 * POSIX Message Queue Simulation
 * 
 * This program simulates the POSIX message queue system based on Linux kernel's implementation.
 * It provides a user-space simulation of message queues with similar functionality to the kernel.
 *
 * Author: Cascade AI
 * Date: 2024-12-29
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/time.h>
#include <assert.h>

/* Configuration Constants */
#define MQUEUE_MAGIC     0x19800202
#define MAX_QUEUES       256
#define MAX_MSG_SIZE     8192
#define MAX_MSG_DEFAULT  10
#define MAX_MSGSIZE_DEFAULT 8192
#define MAX_PRIORITIES   32768
#define MQ_PRIO_MAX     32768

/* Message Queue Attributes */
struct mq_attr {
    long mq_flags;      /* Message queue flags */
    long mq_maxmsg;     /* Maximum number of messages */
    long mq_msgsize;    /* Maximum message size */
    long mq_curmsgs;    /* Number of messages currently queued */
};

/* Message Structure */
struct msg_msg {
    struct list_head list;
    int msg_prio;
    size_t msg_len;
    char *msg_data;
};

/* Message Tree Node */
struct msg_tree_node {
    struct rb_node rb_node;
    struct list_head msg_list;
    int priority;
};

/* List Management */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/* Red-Black Tree Node */
struct rb_node {
    unsigned long rb_parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
};

/* Red-Black Tree Root */
struct rb_root {
    struct rb_node *rb_node;
};

/* Waiting Task Structure */
struct wait_entry {
    pthread_t thread;
    struct list_head list;
    struct msg_msg *msg;
    int state;
    bool completed;
};

/* Message Queue Information */
struct mqueue_info {
    pthread_mutex_t lock;
    char *name;
    struct mq_attr attr;
    struct rb_root msg_tree;
    struct rb_node *msg_tree_rightmost;
    struct msg_tree_node *node_cache;
    
    /* Waiting Lists */
    struct list_head senders_list;   /* Tasks waiting to send */
    struct list_head receivers_list; /* Tasks waiting to receive */
    
    unsigned long qsize;    /* Current queue size in bytes */
    bool valid;            /* Whether queue is valid */
    
    /* Statistics */
    atomic_long_t sent_messages;
    atomic_long_t received_messages;
    time_t creation_time;
};

/* Global Variables */
static struct mqueue_info *queues[MAX_QUEUES];
static pthread_mutex_t queues_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int queue_count = ATOMIC_VAR_INIT(0);

/* List Management Functions */

static inline void list_init(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *new,
                            struct list_head *prev,
                            struct list_head *next) {
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head) {
    __list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head) {
    __list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next) {
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

/* Red-Black Tree Functions */

#define RB_RED      0
#define RB_BLACK    1

static inline void rb_set_parent(struct rb_node *rb, struct rb_node *p) {
    rb->rb_parent_color = (rb->rb_parent_color & 3) | (unsigned long)p;
}

static inline void rb_set_color(struct rb_node *rb, int color) {
    rb->rb_parent_color = (rb->rb_parent_color & ~1) | color;
}

static void rb_insert_color(struct rb_node *node, struct rb_root *root) {
    struct rb_node *parent, *gparent;

    while ((parent = (struct rb_node *)((node->rb_parent_color) & ~3)) &&
           (parent->rb_parent_color & 1) == 0) {
        gparent = (struct rb_node *)((parent->rb_parent_color) & ~3);

        if (parent == gparent->rb_left) {
            struct rb_node *uncle = gparent->rb_right;

            if (uncle && (uncle->rb_parent_color & 1) == 0) {
                uncle->rb_parent_color |= 1;
                parent->rb_parent_color |= 1;
                gparent->rb_parent_color &= ~1;
                node = gparent;
                continue;
            }

            if (parent->rb_right == node) {
                struct rb_node *tmp;
                node = parent;
                tmp = node->rb_right;
                node->rb_right = tmp->rb_left;
                tmp->rb_left = node;
                parent = tmp;
            }

            parent->rb_parent_color |= 1;
            gparent->rb_parent_color &= ~1;
        } else {
            struct rb_node *uncle = gparent->rb_left;

            if (uncle && (uncle->rb_parent_color & 1) == 0) {
                uncle->rb_parent_color |= 1;
                parent->rb_parent_color |= 1;
                gparent->rb_parent_color &= ~1;
                node = gparent;
                continue;
            }

            if (parent->rb_left == node) {
                struct rb_node *tmp;
                node = parent;
                tmp = node->rb_left;
                node->rb_left = tmp->rb_right;
                tmp->rb_right = node;
                parent = tmp;
            }

            parent->rb_parent_color |= 1;
            gparent->rb_parent_color &= ~1;
        }
    }

    root->rb_node->rb_parent_color |= 1;
}

/* Message Queue Functions */

static struct msg_msg *alloc_msg(size_t len) {
    struct msg_msg *msg;
    
    if (len > MAX_MSG_SIZE)
        return NULL;
        
    msg = malloc(sizeof(*msg));
    if (!msg)
        return NULL;
        
    msg->msg_data = malloc(len);
    if (!msg->msg_data) {
        free(msg);
        return NULL;
    }
    
    msg->msg_len = len;
    list_init(&msg->list);
    return msg;
}

static void free_msg(struct msg_msg *msg) {
    if (!msg)
        return;
    if (msg->msg_data)
        free(msg->msg_data);
    free(msg);
}

/* Insert a message into the priority queue */
static int msg_insert(struct msg_msg *msg,
                     struct mqueue_info *info) {
    struct rb_node **p, *parent = NULL;
    struct msg_tree_node *leaf;
    bool rightmost = true;

    p = &info->msg_tree.rb_node;
    while (*p) {
        parent = *p;
        leaf = (struct msg_tree_node *)parent;

        if (msg->msg_prio < leaf->priority) {
            p = &(*p)->rb_left;
            rightmost = false;
        } else if (msg->msg_prio > leaf->priority) {
            p = &(*p)->rb_right;
        } else {
            list_add_tail(&msg->list, &leaf->msg_list);
            return 0;
        }
    }

    leaf = malloc(sizeof(*leaf));
    if (!leaf)
        return -ENOMEM;

    leaf->priority = msg->msg_prio;
    list_init(&leaf->msg_list);
    list_add_tail(&msg->list, &leaf->msg_list);

    rb_link_node(&leaf->rb_node, parent, p);
    rb_insert_color(&leaf->rb_node, &info->msg_tree);

    if (rightmost)
        info->msg_tree_rightmost = &leaf->rb_node;

    return 0;
}

/* Get the highest priority message */
static struct msg_msg *msg_get(struct mqueue_info *info) {
    struct msg_tree_node *leaf;
    struct msg_msg *msg = NULL;
    struct rb_node *rbn;

    rbn = info->msg_tree_rightmost;
    if (!rbn)
        return NULL;

    leaf = (struct msg_tree_node *)rbn;
    if (!list_empty(&leaf->msg_list)) {
        msg = list_entry(leaf->msg_list.next, struct msg_msg, list);
        list_del(&msg->list);
        
        if (list_empty(&leaf->msg_list)) {
            rb_erase(&leaf->rb_node, &info->msg_tree);
            if (info->msg_tree.rb_node)
                info->msg_tree_rightmost = rb_last(&info->msg_tree);
            else
                info->msg_tree_rightmost = NULL;
            free(leaf);
        }
    }

    return msg;
}

/* Create a new message queue */
static struct mqueue_info *mqueue_create(const char *name,
                                       struct mq_attr *attr) {
    struct mqueue_info *info;
    
    info = calloc(1, sizeof(*info));
    if (!info)
        return NULL;
        
    info->name = strdup(name);
    if (!info->name) {
        free(info);
        return NULL;
    }
    
    pthread_mutex_init(&info->lock, NULL);
    info->msg_tree.rb_node = NULL;
    info->msg_tree_rightmost = NULL;
    info->node_cache = NULL;
    
    list_init(&info->senders_list);
    list_init(&info->receivers_list);
    
    if (attr) {
        info->attr = *attr;
    } else {
        info->attr.mq_maxmsg = MAX_MSG_DEFAULT;
        info->attr.mq_msgsize = MAX_MSGSIZE_DEFAULT;
    }
    
    info->valid = true;
    info->creation_time = time(NULL);
    atomic_init(&info->sent_messages, 0);
    atomic_init(&info->received_messages, 0);
    
    return info;
}

/* Destroy a message queue */
static void mqueue_destroy(struct mqueue_info *info) {
    struct msg_msg *msg;
    
    if (!info)
        return;
        
    pthread_mutex_lock(&info->lock);
    
    while ((msg = msg_get(info)) != NULL)
        free_msg(msg);
        
    pthread_mutex_unlock(&info->lock);
    pthread_mutex_destroy(&info->lock);
    
    if (info->name)
        free(info->name);
    free(info);
}

/* Send a message to the queue */
static int mqueue_send(struct mqueue_info *info,
                      const char *msg_ptr,
                      size_t msg_len,
                      unsigned int msg_prio) {
    struct msg_msg *msg;
    int ret;
    
    if (!info || !info->valid)
        return -EINVAL;
        
    if (msg_prio >= MQ_PRIO_MAX)
        return -EINVAL;
        
    if (msg_len > info->attr.mq_msgsize)
        return -EMSGSIZE;
        
    msg = alloc_msg(msg_len);
    if (!msg)
        return -ENOMEM;
        
    memcpy(msg->msg_data, msg_ptr, msg_len);
    msg->msg_prio = msg_prio;
    
    pthread_mutex_lock(&info->lock);
    
    if (info->attr.mq_curmsgs >= info->attr.mq_maxmsg) {
        pthread_mutex_unlock(&info->lock);
        free_msg(msg);
        return -EAGAIN;
    }
    
    ret = msg_insert(msg, info);
    if (ret) {
        pthread_mutex_unlock(&info->lock);
        free_msg(msg);
        return ret;
    }
    
    info->qsize += msg_len;
    info->attr.mq_curmsgs++;
    atomic_fetch_add(&info->sent_messages, 1);
    
    /* Wake up first waiting receiver if any */
    if (!list_empty(&info->receivers_list)) {
        struct wait_entry *waiter;
        waiter = list_entry(info->receivers_list.next,
                           struct wait_entry, list);
        list_del(&waiter->list);
        waiter->msg = msg;
        waiter->completed = true;
    }
    
    pthread_mutex_unlock(&info->lock);
    return 0;
}

/* Receive a message from the queue */
static ssize_t mqueue_receive(struct mqueue_info *info,
                            char *msg_ptr,
                            size_t msg_len,
                            unsigned int *msg_prio) {
    struct msg_msg *msg;
    ssize_t ret;
    
    if (!info || !info->valid)
        return -EINVAL;
        
    if (msg_len < info->attr.mq_msgsize)
        return -EMSGSIZE;
        
    pthread_mutex_lock(&info->lock);
    
    msg = msg_get(info);
    if (!msg) {
        pthread_mutex_unlock(&info->lock);
        return -EAGAIN;
    }
    
    memcpy(msg_ptr, msg->msg_data, msg->msg_len);
    ret = msg->msg_len;
    if (msg_prio)
        *msg_prio = msg->msg_prio;
        
    info->qsize -= msg->msg_len;
    info->attr.mq_curmsgs--;
    atomic_fetch_add(&info->received_messages, 1);
    
    /* Wake up first waiting sender if any */
    if (!list_empty(&info->senders_list)) {
        struct wait_entry *waiter;
        waiter = list_entry(info->senders_list.next,
                           struct wait_entry, list);
        list_del(&waiter->list);
        waiter->completed = true;
    }
    
    free_msg(msg);
    pthread_mutex_unlock(&info->lock);
    return ret;
}

/* Get queue attributes */
static int mqueue_getattr(struct mqueue_info *info,
                         struct mq_attr *attr) {
    if (!info || !info->valid || !attr)
        return -EINVAL;
        
    pthread_mutex_lock(&info->lock);
    *attr = info->attr;
    pthread_mutex_unlock(&info->lock);
    
    return 0;
}

/* Set queue attributes */
static int mqueue_setattr(struct mqueue_info *info,
                         struct mq_attr *new_attr,
                         struct mq_attr *old_attr) {
    if (!info || !info->valid || !new_attr)
        return -EINVAL;
        
    pthread_mutex_lock(&info->lock);
    
    if (old_attr)
        *old_attr = info->attr;
        
    info->attr.mq_flags = new_attr->mq_flags;
    
    pthread_mutex_unlock(&info->lock);
    return 0;
}

/* Demo Functions */

static void print_queue_stats(struct mqueue_info *info) {
    printf("\nQueue Statistics:\n");
    printf("Name: %s\n", info->name);
    printf("Current messages: %ld\n", info->attr.mq_curmsgs);
    printf("Maximum messages: %ld\n", info->attr.mq_maxmsg);
    printf("Message size: %ld\n", info->attr.mq_msgsize);
    printf("Total sent messages: %ld\n",
           atomic_load(&info->sent_messages));
    printf("Total received messages: %ld\n",
           atomic_load(&info->received_messages));
    printf("Queue size: %lu bytes\n", info->qsize);
    printf("Creation time: %s", ctime(&info->creation_time));
}

/* Main Function */

int main(void) {
    struct mqueue_info *queue;
    struct mq_attr attr;
    char msg_buffer[MAX_MSG_SIZE];
    unsigned int prio;
    int ret;
    
    printf("POSIX Message Queue Simulation\n");
    printf("=============================\n\n");
    
    /* Create a message queue */
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = 256;
    queue = mqueue_create("/test_queue", &attr);
    if (!queue) {
        fprintf(stderr, "Failed to create message queue\n");
        return 1;
    }
    
    printf("Message queue created successfully\n");
    print_queue_stats(queue);
    
    /* Send some messages */
    printf("\nSending messages...\n");
    const char *messages[] = {
        "High priority message",
        "Medium priority message",
        "Low priority message"
    };
    int priorities[] = {10, 5, 1};
    
    for (int i = 0; i < 3; i++) {
        ret = mqueue_send(queue, messages[i], strlen(messages[i]) + 1,
                         priorities[i]);
        if (ret == 0)
            printf("Sent message with priority %d\n", priorities[i]);
        else
            printf("Failed to send message: %d\n", ret);
    }
    
    print_queue_stats(queue);
    
    /* Receive messages */
    printf("\nReceiving messages...\n");
    for (int i = 0; i < 3; i++) {
        ret = mqueue_receive(queue, msg_buffer, sizeof(msg_buffer), &prio);
        if (ret > 0)
            printf("Received message (prio %u): %s\n", prio, msg_buffer);
        else
            printf("Failed to receive message: %d\n", ret);
    }
    
    print_queue_stats(queue);
    
    /* Cleanup */
    mqueue_destroy(queue);
    printf("\nMessage queue destroyed\n");
    
    return 0;
}
