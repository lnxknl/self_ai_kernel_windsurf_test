/*
 * IO_URING Poll Simulation
 * 
 * This program simulates the polling mechanism used by io_uring for
 * asynchronous I/O operations. It provides a user-space implementation
 * of event polling and notification similar to the kernel's implementation.
 *
 * Author: Cascade AI
 * Date: 2024-12-29
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <fcntl.h>
#include <assert.h>

/* Configuration Constants */
#define MAX_POLL_ENTRIES    1024
#define POLL_HASH_BITS      6
#define POLL_HASH_SIZE      (1 << POLL_HASH_BITS)
#define POLL_BATCH_SIZE     32
#define MAX_RETRY_COUNT     5

/* Poll Flags */
enum {
    POLL_F_DONE       = 1 << 0,  /* Poll operation completed */
    POLL_F_CANCELLED  = 1 << 1,  /* Poll operation cancelled */
    POLL_F_MULTISHOT  = 1 << 2,  /* Multi-shot poll */
    POLL_F_DOUBLE     = 1 << 3,  /* Double poll entry */
};

/* Poll Status */
enum {
    POLL_DONE           = 0,    /* Poll completed */
    POLL_NO_ACTION     = 1,    /* No action needed */
    POLL_REMOVE        = 2,    /* Remove poll entry */
    POLL_RETRY         = 3,    /* Retry poll operation */
};

/* Forward Declarations */
struct poll_table;
struct poll_entry;
struct poll_context;
struct poll_group;

/* Basic Poll Entry Structure */
struct poll_entry {
    struct poll_entry *next;     /* Next in hash chain */
    struct poll_entry *prev;     /* Previous in hash chain */
    
    int fd;                      /* File descriptor */
    uint32_t events;            /* Events to monitor */
    uint32_t mask;              /* Current event mask */
    uint64_t user_data;         /* User data */
    
    unsigned int flags;          /* Entry flags */
    atomic_int refs;            /* Reference count */
    
    /* Callback data */
    void (*callback)(struct poll_entry *, uint32_t);
    void *data;
    
    /* Statistics */
    unsigned long poll_count;    /* Number of times polled */
    time_t last_poll;           /* Last poll time */
    
    /* Synchronization */
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

/* Poll Hash Table Bucket */
struct poll_bucket {
    struct poll_entry *head;
    pthread_mutex_t lock;
    unsigned int count;
};

/* Poll Group Structure */
struct poll_group {
    int epfd;                   /* Epoll file descriptor */
    struct poll_entry **entries;/* Active poll entries */
    unsigned int nr_entries;    /* Number of entries */
    pthread_mutex_t lock;       /* Group lock */
    bool active;                /* Group is active */
};

/* Poll Context Structure */
struct poll_context {
    struct poll_bucket buckets[POLL_HASH_SIZE];
    struct poll_group *group;
    
    /* Thread management */
    pthread_t poll_thread;
    bool thread_running;
    
    /* Statistics */
    atomic_long total_polls;
    atomic_long active_polls;
    atomic_long completed_polls;
    
    /* Synchronization */
    pthread_mutex_t lock;
    int eventfd;               /* For waking poll thread */
};

/* Helper Functions */

static unsigned int hash_fd(int fd) {
    return fd & (POLL_HASH_SIZE - 1);
}

static void poll_entry_ref_get(struct poll_entry *entry) {
    atomic_fetch_add(&entry->refs, 1);
}

static void poll_entry_ref_put(struct poll_entry *entry) {
    if (atomic_fetch_sub(&entry->refs, 1) == 1) {
        pthread_mutex_destroy(&entry->lock);
        pthread_cond_destroy(&entry->cond);
        free(entry);
    }
}

/* Poll Entry Management */

static struct poll_entry *poll_entry_alloc(void) {
    struct poll_entry *entry;
    
    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return NULL;
    
    atomic_init(&entry->refs, 1);
    pthread_mutex_init(&entry->lock, NULL);
    pthread_cond_init(&entry->cond, NULL);
    
