/*
 * System V Message Queue Simulation
 * 
 * This program simulates the System V message queue IPC mechanism based on
 * the Linux kernel's implementation. It provides a user-space simulation
 * of message queues with similar functionality to the kernel.
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
#define MAX_QUEUES      128
#define MAX_MSG_SIZE    8192
#define MAX_MSGS        500
#define MSG_NOERROR     010000  /* Truncate message if too long */
#define MSG_EXCEPT      020000  /* Receive any msg except msgtyp */
#define MSG_COPY        040000  /* Copy (not remove) message */

/* Message Queue Permissions */
#define MSG_R           0400    /* Read permission */
#define MSG_W           0200    /* Write permission */

/* Error Codes */
#define MSGMNI          16      /* Maximum number of message queue identifiers */
#define MSGMAX          8192    /* Maximum size of message */
#define MSGMNB          16384   /* Default max size of a message queue */

/* Message Queue Control Commands */
#define IPC_RMID        0       /* Remove identifier */
#define IPC_SET         1       /* Set options */
#define IPC_STAT        2       /* Get options */
#define IPC_INFO        3       /* See ipcs */

/* Message Types */
#define SEARCH_ANY      1
#define SEARCH_EQUAL    2
#define SEARCH_NOTEQUAL 3
#define SEARCH_LESSEQUAL 4

/* Data Structures */

struct msg_msg {
    struct list_head list;
    long msg_type;
    size_t msg_ts;          /* Message text size */
    void *msg_data;
};

struct msg_queue {
    int id;                 /* Queue identifier */
    pthread_mutex_t lock;   /* Queue lock */
    unsigned int mode;      /* Access permissions */
    time_t q_stime;        /* Last msgsnd time */
    time_t q_rtime;        /* Last msgrcv time */
    time_t q_ctime;        /* Last change time */
    unsigned long q_cbytes; /* Current number of bytes in queue */
    unsigned long q_qnum;   /* Number of messages in queue */
    unsigned long q_qbytes; /* Maximum bytes in queue */
    pid_t q_lspid;         /* PID of last msgsnd */
    pid_t q_lrpid;         /* PID of last msgrcv */
    
    struct list_head q_messages;
    struct list_head q_receivers;
    struct list_head q_senders;
    bool valid;
};

struct msg_receiver {
    struct list_head r_list;
    pthread_t thread;
    int mode;
    long msgtype;
    size_t maxsize;
    struct msg_msg *msg;
    bool completed;
};

struct msg_sender {
    struct list_head list;
    pthread_t thread;
    size_t msgsz;
    bool completed;
};

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/* Global Variables */
static struct msg_queue *queues[MAX_QUEUES];
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
    
    msg->msg_ts = len;
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

/* Create a new message queue */
static struct msg_queue *msgget_new(int key, int msgflg) {
    struct msg_queue *msq;
    int id;
    
    pthread_mutex_lock(&queues_lock);
    
    /* Find first available ID */
    for (id = 0; id < MAX_QUEUES; id++) {
        if (!queues[id])
            break;
    }
    
    if (id >= MAX_QUEUES) {
        pthread_mutex_unlock(&queues_lock);
        return NULL;
    }
    
    msq = calloc(1, sizeof(*msq));
    if (!msq) {
        pthread_mutex_unlock(&queues_lock);
        return NULL;
    }
    
    msq->id = id;
    msq->mode = msgflg & 0777;
    pthread_mutex_init(&msq->lock, NULL);
    msq->q_ctime = time(NULL);
    msq->q_qbytes = MSGMNB;
    list_init(&msq->q_messages);
    list_init(&msq->q_receivers);
    list_init(&msq->q_senders);
    msq->valid = true;
    
    queues[id] = msq;
    atomic_fetch_add(&queue_count, 1);
    
    pthread_mutex_unlock(&queues_lock);
    return msq;
}

/* Find existing queue by key */
static struct msg_queue *msgget_existing(int key, int msgflg) {
    struct msg_queue *msq;
    int i;
    
    pthread_mutex_lock(&queues_lock);
    
    for (i = 0; i < MAX_QUEUES; i++) {
        msq = queues[i];
        if (msq && msq->valid) {
            pthread_mutex_unlock(&queues_lock);
            return msq;
        }
    }
    
    pthread_mutex_unlock(&queues_lock);
    return NULL;
}

/* Message Queue System Call Implementations */

int msgget(int key, int msgflg) {
    struct msg_queue *msq;
    
    if (key == IPC_PRIVATE)
        msq = msgget_new(key, msgflg);
    else {
        msq = msgget_existing(key, msgflg);
        if (!msq && (msgflg & IPC_CREAT))
            msq = msgget_new(key, msgflg);
    }
    
    if (!msq)
        return -1;
        
    return msq->id;
}

static int testmsg(struct msg_msg *msg, long type, int mode) {
    switch (mode) {
        case SEARCH_ANY:
            return 1;
        case SEARCH_EQUAL:
            return msg->msg_type == type;
        case SEARCH_NOTEQUAL:
            return msg->msg_type != type;
        case SEARCH_LESSEQUAL:
            return msg->msg_type <= type;
    }
    return 0;
}

static struct msg_msg *find_msg(struct msg_queue *msq,
                              long *msgtyp,
                              int mode) {
    struct msg_msg *msg, *found = NULL;
    struct list_head *tmp;
    
    list_for_each(tmp, &msq->q_messages) {
        msg = list_entry(tmp, struct msg_msg, list);
        if (testmsg(msg, *msgtyp, mode)) {
            found = msg;
            break;
        }
    }
    
    if (found)
        list_del(&found->list);
        
    return found;
}

int msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg) {
    struct msg_queue *msq;
    struct msg_msg *msg;
    const struct msgbuf *mbuf = msgp;
    int err = 0;
    
    if (msqid < 0 || msqid >= MAX_QUEUES)
        return -1;
        
    msq = queues[msqid];
    if (!msq || !msq->valid)
        return -1;
        
    if (msgsz > msq->q_qbytes)
        return -1;
        
    msg = alloc_msg(msgsz);
    if (!msg)
        return -1;
        
    msg->msg_type = mbuf->mtype;
    memcpy(msg->msg_data, mbuf->mtext, msgsz);
    
    pthread_mutex_lock(&msq->lock);
    
    if (msq->q_qnum >= MAX_MSGS) {
        if (msgflg & IPC_NOWAIT) {
            err = -1;
            goto out_unlock;
        }
        
        /* Wait for space */
        struct msg_sender s;
        list_init(&s.list);
        s.thread = pthread_self();
        s.msgsz = msgsz;
        s.completed = false;
        
        list_add_tail(&s.list, &msq->q_senders);
        pthread_mutex_unlock(&msq->lock);
        
        while (!s.completed)
            usleep(1000);
            
        pthread_mutex_lock(&msq->lock);
        list_del(&s.list);
    }
    
    /* Add message to queue */
    list_add_tail(&msg->list, &msq->q_messages);
    msq->q_qnum++;
    msq->q_cbytes += msgsz;
    msq->q_lspid = getpid();
    msq->q_stime = time(NULL);
    
    /* Wake up first waiting receiver if any */
    if (!list_empty(&msq->q_receivers)) {
        struct msg_receiver *r;
        r = list_entry(msq->q_receivers.next,
                      struct msg_receiver,
                      r_list);
        list_del(&r->r_list);
        r->msg = msg;
        r->completed = true;
    }
    
out_unlock:
    pthread_mutex_unlock(&msq->lock);
    if (err)
        free_msg(msg);
    return err;
}