    return entry;
}

static void poll_entry_add(struct poll_context *ctx, struct poll_entry *entry) {
    unsigned int bucket = hash_fd(entry->fd);
    struct poll_bucket *pb = &ctx->buckets[bucket];
    
    pthread_mutex_lock(&pb->lock);
    
    entry->next = pb->head;
    if (pb->head)
        pb->head->prev = entry;
    pb->head = entry;
    pb->count++;
    
    pthread_mutex_unlock(&pb->lock);
}

static void poll_entry_remove(struct poll_context *ctx, struct poll_entry *entry) {
    unsigned int bucket = hash_fd(entry->fd);
    struct poll_bucket *pb = &ctx->buckets[bucket];
    
    pthread_mutex_lock(&pb->lock);
    
    if (entry->prev)
        entry->prev->next = entry->next;
    else
        pb->head = entry->next;
    
    if (entry->next)
        entry->next->prev = entry->prev;
    
    pb->count--;
    
    pthread_mutex_unlock(&pb->lock);
}

/* Poll Group Management */

static struct poll_group *poll_group_create(void) {
    struct poll_group *group;
    
    group = calloc(1, sizeof(*group));
    if (!group)
        return NULL;
    
    group->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (group->epfd < 0) {
        free(group);
        return NULL;
    }
    
    group->entries = calloc(MAX_POLL_ENTRIES, sizeof(struct poll_entry *));
    if (!group->entries) {
        close(group->epfd);
        free(group);
        return NULL;
    }
    
    pthread_mutex_init(&group->lock, NULL);
    group->active = true;
    