ssize_t msgrcv(int msqid, void *msgp, size_t msgsz,
               long msgtyp, int msgflg) {
    struct msg_queue *msq;
    struct msg_msg *msg;
    struct msgbuf *mbuf = msgp;
    int mode;
    ssize_t ret = -1;
    
    if (msqid < 0 || msqid >= MAX_QUEUES)
        return -1;
        
    msq = queues[msqid];
    if (!msq || !msq->valid)
        return -1;
        
    /* Convert msgtyp to search mode */
    if (msgtyp == 0)
        mode = SEARCH_ANY;
    else if (msgtyp < 0)
        mode = SEARCH_LESSEQUAL;
    else if (msgflg & MSG_EXCEPT)
        mode = SEARCH_NOTEQUAL;
    else
        mode = SEARCH_EQUAL;
        
    pthread_mutex_lock(&msq->lock);
    
    msg = find_msg(msq, &msgtyp, mode);
    if (!msg) {
        if (msgflg & IPC_NOWAIT) {
            ret = -1;
            goto out_unlock;
        }
        
        /* Wait for message */
        struct msg_receiver r;
        list_init(&r.r_list);
        r.thread = pthread_self();
        r.mode = mode;
        r.msgtype = msgtyp;
        r.maxsize = msgsz;
        r.msg = NULL;
        r.completed = false;
        
        list_add_tail(&r.r_list, &msq->q_receivers);
        pthread_mutex_unlock(&msq->lock);
        
        while (!r.completed)
            usleep(1000);
            
        pthread_mutex_lock(&msq->lock);
        msg = r.msg;
        list_del(&r.r_list);
    }
    
    if (!msg)
        goto out_unlock;
        
    /* Copy message to user buffer */
    if (msg->msg_ts > msgsz) {
        if (!(msgflg & MSG_NOERROR)) {
            ret = -1;
            goto out_unlock;
        }
        msg->msg_ts = msgsz;
    }
    
    mbuf->mtype = msg->msg_type;
    memcpy(mbuf->mtext, msg->msg_data, msg->msg_ts);
    ret = msg->msg_ts;
    
    msq->q_qnum--;
    msq->q_cbytes -= msg->msg_ts;
    msq->q_lrpid = getpid();
    msq->q_rtime = time(NULL);
    
    /* Wake up first waiting sender if any */
    if (!list_empty(&msq->q_senders)) {
        struct msg_sender *s;
        s = list_entry(msq->q_senders.next,
                      struct msg_sender,
                      list);
        list_del(&s->list);
        s->completed = true;
    }
    
out_unlock:
    pthread_mutex_unlock(&msq->lock);
    if (msg)
        free_msg(msg);
    return ret;
}

int msgctl(int msqid, int cmd, struct msqid_ds *buf) {
    struct msg_queue *msq;
    int err = 0;
    
    if (msqid < 0 || msqid >= MAX_QUEUES)
        return -1;
        
    msq = queues[msqid];
    if (!msq || !msq->valid)
        return -1;
        
    switch (cmd) {
        case IPC_STAT:
            pthread_mutex_lock(&msq->lock);
            buf->msg_perm.mode = msq->mode;
            buf->msg_qnum = msq->q_qnum;
            buf->msg_qbytes = msq->q_qbytes;
            buf->msg_lspid = msq->q_lspid;
            buf->msg_lrpid = msq->q_lrpid;
            buf->msg_stime = msq->q_stime;
            buf->msg_rtime = msq->q_rtime;
            buf->msg_ctime = msq->q_ctime;
            pthread_mutex_unlock(&msq->lock);
            break;
            
        case IPC_SET:
            pthread_mutex_lock(&msq->lock);
            msq->mode = buf->msg_perm.mode & 0777;
            msq->q_qbytes = buf->msg_qbytes;
            msq->q_ctime = time(NULL);
            pthread_mutex_unlock(&msq->lock);
            break;
            
        case IPC_RMID:
            pthread_mutex_lock(&queues_lock);
            pthread_mutex_lock(&msq->lock);
            msq->valid = false;
            queues[msqid] = NULL;
            atomic_fetch_sub(&queue_count, 1);
            
            /* Wake up all waiters */
            struct list_head *tmp, *next;
            list_for_each_safe(tmp, next, &msq->q_receivers) {
                struct msg_receiver *r;
                r = list_entry(tmp, struct msg_receiver, r_list);
                list_del(&r->r_list);
                r->completed = true;
            }
            list_for_each_safe(tmp, next, &msq->q_senders) {
                struct msg_sender *s;
                s = list_entry(tmp, struct msg_sender, list);
                list_del(&s->list);
                s->completed = true;
            }
            
            /* Free all messages */
            list_for_each_safe(tmp, next, &msq->q_messages) {
                struct msg_msg *msg;
                msg = list_entry(tmp, struct msg_msg, list);
                list_del(&msg->list);
                free_msg(msg);
            }
            
            pthread_mutex_unlock(&msq->lock);
            pthread_mutex_destroy(&msq->lock);
            free(msq);
            pthread_mutex_unlock(&queues_lock);
            break;
            
        default:
            err = -1;
    }
    
    return err;
}