    return group;
}

static void poll_group_destroy(struct poll_group *group) {
    if (!group)
        return;
    
    group->active = false;
    
    if (group->epfd >= 0)
        close(group->epfd);
    
    free(group->entries);
    pthread_mutex_destroy(&group->lock);
    free(group);
}

/* Poll Thread Functions */

static void poll_process_events(struct poll_context *ctx, 
                              struct epoll_event *events,
                              int nr_events) {
    int i;
    struct poll_entry *entry;
    
    for (i = 0; i < nr_events; i++) {
        entry = events[i].data.ptr;
        
        pthread_mutex_lock(&entry->lock);
        
        if (!(entry->flags & POLL_F_CANCELLED)) {
            entry->mask = events[i].events;
            entry->poll_count++;
            entry->last_poll = time(NULL);
            
            if (entry->callback)
                entry->callback(entry, events[i].events);
            
            if (!(entry->flags & POLL_F_MULTISHOT)) {
                entry->flags |= POLL_F_DONE;
                atomic_fetch_add(&ctx->completed_polls, 1);
            }
        }
        
        pthread_mutex_unlock(&entry->lock);
        
        if (entry->flags & POLL_F_DONE)
            poll_entry_ref_put(entry);
    }
}

static void *poll_thread(void *data) {
    struct poll_context *ctx = data;
    struct epoll_event events[POLL_BATCH_SIZE];
    int nr_events;
    
    while (ctx->thread_running) {
        nr_events = epoll_wait(ctx->group->epfd, events, POLL_BATCH_SIZE, -1);
        if (nr_events < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        
        if (nr_events > 0)
            poll_process_events(ctx, events, nr_events);
    }
    
    return NULL;
}

/* Context Management */

struct poll_context *poll_context_create(void) {
    struct poll_context *ctx;
    int i;
    
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    
    /* Initialize hash buckets */
    for (i = 0; i < POLL_HASH_SIZE; i++) {
        pthread_mutex_init(&ctx->buckets[i].lock, NULL);
    }
    
    /* Create poll group */
    ctx->group = poll_group_create();
    if (!ctx->group)
        goto err_group;
    
    /* Create eventfd for waking poll thread */
    ctx->eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (ctx->eventfd < 0)
        goto err_eventfd;
    
    pthread_mutex_init(&ctx->lock, NULL);
    
    /* Start poll thread */
    ctx->thread_running = true;
    if (pthread_create(&ctx->poll_thread, NULL, poll_thread, ctx))
        goto err_thread;
    
    return ctx;

err_thread:
    close(ctx->eventfd);
err_eventfd:
    poll_group_destroy(ctx->group);
err_group:
    for (i = 0; i < POLL_HASH_SIZE; i++)
        pthread_mutex_destroy(&ctx->buckets[i].lock);
    free(ctx);
    return NULL;
}

void poll_context_destroy(struct poll_context *ctx) {
    int i;
    struct poll_bucket *pb;
    struct poll_entry *entry, *next;
    
    if (!ctx)
        return;
    
    /* Stop poll thread */
    ctx->thread_running = false;
    eventfd_write(ctx->eventfd, 1);
    pthread_join(ctx->poll_thread, NULL);
    
    /* Cleanup entries */
    for (i = 0; i < POLL_HASH_SIZE; i++) {
        pb = &ctx->buckets[i];
        pthread_mutex_lock(&pb->lock);
        entry = pb->head;
        while (entry) {
            next = entry->next;
            poll_entry_ref_put(entry);
            entry = next;
        }
        pthread_mutex_unlock(&pb->lock);
        pthread_mutex_destroy(&pb->lock);
    }
    
    close(ctx->eventfd);
    poll_group_destroy(ctx->group);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

/* Public API Functions */

int poll_add(struct poll_context *ctx, int fd, uint32_t events,
            void (*callback)(struct poll_entry *, uint32_t),
            void *data, uint64_t user_data) {
    struct poll_entry *entry;
    struct epoll_event ev;
    int ret;
    
    if (!ctx || fd < 0)
        return -EINVAL;
    
    entry = poll_entry_alloc();
    if (!entry)
        return -ENOMEM;
    
    entry->fd = fd;
    entry->events = events;
    entry->callback = callback;
    entry->data = data;
    entry->user_data = user_data;
    
    /* Add to epoll */
    ev.events = events;
    ev.data.ptr = entry;
    ret = epoll_ctl(ctx->group->epfd, EPOLL_CTL_ADD, fd, &ev);
    if (ret < 0) {
        poll_entry_ref_put(entry);
        return ret;
    }
    
    /* Add to hash table */
    poll_entry_add(ctx, entry);
    atomic_fetch_add(&ctx->active_polls, 1);
    
    return 0;
}

int poll_remove(struct poll_context *ctx, int fd) {
    unsigned int bucket = hash_fd(fd);
    struct poll_bucket *pb = &ctx->buckets[bucket];
    struct poll_entry *entry;
    int ret = -ENOENT;
    
    pthread_mutex_lock(&pb->lock);
    
    entry = pb->head;
    while (entry) {
        if (entry->fd == fd) {
            entry->flags |= POLL_F_CANCELLED;
            epoll_ctl(ctx->group->epfd, EPOLL_CTL_DEL, fd, NULL);
            poll_entry_remove(ctx, entry);
            atomic_fetch_sub(&ctx->active_polls, 1);
            poll_entry_ref_put(entry);
            ret = 0;
            break;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&pb->lock);
    return ret;
}

/* Example Usage */

static void example_callback(struct poll_entry *entry, uint32_t events) {
    printf("Poll callback: fd=%d events=0x%x user_data=%lu\n",
           entry->fd, events, entry->user_data);
}

void example_poll_usage(void) {
    struct poll_context *ctx;
    int timer_fd;
    struct itimerspec its = {
        .it_interval = { .tv_sec = 1, .tv_nsec = 0 },
        .it_value = { .tv_sec = 1, .tv_nsec = 0 }
    };
    
    printf("Poll Simulation Example\n");
    printf("======================\n\n");
    
    /* Create poll context */
    ctx = poll_context_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create poll context\n");
        return;
    }
    
    /* Create timer */
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        fprintf(stderr, "Failed to create timer\n");
        goto out_ctx;
    }
    
    /* Start timer */
    if (timerfd_settime(timer_fd, 0, &its, NULL) < 0) {
        fprintf(stderr, "Failed to start timer\n");
        goto out_timer;
    }
    
    /* Add timer to poll */
    if (poll_add(ctx, timer_fd, EPOLLIN, example_callback, NULL, 42) < 0) {
        fprintf(stderr, "Failed to add timer to poll\n");
        goto out_timer;
    }
    
    printf("Polling for events (will run for 5 seconds)...\n");
    sleep(5);
    
    printf("\nFinal Statistics:\n");
    printf("Total polls: %ld\n", atomic_load(&ctx->total_polls));
    printf("Active polls: %ld\n", atomic_load(&ctx->active_polls));
    printf("Completed polls: %ld\n", atomic_load(&ctx->completed_polls));
    
out_timer:
    close(timer_fd);
out_ctx:
    poll_context_destroy(ctx);
}

int main(void) {
    example_poll_usage();
    return 0;
}