/* Demo Functions */

static void print_queue_stats(struct msg_queue *msq) {
    printf("\nQueue Statistics (ID: %d):\n", msq->id);
    printf("Messages in queue: %lu\n", msq->q_qnum);
    printf("Bytes in queue: %lu\n", msq->q_cbytes);
    printf("Maximum bytes: %lu\n", msq->q_qbytes);
    printf("Last send time: %s", ctime(&msq->q_stime));
    printf("Last receive time: %s", ctime(&msq->q_rtime));
    printf("Last change time: %s", ctime(&msq->q_ctime));
    printf("Last send PID: %d\n", msq->q_lspid);
    printf("Last receive PID: %d\n", msq->q_lrpid);
}

/* Main Function */

int main(void) {
    struct msqid_ds buf;
    int msqid;
    struct {
        long mtype;
        char mtext[100];
    } msg;
    
    printf("System V Message Queue Simulation\n");
    printf("================================\n\n");
    
    /* Create a message queue */
    msqid = msgget(IPC_PRIVATE, MSG_R | MSG_W);
    if (msqid < 0) {
        perror("msgget");
        return 1;
    }
    
    printf("Created message queue with ID: %d\n", msqid);
    
    /* Send some messages */
    printf("\nSending messages...\n");
    const char *messages[] = {
        "High priority message",
        "Medium priority message",
        "Low priority message"
    };
    int priorities[] = {3, 2, 1};
    
    for (int i = 0; i < 3; i++) {
        msg.mtype = priorities[i];
        strcpy(msg.mtext, messages[i]);
        if (msgsnd(msqid, &msg, strlen(msg.mtext) + 1, 0) < 0) {
            perror("msgsnd");
            return 1;
        }
        printf("Sent message with priority %d\n", priorities[i]);
    }
    
    /* Get queue statistics */
    if (msgctl(msqid, IPC_STAT, &buf) < 0) {
        perror("msgctl");
        return 1;
    }
    print_queue_stats(queues[msqid]);
    
    /* Receive messages */
    printf("\nReceiving messages...\n");
    for (int i = 0; i < 3; i++) {
        if (msgrcv(msqid, &msg, sizeof(msg.mtext), 0, 0) < 0) {
            perror("msgrcv");
            return 1;
        }
        printf("Received message (type %ld): %s\n",
               msg.mtype, msg.mtext);
    }
    
    /* Get updated statistics */
    if (msgctl(msqid, IPC_STAT, &buf) < 0) {
        perror("msgctl");
        return 1;
    }
    print_queue_stats(queues[msqid]);
    
    /* Remove the queue */
    if (msgctl(msqid, IPC_RMID, NULL) < 0) {
        perror("msgctl");
        return 1;
    }
    printf("\nMessage queue removed\n");
    
    return 0;
}
